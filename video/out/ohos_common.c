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

#include "ohos_common.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "vo.h"

struct vo_ohos_state {
    struct mp_log *log;
    OHNativeWindow *native_window;
};

bool vo_ohos_init(struct vo *vo)
{
    vo->ohos = talloc_zero(vo, struct vo_ohos_state);
    struct vo_ohos_state *ctx = vo->ohos;

    *ctx = (struct vo_ohos_state){
        .log = mp_log_new(ctx, vo->log, "ohos"),
    };

    if (vo->opts->WinID == 0 || vo->opts->WinID == -1) {
        MP_FATAL(ctx, "Missing surface pointer\n");
        goto fail;
    }

    uint64_t surface = 0;
    // uint64 -> int64 -> uint64 evil hack.
    memcpy(&surface, &vo->opts->WinID, sizeof(vo->opts->WinID));
    OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface, &ctx->native_window);
    if (!ctx->native_window) {
        MP_FATAL(ctx, "Failed to create OHNativeWindow\n");
        goto fail;
    }

    return true;
fail:
    talloc_free(ctx);
    vo->ohos = NULL;
    return false;
}

void vo_ohos_uninit(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;
    if (!ctx)
        return;

    if (ctx->native_window)
        OH_NativeWindow_DestroyNativeWindow(ctx->native_window);

    talloc_free(ctx);
    vo->ohos = NULL;
}

OHNativeWindow *vo_ohos_native_window(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;
    return ctx->native_window;
}

bool vo_ohos_surface_size(struct vo *vo, int *out_w, int *out_h)
{
    struct vo_ohos_state *ctx = vo->ohos;

    int w = vo->opts->ohos_surface_size.w,
        h = vo->opts->ohos_surface_size.h;
    if (!w || !h)
        OH_NativeWindow_NativeWindowHandleOpt(ctx->native_window, GET_BUFFER_GEOMETRY, &h, &w);

    if (w <= 0 || h <= 0) {
        MP_ERR(ctx, "Failed to get height and width.\n");
        return false;
    }
    *out_w = w;
    *out_h = h;
    return true;
}
