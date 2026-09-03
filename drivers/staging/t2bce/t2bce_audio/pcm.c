#include "pcm.h"
#include "audio.h"
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ktime.h>

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

static void t2audio_dma_copy_from(struct t2audio_dma_buf *buf, void *dst, size_t offset, size_t size)
{
    if (!buf || offset >= buf->size)
        return;

    size = min(size, buf->size - offset);
    switch (buf->type) {
        case T2AUDIO_DMA_BUF_IOMEM:
            memcpy_fromio(dst, (u8 __iomem *) buf->ptr + offset, size);
            break;
        case T2AUDIO_DMA_BUF_COHERENT:
            memcpy(dst, (u8 *) buf->ptr + offset, size);
            break;
    }
}

static void t2audio_dma_copy_to(struct t2audio_dma_buf *buf, size_t offset, const void *src, size_t size)
{
    if (!buf || offset >= buf->size)
        return;

    size = min(size, buf->size - offset);
    switch (buf->type) {
        case T2AUDIO_DMA_BUF_IOMEM:
            memcpy_toio((u8 __iomem *) buf->ptr + offset, src, size);
            break;
        case T2AUDIO_DMA_BUF_COHERENT:
            memcpy((u8 *) buf->ptr + offset, src, size);
            break;
    }
}

static void t2audio_pcm_zero_frames(struct snd_pcm_substream *substream,
        snd_pcm_uframes_t first, snd_pcm_uframes_t frames)
{
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    struct t2audio_dma_buf *buf = t2audio_pcm_dma_buf(stream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    size_t offset;
    size_t size;

    if (!buf || !runtime || !runtime->buffer_size || !frames)
        return;

    first %= runtime->buffer_size;
    if (frames > runtime->buffer_size)
        frames = runtime->buffer_size;

    offset = frames_to_bytes(runtime, first);
    if (first + frames <= runtime->buffer_size) {
        size = frames_to_bytes(runtime, frames);
        t2audio_dma_memset(buf, offset, 0, size);
        return;
    }

    size = frames_to_bytes(runtime, runtime->buffer_size - first);
    t2audio_dma_memset(buf, offset, 0, size);
    t2audio_dma_memset(buf, 0, 0, frames_to_bytes(runtime, frames - (runtime->buffer_size - first)));
}

static int t2audio_pcm_open(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);

    pr_debug("t2bce_audio: pcm open dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    substream->runtime->hw = *t2audio_pcm_stream(substream)->alsa_hw_desc;

    return 0;
}

static int t2audio_pcm_close(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);

    pr_debug("t2bce_audio: pcm close dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    return 0;
}

static int t2audio_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);

    stream->waiting_for_first_ts = true;
    stream->remote_timestamp = 0;
    stream->timestamp_accept_after = 0;
    stream->clock_offset_ns = 0;
    stream->timestamp_seed = 0;
    stream->clock_offset_valid = false;
    stream->frame_min = stream->latency;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && substream->runtime->buffer_size)
        t2audio_pcm_zero_frames(substream, 0, substream->runtime->buffer_size);

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

    substream->runtime->dma_area = astream->buffers[0].ptr;
    substream->runtime->dma_addr = astream->buffers[0].dma_addr;
    substream->runtime->dma_bytes = astream->buffers[0].size;
    return 0;
}

static int t2audio_pcm_hw_free(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);

    pr_debug("t2bce_audio: pcm hw_free dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    return 0;
}

static int t2audio_pcm_start(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    struct t2audio_dma_buf *dmabuf = t2audio_pcm_dma_buf(stream);
    void *buf = NULL;
    size_t s = 0;
    ktime_t time_start, time_end;
    bool back_buffer;
    int status;

    time_start = ktime_get();
    smp_store_release(&stream->started, 0);

    back_buffer = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);

    if (back_buffer) {
        snd_pcm_uframes_t appl_ptr;

        if (!dmabuf)
            return -EINVAL;
        if (!substream->runtime->buffer_size)
            return -EINVAL;

        appl_ptr = substream->runtime->control->appl_ptr % substream->runtime->buffer_size;
        s = frames_to_bytes(substream->runtime, appl_ptr);
        if (s) {
            buf = kmalloc(s, GFP_KERNEL);
            if (!buf)
                return -ENOMEM;

            t2audio_dma_copy_from(dmabuf, buf, 0, s);
            time_end = ktime_get();
            pr_debug("t2bce_audio: Backed up the buffer in %lluns [%li]\n", ktime_to_ns(time_end - time_start),
                    appl_ptr);
        }
    }

    status = t2audio_cmd_start_io(sdev->a, sdev->dev_id);
    if (back_buffer && buf)
        t2audio_dma_copy_to(dmabuf, 0, buf, s);
    kfree(buf);

    if (status)
        return status;

    /* Timestamps received while START_IO was pending belong to the old epoch. */
    stream->remote_timestamp = 0;
    stream->waiting_for_first_ts = true;
    stream->timestamp_accept_after = ktime_get_boottime();
    stream->clock_offset_ns = 0;
    stream->timestamp_seed = 0;
    stream->clock_offset_valid = false;
    stream->frame_min = stream->latency;
    smp_store_release(&stream->started, 1);

    time_end = ktime_get();
    pr_debug("t2bce_audio: start_io %s %lld us\n",
            sdev->uid, ktime_to_us(ktime_sub(time_end, time_start)));
    return 0;
}

