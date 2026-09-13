#include "mailbox.h"
#include <linux/atomic.h>
#include "t2bce.h"
#include <linux/version.h>

#define REG_MBOX_OUT_BASE 0x820
#define REG_MBOX_REPLY_COUNTER 0x108
#define REG_MBOX_REPLY_BASE 0x810
#define REG_TIMESTAMP_BASE 0xC000

#define BCE_MBOX_TIMEOUT_MS 1000

void bce_mailbox_init(struct bce_mailbox *mb, void __iomem *reg_mb)
{
    mb->reg_mb = reg_mb;
    mutex_init(&mb->channel_lock);
    init_completion(&mb->mb_completion);
}

int bce_mailbox_channel_pause(struct bce_mailbox *mb)
{
    /*
     * System PM needs to exclude new mailbox transactions while the core
     * driver is moving bridgeOS into or out of sleep.
     */
    mutex_lock(&mb->channel_lock);
    return 0;
}

void bce_mailbox_channel_resume(struct bce_mailbox *mb)
{
    mutex_unlock(&mb->channel_lock);
}

int bce_mailbox_send_locked_timeout(struct bce_mailbox *mb, u64 msg, u64* recv, unsigned int timeout_ms)
{
    u32 __iomem *regb;
    int status = 0;

    if (atomic_cmpxchg(&mb->mb_status, 0, 1) != 0) {
        status = -EEXIST;
        goto out;
    }
    reinit_completion(&mb->mb_completion);

    pr_debug("bce_mailbox_send: %llx\n", msg);
    regb = (u32*) ((u8*) mb->reg_mb + REG_MBOX_OUT_BASE);
    iowrite32((u32) msg, regb);
    iowrite32((u32) (msg >> 32), regb + 1);
    iowrite32(0, regb + 2);
    iowrite32(0, regb + 3);

    wait_for_completion_timeout(&mb->mb_completion, msecs_to_jiffies(timeout_ms));
    if (atomic_read(&mb->mb_status) != 2) {
        atomic_set(&mb->mb_status, 0);
        status = -ETIMEDOUT;
        goto out;
    }

    *recv = mb->mb_result;
    pr_debug("bce_mailbox_send: reply %llx\n", *recv);

    atomic_set(&mb->mb_status, 0);
out:
    return status;
}

int bce_mailbox_send_timeout(struct bce_mailbox *mb, u64 msg, u64* recv, unsigned int timeout_ms)
{
    int status;

    /* Serialize mailbox traffic; bridgeOS accepts one active command at a time. */
    mutex_lock(&mb->channel_lock);
    status = bce_mailbox_send_locked_timeout(mb, msg, recv, timeout_ms);
    mutex_unlock(&mb->channel_lock);
    return status;
}

int bce_mailbox_send(struct bce_mailbox *mb, u64 msg, u64* recv)
{
    return bce_mailbox_send_timeout(mb, msg, recv, BCE_MBOX_TIMEOUT_MS);
}

int bce_mailbox_send_locked(struct bce_mailbox *mb, u64 msg, u64 *recv)
{
    return bce_mailbox_send_locked_timeout(mb, msg, recv, BCE_MBOX_TIMEOUT_MS);
}

int bce_mailbox_send_no_reply_locked(struct bce_mailbox *mb, u64 msg)
{
    u32 __iomem *regb;
    int status = 0;

    if (atomic_cmpxchg(&mb->mb_status, 0, 1) != 0) {
        status = -EEXIST;
        goto out;
    }

    pr_debug("bce_mailbox_send_no_reply: %llx\n", msg);
    regb = (u32*) ((u8*) mb->reg_mb + REG_MBOX_OUT_BASE);
    iowrite32((u32) msg, regb);
    iowrite32((u32) (msg >> 32), regb + 1);
    iowrite32(0, regb + 2);
    iowrite32(0, regb + 3);

    atomic_set(&mb->mb_status, 0);
out:
    return status;
}

int bce_mailbox_send_no_reply(struct bce_mailbox *mb, u64 msg)
{
    int status;

    /* Fire-and-forget commands share the same mailbox serialization. */
    mutex_lock(&mb->channel_lock);
    status = bce_mailbox_send_no_reply_locked(mb, msg);
    mutex_unlock(&mb->channel_lock);
    return status;
}

