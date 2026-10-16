#define pr_fmt(fmt) "t2bce_ave: " fmt

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "protocol.h"
#include "t2bce_core_transport.h"

static int t2bce_ave_submit_q3_buf_nowait(struct t2bce_ave_queues *queues);

static int t2bce_ave_ring_alloc(struct t2bce_ave_queues *queues,
				struct t2bce_ave_ring *ring,
				size_t element_size, size_t element_count)
{
	size_t i;

	ring->element_size = element_size;
	ring->element_count = element_count;
	ring->data = kcalloc(element_count, sizeof(*ring->data), GFP_KERNEL);
	if (!ring->data)
		return -ENOMEM;

	ring->dma = kcalloc(element_count, sizeof(*ring->dma), GFP_KERNEL);
	if (!ring->dma)
		goto err_data;

	for (i = 0; i < element_count; i++) {
		ring->data[i] = dma_alloc_coherent(queues->dma_dev, element_size,
						   &ring->dma[i], GFP_KERNEL);
		if (!ring->data[i])
			goto err_dma;
	}

	return 0;

err_dma:
	while (i--)
		dma_free_coherent(queues->dma_dev, element_size, ring->data[i],
				  ring->dma[i]);
	kfree(ring->dma);
	ring->dma = NULL;
err_data:
	kfree(ring->data);
	ring->data = NULL;
	return -ENOMEM;
}

static void t2bce_ave_ring_free(struct t2bce_ave_queues *queues,
				struct t2bce_ave_ring *ring)
{
	size_t i;

	if (!ring->data)
		return;

	for (i = 0; i < ring->element_count; i++)
		dma_free_coherent(queues->dma_dev, ring->element_size,
				  ring->data[i], ring->dma[i]);

	kfree(ring->dma);
	kfree(ring->data);
	memset(ring, 0, sizeof(*ring));
}

static void t2bce_ave_q0_complete(struct t2bce_core_queue_sq *sq)
{
	struct t2bce_ave_queues *queues = t2bce_core_queue_sq_userdata(sq);
	struct t2bce_core_sq_completion_data *completion;

	while ((completion = t2bce_core_next_completion(sq))) {
		pr_debug("Q0 completion status=%u data_size=%llu result=0x%llx\n",
			completion->status, completion->data_size,
			completion->result);
		t2bce_core_notify_submission_complete(sq);
	}
	wake_up(&queues->parameter_wait);
}

static void t2bce_ave_q1_complete(struct t2bce_core_queue_sq *sq)
{
	struct t2bce_ave_queues *queues = t2bce_core_queue_sq_userdata(sq);
	struct t2bce_core_sq_completion_data *completion;

	while ((completion = t2bce_core_next_completion(sq))) {
		size_t head = queues->q1.head;
		void *response = queues->q1.data[head % queues->q1.element_count];
		s32 response_status = 0;

		pr_debug("Q1 completion status=%u data_size=%llu result=0x%llx\n",
			completion->status, completion->data_size,
			completion->result);

		queues->parameter_status = completion->status ==
			T2BCE_COMPLETION_SUCCESS ? 0 : -EIO;
		if (!queues->parameter_status && completion->data_size >= 8) {
			response_status = get_unaligned_le32(response + 4);
			if (response_status) {
				pr_err("AVE response command=%u status=%d\n",
				       get_unaligned_le32(response), response_status);
				queues->parameter_status = -EREMOTEIO;
			}
		}
		queues->response_status = response_status;
		queues->parameter_response = response;
		queues->parameter_response_size = completion->data_size;
		t2bce_core_notify_submission_complete(sq);
		smp_store_release(&queues->q1.head, head + 1);
	}

	wake_up(&queues->parameter_wait);
}

static void t2bce_ave_q2_complete(struct t2bce_core_queue_sq *sq)
{
	struct t2bce_ave_queues *queues = t2bce_core_queue_sq_userdata(sq);

	while (t2bce_core_next_completion(sq)) {
		t2bce_core_notify_submission_complete(sq);
		WRITE_ONCE(queues->q2.head, queues->q2.head + 1);
	}
	wake_up(&queues->parameter_wait);
}

