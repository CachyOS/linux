#ifndef T2BCE_T2BCE_AVE_ENCODER_H
#define T2BCE_T2BCE_AVE_ENCODER_H

#include "protocol.h"

enum t2bce_ave_session_state {
	T2BCE_AVE_STATE_IDLE,
	T2BCE_AVE_STATE_CONFIGURED,
	T2BCE_AVE_STATE_ENCODING,
	T2BCE_AVE_STATE_ERROR,
};

struct t2bce_ave_enc_params {
	u32 bitrate;
	u32 fps_num;
	u32 fps_den;
	s32 gop_size;
	s32 bitrate_mode;
	s32 quality;
	s32 min_qp;
	s32 max_qp;
	bool min_qp_set;
	bool max_qp_set;
	s32 profile;
	s32 level;
	s32 color_primaries;
	s32 ycbcr_matrix;
	s32 transfer_func;
	u32 input_pixelformat;
	u32 input_stride;
};

struct t2bce_ave_session {
	struct t2bce_core_client *bce;
	struct t2bce_ave_queues queues;

	u32 width;
	u32 height;
	u32 bitrate;
	u32 fps_num;
	u32 fps_den;
	u32 input_stride;
	char input_pixfmt[4];

	enum t2bce_ave_session_state state;
	u64 frame_counter;

	u64 session_token;

	u8 cmd_buf[T2BCE_AVE_CMD_BUF_SIZE];

	void *hvcc_data;
	size_t hvcc_size;

	void *annex_b_header;
	size_t annex_b_header_size;
};

int t2bce_ave_session_setup(struct t2bce_ave_session *session, struct t2bce_core_client *bce,
		      u32 width, u32 height, const struct t2bce_ave_enc_params *params);
void t2bce_ave_session_teardown(struct t2bce_ave_session *session);

int t2bce_ave_session_encode_frame(struct t2bce_ave_session *session,
			     void *y_data, size_t y_size,
			     void *uv_data, size_t uv_size,
			     void *out_buf, size_t out_buf_size,
			     size_t *encoded_size, bool force_keyframe,
			     bool *keyframe_out);

int t2bce_ave_session_set_bitrate(struct t2bce_ave_session *session, u32 bitrate);

int t2bce_ave_convert_to_annex_b(const void *src, size_t src_size,
			   void *dst, size_t dst_size, size_t *out_size);

#endif