static int bce_mailbox_retrive_response(struct bce_mailbox *mb)
{
    u32 __iomem *regb;
    u32 lo, hi;
    int count, counter;
    u32 res = ioread32((u8*) mb->reg_mb + REG_MBOX_REPLY_COUNTER);
    count = (res >> 20) & 0xf;
    counter = count;
    pr_debug("bce_mailbox_retrive_response count=%i\n", count);
    while (counter--) {
        regb = (u32*) ((u8*) mb->reg_mb + REG_MBOX_REPLY_BASE);
        lo = ioread32(regb);
        hi = ioread32(regb + 1);
        ioread32(regb + 2);
        ioread32(regb + 3);
        pr_debug("bce_mailbox_retrive_response %llx\n", ((u64) hi << 32) | lo);
        mb->mb_result = ((u64) hi << 32) | lo;
    }
    return count > 0 ? 0 : -ENODATA;
}

int bce_mailbox_handle_interrupt(struct bce_mailbox *mb)
{
    int status = bce_mailbox_retrive_response(mb);
    if (!status) {
        if (atomic_read(&mb->mb_status) == 1) {
            atomic_set(&mb->mb_status, 2);
            complete(&mb->mb_completion);
        } else {
            pr_debug("bce_mailbox_handle_interrupt: unsolicited reply %llx\n", mb->mb_result);
            atomic_set(&mb->mb_status, 0);
        }
    }
    return status;
}

static void bce_xhci_pm_tick(struct timer_list *tl);

void bce_xhci_pm_init(struct bce_xhci_pm *pm, void __iomem *reg)
{
    u32 __iomem *regb;

    spin_lock_init(&pm->stop_sl);
    pm->stopped = false;

    pm->reg = reg;

    regb = (u32*) ((u8*) pm->reg + REG_TIMESTAMP_BASE);

    ioread32(regb);
    mb();

    timer_setup(&pm->timer, bce_xhci_pm_tick, 0);
}

void bce_xhci_pm_start(struct bce_xhci_pm *pm, bool is_initial)
{
    unsigned long flags;
    u32 __iomem *regb = (u32*) ((u8*) pm->reg + REG_TIMESTAMP_BASE);

    if (is_initial) {
        iowrite32((u32) -4, regb + 2);
        iowrite32((u32) -1, regb);
    } else {
        iowrite32((u32) -3, regb + 2);
        iowrite32((u32) -1, regb);
    }

    spin_lock_irqsave(&pm->stop_sl, flags);
    pm->stopped = false;
    spin_unlock_irqrestore(&pm->stop_sl, flags);
    mod_timer(&pm->timer, jiffies + msecs_to_jiffies(150));
}

void bce_xhci_pm_stop(struct bce_xhci_pm *pm)
{
    unsigned long flags;
    u32 __iomem *regb = (u32*) ((u8*) pm->reg + REG_TIMESTAMP_BASE);

    spin_lock_irqsave(&pm->stop_sl, flags);
    pm->stopped = true;
    spin_unlock_irqrestore(&pm->stop_sl, flags);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,15,0)
    del_timer_sync(&pm->timer);
#else
    timer_delete_sync(&pm->timer);
#endif
    iowrite32((u32) -2, regb + 2);
    iowrite32((u32) -1, regb);
}

static void bce_xhci_pm_tick(struct timer_list *tl)
{
    struct bce_xhci_pm *pm;
    unsigned long flags;
    u32 __iomem *regb;
    ktime_t bt;

    pm = container_of(tl, struct bce_xhci_pm, timer);
    regb = (u32*) ((u8*) pm->reg + REG_TIMESTAMP_BASE);
    local_irq_save(flags);
    ioread32(regb + 2);
    mb();
    bt = ktime_get_boottime();
    iowrite32((u32) bt, regb + 2);
    iowrite32((u32) (bt >> 32), regb);

    spin_lock(&pm->stop_sl);
    if (!pm->stopped)
        mod_timer(&pm->timer, jiffies + msecs_to_jiffies(150));
    spin_unlock(&pm->stop_sl);
    local_irq_restore(flags);
}
