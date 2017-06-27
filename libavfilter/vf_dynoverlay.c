
/*
 * Copyright (c) 2016 Jason Priebe
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * dynamic overlay generator
 */

#include <unistd.h>
#include <sys/stat.h>

#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "lavfutils.h"
#include "lswsutils.h"
#include "internal.h"
#include "video.h"

#include "libswscale/swscale.h"

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/file.h"
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/random_seed.h"
#include "libavutil/parseutils.h"
#include "libavutil/timecode.h"
#include "libavutil/time_internal.h"
#include "libavutil/tree.h"
#include "libavutil/lfg.h"

typedef struct DynOverlayContext {
	const AVClass *class;
	char *overlayfile;
	int check_interval;
	uint64_t ts_last_check;

	time_t ts_last_update;
	AVFrame *overlay_frame;

	int main_pix_step[4]; ///< steps per pixel for each plane of the main output
	int overlay_pix_step[4]; ///< steps per pixel for each plane of the overlay
	int hsub, vsub; ///< chroma subsampling values
} DynOverlayContext;

#define OFFSET(x) offsetof(DynOverlayContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption dynoverlay_options[]= {
{"overlayfile", "set overlay file", OFFSET(overlayfile), AV_OPT_TYPE_STRING, {.str=NULL}, CHAR_MIN, CHAR_MAX, FLAGS},
{"check_interval", "interval (in ms) between checks for updated overlay file", OFFSET(check_interval), AV_OPT_TYPE_INT, {.i64=250}, INT_MIN, INT_MAX , FLAGS},
{ NULL }
};

AVFILTER_DEFINE_CLASS(dynoverlay);

static unsigned long long get_current_time_ms (void)
{
	unsigned long long ms_since_epoch;
	struct timespec spec;

	clock_gettime(CLOCK_REALTIME, &spec);

	ms_since_epoch = (uint64_t)(spec.tv_sec) * 1000 + (uint64_t)(spec.tv_nsec) / 1.0e6;


	return ms_since_epoch;
}

static int load_overlay (AVFilterContext *fctx)
{
	DynOverlayContext *ctx = fctx->priv;

	AVFrame *rgba_frame;

	struct stat attrib;
	int ret;

	if ((ret = stat(ctx->overlayfile, &attrib)) != 0)
	{
		return ret;
	}
	ctx->ts_last_update = attrib.st_mtime;

	if (ctx->overlay_frame)
	{
		// TODO - make sure we have freed everything
		av_freep(ctx->overlay_frame->data);
		av_frame_free (&ctx->overlay_frame);
	}
	
	av_log (fctx, AV_LOG_ERROR, "Loading new overlay image.\n");


	rgba_frame = av_frame_alloc();
	if ((ret = ff_load_image(rgba_frame->data, rgba_frame->linesize,&rgba_frame->width, &rgba_frame->height,&rgba_frame->format, ctx->overlayfile, ctx)) < 0)
	{
		av_frame_free (&rgba_frame);
		av_log (fctx, AV_LOG_ERROR, "Image Load Failed.\n");
		return ret;
	}

	if (rgba_frame->format != AV_PIX_FMT_RGBA)
	{
		av_frame_free (&rgba_frame);
		return 1;
	}

	ctx->overlay_frame = av_frame_alloc();

	ctx->overlay_frame->format = AV_PIX_FMT_YUVA420P;
	ctx->overlay_frame->width = rgba_frame->width;
	ctx->overlay_frame->height = rgba_frame->height;


	if (av_frame_get_buffer (ctx->overlay_frame, 32) < 0) 
	{
		av_frame_free (&ctx->overlay_frame);
		av_frame_free (&rgba_frame);
		return 1;
	}

	if ((ret = ff_scale_image(ctx->overlay_frame->data, ctx->overlay_frame->linesize,rgba_frame->width, rgba_frame->height, ctx->overlay_frame->format,rgba_frame->data, rgba_frame->linesize,rgba_frame->width, rgba_frame->height, rgba_frame->format, ctx)) < 0)
	{
		av_frame_free (&ctx->overlay_frame);
		av_frame_free (&rgba_frame);
		return ret;
	}

	av_frame_free (&rgba_frame);

	return 0;
}


