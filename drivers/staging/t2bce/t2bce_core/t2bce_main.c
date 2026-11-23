#include "t2bce.h"
#include <linux/module.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/version.h>

/*
 * Apple T2 Macs contain a coprocessor serving as a board supervisor.
 * The T2 is basically a repurposed iPhone CPU running bridgeOS,
 * a stripped down version of WatchOS. Apple reuses the audio codec
 * and USB interfaces of this SOC and uses a "Buffer Copy Engine" (BCE)
 * for communication between the host OS, the T2 and the VHCI USB devices
 * behind the T2 USB hub like internal keyboard, trackpad, touchbar etc..
 *
 * This core driver owns the BCE PCI device, mailbox, DMA queue registration
 * and system PM ordering. Other modules bind as clients:
 * - t2bce_audio exposes the bridgeOS audio endpoints to ALSA.
 * - t2bce_vhci exposes the T2-internal virtual USB hub and devices to usbcore.
 * - t2bce_dma provides the queue engine used by both.
 *
 * ACPI describes the host-side PCI devices, but bridgeOS still controls the
 * remote endpoints and must be kept in sync during probe, shutdown and system
 * suspend/resume. The PM code below mirrors the observed firmware ordering:
 * It quiesces clients, drains the mailbox channel, asks bridgeOS to save or discard
 * BCE state, then lets clients rebuild or continue according to the selected
 * resume path.
 */

static dev_t bce_chrdev;
static struct class *bce_class;
static const bool bce_use_stateful_sleep = true;

struct t2bce_device *global_bce;

static int bce_create_command_queues(struct t2bce_device *bce);
static void bce_free_command_queues(struct t2bce_device *bce);
static irqreturn_t bce_handle_mb_irq(int irq, void *dev);
static irqreturn_t bce_handle_dma_irq(int irq, void *dev);
static void bce_process_dma_completions(struct t2bce_device *bce);
static int bce_fw_version_handshake(struct t2bce_device *bce);
static int bce_register_command_queue(struct t2bce_device *bce, struct bce_queue_memcfg *cfg, int is_sq);
static int t2bce_dma_register_queue(void *userdata, struct bce_queue_memcfg *cfg,
        const char *name, bool isdirout);
static int t2bce_dma_unregister_queue(void *userdata, u16 qid);
static int t2bce_dma_flush_queue(void *userdata, u16 qid);
static int bce_alloc_state_buffer(struct t2bce_device *bce, size_t size);
static void bce_free_state_buffer(struct t2bce_device *bce);
static int bce_pm_suspend_prepare(struct t2bce_device *bce);
static void bce_pm_suspend_abort(struct t2bce_device *bce);
static void bce_pm_resume_finish_locked(struct t2bce_device *bce);

static const struct t2bce_dma_engine_ops t2bce_dma_ops = {
    .register_queue = t2bce_dma_register_queue,
    .unregister_queue = t2bce_dma_unregister_queue,
    .flush_queue = t2bce_dma_flush_queue,
};

#define BCE_STATE_INITIAL_SIZE 0x1000
#define BCE_STATE_MAX_SIZE     0x100000
#define BCE_STATE_ADDR_MASK    0xffffffff000ULL

static int bce_alloc_state_buffer(struct t2bce_device *bce, size_t size)
{
    bce->saved_data_dma_ptr = kzalloc(size, GFP_KERNEL);
    if (!bce->saved_data_dma_ptr) {
        bce->saved_data_dma_size = 0;
        bce->saved_data_dma_addr = 0;
        return -ENOMEM;
    }

    bce->saved_data_dma_size = size;
    bce->saved_data_dma_addr = 0;

    return 0;
}

static void bce_free_state_buffer(struct t2bce_device *bce)
{
    if (!bce->saved_data_dma_ptr)
        return;

    kfree(bce->saved_data_dma_ptr);
    bce->saved_data_dma_ptr = NULL;
    bce->saved_data_dma_addr = 0;
    bce->saved_data_dma_size = 0;
}

