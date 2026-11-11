#define pr_fmt(fmt) "apple-ave: " fmt

#include "encoder.h"
#include "t2bce_core_transport.h"
#include "video.h"

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-vmalloc.h>

#define T2BCE_AVE_NAME		"apple-ave"
#define T2BCE_AVE_DEFAULT_WIDTH	1920
#define T2BCE_AVE_DEFAULT_HEIGHT	1080
#define T2BCE_AVE_MIN_WIDTH		128
#define T2BCE_AVE_MIN_HEIGHT		128
#define T2BCE_AVE_MAX_WIDTH		4096
#define T2BCE_AVE_MAX_HEIGHT		2304
#define T2BCE_AVE_DEFAULT_BITRATE	4000000
#define T2BCE_AVE_MIN_BITRATE		100000
#define T2BCE_AVE_MAX_BITRATE		100000000
#define T2BCE_AVE_DEFAULT_FPS_NUM	30
#define T2BCE_AVE_DEFAULT_FPS_DEN	1
#define T2BCE_AVE_XPC_TIMEOUT_MS	2000
#define T2BCE_AVE_STRIDE(w)		ALIGN((w), 64)
#define T2BCE_AVE_P010_STRIDE(w)	ALIGN((w) * 2, 64)
/* Contiguous NV12/P010 places UV immediately after the advertised Y rows. */
#define T2BCE_AVE_NV12_Y_SIZE(w, h) \
	(T2BCE_AVE_STRIDE(w) * (h))
#define T2BCE_AVE_NV12_UV_SIZE(w, h) \
	(T2BCE_AVE_STRIDE(w) * (h) / 2)
#define T2BCE_AVE_NV12_SIZE(w, h) \
	(T2BCE_AVE_NV12_Y_SIZE(w, h) + T2BCE_AVE_NV12_UV_SIZE(w, h))
#define T2BCE_AVE_P010_Y_SIZE(w, h) \
	(T2BCE_AVE_P010_STRIDE(w) * (h))
#define T2BCE_AVE_P010_UV_SIZE(w, h) \
	(T2BCE_AVE_P010_STRIDE(w) * (h) / 2)
#define T2BCE_AVE_P010_SIZE(w, h) \
	(T2BCE_AVE_P010_Y_SIZE(w, h) + T2BCE_AVE_P010_UV_SIZE(w, h))
#define T2BCE_AVE_MIN_CAP_SIZEIMAGE	(256 * 1024)
#define T2BCE_AVE_CAP_SIZEIMAGE(w, h) \
	max_t(u32, T2BCE_AVE_NV12_SIZE(w, h), T2BCE_AVE_MIN_CAP_SIZEIMAGE)


struct t2bce_ave_device {
	struct t2bce_core_client *bce;
	struct device *dma_dev;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex dev_mutex;
	struct mutex session_mutex;
	struct t2bce_ave_session *session;
};

struct t2bce_ave_ctx {
	struct v4l2_fh fh;
	struct t2bce_ave_device *dev;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	struct v4l2_ctrl_handler ctrl_handler;
	u32 bitrate;
	bool bitrate_dirty;
	bool force_keyframe;
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
};

static const struct v4l2_event t2bce_ave_eos_event = {
	.type = V4L2_EVENT_EOS
};

static int t2bce_ave_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq);


static void t2bce_ave_set_default_src_fmt(struct v4l2_pix_format_mplane *f)
{
	memset(f, 0, sizeof(*f));
	f->width = T2BCE_AVE_DEFAULT_WIDTH;
	f->height = T2BCE_AVE_DEFAULT_HEIGHT;
	f->pixelformat = V4L2_PIX_FMT_NV12;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_REC709;
	f->num_planes = 1;
	f->plane_fmt[0].sizeimage = T2BCE_AVE_NV12_SIZE(T2BCE_AVE_DEFAULT_WIDTH, T2BCE_AVE_DEFAULT_HEIGHT);
	f->plane_fmt[0].bytesperline = T2BCE_AVE_STRIDE(T2BCE_AVE_DEFAULT_WIDTH);
}

