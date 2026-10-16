#include "pcm.h"
#include "audio.h"
#include "timing.h"
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mm.h>

static u64 t2audio_get_alsa_fmtbit(struct t2audio_apple_description *desc)
{
    if (desc->format_flags & T2AUDIO_FORMAT_FLAG_FLOAT) {
        if (desc->bits_per_channel == 32) {
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_BIG_ENDIAN)
                return SNDRV_PCM_FMTBIT_FLOAT_BE;
            else
                return SNDRV_PCM_FMTBIT_FLOAT_LE;
        } else if (desc->bits_per_channel == 64) {
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_BIG_ENDIAN)
                return SNDRV_PCM_FMTBIT_FLOAT64_BE;
            else
                return SNDRV_PCM_FMTBIT_FLOAT64_LE;
        } else {
            pr_err("t2bce_audio: unsupported bits per channel for float format: %u\n", desc->bits_per_channel);
            return 0;
        }
    }
#define DEFINE_BPC_OPTION(val, b) \
    case val: \
        if (desc->format_flags & T2AUDIO_FORMAT_FLAG_BIG_ENDIAN) { \
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_SIGNED) \
                return SNDRV_PCM_FMTBIT_S ## b ## BE; \
            else \
                return SNDRV_PCM_FMTBIT_U ## b ## BE; \
        } else { \
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_SIGNED) \
                return SNDRV_PCM_FMTBIT_S ## b ## LE; \
            else \
                return SNDRV_PCM_FMTBIT_U ## b ## LE; \
        }
    if (desc->format_flags & T2AUDIO_FORMAT_FLAG_PACKED) {
        switch (desc->bits_per_channel) {
            case 8:
            case 16:
            case 32:
                break;
            DEFINE_BPC_OPTION(24, 24_3)
            default:
                pr_err("t2bce_audio: unsupported bits per channel for packed format: %u\n", desc->bits_per_channel);
                return 0;
        }
    }
    if (desc->format_flags & T2AUDIO_FORMAT_FLAG_ALIGNED_HIGH) {
        switch (desc->bits_per_channel) {
            DEFINE_BPC_OPTION(24, 32_)
            default:
                pr_err("t2bce_audio: unsupported bits per channel for high-aligned format: %u\n", desc->bits_per_channel);
                return 0;
        }
    }
    switch (desc->bits_per_channel) {
        case 8:
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_SIGNED)
                return SNDRV_PCM_FMTBIT_S8;
            else
                return SNDRV_PCM_FMTBIT_U8;
        DEFINE_BPC_OPTION(16, 16_)
        DEFINE_BPC_OPTION(24, 24_)
        DEFINE_BPC_OPTION(32, 32_)
        default:
            pr_err("t2bce_audio: unsupported bits per channel: %u\n", desc->bits_per_channel);
            return 0;
    }
}
int t2audio_create_hw_info(struct t2audio_apple_description *desc, struct snd_pcm_hardware *alsa_hw,
        size_t buf_size)
{
    uint rate;
    alsa_hw->info = (SNDRV_PCM_INFO_MMAP |
                     SNDRV_PCM_INFO_BLOCK_TRANSFER |
                     SNDRV_PCM_INFO_MMAP_VALID |
                     SNDRV_PCM_INFO_NO_PERIOD_WAKEUP |
                     SNDRV_PCM_INFO_DOUBLE);
    if (desc->format_flags & T2AUDIO_FORMAT_FLAG_NON_MIXABLE)
        pr_warn("t2bce_audio: unsupported hw flag: NON_MIXABLE\n");
    if (!(desc->format_flags & T2AUDIO_FORMAT_FLAG_NON_INTERLEAVED))
        alsa_hw->info |= SNDRV_PCM_INFO_INTERLEAVED;
    alsa_hw->formats = t2audio_get_alsa_fmtbit(desc);
    if (!alsa_hw->formats)
        return -EINVAL;
    rate = (uint) t2audio_double_to_u64(desc->sample_rate_double);
    alsa_hw->rates = snd_pcm_rate_to_rate_bit(rate);
    alsa_hw->rate_min = rate;
    alsa_hw->rate_max = rate;
    alsa_hw->channels_min = desc->channels_per_frame;
    alsa_hw->channels_max = desc->channels_per_frame;
    alsa_hw->buffer_bytes_max = buf_size;
    alsa_hw->period_bytes_min = desc->bytes_per_packet;
    alsa_hw->period_bytes_max = desc->bytes_per_packet;
    alsa_hw->periods_min = (uint) (buf_size / desc->bytes_per_packet);
    alsa_hw->periods_max = (uint) (buf_size / desc->bytes_per_packet);
    pr_debug("t2audio_create_hw_info: format = %llu, rate = %u/%u. channels = %u, periods = %u, period size = %lu\n",
            alsa_hw->formats, alsa_hw->rate_min, alsa_hw->rates, alsa_hw->channels_min, alsa_hw->periods_min,
            alsa_hw->period_bytes_min);
    return 0;
}