static void t2bce_ave_q3_complete(struct t2bce_core_queue_sq *sq)
{
	struct t2bce_ave_queues *queues = t2bce_core_queue_sq_userdata(sq);
	struct t2bce_core_sq_completion_data *completion;
	unsigned int completed = 0;

	while ((completion = t2bce_core_next_completion(sq))) {
		size_t head = queues->q3.head;

		queues->callback_size[head % T2BCE_AVE_OUTPUT_BUF_COUNT] =
			completion->data_size;
		queues->callback_status[head % T2BCE_AVE_OUTPUT_BUF_COUNT] =
			completion->status == T2BCE_COMPLETION_SUCCESS ? 0 : -EIO;
		queues->callback_echoed[head % T2BCE_AVE_OUTPUT_BUF_COUNT] = false;
		t2bce_core_notify_submission_complete(sq);
		smp_store_release(&queues->q3.head, head + 1);
		completed++;
	}

	if (completed)
		wake_up(&queues->callback_wait);
	if (completed)
		wake_up(&queues->parameter_wait);
	while (READ_ONCE(queues->callback_auto_resubmit) && completed--) {
		if (t2bce_ave_submit_q3_buf_nowait(queues))
			break;
	}
}

static int t2bce_ave_create_queue(struct t2bce_ave_queues *queues, unsigned int index,
				  const char *name, enum dma_data_direction direction,
				  t2bce_core_sq_completion completion,
				  struct t2bce_core_queue_sq **sq)
{
	queues->cq[index] = t2bce_core_create_cq(queues->client,
						 T2BCE_AVE_CQ_DEPTH);
	if (!queues->cq[index])
		return -ENOMEM;

	*sq = t2bce_core_create_sq(queues->client, queues->cq[index], name,
				   T2BCE_AVE_SQ_DEPTH, direction, completion,
				   queues);
	if (!*sq) {
		t2bce_core_destroy_cq(queues->client, queues->cq[index]);
		queues->cq[index] = NULL;
		return -ENOMEM;
	}

	return 0;
}

int t2bce_ave_queues_create(struct t2bce_core_client *client,
			    struct t2bce_ave_queues *queues)
{
	int ret;

	memset(queues, 0, sizeof(*queues));
	queues->client = client;
	queues->dma_dev = t2bce_core_client_dma_dev(client);
	init_waitqueue_head(&queues->parameter_wait);
	init_waitqueue_head(&queues->callback_wait);

	ret = t2bce_ave_create_queue(queues, 0, "AVEParameterSubmitQueue",
				     DMA_TO_DEVICE, t2bce_ave_q0_complete,
				     &queues->parameter_submit);
	if (ret)
		goto err;

	ret = t2bce_ave_create_queue(queues, 1, "AVEParameterReturnQueue",
				     DMA_FROM_DEVICE, t2bce_ave_q1_complete,
				     &queues->parameter_return);
	if (ret)
		goto err;

	ret = t2bce_ave_create_queue(queues, 2, "AVECallbackReturnQueue",
				     DMA_TO_DEVICE, t2bce_ave_q2_complete,
				     &queues->callback_return);
	if (ret)
		goto err;

	ret = t2bce_ave_create_queue(queues, 3, "AVECallbackSubmitQueue",
				     DMA_FROM_DEVICE, t2bce_ave_q3_complete,
				     &queues->callback_submit);
	if (ret)
		goto err;

	ret = t2bce_ave_ring_alloc(queues, &queues->q0, T2BCE_AVE_CMD_BUF_SIZE,
				   T2BCE_AVE_RECV_BUF_COUNT);
	if (ret)
		goto err;
	ret = t2bce_ave_ring_alloc(queues, &queues->q1, T2BCE_AVE_CMD_BUF_SIZE,
				   T2BCE_AVE_RECV_BUF_COUNT);
	if (ret)
		goto err;
	ret = t2bce_ave_ring_alloc(queues, &queues->q2, T2BCE_AVE_CMD_BUF_SIZE,
				   T2BCE_AVE_RECV_BUF_COUNT);
	if (ret)
		goto err;
	ret = t2bce_ave_ring_alloc(queues, &queues->q3,
				   T2BCE_AVE_MAX_ENCODED_SIZE,
				   T2BCE_AVE_OUTPUT_BUF_COUNT);
	if (ret)
		goto err;

	return 0;

err:
	t2bce_ave_queues_destroy(queues);
	return ret;
}

static void t2bce_ave_destroy_queue(struct t2bce_ave_queues *queues,
				    unsigned int index,
				    struct t2bce_core_queue_sq **sq)
{
	if (*sq) {
		t2bce_core_destroy_sq(queues->client, *sq);
		*sq = NULL;
	}
	if (queues->cq[index]) {
		t2bce_core_destroy_cq(queues->client, queues->cq[index]);
		queues->cq[index] = NULL;
	}
}

static void t2bce_ave_flush_queue(struct t2bce_ave_queues *queues,
				struct t2bce_core_queue_sq *sq, unsigned int index)
{
	int status;