static void t2bce_ave_set_default_dst_fmt(struct v4l2_pix_format_mplane *f)
{
	memset(f, 0, sizeof(*f));
	f->width = T2BCE_AVE_DEFAULT_WIDTH;
	f->height = T2BCE_AVE_DEFAULT_HEIGHT;
	f->pixelformat = V4L2_PIX_FMT_HEVC;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_REC709;
	f->num_planes = 1;
	f->plane_fmt[0].sizeimage = T2BCE_AVE_CAP_SIZEIMAGE(T2BCE_AVE_DEFAULT_WIDTH, T2BCE_AVE_DEFAULT_HEIGHT);
	f->plane_fmt[0].bytesperline = 0;
}


static int t2bce_ave_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct t2bce_ave_device *adev = video_drvdata(file);

	strscpy(cap->driver, T2BCE_AVE_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple T2 HEVC Encoder", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(adev->dma_dev));
	return 0;
	return 0;
}

static int t2bce_ave_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		switch (f->index) {
		case 0:
			f->pixelformat = V4L2_PIX_FMT_NV12;
			return 0;
		case 1:
			f->pixelformat = V4L2_PIX_FMT_NV12M;
			return 0;
		case 2:
			f->pixelformat = V4L2_PIX_FMT_P010;
			return 0;
		default:
			return -EINVAL;
		}
	} else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		if (f->index != 0)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_HEVC;
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
		return 0;
	}
	return -EINVAL;
}

static int t2bce_ave_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct t2bce_ave_ctx *ctx = container_of(file->private_data, struct t2bce_ave_ctx, fh);

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		f->fmt.pix_mp = ctx->src_fmt;
	else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		f->fmt.pix_mp = ctx->dst_fmt;
	else
		return -EINVAL;
	return 0;
}

static void t2bce_ave_fill_src_fmt(struct v4l2_pix_format_mplane *pix, u32 w, u32 h,
			     u32 format)
{
	pix->width = w;
	pix->height = h;
	pix->field = V4L2_FIELD_NONE;

	if (format == V4L2_PIX_FMT_NV12M) {
		pix->pixelformat = V4L2_PIX_FMT_NV12M;
		pix->num_planes = 2;
		pix->plane_fmt[0].sizeimage = T2BCE_AVE_NV12_Y_SIZE(w, h);
		pix->plane_fmt[0].bytesperline = T2BCE_AVE_STRIDE(w);
		pix->plane_fmt[1].sizeimage = T2BCE_AVE_NV12_UV_SIZE(w, h);
		pix->plane_fmt[1].bytesperline = T2BCE_AVE_STRIDE(w);
	} else if (format == V4L2_PIX_FMT_P010) {
		pix->pixelformat = V4L2_PIX_FMT_P010;
		pix->num_planes = 1;
		pix->plane_fmt[0].sizeimage = T2BCE_AVE_P010_SIZE(w, h);
		pix->plane_fmt[0].bytesperline = T2BCE_AVE_P010_STRIDE(w);
	} else {
		pix->pixelformat = V4L2_PIX_FMT_NV12;
		pix->num_planes = 1;
		pix->plane_fmt[0].sizeimage = T2BCE_AVE_NV12_SIZE(w, h);
		pix->plane_fmt[0].bytesperline = T2BCE_AVE_STRIDE(w);
	}
}

static int t2bce_ave_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct t2bce_ave_ctx *ctx = container_of(file->private_data, struct t2bce_ave_ctx, fh);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	u32 format = pix->pixelformat;
	u32 w, h;

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;

	w = clamp(pix->width, (u32)T2BCE_AVE_MIN_WIDTH, (u32)T2BCE_AVE_MAX_WIDTH);
	h = clamp(pix->height, (u32)T2BCE_AVE_MIN_HEIGHT, (u32)T2BCE_AVE_MAX_HEIGHT);
	w = ALIGN(w, 2);
	h = ALIGN(h, 2);

	if (format != V4L2_PIX_FMT_NV12 && format != V4L2_PIX_FMT_NV12M &&
	    format != V4L2_PIX_FMT_P010)
		format = V4L2_PIX_FMT_NV12;
	t2bce_ave_fill_src_fmt(pix, w, h, format);
	if (!pix->colorspace)
		pix->colorspace = V4L2_COLORSPACE_REC709;

	ctx->src_fmt = *pix;

	ctx->dst_fmt.width = w;
	ctx->dst_fmt.height = h;
	ctx->dst_fmt.colorspace = pix->colorspace;
	ctx->dst_fmt.ycbcr_enc = pix->ycbcr_enc;
	ctx->dst_fmt.xfer_func = pix->xfer_func;
	ctx->dst_fmt.plane_fmt[0].sizeimage = T2BCE_AVE_CAP_SIZEIMAGE(w, h);

	return 0;
}

