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

struct ohcodec_priv {
    struct mp_hwdec_ctx hwctx;
    OHNativeWindow *window;
    // OH_NativeImage *image;

    bool (*interop_init)(struct ra_hwdec_mapper *mapper);
    void (*interop_uninit)(struct ra_hwdec_mapper *mapper);

    bool (*interop_map)(struct ra_hwdec_mapper *mapper);
    void (*interop_unmap)(struct ra_hwdec_mapper *mapper);
};

struct ohcodec_mapper_priv {
    struct mp_log *log;
    struct ra_imgfmt_desc desc;

    OHNativeWindowBuffer* buffer;

    // mp_mutex lock;
    // mp_cond cond;
    // bool image_available;

    void *priv;
};

typedef bool (*ohcodec_interop_init)(const struct ra_hwdec *hw);

bool ohcodec_interop_gl_init(const struct ra_hwdec *hw);
bool ohcodec_interop_pl_init(const struct ra_hwdec *hw);
