#include "t2bce_dma_queue.h"
#include <linux/export.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>

#define REG_DOORBELL_BASE 0x44000

struct bce_qe_submission {
    u64 length;
    u64 addr;

    u64 segl_addr;
    u64 segl_length;
};

enum bce_submission_type {
    BCE_SUBMISSION_SINGLE,
    BCE_SUBMISSION_SEGMENT_LIST,
};

struct bce_submission {
    enum bce_submission_type type;
    union {
        struct {
            dma_addr_t addr;
            size_t size;
        } single;
        struct {
            dma_addr_t addr;
            size_t size;
        } segment_list;
    };
};

enum bce_cmdq_command {
    BCE_CMD_REGISTER_MEMORY_QUEUE = 0x20,
    BCE_CMD_UNREGISTER_MEMORY_QUEUE = 0x30,
    BCE_CMD_FLUSH_MEMORY_QUEUE = 0x40,
    BCE_CMD_SET_MEMORY_QUEUE_PROPERTY = 0x50
};
struct bce_cmdq_simple_memory_queue_cmd {
    u16 cmd; // bce_cmdq_command
    u16 flags;
    u16 qid;
};
struct bce_cmdq_register_memory_queue_cmd {
    u16 cmd; // bce_cmdq_command
    u16 flags;
    u16 qid;
    u16 _pad;
    u16 el_count;
    u16 vector_or_cq;
    u16 _pad2;
    u16 name_len;
    char name[0x20];
    u64 addr;
    u64 length;
};

struct bce_queue_cq *t2bce_dma_alloc_cq(struct t2bce_dma_engine *dma, int qid, u32 el_count)
{
    struct bce_queue_cq *q;
    q = kzalloc(sizeof(struct bce_queue_cq), GFP_KERNEL);
    if (!q)
        return NULL;
    q->qid = qid;
    q->type = BCE_QUEUE_CQ;
    q->el_count = el_count;
    q->data = dma_alloc_coherent(dma->dma_dev, el_count * sizeof(struct bce_qe_completion),
            &q->dma_handle, GFP_KERNEL);
    if (!q->data) {
        pr_err("DMA queue memory alloc failed\n");
        kfree(q);
        return NULL;
    }
    return q;
}
EXPORT_SYMBOL_GPL(t2bce_dma_alloc_cq);

void t2bce_dma_get_cq_memcfg(struct bce_queue_cq *cq, struct bce_queue_memcfg *cfg)
{
    cfg->qid = (u16) cq->qid;
    cfg->el_count = (u16) cq->el_count;
    cfg->vector_or_cq = 0;
    cfg->_pad = 0;
    cfg->addr = cq->dma_handle;
    cfg->length = cq->el_count * sizeof(struct bce_qe_completion);
}
EXPORT_SYMBOL_GPL(t2bce_dma_get_cq_memcfg);

void t2bce_dma_free_cq(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq)
{
    dma_free_coherent(dma->dma_dev, cq->el_count * sizeof(struct bce_qe_completion), cq->data, cq->dma_handle);
    kfree(cq);
}
EXPORT_SYMBOL_GPL(t2bce_dma_free_cq);

static void t2bce_dma_handle_cq_completion(struct t2bce_dma_engine *dma, struct bce_qe_completion *e, size_t *ce)
{
    struct bce_queue *target;
    struct bce_queue_sq *target_sq;
    struct bce_sq_completion_data *cmpl;
    if (e->qid >= BCE_MAX_QUEUE_COUNT) {
        pr_err("Device sent a response for qid (%u) >= BCE_MAX_QUEUE_COUNT\n", e->qid);
        return;
    }
    target = dma->queues[e->qid];
    if (!target || target->type != BCE_QUEUE_SQ) {
        pr_err("Device sent a response for qid (%u), which does not exist\n", e->qid);
        return;
    }
    target_sq = (struct bce_queue_sq *) target;
    if (target_sq->completion_tail != e->completion_index) {
        pr_err("Completion index mismatch; this is likely going to make this driver unusable\n");
        return;
    }
    if (!target_sq->has_pending_completions) {
        target_sq->has_pending_completions = true;
        dma->int_sq_list[(*ce)++] = target_sq;
    }
    cmpl = &target_sq->completion_data[e->completion_index];
    cmpl->status = e->status;
    cmpl->data_size = e->data_size;
    cmpl->result = e->result;
    wmb();
    target_sq->completion_tail = (target_sq->completion_tail + 1) % target_sq->el_count;
}