	if (!sq)
		return;
	status = t2bce_core_flush_queue(queues->client, sq);
	if (!status && !wait_event_timeout(queues->parameter_wait,
			t2bce_core_queue_sq_available(sq) ==
			t2bce_core_queue_sq_capacity(sq) - 1,
			msecs_to_jiffies(T2BCE_AVE_SUBMIT_TIMEOUT_MS)))
		status = -ETIMEDOUT;
	if (status)
		pr_err("teardown: Q%u flush/completion wait failed (%d)\n", index, status);
}

void t2bce_ave_queues_destroy(struct t2bce_ave_queues *queues)
{
	if (!queues->client)
		return;
	WRITE_ONCE(queues->callback_auto_resubmit, false);
	t2bce_core_synchronize_completions(queues->client);

	t2bce_ave_flush_queue(queues, queues->callback_submit, 3);
	t2bce_ave_flush_queue(queues, queues->callback_return, 2);
	t2bce_ave_flush_queue(queues, queues->parameter_return, 1);
	t2bce_ave_flush_queue(queues, queues->parameter_submit, 0);
	t2bce_core_synchronize_completions(queues->client);

	t2bce_ave_destroy_queue(queues, 3, &queues->callback_submit);
	t2bce_ave_destroy_queue(queues, 2, &queues->callback_return);
	t2bce_ave_destroy_queue(queues, 1, &queues->parameter_return);
	t2bce_ave_destroy_queue(queues, 0, &queues->parameter_submit);
	t2bce_core_synchronize_completions(queues->client);
	t2bce_ave_ring_free(queues, &queues->q3);
	t2bce_ave_ring_free(queues, &queues->q2);
	t2bce_ave_ring_free(queues, &queues->q1);
	t2bce_ave_ring_free(queues, &queues->q0);
	queues->client = NULL;
	queues->dma_dev = NULL;
}

static int t2bce_ave_submit_single(struct t2bce_core_queue_sq *sq,
				   dma_addr_t dma, size_t size)
{
	unsigned long timeout = msecs_to_jiffies(T2BCE_AVE_SUBMIT_TIMEOUT_MS);
	int ret;

	ret = t2bce_core_reserve_submission(sq, &timeout);
	if (ret)
		return ret;

	t2bce_core_set_next_submission_single(sq, dma, size);
	t2bce_core_submit_to_device(sq);
	return 0;
}

static int t2bce_ave_submit_q3_buf_nowait(struct t2bce_ave_queues *queues)
{
	struct t2bce_ave_ring *ring = &queues->q3;
	size_t slot = ring->tail % ring->element_count;
	int ret;

	memset(ring->data[slot], 0xff,
	       min_t(size_t, ring->element_size, T2BCE_AVE_CMD_BUF_SIZE));
	ret = t2bce_core_reserve_submission(queues->callback_submit, NULL);
	if (ret)
		return ret;
	t2bce_core_set_next_submission_single(queues->callback_submit,
			ring->dma[slot], ring->element_size);
	ring->tail++;
	t2bce_core_submit_to_device(queues->callback_submit);
	return 0;
}

int t2bce_ave_submit_q0_cmd(struct t2bce_ave_queues *queues, void *buf,
			    size_t size)
{
	struct t2bce_ave_ring *ring = &queues->q0;
	size_t slot;
	int ret;

	if (size > ring->element_size)
		return -EINVAL;
	slot = ring->tail++ % ring->element_count;
	memcpy(ring->data[slot], buf, size);
	ret = t2bce_ave_submit_single(queues->parameter_submit,
				      ring->dma[slot], size);
	if (ret)
		ring->tail--;
	return ret;
}

int t2bce_ave_submit_q1_recv(struct t2bce_ave_queues *queues)
{
	struct t2bce_ave_ring *ring = &queues->q1;
	size_t slot = ring->tail % ring->element_count;
	int ret;

	memset(ring->data[slot], 0xff, ring->element_size);
	ret = t2bce_ave_submit_single(queues->parameter_return,
				      ring->dma[slot], ring->element_size);
	if (!ret)
		ring->tail++;
	return ret;
}

