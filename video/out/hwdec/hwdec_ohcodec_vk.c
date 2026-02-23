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

#include "hwdec_ohcodec.h"

#include "video/out/placebo/ra_pl.h"

static bool ext_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct ohcodec_priv *o = mapper->owner->priv;

    // glGenTextures(1, &p->gl_texture);
    // glBindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    // glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_REPEAT);
    // glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_REPEAT);
    // glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    // glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    struct ra_tex_params params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = ra_find_unorm_format(mapper->ra, 1, 4),
        .render_src = true,
        .src_linear = true,
        .host_mutable = true,
        // .external_oes = true,
    };

    if (params.format->ctype != RA_CTYPE_UNORM)
        return false;

    // mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, p->gl_texture);
    if (!mapper->tex[0])
        return false;

    return true;

}

static void ext_uninit(struct ra_hwdec_mapper *mapper)
{

}

static bool check(const struct ra_hwdec *hw)
{
    pl_gpu gpu = ra_pl_get(hw->ra_ctx->ra);
    if (gpu == NULL)
        return false;

    MP_VERBOSE(hw, "OHCodec is using Vulkan backend\n");
    return true;
}

static void init(const struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;

    p->ext_init = ext_init;
    p->ext_uninit = ext_uninit;
}

struct ohcodec_interop_fn ohcodec_vk_fn = {
    .check = check,
    .init = init
};
