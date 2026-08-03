#ifndef T2BCE_DMA_QUEUE_H
#define T2BCE_DMA_QUEUE_H

#include <linux/completion.h>
#include <linux/pci.h>

#define BCE_CMD_SIZE 0x40
#define BCE_MAX_QUEUE_COUNT 0x100

#define BCE_QUEUE_USER_MIN 2
#define BCE_QUEUE_USER_MAX (BCE_MAX_QUEUE_COUNT - 1)

struct t2bce_dma_segment {
    u64 addr;
    u64 length;
};

struct bce_queue_memcfg;

struct t2bce_dma_engine_ops {
    int (*register_queue)(void *userdata, struct bce_queue_memcfg *cfg,
            const char *name, bool isdirout);
    int (*unregister_queue)(void *userdata, u16 qid);
    int (*flush_queue)(void *userdata, u16 qid);
};

enum bce_queue_type {
    BCE_QUEUE_CQ, BCE_QUEUE_SQ
};
struct bce_queue {
    int qid;
    int type;
};
struct bce_queue_cq {
    int qid;
    int type;
    u32 el_count;
    dma_addr_t dma_handle;
    void *data;

    u32 index;
};
struct bce_queue_sq;
typedef void (*bce_sq_completion)(struct bce_queue_sq *q);
struct bce_sq_completion_data {
    u32 status;
    u64 data_size;
    u64 result;
};
struct bce_queue_sq {
    int qid;
    int type;
    u32 el_size;
    u32 el_count;
    dma_addr_t dma_handle;
    void *data;
    void *userdata;
    void __iomem *reg_mem_dma;

    atomic_t available_commands;
    struct completion available_command_completion;
    atomic_t available_command_completion_waiting_count;
    u32 head, tail;

    u32 completion_cidx, completion_tail;
    struct bce_sq_completion_data *completion_data;
    bool has_pending_completions;
    bce_sq_completion completion;
};

struct bce_queue_cmdq_result_el {
    struct completion cmpl;
    u32 status;
    u64 result;
    u32 slot;
    u32 generation;
};
struct bce_queue_cmdq {
    struct bce_queue_sq *sq;
    struct spinlock lck;
    struct bce_queue_cmdq_result_el **tres;
    u32 *slot_gen;
};

struct t2bce_dma_engine {
    struct device *dma_dev;
    const struct t2bce_dma_engine_ops *ops;
    void *ops_userdata;
    void __iomem *reg_mem_dma;
    struct bce_queue *queues[BCE_MAX_QUEUE_COUNT];
    struct spinlock queues_lock;
    struct ida queue_ida;
    struct bce_queue_cq *cmd_cq;
    struct bce_queue_cmdq *cmd_cmdq;
    struct bce_queue_sq *int_sq_list[BCE_MAX_QUEUE_COUNT];
    bool is_being_removed;
};

struct bce_queue_memcfg {
    u16 qid;
    u16 el_count;
    u16 vector_or_cq;
    u16 _pad;
    u64 addr;
    u64 length;
};

enum bce_qe_completion_status {
    BCE_COMPLETION_SUCCESS = 0,
    BCE_COMPLETION_ERROR = 1,
    BCE_COMPLETION_ABORTED = 2,
    BCE_COMPLETION_NO_SPACE = 3,
    BCE_COMPLETION_OVERRUN = 4
};
enum bce_qe_completion_flags {
    BCE_COMPLETION_FLAG_PENDING = 0x8000
};
struct bce_qe_completion {
    u64 result;
    u64 data_size;
    u16 qid;
    u16 completion_index;
    u16 status; // bce_qe_completion_status
    u16 flags;  // bce_qe_completion_flags
};

static __always_inline void *t2bce_dma_sq_element(struct bce_queue_sq *q, int i) {
    return (void *) ((u8 *) q->data + q->el_size * i);
}
static __always_inline void *t2bce_dma_cq_element(struct bce_queue_cq *q, int i) {
    return (void *) ((struct bce_qe_completion *) q->data + i);
}

static __always_inline struct bce_sq_completion_data *t2bce_dma_next_completion(struct bce_queue_sq *sq) {
    struct bce_sq_completion_data *res;
    rmb();
    if (sq->completion_cidx == sq->completion_tail)
        return NULL;
    res = &sq->completion_data[sq->completion_cidx];
    sq->completion_cidx = (sq->completion_cidx + 1) % sq->el_count;
    return res;
}

struct bce_queue_cq *t2bce_dma_alloc_cq(struct t2bce_dma_engine *dma, int qid, u32 el_count);
void t2bce_dma_get_cq_memcfg(struct bce_queue_cq *cq, struct bce_queue_memcfg *cfg);
void t2bce_dma_free_cq(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq);
void t2bce_dma_handle_cq_completions_locked(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq, size_t *ce);
void t2bce_dma_dispatch_pending_sq_completions(struct t2bce_dma_engine *dma, size_t ce);

struct bce_queue_sq *t2bce_dma_alloc_sq(struct t2bce_dma_engine *dma, int qid, u32 el_size, u32 el_count,
        bce_sq_completion compl, void *userdata);
void t2bce_dma_get_sq_memcfg(struct bce_queue_sq *sq, struct bce_queue_cq *cq, struct bce_queue_memcfg *cfg);
void t2bce_dma_free_sq(struct t2bce_dma_engine *dma, struct bce_queue_sq *sq);
int t2bce_dma_reserve_submission(struct bce_queue_sq *sq, unsigned long *timeout);
void t2bce_dma_cancel_submission_reservation(struct bce_queue_sq *sq);
void t2bce_dma_submit_to_device(struct bce_queue_sq *sq);
void t2bce_dma_notify_submission_complete(struct bce_queue_sq *sq);

void t2bce_dma_set_next_submission_single(struct bce_queue_sq *sq, dma_addr_t addr, size_t size);
void t2bce_dma_set_next_submission_segment_list(struct bce_queue_sq *sq,
        dma_addr_t segl_addr, size_t segl_size);

struct bce_queue_cmdq *t2bce_dma_alloc_cmdq(struct t2bce_dma_engine *dma, int qid, u32 el_count);
void t2bce_dma_free_cmdq(struct t2bce_dma_engine *dma, struct bce_queue_cmdq *cmdq);

u32 t2bce_dma_cmd_register_queue(struct bce_queue_cmdq *cmdq, struct bce_queue_memcfg *cfg, const char *name, bool isdirout);
u32 t2bce_dma_cmd_unregister_memory_queue(struct bce_queue_cmdq *cmdq, u16 qid);
u32 t2bce_dma_cmd_flush_memory_queue(struct bce_queue_cmdq *cmdq, u16 qid);


/* User API - Creates and registers the queue */

struct bce_queue_cq *t2bce_dma_create_cq(struct t2bce_dma_engine *dma, u32 el_count);
struct bce_queue_sq *t2bce_dma_create_sq(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq, const char *name, u32 el_count,
        int direction, bce_sq_completion compl, void *userdata);
struct bce_queue_sq *t2bce_dma_create_sq_with_flags(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq, const char *name,
        u32 el_count, u16 flags, bce_sq_completion compl, void *userdata);
void t2bce_dma_destroy_cq(struct t2bce_dma_engine *dma, struct bce_queue_cq *cq);
void t2bce_dma_destroy_sq(struct t2bce_dma_engine *dma, struct bce_queue_sq *sq);
int t2bce_dma_flush_sq(struct t2bce_dma_engine *dma, struct bce_queue_sq *sq);

#endif