static int t2bce_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct t2bce_device *bce = NULL;
    int status = 0;
    int nvec;

    pr_debug("t2bce_core: capturing our device\n");

    if (pci_enable_device(dev))
        return -ENODEV;
    if (pci_request_regions(dev, "t2bce_core")) {
        status = -ENODEV;
        goto fail;
    }
    pci_set_master(dev);
    nvec = pci_alloc_irq_vectors(dev, 1, 8, PCI_IRQ_MSI);
    if (nvec < 5) {
        status = -EINVAL;
        goto fail;
    }

    bce = kzalloc(sizeof(struct t2bce_device), GFP_KERNEL);
    if (!bce) {
        status = -ENOMEM;
        goto fail;
    }

    bce->pci = dev;
    pci_set_drvdata(dev, bce);

    bce->devt = bce_chrdev;
    bce->dev = device_create(bce_class, &dev->dev, bce->devt, NULL, "t2bce_core");
    if (IS_ERR_OR_NULL(bce->dev)) {
        status = PTR_ERR(bce->dev);
        goto fail;
    }

    bce->reg_mem_mb = pci_iomap(dev, 4, 0);
    bce->reg_mem_dma = pci_iomap(dev, 2, 0);

    if (IS_ERR_OR_NULL(bce->reg_mem_mb) || IS_ERR_OR_NULL(bce->reg_mem_dma)) {
        dev_warn(&dev->dev, "t2bce_core: Failed to pci_iomap required regions\n");
        goto fail;
    }

    bce_mailbox_init(&bce->mbox, bce->reg_mem_mb);
    bce_xhci_pm_init(&bce->xhci_pm, bce->reg_mem_mb);
    bce->dma.dma_dev = &dev->dev;
    bce->dma.ops = &t2bce_dma_ops;
    bce->dma.ops_userdata = bce;
    bce->dma.reg_mem_dma = bce->reg_mem_dma;

    spin_lock_init(&bce->dma.queues_lock);
    atomic_set(&bce->dma.available, 1);
    ida_init(&bce->dma.queue_ida);
    mutex_init(&bce->pm_lock);
    mutex_init(&bce->dma_completion_lock);
    mutex_init(&bce->clients_lock);
    INIT_LIST_HEAD(&bce->clients);
    status = init_srcu_struct(&bce->clients_srcu);
    if (status)
        goto fail;
    bce->clients_srcu_ready = true;
    bce->mailbox_channel_active = true;

    if ((status = pci_request_irq(dev, 0, bce_handle_mb_irq, NULL, dev, "bce_mbox")))
        goto fail;
    if ((status = pci_request_irq(dev, 4, NULL, bce_handle_dma_irq, dev, "bce_dma")))
        goto fail_interrupt_0;

    if ((status = dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(37)))) {
        dev_warn(&dev->dev, "dma: Setting mask failed\n");
        goto fail_interrupt;
    }

    if ((status = t2bce_dma_init_segment_list_pool(&bce->dma)))
        goto fail_interrupt;

    /*
     * DMA on the BCE function depends on function 0 also being bus master.
     * Keep a reference so probe and resume can restore that firmware-visible
     * host state.
     */
    bce->pci0 = pci_get_slot(dev->bus, PCI_DEVFN(PCI_SLOT(dev->devfn), 0));
#ifndef WITHOUT_NVME_PATCH
    if ((status = pci_enable_device_mem(bce->pci0))) {
        dev_warn(&dev->dev, "t2bce_core: failed to enable function 0\n");
        goto fail_dev0;
    }
#endif
    pci_set_master(bce->pci0);

    /*
     * Function 0 must stay bus master while bridgeOS saves state on
     * SAVE_STATE_AND_SLEEP and must be bus master again before
     * RESTORE_STATE_AND_WAKE. Its driver (nvme) disables the function on
     * suspend. Order it as our PM supplier so it suspends after and resumes
     * before this function, also with pm_async.
     */
    bce->pci0_link = device_link_add(&dev->dev, &bce->pci0->dev, DL_FLAG_STATELESS);
    if (!bce->pci0_link)
        dev_warn(&dev->dev, "t2bce_core: failed to link PM order to function 0\n");

    /* bridgeOS expects the XHCI PM doorbell before the controller becomes active. */
    bce_xhci_pm_start(&bce->xhci_pm, true);

    if ((status = bce_fw_version_handshake(bce)))
        goto fail_ts;
    pr_debug("t2bce_core: handshake done\n");

    if ((status = bce_create_command_queues(bce))) {
        pr_err("t2bce_core: Creating command queues failed\n");
        goto fail_ts;
    }

    global_bce = bce;
    pr_info("t2bce_core: device initialized\n");

    return 0;

fail_ts:
    bce_free_state_buffer(bce);
    bce_xhci_pm_stop(&bce->xhci_pm);
    if (bce->pci0_link)
        device_link_del(bce->pci0_link);
#ifndef WITHOUT_NVME_PATCH
    pci_disable_device(bce->pci0);