static int clear_overlay (AVFilterContext *fctx)
{
	DynOverlayContext *ctx = fctx->priv;

	ctx->ts_last_update = 0;
	
	av_frame_free (&ctx->overlay_frame);

	return 0;
}

/**
 * Looks for a newly created/updated/deleted overlay file
 */
static int check_overlay (AVFilterContext *fctx)
{
	DynOverlayContext *ctx = fctx->priv;

	struct stat attrib;
	unsigned long long now;

	now = get_current_time_ms ();


	if( ctx->ts_last_check == 0 )
	{
		ctx->ts_last_check = get_current_time_ms ();
	}

	if ((int)(now - ctx->ts_last_check) < ctx->check_interval)
	{
		return 0;
	}

	ctx->ts_last_check = get_current_time_ms ();

	if (ctx->ts_last_update == 0)
	{
		// last time we checked, file did not exist (or this is our first time through)
		if( access( ctx->overlayfile, F_OK ) == -1 ) 
		{
			// file still doesn't exist, so bail out
			return 0;
		}

		load_overlay (fctx);
		return 0;
	}

	// if we fall through to this line, then last time we checked, the file *did* exist,
	// so now we look to see if it has been deleted or updated
	if( access( ctx->overlayfile, F_OK ) != 0 ) 
	{
		// file gone
		clear_overlay (fctx);
		return 0;
	}

	// file is still there, so get its mod time
	if (stat(ctx->overlayfile, &attrib) != 0)
	{
		// Whaa? file was there just a few lines ago, but now we can't stat it...
		return 1;
	}

	if (attrib.st_mtime > ctx->ts_last_update)
	{
		load_overlay (fctx);
	}



	return 0;
}

// divide by 255 and round to nearest
// apply a fast variant: (X+127)/255 = ((X+127)*257+257)>>16 = ((X+128)*257)>>16
#define FAST_DIV255(x) ((((x) + 128) * 257) >> 16)

// calculate the unpremultiplied alpha, applying the general equation:
// alpha = alpha_overlay / ( (alpha_main + alpha_overlay) - (alpha_main * alpha_overlay) )
// (((x) << 16) - ((x) << 9) + (x)) is a faster version of: 255 * 255 * x
// ((((x) + (y)) << 8) - ((x) + (y)) - (y) * (x)) is a faster version of: 255 * (x + y)
#define UNPREMULTIPLY_ALPHA(x, y) ((((x) << 16) - ((x) << 9) + (x)) / ((((x) + (y)) << 8) - ((x) + (y)) - (y) * (x)))

static void blend_image(AVFilterContext *fctx,AVFrame *dst, const AVFrame *src)
{
	DynOverlayContext *ctx = fctx->priv;

	int i, j, jmax, k, kmax;
	const int src_w = src->width;
	const int src_h = src->height;
	const int dst_w = dst->width;
	const int dst_h = dst->height;

 	for (i = 0; i < 3; i++)
	{
		int hsub = i ? ctx->hsub : 0;
		int vsub = i ? ctx->vsub : 0;
		int src_wp = AV_CEIL_RSHIFT(src_w, hsub);
		int src_hp = AV_CEIL_RSHIFT(src_h, vsub);
		int dst_wp = AV_CEIL_RSHIFT(dst_w, hsub);
		int dst_hp = AV_CEIL_RSHIFT(dst_h, vsub);
		uint8_t *s, *sp, *d, *dp, *a, *ap;

		j = 0;
		sp = src->data[i] + j * src->linesize[i];
		dp = dst->data[i] + j * dst->linesize[i];
		ap = src->data[3] + (j<<vsub) * src->linesize[3];

		for (jmax = FFMIN(dst_hp, src_hp); j < jmax; j++)
		{
			k = 0;
			d = dp + k;
			s = sp + k;
			a = ap + (k<<hsub);

			for (kmax = FFMIN(dst_wp, src_wp); k < kmax; k++) 
			{
				int alpha_v, alpha_h, alpha;

				// average alpha for color components, improve quality
				if (hsub && vsub && j+1 < src_hp && k+1 < src_wp)
				{
					alpha = (a[0] + a[src->linesize[3]] +
					a[1] + a[src->linesize[3]+1]) >> 2;
				}
				else if (hsub || vsub)
				{
					alpha_h = hsub && k+1 < src_wp ?
					(a[0] + a[1]) >> 1 : a[0];
					alpha_v = vsub && j+1 < src_hp ?
					(a[0] + a[src->linesize[3]]) >> 1 : a[0];
					alpha = (alpha_v + alpha_h) >> 1;
				}
				else
				{
					alpha = a[0];
				}

				*d = FAST_DIV255(*d * (255 - alpha) + *s * alpha);
				s++;
				d++;
				a += 1 << hsub;
			}
			dp += dst->linesize[i];
			sp += src->linesize[i];
			ap += (1 << vsub) * src->linesize[3];
		}
	}
}

