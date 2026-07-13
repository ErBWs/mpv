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

#pragma once

#include <libplacebo/colorspace.h>
#include <native_buffer/buffer_common.h>
#include <native_window/external_window.h>

#include "common/common.h"

struct vo;
struct mp_image_params;

bool vo_ohos_init(struct vo *vo);
void vo_ohos_uninit(struct vo *vo);
OHNativeWindow *vo_ohos_native_window(struct vo *vo);
bool vo_ohos_surface_size(struct vo *vo, int *w, int *h);
struct pl_color_space vo_ohos_preferred_csp(struct vo *vo);
bool vo_ohos_set_color(struct vo *vo, struct mp_image_params *params);
void vo_ohos_invalidate_color(struct vo *vo);