fail_dev0:
#endif
    pci_dev_put(bce->pci0);
    t2bce_dma_destroy_segment_list_pool(&bce->dma);
fail_interrupt:
    pci_free_irq(dev, 4, dev);
fail_interrupt_0:
    pci_free_irq(dev, 0, dev);
fail:
    if (bce && bce->dev) {
        device_destroy(bce_class, bce->devt);

        if (!IS_ERR_OR_NULL(bce->reg_mem_mb))
            pci_iounmap(dev, bce->reg_mem_mb);
        if (!IS_ERR_OR_NULL(bce->reg_mem_dma))
            pci_iounmap(dev, bce->reg_mem_dma);

        if (bce->clients_srcu_ready)
            cleanup_srcu_struct(&bce->clients_srcu);

        kfree(bce);
    }

    pci_free_irq_vectors(dev);
    pci_release_regions(dev);
    pci_disable_device(dev);

    if (!status)
        status = -EINVAL;
    return status;
}

static int bce_create_command_queues(struct t2bce_device *bce)
{
    int status;
    struct bce_queue_memcfg *cfg;
    struct t2bce_dma_engine *dma = &bce->dma;

    dma->cmd_cq = t2bce_dma_alloc_cq(dma, 0, 0x20);
    dma->cmd_cmdq = t2bce_dma_alloc_cmdq(dma, 1, 0x20);
    if (dma->cmd_cq == NULL || dma->cmd_cmdq == NULL) {
        status = -ENOMEM;
        goto err;
    }
    dma->queues[0] = (struct bce_queue *) dma->cmd_cq;
    dma->queues[1] = (struct bce_queue *) dma->cmd_cmdq->sq;

    cfg = kzalloc(sizeof(struct bce_queue_memcfg), GFP_KERNEL);
    if (!cfg) {
        status = -ENOMEM;
        goto err;
    }
    t2bce_dma_get_cq_memcfg(dma->cmd_cq, cfg);
    if ((status = bce_register_command_queue(bce, cfg, false)))
        goto err;
    t2bce_dma_get_sq_memcfg(dma->cmd_cmdq->sq, dma->cmd_cq, cfg);
    if ((status = bce_register_command_queue(bce, cfg, true)))
        goto err;
    kfree(cfg);

    return 0;

err:
    if (dma->cmd_cq)
        t2bce_dma_free_cq(dma, dma->cmd_cq);
    if (dma->cmd_cmdq)
        t2bce_dma_free_cmdq(dma, dma->cmd_cmdq);
    return status;
}

static void bce_free_command_queues(struct t2bce_device *bce)
{
    struct t2bce_dma_engine *dma = &bce->dma;

    t2bce_dma_free_cq(dma, dma->cmd_cq);
    t2bce_dma_free_cmdq(dma, dma->cmd_cmdq);
    dma->cmd_cq = NULL;
    dma->queues[0] = NULL;
}

static irqreturn_t bce_handle_mb_irq(int irq, void *dev)
{
    struct t2bce_device *bce = pci_get_drvdata(dev);
    bce_mailbox_handle_interrupt(&bce->mbox);
    return IRQ_HANDLED;
}

static void bce_process_dma_completions_locked(struct t2bce_device *bce)
{
    int i;
    size_t ce = 0;
    struct t2bce_dma_engine *dma = &bce->dma;

    spin_lock(&dma->queues_lock);
    for (i = 0; i < BCE_MAX_QUEUE_COUNT; i++)
        if (dma->queues[i] && dma->queues[i]->type == BCE_QUEUE_CQ)
            t2bce_dma_handle_cq_completions_locked(dma, (struct bce_queue_cq *) dma->queues[i], &ce);
    spin_unlock(&dma->queues_lock);
    t2bce_dma_dispatch_pending_sq_completions(dma, ce);
}

static void bce_process_dma_completions(struct t2bce_device *bce)
{
    mutex_lock(&bce->dma_completion_lock);
    bce_process_dma_completions_locked(bce);
    mutex_unlock(&bce->dma_completion_lock);
}

static irqreturn_t bce_handle_dma_irq(int irq, void *dev)
{
    struct t2bce_device *bce = pci_get_drvdata(dev);

    mutex_lock(&bce->dma_completion_lock);
    bce_process_dma_completions_locked(bce);
    mutex_unlock(&bce->dma_completion_lock);
    return IRQ_HANDLED;
}

