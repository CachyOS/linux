#include "queue.h"
#include "vhci.h"

#include <linux/dma-mapping.h>


static void bce_vhci_message_queue_completion(struct t2bce_core_queue_sq *sq);

int bce_vhci_message_queue_create(struct bce_vhci *vhci, struct bce_vhci_message_queue *ret, const char *name)
{
    int status;
    ret->cq = t2bce_core_create_cq(vhci->client, VHCI_EVENT_QUEUE_EL_COUNT);
    if (!ret->cq)
        return -EINVAL;
    ret->sq = t2bce_core_create_sq(vhci->client, ret->cq, name, VHCI_EVENT_QUEUE_EL_COUNT, DMA_TO_DEVICE,
                            bce_vhci_message_queue_completion, ret);
    if (!ret->sq) {
        status = -EINVAL;
        goto fail_cq;
    }
    ret->data = dma_alloc_coherent(t2bce_core_client_dma_dev(vhci->client),
                                   sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                                   &ret->dma_addr, GFP_KERNEL);
    if (!ret->data) {
        status = -EINVAL;
        goto fail_sq;
    }
    return 0;

fail_sq:
    t2bce_core_destroy_sq(vhci->client, ret->sq);
    ret->sq = NULL;
fail_cq:
    t2bce_core_destroy_cq(vhci->client, ret->cq);
    ret->cq = NULL;
    return status;
}

void bce_vhci_message_queue_destroy(struct bce_vhci *vhci, struct bce_vhci_message_queue *q)
{
    if (!q->cq)
        return;
    dma_free_coherent(t2bce_core_client_dma_dev(vhci->client),
                      sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                      q->data, q->dma_addr);
    t2bce_core_destroy_sq(vhci->client, q->sq);
    t2bce_core_destroy_cq(vhci->client, q->cq);
}

void bce_vhci_message_queue_write(struct bce_vhci_message_queue *q, struct bce_vhci_message *req)
{
    int sidx;
    sidx = t2bce_core_queue_sq_tail(q->sq);
    pr_debug("t2bce_vhci: Send message: %x s=%x p1=%x p2=%llx\n", req->cmd, req->status, req->param1, req->param2);
    q->data[sidx] = *req;
    t2bce_core_set_next_submission_single(q->sq, q->dma_addr + sizeof(struct bce_vhci_message) * sidx,
            sizeof(struct bce_vhci_message));
    t2bce_core_submit_to_device(q->sq);
}

static void bce_vhci_message_queue_completion(struct t2bce_core_queue_sq *sq)
{
    while (t2bce_core_next_completion(sq))
        t2bce_core_notify_submission_complete(sq);
}



static void bce_vhci_event_queue_completion(struct t2bce_core_queue_sq *sq);

int __bce_vhci_event_queue_create(struct bce_vhci *vhci, struct bce_vhci_event_queue *ret, const char *name,
                                  t2bce_core_sq_completion compl)
{
    ret->vhci = vhci;

    ret->sq = t2bce_core_create_sq(vhci->client, vhci->ev_cq, name, VHCI_EVENT_QUEUE_EL_COUNT, DMA_FROM_DEVICE, compl, ret);
    if (!ret->sq)
        return -EINVAL;
    ret->data = dma_alloc_coherent(t2bce_core_client_dma_dev(vhci->client),
                                   sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                                   &ret->dma_addr, GFP_KERNEL);
    if (!ret->data) {
        t2bce_core_destroy_sq(vhci->client, ret->sq);
        ret->sq = NULL;
        return -EINVAL;
    }

    init_completion(&ret->queue_empty_completion);
    bce_vhci_event_queue_submit_pending(ret, VHCI_EVENT_PENDING_COUNT);
    return 0;
}

int bce_vhci_event_queue_create(struct bce_vhci *vhci, struct bce_vhci_event_queue *ret, const char *name,
        bce_vhci_event_queue_callback cb)
{
    ret->cb = cb;
    return __bce_vhci_event_queue_create(vhci, ret, name, bce_vhci_event_queue_completion);
}

void bce_vhci_event_queue_destroy(struct bce_vhci *vhci, struct bce_vhci_event_queue *q)
{
    if (!q->sq)
        return;
    dma_free_coherent(t2bce_core_client_dma_dev(vhci->client),
                      sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                      q->data, q->dma_addr);
    t2bce_core_destroy_sq(vhci->client, q->sq);
}

static void bce_vhci_event_queue_completion(struct t2bce_core_queue_sq *sq)
{
    struct t2bce_core_sq_completion_data *cd;
    struct bce_vhci_event_queue *ev = t2bce_core_queue_sq_userdata(sq);
    struct bce_vhci_message *msg;
    size_t cnt = 0;

    while ((cd = t2bce_core_next_completion(sq))) {
        if (cd->status == T2BCE_COMPLETION_ABORTED) {
            t2bce_core_notify_submission_complete(sq);
            continue;
        }
        msg = &ev->data[t2bce_core_queue_sq_head(sq)];
        pr_debug("t2bce_vhci: Got event: %x s=%x p1=%x p2=%llx\n", msg->cmd, msg->status, msg->param1, msg->param2);
        ev->cb(ev, msg);

        t2bce_core_notify_submission_complete(sq);
        ++cnt;
    }
    bce_vhci_event_queue_submit_pending(ev, cnt);
    if (t2bce_core_queue_sq_available(sq) == t2bce_core_queue_sq_capacity(sq) - 1)
        complete(&ev->queue_empty_completion);
}

