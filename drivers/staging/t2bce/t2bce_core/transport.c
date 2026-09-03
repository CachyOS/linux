#include "t2bce_core_transport.h"
#include "t2bce.h"

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/string.h>

struct t2bce_core_client {
    struct t2bce_device *bce;
    struct device *dev;
    struct device_link *link;
    struct list_head list;
    t2bce_core_client_resume_callback resume_complete;
    void *resume_complete_userdata;
    struct t2bce_core_client_pm_ops pm_ops;
    void *pm_userdata;
};

struct t2bce_sq_ctx {
    t2bce_core_sq_completion completion;
    void *userdata;
};

static struct bce_queue_cq *to_bce_cq(struct t2bce_core_queue_cq *cq)
{
    return (struct bce_queue_cq *) cq;
}

static struct t2bce_core_queue_cq *to_t2bce_cq(struct bce_queue_cq *cq)
{
    return (struct t2bce_core_queue_cq *) cq;
}

static struct bce_queue_sq *to_bce_sq(struct t2bce_core_queue_sq *sq)
{
    return (struct bce_queue_sq *) sq;
}

static struct t2bce_core_queue_sq *to_t2bce_sq(struct bce_queue_sq *sq)
{
    return (struct t2bce_core_queue_sq *) sq;
}

static void t2bce_sq_completion_adapter(struct bce_queue_sq *sq)
{
    struct t2bce_sq_ctx *ctx = sq->userdata;

    ctx->completion(to_t2bce_sq(sq));
}

struct t2bce_core_client *t2bce_core_client_get(struct device *dev)
{
    struct t2bce_device *bce = global_bce;
    struct t2bce_core_client *client;

    if (!bce)
        return ERR_PTR(-EPROBE_DEFER);

    if (bce->is_being_removed)
        return ERR_PTR(-ENODEV);

    client = kzalloc(sizeof(*client), GFP_KERNEL);
    if (!client)
        return ERR_PTR(-ENOMEM);

    client->bce = bce;
    client->dev = dev;
    client->link = device_link_add(dev, &bce->pci->dev,
            DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_CONSUMER);
    if (!client->link) {
        kfree(client);
        return ERR_PTR(-ENODEV);
    }

    mutex_lock(&bce->clients_lock);
    list_add_tail_rcu(&client->list, &bce->clients);
    mutex_unlock(&bce->clients_lock);

    return client;
}
EXPORT_SYMBOL_GPL(t2bce_core_client_get);

void t2bce_core_client_put(struct t2bce_core_client *client)
{
    if (!client)
        return;

    mutex_lock(&client->bce->clients_lock);
    list_del_rcu(&client->list);
    mutex_unlock(&client->bce->clients_lock);

    synchronize_srcu(&client->bce->clients_srcu);

    if (client->link)
        device_link_del(client->link);
    kfree(client);
}
EXPORT_SYMBOL_GPL(t2bce_core_client_put);

struct device *t2bce_core_device_get(void)
{
    struct t2bce_device *bce = global_bce;

    if (!bce)
        return ERR_PTR(-EPROBE_DEFER);

    if (bce->is_being_removed || !bce->dev)
        return ERR_PTR(-ENODEV);

    return get_device(bce->dev);
}
EXPORT_SYMBOL_GPL(t2bce_core_device_get);

void t2bce_core_device_put(struct device *dev)
{
    if (dev)
        put_device(dev);
}
EXPORT_SYMBOL_GPL(t2bce_core_device_put);

struct device *t2bce_core_client_dma_dev(struct t2bce_core_client *client)
{
    return &client->bce->pci->dev;
}
EXPORT_SYMBOL_GPL(t2bce_core_client_dma_dev);

bool t2bce_core_client_no_state_resume(struct t2bce_core_client *client)
{
    return client->bce->no_state_resume;
}
EXPORT_SYMBOL_GPL(t2bce_core_client_no_state_resume);

void t2bce_core_client_set_resume_complete_callback(struct t2bce_core_client *client,
        t2bce_core_client_resume_callback callback, void *userdata)
{
    if (!callback) {
        smp_store_release(&client->resume_complete, NULL);
        WRITE_ONCE(client->resume_complete_userdata, NULL);
        return;
    }

    WRITE_ONCE(client->resume_complete_userdata, userdata);
    smp_store_release(&client->resume_complete, callback);
}
EXPORT_SYMBOL_GPL(t2bce_core_client_set_resume_complete_callback);

void t2bce_core_client_set_pm_ops(struct t2bce_core_client *client,
        const struct t2bce_core_client_pm_ops *ops, void *userdata)
{
    if (!ops) {
        memset(&client->pm_ops, 0, sizeof(client->pm_ops));
        WRITE_ONCE(client->pm_userdata, NULL);
        return;
    }

    WRITE_ONCE(client->pm_userdata, userdata);
    client->pm_ops = *ops;
}
EXPORT_SYMBOL_GPL(t2bce_core_client_set_pm_ops);

