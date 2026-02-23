/*
 *
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

#pragma once

#include <stdbool.h>
#include <native_window/external_window.h>
#include <native_image/native_image.h>

#include "video/out/gpu/hwdec.h"
#include "video/out/opengl/gl_headers.h"

struct ohcodec_priv {
    struct mp_hwdec_ctx hwctx;
    OHNativeWindow *window;
    OH_NativeImage *image;

    bool (*ext_init)(struct ra_hwdec_mapper *mapper);
    void (*ext_uninit)(struct ra_hwdec_mapper *mapper);
    void (*ext_map)(struct ra_hwdec_mapper *mapper);

    // These are only necessary if the gpu api requires synchronisation
    bool (*ext_wait)(const struct ra_hwdec_mapper *mapper, int n);
    bool (*ext_signal)(const struct ra_hwdec_mapper *mapper, int n);
};

struct ohcodec_mapper_priv {
    struct mp_log *log;

    // OpenGL
    GLuint gl_texture;

    OHNativeWindowBuffer* buffer;

    mp_mutex lock;
    mp_cond cond;
    bool image_available;
};

struct ohcodec_interop_fn {
    bool (*check)(const struct ra_hwdec *hw);
    void (*init)(const struct ra_hwdec *hw);
};

extern struct ohcodec_interop_fn ohcodec_gl_fn;

extern struct ohcodec_interop_fn ohcodec_vk_fn;