static int t2bce_ave_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct t2bce_ave_ctx *ctx = container_of(file->private_data, struct t2bce_ave_ctx, fh);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;

	pix->pixelformat = V4L2_PIX_FMT_HEVC;
	pix->width = ctx->src_fmt.width;
	pix->height = ctx->src_fmt.height;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = ctx->src_fmt.colorspace;
	pix->ycbcr_enc = ctx->src_fmt.ycbcr_enc;
	pix->xfer_func = ctx->src_fmt.xfer_func;
	pix->num_planes = 1;
	pix->plane_fmt[0].sizeimage = T2BCE_AVE_CAP_SIZEIMAGE(pix->width, pix->height);
	pix->plane_fmt[0].bytesperline = 0;

	ctx->dst_fmt = *pix;
	return 0;
}

static int t2bce_ave_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	u32 format = pix->pixelformat;
	u32 w, h;

	w = clamp(pix->width, (u32)T2BCE_AVE_MIN_WIDTH, (u32)T2BCE_AVE_MAX_WIDTH);
	h = clamp(pix->height, (u32)T2BCE_AVE_MIN_HEIGHT, (u32)T2BCE_AVE_MAX_HEIGHT);
	w = ALIGN(w, 2);
	h = ALIGN(h, 2);

	if (format != V4L2_PIX_FMT_NV12 && format != V4L2_PIX_FMT_NV12M &&
	    format != V4L2_PIX_FMT_P010)
		format = V4L2_PIX_FMT_NV12;
	t2bce_ave_fill_src_fmt(pix, w, h, format);

	return 0;
}

static int t2bce_ave_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	u32 w, h;

	w = clamp(pix->width, (u32)T2BCE_AVE_MIN_WIDTH, (u32)T2BCE_AVE_MAX_WIDTH);
	h = clamp(pix->height, (u32)T2BCE_AVE_MIN_HEIGHT, (u32)T2BCE_AVE_MAX_HEIGHT);
	w = ALIGN(w, 2);
	h = ALIGN(h, 2);

	pix->width = w;
	pix->height = h;
	pix->pixelformat = V4L2_PIX_FMT_HEVC;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->plane_fmt[0].sizeimage = T2BCE_AVE_CAP_SIZEIMAGE(w, h);
	pix->plane_fmt[0].bytesperline = 0;

	return 0;
}

static int t2bce_ave_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index != 0)
		return -EINVAL;
	if (fsize->pixel_format != V4L2_PIX_FMT_NV12 &&
	    fsize->pixel_format != V4L2_PIX_FMT_NV12M &&
	    fsize->pixel_format != V4L2_PIX_FMT_P010 &&
	    fsize->pixel_format != V4L2_PIX_FMT_HEVC)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = T2BCE_AVE_MIN_WIDTH;
	fsize->stepwise.max_width = T2BCE_AVE_MAX_WIDTH;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.min_height = T2BCE_AVE_MIN_HEIGHT;
	fsize->stepwise.max_height = T2BCE_AVE_MAX_HEIGHT;
	fsize->stepwise.step_height = 2;

	return 0;
}


static s32 t2bce_ave_v4l2_to_t2_primaries(enum v4l2_colorspace cs)
{
	switch (cs) {
	case V4L2_COLORSPACE_SMPTE170M:
	case V4L2_COLORSPACE_470_SYSTEM_BG:
		return 6;
	case V4L2_COLORSPACE_BT2020:
		return 9;
	case V4L2_COLORSPACE_SMPTE240M:
		return 7;
	case V4L2_COLORSPACE_REC709:
	default:
		return 1;
	}
}