void t2bce_core_clients_shutdown(struct t2bce_device *bce)
{
    struct t2bce_core_client *client;
    int srcu_idx;

    srcu_idx = srcu_read_lock(&bce->clients_srcu);
    list_for_each_entry_srcu(client, &bce->clients, list,
            srcu_read_lock_held(&bce->clients_srcu)) {
        if (client->pm_ops.shutdown)
            client->pm_ops.shutdown(READ_ONCE(client->pm_userdata));
    }
    srcu_read_unlock(&bce->clients_srcu, srcu_idx);
}

void t2bce_core_clients_pm_reset(struct t2bce_device *bce)
{
    struct t2bce_core_client *client;
    int srcu_idx;

    srcu_idx = srcu_read_lock(&bce->clients_srcu);
    list_for_each_entry_srcu(client, &bce->clients, list,
            srcu_read_lock_held(&bce->clients_srcu)) {
        if (client->pm_ops.pm_reset)
            client->pm_ops.pm_reset(READ_ONCE(client->pm_userdata));
    }
    srcu_read_unlock(&bce->clients_srcu, srcu_idx);
}

void t2bce_core_clients_pm_prepare(struct t2bce_device *bce)
{
    struct t2bce_core_client *client;
    int srcu_idx;

    srcu_idx = srcu_read_lock(&bce->clients_srcu);
    list_for_each_entry_srcu(client, &bce->clients, list,
            srcu_read_lock_held(&bce->clients_srcu)) {
        if (client->pm_ops.pm_prepare)
            client->pm_ops.pm_prepare(READ_ONCE(client->pm_userdata));
    }
    srcu_read_unlock(&bce->clients_srcu, srcu_idx);
}

void t2bce_core_clients_pm_prepare_no_state(struct t2bce_device *bce)
{
    struct t2bce_core_client *client;
    int srcu_idx;

    srcu_idx = srcu_read_lock(&bce->clients_srcu);
    list_for_each_entry_srcu(client, &bce->clients, list,
            srcu_read_lock_held(&bce->clients_srcu)) {
        if (client->pm_ops.pm_prepare_no_state)
            client->pm_ops.pm_prepare_no_state(READ_ONCE(client->pm_userdata));
    }
    srcu_read_unlock(&bce->clients_srcu, srcu_idx);
}

void t2bce_core_clients_pm_mark_no_state_resume(struct t2bce_device *bce)
{
    struct t2bce_core_client *client;
    int srcu_idx;

    srcu_idx = srcu_read_lock(&bce->clients_srcu);
    list_for_each_entry_srcu(client, &bce->clients, list,
            srcu_read_lock_held(&bce->clients_srcu)) {
        if (client->pm_ops.pm_mark_no_state_resume)
            client->pm_ops.pm_mark_no_state_resume(READ_ONCE(client->pm_userdata));
    }
    srcu_read_unlock(&bce->clients_srcu, srcu_idx);
}

bool t2bce_core_clients_pm_has_no_state_resume(struct t2bce_device *bce)
{
    struct t2bce_core_client *client;
    bool ret = false;
    int srcu_idx;

    srcu_idx = srcu_read_lock(&bce->clients_srcu);
    list_for_each_entry_srcu(client, &bce->clients, list,
            srcu_read_lock_held(&bce->clients_srcu)) {
        if (client->pm_ops.pm_is_no_state_resume &&
            client->pm_ops.pm_is_no_state_resume(READ_ONCE(client->pm_userdata))) {
            ret = true;
            break;
        }
    }
    srcu_read_unlock(&bce->clients_srcu, srcu_idx);
    return ret;
}

void t2bce_core_clients_pm_complete(struct t2bce_device *bce)
{
    struct t2bce_core_client *client;
    int srcu_idx;

    srcu_idx = srcu_read_lock(&bce->clients_srcu);
    list_for_each_entry_srcu(client, &bce->clients, list,
            srcu_read_lock_held(&bce->clients_srcu)) {
        if (client->pm_ops.pm_complete)
            client->pm_ops.pm_complete(READ_ONCE(client->pm_userdata));
    }
    srcu_read_unlock(&bce->clients_srcu, srcu_idx);
}

void t2bce_core_notify_resume_complete(struct t2bce_device *bce)
{
    struct t2bce_core_client *client;
    int srcu_idx;

    srcu_idx = srcu_read_lock(&bce->clients_srcu);
    list_for_each_entry_srcu(client, &bce->clients, list,
            srcu_read_lock_held(&bce->clients_srcu)) {
        t2bce_core_client_resume_callback callback = smp_load_acquire(&client->resume_complete);

        if (callback)
            callback(READ_ONCE(client->resume_complete_userdata));
    }
    srcu_read_unlock(&bce->clients_srcu, srcu_idx);
}

struct t2bce_core_queue_cq *t2bce_core_create_cq(struct t2bce_core_client *client, u32 el_count)
{
    return to_t2bce_cq(t2bce_dma_create_cq(&client->bce->dma, el_count));
}
EXPORT_SYMBOL_GPL(t2bce_core_create_cq);

