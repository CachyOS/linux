#ifndef T2AUDIO_PROTOCOL_BCE_H
#define T2AUDIO_PROTOCOL_BCE_H

#include "protocol.h"
#include "t2bce_core_transport.h"

#define T2AUDIO_BCE_QUEUE_ELEMENT_SIZE 0x1000
#define T2AUDIO_BCE_QUEUE_ELEMENT_COUNT 20

#define T2AUDIO_BCE_QUEUE_TAG_COUNT 1000

struct t2audio_device;

struct t2audio_bce_queue_entry {
    struct t2audio_msg *msg;
    struct completion *cmpl;
};
struct t2audio_bce_queue {
    struct t2bce_core_queue_cq *cq;
    struct t2bce_core_queue_sq *sq;
    void *data;
    dma_addr_t dma_addr;
    size_t data_head, data_tail;
    size_t el_size, el_count;
};
struct t2audio_bce {
    struct t2bce_core_queue_cq *cq;
    struct t2audio_bce_queue qin;
    struct t2audio_bce_queue qout;
    int tag_num;
    struct t2audio_bce_queue_entry *pending_entries[T2AUDIO_BCE_QUEUE_TAG_COUNT];
    struct spinlock spinlock;
};

struct t2audio_send_ctx {
    int status;
    int tag_n;
    unsigned long irq_flags;
    struct t2audio_msg msg;
    unsigned long timeout;
};

int t2audio_bce_init(struct t2audio_device *dev);
int __t2audio_send_prepare(struct t2audio_bce *b, struct t2audio_send_ctx *ctx, char *tag);
void __t2audio_send(struct t2audio_bce *b, struct t2audio_send_ctx *ctx);
int __t2audio_send_cmd_sync(struct t2audio_bce *b, struct t2audio_send_ctx *ctx, struct t2audio_msg *reply);

#define t2audio_send_with_tag(a, ctx, tag, tout, fn, ...) ({ \
    (ctx)->timeout = msecs_to_jiffies(tout); \
    (ctx)->status = __t2audio_send_prepare(&(a)->bcem, (ctx), (tag)); \
    if (!(ctx)->status) { \
        fn(&(ctx)->msg, ##__VA_ARGS__); \
        __t2audio_send(&(a)->bcem, (ctx)); \
    } \
    (ctx)->status; \
})
#define t2audio_send(a, ctx, tout, fn, ...) t2audio_send_with_tag(a, ctx, NULL, tout, fn, ##__VA_ARGS__)

#define t2audio_send_cmd_sync(a, ctx, reply, tout, fn, ...) ({ \
    (ctx)->timeout = msecs_to_jiffies(tout); \
    (ctx)->status = __t2audio_send_prepare(&(a)->bcem, (ctx), NULL); \
    if (!(ctx)->status) { \
        fn(&(ctx)->msg, ##__VA_ARGS__); \
        (ctx)->status = __t2audio_send_cmd_sync(&(a)->bcem, (ctx), (reply)); \
    } \
    (ctx)->status; \
})

struct t2audio_msg t2audio_reply_alloc(void);
void t2audio_reply_free(struct t2audio_msg *reply);

#endif //T2AUDIO_PROTOCOL_BCE_H
