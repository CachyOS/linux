#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/err.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/jack.h>
#include "audio.h"
#include "pcm.h"
#include <linux/version.h>

static int t2audio_alsa_index = SNDRV_DEFAULT_IDX1;
static char *t2audio_alsa_id = SNDRV_DEFAULT_STR1;

static dev_t t2audio_chrdev;
static struct class *t2audio_class;

static int t2audio_init_cmd(struct t2audio_device *a);
static int t2audio_init_bs(struct t2audio_device *a);
static void t2audio_init_dev(struct t2audio_device *a, t2audio_device_id_t dev_id);
static void t2audio_free_dev(struct t2audio_subdevice *sdev);
static void t2audio_reset_stream(struct t2audio_stream *stream);
static void t2audio_reset_streams(struct t2audio_device *a);
static void t2audio_reset_clock(struct t2audio_device *a);
static void t2audio_resume_work(struct work_struct *ws);
static void t2audio_resume_complete(void *userdata);
static void t2audio_pm_prepare_client(void *userdata);
static void t2audio_pm_shutdown_client(void *userdata);

static const struct t2bce_core_client_pm_ops t2audio_pm_ops = {
        .shutdown = t2audio_pm_shutdown_client,
        .pm_prepare = t2audio_pm_prepare_client,
};

static int t2audio_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct t2audio_device *t2audio = NULL;
    struct t2audio_subdevice *sdev = NULL;
    int status = 0;
    u32 cfg;

    pr_debug("t2bce_audio: capturing our device\n");

    if (pci_enable_device(dev))
        return -ENODEV;
    if (pci_request_regions(dev, "t2bce_audio")) {
        status = -ENODEV;
        goto fail;
    }
    pci_set_master(dev);

    t2audio = kzalloc(sizeof(struct t2audio_device), GFP_KERNEL);
    if (!t2audio) {
        status = -ENOMEM;
        goto fail;
    }

    t2audio->bce = t2bce_core_client_get(&dev->dev);
    if (IS_ERR(t2audio->bce)) {
        status = PTR_ERR(t2audio->bce);
        t2audio->bce = NULL;
        if (status != -EPROBE_DEFER)
            dev_warn(&dev->dev, "t2bce_audio: Failed to get BCE client: %d\n", status);
        goto fail;
    }

    t2audio->pci = dev;
    pci_set_drvdata(dev, t2audio);
    t2bce_core_client_set_resume_complete_callback(t2audio->bce, t2audio_resume_complete, t2audio);
    t2bce_core_client_set_pm_ops(t2audio->bce, &t2audio_pm_ops, t2audio);

    t2audio->devt = t2audio_chrdev;
    t2audio->dev = device_create(t2audio_class, &dev->dev, t2audio->devt, NULL, "t2bce_audio");
    if (IS_ERR_OR_NULL(t2audio->dev)) {
        status = PTR_ERR(t2audio->dev);
        goto fail;
    }
    init_completion(&t2audio->remote_alive);
    INIT_WORK(&t2audio->resume_work, t2audio_resume_work);
    INIT_LIST_HEAD(&t2audio->subdevice_list);
    spin_lock_init(&t2audio->clock_lock);
    t2audio->clock_samples = 0;
    t2audio->clock_offset_valid = false;

    /* Init: set an unknown flag in the bitset */
    if (pci_read_config_dword(dev, 4, &cfg))
        dev_warn(&dev->dev, "t2bce_audio: pci_read_config_dword fail\n");
    if (pci_write_config_dword(dev, 4, cfg | 6u))
        dev_warn(&dev->dev, "t2bce_audio: pci_write_config_dword fail\n");

    pr_debug("t2bce_audio: bs len = %llx\n", pci_resource_len(dev, 0));
    t2audio->reg_mem_bs_dma = pci_resource_start(dev, 0);
    t2audio->reg_mem_bs = pci_iomap(dev, 0, 0);
    t2audio->reg_mem_cfg = pci_iomap(dev, 4, 0);

    t2audio->reg_mem_gpr = (u32 __iomem *) ((u8 __iomem *) t2audio->reg_mem_cfg + 0xC000);

    if (IS_ERR_OR_NULL(t2audio->reg_mem_bs) || IS_ERR_OR_NULL(t2audio->reg_mem_cfg)) {
        dev_warn(&dev->dev, "t2bce_audio: Failed to pci_iomap required regions\n");
        goto fail;
    }

    if (t2audio_bce_init(t2audio)) {
        dev_warn(&dev->dev, "t2bce_audio: Failed to init BCE command transport\n");
        goto fail;
    }

    if (snd_card_new(t2audio->dev, t2audio_alsa_index, t2audio_alsa_id, THIS_MODULE, 0, &t2audio->card)) {
        dev_err(&dev->dev, "t2bce_audio: Failed to create ALSA card\n");
        goto fail;
    }

    strcpy(t2audio->card->shortname, "Apple T2 Audio");
    strcpy(t2audio->card->longname, "Apple T2 Audio");
    strcpy(t2audio->card->mixername, "Apple T2 Audio");
    /* Dynamic alsa ids start at 100 */
    t2audio->next_alsa_id = 100;

    if (t2audio_init_cmd(t2audio)) {
        dev_err(&dev->dev, "t2bce_audio: Failed to initialize over BCE\n");
        goto fail_snd;
    }

    if (t2audio_init_bs(t2audio)) {
        dev_err(&dev->dev, "t2bce_audio: Failed to initialize BufferStruct\n");
        goto fail_snd;
    }

    if ((status = t2audio_cmd_set_remote_access(t2audio, T2AUDIO_REMOTE_ACCESS_ON))) {
        dev_err(&dev->dev, "Failed to set remote access\n");
        goto fail_snd;
    }

    if (snd_card_register(t2audio->card)) {
        dev_err(&dev->dev, "t2bce_audio: Failed to register ALSA sound device\n");
        goto fail_snd;
    }

    list_for_each_entry(sdev, &t2audio->subdevice_list, list) {
        struct t2audio_buffer_struct_device *dev = &t2audio->bs->devices[sdev->buf_id];

        if (sdev->out_stream_cnt == 1 && !strcmp(dev->name, "Speaker")) {
            struct snd_pcm_hardware *hw = sdev->out_streams[0].alsa_hw_desc;

            snprintf(t2audio->card->driver, sizeof(t2audio->card->driver) / sizeof(char), "AppleT2x%d", hw->channels_min);
        }
    }

    return 0;

