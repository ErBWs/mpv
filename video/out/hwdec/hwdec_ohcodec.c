/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_oh.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <native_window/graphic_error_code.h>

#include "config.h"
#include "hwdec_ohcodec.h"

#include "video/out/gpu/hwdec.h"

static const ohcodec_interop_init interop_inits[] = {
#if HAVE_VULKAN
    ohcodec_interop_pl_init,
#endif
#if HAVE_GL
    ohcodec_interop_gl_init,
#endif
    NULL
};

static AVBufferRef *create_ohcodec_device_ref(struct ra_hwdec *hw)
{
   struct ohcodec_priv *p = hw->priv;
   AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_OHCODEC);
   if (!device_ref)
       return NULL;

   AVHWDeviceContext *ctx = (void *)device_ref->data;
   AVOHCodecDeviceContext *hwctx = ctx->hwctx;
   hwctx->output_mode = p->output_mode;

   if (hwctx->output_mode == AV_OHCODEC_OUTPUT_MODE_SURFACE) {
       hwctx->native_window = p->interop_get_native_window
           ? p->interop_get_native_window(hw)
           : NULL;
       hwctx->native_window_owned = 0;
       if (!hwctx->native_window) {
           av_buffer_unref(&device_ref);
           return NULL;
       }
   }

   if (av_hwdevice_ctx_init(device_ref) < 0)
       av_buffer_unref(&device_ref);

   return device_ref;
}

AVOHCodecDeviceContext *ohcodec_mapper_device_hwctx(struct ra_hwdec_mapper *mapper)
{
   if (!mapper->src || !mapper->src->hwctx)
       return NULL;

    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)mapper->src->hwctx->data;
    if (!frames_ctx || !frames_ctx->device_ref)
        return NULL;

    AVHWDeviceContext *device_ctx = (AVHWDeviceContext *)frames_ctx->device_ref->data;
    if (!device_ctx || device_ctx->type != AV_HWDEVICE_TYPE_OHCODEC)
        return NULL;

    return device_ctx->hwctx;
}

const AVOHCodecFrameDescriptor *ohcodec_mapper_frame_desc(struct ra_hwdec_mapper *mapper)
{
    if (!mapper->src || mapper->src->imgfmt != IMGFMT_OHCODEC)
        return NULL;

    return (const AVOHCodecFrameDescriptor *)mapper->src->planes[3];
}

OH_NativeBuffer *ohcodec_get_native_buffer(struct ra_hwdec_mapper *mapper,
                                           const AVOHCodecFrameDescriptor *desc)
{
    OH_NativeBuffer *native_buffer =
        OH_AVBuffer_GetNativeBuffer((OH_AVBuffer *)desc->buffer);
    if (!native_buffer)
        MP_ERR(mapper, "Failed to get OH_NativeBuffer from OH_AVBuffer\n");
    return native_buffer;
}

static void release_upload_buffer(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;

    if (p->mapped_addr && p->native_buffer)
        OH_NativeBuffer_Unmap(p->native_buffer);

    if (p->native_buffer)
        OH_NativeBuffer_Unreference(p->native_buffer);

    p->native_buffer = NULL;
    p->mapped_addr = NULL;
    p->planes = (OH_NativeBuffer_Planes){0};
}

struct plane_upload_layout {
    const uint8_t *src;
    ptrdiff_t stride;
};