void bce_vhci_event_queue_submit_pending(struct bce_vhci_event_queue *q, size_t count)
{
    int idx;
    while (count--) {
        if (t2bce_core_reserve_submission(q->sq, NULL)) {
            pr_err("t2bce_vhci: Failed to reserve an event queue submission\n");
            break;
        }
        idx = t2bce_core_queue_sq_tail(q->sq);
        t2bce_core_set_next_submission_single(q->sq,
                                  q->dma_addr + idx * sizeof(struct bce_vhci_message), sizeof(struct bce_vhci_message));
    }
    t2bce_core_submit_to_device(q->sq);
}

void bce_vhci_event_queue_pause(struct bce_vhci_event_queue *q)
{
    unsigned long timeout;
    reinit_completion(&q->queue_empty_completion);
    if (t2bce_core_flush_queue(q->vhci->client, q->sq))
        pr_warn("t2bce_vhci: failed to flush event queue\n");
    timeout = msecs_to_jiffies(5000);
    while (t2bce_core_queue_sq_available(q->sq) != t2bce_core_queue_sq_capacity(q->sq) - 1) {
        timeout = wait_for_completion_timeout(&q->queue_empty_completion, timeout);
        if (timeout == 0) {
            pr_err("t2bce_vhci: waiting for queue to be flushed timed out\n");
            break;
        }
    }
}

void bce_vhci_event_queue_resume(struct bce_vhci_event_queue *q)
{
    if (t2bce_core_queue_sq_available(q->sq) != t2bce_core_queue_sq_capacity(q->sq) - 1) {
        pr_err("t2bce_vhci: resume of a queue with pending submissions\n");
        return;
    }
    bce_vhci_event_queue_submit_pending(q, VHCI_EVENT_PENDING_COUNT);
}

void bce_vhci_command_queue_create(struct bce_vhci_command_queue *ret, struct bce_vhci_message_queue *mq)
{
    ret->mq = mq;
    ret->completion.result = NULL;
    init_completion(&ret->completion.completion);
    spin_lock_init(&ret->completion_lock);
    mutex_init(&ret->mutex);
}

void bce_vhci_command_queue_destroy(struct bce_vhci_command_queue *cq)
{
    spin_lock(&cq->completion_lock);
    if (cq->completion.result) {
        memset(cq->completion.result, 0, sizeof(struct bce_vhci_message));
        cq->completion.result->status = BCE_VHCI_ABORT;
        complete(&cq->completion.completion);
        cq->completion.result = NULL;
    }
    spin_unlock(&cq->completion_lock);
    mutex_lock(&cq->mutex);
    mutex_unlock(&cq->mutex);
    mutex_destroy(&cq->mutex);
}

void bce_vhci_command_queue_deliver_completion(struct bce_vhci_command_queue *cq, struct bce_vhci_message *msg)
{
    struct bce_vhci_command_queue_completion *c = &cq->completion;

    spin_lock(&cq->completion_lock);
    if (c->result) {
        *c->result = *msg;
        complete(&c->completion);
        c->result = NULL;
    }
    spin_unlock(&cq->completion_lock);
}

static int __bce_vhci_command_queue_execute(struct bce_vhci_command_queue *cq, struct bce_vhci_message *req,
        struct bce_vhci_message *res, unsigned long timeout)
{
    int status;
    struct bce_vhci_command_queue_completion *c;
    struct bce_vhci_message creq;
    c = &cq->completion;

    if ((status = t2bce_core_reserve_submission(cq->mq->sq, &timeout)))
        return status;

    spin_lock(&cq->completion_lock);
    c->result = res;
    reinit_completion(&c->completion);
    spin_unlock(&cq->completion_lock);

    bce_vhci_message_queue_write(cq->mq, req);

    if (!wait_for_completion_timeout(&c->completion, timeout)) {
        /* Ask bridgeOS to cancel the command before reporting timeout. */
        pr_debug("t2bce_vhci: command timed out req=%x\n", req->cmd);
        if ((status = t2bce_core_reserve_submission(cq->mq->sq, &timeout)))
            return status;

        creq = *req;
        creq.cmd |= 0x4000;
        bce_vhci_message_queue_write(cq->mq, &creq);

        if (!wait_for_completion_timeout(&c->completion, 1000)) {
            pr_err("t2bce_vhci: Possible desync, cmd cancel timed out\n");

            spin_lock(&cq->completion_lock);
            c->result = NULL;
            spin_unlock(&cq->completion_lock);
            return -ETIMEDOUT;
        }
        if ((res->cmd & ~0x8000) == creq.cmd)
            return -ETIMEDOUT;
        /* The original reply arrived after the cancellation was queued. */
    }

    if ((res->cmd & ~0x8000) != req->cmd) {
        pr_err("t2bce_vhci: Possible desync, cmd reply mismatch req=%x, res=%x\n", req->cmd, res->cmd);
        return -EIO;
    }
    if (res->status == BCE_VHCI_SUCCESS)
        return 0;
    return res->status;
}

int bce_vhci_command_queue_execute(struct bce_vhci_command_queue *cq, struct bce_vhci_message *req,
                                   struct bce_vhci_message *res, unsigned long timeout)
{
    int status;
    mutex_lock(&cq->mutex);
    status = __bce_vhci_command_queue_execute(cq, req, res, timeout);
    mutex_unlock(&cq->mutex);
    return status;
}