static s32 t2bce_ave_v4l2_to_t2_matrix(enum v4l2_colorspace cs,
				  enum v4l2_ycbcr_encoding enc)
{
	if (enc == V4L2_YCBCR_ENC_BT2020)
		return 9;
	if (enc == V4L2_YCBCR_ENC_601)
		return 5;
	if (enc == V4L2_YCBCR_ENC_709 || enc == V4L2_YCBCR_ENC_DEFAULT) {
		switch (cs) {
		case V4L2_COLORSPACE_SMPTE170M:
		case V4L2_COLORSPACE_470_SYSTEM_BG:
			return 5;
		case V4L2_COLORSPACE_BT2020:
			return 9;
		default:
			return 1;
		}
	}
	return 1;
}

static s32 t2bce_ave_v4l2_to_t2_xfer(enum v4l2_colorspace cs,
				enum v4l2_xfer_func xfer)
{
	if (xfer == V4L2_XFER_FUNC_SMPTE2084)
		return 16;
	if (xfer == V4L2_XFER_FUNC_709 || xfer == V4L2_XFER_FUNC_DEFAULT) {
		if (cs == V4L2_COLORSPACE_BT2020)
			return 14;
		return 1;
	}
	return 1;
}


static int t2bce_ave_g_parm(struct file *file, void *priv, struct v4l2_streamparm *sp)
{
	struct t2bce_ave_ctx *ctx = container_of(file->private_data, struct t2bce_ave_ctx, fh);

	if (sp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	    sp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		memset(&sp->parm, 0, sizeof(sp->parm));
		sp->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
		sp->parm.output.timeperframe.numerator = ctx->fps_den;
		sp->parm.output.timeperframe.denominator = ctx->fps_num;
		return 0;
	}
	if (sp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	    sp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		memset(&sp->parm, 0, sizeof(sp->parm));
		sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		sp->parm.capture.timeperframe.numerator = ctx->fps_den;
		sp->parm.capture.timeperframe.denominator = ctx->fps_num;
		return 0;
	}
	return -EINVAL;
}

static int t2bce_ave_s_parm(struct file *file, void *priv, struct v4l2_streamparm *sp)
{
	struct t2bce_ave_ctx *ctx = container_of(file->private_data, struct t2bce_ave_ctx, fh);
	u32 num = 0, den = 0;

	if (sp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	    sp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		num = sp->parm.output.timeperframe.numerator;
		den = sp->parm.output.timeperframe.denominator;
	} else if (sp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
		   sp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		num = sp->parm.capture.timeperframe.numerator;
		den = sp->parm.capture.timeperframe.denominator;
	} else {
		return -EINVAL;
	}

	if (num && den) {
		ctx->fps_den = num;
		ctx->fps_num = den;
	} else {
		ctx->fps_num = T2BCE_AVE_DEFAULT_FPS_NUM;
		ctx->fps_den = T2BCE_AVE_DEFAULT_FPS_DEN;
	}

	return t2bce_ave_g_parm(file, priv, sp);
}

static int t2bce_ave_encoder_cmd(struct file *file, void *priv,
			   struct v4l2_encoder_cmd *ec)
{
	struct t2bce_ave_ctx *ctx = container_of(file->private_data, struct t2bce_ave_ctx, fh);
	int ret;

	ret = v4l2_m2m_ioctl_encoder_cmd(file, priv, ec);
	if (ret)
		return ret;

	if (ec->cmd == V4L2_ENC_CMD_STOP &&
	    v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &t2bce_ave_eos_event);
	return 0;
}

