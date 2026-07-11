/* HarmonyOS OHCodec Surface interop. */
#pragma once

#include <stdbool.h>

#include <libavcodec/ohcodec_buffer.h>
#include <native_image/native_image.h>

#include "osdep/threads.h"
#include "video/out/gpu/hwdec.h"

struct ohcodec_surface {
    OH_NativeImage *image;
    OHNativeWindow *window;
    mp_mutex lock;
    mp_cond cond;
    unsigned pending;
    bool initialized;
};

struct ohcodec_interop {
    void (*owner_uninit)(struct ra_hwdec *hw);
    bool (*mapper_init)(struct ra_hwdec_mapper *mapper);
    void (*mapper_uninit)(struct ra_hwdec_mapper *mapper);
    int (*map)(struct ra_hwdec_mapper *mapper);
    int (*reuse)(struct ra_hwdec_mapper *mapper);
    void (*unmap)(struct ra_hwdec_mapper *mapper);
    void (*release)(struct ra_hwdec_mapper *mapper);
};

struct ohcodec_priv {
    struct mp_hwdec_ctx hwctx;
    struct ohcodec_surface *surface;
    void *interop_owner_priv;
    const struct ohcodec_interop *interop;
};

struct ohcodec_mapper_priv {
    void *priv;
    AVOHCodecBuffer *last_token;
    struct mp_image *last_src;
};

typedef bool (*ohcodec_interop_init)(struct ra_hwdec *hw);

bool ohcodec_surface_init(struct ohcodec_surface *surface,
                          OH_NativeImage *image);
void ohcodec_surface_destroy(struct ohcodec_surface *surface);
bool ohcodec_surface_wait(struct ohcodec_surface *surface,
                          struct mp_log *log, int64_t *wait_ns);

bool ohcodec_interop_gl_init(struct ra_hwdec *hw);
bool ohcodec_interop_pl_init(struct ra_hwdec *hw);
