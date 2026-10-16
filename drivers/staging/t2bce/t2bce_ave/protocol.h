#ifndef T2BCE_AVE_PROTOCOL_H
#define T2BCE_AVE_PROTOCOL_H

#include <linux/dma-mapping.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/scatterlist.h>

#define T2BCE_AVE_CMD_BUF_SIZE		4096
#define T2BCE_AVE_CQ_DEPTH		256
#define T2BCE_AVE_SQ_DEPTH		128
#define T2BCE_AVE_RECV_BUF_COUNT	8
#define T2BCE_AVE_OUTPUT_BUF_COUNT	8
#define T2BCE_AVE_MAX_ENCODED_SIZE	(2 * 1024 * 1024)
#define T2BCE_AVE_SUBMIT_TIMEOUT_MS	5000
#define T2BCE_AVE_RESPONSE_TIMEOUT_MS	10000
#define T2BCE_AVE_FRAME_TIMEOUT_MS	2000
#define T2BCE_AVE_HVCC_OFFSET		0x68
#define T2BCE_AVE_HVCC_HEADER_SIZE	23

enum t2bce_ave_command {
	T2BCE_AVE_CMD_CODEC_ID = 0x00,
	T2BCE_AVE_CMD_SESSION_CONFIG = 0x01,
	T2BCE_AVE_CMD_ENCODE_FRAME = 0x02,
	T2BCE_AVE_CMD_COMPLETE_FRAMES = 0x03,
	T2BCE_AVE_CMD_PREPARE = 0x05,
	T2BCE_AVE_CMD_END_SESSION = 0x06,
	T2BCE_AVE_CMD_COPY_PROPERTY = 0x08,
	T2BCE_AVE_CMD_SET_PROPERTY = 0x09,
	T2BCE_AVE_CMD_DATA_NOTIFY = 0x0a,
	T2BCE_AVE_CMD_ENCODED_FRAME = 0x0b,
};

struct device;
struct t2bce_core_client;
struct t2bce_core_queue_cq;
struct t2bce_core_queue_sq;
struct t2bce_core_segment_list;

struct t2bce_ave_dma_buffer {
	struct sg_table sgt;
	struct t2bce_core_segment_list *segments;
	enum dma_data_direction direction;
};

struct t2bce_ave_ring {
	void **data;
	dma_addr_t *dma;
	size_t element_size;
	size_t element_count;
	size_t head;
	size_t tail;
};

struct t2bce_ave_queues {
	struct t2bce_core_client *client;
	struct device *dma_dev;
	struct t2bce_core_queue_cq *cq[4];
	struct t2bce_core_queue_sq *parameter_submit;
	struct t2bce_core_queue_sq *parameter_return;
	struct t2bce_core_queue_sq *callback_return;
	struct t2bce_core_queue_sq *callback_submit;
	struct t2bce_ave_ring q0;
	struct t2bce_ave_ring q1;
	struct t2bce_ave_ring q2;
	struct t2bce_ave_ring q3;
	wait_queue_head_t parameter_wait;
	wait_queue_head_t callback_wait;
	int parameter_status;
	s32 response_status;
	void *parameter_response;
	size_t parameter_response_size;
	size_t callback_size[T2BCE_AVE_OUTPUT_BUF_COUNT];
	int callback_status[T2BCE_AVE_OUTPUT_BUF_COUNT];
	bool callback_echoed[T2BCE_AVE_OUTPUT_BUF_COUNT];
	bool drain_payload;
	u64 callback_base;
	bool callback_auto_resubmit;
};

int t2bce_ave_queues_create(struct t2bce_core_client *client,
			    struct t2bce_ave_queues *queues);
void t2bce_ave_queues_destroy(struct t2bce_ave_queues *queues);
int t2bce_ave_codec_id(struct t2bce_ave_queues *queues, u64 *session_token);
int t2bce_ave_cmd_send_sync(struct t2bce_ave_queues *queues, void *buf,
			    size_t size);
int t2bce_ave_cmd_drain(struct t2bce_ave_queues *queues, void *buf);
int t2bce_ave_submit_q0_cmd(struct t2bce_ave_queues *queues, void *buf,
			    size_t size);
int t2bce_ave_submit_q1_recv(struct t2bce_ave_queues *queues);
int t2bce_ave_submit_q3_buf(struct t2bce_ave_queues *queues);
int t2bce_ave_submit_q2_echo(struct t2bce_ave_queues *queues,
			     const void *data);
int t2bce_ave_wait_q1(struct t2bce_ave_queues *queues, unsigned long timeout_ms);
int t2bce_ave_wait_q3(struct t2bce_ave_queues *queues, size_t count,
			      unsigned long timeout_ms);
void *t2bce_ave_q3_completed_data(struct t2bce_ave_queues *queues,
				  size_t index);
size_t t2bce_ave_q3_completed_size(struct t2bce_ave_queues *queues,
				   size_t index);
size_t t2bce_ave_q3_event_count(struct t2bce_ave_queues *queues);
void t2bce_ave_q3_reset(struct t2bce_ave_queues *queues);
void t2bce_ave_presubmit_recv_bufs(struct t2bce_ave_queues *queues);
int t2bce_ave_submit_frame_data_async(struct t2bce_ave_queues *queues,
		void *data, size_t size, struct t2bce_ave_dma_buffer *dma);
void t2bce_ave_unmap_frame_data(struct t2bce_ave_queues *queues,
		struct t2bce_ave_dma_buffer *dma);

void t2bce_ave_build_cmd_codec_id(void *buf);
void t2bce_ave_build_cmd_session_config(void *buf, u32 width, u32 height);
void t2bce_ave_build_cmd_encode_frame(void *buf, u64 frame_num, u32 width,
		u32 height, u32 stride, const char pixfmt[4], u32 fps_num,
		u32 fps_den, bool keyframe, u64 cookie);
void t2bce_ave_build_cmd_copy_property(void *buf, const char *name);
void t2bce_ave_build_cmd_set_property_bool(void *buf, const char *name, bool value);
void t2bce_ave_build_cmd_set_property_s32(void *buf, const char *name, s32 value);
void t2bce_ave_build_cmd_set_property_float32(void *buf, const char *name, u32 value);
void t2bce_ave_build_cmd_set_property_string(void *buf, const char *name,
				     const char *value);
void t2bce_ave_build_cmd_prepare(void *buf);
void t2bce_ave_build_cmd_complete_frames(void *buf);
void t2bce_ave_build_cmd_end_session(void *buf);

#endif