static void bce_dma_engine_disable(struct t2bce_device *bce)
{
    /*
     * Tahoe closes the submission gate and sleeps until its command queue's
     * pending-element count reaches zero.  Process anything already visible
     * before waiting; MSI0 remains enabled to deliver later completions.
     */
    mutex_lock(&bce->dma_completion_lock);
    t2bce_dma_disable(&bce->dma);
    bce_process_dma_completions_locked(bce);
    mutex_unlock(&bce->dma_completion_lock);
    t2bce_dma_wait_command_queue_idle(&bce->dma);
}

static void bce_dma_engine_enable(struct t2bce_device *bce)
{
    t2bce_dma_enable(&bce->dma);
}

static void bce_dma_irq_disable(struct t2bce_device *bce)
{
    if (bce->dma_irq_disabled)
        return;

    disable_irq(pci_irq_vector(bce->pci, 4));
    bce->dma_irq_disabled = true;
}

static void bce_dma_irq_enable(struct t2bce_device *bce)
{
    if (!bce->dma_irq_disabled)
        return;

    enable_irq(pci_irq_vector(bce->pci, 4));
    bce->dma_irq_disabled = false;
}

static int bce_fw_version_handshake(struct t2bce_device *bce)
{
    u64 result;
    u32 fw_version;
    int status;

    if ((status = bce_mailbox_send(&bce->mbox, BCE_MB_MSG(BCE_MB_SET_FW_PROTOCOL_VERSION, BC_PROTOCOL_VERSION),
            &result)))
        return status;
    fw_version = (u32) BCE_MB_VALUE(result);
    if (BCE_MB_TYPE(result) != BCE_MB_SET_FW_PROTOCOL_VERSION ||
        fw_version > BC_PROTOCOL_VERSION) {
        pr_err("t2bce_core: FW version handshake failed %x:%llx\n", BCE_MB_TYPE(result), BCE_MB_VALUE(result));
        return -EINVAL;
    }
    bce->fw_version = fw_version;
    return 0;
}

static bool bce_stateful_supported(struct t2bce_device *bce)
{
    /* bridgeOS exposes stateful sleep through protocol version 0x20001+. */
    return bce_use_stateful_sleep && bce->fw_version >= BC_PROTOCOL_VERSION;
}

static int bce_pm_wait_mailbox_idle(struct t2bce_device *bce)
{
    int retries = 100;

    while (atomic_read(&bce->mbox.mb_status) != 0) {
        if (!--retries) {
            pr_err("t2bce_core: timeout waiting for BCE mailbox channel to quiesce\n");
            return -ETIMEDOUT;
        }
        usleep_range(5000, 10000);
    }

    return 0;
}

static int bce_pm_channel_pause(struct t2bce_device *bce)
{
    int status;

    status = bce_mailbox_channel_pause(&bce->mbox);
    if (status)
        return status;

    status = bce_pm_wait_mailbox_idle(bce);

    if (status)
        bce_mailbox_channel_resume(&bce->mbox);
    if (status)
        return status;

    /* Keep new mailbox commands out while bridgeOS is entering sleep. */
    bce->mailbox_channel_active = false;
    return 0;
}

static void bce_pm_channel_resume(struct t2bce_device *bce)
{
    /* Reopen mailbox traffic after bridgeOS completed the wake handshake. */
    bce->mailbox_channel_active = true;
    bce_mailbox_channel_resume(&bce->mbox);
}

static int bce_pm_suspend_prepare(struct t2bce_device *bce)
{
    int status;

    bce_dma_engine_disable(bce);
    status = bce_pm_channel_pause(bce);
    if (status) {
        bce_dma_engine_enable(bce);
        return status;
    }

    /* The XHCI PM doorbell is part of the bridgeOS sleep ordering. */
    bce_xhci_pm_stop(&bce->xhci_pm);
    return 0;
}

static void bce_pm_suspend_abort(struct t2bce_device *bce)
{
    /* Failed suspend must unwind the wrapper order back to the running state. */
    bce_dma_irq_enable(bce);
    bce_xhci_pm_start(&bce->xhci_pm, false);
    bce_pm_channel_resume(bce);
    bce_dma_engine_enable(bce);
    bce_process_dma_completions(bce);
}

/* dma_completion_lock serializes this sequence against the threaded MSI0 handler. */
static void bce_pm_resume_finish_locked(struct t2bce_device *bce)
{
    /* Match the bridgeOS wake ordering before clients observe resume complete. */
    bce_xhci_pm_start(&bce->xhci_pm, false);
    bce_pm_channel_resume(bce);
    bce_dma_engine_enable(bce);
    /* Tahoe explicitly services completion vector 0 after enabling the engine. */
    bce_process_dma_completions_locked(bce);
}