fail_snd:
    snd_card_free(t2audio->card);
fail:
    if (t2audio) {
        if (t2audio->dev)
            device_destroy(t2audio_class, t2audio->devt);
        if (!IS_ERR_OR_NULL(t2audio->reg_mem_bs))
            pci_iounmap(dev, t2audio->reg_mem_bs);
        if (!IS_ERR_OR_NULL(t2audio->reg_mem_cfg))
            pci_iounmap(dev, t2audio->reg_mem_cfg);
        if (t2audio->bce)
            t2bce_core_client_set_pm_ops(t2audio->bce, NULL, NULL);
        t2bce_core_client_put(t2audio->bce);
        kfree(t2audio);
    }

    pci_release_regions(dev);
    pci_disable_device(dev);

    if (!status)
        status = -EINVAL;
    return status;
}



static void t2audio_remove(struct pci_dev *dev)
{
    struct t2audio_subdevice *sdev;
    struct t2audio_device *t2audio = pci_get_drvdata(dev);

    cancel_work_sync(&t2audio->resume_work);
    snd_card_free(t2audio->card);
    while (!list_empty(&t2audio->subdevice_list)) {
        sdev = list_first_entry(&t2audio->subdevice_list, struct t2audio_subdevice, list);
        list_del(&sdev->list);
        t2audio_free_dev(sdev);
    }
    pci_iounmap(dev, t2audio->reg_mem_bs);
    pci_iounmap(dev, t2audio->reg_mem_cfg);
    device_destroy(t2audio_class, t2audio->devt);
    pci_free_irq_vectors(dev);
    pci_release_regions(dev);
    pci_disable_device(dev);
    t2bce_core_client_set_pm_ops(t2audio->bce, NULL, NULL);
    t2bce_core_client_put(t2audio->bce);
    kfree(t2audio);
}

static int t2audio_quiesce(struct t2audio_device *t2audio, bool suspend_pcm)
{
    struct t2audio_subdevice *sdev;
    size_t i;
    int status;

    if (t2audio->pm_quiesced)
        return 0;

    cancel_work_sync(&t2audio->resume_work);
    t2audio->resume_deferred = false;

    /* Suspend PCM streams */
    list_for_each_entry(sdev, &t2audio->subdevice_list, list) {
        bool stopped_io = false;

        for (i = 0; i < sdev->out_stream_cnt; i++) {
            if (!smp_load_acquire(&sdev->out_streams[i].started))
                continue;
            stopped_io = true;
            smp_store_release(&sdev->out_streams[i].started, 0);
        }

        for (i = 0; i < sdev->in_stream_cnt; i++) {
            if (!smp_load_acquire(&sdev->in_streams[i].started))
                continue;
            stopped_io = true;
            smp_store_release(&sdev->in_streams[i].started, 0);
        }

        if (stopped_io)
            t2audio_cmd_stop_io(sdev->a, sdev->dev_id);

        if (suspend_pcm && sdev->pcm)
            snd_pcm_suspend_all(sdev->pcm);
    }

    status = t2audio_cmd_set_remote_access(t2audio, T2AUDIO_REMOTE_ACCESS_OFF);
    if (status)
        dev_warn(t2audio->dev, "Failed to reset remote access\n");
    else
        t2audio->pm_quiesced = true;

    return status;
}

