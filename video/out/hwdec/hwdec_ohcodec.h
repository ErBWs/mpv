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
#include <native_buffer/native_buffer.h>

#include "video/out/gpu/hwdec.h"
#include "video/mp_image.h"

struct ohcodec_priv {
    struct mp_hwdec_ctx hwctx;

    bool (*interop_init)(struct ra_hwdec_mapper *mapper);
    void (*interop_uninit)(struct ra_hwdec_mapper *mapper);

    bool (*interop_map)(struct ra_hwdec_mapper *mapper);
    void (*interop_unmap)(struct ra_hwdec_mapper *mapper);
};

struct ohcodec_mapper_priv {
    struct mp_log *log;
    struct ra_imgfmt_desc desc;
    struct mp_image layout;
    OH_NativeBuffer *native_buffer;
    void *mapped_addr;
    OH_NativeBuffer_Planes planes;
    uint8_t *upload_buffer;
    size_t upload_buffer_size;

    void *priv;
};

typedef bool (*ohcodec_interop_init)(const struct ra_hwdec *hw);

bool ohcodec_interop_gl_init(const struct ra_hwdec *hw);
bool ohcodec_interop_pl_init(const struct ra_hwdec *hw);
bool ohcodec_upload_init(struct ra_hwdec_mapper *mapper);
void ohcodec_upload_uninit(struct ra_hwdec_mapper *mapper);
bool ohcodec_upload_map(struct ra_hwdec_mapper *mapper);
void ohcodec_upload_unmap(struct ra_hwdec_mapper *mapper);