static struct t2audio_stream *t2audio_pcm_stream(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
        return &sdev->out_streams[substream->number];
    else
        return &sdev->in_streams[substream->number];
}

static struct t2audio_dma_buf *t2audio_pcm_dma_buf(struct t2audio_stream *stream)
{
    if (!stream->buffer_cnt || !stream->buffers)
        return NULL;

    return &stream->buffers[0];
}

enum t2audio_remote_io_state {
    T2AUDIO_REMOTE_IO_UNKNOWN = -1,
    T2AUDIO_REMOTE_IO_STOPPED,
    T2AUDIO_REMOTE_IO_STARTED,
};

static void t2audio_dma_memset(struct t2audio_dma_buf *buf, size_t offset, int value, size_t size)
{
    if (!buf || offset >= buf->size)
        return;

    size = min(size, buf->size - offset);
    switch (buf->type) {
        case T2AUDIO_DMA_BUF_IOMEM:
            memset_io((u8 __iomem *) buf->ptr + offset, value, size);
            break;
        case T2AUDIO_DMA_BUF_COHERENT:
            memset((u8 *) buf->ptr + offset, value, size);
            break;
    }
}

void t2audio_pcm_quiesce_stream(struct t2audio_stream *stream)
{
    smp_store_release(&stream->started, 0);
}

void t2audio_pcm_reset_timing(struct t2audio_stream *stream)
{
    stream->waiting_for_first_ts = true;
    stream->remote_timestamp = 0;
    stream->timestamp_accept_after = 0;
    stream->last_device_timestamp = 0;
    stream->last_sample_frames = 0;
    stream->sample_frames = 0;
    stream->reported_frames = 0;
    stream->timeline_fault = false;
}

static int t2audio_pcm_open(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    pr_debug("t2bce_audio: pcm open dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    if (!t2audio_pcm_stream(substream)->alsa_hw_desc)
        return -ENODEV;
    substream->runtime->hw = *t2audio_pcm_stream(substream)->alsa_hw_desc;

    return 0;
}

static int t2audio_pcm_close(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    pr_debug("t2bce_audio: pcm close dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    flush_work(&stream->io_work);
    return 0;
}

static int t2audio_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);

    /* A previous asynchronous STOP must finish before the ring is cleared. */
    flush_work(&stream->io_work);
    t2audio_pcm_reset_timing(stream);

    /* Neither direction may expose samples left over from the previous run. */
    {
        struct t2audio_dma_buf *bridge = t2audio_pcm_dma_buf(stream);

        if (bridge)
            t2audio_dma_memset(bridge, 0, 0, bridge->size);
    }

    return 0;
}

static int t2audio_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *astream = t2audio_pcm_stream(substream);

    pr_debug("t2bce_audio: pcm hw_params dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);

    if (!astream->buffer_cnt || !astream->buffers)
        return -EINVAL;

    if (params_buffer_bytes(hw_params) != astream->buffers[0].size)
        return -EINVAL;

    /* Match BridgeAudioController: expose the bridgeOS ring itself. */
    substream->runtime->dma_area = astream->buffers[0].ptr;
    substream->runtime->dma_addr = astream->buffers[0].dma_addr;
    substream->runtime->dma_bytes = astream->buffers[0].size;
    return 0;
}

