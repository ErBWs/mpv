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
#include "video/mp_image.h"
#include "vo.h"

struct vo_ohos_state {
    struct mp_log *log;
    OHNativeWindow *native_window;
    bool color_space_valid;
    OH_NativeBuffer_ColorSpace color_space;
    bool metadata_type_valid;
    OH_NativeBuffer_MetadataType metadata_type;
    bool static_metadata_valid;
    OH_NativeBuffer_StaticMetadata static_metadata;
    bool hdr_brightness_valid;
    float hdr_brightness;
};

static bool is_ohos_hdr(const struct pl_color_space *color)
{
    return color->transfer == PL_COLOR_TRC_PQ ||
           color->transfer == PL_COLOR_TRC_HLG;
}

static OH_NativeBuffer_ColorSpace get_ohos_color_space(
    const struct pl_color_space *color)
{
    if (color->transfer == PL_COLOR_TRC_PQ)
        return OH_COLORSPACE_DISPLAY_BT2020_PQ;
    if (color->transfer == PL_COLOR_TRC_HLG)
        return OH_COLORSPACE_DISPLAY_BT2020_HLG;
    return OH_COLORSPACE_DISPLAY_SRGB;
}

static OH_NativeBuffer_StaticMetadata get_ohos_static_metadata(
    const struct pl_color_space *color)
{
    struct pl_raw_primaries prim = color->hdr.prim;
    if (!pl_primaries_valid(&prim))
        prim = *pl_raw_primaries_get(color->primaries);

    return (OH_NativeBuffer_StaticMetadata){
        .smpte2086 = {
            .displayPrimaryRed = {prim.red.x, prim.red.y},
            .displayPrimaryGreen = {prim.green.x, prim.green.y},
            .displayPrimaryBlue = {prim.blue.x, prim.blue.y},
            .whitePoint = {prim.white.x, prim.white.y},
            .maxLuminance = color->hdr.max_luma,
            .minLuminance = color->hdr.min_luma,
        },
        .cta861 = {
            .maxContentLightLevel = color->hdr.max_cll,
            .maxFrameAverageLightLevel = color->hdr.max_fall,
        },
    };
}

static void set_ohos_output_csp(struct vo_ohos_state *ctx,
                                struct pl_color_space color)
{
    pl_color_space_infer(&color);

    bool hdr = is_ohos_hdr(&color);
    OH_NativeBuffer_ColorSpace color_space = get_ohos_color_space(&color);
    OH_NativeBuffer_MetadataType metadata_type = hdr
        ? (color.transfer == PL_COLOR_TRC_HLG ? OH_VIDEO_HDR_HLG
                                              : OH_VIDEO_HDR_HDR10)
        : OH_VIDEO_NONE;
    float hdr_brightness = hdr ? 1.0f : 0.0f;
    OH_NativeBuffer_StaticMetadata static_metadata =
        get_ohos_static_metadata(&color);

    if (!ctx->metadata_type_valid || ctx->metadata_type != metadata_type) {
        int ret = OH_NativeWindow_SetMetadataValue(
            ctx->native_window, OH_HDR_METADATA_TYPE, sizeof(metadata_type),
            (uint8_t *)&metadata_type);
        if (ret == 0) {
            ctx->metadata_type = metadata_type;
            ctx->metadata_type_valid = true;
        } else {
            MP_WARN(ctx, "Failed to set NativeWindow HDR metadata type: %d\n", ret);
        }
    }

    if (hdr && (!ctx->static_metadata_valid ||
                memcmp(&ctx->static_metadata, &static_metadata,
                       sizeof(static_metadata)) != 0))
    {
        int ret = OH_NativeWindow_SetMetadataValue(
            ctx->native_window, OH_HDR_STATIC_METADATA,
            sizeof(static_metadata), (uint8_t *)&static_metadata);
        if (ret == 0) {
            ctx->static_metadata = static_metadata;
            ctx->static_metadata_valid = true;
        } else {
            MP_WARN(ctx, "Failed to set NativeWindow HDR static metadata: %d\n", ret);
        }
    }

    if (!ctx->hdr_brightness_valid || ctx->hdr_brightness != hdr_brightness) {
        int ret = OH_NativeWindow_NativeWindowHandleOpt(
            ctx->native_window, SET_HDR_WHITE_POINT_BRIGHTNESS, hdr_brightness);
        if (ret == 0) {
            ctx->hdr_brightness = hdr_brightness;
            ctx->hdr_brightness_valid = true;
        } else {
            MP_WARN(ctx, "Failed to set NativeWindow HDR brightness: %d\n", ret);
        }
    }

    if (!ctx->color_space_valid || ctx->color_space != color_space) {
        int ret = OH_NativeWindow_SetColorSpace(ctx->native_window, color_space);
        if (ret == 0) {
            ctx->color_space = color_space;
            ctx->color_space_valid = true;
            MP_VERBOSE(ctx, "NativeWindow output switched to %s\n",
                       hdr ? (color.transfer == PL_COLOR_TRC_HLG ? "BT.2020 HLG"
                                                                : "BT.2020 PQ")
                           : "sRGB");
        } else {
            MP_WARN(ctx, "Failed to set NativeWindow color space: %d\n", ret);
        }
    }
}

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

struct pl_color_space vo_ohos_preferred_csp(struct vo *vo)
{
    struct mp_image_params params = vo_get_current_params(vo);
    struct pl_color_space color = pl_color_space_srgb;

    if (is_ohos_hdr(&params.color)) {
        color = params.color;
        color.primaries = PL_COLOR_PRIM_BT_2020;
        pl_color_space_infer(&color);
    }

    set_ohos_output_csp(vo->ohos, color);
    return color;
}

bool vo_ohos_set_10bit_format(struct vo *vo)
{
    struct vo_ohos_state *ctx = vo->ohos;
    int32_t format = NATIVEBUFFER_PIXEL_FMT_RGBA_1010102;
    int ret = OH_NativeWindow_NativeWindowHandleOpt(ctx->native_window,
                                                     SET_FORMAT, format);
    if (ret != 0) {
        MP_WARN(ctx, "Failed to request 10-bit NativeWindow format: %d\n", ret);
        return false;
    }

    int32_t actual = 0;
    ret = OH_NativeWindow_NativeWindowHandleOpt(ctx->native_window,
                                                GET_FORMAT, &actual);
    if (ret != 0 || actual != format) {
        MP_WARN(ctx, "NativeWindow did not accept 10-bit format "
                "(ret=%d, format=%d)\n", ret, actual);
        return false;
    }

    MP_VERBOSE(ctx, "Using 10-bit RGBA NativeWindow format\n");
    return true;
}