void t2bce_core_destroy_cq(struct t2bce_core_client *client, struct t2bce_core_queue_cq *cq)
{
    t2bce_dma_destroy_cq(&client->bce->dma, to_bce_cq(cq));
}
EXPORT_SYMBOL_GPL(t2bce_core_destroy_cq);

struct t2bce_core_queue_sq *t2bce_core_create_sq(struct t2bce_core_client *client, struct t2bce_core_queue_cq *cq,
        const char *name, u32 el_count, enum dma_data_direction direction,
        t2bce_core_sq_completion compl, void *userdata)
{
    struct t2bce_sq_ctx *ctx;
    struct bce_queue_sq *sq;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return NULL;

    ctx->completion = compl;
    ctx->userdata = userdata;

    sq = t2bce_dma_create_sq(&client->bce->dma, to_bce_cq(cq), name, el_count, direction,
            t2bce_sq_completion_adapter, ctx);
    if (!sq) {
        kfree(ctx);
        return NULL;
    }

    return to_t2bce_sq(sq);
}
EXPORT_SYMBOL_GPL(t2bce_core_create_sq);

void t2bce_core_destroy_sq(struct t2bce_core_client *client, struct t2bce_core_queue_sq *sq)
{
    struct bce_queue_sq *bce_sq = to_bce_sq(sq);
    struct t2bce_sq_ctx *ctx = bce_sq->userdata;

    t2bce_dma_destroy_sq(&client->bce->dma, bce_sq);
    kfree(ctx);
}
EXPORT_SYMBOL_GPL(t2bce_core_destroy_sq);

void *t2bce_core_queue_sq_userdata(struct t2bce_core_queue_sq *sq)
{
    struct t2bce_sq_ctx *ctx = to_bce_sq(sq)->userdata;

    return ctx->userdata;
}
EXPORT_SYMBOL_GPL(t2bce_core_queue_sq_userdata);

int t2bce_core_reserve_submission(struct t2bce_core_queue_sq *sq, unsigned long *timeout)
{
    return t2bce_dma_reserve_submission(to_bce_sq(sq), timeout);
}
EXPORT_SYMBOL_GPL(t2bce_core_reserve_submission);

void t2bce_core_cancel_submission_reservation(struct t2bce_core_queue_sq *sq)
{
    t2bce_dma_cancel_submission_reservation(to_bce_sq(sq));
}
EXPORT_SYMBOL_GPL(t2bce_core_cancel_submission_reservation);

void t2bce_core_set_next_submission_single(struct t2bce_core_queue_sq *sq, dma_addr_t addr, size_t size)
{
    t2bce_dma_set_next_submission_single(to_bce_sq(sq), addr, size);
}
EXPORT_SYMBOL_GPL(t2bce_core_set_next_submission_single);

void t2bce_core_set_next_submission_segment_list(struct t2bce_core_queue_sq *sq,
        dma_addr_t segl_addr, size_t segl_size)
{
    t2bce_dma_set_next_submission_segment_list(to_bce_sq(sq), segl_addr, segl_size);
}
EXPORT_SYMBOL_GPL(t2bce_core_set_next_submission_segment_list);

void t2bce_core_submit_to_device(struct t2bce_core_queue_sq *sq)
{
    t2bce_dma_submit_to_device(to_bce_sq(sq));
}
EXPORT_SYMBOL_GPL(t2bce_core_submit_to_device);

void t2bce_core_notify_submission_complete(struct t2bce_core_queue_sq *sq)
{
    t2bce_dma_notify_submission_complete(to_bce_sq(sq));
}
EXPORT_SYMBOL_GPL(t2bce_core_notify_submission_complete);

struct t2bce_core_sq_completion_data *t2bce_core_next_completion(struct t2bce_core_queue_sq *sq)
{
    return (struct t2bce_core_sq_completion_data *) t2bce_dma_next_completion(to_bce_sq(sq));
}
EXPORT_SYMBOL_GPL(t2bce_core_next_completion);

u32 t2bce_core_queue_sq_head(struct t2bce_core_queue_sq *sq)
{
    return to_bce_sq(sq)->head;
}
EXPORT_SYMBOL_GPL(t2bce_core_queue_sq_head);

u32 t2bce_core_queue_sq_tail(struct t2bce_core_queue_sq *sq)
{
    return to_bce_sq(sq)->tail;
}
EXPORT_SYMBOL_GPL(t2bce_core_queue_sq_tail);

u32 t2bce_core_queue_sq_available(struct t2bce_core_queue_sq *sq)
{
    return atomic_read(&to_bce_sq(sq)->available_commands);
}
EXPORT_SYMBOL_GPL(t2bce_core_queue_sq_available);

u32 t2bce_core_queue_sq_capacity(struct t2bce_core_queue_sq *sq)
{
    return to_bce_sq(sq)->el_count;
}
EXPORT_SYMBOL_GPL(t2bce_core_queue_sq_capacity);

int t2bce_core_flush_queue(struct t2bce_core_client *client, struct t2bce_core_queue_sq *sq)
{
    return t2bce_dma_flush_sq(&client->bce->dma, to_bce_sq(sq));
}
EXPORT_SYMBOL_GPL(t2bce_core_flush_queue);