static int t2audio_pcm_hw_free(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    pr_debug("t2bce_audio: pcm hw_free dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    flush_work(&stream->io_work);
    return 0;
}

static int t2audio_pcm_start_sync(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    ktime_t time_start, time_end;
    int status;

    time_start = ktime_get();
    smp_store_release(&stream->started, 0);

    if (!substream->runtime->buffer_size || !t2audio_pcm_dma_buf(stream))
        return -EINVAL;

    t2audio_pcm_reset_timing(stream);
    /* Accept notifications received during START_IO, including sample zero. */
    stream->timestamp_accept_after = ktime_get();

    /* Publish userspace's prefill before starting the remote consumer. */
    wmb();

    /*
     * START_IO can emit the reset timestamp before its response.  Publish the
     * stream first so deferred command handling cannot discard that anchor.
     */
    smp_store_release(&stream->started, 1);
    status = t2audio_cmd_start_io(sdev->a, sdev->dev_id);
    if (status) {
        smp_store_release(&stream->started, 0);
        return status;
    }

    time_end = ktime_get();
    pr_debug("t2bce_audio: start_io %s %lld us\n",
            sdev->uid, ktime_to_us(ktime_sub(time_end, time_start)));
    return 0;
}

static int t2audio_pcm_stop_sync(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    int status;

    t2audio_pcm_quiesce_stream(stream);
    status = t2audio_cmd_stop_io(sdev->a, sdev->dev_id);
    stream->remote_timestamp = 0;
    stream->waiting_for_first_ts = true;
    stream->timestamp_accept_after = 0;
    return status;
}

static void t2audio_pcm_io_work(struct work_struct *work)
{
    struct t2audio_stream *stream = container_of(work, struct t2audio_stream,
                                                  io_work);
    struct t2audio_subdevice *sdev = stream->sdev;
    unsigned long flags;

    for (;;) {
        unsigned int generation;
        bool requested;
        int target_state;
        int status;

        spin_lock_irqsave(&stream->io_lock, flags);
        generation = stream->io_generation;
        requested = stream->io_requested;
        target_state = requested ? T2AUDIO_REMOTE_IO_STARTED :
                                   T2AUDIO_REMOTE_IO_STOPPED;
        if (stream->remote_io_state == target_state) {
            spin_unlock_irqrestore(&stream->io_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&stream->io_lock, flags);

        status = requested ? t2audio_pcm_start_sync(stream->substream) :
                             t2audio_pcm_stop_sync(stream->substream);

        spin_lock_irqsave(&stream->io_lock, flags);
        stream->remote_io_state = status ? T2AUDIO_REMOTE_IO_UNKNOWN :
                                           target_state;
        if (status || generation == stream->io_generation) {
            spin_unlock_irqrestore(&stream->io_lock, flags);
            if (status)
                dev_err(sdev->a->dev, "%s_IO failed for %s: %d\n",
                        requested ? "START" : "STOP", sdev->uid, status);
            return;
        }
        spin_unlock_irqrestore(&stream->io_lock, flags);
    }
}

static void t2audio_pcm_request_io(struct t2audio_stream *stream, bool start)
{
    unsigned long flags;

    if (!start)
        t2audio_pcm_quiesce_stream(stream);

    spin_lock_irqsave(&stream->io_lock, flags);
    stream->io_requested = start;
    stream->io_generation++;
    spin_unlock_irqrestore(&stream->io_lock, flags);
    queue_work(stream->sdev->a->pcm_io_wq, &stream->io_work);
}

static int t2audio_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);

    /* bridgeOS exposes one ALSA substream per remote stream. */
    if (substream->number != 0)
        return 0;
    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
            pr_debug("t2bce_audio: TRIGGER START %s\n", sdev->uid);
            if (!substream->runtime->buffer_size || !t2audio_pcm_dma_buf(stream))
                return -EINVAL;
            t2audio_pcm_request_io(stream, true);
            break;
        case SNDRV_PCM_TRIGGER_STOP:
            pr_debug("t2bce_audio: TRIGGER STOP %s\n", sdev->uid);
            t2audio_pcm_request_io(stream, false);
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static void t2audio_pcm_init_stream_work(struct t2audio_subdevice *sdev,
        struct t2audio_stream *stream, struct snd_pcm_substream *substream)
{
    stream->sdev = sdev;
    stream->substream = substream;
    spin_lock_init(&stream->io_lock);
    INIT_WORK(&stream->io_work, t2audio_pcm_io_work);
    stream->remote_io_state = T2AUDIO_REMOTE_IO_STOPPED;
}

static snd_pcm_uframes_t t2audio_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    s64 elapsed_ns;
    u64 elapsed_frames;
    u64 latency = substream->stream == SNDRV_PCM_STREAM_CAPTURE ?
            stream->latency : 0;

    if (!smp_load_acquire(&stream->started))
        return 0;

    if (stream->timeline_fault)
        return SNDRV_PCM_POS_XRUN;
    if (stream->waiting_for_first_ts)
        return 0;

    /*
     * bridgeOS converts the hardware timestamp with mach_bridge_remote_time(),
     * using the host clock supplied through BCE. It is therefore already in
     * Linux monotonic nanoseconds. sampleTime advances by bridgeaudiod's fixed
     * timestampPeriod, so callback spacing must not be used as a sample-rate
     * estimate: interpolate between anchors at the negotiated stream rate.
     */
    elapsed_ns = ktime_to_ns(ktime_sub(ktime_get(), stream->remote_timestamp));
    if (elapsed_ns <= 0)
        elapsed_frames = 0;
    else
        elapsed_frames = mul_u64_u32_div(elapsed_ns,
                substream->runtime->rate, NSEC_PER_SEC);
    stream->reported_frames = t2audio_position_frames(stream->sample_frames,
            elapsed_frames, latency, stream->reported_frames);
    substream->runtime->delay = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
            stream->latency : 0;
    return stream->reported_frames % substream->runtime->buffer_size;
}