static int bce_register_command_queue(struct t2bce_device *bce, struct bce_queue_memcfg *cfg, int is_sq)
{
    int status;
    int cmd_type;
    u64 result;
    /* The firmware accepts a TO_DEVICE mapping for this command payload. */
    dma_addr_t a = dma_map_single(&bce->pci->dev, cfg, sizeof(struct bce_queue_memcfg), DMA_TO_DEVICE);
    if (dma_mapping_error(&bce->pci->dev, a))
        return -ENOMEM;
    cmd_type = is_sq ? BCE_MB_REGISTER_COMMAND_SQ : BCE_MB_REGISTER_COMMAND_CQ;
    status = bce_mailbox_send(&bce->mbox, BCE_MB_MSG(cmd_type, a), &result);
    dma_unmap_single(&bce->pci->dev, a, sizeof(struct bce_queue_memcfg), DMA_TO_DEVICE);
    if (status)
        return status;
    if (BCE_MB_TYPE(result) != BCE_MB_REGISTER_COMMAND_QUEUE_REPLY)
        return -EINVAL;
    return 0;
}

static int t2bce_dma_register_queue(void *userdata, struct bce_queue_memcfg *cfg,
        const char *name, bool isdirout)
{
    struct t2bce_device *bce = userdata;

    return t2bce_dma_cmd_register_queue(bce->dma.cmd_cmdq, cfg, name, isdirout);
}

static int t2bce_dma_unregister_queue(void *userdata, u16 qid)
{
    struct t2bce_device *bce = userdata;

    return t2bce_dma_cmd_unregister_memory_queue(bce->dma.cmd_cmdq, qid);
}

static int t2bce_dma_flush_queue(void *userdata, u16 qid)
{
    struct t2bce_device *bce = userdata;

    return t2bce_dma_cmd_flush_memory_queue(bce->dma.cmd_cmdq, qid);
}

static void t2bce_remove(struct pci_dev *dev)
{
    struct t2bce_device *bce = pci_get_drvdata(dev);
    bce->is_being_removed = true;
    bce->dma.is_being_removed = true;
    global_bce = NULL;

    bce_free_state_buffer(bce);
    bce_xhci_pm_stop(&bce->xhci_pm);
    if (bce->pci0_link)
        device_link_del(bce->pci0_link);
#ifndef WITHOUT_NVME_PATCH
    pci_disable_device(bce->pci0);
#endif
    pci_dev_put(bce->pci0);
    pci_free_irq(dev, 0, dev);
    pci_free_irq(dev, 4, dev);
    bce_free_command_queues(bce);
    t2bce_dma_destroy_segment_list_pool(&bce->dma);
    pci_iounmap(dev, bce->reg_mem_mb);
    pci_iounmap(dev, bce->reg_mem_dma);
    device_destroy(bce_class, bce->devt);
    pci_free_irq_vectors(dev);
    pci_release_regions(dev);
    pci_disable_device(dev);
    cleanup_srcu_struct(&bce->clients_srcu);
    kfree(bce);
}

static void t2bce_shutdown(struct pci_dev *dev)
{
    struct t2bce_device *bce = pci_get_drvdata(dev);
    int status = 0;

    if (!bce)
        return;

    mutex_lock(&bce->pm_lock);
    bce->is_being_removed = true;
    bce->dma.is_being_removed = true;
    bce->stateful_suspend_valid = false;
    bce->no_state_fallback = false;
    bce->no_state_resume = false;
    t2bce_core_clients_pm_reset(bce);

    /*
     * Do not tear down allocations here;
     * just leave the T2 side quiet while command queues and mailbox access are
     * still valid: USB HCD off, XHCI PM sentinel written, mailbox drained.
     */
    t2bce_core_clients_shutdown(bce);
    bce_xhci_pm_stop(&bce->xhci_pm);

    if (bce->mailbox_channel_active) {
        status = bce_pm_channel_pause(bce);
        if (status)
            pr_warn("t2bce_core: shutdown mailbox quiesce failed: %d\n", status);
    }

    mutex_unlock(&bce->pm_lock);
}

static int bce_pm_suspend_fallback_no_state(struct t2bce_device *bce)
{
    pr_debug("t2bce_core: suspend: forcing SLEEP_NO_STATE (no reply expected)\n");
    if (bce_mailbox_send_no_reply_locked(&bce->mbox, BCE_MB_MSG(BCE_MB_SLEEP_NO_STATE, 0))) {
        pr_err("t2bce_core: suspend: SLEEP_NO_STATE send failed\n");
        return -EIO;
    }
    return 0;
}