static int t2audio_suspend(struct device *dev)
{
    struct t2audio_device *t2audio = pci_get_drvdata(to_pci_dev(dev));
    int status;

    pr_debug("t2bce_audio: suspend entry\n");
    status = t2audio_quiesce(t2audio, true);
    pci_disable_device(t2audio->pci);
    pr_info("t2bce_audio: suspend exit status=%d\n", status);
    return 0;
}

static void t2audio_pm_prepare_client(void *userdata)
{
    t2audio_quiesce(userdata, true);
}

static void t2audio_pm_shutdown_client(void *userdata)
{
    t2audio_quiesce(userdata, false);
}

static void t2audio_shutdown(struct pci_dev *dev)
{
    struct t2audio_device *t2audio = pci_get_drvdata(dev);

    if (!t2audio)
        return;

    /*
     * Shutdown is not remove: keep allocations intact, but stop active audio
     * IO and revoke remote access while the BCE command transport is still
     * alive. The t2bce shutdown path will close the shared bus afterwards.
     */
    t2audio_quiesce(t2audio, false);
    pci_disable_device(t2audio->pci);
}

static int t2audio_resume(struct device *dev)
{
    int status;
    struct t2audio_device *t2audio = pci_get_drvdata(to_pci_dev(dev));
    bool no_state_resume = t2bce_core_client_no_state_resume(t2audio->bce);
    const char *path = no_state_resume ? "no-state" : "stateful";

    if ((status = pci_enable_device(t2audio->pci))) {
        pr_info("t2bce_audio: resume exit status=%d path=%s\n", status, path);
        return status;
    }
    pci_set_master(t2audio->pci);

    /* we are deferring t2audio resume here until vhci is finished*/
    if (!no_state_resume) {
        t2audio->resume_deferred = true;
        return 0;
    }

    if ((status = t2audio_cmd_set_remote_access(t2audio, T2AUDIO_REMOTE_ACCESS_ON))) {
        dev_err(t2audio->dev, "Failed to set remote access\n");
        pr_info("t2bce_audio: resume exit status=%d path=%s\n", status, path);
        return status;
    }

    t2audio->resume_deferred = false;
    t2audio->pm_quiesced = false;
    t2audio_reset_clock(t2audio);
    t2audio_reset_streams(t2audio);

    pr_info("t2bce_audio: resume exit status=0 path=%s\n", path);
    return 0;
}

static void t2audio_resume_work(struct work_struct *ws)
{
    struct t2audio_device *t2audio = container_of(ws, struct t2audio_device, resume_work);

    if (!t2audio->resume_deferred)
        return;

    if (t2audio_cmd_set_remote_access(t2audio, T2AUDIO_REMOTE_ACCESS_ON)) {
        t2audio->resume_deferred = false;
        dev_err(t2audio->dev, "Deferred remote access enable failed\n");
        pr_info("t2bce_audio: resume deferred path failed\n");
        return;
    }

    t2audio->resume_deferred = false;
    t2audio->pm_quiesced = false;
    t2audio_reset_clock(t2audio);
    t2audio_reset_streams(t2audio);
    pr_info("t2bce_audio: resume deferred path complete\n");
}

static void t2audio_resume_complete(void *userdata)
{
    struct t2audio_device *t2audio = userdata;

    if (!t2audio || !t2audio->resume_deferred)
        return;

    schedule_work(&t2audio->resume_work);
}

static void t2audio_reset_stream(struct t2audio_stream *stream)
{
    smp_store_release(&stream->started, 0);
    stream->waiting_for_first_ts = true;
    stream->remote_timestamp = 0;
    stream->timestamp_accept_after = 0;
    stream->clock_offset_ns = 0;
    stream->timestamp_seed = 0;
    stream->clock_offset_valid = false;
    stream->frame_min = stream->latency;
}

static void t2audio_reset_clock(struct t2audio_device *a)
{
    unsigned long flags;

    spin_lock_irqsave(&a->clock_lock, flags);
    a->clock_offset_ns = 0;
    a->clock_samples = 0;
    a->clock_offset_valid = false;
    spin_unlock_irqrestore(&a->clock_lock, flags);
}

static void t2audio_reset_streams(struct t2audio_device *a)
{
    struct t2audio_subdevice *sdev;
    size_t i;

    list_for_each_entry(sdev, &a->subdevice_list, list) {
        for (i = 0; i < sdev->in_stream_cnt; i++)
            t2audio_reset_stream(&sdev->in_streams[i]);
        for (i = 0; i < sdev->out_stream_cnt; i++)
            t2audio_reset_stream(&sdev->out_streams[i]);
    }
}

