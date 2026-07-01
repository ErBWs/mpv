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
    int output_mode;
    bool release_src_after_map;
    void *interop_owner_priv;
    void *(*interop_get_native_window)(struct ra_hwdec *hw);
    void (*interop_owner_uninit)(struct ra_hwdec *hw);

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

typedef bool (*ohcodec_interop_init)(struct ra_hwdec *hw);

bool ohcodec_interop_gl_init(struct ra_hwdec *hw);
bool ohcodec_interop_pl_init(struct ra_hwdec *hw);
AVOHCodecDeviceContext *ohcodec_mapper_device_hwctx(struct ra_hwdec_mapper *mapper);
const AVOHCodecFrameDescriptor *ohcodec_mapper_frame_desc(struct ra_hwdec_mapper *mapper);
OH_NativeBuffer *ohcodec_get_native_buffer(struct ra_hwdec_mapper *mapper,
                                           const AVOHCodecFrameDescriptor *desc);