static int bce_pm_suspend_no_state_fallback(struct t2bce_device *bce)
{
    int status;

    /* Client teardown sends firmware commands, so restore transport first. */
    bce_pm_suspend_abort(bce);

    /* Let ->complete rebuild clients if teardown or the sleep request fails. */
    bce->no_state_fallback = true;
    bce->no_state_resume = true;
    t2bce_core_clients_pm_mark_no_state_resume(bce);

    status = t2bce_core_clients_pm_prepare_no_state(bce);
    if (status) {
        pr_err("t2bce_core: no-state client preparation failed: %d\n", status);
        bce_pm_suspend_abort(bce);
        return status;
    }

    /* Client teardown is complete; quiesce transport before no-state sleep. */
    status = bce_pm_suspend_prepare(bce);
    if (!status)
        status = bce_pm_suspend_fallback_no_state(bce);
    if (status)
        bce_pm_suspend_abort(bce);
    return status;
}

static int bce_pm_suspend_try_state(struct t2bce_device *bce)
{
    int status;
    size_t requested_size;
    u64 resp;

    /* macOS creates a fresh 4 KiB state buffer for every suspend attempt. */
    bce->stateful_suspend_valid = false;
    bce_free_state_buffer(bce);
    status = bce_alloc_state_buffer(bce, BCE_STATE_INITIAL_SIZE);
    if (status)
        return status;

    for (;;) {
        bce->saved_data_dma_addr = dma_map_single(&bce->pci->dev,
                bce->saved_data_dma_ptr, bce->saved_data_dma_size,
                DMA_FROM_DEVICE);
        if (dma_mapping_error(&bce->pci->dev,
                bce->saved_data_dma_addr)) {
            bce->saved_data_dma_addr = 0;
            status = -ENOMEM;
            goto fail;
        }
        if (bce->saved_data_dma_addr & (BCE_STATE_INITIAL_SIZE - 1)) {
            pr_err("t2bce_core: suspend state DMA address is not page aligned\n");
            status = -EINVAL;
            goto unmap_fail;
        }

        status = bce_mailbox_send_locked(&bce->mbox,
                BCE_MB_MSG(BCE_MB_SAVE_STATE_AND_SLEEP,
                        (bce->saved_data_dma_addr & BCE_STATE_ADDR_MASK) |
                        (bce->saved_data_dma_size / BCE_STATE_INITIAL_SIZE)),
                &resp);
        dma_unmap_single(&bce->pci->dev, bce->saved_data_dma_addr,
                bce->saved_data_dma_size, DMA_FROM_DEVICE);
        bce->saved_data_dma_addr = 0;
        if (status) {
            pr_err("t2bce_core: suspend failed (mailbox send)\n");
            goto fail;
        }

        if (BCE_MB_TYPE(resp) == BCE_MB_SAVE_RESTORE_STATE_COMPLETE) {
            pr_debug("t2bce_core: suspend: remote response: restore state saved\n");
            bce->stateful_suspend_valid = true;
            return 0;
        }

        if (BCE_MB_TYPE(resp) != BCE_MB_SAVE_STATE_AND_SLEEP_GROW) {
            pr_err("t2bce_core: unexpected state-save response type %#x\n",
                    BCE_MB_TYPE(resp));
            status = -EINVAL;
            goto fail;
        }

        requested_size = BCE_MB_VALUE(resp);
        if (requested_size <= bce->saved_data_dma_size ||
                requested_size > BCE_STATE_MAX_SIZE) {
            pr_err("t2bce_core: invalid state buffer size request %zu\n",
                    requested_size);
            status = -EINVAL;
            goto fail;
        }

        requested_size = ALIGN(requested_size, BCE_STATE_INITIAL_SIZE);
        pr_debug("t2bce_core: growing suspend state buffer to %zu bytes\n",
                requested_size);
        bce_free_state_buffer(bce);
        status = bce_alloc_state_buffer(bce, requested_size);
        if (status)
            return status;
    }

unmap_fail:
    dma_unmap_single(&bce->pci->dev, bce->saved_data_dma_addr,
            bce->saved_data_dma_size, DMA_FROM_DEVICE);
    bce->saved_data_dma_addr = 0;
fail:
    bce_free_state_buffer(bce);
    return status;
}

