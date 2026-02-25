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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include "video/out/opengl/ra_gl.h"

typedef void *GLeglImageOES;
typedef void *EGLImageKHR;

struct ohcodec_gl_priv {
    GLuint gl_texture;
};

static bool interop_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p_mapper = mapper->priv;
    struct ohcodec_priv *o = mapper->owner->priv;
    struct ohcodec_gl_priv *p = talloc_zero(p_mapper, struct ohcodec_gl_priv);
    p_mapper->priv = p;
    GL *gl = ra_gl_get(mapper->ra);

    gl->GenTextures(1, &p->gl_texture);
    MP_VERBOSE(mapper, "test: gl_texture: %d\n", p->gl_texture);

    return true;
}

static void interop_uninit(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p_mapper = mapper->priv;
    struct ohcodec_gl_priv *p = p_mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;

    ra_tex_free(mapper->ra, &mapper->tex[0]);
}

static bool interop_map(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p_mapper = mapper->priv;
    struct ohcodec_priv *o = mapper->owner->priv;
    struct ohcodec_gl_priv *p = p_mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);

    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    // OH_NativeImage_AttachContext(o->image, p->gl_texture);

    struct ra_tex_params params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = ra_find_unorm_format(mapper->ra, 1, 4),
        .render_src = true,
        .src_linear = true,
    };

    if (params.format->ctype != RA_CTYPE_UNORM)
        return false;

    mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, p->gl_texture);
    if (!mapper->tex[0])
        return false;

    return true;
}

static void interop_unmap(struct ra_hwdec_mapper *mapper)
{

}

bool ohcodec_interop_gl_init(const struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;

    if (!ra_is_gl(hw->ra_ctx->ra))
        return false;
    MP_VERBOSE(hw, "OHCodec is using OpenGL backend\n");

    p->interop_init = interop_init;
    p->interop_uninit = interop_uninit;
    p->interop_map = interop_map;
    p->interop_unmap = interop_unmap;

    return true;
}
