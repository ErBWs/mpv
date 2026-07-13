/* HarmonyOS decoder Surface -> GPU zero-copy bridge. */

#include <errno.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_oh.h>
#include <native_window/graphic_error_code.h>

#include "config.h"
#include "hwdec_ohcodec.h"
#include "osdep/timer.h"
#include "video/out/gpu/hwdec.h"

static const ohcodec_interop_init interop_inits[] = {
#if HAVE_VULKAN
    ohcodec_interop_pl_init,
#endif
#if HAVE_GL
    ohcodec_interop_gl_init,
#endif
    NULL,
};

static void on_frame_available(void *opaque)
{
    struct ohcodec_surface *surface = opaque;
    // NativeImage APIs are deliberately not called from this callback.
    mp_mutex_lock(&surface->lock);
    surface->pending++;
    mp_cond_signal(&surface->cond);
    mp_mutex_unlock(&surface->lock);
}

bool ohcodec_surface_init(struct ohcodec_surface *surface,
                          OH_NativeImage *image)
{
    *surface = (struct ohcodec_surface){ .image = image };
    if (!image || mp_mutex_init(&surface->lock))
        return false;
    if (mp_cond_init(&surface->cond)) {
        mp_mutex_destroy(&surface->lock);
        return false;
    }

    surface->window = OH_NativeImage_AcquireNativeWindow(image);
    if (!surface->window)
        goto fail;

    OH_OnFrameAvailableListener listener = {
        .context = surface,
        .onFrameAvailable = on_frame_available,
    };
    if (OH_NativeImage_SetOnFrameAvailableListener(image, listener) !=
        NATIVE_ERROR_OK)
        goto fail;

    surface->initialized = true;
    return true;

fail:
    mp_cond_destroy(&surface->cond);
    mp_mutex_destroy(&surface->lock);
    surface->image = NULL;
    surface->window = NULL;
    return false;
}

void ohcodec_surface_destroy(struct ohcodec_surface *surface)
{
    if (!surface || !surface->initialized)
        return;
    OH_NativeImage_UnsetOnFrameAvailableListener(surface->image);
    mp_cond_destroy(&surface->cond);
    mp_mutex_destroy(&surface->lock);
    surface->initialized = false;
    surface->window = NULL;
}

bool ohcodec_surface_wait(struct ohcodec_surface *surface,
                          struct mp_log *log, int64_t *wait_ns)
{
    int64_t start = mp_time_ns();
    bool ready;

    mp_mutex_lock(&surface->lock);
    while (!surface->pending) {
        if (mp_cond_timedwait(&surface->cond, &surface->lock,
                              MP_TIME_MS_TO_NS(250)))
            break;
    }
    ready = surface->pending != 0;
    if (ready)
        surface->pending--;
    mp_mutex_unlock(&surface->lock);

    *wait_ns = mp_time_ns() - start;
    if (!ready)
        mp_msg(log, MSGL_TRACE, "OHCodec Surface frame callback timed out\n");
    else
        mp_msg(log, MSGL_TRACE, "OHCodec render->callback wait %.3f ms\n",
               *wait_ns / 1e6);
    return ready;
}

static AVBufferRef *create_device_ref(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_OHCODEC);
    if (!ref)
        return NULL;

    AVHWDeviceContext *device = (AVHWDeviceContext *)ref->data;
    AVOHCodecDeviceContext *oh = device->hwctx;
    oh->native_window = p->surface->window;
    if (!oh->native_window || av_hwdevice_ctx_init(ref) < 0)
        av_buffer_unref(&ref);
    return ref;
}

static int init(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;

    for (int i = 0; interop_inits[i] && !p->surface; i++)
        interop_inits[i](hw);

    if (!p->surface || !p->interop) {
        MP_VERBOSE(hw, "OHCodec Surface hwdec requires compatible GL or Vulkan.\n");
        return -1;
    }

    p->hwctx = (struct mp_hwdec_ctx) {
        .driver_name = hw->driver->name,
        .av_device_ref = create_device_ref(hw),
        .hw_imgfmt = IMGFMT_OHCODEC,
    };
    if (!p->hwctx.av_device_ref) {
        p->interop->owner_uninit(hw);
        return -1;
    }
    hwdec_devices_add(hw->devs, &p->hwctx);
    return 0;
}

