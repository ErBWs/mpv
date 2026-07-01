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

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <native_window/external_window.h>

#include <libavutil/hwcontext_oh.h>

#include "hwdec_ohcodec.h"

#include "video/out/opengl/ra_gl.h"

#ifndef EGL_NATIVE_BUFFER_OHOS
#define EGL_NATIVE_BUFFER_OHOS 0x34E1
#endif

typedef void *GLeglImageOES;

struct gl_mapper_priv {
    EGLDisplay display;
    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(
        EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
    GLuint texture;
    EGLImageKHR egl_image;
    OH_NativeBuffer *native_buffer;
    OHNativeWindowBuffer *window_buffer;
};

static struct gl_mapper_priv *gl_priv(struct ohcodec_mapper_priv *p)
{
    return p->priv;
}

static void destroy_image(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct gl_mapper_priv *g = gl_priv(p);
    if (!g)
        return;

    if (g->egl_image != EGL_NO_IMAGE_KHR) {
        g->DestroyImageKHR(g->display, g->egl_image);
        g->egl_image = EGL_NO_IMAGE_KHR;
    }

    if (g->window_buffer) {
        OH_NativeWindow_DestroyNativeWindowBuffer(g->window_buffer);
        g->window_buffer = NULL;
    }

    if (g->native_buffer) {
        OH_NativeBuffer_Unreference(g->native_buffer);
        g->native_buffer = NULL;
    }
}

static bool mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct gl_mapper_priv *g = talloc_zero(mapper, struct gl_mapper_priv);
    GL *gl = ra_gl_get(mapper->ra);

    if (!gl)
        return false;

    g->display = eglGetCurrentDisplay();
    g->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    g->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    g->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    g->egl_image = EGL_NO_IMAGE_KHR;

    if (g->display == EGL_NO_DISPLAY || !g->CreateImageKHR ||
        !g->DestroyImageKHR || !g->EGLImageTargetTexture2DOES)
        goto fail;

    p->priv = g;
    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = IMGFMT_RGB0;
    mapper->dst_params.hw_subfmt = 0;
    mp_image_params_guess_csp(&mapper->dst_params);

    struct ra_tex_params params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = ra_find_unorm_format(mapper->ra, 1, 4),
        .render_src = true,
        .src_linear = true,
        .external_oes = true,
    };

    if (!params.format || params.format->ctype != RA_CTYPE_UNORM)
        goto fail;

    gl->GenTextures(1, &g->texture);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, g->texture);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, g->texture);
    if (!mapper->tex[0]) {
        gl->DeleteTextures(1, &g->texture);
        g->texture = 0;
        goto fail;
    }

    return true;

fail:
    talloc_free(g);
    p->priv = NULL;
    return false;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct gl_mapper_priv *g = gl_priv(p);
    GL *gl = ra_gl_get(mapper->ra);

    if (!g || !gl)
        return;

    destroy_image(mapper);
    ra_tex_free(mapper->ra, &mapper->tex[0]);
    gl->DeleteTextures(1, &g->texture);
    p->priv = NULL;
}

static bool mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct gl_mapper_priv *g = gl_priv(p);
    const AVOHCodecFrameDescriptor *desc = ohcodec_mapper_frame_desc(mapper);
    GL *gl = ra_gl_get(mapper->ra);

    if (!g || !desc || !gl)
        return false;

    destroy_image(mapper);

    g->native_buffer = ohcodec_get_native_buffer(mapper, desc);
    if (!g->native_buffer)
        return false;

    OH_NativeBuffer_Config config = {0};
    OH_NativeBuffer_GetConfig(g->native_buffer, &config);
    if (config.width <= 0 || config.height <= 0) {
        MP_ERR(mapper, "Invalid OHCodec buffer size %dx%d\n",
               config.width, config.height);
        destroy_image(mapper);
        return false;
    }

    if (mapper->tex[0]->params.w != config.width ||
        mapper->tex[0]->params.h != config.height)
    {
        MP_VERBOSE(mapper, "Texture dimensions changed to %dx%d\n",
                   config.width, config.height);
        mapper->tex[0]->params.w = config.width;
        mapper->tex[0]->params.h = config.height;
    }

    g->window_buffer =
        OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(g->native_buffer);
    if (!g->window_buffer) {
        MP_ERR(mapper, "Failed to wrap OH_NativeBuffer as OHNativeWindowBuffer\n");
        return false;
    }

    const EGLint attribs[] = {EGL_NONE};
    g->egl_image = g->CreateImageKHR(g->display, EGL_NO_CONTEXT,
                                     EGL_NATIVE_BUFFER_OHOS,
                                     (EGLClientBuffer)g->window_buffer,
                                     attribs);
    if (g->egl_image == EGL_NO_IMAGE_KHR) {
        MP_ERR(mapper, "eglCreateImageKHR(EGL_NATIVE_BUFFER_OHOS) failed: 0x%x\n",
               eglGetError());
        destroy_image(mapper);
        return false;
    }

    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, g->texture);
    g->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, g->egl_image);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    return true;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    destroy_image(mapper);
}

bool ohcodec_interop_gl_init(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;
    GL *gl = ra_gl_get(hw->ra_ctx->ra);

    if (!gl || !ra_is_gl(hw->ra_ctx->ra) || !eglGetCurrentContext())
        return false;

    static const char *es2_exts[] = {"GL_OES_EGL_image_external", 0};
    static const char *es3_exts[] = {"GL_OES_EGL_image_external_essl3", 0};
    if (gl_check_extension(gl->extensions, es3_exts[0])) {
        hw->glsl_extensions = es3_exts;
    } else if (gl_check_extension(gl->extensions, es2_exts[0])) {
        hw->glsl_extensions = es2_exts;
    } else {
        return false;
    }

    MP_VERBOSE(hw, "OHCodec is using OpenGL EGL native buffer backend\n");

    p->output_mode = AV_OHCODEC_OUTPUT_MODE_BUFFER;
    p->release_src_after_map = false;
    p->interop_get_native_window = NULL;
    p->interop_owner_uninit = NULL;
    p->interop_init = mapper_init;
    p->interop_uninit = mapper_uninit;
    p->interop_map = mapper_map;
    p->interop_unmap = mapper_unmap;

    return true;
}