int t2bce_ave_wait_q1(struct t2bce_ave_queues *queues, unsigned long timeout_ms)
{
	size_t target = queues->q1.tail;

	if (!wait_event_timeout(queues->parameter_wait,
			smp_load_acquire(&queues->q1.head) >= target,
			msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;
	return queues->parameter_status;
}

int t2bce_ave_cmd_send_sync(struct t2bce_ave_queues *queues, void *buf,
			    size_t size)
{
	int ret;

	ret = t2bce_ave_submit_q0_cmd(queues, buf, size);
	if (ret)
		return ret;
	ret = t2bce_ave_submit_q1_recv(queues);
	if (ret)
		return ret;
	return t2bce_ave_wait_q1(queues, T2BCE_AVE_RESPONSE_TIMEOUT_MS);
}

int t2bce_ave_submit_q3_buf(struct t2bce_ave_queues *queues)
{
	struct t2bce_ave_ring *ring = &queues->q3;
	size_t slot = ring->tail % ring->element_count;
	int ret;

	memset(ring->data[slot], 0xff,
	       min_t(size_t, ring->element_size, T2BCE_AVE_CMD_BUF_SIZE));
	ret = t2bce_ave_submit_single(queues->callback_submit,
				      ring->dma[slot], ring->element_size);
	if (!ret)
		ring->tail++;
	return ret;
}

void t2bce_ave_presubmit_recv_bufs(struct t2bce_ave_queues *queues)
{
	unsigned int i;

	queues->callback_auto_resubmit = true;
	for (i = 0; i < T2BCE_AVE_OUTPUT_BUF_COUNT; i++)
		if (t2bce_ave_submit_q3_buf(queues))
			break;
}

int t2bce_ave_submit_q2_echo(struct t2bce_ave_queues *queues,
			     const void *data)
{
	struct t2bce_ave_ring *ring = &queues->q2;
	size_t slot = ring->tail % ring->element_count;
	size_t i;
	int ret;

	memcpy(ring->data[slot], data, ring->element_size);
	ret = t2bce_ave_submit_single(queues->callback_return,
				      ring->dma[slot], ring->element_size);
	if (!ret) {
		ring->tail++;
		for (i = 0; i < queues->q3.element_count; i++)
			if (queues->q3.data[i] == data)
				queues->callback_echoed[i] = true;
	}
	return ret;
}

/* Called only during teardown, after disabling Q3 auto-resubmission.
 * aveserverd emits 0x0b metadata, an optional payload, then waits for a
 * metadata echo on Q2. Recycling Q3 alone cannot release that callback.
 */
static int t2bce_ave_drain_callbacks(struct t2bce_ave_queues *queues)
{
	struct t2bce_ave_ring *ring = &queues->q3;
	size_t seq, slot;
	void *data;
	bool payload;
	int status;

	while (ring->tail < ring->element_count) {
		status = t2bce_ave_submit_q3_buf(queues);
		if (status)
			return status;
	}
	while ((seq = ring->tail - ring->element_count) <
	       smp_load_acquire(&ring->head)) {
		slot = seq % ring->element_count;
		data = ring->data[slot];
		if (queues->callback_status[slot])
			return queues->callback_status[slot];
		payload = false;
		if (!queues->drain_payload) {
			if (queues->callback_size[slot] != T2BCE_AVE_CMD_BUF_SIZE ||
			    get_unaligned_le32(data) != T2BCE_AVE_CMD_ENCODED_FRAME)
				return -EPROTO;
			payload = get_unaligned_le64(data + 0x50) != 0;
			if (!queues->callback_echoed[slot]) {
				if (queues->q2.tail - READ_ONCE(queues->q2.head) >=
				    queues->q2.element_count)
					return -EBUSY;
				status = t2bce_ave_submit_q2_echo(queues, data);
				if (status)
					return status;
			}
		}
		status = t2bce_ave_submit_q3_buf(queues);
		if (status)
			return status;
		queues->drain_payload = payload;
	}
	return 0;
}

int t2bce_ave_cmd_drain(struct t2bce_ave_queues *queues, void *buf)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(T2BCE_AVE_RESPONSE_TIMEOUT_MS);
	u32 command = get_unaligned_le32(buf);
	size_t seen, callbacks;
	int status;

	WRITE_ONCE(queues->callback_auto_resubmit, false);
	t2bce_core_synchronize_completions(queues->client);
	seen = smp_load_acquire(&queues->q1.head);
	/* A previous timeout can leave one receive outstanding. Reuse it. */
	if (queues->q1.tail - seen > 1)
		return -EPROTO;
	status = t2bce_ave_submit_q0_cmd(queues, buf, T2BCE_AVE_CMD_BUF_SIZE);
	if (status)
		return status;
	for (;;) {
		if (smp_load_acquire(&queues->q1.head) != seen) {
			seen = smp_load_acquire(&queues->q1.head);
			if (queues->parameter_response_size < 8)
				return -EIO;
			if (get_unaligned_le32(queues->parameter_response) == command)
				return queues->parameter_status;
			/* A late frame reply does not acknowledge EndSession. */
		}
		status = t2bce_ave_drain_callbacks(queues);
		if (status)
			return status;
		if (seen == queues->q1.tail) {
			status = t2bce_ave_submit_q1_recv(queues);
			if (status)
				return status;
		}
		callbacks = queues->q3.tail - queues->q3.element_count;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		wait_event_timeout(queues->parameter_wait,
			smp_load_acquire(&queues->q1.head) != seen ||
			smp_load_acquire(&queues->q3.head) > callbacks,
			deadline - jiffies);
	}
}

void t2bce_ave_q3_reset(struct t2bce_ave_queues *queues)
{
	queues->callback_base = READ_ONCE(queues->q3.head);
}

int t2bce_ave_wait_q3(struct t2bce_ave_queues *queues, size_t count,
			      unsigned long timeout_ms)
{
	u64 target = queues->callback_base + count;

	if (!wait_event_timeout(queues->callback_wait,
			smp_load_acquire(&queues->q3.head) >= target,
			msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;
	return 0;
}

void *t2bce_ave_q3_completed_data(struct t2bce_ave_queues *queues,
				  size_t index)
{
	u64 seq = queues->callback_base + index;

	if (seq >= smp_load_acquire(&queues->q3.head))
		return NULL;
	return queues->q3.data[seq % queues->q3.element_count];
}

size_t t2bce_ave_q3_completed_size(struct t2bce_ave_queues *queues,
				   size_t index)
{
	u64 seq = queues->callback_base + index;

	if (seq >= smp_load_acquire(&queues->q3.head))
		return 0;
	return queues->callback_size[seq % T2BCE_AVE_OUTPUT_BUF_COUNT];
}

size_t t2bce_ave_q3_event_count(struct t2bce_ave_queues *queues)
{
	return smp_load_acquire(&queues->q3.head) - queues->callback_base;
}

int t2bce_ave_submit_frame_data_async(struct t2bce_ave_queues *queues,
		void *data, size_t size, struct t2bce_ave_dma_buffer *dma)
{
	struct page **pages;
	struct scatterlist *sg;
	unsigned int offset, page_count, i;
	size_t mapped_size = 0, submitted;
	unsigned long timeout;
	int ret;

	memset(dma, 0, sizeof(*dma));
	offset = offset_in_page(data);
	page_count = DIV_ROUND_UP(offset + size, PAGE_SIZE);
	pages = kcalloc(page_count, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	for (i = 0; i < page_count; i++) {
		void *addr = (void *)((unsigned long)data - offset + i * PAGE_SIZE);

		pages[i] = is_vmalloc_addr(addr) ? vmalloc_to_page(addr) : virt_to_page(addr);
		if (!pages[i]) {
			ret = -EFAULT;
			goto free_pages;
		}
	}
	ret = sg_alloc_table_from_pages(&dma->sgt, pages, page_count, offset,
					size, GFP_KERNEL);
	if (ret)
		goto free_pages;
	dma->direction = DMA_TO_DEVICE;
	ret = dma_map_sgtable(queues->dma_dev, &dma->sgt, dma->direction, 0);
	if (ret)
		goto free_sgt;
	pr_debug("frame DMA size=%zu pages=%u orig_nents=%u mapped_nents=%u first_dma_len=%u\n",
		size, page_count, dma->sgt.orig_nents, dma->sgt.nents,
		sg_dma_len(dma->sgt.sgl));
	for_each_sg(dma->sgt.sgl, sg, dma->sgt.nents, i) {
		dma_addr_t addr = sg_dma_address(sg);
		unsigned int len = sg_dma_len(sg);

		pr_debug("frame DMA sg[%u] addr=%pad length=%u\n", i, &addr, len);
		mapped_size += len;
	}
	pr_debug("frame DMA mapped_size=%zu requested_size=%zu\n",
		mapped_size, size);

	if (dma->sgt.nents == 1) {
		pr_debug("frame DMA descriptor addr=%pad length=%zu segl_addr=0 segl_length=0\n",
			&dma->sgt.sgl->dma_address, size);
		goto reserve;
	}

	dma->segments = t2bce_core_create_segment_list(queues->client,
			dma->sgt.sgl, dma->sgt.nents, GFP_KERNEL);
	if (IS_ERR(dma->segments)) {
		ret = PTR_ERR(dma->segments);
		pr_err("frame DMA segment-list creation failed: nents=%u status=%d\n",
			dma->sgt.nents, ret);
		dma->segments = NULL;
		goto unmap;
	}
	pr_debug("frame DMA descriptor addr=0 length=%zu segl_length=%lu nents=%u\n",
		size, PAGE_SIZE, dma->sgt.nents);

reserve:
	timeout = msecs_to_jiffies(T2BCE_AVE_SUBMIT_TIMEOUT_MS);
	ret = t2bce_core_reserve_submission(queues->parameter_submit, &timeout);
	if (ret)
		goto destroy_segments;
	if (!dma->segments) {
		t2bce_core_set_next_submission_single(queues->parameter_submit,
				sg_dma_address(dma->sgt.sgl), size);
		submitted = size;
		ret = 0;
	} else {
		ret = t2bce_core_set_next_submission_segment_list(
			queues->parameter_submit, dma->segments, 0, size, &submitted);
		pr_debug("frame DMA segment-list result=%d requested=%zu submitted=%zu\n",
			ret, size, ret ? 0 : submitted);
	}
	if (ret || submitted != size) {
		t2bce_core_cancel_submission_reservation(queues->parameter_submit);
		if (!ret)
			ret = -EIO;
		goto destroy_segments;
	}
	t2bce_core_submit_to_device(queues->parameter_submit);
	kfree(pages);
	return 0;

destroy_segments:
	if (dma->segments)
		t2bce_core_destroy_segment_list(queues->client, dma->segments);
	dma->segments = NULL;
unmap:
	dma_unmap_sgtable(queues->dma_dev, &dma->sgt, dma->direction, 0);
free_sgt:
	sg_free_table(&dma->sgt);
free_pages:
	kfree(pages);
	return ret;
}

void t2bce_ave_unmap_frame_data(struct t2bce_ave_queues *queues,
		struct t2bce_ave_dma_buffer *dma)
{
	if (!dma->sgt.sgl)
		return;
	if (dma->segments)
		t2bce_core_destroy_segment_list(queues->client, dma->segments);
	dma_unmap_sgtable(queues->dma_dev, &dma->sgt, dma->direction, 0);
	sg_free_table(&dma->sgt);
	memset(&dma->sgt, 0, sizeof(dma->sgt));
	dma->segments = NULL;
}

static int t2bce_ave_command(struct t2bce_ave_queues *queues, const void *command,
			     size_t command_size, void **response,
			     size_t *response_size)
{
	struct t2bce_ave_ring *q0 = &queues->q0;
	struct t2bce_ave_ring *q1 = &queues->q1;
	size_t q0_slot, q1_slot, target;
	int ret;

	if (!command || command_size > q0->element_size)
		return -EINVAL;
	/* Q0 also carries out-of-line frame data, so its command-ring head
	 * cannot be derived from Q0 completions. Command execution is serialized
	 * and a slot is reusable after the paired Q1 response. */
	if (q1->tail - READ_ONCE(q1->head) >= q1->element_count)
		return -EBUSY;

	q0_slot = q0->tail % q0->element_count;
	q1_slot = q1->tail % q1->element_count;
	memcpy(q0->data[q0_slot], command, command_size);
	memset(q1->data[q1_slot], 0xff, q1->element_size);

	ret = t2bce_ave_submit_single(queues->parameter_submit,
				      q0->dma[q0_slot], command_size);
	if (ret)
		return ret;
	q0->tail++;

	ret = t2bce_ave_submit_single(queues->parameter_return,
				      q1->dma[q1_slot], q1->element_size);
	if (ret)
		return ret;
	target = ++q1->tail;

	if (!wait_event_timeout(queues->parameter_wait,
			smp_load_acquire(&q1->head) >= target,
			msecs_to_jiffies(T2BCE_AVE_RESPONSE_TIMEOUT_MS))) {
		pr_err("Q1 timeout: q0=%zu/%zu q1=%zu/%zu core-q0=%u/%u avail=%u core-q1=%u/%u avail=%u\n",
			READ_ONCE(q0->head), q0->tail,
			READ_ONCE(q1->head), q1->tail,
			t2bce_core_queue_sq_head(queues->parameter_submit),
			t2bce_core_queue_sq_tail(queues->parameter_submit),
			t2bce_core_queue_sq_available(queues->parameter_submit),
			t2bce_core_queue_sq_head(queues->parameter_return),
			t2bce_core_queue_sq_tail(queues->parameter_return),
			t2bce_core_queue_sq_available(queues->parameter_return));
		return -ETIMEDOUT;
	}
	if (queues->parameter_status)
		return queues->parameter_status;

	if (response)
		*response = queues->parameter_response;
	if (response_size)
		*response_size = queues->parameter_response_size;
	return 0;
}

int t2bce_ave_codec_id(struct t2bce_ave_queues *queues, u64 *session_token)
{
	u8 *command;
	void *response;
	size_t response_size;
	int ret;

	command = kmalloc(T2BCE_AVE_CMD_BUF_SIZE, GFP_KERNEL);
	if (!command)
		return -ENOMEM;

	memset(command, 0xbb, T2BCE_AVE_CMD_BUF_SIZE);
	put_unaligned_le32(T2BCE_AVE_CMD_CODEC_ID, command);
	put_unaligned_le32(0x68766331, command + 0x08);

	ret = t2bce_ave_command(queues, command, T2BCE_AVE_CMD_BUF_SIZE, &response,
				&response_size);
	kfree(command);
	if (ret)
		return ret;
	if (response_size && response_size < 0x20)
		return -EPROTO;

	*session_token = get_unaligned_le64(response + 0x18);
	return 0;
}
struct t2bce_t2bce_ave_cmd_hdr {
	u32 cmd;
	u32 pad04;
	u64 token;
} __packed;

struct t2bce_t2bce_ave_cmd_codec_id {
	u32 cmd;
	u32 pad04;
	u32 fourcc;
} __packed;

struct t2bce_t2bce_ave_cmd_session_config {
	u32 cmd;
	u32 pad04;
	u64 token;
	u64 pad10;
	u32 width;
	u32 height;
} __packed;

struct t2bce_t2bce_ave_cmd_encode_frame {
	u32 cmd;
	u32 pad04;
	u64 token;
	u64 frame_num;
	u32 keyframe;
	u32 pad1c;
	u32 fps_num;
	u32 fps_den;
	u64 pad28;
	u32 unk30;
	u32 pad34;
	u32 duration_num;
	u32 duration_den;
	u64 pad40;
	u32 width;
	u32 pad4c;
	u32 height;
	u32 pad54;
	u32 extended_right;
	u32 pad5c;
	u32 extended_bottom;
	u32 pad64;
	u32 stride;
	u32 pad6c;
	char pixfmt[4];
	u32 pad74;
	u64 pad78;
	u64 cookie;
} __packed;

struct t2bce_t2bce_ave_cmd_copy_property {
	u32 cmd;
	u32 pad04;
	u64 token;
	char name[T2BCE_AVE_CMD_BUF_SIZE - 0x10];
} __packed;

struct t2bce_t2bce_ave_cmd_set_property {
	u32 cmd;
	u32 pad04;
	u64 token;
	char name[0x20];
	u32 buf_size;
	u32 pad34;
	u8  pad38[0x2c];
	u32 value_present;
	u8  pad68[0x0c];
	u32 type;
	u32 length;
	u32 pad7c;
	u8  value[];
} __packed;

struct t2bce_t2bce_ave_cmd_end_session {
	u32 cmd;
	u32 pad04;
	u64 token;
} __packed;

struct t2bce_ave_cmd_complete_frames {
	u32 cmd;
	u32 pad04;
	u64 token;
	s64 value;
	s32 timescale;
	u32 flags;
	s64 epoch;
} __packed;

static_assert(offsetof(struct t2bce_t2bce_ave_cmd_hdr, token) == 0x08);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_codec_id, fourcc) == 0x08);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_session_config, width) == 0x18);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_session_config, height) == 0x1c);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, frame_num) == 0x10);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, keyframe) == 0x18);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, fps_num) == 0x20);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, unk30) == 0x30);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, duration_num) == 0x38);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, width) == 0x48);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, height) == 0x50);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, extended_right) == 0x58);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, extended_bottom) == 0x60);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, stride) == 0x68);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, pixfmt) == 0x70);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_encode_frame, cookie) == 0x80);
static_assert(offsetof(struct t2bce_ave_cmd_complete_frames, value) == 0x10);
static_assert(offsetof(struct t2bce_ave_cmd_complete_frames, flags) == 0x1c);
static_assert(offsetof(struct t2bce_ave_cmd_complete_frames, epoch) == 0x20);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_copy_property, name) == 0x10);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_set_property, name) == 0x10);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_set_property, buf_size) == 0x30);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_set_property, value_present) == 0x64);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_set_property, type) == 0x74);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_set_property, length) == 0x78);
static_assert(sizeof(struct t2bce_t2bce_ave_cmd_set_property) == 0x80);
static_assert(offsetof(struct t2bce_t2bce_ave_cmd_end_session, token) == 0x08);