void t2bce_dma_handle_cq_completions_locked(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq, size_t *ce)
{
    struct bce_qe_completion *e;
    e = t2bce_dma_cq_element(cq, cq->index);
    if (!(e->flags & BCE_COMPLETION_FLAG_PENDING))
        return;
    mb();
    while (true) {
        e = t2bce_dma_cq_element(cq, cq->index);
        if (!(e->flags & BCE_COMPLETION_FLAG_PENDING))
            break;
        pr_debug("t2bce_dma: compl: %i: %i %llx %llx", e->qid, e->status, e->data_size, e->result);
        t2bce_dma_handle_cq_completion(dma, e, ce);
        e->flags = 0;
        cq->index = (cq->index + 1) % cq->el_count;
    }
    mb();
    iowrite32(cq->index, (u32 *) ((u8 *) dma->reg_mem_dma +  REG_DOORBELL_BASE) + cq->qid);
}
EXPORT_SYMBOL_GPL(t2bce_dma_handle_cq_completions_locked);

void t2bce_dma_dispatch_pending_sq_completions(struct t2bce_dma_engine *dma, size_t ce)
{
    struct bce_queue_sq *sq;

    while (ce) {
        --ce;
        sq = dma->int_sq_list[ce];
        sq->completion(sq);
        sq->has_pending_completions = false;
    }
}
EXPORT_SYMBOL_GPL(t2bce_dma_dispatch_pending_sq_completions);


struct bce_queue_sq *t2bce_dma_alloc_sq(struct t2bce_dma_engine *dma, int qid, u32 el_size, u32 el_count,
        bce_sq_completion compl, void *userdata)
{
    struct bce_queue_sq *q;
    q = kzalloc(sizeof(struct bce_queue_sq), GFP_KERNEL);
    if (!q)
        return NULL;
    q->qid = qid;
    q->type = BCE_QUEUE_SQ;
    q->el_size = el_size;
    q->el_count = el_count;
    q->data = dma_alloc_coherent(dma->dma_dev, el_count * el_size,
                                 &q->dma_handle, GFP_KERNEL);
    q->completion = compl;
    q->userdata = userdata;
    q->completion_data = kzalloc(sizeof(struct bce_sq_completion_data) * el_count, GFP_KERNEL);
    q->reg_mem_dma = dma->reg_mem_dma;
    atomic_set(&q->available_commands, el_count - 1);
    init_completion(&q->available_command_completion);
    atomic_set(&q->available_command_completion_waiting_count, 0);
    if (!q->data || !q->completion_data) {
        pr_err("DMA queue memory alloc failed\n");
        if (q->data)
            dma_free_coherent(dma->dma_dev, el_count * el_size, q->data, q->dma_handle);
        kfree(q->completion_data);
        kfree(q);
        return NULL;
    }
    return q;
}
EXPORT_SYMBOL_GPL(t2bce_dma_alloc_sq);

void t2bce_dma_get_sq_memcfg(struct bce_queue_sq *sq, struct bce_queue_cq *cq, struct bce_queue_memcfg *cfg)
{
    cfg->qid = (u16) sq->qid;
    cfg->el_count = (u16) sq->el_count;
    cfg->vector_or_cq = (u16) cq->qid;
    cfg->_pad = 0;
    cfg->addr = sq->dma_handle;
    cfg->length = sq->el_count * sq->el_size;
}
EXPORT_SYMBOL_GPL(t2bce_dma_get_sq_memcfg);

void t2bce_dma_free_sq(struct t2bce_dma_engine *dma, struct bce_queue_sq *sq)
{
    dma_free_coherent(dma->dma_dev, sq->el_count * sq->el_size, sq->data, sq->dma_handle);
    kfree(sq->completion_data);
    kfree(sq);
}
EXPORT_SYMBOL_GPL(t2bce_dma_free_sq);