static int t2audio_init_cmd(struct t2audio_device *a)
{
    int status;
    struct t2audio_send_ctx sctx;
    struct t2audio_msg buf;
    u64 dev_cnt, dev_i;
    t2audio_device_id_t *dev_l;

    if ((status = t2audio_send(a, &sctx, 500,
                              t2audio_msg_write_alive_notification, 1, 3))) {
        dev_err(a->dev, "Sending alive notification failed\n");
        return status;
    }

    if (wait_for_completion_timeout(&a->remote_alive, msecs_to_jiffies(500)) == 0) {
        dev_err(a->dev, "Timed out waiting for remote\n");
        return -ETIMEDOUT;
    }
    pr_debug("t2bce_audio: Continuing init\n");

    buf = t2audio_reply_alloc();
    if ((status = t2audio_cmd_get_device_list(a, &buf, &dev_l, &dev_cnt))) {
        dev_err(a->dev, "Failed to get device list\n");
        t2audio_reply_free(&buf);
        return status;
    }
    for (dev_i = 0; dev_i < dev_cnt; ++dev_i)
        t2audio_init_dev(a, dev_l[dev_i]);
    t2audio_reply_free(&buf);

    return 0;
}

static void t2audio_init_stream_info(struct t2audio_subdevice *sdev, struct t2audio_stream *strm);
static void t2audio_handle_jack_connection_change(struct t2audio_subdevice *sdev);

static void t2audio_init_dev(struct t2audio_device *a, t2audio_device_id_t dev_id)
{
    struct t2audio_subdevice *sdev;
    struct t2audio_msg buf = t2audio_reply_alloc();
    u64 uid_len, stream_cnt, i;
    t2audio_object_id_t *stream_list;
    char *uid;

    sdev = kzalloc(sizeof(struct t2audio_subdevice), GFP_KERNEL);

    if (t2audio_cmd_get_property(a, &buf, dev_id, dev_id, T2AUDIO_PROP(T2AUDIO_PROP_SCOPE_GLOBAL, T2AUDIO_PROP_UID, 0),
            NULL, 0, (void **) &uid, &uid_len) || uid_len > T2AUDIO_DEVICE_MAX_UID_LEN) {
        dev_err(a->dev, "Failed to get device uid for device %llx\n", dev_id);
        goto fail;
    }
    pr_debug("t2bce_audio: Remote device %llx %.*s\n", dev_id, (int) uid_len, uid);

    sdev->a = a;
    INIT_LIST_HEAD(&sdev->list);
    sdev->dev_id = dev_id;
    sdev->buf_id = T2AUDIO_BUFFER_ID_NONE;
    strncpy(sdev->uid, uid, uid_len);
    sdev->uid[uid_len + 1] = '\0';

    if (t2audio_cmd_get_primitive_property(a, dev_id, dev_id,
            T2AUDIO_PROP(T2AUDIO_PROP_SCOPE_INPUT, T2AUDIO_PROP_LATENCY, 0), NULL, 0, &sdev->in_latency, sizeof(u32)))
        dev_warn(a->dev, "Failed to query device input latency\n");
    if (t2audio_cmd_get_primitive_property(a, dev_id, dev_id,
            T2AUDIO_PROP(T2AUDIO_PROP_SCOPE_OUTPUT, T2AUDIO_PROP_LATENCY, 0), NULL, 0, &sdev->out_latency, sizeof(u32)))
        dev_warn(a->dev, "Failed to query device output latency\n");

    if (t2audio_cmd_get_input_stream_list(a, &buf, dev_id, &stream_list, &stream_cnt)) {
        dev_err(a->dev, "Failed to get input stream list for device %llx\n", dev_id);
        goto fail;
    }
    if (stream_cnt > T2AUDIO_DEVICE_MAX_INPUT_STREAMS) {
        dev_warn(a->dev, "Device %s input stream count %llu is larger than the supported count of %u\n",
                sdev->uid, stream_cnt, T2AUDIO_DEVICE_MAX_INPUT_STREAMS);
        stream_cnt = T2AUDIO_DEVICE_MAX_INPUT_STREAMS;
    }
    sdev->in_stream_cnt = stream_cnt;
    for (i = 0; i < stream_cnt; i++) {
        sdev->in_streams[i].id = stream_list[i];
        sdev->in_streams[i].buffer_cnt = 0;
        t2audio_init_stream_info(sdev, &sdev->in_streams[i]);
        sdev->in_streams[i].latency += sdev->in_latency;
    }

    if (t2audio_cmd_get_output_stream_list(a, &buf, dev_id, &stream_list, &stream_cnt)) {
        dev_err(a->dev, "Failed to get output stream list for device %llx\n", dev_id);
        goto fail;
    }
    if (stream_cnt > T2AUDIO_DEVICE_MAX_OUTPUT_STREAMS) {
        dev_warn(a->dev, "Device %s output stream count %llu is larger than the supported count of %u\n",
                 sdev->uid, stream_cnt, T2AUDIO_DEVICE_MAX_OUTPUT_STREAMS);
        stream_cnt = T2AUDIO_DEVICE_MAX_OUTPUT_STREAMS;
    }
    sdev->out_stream_cnt = stream_cnt;
    for (i = 0; i < stream_cnt; i++) {
        sdev->out_streams[i].id = stream_list[i];
        sdev->out_streams[i].buffer_cnt = 0;
        t2audio_init_stream_info(sdev, &sdev->out_streams[i]);
        sdev->out_streams[i].latency += sdev->out_latency;
    }

    if (sdev->is_pcm)
        t2audio_create_pcm(sdev);
    /* Headphone Jack status */
    if (!strcmp(sdev->uid, "Codec Output")) {
        if (snd_jack_new(a->card, sdev->uid, SND_JACK_HEADPHONE, &sdev->jack, true, false))
            dev_warn(a->dev, "Failed to create an attached jack for %s\n", sdev->uid);
        t2audio_cmd_property_listener(a, sdev->dev_id, sdev->dev_id,
                T2AUDIO_PROP(T2AUDIO_PROP_SCOPE_OUTPUT, T2AUDIO_PROP_JACK_PLUGGED, 0));
        t2audio_handle_jack_connection_change(sdev);
    }

    t2audio_reply_free(&buf);

    list_add_tail(&sdev->list, &a->subdevice_list);
    return;

fail:
    t2audio_reply_free(&buf);
    kfree(sdev);
}

