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

static bool interop_init(struct ra_hwdec_mapper *mapper)
{
    return false;
}

static void interop_uninit(struct ra_hwdec_mapper *mapper)
{

}

static bool interop_map(struct ra_hwdec_mapper *mapper)
{
    return false;
}

static void interop_unmap(struct ra_hwdec_mapper *mapper)
{

}

bool ohcodec_interop_pl_init(const struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;

    pl_gpu gpu = ra_pl_get(hw->ra_ctx->ra);
    if (gpu == NULL)
        return false;
    MP_VERBOSE(hw, "OHCodec is using Vulkan backend\n");

    p->interop_init = interop_init;
    p->interop_uninit = interop_uninit;
    p->interop_map = interop_map;
    p->interop_unmap = interop_unmap;

    return true;
}