int t2bce_dma_reserve_submission(struct bce_queue_sq *sq, unsigned long *timeout)
{
    while (atomic_dec_if_positive(&sq->available_commands) < 0) {
        if (!timeout || !*timeout)
            return -EAGAIN;
        atomic_inc(&sq->available_command_completion_waiting_count);
        *timeout = wait_for_completion_timeout(&sq->available_command_completion, *timeout);
        if (!*timeout) {
            if (atomic_dec_if_positive(&sq->available_command_completion_waiting_count) < 0)
                try_wait_for_completion(&sq->available_command_completion); /* consume the pending completion */
        }
    }
    return 0;
}
EXPORT_SYMBOL_GPL(t2bce_dma_reserve_submission);

void t2bce_dma_cancel_submission_reservation(struct bce_queue_sq *sq)
{
    atomic_inc(&sq->available_commands);
}
EXPORT_SYMBOL_GPL(t2bce_dma_cancel_submission_reservation);

static void *t2bce_dma_next_submission(struct bce_queue_sq *sq)
{
    void *ret = t2bce_dma_sq_element(sq, sq->tail);
    sq->tail = (sq->tail + 1) % sq->el_count;
    return ret;
}

void t2bce_dma_submit_to_device(struct bce_queue_sq *sq)
{
    mb();
    iowrite32(sq->tail, (u32 *) ((u8 *) sq->reg_mem_dma +  REG_DOORBELL_BASE) + sq->qid);
}
EXPORT_SYMBOL_GPL(t2bce_dma_submit_to_device);

void t2bce_dma_notify_submission_complete(struct bce_queue_sq *sq)
{
    sq->head = (sq->head + 1) % sq->el_count;
    atomic_inc(&sq->available_commands);
    if (atomic_dec_if_positive(&sq->available_command_completion_waiting_count) >= 0) {
        complete(&sq->available_command_completion);
    }
}
EXPORT_SYMBOL_GPL(t2bce_dma_notify_submission_complete);

static void t2bce_dma_write_submission(struct bce_qe_submission *element, const struct bce_submission *submission)
{
    switch (submission->type) {
    case BCE_SUBMISSION_SINGLE:
        element->addr = submission->single.addr;
        element->length = submission->single.size;
        element->segl_addr = 0;
        element->segl_length = 0;
        break;
    case BCE_SUBMISSION_SEGMENT_LIST:
        element->addr = 0;
        element->length = 0;
        element->segl_addr = submission->segment_list.addr;
        element->segl_length = submission->segment_list.size;
        break;
    }
}

void t2bce_dma_set_next_submission_single(struct bce_queue_sq *sq, dma_addr_t addr, size_t size)
{
    struct bce_submission submission = {
        .type = BCE_SUBMISSION_SINGLE,
        .single = {
            .addr = addr,
            .size = size,
        },
    };

    t2bce_dma_write_submission(t2bce_dma_next_submission(sq), &submission);
}
EXPORT_SYMBOL_GPL(t2bce_dma_set_next_submission_single);

void t2bce_dma_set_next_submission_segment_list(struct bce_queue_sq *sq,
        dma_addr_t segl_addr, size_t segl_size)
{
    struct bce_submission submission = {
        .type = BCE_SUBMISSION_SEGMENT_LIST,
        .segment_list = {
            .addr = segl_addr,
            .size = segl_size,
        },
    };

    t2bce_dma_write_submission(t2bce_dma_next_submission(sq), &submission);
}
EXPORT_SYMBOL_GPL(t2bce_dma_set_next_submission_segment_list);

static void t2bce_dma_cmdq_completion(struct bce_queue_sq *q);

struct bce_queue_cmdq *t2bce_dma_alloc_cmdq(struct t2bce_dma_engine *dma, int qid, u32 el_count)
{
    struct bce_queue_cmdq *q;
    q = kzalloc(sizeof(struct bce_queue_cmdq), GFP_KERNEL);
    if (!q)
        return NULL;
    q->sq = t2bce_dma_alloc_sq(dma, qid, BCE_CMD_SIZE, el_count, t2bce_dma_cmdq_completion, q);
    if (!q->sq) {
        kfree(q);
        return NULL;
    }
    spin_lock_init(&q->lck);
    q->tres = kzalloc(sizeof(struct bce_queue_cmdq_result_el*) * el_count, GFP_KERNEL);
    if (!q->tres) {
        t2bce_dma_free_sq(dma, q->sq);
        kfree(q);
        return NULL;
    }
    q->slot_gen = kzalloc(sizeof(u32) * el_count, GFP_KERNEL);
    if (!q->slot_gen) {
        kfree(q->tres);
        t2bce_dma_free_sq(dma, q->sq);
        kfree(q);
        return NULL;
    }
    return q;
}
EXPORT_SYMBOL_GPL(t2bce_dma_alloc_cmdq);