static void t2audio_init_stream_info(struct t2audio_subdevice *sdev, struct t2audio_stream *strm)
{
    if (t2audio_cmd_get_primitive_property(sdev->a, sdev->dev_id, strm->id,
            T2AUDIO_PROP(T2AUDIO_PROP_SCOPE_GLOBAL, T2AUDIO_PROP_PHYS_FORMAT, 0), NULL, 0,
            &strm->desc, sizeof(strm->desc)))
        dev_warn(sdev->a->dev, "Failed to query stream descriptor\n");
    if (t2audio_cmd_get_primitive_property(sdev->a, sdev->dev_id, strm->id,
            T2AUDIO_PROP(T2AUDIO_PROP_SCOPE_GLOBAL, T2AUDIO_PROP_LATENCY, 0), NULL, 0, &strm->latency, sizeof(u32)))
        dev_warn(sdev->a->dev, "Failed to query stream latency\n");
    if (strm->desc.format_id == T2AUDIO_FORMAT_LPCM)
        sdev->is_pcm = true;
}

static void t2audio_free_dev(struct t2audio_subdevice *sdev)
{
    size_t i;
    for (i = 0; i < sdev->in_stream_cnt; i++) {
        if (sdev->in_streams[i].alsa_hw_desc)
            kfree(sdev->in_streams[i].alsa_hw_desc);
        if (sdev->in_streams[i].buffers)
            kfree(sdev->in_streams[i].buffers);
    }
    for (i = 0; i < sdev->out_stream_cnt; i++) {
        if (sdev->out_streams[i].alsa_hw_desc)
            kfree(sdev->out_streams[i].alsa_hw_desc);
        if (sdev->out_streams[i].buffers)
            kfree(sdev->out_streams[i].buffers);
    }
    kfree(sdev);
}

static struct t2audio_subdevice *t2audio_find_dev_by_dev_id(struct t2audio_device *a, t2audio_device_id_t dev_id)
{
    struct t2audio_subdevice *sdev;
    list_for_each_entry(sdev, &a->subdevice_list, list) {
        if (dev_id == sdev->dev_id)
            return sdev;
    }
    return NULL;
}

static struct t2audio_subdevice *t2audio_find_dev_by_uid(struct t2audio_device *a, const char *uid)
{
    struct t2audio_subdevice *sdev;
    list_for_each_entry(sdev, &a->subdevice_list, list) {
        if (!strcmp(uid, sdev->uid))
            return sdev;
    }
    return NULL;
}

static void t2audio_init_bs_stream(struct t2audio_device *a, struct t2audio_stream *strm,
        struct t2audio_buffer_struct_stream *bs_strm);
static void t2audio_init_bs_stream_host(struct t2audio_device *a, struct t2audio_stream *strm,
        struct t2audio_buffer_struct_stream *bs_strm);