void t2bce_ave_build_cmd_codec_id(void *buf)
{
	struct t2bce_t2bce_ave_cmd_codec_id *c = buf;

	memset(buf, 0xBB, T2BCE_AVE_CMD_BUF_SIZE);
	c->cmd = T2BCE_AVE_CMD_CODEC_ID;
	c->fourcc = 0x68766331;
}

void t2bce_ave_build_cmd_session_config(void *buf, u32 width, u32 height)
{
	struct t2bce_t2bce_ave_cmd_session_config *c = buf;

	memset(buf, 0x00, T2BCE_AVE_CMD_BUF_SIZE);
	c->cmd = T2BCE_AVE_CMD_SESSION_CONFIG;
	c->width = width;
	c->height = height;
}

void t2bce_ave_build_cmd_encode_frame(void *buf, u64 frame_num, u32 width, u32 height,
				u32 stride, const char pixfmt[4],
				u32 fps_num, u32 fps_den, bool keyframe,
				u64 cookie)
{
	struct t2bce_t2bce_ave_cmd_encode_frame *c = buf;

	memset(buf, 0x00, T2BCE_AVE_CMD_BUF_SIZE);
	c->cmd = T2BCE_AVE_CMD_ENCODE_FRAME;
	c->frame_num = frame_num;
	c->keyframe = keyframe ? 1 : 0;
	c->fps_num = fps_num;
	c->fps_den = fps_den;
	c->unk30 = 1;
	c->duration_num = fps_num;
	c->duration_den = fps_den;
	c->width = width;
	c->height = height;
	c->extended_right = ALIGN(width, 16) - width;
	c->extended_bottom = ALIGN(height, 16) - height;
	c->stride = stride;
	memcpy(c->pixfmt, pixfmt, sizeof(c->pixfmt));
	c->cookie = cookie;
}

