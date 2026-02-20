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

#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdbool.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_oh.h>
#include <native_window/external_window.h>
#include <native_window/graphic_error_code.h>
#include <native_image/native_image.h>
#include <native_buffer/native_buffer.h>

#include "config.h"

#include "video/out/gpu/hwdec.h"
#include "video/out/ohos_common.h"
#include "video/out/vulkan/context.h"

#if HAVE_GL
#include "video/out/opengl/ra_gl.h"
#endif

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
    OHNativeWindow *window;
    OH_NativeImage *image;
};

struct priv {
    struct mp_log *log;

    OHNativeWindowBuffer* buffer;

    mp_mutex lock;
    mp_cond cond;
    bool image_available;
};

static AVBufferRef *create_ohcodec_device_ref(OHNativeWindow *window)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_OHCODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVOHCodecDeviceContext *hwctx = ctx->hwctx;
    hwctx->native_window = window;

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static int init(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    p->image = OH_NativeImage_Create(-1, GL_TEXTURE_EXTERNAL_OES);
    mp_assert(p->image);

    p->window = OH_NativeImage_AcquireNativeWindow(p->image);
    mp_assert(p->window);

    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = hw->driver->name,
        .av_device_ref = create_ohcodec_device_ref(p->window),
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
    struct priv_owner *p = hw->priv;

    if (p->image) {
        OH_NativeImage_Destroy(&p->image);
        p->window = NULL;
    }

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
}

static void image_callback(void *context)
{
    struct priv *p = context;

    mp_mutex_lock(&p->lock);
    p->image_available = true;
    mp_cond_signal(&p->cond);
    mp_mutex_unlock(&p->lock);
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    p->log = mapper->log;
    mp_mutex_init(&p->lock);
    mp_cond_init(&p->cond);

    OH_OnFrameAvailableListener listener = {
        .context = p,
        .onFrameAvailable = image_callback
    };
    OH_NativeImage_SetOnFrameAvailableListener(o->image, listener);

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = IMGFMT_RGB0;
    mapper->dst_params.hw_subfmt = 0;

    return 0;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    OH_NativeImage_UnsetOnFrameAvailableListener(o->image);
    mp_mutex_destroy(&p->lock);
    mp_cond_destroy(&p->cond);
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    {
        if (mapper->src->imgfmt != IMGFMT_OHCODEC)
            return -1;
        AVFrame *frame = (AVFrame *)mapper->src->planes[3];
        av_frame_unref(frame);
    }

    bool image_available = false;
    mp_mutex_lock(&p->lock);
    if (!p->image_available) {
        mp_cond_timedwait(&p->cond, &p->lock, MP_TIME_MS_TO_NS(100));
    }
    image_available = p->image_available;
    p->image_available = false;
    mp_mutex_unlock(&p->lock);

    int fence_fd = -1;
    int32_t stride = 0;

    int ret = OH_NativeImage_AcquireNativeWindowBuffer(o->image, &p->buffer, &fence_fd);
    if (ret != NATIVE_ERROR_OK) {
        MP_ERR(mapper, "AcquireNativeWindowBuffer failed: %d\n", ret);
        return image_available ? -1 : 0;
    }
    mp_assert(p->buffer);
    OH_NativeWindow_NativeObjectReference(p->buffer);

    OH_NativeBuffer *native_buffer = NULL;
    OH_NativeBuffer_FromNativeWindowBuffer(p->buffer, &native_buffer);
    BufferHandle* handle = OH_NativeWindow_GetBufferHandleFromNative(p->buffer);

    stride = handle->stride;

    void *addr = NULL;
    OH_NativeBuffer_Map(native_buffer, &addr);

    const AVFrame *frame = (AVFrame *)mapper->src->planes[3];
    uint8_t *src = frame->data[0];
    uint8_t *dst = addr;
    for (int i = 0; i < mapper->dst_params.h; i++) {
        memcpy(dst, src, frame->linesize[0] > stride ? stride : frame->linesize[0]);
        src += frame->linesize[0];
        dst += stride;
    }
    OH_NativeBuffer_Unmap(native_buffer);

    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    if (p->buffer) {
        OH_NativeWindow_NativeObjectUnreference(p->buffer);
        OH_NativeImage_ReleaseNativeWindowBuffer(o->image, p->buffer, -1);
        Region region = {};
        OH_NativeWindow_NativeWindowFlushBuffer(o->window, p->buffer, -1, region);
    }
}

const struct ra_hwdec_driver ra_hwdec_ohcodec = {
    .name = "ohcodec",
    .priv_size = sizeof(struct priv_owner),
    .imgfmts = {IMGFMT_OHCODEC, 0},
    .device_type = AV_HWDEVICE_TYPE_OHCODEC,
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};