static int t2audio_init_bs(struct t2audio_device *a)
{
    int i, j;
    struct t2audio_buffer_struct_device *dev;
    struct t2audio_subdevice *sdev;
    u32 ver, sig, bs_base;

    ver = ioread32(&a->reg_mem_gpr[0]);
    if (ver < 3) {
        dev_err(a->dev, "t2bce_audio: Bad GPR version (%u)", ver);
        return -EINVAL;
    }
    sig = ioread32(&a->reg_mem_gpr[1]);
    if (sig != T2AUDIO_SIG) {
        dev_err(a->dev, "t2bce_audio: Bad GPR sig (%x)", sig);
        return -EINVAL;
    }
    bs_base = ioread32(&a->reg_mem_gpr[2]);
    a->bs = (struct t2audio_buffer_struct *) ((u8 *) a->reg_mem_bs + bs_base);
    if (a->bs->signature != T2AUDIO_SIG) {
        dev_err(a->dev, "t2bce_audio: Bad BufferStruct sig (%x)", a->bs->signature);
        return -EINVAL;
    }
    pr_debug("t2bce_audio: BufferStruct ver = %i\n", a->bs->version);
    pr_debug("t2bce_audio: Num devices = %i\n", a->bs->num_devices);
    for (i = 0; i < a->bs->num_devices; i++) {
        dev = &a->bs->devices[i];
        pr_debug("t2bce_audio: Device %i %s\n", i, dev->name);

        sdev = t2audio_find_dev_by_uid(a, dev->name);
        if (!sdev) {
            dev_err(a->dev, "t2bce_audio: Subdevice not found for BufferStruct device %s\n", dev->name);
            continue;
        }
        sdev->buf_id = (u8) i;
        dev->num_input_streams = 0;
        for (j = 0; j < dev->num_output_streams; j++) {
            pr_debug("t2bce_audio: Device %i Stream %i: Output; Buffer Count = %i\n", i, j,
                     dev->output_streams[j].num_buffers);
            if (j < sdev->out_stream_cnt)
                t2audio_init_bs_stream(a, &sdev->out_streams[j], &dev->output_streams[j]);
        }
    }

    list_for_each_entry(sdev, &a->subdevice_list, list) {
        if (sdev->buf_id != T2AUDIO_BUFFER_ID_NONE)
            continue;
        sdev->buf_id = i;
        pr_debug("t2bce_audio: Created device %i %s\n", i, sdev->uid);
        strcpy(a->bs->devices[i].name, sdev->uid);
        a->bs->devices[i].num_input_streams = 0;
        a->bs->devices[i].num_output_streams = 0;
        a->bs->num_devices = ++i;
    }
    list_for_each_entry(sdev, &a->subdevice_list, list) {
        if (sdev->in_stream_cnt == 1) {
            pr_debug("t2bce_audio: Device %i Host Stream; Input\n", sdev->buf_id);
            t2audio_init_bs_stream_host(a, &sdev->in_streams[0], &a->bs->devices[sdev->buf_id].input_streams[0]);
            a->bs->devices[sdev->buf_id].num_input_streams = 1;
            wmb();

            if (t2audio_cmd_set_input_stream_address_ranges(a, sdev->dev_id)) {
                dev_err(a->dev, "t2bce_audio: Failed to set input stream address ranges\n");
            }
        }
    }

    return 0;
}

static void t2audio_init_bs_stream(struct t2audio_device *a, struct t2audio_stream *strm,
                                  struct t2audio_buffer_struct_stream *bs_strm)
{
    size_t i;
    strm->buffer_cnt = bs_strm->num_buffers;
    if (bs_strm->num_buffers > T2AUDIO_DEVICE_MAX_BUFFER_COUNT) {
        dev_warn(a->dev, "BufferStruct buffer count %u exceeds driver limit of %u\n", bs_strm->num_buffers,
                T2AUDIO_DEVICE_MAX_BUFFER_COUNT);
        strm->buffer_cnt = T2AUDIO_DEVICE_MAX_BUFFER_COUNT;
    }
    if (!strm->buffer_cnt)
        return;
    strm->buffers = kmalloc_array(strm->buffer_cnt, sizeof(struct t2audio_dma_buf), GFP_KERNEL);
    if (!strm->buffers) {
        dev_err(a->dev, "Buffer list allocation failed\n");
        return;
    }
    for (i = 0; i < strm->buffer_cnt; i++) {
        strm->buffers[i].dma_addr = a->reg_mem_bs_dma + (dma_addr_t) bs_strm->buffers[i].address;
        strm->buffers[i].ptr = a->reg_mem_bs + bs_strm->buffers[i].address;
        strm->buffers[i].size = bs_strm->buffers[i].size;
        strm->buffers[i].type = T2AUDIO_DMA_BUF_IOMEM;
    }

    if (strm->buffer_cnt == 1) {
        strm->alsa_hw_desc = kmalloc(sizeof(struct snd_pcm_hardware), GFP_KERNEL);
        if (t2audio_create_hw_info(&strm->desc, strm->alsa_hw_desc, strm->buffers[0].size)) {
            kfree(strm->alsa_hw_desc);
            strm->alsa_hw_desc = NULL;
        }
    }
}

static void t2audio_init_bs_stream_host(struct t2audio_device *a, struct t2audio_stream *strm,
        struct t2audio_buffer_struct_stream *bs_strm)
{
    size_t size;
    dma_addr_t dma_addr;
    void *dma_ptr;
    size = strm->desc.bytes_per_packet * 16640;
    dma_ptr = dma_alloc_coherent(&a->pci->dev, size, &dma_addr, GFP_KERNEL);
    if (!dma_ptr) {
        dev_err(a->dev, "dma_alloc_coherent failed\n");
        return;
    }
    bs_strm->buffers[0].address = dma_addr;
    bs_strm->buffers[0].size = size;
    bs_strm->num_buffers = 1;

    memset(dma_ptr, 0, size);