static bool get_plane_upload_layout(struct ra_hwdec_mapper *mapper, int n,
                                    struct plane_upload_layout *out)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    const OH_NativeBuffer_Plane *plane = &p->planes.planes[n];
    const uint8_t *base = (uint8_t *)p->mapped_addr + plane->offset;
    int pixel_size = mapper->tex[n]->params.format->pixel_size;
    int plane_w = mp_image_plane_w(&p->layout, n);
    int plane_h = mp_image_plane_h(&p->layout, n);
    uint32_t row_stride = plane->rowStride;
    uint32_t column_stride = plane->columnStride;
    int packed_row_bytes = plane_w * pixel_size;

    if (row_stride < (uint32_t)packed_row_bytes &&
        column_stride >= (uint32_t)packed_row_bytes)
    {
        MP_VERBOSE(mapper,
                   "OHCodec plane %d appears to report swapped strides (%u, %u)\n",
                   n, row_stride, column_stride);
        MPSWAP(uint32_t, row_stride, column_stride);
    }

    if (column_stride && column_stride < (uint32_t)pixel_size &&
        pixel_size % column_stride == 0)
    {
        uint32_t scale = pixel_size / column_stride;
        row_stride *= scale;
        column_stride *= scale;
        MP_VERBOSE(mapper,
                   "Scaled OHCodec plane %d strides by %u to bytes (%u, %u)\n",
                   n, scale, row_stride, column_stride);
    }

    if (!column_stride)
        column_stride = pixel_size;

    if (row_stride < (uint32_t)pixel_size || column_stride < (uint32_t)pixel_size) {
        MP_ERR(mapper, "Invalid OHCodec plane %d strides row=%u column=%u\n",
               n, row_stride, column_stride);
        return false;
    }

    if ((int)column_stride == pixel_size) {
        *out = (struct plane_upload_layout){
            .src = base,
            .stride = row_stride,
        };
        return true;
    }

    size_t required = (size_t)packed_row_bytes * plane_h;
    if (required > p->upload_buffer_size) {
        p->upload_buffer = talloc_realloc_size(p, p->upload_buffer, required);
        if (!p->upload_buffer) {
            p->upload_buffer_size = 0;
            MP_ERR(mapper, "Failed to allocate OHCodec repack buffer\n");
            return false;
        }
        p->upload_buffer_size = required;
    }

    for (int y = 0; y < plane_h; y++) {
        const uint8_t *src_row = base + (size_t)y * row_stride;
        uint8_t *dst_row = p->upload_buffer + (size_t)y * packed_row_bytes;
        for (int x = 0; x < plane_w; x++) {
            memcpy(dst_row + (size_t)x * pixel_size,
                   src_row + (size_t)x * column_stride, pixel_size);
        }
    }

    MP_VERBOSE(mapper,
               "Repacked OHCodec plane %d with row=%u column=%u pixel=%d\n",
               n, row_stride, column_stride, pixel_size);

    *out = (struct plane_upload_layout){
        .src = p->upload_buffer,
        .stride = packed_row_bytes,
    };
    return true;
}

static int acquire_native_buffer(struct ra_hwdec_mapper *mapper,
                                 const AVOHCodecFrameDescriptor *desc)
{
    struct ohcodec_mapper_priv *p = mapper->priv;

    p->native_buffer = ohcodec_get_native_buffer(mapper, desc);
    if (!p->native_buffer) {
        return -1;
    }

    int ret = OH_NativeBuffer_MapPlanes(p->native_buffer, &p->mapped_addr, &p->planes);
    if (ret != NATIVE_ERROR_OK) {
        MP_ERR(mapper, "OH_NativeBuffer_MapPlanes failed: %d\n", ret);
        release_upload_buffer(mapper);
        return -1;
    }

    if (!p->mapped_addr || p->planes.planeCount < p->layout.num_planes) {
        MP_ERR(mapper, "Mapped OH_NativeBuffer returned %u planes, need %d\n",
               p->planes.planeCount, p->layout.num_planes);
        release_upload_buffer(mapper);
        return -1;
    }

    return 0;
}

static void ohcodec_upload_uninit(struct ra_hwdec_mapper *mapper);

