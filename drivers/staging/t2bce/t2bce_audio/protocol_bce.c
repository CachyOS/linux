#include "protocol_bce.h"

#include "audio.h"
#include <linux/dma-mapping.h>

static void t2audio_bce_out_queue_completion(struct t2bce_core_queue_sq *sq);
static void t2audio_bce_in_queue_completion(struct t2bce_core_queue_sq *sq);
static int t2audio_bce_queue_init(struct t2audio_device *dev, struct t2audio_bce_queue *q, const char *name,
                                 enum dma_data_direction direction, t2bce_core_sq_completion cfn);
void t2audio_bce_in_queue_submit_pending(struct t2audio_bce_queue *q, size_t count);
static void t2audio_deferred_msg_work(struct work_struct *ws);

int t2audio_bce_init(struct t2audio_device *dev)
{
    int status;
    struct t2audio_bce *bce = &dev->bcem;
    bce->cq = t2bce_core_create_cq(dev->bce, 0x80);
    spin_lock_init(&bce->spinlock);
    if (!bce->cq)
        return -EINVAL;
    if ((status = t2audio_bce_queue_init(dev, &bce->qout, "com.apple.BridgeAudio.IntelToARM", DMA_TO_DEVICE,
            t2audio_bce_out_queue_completion))) {
        return status;
    }
    if ((status = t2audio_bce_queue_init(dev, &bce->qin, "com.apple.BridgeAudio.ARMToIntel", DMA_FROM_DEVICE,
            t2audio_bce_in_queue_completion))) {
        return status;
    }
    t2audio_bce_in_queue_submit_pending(&bce->qin, bce->qin.el_count);
    return 0;
}

int t2audio_bce_queue_init(struct t2audio_device *dev, struct t2audio_bce_queue *q, const char *name,
        enum dma_data_direction direction, t2bce_core_sq_completion cfn)
{
    q->cq = dev->bcem.cq;
    q->el_size = T2AUDIO_BCE_QUEUE_ELEMENT_SIZE;
    q->el_count = T2AUDIO_BCE_QUEUE_ELEMENT_COUNT;
    /* Allocate one extra SQ slot because the BCE ring always keeps one entry empty. */
    q->sq = t2bce_core_create_sq(dev->bce, q->cq, name, (u32) (q->el_count + 1), direction, cfn, dev);
    if (!q->sq)
        return -EINVAL;

    q->data = dma_alloc_coherent(t2bce_core_client_dma_dev(dev->bce), q->el_size * q->el_count, &q->dma_addr, GFP_KERNEL);
    if (!q->data) {
        t2bce_core_destroy_sq(dev->bce, q->sq);
        return -EINVAL;
    }
    return 0;
}

static void t2audio_send_create_tag(struct t2audio_bce *b, int *tagn, char tag[4])
{
    b->tag_num = (b->tag_num + 1) % T2AUDIO_BCE_QUEUE_TAG_COUNT;
    *tagn = b->tag_num;
    tag[0] = 'S';
    tag[1] = '0' + b->tag_num / 100;
    tag[2] = '0' + (b->tag_num / 10) % 10;
    tag[3] = '0' + b->tag_num % 10;
}

int __t2audio_send_prepare(struct t2audio_bce *b, struct t2audio_send_ctx *ctx, char *tag)
{
    int status;
    size_t index;
    void *dptr;
    struct t2audio_msg_header *header;
    if ((status = t2bce_core_reserve_submission(b->qout.sq, &ctx->timeout)))
        return status;
    spin_lock_irqsave(&b->spinlock, ctx->irq_flags);
    index = b->qout.data_tail;
    dptr = (u8 *) b->qout.data + index * b->qout.el_size;
    ctx->msg.data = dptr;
    header = dptr;
    if (tag)
        *((u32 *) header->tag) = *((u32 *) tag);
    else
        t2audio_send_create_tag(b, &ctx->tag_n, header->tag);
    return 0;
}

void __t2audio_send(struct t2audio_bce *b, struct t2audio_send_ctx *ctx)
{
#ifdef DEBUG
    pr_debug("t2bce_audio: Sending command data\n");
    print_hex_dump(KERN_DEBUG, "t2bce_audio:OUT ", DUMP_PREFIX_NONE, 32, 1, ctx->msg.data, ctx->msg.size, true);
#endif
    t2bce_core_set_next_submission_single(b->qout.sq,
            b->qout.dma_addr + (dma_addr_t) (ctx->msg.data - b->qout.data), ctx->msg.size);
    t2bce_core_submit_to_device(b->qout.sq);
    b->qout.data_tail = (b->qout.data_tail + 1) % b->qout.el_count;
    spin_unlock_irqrestore(&b->spinlock, ctx->irq_flags);
}

int __t2audio_send_cmd_sync(struct t2audio_bce *b, struct t2audio_send_ctx *ctx, struct t2audio_msg *reply)
{
    struct t2audio_bce_queue_entry ent;
    DECLARE_COMPLETION_ONSTACK(cmpl);
    ent.msg = reply;
    ent.cmpl = &cmpl;
    b->pending_entries[ctx->tag_n] = &ent;
    __t2audio_send(b, ctx); /* drops b->spinlock */
    ctx->timeout = wait_for_completion_timeout(&cmpl, ctx->timeout);
    if (ctx->timeout == 0) {
        /*
         * Timed-out replies may still arrive later; remove the waiter before
         * returning so a stale reply cannot complete a reused stack object.
         */
        spin_lock_irqsave(&b->spinlock, ctx->irq_flags);
        if (b->pending_entries[ctx->tag_n] == &ent)
            b->pending_entries[ctx->tag_n] = NULL;
        spin_unlock_irqrestore(&b->spinlock, ctx->irq_flags);
        return -ETIMEDOUT;
    }
    return 0;
}