static int bce_pm_resume_no_state(struct t2bce_device *bce)
{
    int status;
    u64 resp;

    if ((status = bce_mailbox_send_locked(&bce->mbox, BCE_MB_MSG(BCE_MB_RESTORE_NO_STATE, 0), &resp))) {
        pr_err("t2bce_core: resume with no state failed (mailbox send)\n");
        return status;
    }
    if (BCE_MB_TYPE(resp) != BCE_MB_RESTORE_NO_STATE) {
        pr_err("t2bce_core: resume with no state failed (invalid device response)\n");
        return -EINVAL;
    }
    return 0;
}

static int bce_pm_resume_stateful(struct t2bce_device *bce)
{
    int status;
    u64 resp;

    if (!bce->saved_data_dma_ptr || !bce->saved_data_dma_size)
        return -EINVAL;

    /* macOS prepares a new DMA mapping for the retained state on wake. */
    bce->saved_data_dma_addr = dma_map_single(&bce->pci->dev,
            bce->saved_data_dma_ptr, bce->saved_data_dma_size,
            DMA_TO_DEVICE);
    if (dma_mapping_error(&bce->pci->dev, bce->saved_data_dma_addr)) {
        bce->saved_data_dma_addr = 0;
        bce_free_state_buffer(bce);
        return -ENOMEM;
    }
    if (bce->saved_data_dma_addr & (BCE_STATE_INITIAL_SIZE - 1)) {
        pr_err("t2bce_core: resume state DMA address is not page aligned\n");
        status = -EINVAL;
        goto out_unmap;
    }

    status = bce_mailbox_send_locked(&bce->mbox,
            BCE_MB_MSG(BCE_MB_RESTORE_STATE_AND_WAKE,
                    (bce->saved_data_dma_addr & BCE_STATE_ADDR_MASK) |
                    (bce->saved_data_dma_size / BCE_STATE_INITIAL_SIZE)),
            &resp);
    if (status) {
        pr_err("t2bce_core: resume with state failed (mailbox send)\n");
        goto out_unmap;
    }
    if (BCE_MB_TYPE(resp) != BCE_MB_SAVE_RESTORE_STATE_COMPLETE) {
        pr_err("t2bce_core: resume with state failed (invalid device response)\n");
        status = -EINVAL;
    }

out_unmap:
    dma_unmap_single(&bce->pci->dev, bce->saved_data_dma_addr,
            bce->saved_data_dma_size, DMA_TO_DEVICE);
    bce->saved_data_dma_addr = 0;
    bce_free_state_buffer(bce);
    return status;
}

static int t2bce_suspend(struct device *dev)
{
    struct t2bce_device *bce = pci_get_drvdata(to_pci_dev(dev));
    int status;

    pr_info("t2bce_core: suspend: entry\n");
    mutex_lock(&bce->pm_lock);

    bce->stateful_suspend_valid = false;
    bce->no_state_fallback = false;
    bce->no_state_resume = false;
    t2bce_core_clients_pm_reset(bce);
    status = t2bce_core_clients_pm_prepare(bce);
    if (status)
        goto out_unlock;

    status = bce_pm_suspend_prepare(bce);
    if (status)
        goto out_unlock;

    if (!bce_stateful_supported(bce)) {
        /* No-state resume tears down and rebuilds clients that cannot preserve firmware state. */
        status = bce_pm_suspend_no_state_fallback(bce);
        goto out_unlock;
    }

    status = bce_pm_suspend_try_state(bce);
    if (!status)
        goto out_unlock;

    if (status != -EAGAIN) {
        bce_pm_suspend_abort(bce);
        goto out_unlock;
    }

    /* bridgeOS can reject state save; fall back to the no-state path then. */
    pr_info("t2bce_core: suspend: stateful path not ready, falling back to no-state\n");
    status = bce_pm_suspend_no_state_fallback(bce);

out_unlock:
    if (!status)
        bce_dma_irq_disable(bce);
    mutex_unlock(&bce->pm_lock);
    pr_info("t2bce_core: suspend: exit status=%d stateful_valid=%d no_state_resume=%d no_state_fallback=%d\n",
            status, bce->stateful_suspend_valid, bce->no_state_resume, bce->no_state_fallback);
    return status;
}