void t2bce_ave_build_cmd_copy_property(void *buf, const char *name)
{
	struct t2bce_t2bce_ave_cmd_copy_property *c = buf;

	memset(buf, 0x00, T2BCE_AVE_CMD_BUF_SIZE);
	c->cmd = T2BCE_AVE_CMD_COPY_PROPERTY;
	strscpy(c->name, name, sizeof(c->name));
}

static struct t2bce_t2bce_ave_cmd_set_property *t2bce_ave_build_cmd_set_property_common(void *buf,
								      const char *name)
{
	struct t2bce_t2bce_ave_cmd_set_property *c = buf;

	memset(buf, 0xBB, T2BCE_AVE_CMD_BUF_SIZE);
	memset(buf, 0x00, 0x20);

	c->cmd = T2BCE_AVE_CMD_SET_PROPERTY;
	strscpy(c->name, name, sizeof(c->name));

	c->buf_size = 0x1000;
	c->pad34 = 0x0000;
	c->value_present = 0x00000001;
	return c;
}

void t2bce_ave_build_cmd_set_property_bool(void *buf, const char *name, bool value)
{
	struct t2bce_t2bce_ave_cmd_set_property *c = t2bce_ave_build_cmd_set_property_common(buf, name);

	c->type = 0x00000001;
	c->length = 0x00000001;
	c->value[0] = value ? 1 : 0;
}

