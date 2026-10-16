#define pr_fmt(fmt) "apple-ave: " fmt

#include "encoder.h"
#include "t2bce_core_transport.h"

#include <linux/slab.h>
#include <linux/unaligned.h>
#include <media/v4l2-ctrls.h>

static inline void t2bce_ave_stamp_session_token(struct t2bce_ave_session *session)
{
	*(u64 *)(session->cmd_buf + 0x08) = session->session_token;
}

static int t2bce_ave_send_copy_property(struct t2bce_ave_session *session, const char *name)
{
	pr_debug("[setup] CopyProperty (0x08): \"%s\"\n", name);
	t2bce_ave_build_cmd_copy_property(session->cmd_buf, name);
	t2bce_ave_stamp_session_token(session);
	return t2bce_ave_cmd_send_sync(&session->queues, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
}

static int t2bce_ave_send_set_property_bool(struct t2bce_ave_session *session, const char *name, bool value)
{
	pr_debug("[setup] SetProperty (0x09): \"%s\" = %s\n",
		 name, value ? "true" : "false");
	t2bce_ave_build_cmd_set_property_bool(session->cmd_buf, name, value);
	t2bce_ave_stamp_session_token(session);
	return t2bce_ave_cmd_send_sync(&session->queues, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
}

static int t2bce_ave_send_set_property_s32(struct t2bce_ave_session *session, const char *name, s32 value)
{
	pr_debug("[setup] SetProperty (0x09): \"%s\" = %d (0x%x)\n",
		 name, value, value);
	t2bce_ave_build_cmd_set_property_s32(session->cmd_buf, name, value);
	t2bce_ave_stamp_session_token(session);
	return t2bce_ave_cmd_send_sync(&session->queues, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
}

static int t2bce_ave_send_set_property_float32(struct t2bce_ave_session *session, const char *name,
					 u32 ieee754_bits)
{
	pr_debug("[setup] SetProperty (0x09): \"%s\" = float32(0x%08x)\n",
		 name, ieee754_bits);
	t2bce_ave_build_cmd_set_property_float32(session->cmd_buf, name, ieee754_bits);
	t2bce_ave_stamp_session_token(session);
	return t2bce_ave_cmd_send_sync(&session->queues, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
}

static int t2bce_ave_send_set_property_string(struct t2bce_ave_session *session, const char *name,
					const char *value)
{
	pr_debug("[setup] SetProperty (0x09): \"%s\" = \"%s\"\n", name, value);
	t2bce_ave_build_cmd_set_property_string(session->cmd_buf, name, value);
	t2bce_ave_stamp_session_token(session);
	return t2bce_ave_cmd_send_sync(&session->queues, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
}


static const u32 t2bce_ave_quality_to_float32[101] = {
	0x00000000,
	0x3C23D70A,
	0x3CA3D70A,
	0x3CF5C28F,
	0x3D23D70A,
	0x3D4CCCCD,
	0x3D75C28F,
	0x3D8F5C29,
	0x3DA3D70A,
	0x3DB851EC,
	0x3DCCCCCD,
	0x3DE147AE,
	0x3DF5C28F,
	0x3E051EB8,
	0x3E0F5C29,
	0x3E19999A,
	0x3E23D70A,
	0x3E2E147B,
	0x3E3851EC,
	0x3E428F5C,
	0x3E4CCCCD,
	0x3E570A3D,
	0x3E6147AE,
	0x3E6B851F,
	0x3E75C28F,
	0x3E800000,
	0x3E851EB8,
	0x3E8A3D71,
	0x3E8F5C29,
	0x3E947AE1,
	0x3E99999A,
	0x3E9EB852,
	0x3EA3D70A,
	0x3EA8F5C3,
	0x3EAE147B,
	0x3EB33333,
	0x3EB851EC,
	0x3EBD70A4,
	0x3EC28F5C,
	0x3EC7AE14,
	0x3ECCCCCD,
	0x3ED1EB85,
	0x3ED70A3D,
	0x3EDC28F6,
	0x3EE147AE,
	0x3EE66666,
	0x3EEB851F,
	0x3EF0A3D7,
	0x3EF5C28F,
	0x3EFAE148,
	0x3F000000,
	0x3F028F5C,
	0x3F051EB8,
	0x3F07AE14,
	0x3F0A3D71,
	0x3F0CCCCD,
	0x3F0F5C29,
	0x3F11EB85,
	0x3F147AE1,
	0x3F170A3D,
	0x3F19999A,
	0x3F1C28F6,
	0x3F1EB852,
	0x3F2147AE,
	0x3F23D70A,
	0x3F266666,
	0x3F28F5C3,
	0x3F2B851F,
	0x3F2E147B,
	0x3F30A3D7,
	0x3F333333,
	0x3F35C28F,
	0x3F3851EC,
	0x3F3AE148,
	0x3F3D70A4,
	0x3F400000,
	0x3F428F5C,
	0x3F451EB8,
	0x3F47AE14,
	0x3F4A3D71,
	0x3F4CCCCD,
	0x3F4F5C29,
	0x3F51EB85,
	0x3F547AE1,
	0x3F570A3D,
	0x3F59999A,
	0x3F5C28F6,
	0x3F5EB852,
	0x3F6147AE,
	0x3F63D70A,
	0x3F666666,
	0x3F68F5C3,
	0x3F6B851F,
	0x3F6E147B,
	0x3F70A3D7,
	0x3F733333,
	0x3F75C28F,
	0x3F7851EC,
	0x3F7AE148,
	0x3F7D70A4,
	0x3F800000,
};


static void t2bce_ave_build_profile_level_string(char *buf, size_t size, s32 profile, s32 level)
{
	const char *prof_str;

	(void)level;

	switch (profile) {
	case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10:
		prof_str = "Main10";
		break;
	case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE:
		prof_str = "MainStill";
		break;
	default:
		prof_str = "Main";
		break;
	}

	snprintf(buf, size, "HEVC_%s_AutoLevel", prof_str);
}

static const char *t2bce_ave_color_primaries(s32 value)
{
	switch (value) {
	case 1: return "ITU_R_709_2";
	case 6: return "SMPTE_C";
	case 7: return "SMPTE_240M_1995";
	case 9: return "ITU_R_2020";
	default: return NULL;
	}
}

static const char *t2bce_ave_ycbcr_matrix(s32 value)
{
	switch (value) {
	case 1: return "ITU_R_709_2";
	case 5: return "ITU_R_601_4";
	case 9: return "ITU_R_2020";
	default: return NULL;
	}
}

static const char *t2bce_ave_transfer_function(s32 value)
{
	switch (value) {
	case 1: return "ITU_R_709_2";
	case 14: return "ITU_R_2020";
	case 16: return "SMPTE_ST_2084_PQ";
	default: return NULL;
	}
}

int t2bce_ave_session_setup(struct t2bce_ave_session *session, struct t2bce_core_client *bce,
		      u32 width, u32 height, const struct t2bce_ave_enc_params *params)
{
	const char *property_value;
	bool remote_created = false;
	int status;
	char profile_buf[64];

	pr_debug("=== SESSION SETUP START ===\n");
	pr_debug("params: %ux%u @ %u bps, %u/%u fps\n",
		 width, height, params->bitrate, params->fps_num, params->fps_den);

	memset(session, 0, sizeof(*session));
	session->bce = bce;
	session->width = width;
	session->height = height;
	session->bitrate = params->bitrate;
	session->fps_num = params->fps_num;
	session->fps_den = params->fps_den;
	session->input_stride = params->input_stride;
	if (params->input_pixelformat == V4L2_PIX_FMT_P010)
		memcpy(session->input_pixfmt, "024x", sizeof(session->input_pixfmt));
	else
		memcpy(session->input_pixfmt, "v024", sizeof(session->input_pixfmt));
	session->state = T2BCE_AVE_STATE_IDLE;

	pr_debug("[setup] Step 1: Creating BCE queues\n");
	status = t2bce_ave_queues_create(bce, &session->queues);
	if (status) {
		pr_err("[setup] queue creation failed (%d)\n", status);
		return status;
	}

	pr_debug("[setup] Step 3: CodecID (hvc1)\n");
	t2bce_ave_build_cmd_codec_id(session->cmd_buf);
	status = t2bce_ave_cmd_send_sync(&session->queues, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("[setup] CodecID FAILED (%d)\n", status);
		goto fail_queues;
	}
	pr_debug("[setup] Step 3: CodecID OK\n");
	remote_created = true;

	session->session_token = *(u64 *)(session->queues.parameter_response + 0x18);
	pr_debug("[setup] session token = 0x%llx\n", session->session_token);

	pr_debug("[setup] Step 4: SessionConfig (%ux%u)\n", width, height);
	t2bce_ave_build_cmd_session_config(session->cmd_buf, width, height);
	t2bce_ave_stamp_session_token(session);
	status = t2bce_ave_cmd_send_sync(&session->queues, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("[setup] SessionConfig FAILED (%d)\n", status);
		goto fail_queues;
	}
	pr_debug("[setup] Step 4: SessionConfig OK\n");

	pr_debug("[setup] Step 5: CopyProperty UsingHardwareAcceleratedVideoEncoder\n");
	status = t2bce_ave_send_copy_property(session, "UsingHardwareAcceleratedVideoEncoder");
	if (status) {
		pr_err("[setup] UsingHWAccel #1 FAILED (%d)\n", status);
		goto fail_queues;
	}

	pr_debug("[setup] Step 6: CopyProperty UsingHardwareAcceleratedVideoEncoder\n");
	status = t2bce_ave_send_copy_property(session, "UsingHardwareAcceleratedVideoEncoder");
	if (status) {
		pr_err("[setup] UsingHWAccel #2 FAILED (%d)\n", status);
		goto fail_queues;
	}

	status = t2bce_ave_send_set_property_bool(session, "RealTime", true);
	if (status) {
		pr_err("[setup] RealTime FAILED (%d)\n", status);
		goto fail_queues;
	}

	status = t2bce_ave_send_set_property_bool(session, "AllowFrameReordering", false);
	if (status) {
		pr_err("[setup] AllowFrameReordering FAILED (%d)\n", status);
		goto fail_queues;
	}

	if (params->fps_den == 1 && params->fps_num > 0) {
		status = t2bce_ave_send_set_property_s32(session, "ExpectedFrameRate",
						   params->fps_num);
		if (status == -ETIMEDOUT)
			goto fail_queues;
		if (status)
			pr_warn("[setup] ExpectedFrameRate rejected (%d)\n", status);
	}

	t2bce_ave_build_profile_level_string(profile_buf, sizeof(profile_buf),
				       params->profile, params->level);
	pr_info("[setup] HEVC profile=%d level=%d ProfileLevel=%s\n",
		params->profile, params->level, profile_buf);
	status = t2bce_ave_send_set_property_string(session, "ProfileLevel", profile_buf);
	if (status == -ETIMEDOUT)
		goto fail_queues;
	if (status)
		pr_warn("[setup] ProfileLevel \"%s\" rejected (%d), using default\n",
			profile_buf, status);

	status = t2bce_ave_send_set_property_s32(session, "AverageBitRate", params->bitrate);
	if (status) {
		pr_err("[setup] AverageBitRate FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* AverageBitRate alone leaves the AVE in constant-QP; the rate control is
	 * only engaged once ConstantBitRate is set, so send it for VBR (false)
	 * as well as CBR (true). CQ uses Quality instead. */
	if (params->bitrate_mode != V4L2_MPEG_VIDEO_BITRATE_MODE_CQ) {
		bool cbr = params->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;

		status = t2bce_ave_send_set_property_bool(session, "ConstantBitRate", cbr);
		if (status == -ETIMEDOUT)
			goto fail_queues;
		if (status)
			pr_warn("[setup] ConstantBitRate rejected (%d)\n", status);
	}

	if (params->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ) {
		s32 q = clamp(params->quality, 1, 100);

		status = t2bce_ave_send_set_property_float32(session, "Quality",
						       t2bce_ave_quality_to_float32[q]);
		if (status == -ETIMEDOUT)
			goto fail_queues;
		if (status)
			pr_warn("[setup] Quality rejected (%d)\n", status);
	}

	if (params->gop_size > 0) {
		status = t2bce_ave_send_set_property_s32(session, "MaxKeyFrameInterval",
						   params->gop_size);
		if (status == -ETIMEDOUT)
			goto fail_queues;
		if (status)
			pr_warn("[setup] MaxKeyFrameInterval rejected (%d)\n", status);
	}

	if (params->min_qp_set) {
		status = t2bce_ave_send_set_property_s32(session, "MinAllowedFrameQP",
						   params->min_qp);
		if (status == -ETIMEDOUT)
			goto fail_queues;
		if (status)
			pr_warn("[setup] MinAllowedFrameQP rejected (%d)\n", status);
	}

	if (params->max_qp_set) {
		status = t2bce_ave_send_set_property_s32(session, "MaxAllowedFrameQP",
						   params->max_qp);
		if (status == -ETIMEDOUT)
			goto fail_queues;
		if (status)
			pr_warn("[setup] MaxAllowedFrameQP rejected (%d)\n", status);
	}

	t2bce_ave_build_cmd_prepare(session->cmd_buf);
	t2bce_ave_stamp_session_token(session);
	status = t2bce_ave_cmd_send_sync(&session->queues, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("[setup] Prepare FAILED (%d)\n", status);
		goto fail_queues;
	}

	status = t2bce_ave_send_set_property_s32(session, "BPictures", 0);
	if (status) {
		pr_err("[setup] BPictures FAILED (%d)\n", status);
		goto fail_queues;
	}

	property_value = t2bce_ave_color_primaries(params->color_primaries);
	status = property_value ? t2bce_ave_send_set_property_string(session,
			"ColorPrimaries", property_value) : 0;
	if (status) {
		pr_err("[setup] ColorPrimaries FAILED (%d)\n", status);
		goto fail_queues;
	}

	property_value = t2bce_ave_ycbcr_matrix(params->ycbcr_matrix);
	status = property_value ? t2bce_ave_send_set_property_string(session,
			"YCbCrMatrix", property_value) : 0;
	if (status) {
		pr_err("[setup] YCbCrMatrix FAILED (%d)\n", status);
		goto fail_queues;
	}

	property_value = t2bce_ave_transfer_function(params->transfer_func);
	status = property_value ? t2bce_ave_send_set_property_string(session,
			"TransferFunction", property_value) : 0;
	if (status == -ETIMEDOUT)
		goto fail_queues;
	if (status)
		pr_warn("[setup] TransferFunction rejected (%d)\n", status);

	session->state = T2BCE_AVE_STATE_CONFIGURED;
	session->frame_counter = 0;
	pr_debug("=== SESSION SETUP COMPLETE (%ux%u @ %u bps) ===\n",
		 width, height, params->bitrate);
	return 0;

fail_queues:
	pr_debug("[setup] cleaning up after failure...\n");
	if (remote_created) {
		session->state = T2BCE_AVE_STATE_ERROR;
		t2bce_ave_session_teardown(session);
	} else {
		t2bce_ave_queues_destroy(&session->queues);
	}
	session->state = T2BCE_AVE_STATE_IDLE;
	pr_err("=== SESSION SETUP FAILED ===\n");
	return status;
}

void t2bce_ave_session_teardown(struct t2bce_ave_session *session)
{
	int drain_status, end_status;

	if (session->state == T2BCE_AVE_STATE_IDLE || !session->queues.client)
		return;

	pr_debug("=== SESSION TEARDOWN START ===\n");

	t2bce_ave_build_cmd_complete_frames(session->cmd_buf);
	t2bce_ave_stamp_session_token(session);
	drain_status = t2bce_ave_cmd_drain(&session->queues, session->cmd_buf);

	/* Release the remote encoder even if draining failed. */
	t2bce_ave_build_cmd_end_session(session->cmd_buf);
	t2bce_ave_stamp_session_token(session);
	end_status = t2bce_ave_cmd_drain(&session->queues, session->cmd_buf);
	pr_info("teardown: CompleteFrames=%d EndSession=%d\n", drain_status, end_status);
	if (end_status)
		pr_err("teardown: remote session release unconfirmed; AVE service recovery required\n");

	t2bce_ave_queues_destroy(&session->queues);
	kfree(session->hvcc_data);
	session->hvcc_data = NULL;
	kfree(session->annex_b_header);
	session->annex_b_header = NULL;
	session->state = T2BCE_AVE_STATE_IDLE;
	pr_debug("=== SESSION TEARDOWN COMPLETE ===\n");
}

int t2bce_ave_session_set_bitrate(struct t2bce_ave_session *session, u32 bitrate)
{
	int status;

	if (session->state != T2BCE_AVE_STATE_CONFIGURED &&
	    session->state != T2BCE_AVE_STATE_ENCODING)
		return -EINVAL;

	status = t2bce_ave_send_set_property_s32(session, "AverageBitRate", bitrate);
	if (status == -ETIMEDOUT) {
		pr_err("live AverageBitRate update timed out — failing session\n");
		session->state = T2BCE_AVE_STATE_ERROR;
	} else if (status) {
		pr_warn("live AverageBitRate update rejected (%d)\n", status);
	} else {
		session->bitrate = bitrate;
	}
	return status;
}

static int t2bce_ave_extract_hvcc(struct t2bce_ave_session *session, const void *q3_data)
{
	const u8 *hvc = (const u8 *)q3_data + T2BCE_AVE_HVCC_OFFSET;
	const u8 *hvc_end = hvc + T2BCE_AVE_CMD_BUF_SIZE - T2BCE_AVE_HVCC_OFFSET;
	u8 num_arrays, nal_type;
	u16 num_nalus, nal_len;
	const u8 *p;
	u8 *out;
	size_t out_pos = 0;
	int i, j;
	static const u8 start_code[4] = {0x00, 0x00, 0x00, 0x01};

	if (hvc[0] != 1) {
		pr_warn("[hvcc] unexpected version %d, expected 1\n", hvc[0]);
		return -EINVAL;
	}

	num_arrays = hvc[T2BCE_AVE_HVCC_HEADER_SIZE - 1];
	pr_debug("[hvcc] version=%d profile=%d level=%d numArrays=%d\n",
		 hvc[0], hvc[1] & 0x1F, hvc[12], num_arrays);

	p = hvc + T2BCE_AVE_HVCC_HEADER_SIZE;
	for (i = 0; i < num_arrays && i < 8; i++) {
		if (p + 3 > hvc_end)
			break;
		num_nalus = get_unaligned_be16(p + 1);
		p += 3;
		for (j = 0; j < num_nalus && j < 16; j++) {
			if (p + 2 > hvc_end)
				goto size_done;
			nal_len = get_unaligned_be16(p);
			p += 2;
			if (p + nal_len > hvc_end)
				goto size_done;
			out_pos += 4 + nal_len;
			p += nal_len;
		}
	}
size_done:
	if (out_pos == 0) {
		pr_warn("[hvcc] no parameter sets found\n");
		return -EINVAL;
	}

	session->annex_b_header = kmalloc(out_pos, GFP_KERNEL);
	if (!session->annex_b_header)
		return -ENOMEM;
	session->annex_b_header_size = out_pos;

	out = session->annex_b_header;
	out_pos = 0;
	p = hvc + T2BCE_AVE_HVCC_HEADER_SIZE;
	for (i = 0; i < num_arrays && i < 8; i++) {
		if (p + 3 > hvc_end)
			break;
		nal_type = p[0] & 0x3F;
		num_nalus = get_unaligned_be16(p + 1);
		p += 3;
		for (j = 0; j < num_nalus && j < 16; j++) {
			if (p + 2 > hvc_end)
				goto build_done;
			nal_len = get_unaligned_be16(p);
			p += 2;
			if (p + nal_len > hvc_end)
				goto build_done;

			pr_debug("[hvcc] array[%d]: type=%d (%s) len=%d\n",
				 i, nal_type,
				 nal_type == 32 ? "VPS" :
				 nal_type == 33 ? "SPS" :
				 nal_type == 34 ? "PPS" : "?",
				 nal_len);

			memcpy(out + out_pos, start_code, 4);
			out_pos += 4;
			memcpy(out + out_pos, p, nal_len);
			out_pos += nal_len;
			p += nal_len;
		}
	}
build_done:
	session->annex_b_header_size = out_pos;
	session->hvcc_data = kmalloc(T2BCE_AVE_CMD_BUF_SIZE, GFP_KERNEL);
	if (session->hvcc_data) {
		memcpy(session->hvcc_data, q3_data, T2BCE_AVE_CMD_BUF_SIZE);
		session->hvcc_size = T2BCE_AVE_CMD_BUF_SIZE;
	}

	pr_debug("[hvcc] Annex B header built: %zu bytes (VPS+SPS+PPS)\n",
		 session->annex_b_header_size);
	return 0;
}

static bool t2bce_ave_output_has_irap(const void *src, size_t size)
{
	const u8 *in = src;
	size_t pos = 0;

	while (pos + 4 <= size) {
		u32 nal_len = get_unaligned_be32(in + pos);
		u8 nal_type;

		pos += 4;
		if (nal_len == 0 || nal_len > size - pos)
			break;
		nal_type = (in[pos] >> 1) & 0x3f;
		if (nal_type < 32)
			return nal_type >= 16 && nal_type <= 21;
		pos += nal_len;
	}
	return false;
}

static void t2bce_ave_resubmit_q3_bufs(struct t2bce_ave_queues *queues, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (t2bce_ave_submit_q3_buf(queues)) {
			pr_err("[encode] failed to resubmit Q3 buf %zu\n", i);
			break;
		}
	}
}

static void t2bce_ave_encode_fail_cleanup(struct t2bce_ave_session *session,
				    struct t2bce_ave_dma_buffer *y_dma, bool y_mapped,
				    struct t2bce_ave_dma_buffer *uv_dma, bool uv_mapped)
{
	struct t2bce_ave_queues dma_context = {
		.client = session->queues.client,
		.dma_dev = session->queues.dma_dev,
	};

	/* Pending planes must remain mapped while the remote encoder drains.
	 * On timeout, teardown unregisters its queues before we release them.
	 */
	session->state = T2BCE_AVE_STATE_ERROR;
	t2bce_ave_session_teardown(session);
	if (y_mapped)
		t2bce_ave_unmap_frame_data(&dma_context, y_dma);
	if (uv_mapped)
		t2bce_ave_unmap_frame_data(&dma_context, uv_dma);
	session->state = T2BCE_AVE_STATE_ERROR;
}

static int t2bce_ave_submit_encode_frame(struct t2bce_ave_session *session, const char *tag,
				   void *y_data, size_t y_size,
				   void *uv_data, size_t uv_size,
				   struct t2bce_ave_dma_buffer *y_dma, bool *y_mapped,
				   struct t2bce_ave_dma_buffer *uv_dma, bool *uv_mapped)
{
	struct t2bce_ave_queues *q = &session->queues;
	int status;

	status = t2bce_ave_submit_q0_cmd(q, session->cmd_buf,
					 T2BCE_AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("[%s] Q0 EncodeFrame submit failed (%d)\n", tag, status);
		return status;
	}

	status = t2bce_ave_submit_frame_data_async(q, y_data, y_size, y_dma);
	if (status) {
		pr_err("[%s] Y plane submit failed (%d)\n", tag, status);
		return status;
	}
	*y_mapped = true;

	status = t2bce_ave_submit_frame_data_async(q, uv_data, uv_size, uv_dma);
	if (status) {
		pr_err("[%s] UV plane submit failed (%d)\n", tag, status);
		return status;
	}
	*uv_mapped = true;

	status = t2bce_ave_submit_q1_recv(q);
	if (status)
		pr_err("[%s] Q1 recv submit failed (%d)\n", tag, status);
	return status;
}

static int t2bce_ave_encode_first_frame(struct t2bce_ave_session *session,
			      void *y_data, size_t y_size,
			      void *uv_data, size_t uv_size,
			      void *out_buf, size_t out_buf_size,
			      size_t *encoded_size, bool *keyframe_out)
{
	struct t2bce_ave_queues *q = &session->queues;
	struct t2bce_ave_dma_buffer y_dma, uv_dma;
	bool y_mapped = false, uv_mapped = false;
	void *q3_data;
	size_t raw_size, out_pos = 0;
	int status;

	q->callback_auto_resubmit = false;
	t2bce_ave_q3_reset(q);

	status = t2bce_ave_submit_encode_frame(session, "frame1", y_data, y_size,
					 uv_data, uv_size,
					 &y_dma, &y_mapped, &uv_dma, &uv_mapped);
	if (status)
		goto fail;

	pr_debug("[frame1] waiting for Q3 #1 (hvcC)...\n");
	status = t2bce_ave_wait_q3(q, 1, T2BCE_AVE_RESPONSE_TIMEOUT_MS);
	if (status) {
		pr_err("[frame1] Q3 #1 (hvcC) TIMEOUT\n");
		goto fail;
	}
	pr_debug("[frame1] Q3 #1 (hvcC) received, result=0x%zx\n",
		t2bce_ave_q3_completed_size(q, 0));

	q3_data = t2bce_ave_q3_completed_data(q, 0);
	status = t2bce_ave_extract_hvcc(session, q3_data);
	if (status)
		pr_warn("[frame1] hvcC extraction failed (%d), continuing without header\n",
			status);

	status = t2bce_ave_submit_q2_echo(q, q3_data);
	if (status)
		pr_warn("[frame1] Q2 echo (hvcC) failed (%d)\n", status);

	pr_debug("[frame1] waiting for Q3 #2 (metadata)...\n");
	status = t2bce_ave_wait_q3(q, 2, T2BCE_AVE_FRAME_TIMEOUT_MS);
	if (status) {
		pr_err("[frame1] Q3 #2 (metadata) TIMEOUT\n");
		goto fail;
	}
	pr_debug("[frame1] Q3 #2 (metadata) received\n");

	pr_debug("[frame1] waiting for Q3 #3 (output)...\n");
	status = t2bce_ave_wait_q3(q, 3, T2BCE_AVE_FRAME_TIMEOUT_MS);
	if (status) {
		pr_err("[frame1] Q3 #3 (output) TIMEOUT\n");
		goto fail;
	}

	raw_size = t2bce_ave_q3_completed_size(q, 2);
	pr_debug("[frame1] Q3 #3 (output) received, encoded_size=%zu\n", raw_size);

	if (raw_size > T2BCE_AVE_MAX_ENCODED_SIZE) {
		pr_err("[frame1] encoded size %zu exceeds buffer size %d\n",
		       raw_size, T2BCE_AVE_MAX_ENCODED_SIZE);
		status = -ENOSPC;
		goto fail;
	}

	q3_data = t2bce_ave_q3_completed_data(q, 1);
	status = t2bce_ave_submit_q2_echo(q, q3_data);
	if (status)
		pr_warn("[frame1] Q2 echo (metadata) failed (%d)\n", status);

	pr_debug("[frame1] waiting for Q1...\n");
	status = t2bce_ave_wait_q1(q, T2BCE_AVE_FRAME_TIMEOUT_MS);
	if (status) {
		pr_err("[frame1] Q1 TIMEOUT or error (%d)\n", status);
		goto fail;
	}
	pr_debug("[frame1] Q1 done\n");

	t2bce_ave_unmap_frame_data(q, &y_dma);
	y_mapped = false;
	t2bce_ave_unmap_frame_data(q, &uv_dma);
	uv_mapped = false;

	*keyframe_out = true;

	if (raw_size == 0) {
		pr_warn("[frame1] T2 returned 0 bytes\n");
		*encoded_size = 0;
		goto resubmit;
	}

	if (session->annex_b_header && session->annex_b_header_size > 0) {
		if (session->annex_b_header_size > out_buf_size) {
			status = -ENOSPC;
			goto fail;
		}
		memcpy(out_buf, session->annex_b_header, session->annex_b_header_size);
		out_pos = session->annex_b_header_size;
	}

	q3_data = t2bce_ave_q3_completed_data(q, 2);
	status = t2bce_ave_convert_to_annex_b(q3_data, raw_size,
					(u8 *)out_buf + out_pos,
					out_buf_size - out_pos, encoded_size);
	if (status) {
		pr_err("[frame1] Annex B conversion failed (%d)\n", status);
		goto fail;
	}
	*encoded_size += out_pos;

resubmit:
	t2bce_ave_resubmit_q3_bufs(q, t2bce_ave_q3_event_count(q));
	return 0;

fail:
	t2bce_ave_encode_fail_cleanup(session, &y_dma, y_mapped, &uv_dma, uv_mapped);
	return status ? status : -EIO;
}

static int t2bce_ave_encode_next_frame(struct t2bce_ave_session *session,
			      void *y_data, size_t y_size,
			      void *uv_data, size_t uv_size,
			      void *out_buf, size_t out_buf_size,
			      size_t *encoded_size, bool *keyframe_out)
{
	struct t2bce_ave_queues *q = &session->queues;
	struct t2bce_ave_dma_buffer y_dma, uv_dma;
	bool y_mapped = false, uv_mapped = false;
	void *q3_data;
	size_t raw_size, out_pos = 0;
	bool is_irap;
	int status;

	q->callback_auto_resubmit = false;
	t2bce_ave_q3_reset(q);

	status = t2bce_ave_submit_encode_frame(session, "frameN", y_data, y_size,
					 uv_data, uv_size,
					 &y_dma, &y_mapped, &uv_dma, &uv_mapped);
	if (status)
		goto fail;

	pr_debug("[frameN] waiting for Q1 EncodeFrame accept...\n");
	status = t2bce_ave_wait_q1(q, T2BCE_AVE_FRAME_TIMEOUT_MS);
	if (status) {
		pr_err("[frameN] Q1 EncodeFrame accept TIMEOUT or error (%d)\n", status);
		goto fail;
	}
	pr_debug("[frameN] EncodeFrame accepted\n");

	t2bce_ave_unmap_frame_data(q, &y_dma);
	y_mapped = false;
	t2bce_ave_unmap_frame_data(q, &uv_dma);
	uv_mapped = false;

	t2bce_ave_build_cmd_complete_frames(session->cmd_buf);
	t2bce_ave_stamp_session_token(session);

	status = t2bce_ave_submit_q0_cmd(q, session->cmd_buf, T2BCE_AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("[frameN] Q0 CompleteFrames submit failed (%d)\n", status);
		goto fail;
	}

	status = t2bce_ave_submit_q1_recv(q);
	if (status) {
		pr_err("[frameN] Q1 recv for CompleteFrames failed (%d)\n", status);
		goto fail;
	}


	pr_debug("[frameN] waiting for Q3 #1 (metadata)...\n");
	status = t2bce_ave_wait_q3(q, 1, T2BCE_AVE_FRAME_TIMEOUT_MS);
	if (status) {
		pr_err("[frameN] Q3 #1 (metadata) TIMEOUT\n");
		goto fail;
	}
	pr_debug("[frameN] Q3 #1 (metadata) received\n");

	pr_debug("[frameN] waiting for Q3 #2 (output)...\n");
	status = t2bce_ave_wait_q3(q, 2, T2BCE_AVE_FRAME_TIMEOUT_MS);
	if (status) {
		pr_err("[frameN] Q3 #2 (output) TIMEOUT\n");
		goto fail;
	}

	raw_size = t2bce_ave_q3_completed_size(q, 1);
	pr_debug("[frameN] Q3 #2 (output) received, encoded_size=%zu\n", raw_size);

	if (raw_size > T2BCE_AVE_MAX_ENCODED_SIZE) {
		pr_err("[frameN] encoded size %zu exceeds buffer size %d\n",
		       raw_size, T2BCE_AVE_MAX_ENCODED_SIZE);
		status = -ENOSPC;
		goto fail;
	}

	q3_data = t2bce_ave_q3_completed_data(q, 0);
	status = t2bce_ave_submit_q2_echo(q, q3_data);
	if (status)
		pr_warn("[frameN] Q2 echo (metadata) failed (%d)\n", status);

	pr_debug("[frameN] waiting for Q1 CompleteFrames...\n");
	status = t2bce_ave_wait_q1(q, T2BCE_AVE_FRAME_TIMEOUT_MS);
	if (status) {
		pr_err("[frameN] Q1 CompleteFrames TIMEOUT or error (%d)\n", status);
		goto fail;
	}
	pr_debug("[frameN] CompleteFrames done\n");

	if (raw_size == 0) {
		pr_warn("[frameN] T2 returned 0 bytes\n");
		*encoded_size = 0;
		goto resubmit;
	}

	q3_data = t2bce_ave_q3_completed_data(q, 1);
	is_irap = t2bce_ave_output_has_irap(q3_data, raw_size);
	*keyframe_out = is_irap;

	if (is_irap && session->annex_b_header && session->annex_b_header_size > 0) {
		if (session->annex_b_header_size > out_buf_size) {
			status = -ENOSPC;
			goto fail;
		}
		memcpy(out_buf, session->annex_b_header, session->annex_b_header_size);
		out_pos = session->annex_b_header_size;
	}

	status = t2bce_ave_convert_to_annex_b(q3_data, raw_size,
					(u8 *)out_buf + out_pos,
					out_buf_size - out_pos, encoded_size);
	if (status) {
		pr_err("[frameN] Annex B conversion failed (%d)\n", status);
		goto fail;
	}
	*encoded_size += out_pos;

resubmit:
	t2bce_ave_resubmit_q3_bufs(q, t2bce_ave_q3_event_count(q));
	return 0;

fail:
	t2bce_ave_encode_fail_cleanup(session, &y_dma, y_mapped, &uv_dma, uv_mapped);
	return status ? status : -EIO;
}

int t2bce_ave_session_encode_frame(struct t2bce_ave_session *session,
			     void *y_data, size_t y_size,
			     void *uv_data, size_t uv_size,
			     void *out_buf, size_t out_buf_size,
			     size_t *encoded_size, bool force_keyframe,
			     bool *keyframe_out)
{
	int status;
	bool is_first, req_keyframe;

	if (session->state != T2BCE_AVE_STATE_CONFIGURED &&
	    session->state != T2BCE_AVE_STATE_ENCODING)
		return -EINVAL;

	session->state = T2BCE_AVE_STATE_ENCODING;
	session->frame_counter++;
	is_first = (session->frame_counter == 1);
	req_keyframe = force_keyframe || is_first;
	*keyframe_out = false;

	if (is_first) {
		pr_debug("[encode] first frame — pre-submitting Q2/Q3 buffers\n");
		t2bce_ave_presubmit_recv_bufs(&session->queues);
	}

	pr_debug("=== ENCODE FRAME %llu (%s) ===\n",
		session->frame_counter, is_first ? "Frame1" : "FrameN");

	t2bce_ave_build_cmd_encode_frame(session->cmd_buf, session->frame_counter,
				   session->width, session->height,
				   session->input_stride, session->input_pixfmt,
				   session->fps_num, session->fps_den,
				   req_keyframe,
				   session->frame_counter);
	t2bce_ave_stamp_session_token(session);

	if (is_first)
		status = t2bce_ave_encode_first_frame(session, y_data, y_size,
					    uv_data, uv_size,
					    out_buf, out_buf_size, encoded_size,
					    keyframe_out);
	else
		status = t2bce_ave_encode_next_frame(session, y_data, y_size,
					    uv_data, uv_size,
					    out_buf, out_buf_size, encoded_size,
					    keyframe_out);

	if (status) {
		pr_err("=== FRAME %llu FAILED (%d) ===\n",
		       session->frame_counter, status);
		return status;
	}

	pr_debug("=== FRAME %llu ENCODED: %zu bytes (%s) ===\n",
		session->frame_counter, *encoded_size,
		*keyframe_out ? "key" : "delta");
	return 0;
}

int t2bce_ave_convert_to_annex_b(const void *src, size_t src_size,
			   void *dst, size_t dst_size, size_t *out_size)
{
	const u8 *in = src;
	u8 *out = dst;
	size_t in_pos = 0, out_pos = 0;
	u32 nal_len;
	int nal_count = 0;
	static const u8 start_code[4] = {0x00, 0x00, 0x00, 0x01};

	pr_debug("[annexb] converting %zu bytes\n", src_size);

	while (in_pos + 4 <= src_size) {
		nal_len = get_unaligned_be32(in + in_pos);
		in_pos += 4;

		if (nal_len == 0 || in_pos + nal_len > src_size) {
			pr_warn("[annexb] invalid NAL length %u at offset %zu (remaining %zu)\n",
				nal_len, in_pos - 4, src_size - in_pos);
			break;
		}

		if (out_pos + 4 + nal_len > dst_size) {
			pr_err("[annexb] output buffer too small\n");
			return -ENOSPC;
		}

		if (nal_len >= 2) {
			u8 nal_type = (in[in_pos] >> 1) & 0x3f;
			pr_debug("[annexb] NAL #%d: type=%u len=%u\n",
				nal_count, nal_type, nal_len);
		}

		memcpy(out + out_pos, start_code, 4);
		out_pos += 4;

		memcpy(out + out_pos, in + in_pos, nal_len);
		out_pos += nal_len;

		in_pos += nal_len;
		nal_count++;
	}

	*out_size = out_pos;
	pr_debug("[annexb] converted %d NALs, %zu -> %zu bytes\n",
		nal_count, src_size, out_pos);
	return 0;
}