static void uninit(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;
    hwdec_devices_remove(hw->devs, &p->hwctx);
    // Decoder and device disappear before listener/NativeImage/GPU objects.
    av_buffer_unref(&p->hwctx.av_device_ref);
    p->interop->owner_uninit(hw);
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_priv *owner = mapper->owner->priv;
    return owner->interop->mapper_init(mapper) ? 0 : -1;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct ohcodec_priv *owner = mapper->owner->priv;
    if (p->last_token && owner->interop->release)
        owner->interop->release(mapper);
    mp_image_unrefp(&p->last_src);
    p->last_token = NULL;
    owner->interop->mapper_uninit(mapper);
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_priv *owner = mapper->owner->priv;
    AVOHCodecBuffer *token = mapper->src && mapper->src->imgfmt == IMGFMT_OHCODEC
                           ? (AVOHCodecBuffer *)mapper->src->planes[3] : NULL;
    struct ohcodec_mapper_priv *p = mapper->priv;
    int64_t begin = mp_time_ns(), wait_ns = 0;

    if (!token) {
        MP_ERR(mapper, "Missing OHCodec output token\n");
        return -1;
    }

    // libplacebo acquires the same pl_frame again on every presentation. A
    // decoder output token is one-shot, so repeated acquisition must reuse the
    // retained Surface image instead of rendering the token a second time.
    if (token == p->last_token) {
        int ret = owner->interop->reuse(mapper);
        if (ret != NATIVE_ERROR_OK) {
            MP_ERR(mapper, "Failed to reacquire retained OHCodec surface: %d\n",
                   ret);
            return -1;
        }
        mp_image_unrefp(&mapper->src);
        return 0;
    }

    int render_ret = av_ohcodec_release_buffer(token, 1);
    if (render_ret < 0) {
        // Flush/EOF can invalidate a frame after it entered libplacebo's
        // queue. It produced no Surface buffer, so keep presenting the last
        // valid image instead of waiting for a callback that cannot arrive.
        if ((render_ret == AVERROR(ESTALE) ||
             render_ret == AVERROR(EALREADY)) && p->last_token &&
            owner->interop->reuse(mapper) == NATIVE_ERROR_OK)
        {
            MP_VERBOSE(mapper, "Reusing last OHCodec Surface after stale "
                               "output token at drain/flush\n");
            mp_image_unrefp(&mapper->src);
            return 0;
        }
        MP_ERR(mapper, "Failed to render OHCodec output token\n");
        return -1;
    }

    int map_ret = NATIVE_ERROR_NO_BUFFER;
    for (int attempts = 0; attempts < 32 && map_ret == NATIVE_ERROR_NO_BUFFER;
         attempts++) {
        int64_t current_wait = 0;
        // NativeImage consumption is only valid after its frame-available
        // callback. If a delayed callback no longer has a matching buffer,
        // discard that notification and wait for the current rendered frame.
        if (!ohcodec_surface_wait(owner->surface, mapper->log, &current_wait))
            break;
        wait_ns += current_wait;
        map_ret = owner->interop->map(mapper);
        if (map_ret == NATIVE_ERROR_NO_BUFFER)
            MP_TRACE(mapper, "Ignoring stale OHCodec Surface callback\n");
    }
    if (map_ret != NATIVE_ERROR_OK) {
        if (map_ret == NATIVE_ERROR_NO_BUFFER)
            MP_ERR(mapper, "Timed out waiting for rendered OHCodec Surface buffer\n");
        else
            MP_ERR(mapper, "OHCodec Surface interop failed: %d\n", map_ret);
        return -1;
    }

    // The backend replaced the retained image successfully. Keep the new
    // token address alive so repeated presentations can reuse it.
    mp_image_unrefp(&p->last_src);
    p->last_token = token;
    p->last_src = mapper->src;
    mapper->src = NULL;
    MP_TRACE(mapper, "OHCodec hwdec-map %.3f ms (wait %.3f ms)\n",
             (mp_time_ns() - begin) / 1e6, wait_ns / 1e6);
    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_priv *owner = mapper->owner->priv;
    if (owner->interop->unmap)
        owner->interop->unmap(mapper);
}

const struct ra_hwdec_driver ra_hwdec_ohcodec = {
    .name = "ohcodec",
    .priv_size = sizeof(struct ohcodec_priv),
    .imgfmts = { IMGFMT_OHCODEC, 0 },
    .device_type = AV_HWDEVICE_TYPE_OHCODEC,
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver) {
        .priv_size = sizeof(struct ohcodec_mapper_priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};
