#ifndef T2BCE_CORE_TRANSPORT_H
#define T2BCE_CORE_TRANSPORT_H

#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/gfp_types.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

struct t2bce_core_client;
struct t2bce_core_queue_cq;
struct t2bce_core_queue_sq;
struct t2bce_core_segment_list;

typedef void (*t2bce_core_sq_completion)(struct t2bce_core_queue_sq *sq);
typedef void (*t2bce_core_client_resume_callback)(void *userdata);

struct t2bce_core_client_pm_ops {
    void (*shutdown)(void *userdata);
    void (*pm_reset)(void *userdata);
    int (*pm_prepare)(void *userdata);
    int (*pm_prepare_no_state)(void *userdata);
    void (*pm_mark_no_state_resume)(void *userdata);
    bool (*pm_is_no_state_resume)(void *userdata);
    void (*pm_complete)(void *userdata);
};

struct t2bce_core_sq_completion_data {
    u32 status;
    u64 data_size;
    u64 result;
};

enum t2bce_completion_status {
    T2BCE_COMPLETION_SUCCESS = 0,
    T2BCE_COMPLETION_ERROR = 1,
    T2BCE_COMPLETION_ABORTED = 2,
    T2BCE_COMPLETION_NO_SPACE = 3,
    T2BCE_COMPLETION_OVERRUN = 4,
};

struct t2bce_core_client *t2bce_core_client_get(struct device *dev);
void t2bce_core_client_put(struct t2bce_core_client *client);

struct device *t2bce_core_device_get(void);
void t2bce_core_device_put(struct device *dev);
struct device *t2bce_core_client_dma_dev(struct t2bce_core_client *client);
bool t2bce_core_client_no_state_resume(struct t2bce_core_client *client);

void t2bce_core_client_set_resume_complete_callback(struct t2bce_core_client *client,
        t2bce_core_client_resume_callback callback, void *userdata);
void t2bce_core_client_set_pm_ops(struct t2bce_core_client *client,
        const struct t2bce_core_client_pm_ops *ops, void *userdata);

struct t2bce_core_queue_cq *t2bce_core_create_cq(struct t2bce_core_client *client, u32 el_count);
void t2bce_core_destroy_cq(struct t2bce_core_client *client, struct t2bce_core_queue_cq *cq);

struct t2bce_core_queue_sq *t2bce_core_create_sq(struct t2bce_core_client *client, struct t2bce_core_queue_cq *cq,
        const char *name, u32 el_count, enum dma_data_direction direction,
        t2bce_core_sq_completion compl, void *userdata);
void t2bce_core_destroy_sq(struct t2bce_core_client *client, struct t2bce_core_queue_sq *sq);

/* Reserved-qid variants: allocate from a fixed band (must match
 * BCE_QUEUE_AVE_MIN/MAX in t2bce_dma_queue.h) that vhci/audio/hid never
 * touch, so a late completion for a qid they just recycled can never land
 * on a queue that took over the same number moments later. */
struct t2bce_core_queue_cq *t2bce_core_create_cq_reserved(struct t2bce_core_client *client, u32 el_count);
struct t2bce_core_queue_sq *t2bce_core_create_sq_reserved(struct t2bce_core_client *client, struct t2bce_core_queue_cq *cq,
        const char *name, u32 el_count, enum dma_data_direction direction,
        t2bce_core_sq_completion compl, void *userdata);

void *t2bce_core_queue_sq_userdata(struct t2bce_core_queue_sq *sq);

int t2bce_core_reserve_submission(struct t2bce_core_queue_sq *sq, unsigned long *timeout);
void t2bce_core_cancel_submission_reservation(struct t2bce_core_queue_sq *sq);
void t2bce_core_set_next_submission_single(struct t2bce_core_queue_sq *sq, dma_addr_t addr, size_t size);
struct t2bce_core_segment_list *t2bce_core_create_segment_list(
        struct t2bce_core_client *client, struct scatterlist *sgl,
        unsigned int mapped_nents, gfp_t gfp);
void t2bce_core_destroy_segment_list(struct t2bce_core_client *client,
        struct t2bce_core_segment_list *list);
int t2bce_core_set_next_submission_segment_list(struct t2bce_core_queue_sq *sq,
        const struct t2bce_core_segment_list *list, size_t offset, size_t size,
        size_t *submitted_size);
void t2bce_core_submit_to_device(struct t2bce_core_queue_sq *sq);
void t2bce_core_notify_submission_complete(struct t2bce_core_queue_sq *sq);

struct t2bce_core_sq_completion_data *t2bce_core_next_completion(struct t2bce_core_queue_sq *sq);

u32 t2bce_core_queue_sq_head(struct t2bce_core_queue_sq *sq);
u32 t2bce_core_queue_sq_tail(struct t2bce_core_queue_sq *sq);
u32 t2bce_core_queue_sq_available(struct t2bce_core_queue_sq *sq);
u32 t2bce_core_queue_sq_capacity(struct t2bce_core_queue_sq *sq);
int t2bce_core_flush_queue(struct t2bce_core_client *client, struct t2bce_core_queue_sq *sq);
void t2bce_core_synchronize_completions(struct t2bce_core_client *client);

#endif
