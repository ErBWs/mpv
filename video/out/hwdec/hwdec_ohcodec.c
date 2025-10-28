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
#include <libavcodec/codec.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_oh.h>
#include <native_window/external_window.h>

#include "config.h"

#include "video/out/gpu/hwdec.h"
#include "video/out/ohos_common.h"

#if HAVE_GL
#include "video/out/opengl/ra_gl.h"
#endif

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
    OHNativeWindow* window;
};

struct priv {
    void *interop_mapper_priv;

    struct ra_imgfmt_desc desc;
};

static void uninit(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
}

static int init(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = hw->driver->name,
        .hw_imgfmt = IMGFMT_OHCODEC,
    };

    int ret = av_hwdevice_ctx_create(&p->hwctx.av_device_ref,
                                     AV_HWDEVICE_TYPE_OHCODEC, NULL, NULL, 0);
    if (ret != 0) {
        MP_VERBOSE(hw, "Failed to create hwdevice_ctx: %s\n", av_err2str(ret));
        return -1;
    }

    AVHWDeviceContext *ctx = (void *)p->hwctx.av_device_ref->data;
    AVOHCodecDeviceContext *hwctx = ctx->hwctx;

    hwdec_devices_add(hw->devs, &p->hwctx);

    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv_owner *p_owner = mapper->owner->priv;
    struct priv *p = mapper->priv;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv_owner *p_owner = mapper->owner->priv;
    struct priv *p = mapper->priv;
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv_owner *p_owner = mapper->owner->priv;
    struct priv *p = mapper->priv;

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = mapper->src_params.hw_subfmt;
    mapper->dst_params.hw_subfmt = 0;

    if (!mapper->dst_params.imgfmt) {
        MP_ERR(mapper, "Unsupported OHCodec format.\n");
        return -1;
    }

    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &p->desc)) {
        MP_ERR(mapper, "Unsupported texture format.\n");
        return -1;
    }

    return 0;
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv_owner *p_owner = mapper->owner->priv;
    struct priv *p = mapper->priv;

    return 0;
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