static int t2bce_resume(struct device *dev)
{
    struct t2bce_device *bce = pci_get_drvdata(to_pci_dev(dev));
    int status;
    bool used_stateful;

    pr_info("t2bce_core: resume: entry\n");
    mutex_lock(&bce->pm_lock);
    /* Serialize MSI0 delivery across RESTORE, engine enable and the CQ0 poll. */
    mutex_lock(&bce->dma_completion_lock);

    pci_set_master(bce->pci);
    pci_set_master(bce->pci0);
    /* Tahoe enables MSI0 before issuing the RESTORE/WAKE mailbox command. */
    bce_dma_irq_enable(bce);

    /* Resume follows the suspend result, not a preselected policy. */
    used_stateful = bce_stateful_supported(bce) && bce->stateful_suspend_valid;
    pr_info("t2bce_core: resume path: %s\n", used_stateful ? "stateful" : "no-state");
    if (used_stateful)
        status = bce_pm_resume_stateful(bce);
    else
        status = bce_pm_resume_no_state(bce);
    if (status) {
        bce_pm_resume_finish_locked(bce);
        goto out_unlock;
    }

    if (used_stateful)
        bce->stateful_suspend_valid = false;

    bce_pm_resume_finish_locked(bce);

out_unlock:
    mutex_unlock(&bce->dma_completion_lock);
    mutex_unlock(&bce->pm_lock);
    pr_info("t2bce_core: resume: exit status=%d path=%s stateful_valid=%d no_state_resume=%d no_state_fallback=%d\n",
            status, used_stateful ? "stateful" : "no-state",
            bce->stateful_suspend_valid, bce->no_state_resume, bce->no_state_fallback);
    return status;
}

static int t2bce_prepare(struct device *dev)
{
    /*
     * Force PCI PM to run our real suspend/resume callbacks instead of
     * short-cutting an "already runtime suspended" device through complete().
     * The T2 mailbox save/restore handshake lives in those callbacks.
     */
    return 0;
}

static void t2bce_complete(struct device *dev)
{
    struct t2bce_device *bce = pci_get_drvdata(to_pci_dev(dev));

    pr_debug("t2bce_core: complete: entry no_state_fallback=%d no_state_resume=%d\n",
            bce->no_state_fallback, bce->no_state_resume);
    if (bce->no_state_fallback && t2bce_core_clients_pm_has_no_state_resume(bce)) {
        t2bce_core_clients_pm_complete(bce);
        bce->no_state_fallback = false;
    }

    t2bce_core_notify_resume_complete(bce);
    bce->no_state_resume = false;
    pr_debug("t2bce_core: complete: exit\n");
}

static struct pci_device_id t2bce_ids[  ] = {
        { PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1801) },
        { 0, },
};

MODULE_DEVICE_TABLE(pci, t2bce_ids);

struct dev_pm_ops t2bce_pci_driver_pm = {
        .prepare = t2bce_prepare,
        .suspend = t2bce_suspend,
        .resume = t2bce_resume,
        .complete = t2bce_complete
};
struct pci_driver t2bce_pci_driver = {
        .name = "t2bce_core",
        .id_table = t2bce_ids,
        .probe = t2bce_probe,
        .remove = t2bce_remove,
        .shutdown = t2bce_shutdown,
        .driver = {
                .pm = &t2bce_pci_driver_pm
        }
};


static int __init t2bce_module_init(void)
{
    int result;

    if ((result = alloc_chrdev_region(&bce_chrdev, 0, 1, "t2bce_core")))
        goto fail_chrdev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,4,0)
    bce_class = class_create(THIS_MODULE, "t2bce_core");
#else
    bce_class = class_create("t2bce_core");
#endif
    if (IS_ERR(bce_class)) {
        result = PTR_ERR(bce_class);
        goto fail_class;
    }
    result = pci_register_driver(&t2bce_pci_driver);
    if (result)
        goto fail_drv;

    pr_info("t2bce_core: module initialized\n");
    return 0;

fail_drv:
    pci_unregister_driver(&t2bce_pci_driver);
fail_class:
    class_destroy(bce_class);
fail_chrdev:
    unregister_chrdev_region(bce_chrdev, 1);
    if (!result)
        result = -EINVAL;
    pr_info("t2bce_core: module init failed status=%d\n", result);
    return result;
}
static void __exit t2bce_module_exit(void)
{
    pci_unregister_driver(&t2bce_pci_driver);

    class_destroy(bce_class);
    unregister_chrdev_region(bce_chrdev, 1);
    pr_info("t2bce_core: module exited\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("André Eikmeyer <andre.eikmeyer@kait2en.org>");
MODULE_DESCRIPTION("T2 BCE core driver");
MODULE_VERSION("0.09");
MODULE_SOFTDEP("post: t2bce_vhci");
module_init(t2bce_module_init);
module_exit(t2bce_module_exit);