static void t2audio_handle_reply(struct t2audio_bce *b, struct t2audio_msg *reply)
{
    const char *tag;
    int tagn;
    unsigned long irq_flags;
    char tag_zero[5];
    struct t2audio_bce_queue_entry *entry;

    tag = ((struct t2audio_msg_header *) reply->data)->tag;
    if (tag[0] != 'S') {
        pr_err("t2bce_audio: unexpected reply tag: %.4s\n", tag);
        return;
    }
    *((u32 *) tag_zero) = *((u32 *) tag);
    tag_zero[4] = 0;
    if (kstrtoint(&tag_zero[1], 10, &tagn)) {
        pr_err("t2bce_audio: reply tag parse failed: %.4s\n", tag);
        return;
    }

    spin_lock_irqsave(&b->spinlock, irq_flags);
    entry = b->pending_entries[tagn];
    if (entry) {
        if (reply->size < entry->msg->size)
            entry->msg->size = reply->size;
        memcpy(entry->msg->data, reply->data, entry->msg->size);
        complete(entry->cmpl);

        b->pending_entries[tagn] = NULL;
    } else {
        pr_warn_ratelimited("t2bce_audio: stale reply without queued command tag=%.4s\n", tag);
    }
    spin_unlock_irqrestore(&b->spinlock, irq_flags);
}

static void t2audio_bce_out_queue_completion(struct t2bce_core_queue_sq *sq)
{
    while (t2bce_core_next_completion(sq)) {
        t2bce_core_notify_submission_complete(sq);
    }
}

static void t2audio_bce_in_queue_handle_msg(struct t2audio_device *a, struct t2audio_msg *msg);

static void t2audio_deferred_msg_work(struct work_struct *ws)
{
    struct t2audio_deferred_msg *work = container_of(ws, struct t2audio_deferred_msg, ws);
    struct t2audio_msg_header *header = work->msg.data;

    if (header->type == T2AUDIO_MSG_TYPE_COMMAND)
        t2audio_handle_command(work->a, &work->msg);
    else if (header->type == T2AUDIO_MSG_TYPE_NOTIFICATION)
        t2audio_handle_notification(work->a, &work->msg);

    kfree(work->msg.data);
    kfree(work);
}

static void t2audio_bce_in_queue_completion(struct t2bce_core_queue_sq *sq)
{
    struct t2audio_msg msg;
    struct t2audio_device *dev = t2bce_core_queue_sq_userdata(sq);
    struct t2audio_bce_queue *q = &dev->bcem.qin;
    struct t2bce_core_sq_completion_data *c;
    size_t cnt = 0;

    mb();
    while ((c = t2bce_core_next_completion(sq))) {
        msg.data = (u8 *) q->data + q->data_head * q->el_size;
        msg.size = c->data_size;
#ifdef DEBUG
        pr_debug("t2bce_audio: Received command data %llx\n", c->data_size);
        print_hex_dump(KERN_DEBUG, "t2bce_audio:IN ", DUMP_PREFIX_NONE, 32, 1, msg.data, min(msg.size, 128UL), true);
#endif
        t2audio_bce_in_queue_handle_msg(dev, &msg);

        q->data_head = (q->data_head + 1) % q->el_count;

        t2bce_core_notify_submission_complete(sq);
        ++cnt;
    }
    t2audio_bce_in_queue_submit_pending(q, cnt);
}

static void t2audio_bce_in_queue_handle_msg(struct t2audio_device *a, struct t2audio_msg *msg)
{
    struct t2audio_msg_header *header = (struct t2audio_msg_header *) msg->data;
    struct t2audio_deferred_msg *work;

    if (msg->size < sizeof(struct t2audio_msg_header)) {
        pr_err("t2bce_audio: Msg size smaller than header (%lx)", msg->size);
        return;
    }
    if (header->type == T2AUDIO_MSG_TYPE_RESPONSE) {
        t2audio_handle_reply(&a->bcem, msg);
        return;
    }

    work = kzalloc(sizeof(*work), GFP_ATOMIC);
    if (!work) {
        pr_err("t2bce_audio: Failed to allocate deferred message work\n");
        return;
    }

    work->msg.data = kmemdup(msg->data, msg->size, GFP_ATOMIC);
    if (!work->msg.data) {
        kfree(work);
        pr_err("t2bce_audio: Failed to copy deferred message\n");
        return;
    }

    work->a = a;
    work->msg.size = msg->size;
    INIT_WORK(&work->ws, t2audio_deferred_msg_work);
    schedule_work(&work->ws);
}

void t2audio_bce_in_queue_submit_pending(struct t2audio_bce_queue *q, size_t count)
{
    while (count--) {
        if (t2bce_core_reserve_submission(q->sq, NULL)) {
            pr_err("t2bce_audio: Failed to reserve an event queue submission\n");
            break;
        }
        t2bce_core_set_next_submission_single(q->sq,
                q->dma_addr + (dma_addr_t) (q->data_tail * q->el_size), q->el_size);
        q->data_tail = (q->data_tail + 1) % q->el_count;
    }
    t2bce_core_submit_to_device(q->sq);
}

struct t2audio_msg t2audio_reply_alloc(void)
{
    struct t2audio_msg ret;
    ret.size = T2AUDIO_BCE_QUEUE_ELEMENT_SIZE;
    ret.data = kmalloc(ret.size, GFP_KERNEL);
    return ret;
}

void t2audio_reply_free(struct t2audio_msg *reply)
{
    kfree(reply->data);
}