static int config_input(AVFilterLink *link)
{
	DynOverlayContext *ctx = link->dst->priv;

	const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(link->format);

	av_image_fill_max_pixsteps(ctx->main_pix_step, NULL, pix_desc);

	ctx->hsub = pix_desc->log2_chroma_w;
	ctx->vsub = pix_desc->log2_chroma_h;

	return 0;
}

static AVFrame *get_video_buffer(AVFilterLink *link, int w, int h)
{
	AVFrame *frame;

	frame = ff_get_video_buffer(link->dst->outputs[0], w, h);
	if (!frame)
		return NULL;

	return frame;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *inframe)
{

	DynOverlayContext *ctx = inlink->dst->priv;
	AVFilterLink *outlink = inlink->dst->outputs[0];
	AVFrame *outframe;
	int direct = 0;

	check_overlay (inlink->dst);

	if (!ctx->overlay_frame)
	{
		// if we don't have an overlay, pass the frame along with no modifications...
		return ff_filter_frame(inlink->dst->outputs[0], inframe);
	}

	if (av_frame_is_writable (inframe))
	{
		direct = 1;
		outframe = inframe;
	}
	else
	{
		outframe = ff_get_video_buffer (outlink, outlink->w, outlink->h);
		if (!outframe)
		{
			av_frame_free (&inframe);
			return AVERROR (ENOMEM);
		}
		av_frame_copy_props (outframe, inframe);
	}

	blend_image (inlink->dst, outframe, ctx->overlay_frame);

	if (!direct)
	{
		av_frame_free(&inframe);
	}

	return ff_filter_frame(outlink, outframe);
}

static const AVFilterPad avfilter_vf_dynoverlay_inputs[] = {
 {
	.name = "default",
	.type = AVMEDIA_TYPE_VIDEO,
	.get_video_buffer = get_video_buffer,
	.filter_frame = filter_frame,
	.config_props = config_input,
	},
	{ NULL }
};

static const AVFilterPad avfilter_vf_dynoverlay_outputs[] =
{
	{
		.name = "default",
		.type = AVMEDIA_TYPE_VIDEO,
	},
	{ NULL }
};


static av_cold int init(AVFilterContext *fctx)
{
	DynOverlayContext *ctx = fctx->priv;
	av_log(fctx, AV_LOG_ERROR, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! LOADING OVERLAY PLUGIN......\n");
	
	if (!ctx->overlayfile) 
	{
		av_log(fctx, AV_LOG_ERROR, "No overlay filename provided\n");
		return AVERROR(EINVAL);
	}

	ctx->ts_last_check = 0;
	ctx->ts_last_update = 0;

	return 0;
}

static av_cold void uninit(AVFilterContext *fctx)
{
	DynOverlayContext *ctx = fctx->priv;

	av_frame_free (&ctx->overlay_frame);
	
	return;
}

static int query_formats(AVFilterContext *ctx)
{
	static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
	AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
	if (!fmts_list)
		return AVERROR(ENOMEM);
	return ff_set_common_formats(ctx, fmts_list);
}

AVFilter ff_vf_dynoverlay = {
	.name = "dynoverlay",
	.description = NULL_IF_CONFIG_SMALL("Adds a dynamic PNG overlay to live streams."),
	.priv_size = sizeof(DynOverlayContext),
	.priv_class = &dynoverlay_class,
	.query_formats = query_formats,
	.init = init,
	.uninit = uninit,
	.inputs = avfilter_vf_dynoverlay_inputs,
	.outputs = avfilter_vf_dynoverlay_outputs,
};