static int t2audio_pcm_mmap(struct snd_pcm_substream *substream, struct vm_area_struct *area)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    struct t2audio_dma_buf *buf;

    if (!stream->buffer_cnt || !stream->buffers)
        return -EINVAL;

    buf = &stream->buffers[0];
    switch (buf->type) {
        case T2AUDIO_DMA_BUF_IOMEM:
            if (sdev->a->bs_wc)
                area->vm_page_prot = pgprot_writecombine(area->vm_page_prot);
            else
                area->vm_page_prot = pgprot_noncached(area->vm_page_prot);
            return vm_iomap_memory(area, buf->dma_addr, buf->size);
        case T2AUDIO_DMA_BUF_COHERENT:
            return dma_mmap_coherent(sdev->a->dev, area, buf->ptr, buf->dma_addr, buf->size);
        default:
            return -EINVAL;
    }
}

static struct snd_pcm_ops t2audio_pcm_ops = {
        .open =        t2audio_pcm_open,
        .close =       t2audio_pcm_close,
        .ioctl =       snd_pcm_lib_ioctl,
        .hw_params =   t2audio_pcm_hw_params,
        .hw_free =     t2audio_pcm_hw_free,
        .prepare =     t2audio_pcm_prepare,
        .trigger =     t2audio_pcm_trigger,
        .pointer =     t2audio_pcm_pointer,
        .mmap    =     t2audio_pcm_mmap
};

int t2audio_create_pcm(struct t2audio_subdevice *sdev)
{
    struct snd_pcm *pcm;
    struct t2audio_alsa_pcm_id_mapping *id_mapping;
    int err;

    if (!sdev->is_pcm || (sdev->in_stream_cnt == 0 && sdev->out_stream_cnt == 0)) {
        return -EINVAL;
    }

    for (id_mapping = t2audio_alsa_id_mappings; id_mapping->name; id_mapping++) {
        if (!strcmp(sdev->uid, id_mapping->name)) {
            sdev->alsa_id = id_mapping->alsa_id;
            break;
        }
    }
    if (!id_mapping->name)
        sdev->alsa_id = sdev->a->next_alsa_id++;
    err = snd_pcm_new(sdev->a->card, sdev->uid, sdev->alsa_id,
            (int) sdev->out_stream_cnt, (int) sdev->in_stream_cnt, &pcm);
    if (err < 0)
        return err;
    pcm->private_data = sdev;
    pcm->nonatomic = 1;
    sdev->pcm = pcm;
    strcpy(pcm->name, sdev->uid);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &t2audio_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &t2audio_pcm_ops);
    if (sdev->out_stream_cnt)
        t2audio_pcm_init_stream_work(sdev, &sdev->out_streams[0],
                pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream);
    if (sdev->in_stream_cnt)
        t2audio_pcm_init_stream_work(sdev, &sdev->in_streams[0],
                pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream);
    return 0;
}

int t2audio_pcm_quiesce_subdevice(struct t2audio_subdevice *sdev)
{
    struct t2audio_stream *streams[2];
    unsigned long flags;
    bool need_stop = false;
    size_t count = 0;
    size_t i;
    int status = 0;

    if (sdev->out_stream_cnt && sdev->out_streams[0].substream)
        streams[count++] = &sdev->out_streams[0];
    if (sdev->in_stream_cnt && sdev->in_streams[0].substream)
        streams[count++] = &sdev->in_streams[0];

    for (i = 0; i < count; i++)
        t2audio_pcm_request_io(streams[i], false);
    for (i = 0; i < count; i++)
        flush_work(&streams[i]->io_work);

    for (i = 0; i < count; i++) {
        spin_lock_irqsave(&streams[i]->io_lock, flags);
        if (streams[i]->remote_io_state != T2AUDIO_REMOTE_IO_STOPPED)
            need_stop = true;
        spin_unlock_irqrestore(&streams[i]->io_lock, flags);
    }

    if (need_stop)
        status = t2audio_cmd_stop_io(sdev->a, sdev->dev_id);

    for (i = 0; i < count; i++) {
        spin_lock_irqsave(&streams[i]->io_lock, flags);
        streams[i]->remote_io_state = status ? T2AUDIO_REMOTE_IO_UNKNOWN :
                                               T2AUDIO_REMOTE_IO_STOPPED;
        spin_unlock_irqrestore(&streams[i]->io_lock, flags);
    }
    return status;
}