    strm->buffer_cnt = 1;
    strm->buffers = kmalloc_array(strm->buffer_cnt, sizeof(struct t2audio_dma_buf), GFP_KERNEL);
    if (!strm->buffers) {
        dev_err(a->dev, "Buffer list allocation failed\n");
        return;
    }
    strm->buffers[0].dma_addr = dma_addr;
    strm->buffers[0].ptr = dma_ptr;
    strm->buffers[0].size = size;
    strm->buffers[0].type = T2AUDIO_DMA_BUF_COHERENT;

    strm->alsa_hw_desc = kmalloc(sizeof(struct snd_pcm_hardware), GFP_KERNEL);
    if (t2audio_create_hw_info(&strm->desc, strm->alsa_hw_desc, strm->buffers[0].size)) {
        kfree(strm->alsa_hw_desc);
        strm->alsa_hw_desc = NULL;
    }
}

static void t2audio_handle_prop_change(struct t2audio_device *a, struct t2audio_msg *msg);

void t2audio_handle_notification(struct t2audio_device *a, struct t2audio_msg *msg)
{
    struct t2audio_send_ctx sctx;
    struct t2audio_msg_base base;
    if (t2audio_msg_read_base(msg, &base))
        return;
    switch (base.msg) {
        case T2AUDIO_MSG_NOTIFICATION_BOOT:
            pr_debug("t2bce_audio: Received boot notification from remote\n");

            /* Resend the alive notify */
            if (t2audio_send(a, &sctx, 500,
                    t2audio_msg_write_alive_notification, 1, 3)) {
                pr_err("Sending alive notification failed\n");
            }
            break;
        case T2AUDIO_MSG_NOTIFICATION_ALIVE:
            pr_debug("t2bce_audio: Received alive notification from remote\n");
            complete_all(&a->remote_alive);
            break;
        case T2AUDIO_MSG_PROPERTY_CHANGED:
            t2audio_handle_prop_change(a, msg);
            break;
        default:
            pr_debug("t2bce_audio: Unhandled notification %i\n", base.msg);
            break;
    }
}

struct t2audio_prop_change_work_struct {
    struct work_struct ws;
    struct t2audio_device *a;
    t2audio_device_id_t dev;
    t2audio_object_id_t obj;
    struct t2audio_prop_addr prop;
};

static void t2audio_handle_jack_connection_change(struct t2audio_subdevice *sdev)
{
    u32 plugged;
    if (!sdev->jack)
        return;
    /* NOTE: Apple made the plug status scoped to the input and output streams. This makes no sense for us, so I just
     * always pick the OUTPUT status. */
    if (t2audio_cmd_get_primitive_property(sdev->a, sdev->dev_id, sdev->dev_id,
            T2AUDIO_PROP(T2AUDIO_PROP_SCOPE_OUTPUT, T2AUDIO_PROP_JACK_PLUGGED, 0), NULL, 0, &plugged, sizeof(plugged))) {
        dev_err(sdev->a->dev, "Failed to get jack enable status\n");
        return;
    }
    pr_debug("t2bce_audio: Jack is now %s\n", plugged ? "plugged" : "unplugged");
    snd_jack_report(sdev->jack, plugged ? sdev->jack->type : 0);
}

void t2audio_handle_prop_change_work(struct work_struct *ws)
{
    struct t2audio_prop_change_work_struct *work = container_of(ws, struct t2audio_prop_change_work_struct, ws);
    struct t2audio_subdevice *sdev;

    sdev = t2audio_find_dev_by_dev_id(work->a, work->dev);
    if (!sdev) {
        dev_err(work->a->dev, "Property notification change: device not found\n");
        goto done;
    }
    pr_debug("t2bce_audio: Property changed for device: %s\n", sdev->uid);

    if (work->prop.scope == T2AUDIO_PROP_SCOPE_OUTPUT && work->prop.selector == T2AUDIO_PROP_JACK_PLUGGED) {
        t2audio_handle_jack_connection_change(sdev);
    }

done:
    kfree(work);
}

void t2audio_handle_prop_change(struct t2audio_device *a, struct t2audio_msg *msg)
{
    /* NOTE: This is a scheduled work because this callback will generally need to query device information and this
     * is not possible when we are in the reply parsing code's context. */
    struct t2audio_prop_change_work_struct *work;
    work = kmalloc(sizeof(struct t2audio_prop_change_work_struct), GFP_KERNEL);
    work->a = a;
    INIT_WORK(&work->ws, t2audio_handle_prop_change_work);
    t2audio_msg_read_property_changed(msg, &work->dev, &work->obj, &work->prop);
    schedule_work(&work->ws);
}

