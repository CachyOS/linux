#ifndef T2AUDIO_H
#define T2AUDIO_H

#include <linux/types.h>
#include <linux/workqueue.h>
#include <sound/pcm.h>
#include "t2bce_core_transport.h"
#include "protocol_bce.h"
#include "description.h"

#define T2AUDIO_SIG 0x19870423

#define T2AUDIO_DEVICE_MAX_UID_LEN 128
#define T2AUDIO_DEVICE_MAX_INPUT_STREAMS 1
#define T2AUDIO_DEVICE_MAX_OUTPUT_STREAMS 1
#define T2AUDIO_DEVICE_MAX_BUFFER_COUNT 1

#define T2AUDIO_BUFFER_ID_NONE 0xffu

struct snd_card;
struct snd_pcm;
struct snd_pcm_hardware;
struct snd_jack;

struct __attribute__((packed)) __attribute__((aligned(4))) t2audio_buffer_struct_buffer {
    size_t address;
    size_t size;
    size_t pad[4];
};
struct t2audio_buffer_struct_stream {
    u8 num_buffers;
    struct t2audio_buffer_struct_buffer buffers[100];
    char filler[32];
};
struct t2audio_buffer_struct_device {
    char name[128];
    u8 num_input_streams;
    u8 num_output_streams;
    struct t2audio_buffer_struct_stream input_streams[5];
    struct t2audio_buffer_struct_stream output_streams[5];
    char filler[128];
};
struct t2audio_buffer_struct {
    u32 version;
    u32 signature;
    u32 flags;
    u8 num_devices;
    struct t2audio_buffer_struct_device devices[20];
};

struct t2audio_device;

struct t2audio_deferred_msg {
    struct work_struct ws;
    struct t2audio_device *a;
    struct t2audio_msg msg;
};

enum t2audio_dma_buf_type {
    T2AUDIO_DMA_BUF_IOMEM,
    T2AUDIO_DMA_BUF_COHERENT,
};

struct t2audio_dma_buf {
    dma_addr_t dma_addr;
    void *ptr;
    size_t size;
    enum t2audio_dma_buf_type type;
};
struct t2audio_stream {
    t2audio_object_id_t id;
    size_t buffer_cnt;
    struct t2audio_dma_buf *buffers;

    struct t2audio_apple_description desc;
    struct snd_pcm_hardware *alsa_hw_desc;
    u32 latency;

    bool waiting_for_first_ts;

    ktime_t remote_timestamp;
    ktime_t timestamp_accept_after;
    s64 clock_offset_ns;
    u64 timestamp_seed;
    bool clock_offset_valid;
    snd_pcm_sframes_t frame_min;
    int started;
};
struct t2audio_subdevice {
    struct t2audio_device *a;
    struct list_head list;
    t2audio_device_id_t dev_id;
    u32 in_latency, out_latency;
    u8 buf_id;
    int alsa_id;
    char uid[T2AUDIO_DEVICE_MAX_UID_LEN + 1];
    size_t in_stream_cnt;
    struct t2audio_stream in_streams[T2AUDIO_DEVICE_MAX_INPUT_STREAMS];
    size_t out_stream_cnt;
    struct t2audio_stream out_streams[T2AUDIO_DEVICE_MAX_OUTPUT_STREAMS];
    bool is_pcm;
    struct snd_pcm *pcm;
    struct snd_jack *jack;
};
struct t2audio_alsa_pcm_id_mapping {
    const char *name;
    int alsa_id;
};

struct t2audio_device {
    struct pci_dev *pci;
    dev_t devt;
    struct device *dev;
    void __iomem *reg_mem_bs;
    dma_addr_t reg_mem_bs_dma;
    void __iomem *reg_mem_cfg;

    u32 __iomem *reg_mem_gpr;

    struct t2audio_buffer_struct *bs;

    struct t2bce_core_client *bce;
    struct t2audio_bce bcem;

    struct snd_card *card;

    struct list_head subdevice_list;
    int next_alsa_id;

    struct completion remote_alive;
    struct work_struct resume_work;
    bool resume_deferred;
    bool pm_quiesced;

    spinlock_t clock_lock;
    s64 clock_offset_ns;
    u32 clock_samples;
    bool clock_offset_valid;
};

void t2audio_handle_notification(struct t2audio_device *a, struct t2audio_msg *msg);
void t2audio_handle_prop_change_work(struct work_struct *ws);
void t2audio_handle_cmd_timestamp(struct t2audio_device *a, struct t2audio_msg *msg);
void t2audio_handle_command(struct t2audio_device *a, struct t2audio_msg *msg);

extern struct t2audio_alsa_pcm_id_mapping t2audio_alsa_id_mappings[];

#endif //T2AUDIO_H