static int init(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;

    for (int i = 0; interop_inits[i]; i++) {
        if (interop_inits[i](hw))
            break;
    }

    if (!p->interop_init || !p->interop_map || !p->interop_uninit ||
        !p->interop_unmap)
    {
        MP_VERBOSE(hw, "OHCodec buffer hwdec only works with GL or Vulkan backends.\n");
        return -1;
    }

    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = hw->driver->name,
        .av_device_ref = create_ohcodec_device_ref(hw),
        .hw_imgfmt = IMGFMT_OHCODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(hw, "Failed to create hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(hw->devs, &p->hwctx);

    return 0;
}

static void uninit(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    if (p->interop_owner_uninit)
        p->interop_owner_uninit(hw);
}

static bool ohcodec_upload_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = mapper->src_params.hw_subfmt;
    mapper->dst_params.hw_subfmt = 0;

    if (!mapper->dst_params.imgfmt) {
        MP_ERR(mapper, "OHCodec hwdec requires a software sub-format.\n");
        return false;
    }

    mp_image_set_params(&p->layout, &mapper->dst_params);
    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &p->desc)) {
        MP_ERR(mapper, "Unsupported texture format: %s\n",
               mp_imgfmt_to_name(mapper->dst_params.imgfmt));
        return false;
    }

    for (int n = 0; n < p->layout.num_planes; n++) {
        if (!p->desc.planes[n]) {
            MP_ERR(mapper, "Missing texture format for OHCodec plane %d\n", n);
            return false;
        }

        struct ra_tex_params params = {
            .dimensions = 2,
            .w = mp_image_plane_w(&p->layout, n),
            .h = mp_image_plane_h(&p->layout, n),
            .d = 1,
            .format = p->desc.planes[n],
            .render_src = true,
            .host_mutable = true,
            .src_linear = p->desc.planes[n]->linear_filter,
        };

        mapper->tex[n] = ra_tex_create(mapper->ra, &params);
        if (!mapper->tex[n]) {
            MP_ERR(mapper, "Failed to create upload texture for OHCodec plane %d\n", n);
            ohcodec_upload_uninit(mapper);
            return false;
        }
    }

    return true;
}

static void ohcodec_upload_uninit(struct ra_hwdec_mapper *mapper)
{
    for (int n = 0; n < MP_MAX_PLANES; n++)
        ra_tex_free(mapper->ra, &mapper->tex[n]);
}

static bool ohcodec_upload_map(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    const AVOHCodecFrameDescriptor *desc = ohcodec_mapper_frame_desc(mapper);
    if (!desc)
        return false;

    if (acquire_native_buffer(mapper, desc) < 0)
        return false;

    for (int n = 0; n < p->layout.num_planes; n++) {
        struct plane_upload_layout layout;
        if (!get_plane_upload_layout(mapper, n, &layout)) {
            release_upload_buffer(mapper);
            return false;
        }

        struct ra_tex_upload_params params = {
            .tex = mapper->tex[n],
            .src = layout.src,
            .invalidate = true,
            .stride = layout.stride,
        };

        if (!params.src || !params.stride) {
            MP_ERR(mapper, "Missing mapped data for OHCodec plane %d\n", n);
            release_upload_buffer(mapper);
            return false;
        }

        if (!mapper->ra->fns->tex_upload(mapper->ra, &params)) {
            MP_ERR(mapper, "Failed to upload OHCodec plane %d\n", n);
            release_upload_buffer(mapper);
            return false;
        }
    }

    release_upload_buffer(mapper);
    return true;
}

static void ohcodec_upload_unmap(struct ra_hwdec_mapper *mapper)
{
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct ohcodec_priv *o = mapper->owner->priv;
    p->log = mapper->log;

    if (!o->interop_init(mapper))
        return -1;

    return 0;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_priv *o = mapper->owner->priv;

    o->interop_uninit(mapper);
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_priv *o = mapper->owner->priv;
    const AVOHCodecFrameDescriptor *desc = ohcodec_mapper_frame_desc(mapper);
    AVOHCodecDeviceContext *device = ohcodec_mapper_device_hwctx(mapper);

    if (!desc) {
        MP_ERR(mapper, "Missing OHCodec frame descriptor\n");
        return -1;
    }

    if (device && desc->generation != device->output_generation) {
        MP_WARN(mapper, "Discarding stale OHCodec frame after decoder reset/flush\n");
        return -1;
    }

    if (!o->interop_map(mapper)) {
        return -1;
    }

    if (o->release_src_after_map)
        mp_image_unrefp(&mapper->src);

    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_priv *o = mapper->owner->priv;

    o->interop_unmap(mapper);
}

const struct ra_hwdec_driver ra_hwdec_ohcodec = {
    .name = "ohcodec",
    .priv_size = sizeof(struct ohcodec_priv),
    .imgfmts = {IMGFMT_OHCODEC, 0},
    .device_type = AV_HWDEVICE_TYPE_OHCODEC,
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct ohcodec_mapper_priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};