#define t2audio_send_cmd_response(a, sctx, msg, fn, ...) \
    if (t2audio_send_with_tag(a, sctx, ((struct t2audio_msg_header *) msg->data)->tag, 500, fn, ##__VA_ARGS__)) \
        pr_err("t2bce_audio: Failed to reply to a command\n");

void t2audio_handle_cmd_timestamp(struct t2audio_device *a, struct t2audio_msg *msg)
{
    ktime_t time_os = ktime_get_boottime();
    unsigned long flags;
    struct t2audio_send_ctx sctx;
    struct t2audio_subdevice *sdev;
    bool clock_ready;
    s64 clock_offset_ns;
    s64 offset_sample;
    u64 devid, timestamp, update_seed;
    t2audio_msg_read_update_timestamp(msg, &devid, &timestamp, &update_seed);

    offset_sample = ktime_to_ns(time_os) - (s64)timestamp;
    spin_lock_irqsave(&a->clock_lock, flags);
    if (offset_sample >= 0) {
        if (!a->clock_offset_valid || offset_sample < a->clock_offset_ns) {
            a->clock_offset_ns = offset_sample;
            a->clock_offset_valid = true;
        }
        if (a->clock_samples < 2)
            a->clock_samples++;
    }
    clock_offset_ns = a->clock_offset_ns;
    clock_ready = a->clock_samples >= 2;
    spin_unlock_irqrestore(&a->clock_lock, flags);
    pr_debug("t2bce_audio: timestamp dev=%llx t2=%llx host=%lld seed=%llx sample_offset=%lld clock_offset=%lld\n",
            devid, timestamp, ktime_to_ns(time_os), update_seed,
            offset_sample, clock_offset_ns);

    sdev = t2audio_find_dev_by_dev_id(a, devid);
    if (sdev)
        t2audio_handle_timestamp(sdev, timestamp, update_seed, clock_offset_ns,
                clock_ready);

    t2audio_send_cmd_response(a, &sctx, msg,
            t2audio_msg_write_update_timestamp_response);
}

void t2audio_handle_command(struct t2audio_device *a, struct t2audio_msg *msg)
{
    struct t2audio_msg_base base;
    if (t2audio_msg_read_base(msg, &base))
        return;
    switch (base.msg) {
        case T2AUDIO_MSG_UPDATE_TIMESTAMP:
            t2audio_handle_cmd_timestamp(a, msg);
            break;
        default:
            pr_debug("t2bce_audio: Unhandled device command %i\n", base.msg);
            break;
    }
}

static struct pci_device_id t2audio_ids[  ] = {
        { PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1803) },
        { 0, },
};
MODULE_DEVICE_TABLE(pci, t2audio_ids);

struct dev_pm_ops t2audio_pci_driver_pm = {
        .suspend = t2audio_suspend,
        .resume = t2audio_resume
};
struct pci_driver t2audio_pci_driver = {
        .name = "t2bce_audio",
        .id_table = t2audio_ids,
        .probe = t2audio_probe,
        .remove = t2audio_remove,
        .shutdown = t2audio_shutdown,
        .driver = {
                .pm = &t2audio_pci_driver_pm
        }
};


static int __init t2audio_module_init(void)
{
    int result;
    if ((result = alloc_chrdev_region(&t2audio_chrdev, 0, 1, "t2bce_audio")))
        goto fail_chrdev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,4,0)
    t2audio_class = class_create(THIS_MODULE, "t2bce_audio");
#else
    t2audio_class = class_create("t2bce_audio");
#endif
    if (IS_ERR(t2audio_class)) {
        result = PTR_ERR(t2audio_class);
        goto fail_class;
    }

    result = pci_register_driver(&t2audio_pci_driver);
    if (result)
        goto fail_drv;
    pr_info("t2bce_audio: module initialized\n");
    return 0;

fail_drv:
    pci_unregister_driver(&t2audio_pci_driver);
fail_class:
    class_destroy(t2audio_class);
fail_chrdev:
    unregister_chrdev_region(t2audio_chrdev, 1);
    if (!result)
        result = -EINVAL;
    pr_info("t2bce_audio: module init failed status=%d\n", result);
    return result;
}

static void __exit t2audio_module_exit(void)
{
    pci_unregister_driver(&t2audio_pci_driver);
    class_destroy(t2audio_class);
    unregister_chrdev_region(t2audio_chrdev, 1);
    pr_info("t2bce_audio: module exited\n");
}

struct t2audio_alsa_pcm_id_mapping t2audio_alsa_id_mappings[] = {
        {"Speaker", 0},
        {"Digital Mic", 1},
        {"Codec Output", 2},
        {"Codec Input", 3},
        {"Bridge Loopback", 4},
        {}
};

module_param_named(index, t2audio_alsa_index, int, 0444);
MODULE_PARM_DESC(index, "Index value for Apple Internal Audio soundcard.");
module_param_named(id, t2audio_alsa_id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for Apple Internal Audio soundcard.");
MODULE_SOFTDEP("pre: t2bce_core");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("André Eikmeyer <andre.eikmeyer@gmail.com>");
MODULE_DESCRIPTION("Apple T2 Audio Driver");
MODULE_VERSION("0.01");
module_init(t2audio_module_init);
module_exit(t2audio_module_exit);