void t2audio_pcm_cleanup_subdevice(struct t2audio_subdevice *sdev)
{
    if (sdev->out_stream_cnt && sdev->out_streams[0].substream)
        cancel_work_sync(&sdev->out_streams[0].io_work);
    if (sdev->in_stream_cnt && sdev->in_streams[0].substream)
        cancel_work_sync(&sdev->in_streams[0].io_work);
}

static void t2audio_handle_stream_timestamp(struct snd_pcm_substream *substream,
        u64 device_timestamp, u64 update_seed, u64 sample_frames)
{
    unsigned long flags;
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    u64 anchor_frames = sample_frames;
    u64 elapsed_frames;
    u64 elapsed_ns;

    snd_pcm_stream_lock_irqsave(substream, flags);
    if (!smp_load_acquire(&stream->started) || !substream->runtime ||
            device_timestamp < ktime_to_ns(stream->timestamp_accept_after))
        goto out;

    /* Deferred work can run out of order. Never apply an older anchor. */
    if (!stream->waiting_for_first_ts &&
            device_timestamp <= stream->last_device_timestamp)
        goto out;

    /*
     * bridgeaudiod sends seed=1 with sampleTime=0 at a reset, seed=0 for
     * continuation. A running timeline cannot silently change ring origin.
     */
    if ((update_seed && sample_frames) ||
            (!stream->waiting_for_first_ts &&
             ((update_seed && (stream->reported_frames || stream->sample_frames)) ||
              sample_frames < stream->last_sample_frames ||
              device_timestamp < stream->last_device_timestamp))) {
        stream->timeline_fault = true;
        pr_warn_ratelimited("t2bce_audio: timeline reset dev=%s sample=%llu previous=%llu seed=%llu\n",
                sdev->uid, sample_frames, stream->last_sample_frames, update_seed);
        snd_pcm_period_elapsed_under_stream_lock(substream);
        goto out;
    }

    /*
     * bridgeaudiod increments sampleTime by one fixed timestampPeriod per
     * callback. A delayed or missed callback therefore leaves sampleTime
     * behind the continuously running DMA clock. Do not replace the current
     * interpolation anchor with that older position: doing so freezes the
     * monotonic ALSA pointer until sampleTime catches up and triggers XRUN
     * recovery. Carry forward the frames elapsed from the previous hardware
     * timestamp instead. A genuine reset is handled above and START_IO clears
     * the timing state before the first new anchor.
     */
    if (!stream->waiting_for_first_ts &&
            device_timestamp > stream->last_device_timestamp) {
        elapsed_ns = device_timestamp - stream->last_device_timestamp;
        elapsed_frames = mul_u64_u32_div(elapsed_ns,
                substream->runtime->rate, NSEC_PER_SEC);
        anchor_frames = t2audio_reanchor_frames(anchor_frames,
                stream->sample_frames, elapsed_frames);
    }

    stream->last_sample_frames = sample_frames;
    stream->sample_frames = anchor_frames;
    stream->last_device_timestamp = device_timestamp;
    stream->remote_timestamp = ns_to_ktime(device_timestamp);
    stream->waiting_for_first_ts = false;
    pr_debug("t2bce_audio: anchor dev=%s direction=%d sample=%llu reported=%llu host=%llu seed=%llu\n",
            sdev->uid, substream->stream, anchor_frames, stream->reported_frames,
            device_timestamp, update_seed);

    if (!substream->runtime->no_period_wakeup)
        snd_pcm_period_elapsed_under_stream_lock(substream);
out:
    snd_pcm_stream_unlock_irqrestore(substream, flags);
}

void t2audio_handle_timestamp(struct t2audio_subdevice *sdev,
        u64 device_timestamp, u64 update_seed, u64 sample_frames)
{
    struct snd_pcm_substream *substream;

    if (!sdev->pcm)
        return;
    substream = sdev->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
    if (substream)
        t2audio_handle_stream_timestamp(substream, device_timestamp,
                update_seed, sample_frames);
    substream = sdev->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
    if (substream)
        t2audio_handle_stream_timestamp(substream, device_timestamp,
                update_seed, sample_frames);
}