void t2bce_dma_free_cmdq(struct t2bce_dma_engine *dma, struct bce_queue_cmdq *cmdq)
{
    t2bce_dma_free_sq(dma, cmdq->sq);
    kfree(cmdq->slot_gen);
    kfree(cmdq->tres);
    kfree(cmdq);
}
EXPORT_SYMBOL_GPL(t2bce_dma_free_cmdq);

void t2bce_dma_cmdq_completion(struct bce_queue_sq *q)
{
    struct bce_queue_cmdq_result_el *el;
    struct bce_queue_cmdq *cmdq = q->userdata;
    struct bce_sq_completion_data *result;

    spin_lock(&cmdq->lck);
    while ((result = t2bce_dma_next_completion(q))) {
        el = cmdq->tres[cmdq->sq->head];
        if (el && el->generation == cmdq->slot_gen[cmdq->sq->head]) {
            el->result = result->result;
            el->status = result->status;
            mb();
            complete(&el->cmpl);
        }
        cmdq->tres[cmdq->sq->head] = NULL;
        t2bce_dma_notify_submission_complete(q);
    }
    spin_unlock(&cmdq->lck);
}

static __always_inline void *t2bce_dma_cmd_start(struct bce_queue_cmdq *cmdq, struct bce_queue_cmdq_result_el *res)
{
    void *ret;
    unsigned long timeout;
    init_completion(&res->cmpl);
    mb();

    timeout = msecs_to_jiffies(1000L * 60 * 5); /* wait for up to ~5 minutes */
    if (t2bce_dma_reserve_submission(cmdq->sq, &timeout))
        return NULL;

    spin_lock(&cmdq->lck);
    res->slot = cmdq->sq->tail;
    res->generation = cmdq->slot_gen[cmdq->sq->tail];
    cmdq->tres[cmdq->sq->tail] = res;
    ret = t2bce_dma_next_submission(cmdq->sq);
    return ret;
}

static __always_inline int t2bce_dma_cmd_finish(struct bce_queue_cmdq *cmdq, struct bce_queue_cmdq_result_el *res)
{
    t2bce_dma_submit_to_device(cmdq->sq);
    spin_unlock(&cmdq->lck);

    if (!wait_for_completion_timeout(&res->cmpl, msecs_to_jiffies(5000))) {
        pr_err("t2bce_dma: command queue timeout (slot %u)\n", res->slot);
        spin_lock(&cmdq->lck);
        cmdq->tres[res->slot] = NULL;
        cmdq->slot_gen[res->slot]++;
        spin_unlock(&cmdq->lck);
        t2bce_dma_notify_submission_complete(cmdq->sq);
        return -ETIMEDOUT;
    }
    mb();
    return 0;
}

u32 t2bce_dma_cmd_register_queue(struct bce_queue_cmdq *cmdq, struct bce_queue_memcfg *cfg, const char *name, bool isdirout)
{
    struct bce_queue_cmdq_result_el res;
    struct bce_cmdq_register_memory_queue_cmd *cmd = t2bce_dma_cmd_start(cmdq, &res);
    if (!cmd)
        return (u32) -1;
    cmd->cmd = BCE_CMD_REGISTER_MEMORY_QUEUE;
    cmd->flags = (u16) ((name ? 2 : 0) | (isdirout ? 1 : 0));
    cmd->qid = cfg->qid;
    cmd->el_count = cfg->el_count;
    cmd->vector_or_cq = cfg->vector_or_cq;
    memset(cmd->name, 0, sizeof(cmd->name));
    if (name) {
        cmd->name_len = (u16) min(strlen(name), (size_t) sizeof(cmd->name));
        memcpy(cmd->name, name, cmd->name_len);
    } else {
        cmd->name_len = 0;
    }
    cmd->addr = cfg->addr;
    cmd->length = cfg->length;

    if (t2bce_dma_cmd_finish(cmdq, &res))
        return (u32)-1;
    return res.status;
}
EXPORT_SYMBOL_GPL(t2bce_dma_cmd_register_queue);