void t2bce_ave_build_cmd_set_property_s32(void *buf, const char *name, s32 value)
{
	struct t2bce_t2bce_ave_cmd_set_property *c = t2bce_ave_build_cmd_set_property_common(buf, name);

	c->type = 0x00000002;
	c->length = 0x00000004;
	*(s32 *)c->value = value;
}

void t2bce_ave_build_cmd_set_property_float32(void *buf, const char *name, u32 ieee754_bits)
{
	struct t2bce_t2bce_ave_cmd_set_property *c = t2bce_ave_build_cmd_set_property_common(buf, name);

	c->type = 0x00000004;
	c->length = 0x00000004;
	*(u32 *)c->value = ieee754_bits;
}

void t2bce_ave_build_cmd_set_property_string(void *buf, const char *name, const char *value)
{
	struct t2bce_t2bce_ave_cmd_set_property *c = t2bce_ave_build_cmd_set_property_common(buf, name);
	size_t len = strlen(value);

	c->type = 0x00000006;
	c->length = len;
	memcpy(c->value, value, min_t(size_t, len + 1, T2BCE_AVE_CMD_BUF_SIZE - 0x80));
}

void t2bce_ave_build_cmd_prepare(void *buf)
{
	struct t2bce_t2bce_ave_cmd_hdr *c = buf;

	memset(buf, 0x00, T2BCE_AVE_CMD_BUF_SIZE);
	c->cmd = T2BCE_AVE_CMD_PREPARE;
}

void t2bce_ave_build_cmd_complete_frames(void *buf)
{
	struct t2bce_ave_cmd_complete_frames *c = buf;

	memset(buf, 0x00, T2BCE_AVE_CMD_BUF_SIZE);
	c->cmd = T2BCE_AVE_CMD_COMPLETE_FRAMES;
	/* kCMTimeIndefinite = { 0, 0, kCMTimeFlags_Indefinite, 0 }. */
	c->flags = 1U << 4;
}

void t2bce_ave_build_cmd_end_session(void *buf)
{
	struct t2bce_t2bce_ave_cmd_end_session *c = buf;

	memset(buf, 0xFF, T2BCE_AVE_CMD_BUF_SIZE);
	c->cmd = T2BCE_AVE_CMD_END_SESSION;
	c->pad04 = 0;
}