static int t2bce_ave_subscribe_event(struct v4l2_fh *fh,
			       const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static const struct v4l2_ioctl_ops t2bce_ave_ioctl_ops = {
	.vidioc_querycap		= t2bce_ave_querycap,

	.vidioc_enum_fmt_vid_cap	= t2bce_ave_enum_fmt,
	.vidioc_enum_fmt_vid_out	= t2bce_ave_enum_fmt,

	.vidioc_g_fmt_vid_cap_mplane	= t2bce_ave_g_fmt,
	.vidioc_g_fmt_vid_out_mplane	= t2bce_ave_g_fmt,

	.vidioc_s_fmt_vid_cap_mplane	= t2bce_ave_s_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane	= t2bce_ave_s_fmt_out,

	.vidioc_try_fmt_vid_cap_mplane	= t2bce_ave_try_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane	= t2bce_ave_try_fmt_out,

	.vidioc_enum_framesizes		= t2bce_ave_enum_framesizes,

	.vidioc_g_parm			= t2bce_ave_g_parm,
	.vidioc_s_parm			= t2bce_ave_s_parm,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_encoder_cmd		= t2bce_ave_encoder_cmd,
	.vidioc_try_encoder_cmd		= v4l2_m2m_ioctl_try_encoder_cmd,

	.vidioc_subscribe_event		= t2bce_ave_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};


static int t2bce_ave_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct t2bce_ave_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *fmt;
	int i;

	if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = &ctx->src_fmt;
	else
		fmt = &ctx->dst_fmt;

	if (*nplanes) {
		if (*nplanes != fmt->num_planes)
			return -EINVAL;
		for (i = 0; i < fmt->num_planes; i++)
			if (sizes[i] < fmt->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}

	*nplanes = fmt->num_planes;
	for (i = 0; i < fmt->num_planes; i++)
		sizes[i] = fmt->plane_fmt[i].sizeimage;

	return 0;
}

static int t2bce_ave_buf_prepare(struct vb2_buffer *vb)
{
	struct t2bce_ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *fmt;
	int i;

	if (vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = &ctx->src_fmt;
	else
		fmt = &ctx->dst_fmt;

	for (i = 0; i < fmt->num_planes; i++) {
		if (vb2_plane_size(vb, i) < fmt->plane_fmt[i].sizeimage)
			return -EINVAL;
	}

	if (vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		for (i = 0; i < fmt->num_planes; i++)
			vb2_set_plane_payload(vb, i, fmt->plane_fmt[i].sizeimage);
	}

	return 0;
}

static void t2bce_ave_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct t2bce_ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void t2bce_ave_build_params(struct t2bce_ave_ctx *ctx, struct t2bce_ave_enc_params *p)
{
	enum v4l2_colorspace cs = ctx->src_fmt.colorspace;

	p->bitrate = ctx->bitrate;
	p->fps_num = ctx->fps_num;
	p->fps_den = ctx->fps_den;
	p->gop_size = ctx->gop_size;
	p->bitrate_mode = ctx->bitrate_mode;
	p->quality = ctx->quality;
	p->min_qp = ctx->min_qp;
	p->max_qp = ctx->max_qp;
	p->min_qp_set = ctx->min_qp_set;
	p->max_qp_set = ctx->max_qp_set;
	p->profile = ctx->profile;
	p->level = ctx->level;
	p->color_primaries = t2bce_ave_v4l2_to_t2_primaries(cs);
	p->ycbcr_matrix = t2bce_ave_v4l2_to_t2_matrix(cs, ctx->src_fmt.ycbcr_enc);
	p->transfer_func = t2bce_ave_v4l2_to_t2_xfer(cs, ctx->src_fmt.xfer_func);
	p->input_pixelformat = ctx->src_fmt.pixelformat;
	p->input_stride = ctx->src_fmt.plane_fmt[0].bytesperline;
	p->profile = ctx->src_fmt.pixelformat == V4L2_PIX_FMT_P010 ?
		V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10 :
		V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
}

static int t2bce_ave_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct t2bce_ave_ctx *ctx = vb2_get_drv_priv(vq);
	struct t2bce_ave_device *adev = ctx->dev;
	struct t2bce_ave_enc_params params;
	struct vb2_v4l2_buffer *vbuf;
	int status = 0;

	if (vq->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, vq);
		return 0;
	}

	t2bce_ave_build_params(ctx, &params);

	mutex_lock(&adev->session_mutex);
	if (adev->session) {
		mutex_unlock(&adev->session_mutex);
		pr_err("encoder session already active\n");
		status = -EBUSY;
		goto return_bufs;
	}

	adev->session = kzalloc(sizeof(struct t2bce_ave_session), GFP_KERNEL);
	if (!adev->session) {
		mutex_unlock(&adev->session_mutex);
		status = -ENOMEM;
		goto return_bufs;
	}

	status = t2bce_ave_session_setup(adev->session, adev->bce,
				   ctx->src_fmt.width, ctx->src_fmt.height,
				   &params);
	if (status) {
		pr_err("session setup failed (%d)\n", status);
		kfree(adev->session);
		adev->session = NULL;
		mutex_unlock(&adev->session_mutex);
		goto return_bufs;
	}
	mutex_unlock(&adev->session_mutex);

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, vq);

	return 0;

