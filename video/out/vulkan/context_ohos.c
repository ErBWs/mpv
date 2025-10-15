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

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include "video/out/ohos_common.h"
#include "common.h"
#include "context.h"
#include "utils.h"

struct priv {
    struct mpvk_ctx vk;
};

static void ohos_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ra_vk_ctx_uninit(ctx);
    mpvk_uninit(&p->vk);

    vo_ohos_uninit(ctx->vo);
}

static bool ohos_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);
    struct mpvk_ctx *vk = &p->vk;
    int msgl = ctx->opts.probing ? MSGL_V : MSGL_ERR;

    if (!vo_ohos_init(ctx->vo))
        goto fail;

    if (!mpvk_init(vk, ctx, VK_OHOS_SURFACE_EXTENSION_NAME))
        goto fail;

    VkSurfaceCreateInfoOHOS info = {
         .sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS,
         .window = vo_ohos_native_window(ctx->vo)
    };

    struct ra_ctx_params params = {0};

    VkInstance inst = vk->vkinst->instance;
    VkResult res = vkCreateSurfaceOHOS(inst, &info, NULL, &vk->surface);
    if (res != VK_SUCCESS) {
        MP_MSG(ctx, msgl, "Failed creating ohos surface\n");
        goto fail;
    }

    if (!ra_vk_ctx_init(ctx, vk, params, VK_PRESENT_MODE_FIFO_KHR))
        goto fail;

    return true;
fail:
    ohos_uninit(ctx);
    return false;
}

static bool ohos_reconfig(struct ra_ctx *ctx)
{
    int w, h;
    if (!vo_ohos_surface_size(ctx->vo, &w, &h))
        return false;

    ra_vk_ctx_resize(ctx, w, h);
    return true;
}

static int ohos_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    return VO_NOTIMPL;
}

const struct ra_ctx_fns ra_ctx_vulkan_ohos = {
    .type           = "vulkan",
    .name           = "ohosvk",
    .description    = "HarmonyOS/Vulkan",
    .reconfig       = ohos_reconfig,
    .control        = ohos_control,
    .init           = ohos_init,
    .uninit         = ohos_uninit,
};
