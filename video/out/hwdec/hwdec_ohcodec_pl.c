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

bool ohcodec_interop_pl_init(const struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;

    if (!ra_pl_get(hw->ra_ctx->ra))
        return false;

    MP_VERBOSE(hw, "OHCodec is using libplacebo backend\n");

    p->interop_init = ohcodec_upload_init;
    p->interop_uninit = ohcodec_upload_uninit;
    p->interop_map = ohcodec_upload_map;
    p->interop_unmap = ohcodec_upload_unmap;

    return true;
}
