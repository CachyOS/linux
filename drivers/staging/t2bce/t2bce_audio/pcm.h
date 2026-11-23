#ifndef T2AUDIO_PCM_H
#define T2AUDIO_PCM_H

#include <linux/types.h>
#include <linux/ktime.h>

struct t2audio_subdevice;
struct t2audio_stream;
struct t2audio_apple_description;
struct snd_pcm_hardware;

int t2audio_create_hw_info(struct t2audio_apple_description *desc, struct snd_pcm_hardware *alsa_hw, size_t buf_size);
int t2audio_create_pcm(struct t2audio_subdevice *sdev);
void t2audio_pcm_quiesce_stream(struct t2audio_stream *stream);
void t2audio_pcm_reset_timing(struct t2audio_stream *stream);
int t2audio_pcm_quiesce_subdevice(struct t2audio_subdevice *sdev);
void t2audio_pcm_cleanup_subdevice(struct t2audio_subdevice *sdev);

void t2audio_handle_timestamp(struct t2audio_subdevice *sdev,
        u64 device_timestamp, u64 update_seed, u64 sample_frames);

#endif //T2AUDIO_PCM_H