u32 t2bce_dma_cmd_unregister_memory_queue(struct bce_queue_cmdq *cmdq, u16 qid)
{
    struct bce_queue_cmdq_result_el res;
    struct bce_cmdq_simple_memory_queue_cmd *cmd = t2bce_dma_cmd_start(cmdq, &res);
    if (!cmd)
        return (u32) -1;
    cmd->cmd = BCE_CMD_UNREGISTER_MEMORY_QUEUE;
    cmd->flags = 0;
    cmd->qid = qid;
    if (t2bce_dma_cmd_finish(cmdq, &res))
        return (u32)-1;
    return res.status;
}
EXPORT_SYMBOL_GPL(t2bce_dma_cmd_unregister_memory_queue);

u32 t2bce_dma_cmd_flush_memory_queue(struct bce_queue_cmdq *cmdq, u16 qid)
{
    struct bce_queue_cmdq_result_el res;
    struct bce_cmdq_simple_memory_queue_cmd *cmd = t2bce_dma_cmd_start(cmdq, &res);
    if (!cmd)
        return (u32) -1;
    cmd->cmd = BCE_CMD_FLUSH_MEMORY_QUEUE;
    cmd->flags = 0;
    cmd->qid = qid;
    if (t2bce_dma_cmd_finish(cmdq, &res))
        return (u32)-1;
    return res.status;
}
EXPORT_SYMBOL_GPL(t2bce_dma_cmd_flush_memory_queue);

static int t2bce_dma_register_queue(struct t2bce_dma_engine *dma, struct bce_queue_memcfg *cfg,
        const char *name, bool isdirout)
{
    if (!dma->ops || !dma->ops->register_queue)
        return -ENODEV;

    return dma->ops->register_queue(dma->ops_userdata, cfg, name, isdirout);
}

static int t2bce_dma_unregister_queue(struct t2bce_dma_engine *dma, u16 qid)
{
    if (!dma->ops || !dma->ops->unregister_queue)
        return -ENODEV;

    return dma->ops->unregister_queue(dma->ops_userdata, qid);
}

static int t2bce_dma_flush_queue(struct t2bce_dma_engine *dma, u16 qid)
{
    if (!dma->ops || !dma->ops->flush_queue)
        return -ENODEV;

    return dma->ops->flush_queue(dma->ops_userdata, qid);
}


struct bce_queue_cq *t2bce_dma_create_cq(struct t2bce_dma_engine *dma, u32 el_count)
{
    struct bce_queue_cq *cq;
    struct bce_queue_memcfg cfg;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0)
    int qid = ida_simple_get(&dma->queue_ida, BCE_QUEUE_USER_MIN, BCE_QUEUE_USER_MAX, GFP_KERNEL);
#else
    int qid = ida_alloc_range(&dma->queue_ida, BCE_QUEUE_USER_MIN, BCE_QUEUE_USER_MAX - 1, GFP_KERNEL);
#endif
    if (qid < 0)
        return NULL;
    cq = t2bce_dma_alloc_cq(dma, qid, el_count);
    if (!cq)
        return NULL;
    t2bce_dma_get_cq_memcfg(cq, &cfg);
    if (t2bce_dma_register_queue(dma, &cfg, NULL, false) != 0) {
        pr_err("t2bce_dma: CQ registration failed (%i)", qid);
        t2bce_dma_free_cq(dma, cq);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0)
        ida_simple_remove(&dma->queue_ida, (uint) qid);
#else
        ida_free(&dma->queue_ida, (uint) qid);
#endif
        return NULL;
    }
    dma->queues[qid] = (struct bce_queue *) cq;
    return cq;
}
EXPORT_SYMBOL_GPL(t2bce_dma_create_cq);

