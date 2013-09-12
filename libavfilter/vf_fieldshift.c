/*
 * Copyright (c) 2013 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * Horizontally shift fields against each other.
 */

#include <float.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <assert.h>

#include <stdio.h>

#include "config.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "libavutil/opt.h"



typedef struct {
    const AVClass *class;

    int max_step[4];    ///< max pixel step for each plane, expressed as a number of bytes
    int hsub,vsub;

    int shift_amount;
} FieldShiftContext;



static av_cold int init(AVFilterContext *ctx)
{
    FieldShiftContext *fieldshift = ctx->priv;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FieldShiftContext *fieldshift = ctx->priv;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *pix_fmts = NULL;
    int fmt;

    for (fmt = 0; fmt < AV_PIX_FMT_NB; fmt++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL ||
              desc->flags & AV_PIX_FMT_FLAG_BITSTREAM ||
              (desc->log2_chroma_w != desc->log2_chroma_h &&
               desc->comp[0].plane == desc->comp[1].plane)))
            ff_add_format(&pix_fmts, fmt);
    }

    ff_set_common_formats(ctx, pix_fmts);
    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    FieldShiftContext *fieldshift = inlink->dst->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    av_image_fill_max_pixsteps(s->max_step, NULL, pix_desc);
    fieldshift->hsub  = desc->log2_chroma_w;
    fieldshift->vsub  = desc->log2_chroma_h;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    FieldShiftContext *fieldshift = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];

    AVFrame *out;

    int i, j, plane, step;
    int shift_right = fieldshift->shift_amount/2;
    int shift_left  = fieldshift->shift_amount - shift_right;


    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    /* copy palette if required */
    if (av_pix_fmt_desc_get(inlink->format)->flags & AV_PIX_FMT_FLAG_PAL)
        memcpy(out->data[1], in->data[1], AVPALETTE_SIZE);


    for (plane = 0; plane < 4 && in->data[plane] && in->linesize[plane]; plane++) {
        const int width  = (plane == 1 || plane == 2) ? FF_CEIL_RSHIFT(inlink->w, s->hsub) : inlink->w;
        const int height = (plane == 1 || plane == 2) ? FF_CEIL_RSHIFT(inlink->h, s->vsub) : inlink->h;
        step = s->max_step[plane];

        outrow = out->data[plane];
        inrow  = in ->data[plane] + (width - 1) * step;
        for (i = 0; i < height; i++) {
            int shift = (i&1) ? shift_left : -shift_right;

            switch (step) {
            case 1:
                if (shift>=0) {
                    for (j = 0; j < shift; j++)
                        outrow[j] = inrow[0];
                    for (j = shift; j < width; j++)
                        outrow[j] = inrow[j-shift];
                }
                else {
                    for (j = 0; j < width+shift; j++)
                        outrow[j] = inrow[j-shift];
                    for (j = width+shift; j < width; j++)
                        outrow[j] = inrow[width-1];
                }
            break;
/*
            case 2:
            {
                uint16_t *outrow16 = (uint16_t *)outrow;
                uint16_t * inrow16 = (uint16_t *) inrow;
                for (j = 0; j < width; j++)
                    outrow16[j] = inrow16[-j];
            }
            break;

            case 3:
            {
                uint8_t *in  =  inrow;
                uint8_t *out = outrow;
                for (j = 0; j < width; j++, out += 3, in -= 3) {
                    int32_t v = AV_RB24(in);
                    AV_WB24(out, v);
                }
            }
            break;

            case 4:
            {
                uint32_t *outrow32 = (uint32_t *)outrow;
                uint32_t * inrow32 = (uint32_t *) inrow;
                for (j = 0; j < width; j++)
                    outrow32[j] = inrow32[-j];
            }
            break;

            default:
                for (j = 0; j < width; j++)
                    memcpy(outrow + j*step, inrow - j*step, step);
            }
*/

            inrow  += in ->linesize[plane];
            outrow += out->linesize[plane];
        }
    }


    av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(FieldShiftContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
static const AVOption options[] = {
    { "offset", "shift offset", OFFSET(shift_amount),  AV_OPT_TYPE_INT, { .i64 = 2.0 }, -100, 100, FLAGS },
    { NULL },
};


static const AVClass fieldshift_class = {
    .class_name = "fieldshift",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


static const AVFilterPad avfilter_vf_fieldshift_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
    { NULL }
};


static const AVFilterPad avfilter_vf_fieldshift_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO
    },
    { NULL }
};

AVFilter avfilter_vf_fieldshift = {
    .name          = "fieldshift",
    .description   = NULL_IF_CONFIG_SMALL("Shift fields against each other"),

    .priv_size     = sizeof(FieldShiftContext),
    .priv_class    = &fieldshift_class,
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,

    .inputs    = avfilter_vf_fieldshift_inputs,
    .outputs   = avfilter_vf_fieldshift_outputs,
};