return_bufs:
	while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_QUEUED);
	return status;
}

static bool t2bce_ave_shutdown_session(struct t2bce_ave_device *adev)
{
	bool had_session = false;

	mutex_lock(&adev->session_mutex);
	if (adev->session) {
		t2bce_ave_session_teardown(adev->session);
		kfree(adev->session);
		adev->session = NULL;
		had_session = true;
	}
	mutex_unlock(&adev->session_mutex);
	return had_session;
}

static void t2bce_ave_stop_streaming(struct vb2_queue *vq)
{
	struct t2bce_ave_ctx *ctx = vb2_get_drv_priv(vq);
	struct t2bce_ave_device *adev = ctx->dev;
	struct vb2_v4l2_buffer *vbuf;

	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, vq);

	if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		t2bce_ave_shutdown_session(adev);

		while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	} else {
		while ((vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops t2bce_ave_vb2_ops = {
	.queue_setup		= t2bce_ave_queue_setup,
	.buf_prepare		= t2bce_ave_buf_prepare,
	.buf_queue		= t2bce_ave_buf_queue,
	.start_streaming	= t2bce_ave_start_streaming,
	.stop_streaming		= t2bce_ave_stop_streaming,
};


static void t2bce_ave_device_run(void *priv)
{
	struct t2bce_ave_ctx *ctx = priv;
	struct t2bce_ave_device *adev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct vb2_buffer *src_vb, *dst_vb;
	enum vb2_buffer_state state = VB2_BUF_STATE_ERROR;
	void *y_data, *uv_data, *out_data;
	size_t y_size, uv_size, encoded_size = 0;
	bool keyframe = false;
	int status;

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	if (!src_buf || !dst_buf) {
		pr_err("device_run called with missing buffers\n");
		v4l2_m2m_job_finish(adev->m2m_dev, ctx->fh.m2m_ctx);
		return;
	}

	src_vb = &src_buf->vb2_buf;
	dst_vb = &dst_buf->vb2_buf;
	vb2_set_plane_payload(dst_vb, 0, 0);

	if (ctx->src_fmt.num_planes == 1) {
		y_data = vb2_plane_vaddr(src_vb, 0);
		if (ctx->src_fmt.pixelformat == V4L2_PIX_FMT_P010) {
			y_size = T2BCE_AVE_P010_Y_SIZE(ctx->src_fmt.width,
						       ctx->src_fmt.height);
			uv_size = T2BCE_AVE_P010_UV_SIZE(ctx->src_fmt.width,
							ctx->src_fmt.height);
		} else {
			y_size = T2BCE_AVE_NV12_Y_SIZE(ctx->src_fmt.width,
						       ctx->src_fmt.height);
			uv_size = T2BCE_AVE_NV12_UV_SIZE(ctx->src_fmt.width,
							ctx->src_fmt.height);
		}
		uv_data = y_data ? y_data + y_size : NULL;
	} else {
		y_data = vb2_plane_vaddr(src_vb, 0);
		y_size = vb2_get_plane_payload(src_vb, 0);
		uv_data = vb2_plane_vaddr(src_vb, 1);
		uv_size = vb2_get_plane_payload(src_vb, 1);
	}
	out_data = vb2_plane_vaddr(dst_vb, 0);

	if (!y_data || !uv_data || !out_data) {
		pr_err("failed to get buffer vaddrs\n");
		goto done;
	}

	mutex_lock(&adev->session_mutex);
	if (!adev->session || adev->session->state == T2BCE_AVE_STATE_ERROR) {
		mutex_unlock(&adev->session_mutex);
		pr_debug("no active session\n");
		goto done;
	}

	if (READ_ONCE(ctx->bitrate_dirty)) {
		WRITE_ONCE(ctx->bitrate_dirty, false);
		t2bce_ave_session_set_bitrate(adev->session, ctx->bitrate);
	}

	status = t2bce_ave_session_encode_frame(adev->session,
					  y_data, y_size, uv_data, uv_size,
					  out_data, vb2_plane_size(dst_vb, 0),
					  &encoded_size, ctx->force_keyframe,
					  &keyframe);
	mutex_unlock(&adev->session_mutex);

	if (status) {
		pr_err("encode failed (%d)\n", status);
		goto done;
	}

	ctx->force_keyframe = false;

	vb2_set_plane_payload(dst_vb, 0, encoded_size);
	v4l2_m2m_buf_copy_metadata(src_buf, dst_buf);
	dst_buf->sequence = src_buf->sequence;
	dst_buf->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
			    V4L2_BUF_FLAG_BFRAME);
	dst_buf->flags |= keyframe ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
	state = VB2_BUF_STATE_DONE;

done:
	if (v4l2_m2m_is_last_draining_src_buf(ctx->fh.m2m_ctx, src_buf)) {
		dst_buf->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
		v4l2_event_queue_fh(&ctx->fh, &t2bce_ave_eos_event);
	}
	v4l2_m2m_buf_done_and_job_finish(adev->m2m_dev, ctx->fh.m2m_ctx, state);
}

static void t2bce_ave_job_abort(void *priv)
{
}

static const struct v4l2_m2m_ops t2bce_ave_m2m_ops = {
	.device_run	= t2bce_ave_device_run,
	.job_abort	= t2bce_ave_job_abort,
};


static int t2bce_ave_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct t2bce_ave_ctx *ctx = container_of(ctrl->handler, struct t2bce_ave_ctx, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		ctx->bitrate = ctrl->val;
		WRITE_ONCE(ctx->bitrate_dirty, true);
		return 0;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		ctx->force_keyframe = true;
		return 0;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctx->gop_size = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
		ctx->bitrate_mode = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY:
		ctx->quality = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP:
		ctx->min_qp = ctrl->val;
		ctx->min_qp_set = true;
		return 0;
	case V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP:
		ctx->max_qp = ctrl->val;
		ctx->max_qp_set = true;
		return 0;
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		ctx->profile = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_HEVC_LEVEL:
		ctx->level = ctrl->val;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops t2bce_ave_ctrl_ops = {
	.s_ctrl = t2bce_ave_s_ctrl,
};

static int t2bce_ave_init_ctrls(struct t2bce_ave_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->ctrl_handler;

	v4l2_ctrl_handler_init(hdl, 10);

	v4l2_ctrl_new_std(hdl, &t2bce_ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_BITRATE,
			  T2BCE_AVE_MIN_BITRATE, T2BCE_AVE_MAX_BITRATE, 1,
			  T2BCE_AVE_DEFAULT_BITRATE);

	v4l2_ctrl_new_std(hdl, &t2bce_ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
			  0, 0, 0, 0);

	v4l2_ctrl_new_std(hdl, &t2bce_ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_GOP_SIZE,
			  0, 600, 1, 0);

	v4l2_ctrl_new_std_menu(hdl, &t2bce_ave_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CQ, 0,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);

	v4l2_ctrl_new_std(hdl, &t2bce_ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,
			  1, 100, 1, 65);

	v4l2_ctrl_new_std(hdl, &t2bce_ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
			  0, 51, 1, 0);

	v4l2_ctrl_new_std(hdl, &t2bce_ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
			  0, 51, 1, 0);

	v4l2_ctrl_new_std_menu(hdl, &t2bce_ave_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10, 0,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);

	v4l2_ctrl_new_std_menu(hdl, &t2bce_ave_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2, 0,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1);

	if (hdl->error)
		return hdl->error;

	ctx->fh.ctrl_handler = hdl;
	return 0;
}


static int t2bce_ave_open(struct file *file)
{
	struct t2bce_ave_device *adev = video_drvdata(file);
	struct t2bce_ave_ctx *ctx;
	int status;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = adev;
	ctx->bitrate = T2BCE_AVE_DEFAULT_BITRATE;
	ctx->fps_num = T2BCE_AVE_DEFAULT_FPS_NUM;
	ctx->fps_den = T2BCE_AVE_DEFAULT_FPS_DEN;
	ctx->gop_size = 0;
	ctx->bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
	ctx->quality = 65;
	ctx->profile = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
	ctx->level = V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1;
	v4l2_fh_init(&ctx->fh, &adev->vdev);

	status = t2bce_ave_init_ctrls(ctx);
	if (status)
		goto err_fh;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(adev->m2m_dev, ctx, t2bce_ave_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		status = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrl;
	}

	t2bce_ave_set_default_src_fmt(&ctx->src_fmt);
	t2bce_ave_set_default_dst_fmt(&ctx->dst_fmt);

	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh, file);

	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return status;
}

static int t2bce_ave_release(struct file *file)
{
	struct t2bce_ave_ctx *ctx = container_of(file->private_data, struct t2bce_ave_ctx, fh);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations t2bce_ave_fops = {
	.owner		= THIS_MODULE,
	.open		= t2bce_ave_open,
	.release	= t2bce_ave_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};


static int t2bce_ave_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct t2bce_ave_ctx *ctx = priv;
	int status;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &t2bce_ave_vb2_ops;
	src_vq->mem_ops = &vb2_vmalloc_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;
	src_vq->dev = ctx->dev->dma_dev;

	status = vb2_queue_init(src_vq);
	if (status)
		return status;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &t2bce_ave_vb2_ops;
	dst_vq->mem_ops = &vb2_vmalloc_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = ctx->dev->dma_dev;

	return vb2_queue_init(dst_vq);
}

int t2bce_ave_video_create(struct t2bce_core_client *bce,
			   struct t2bce_ave_device **result)
{
	struct t2bce_ave_device *adev;
	int status;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->bce = bce;
	adev->dma_dev = t2bce_core_client_dma_dev(bce);
	mutex_init(&adev->dev_mutex);
	mutex_init(&adev->session_mutex);

	strscpy(adev->v4l2_dev.name, "t2bce-ave",
		sizeof(adev->v4l2_dev.name));
	status = v4l2_device_register(NULL, &adev->v4l2_dev);
	if (status) {
		pr_err("v4l2_device_register failed (%d)\n", status);
		goto err_free;
	}

	adev->m2m_dev = v4l2_m2m_init(&t2bce_ave_m2m_ops);
	if (IS_ERR(adev->m2m_dev)) {
		status = PTR_ERR(adev->m2m_dev);
		pr_err("v4l2_m2m_init failed (%d)\n", status);
		goto err_v4l2;
	}

	adev->vdev.fops = &t2bce_ave_fops;
	adev->vdev.ioctl_ops = &t2bce_ave_ioctl_ops;
	adev->vdev.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	adev->vdev.v4l2_dev = &adev->v4l2_dev;
	adev->vdev.release = video_device_release_empty;
	adev->vdev.vfl_dir = VFL_DIR_M2M;
	adev->vdev.lock = &adev->dev_mutex;
	strscpy(adev->vdev.name, "apple-ave-enc", sizeof(adev->vdev.name));

	video_set_drvdata(&adev->vdev, adev);

	status = video_register_device(&adev->vdev, VFL_TYPE_VIDEO, -1);
	if (status) {
		pr_err("video_register_device failed (%d)\n", status);
		goto err_m2m;
	}

	*result = adev;
	pr_info("HEVC encoder registered as /dev/video%d\n",
		adev->vdev.num);
	return 0;

err_m2m:
	v4l2_m2m_release(adev->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&adev->v4l2_dev);
err_free:
	kfree(adev);
	return status;
}

void t2bce_ave_video_suspend(struct t2bce_ave_device *adev)
{
	if (!adev)
		return;

	if (t2bce_ave_shutdown_session(adev))
		pr_info("session torn down for suspend; streaming must be restarted after resume\n");
}

void t2bce_ave_video_destroy(struct t2bce_ave_device *adev)
{
	if (!adev)
		return;

	video_unregister_device(&adev->vdev);
	v4l2_m2m_release(adev->m2m_dev);
	v4l2_device_unregister(&adev->v4l2_dev);

	t2bce_ave_shutdown_session(adev);

	kfree(adev);
	pr_info("encoder unregistered\n");
}