struct bce_queue_sq *t2bce_dma_create_sq(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq, const char *name, u32 el_count,
        int direction, bce_sq_completion compl, void *userdata)
{
    struct bce_queue_sq *sq;
    struct bce_queue_memcfg cfg;
    int qid;
    if (cq == NULL)
        return NULL; /* cq can not be null */
    if (name == NULL)
        return NULL; /* name can not be null */
    if (direction != DMA_TO_DEVICE && direction != DMA_FROM_DEVICE)
        return NULL; /* unsupported direction */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0)
    qid = ida_simple_get(&dma->queue_ida, BCE_QUEUE_USER_MIN, BCE_QUEUE_USER_MAX, GFP_KERNEL);
#else
    qid = ida_alloc_range(&dma->queue_ida, BCE_QUEUE_USER_MIN, BCE_QUEUE_USER_MAX - 1, GFP_KERNEL);
#endif
    if (qid < 0)
        return NULL;
    sq = t2bce_dma_alloc_sq(dma, qid, sizeof(struct bce_qe_submission), el_count, compl, userdata);
    if (!sq)
        return NULL;
    t2bce_dma_get_sq_memcfg(sq, cq, &cfg);
    if (t2bce_dma_register_queue(dma, &cfg, name, direction != DMA_FROM_DEVICE) != 0) {
        pr_err("t2bce_dma: SQ registration failed (%i)", qid);
        t2bce_dma_free_sq(dma, sq);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0)
        ida_simple_remove(&dma->queue_ida, (uint) qid);
#else
        ida_free(&dma->queue_ida, (uint) qid);
#endif
        return NULL;
    }
    spin_lock(&dma->queues_lock);
    dma->queues[qid] = (struct bce_queue *) sq;
    spin_unlock(&dma->queues_lock);
    return sq;
}
EXPORT_SYMBOL_GPL(t2bce_dma_create_sq);

struct bce_queue_sq *t2bce_dma_create_sq_with_flags(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq, const char *name,
        u32 el_count, u16 flags, bce_sq_completion compl, void *userdata)
{
    int direction = (flags & 1) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    return t2bce_dma_create_sq(dma, cq, name, el_count, direction, compl, userdata);
}
EXPORT_SYMBOL_GPL(t2bce_dma_create_sq_with_flags);

void t2bce_dma_destroy_cq(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq)
{
    if (!dma->is_being_removed && t2bce_dma_unregister_queue(dma, (u16) cq->qid))
        pr_err("t2bce_dma: CQ unregister failed");
    spin_lock(&dma->queues_lock);
    dma->queues[cq->qid] = NULL;
    spin_unlock(&dma->queues_lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0)
    ida_simple_remove(&dma->queue_ida, (uint) cq->qid);
#else
    ida_free(&dma->queue_ida, (uint) cq->qid);
#endif
    t2bce_dma_free_cq(dma, cq);
}
EXPORT_SYMBOL_GPL(t2bce_dma_destroy_cq);

void t2bce_dma_destroy_sq(struct t2bce_dma_engine *dma, struct bce_queue_sq *sq)
{
    if (!dma->is_being_removed && t2bce_dma_unregister_queue(dma, (u16) sq->qid))
        pr_err("t2bce_dma: SQ unregister failed");
    spin_lock(&dma->queues_lock);
    dma->queues[sq->qid] = NULL;
    spin_unlock(&dma->queues_lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0)
    ida_simple_remove(&dma->queue_ida, (uint) sq->qid);
#else
    ida_free(&dma->queue_ida, (uint) sq->qid);
#endif
    t2bce_dma_free_sq(dma, sq);
}
EXPORT_SYMBOL_GPL(t2bce_dma_destroy_sq);

int t2bce_dma_flush_sq(struct t2bce_dma_engine *dma, struct bce_queue_sq *sq)
{
    return t2bce_dma_flush_queue(dma, (u16) sq->qid);
}
EXPORT_SYMBOL_GPL(t2bce_dma_flush_sq);

static int __init t2bce_dma_module_init(void)
{
    pr_info("t2bce_dma: module initialized\n");
    return 0;
}

static void __exit t2bce_dma_module_exit(void)
{
    pr_info("t2bce_dma: module exited\n");
}

module_init(t2bce_dma_module_init);
module_exit(t2bce_dma_module_exit);

MODULE_AUTHOR("André Eikmeyer <andre.eikmeyer@gmail.com>");
MODULE_DESCRIPTION("Apple T2 BCE DMA queue engine");
MODULE_VERSION("0.01");
MODULE_LICENSE("GPL");