static int t2audio_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    int err;

    /* bridgeOS exposes one ALSA substream per remote stream. */
    if (substream->number != 0)
        return 0;
    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
            pr_debug("t2bce_audio: TRIGGER START %s\n", sdev->uid);
            err = t2audio_pcm_start(substream);
            if (err)
                return err;
            break;
        case SNDRV_PCM_TRIGGER_STOP:
            pr_debug("t2bce_audio: TRIGGER STOP %s\n", sdev->uid);
            smp_store_release(&stream->started, 0);
            err = t2audio_cmd_stop_io(sdev->a, sdev->dev_id);
            stream->remote_timestamp = 0;
            stream->waiting_for_first_ts = true;
            stream->timestamp_accept_after = 0;
            stream->clock_offset_ns = 0;
            stream->timestamp_seed = 0;
            stream->clock_offset_valid = false;
            if (err)
                return err;
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static snd_pcm_uframes_t t2audio_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    ktime_t time_from_start;
    snd_pcm_sframes_t frames;
    snd_pcm_sframes_t buffer_time_length;

    if (!smp_load_acquire(&stream->started) || stream->waiting_for_first_ts)
        return 0;

    /* bridgeOS reports coarse timestamps; interpolate the ALSA pointer locally. */
    time_from_start = ktime_get_boottime() - stream->remote_timestamp;
    if (ktime_to_ns(time_from_start) < 0)
        return 0;
    buffer_time_length = NSEC_PER_SEC * substream->runtime->buffer_size / substream->runtime->rate;
    frames = (ktime_to_ns(time_from_start) % buffer_time_length) * substream->runtime->buffer_size / buffer_time_length;
    if (ktime_to_ns(time_from_start) < buffer_time_length) {
        if (frames < stream->frame_min)
            frames = stream->frame_min;
        else
            stream->frame_min = 0;
    } else {
        if (ktime_to_ns(time_from_start) < 2 * buffer_time_length)
            stream->frame_min = frames;
        else
            stream->frame_min = 0;
    }
    frames -= stream->latency;
    if (frames < 0)
        frames += ((-frames - 1) / substream->runtime->buffer_size + 1) * substream->runtime->buffer_size;
    frames %= substream->runtime->buffer_size;
    return (snd_pcm_uframes_t) frames;
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
            return snd_pcm_lib_mmap_iomem(substream, area);
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
    return 0;
}

static void t2audio_handle_stream_timestamp(struct snd_pcm_substream *substream,
                                            u64 dev_timestamp, u64 update_seed,
                                            s64 clock_offset_ns, bool clock_ready)
{
    unsigned long flags;
    ktime_t event_timestamp;
    struct t2audio_stream *stream;
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);

    stream = t2audio_pcm_stream(substream);
    snd_pcm_stream_lock_irqsave(substream, flags);
    if (!smp_load_acquire(&stream->started)) {
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    if (!clock_ready) {
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    if (!stream->clock_offset_valid || stream->timestamp_seed != update_seed) {
        stream->clock_offset_ns = clock_offset_ns;
        stream->timestamp_seed = update_seed;
        stream->clock_offset_valid = true;
    }
    event_timestamp = ns_to_ktime((s64)dev_timestamp + stream->clock_offset_ns);
    if (ktime_before(event_timestamp, stream->timestamp_accept_after)) {
        pr_debug("t2bce_audio: ignoring pre-start timestamp dev=%s event=%lld accept_after=%lld\n",
                sdev->uid, ktime_to_ns(event_timestamp),
                ktime_to_ns(stream->timestamp_accept_after));
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    stream->remote_timestamp = event_timestamp;
    if (stream->waiting_for_first_ts) {
        stream->waiting_for_first_ts = false;
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    snd_pcm_stream_unlock_irqrestore(substream, flags);
    if (!substream->runtime->no_period_wakeup)
        snd_pcm_period_elapsed(substream);
}

void t2audio_handle_timestamp(struct t2audio_subdevice *sdev, u64 dev_timestamp,
        u64 update_seed, s64 clock_offset_ns, bool clock_ready)
{
    struct snd_pcm_substream *substream;

    substream = sdev->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
    if (substream)
        t2audio_handle_stream_timestamp(substream, dev_timestamp, update_seed,
                clock_offset_ns, clock_ready);
    substream = sdev->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
    if (substream)
        t2audio_handle_stream_timestamp(substream, dev_timestamp, update_seed,
                clock_offset_ns, clock_ready);
}
