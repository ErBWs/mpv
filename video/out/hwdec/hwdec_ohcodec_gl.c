/* HarmonyOS NativeImage + GL_EXT_YUV_target zero-copy interop. */

#include <EGL/egl.h>
#include <native_window/graphic_error_code.h>

#include "hwdec_ohcodec.h"
#include "video/out/opengl/ra_gl.h"

struct gl_owner {
    struct ohcodec_surface surface;
    GLuint texture;
};

static bool mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_priv *owner = mapper->owner->priv;
    struct gl_owner *g = owner->interop_owner_priv;
    bool high_depth = mapper->src_params.hw_subfmt == IMGFMT_P010;
    const struct ra_format *fmt = high_depth
                                ? ra_find_unorm_format(mapper->ra, 2, 4) : NULL;
    // The format only describes the result type of an external OES sampler;
    // it does not describe the NativeImage allocation. GLES devices commonly
    // expose no renderable RGBA16 RA format even though the external sampler
    // returns the full normalized P010 precision.
    if (!fmt)
        fmt = ra_find_unorm_format(mapper->ra, 1, 4);

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = IMGFMT_OHCODEC_YUV;
    mapper->dst_params.hw_subfmt = 0;
    if (high_depth) {
        mapper->dst_params.repr.bits.sample_depth = 10;
        mapper->dst_params.repr.bits.color_depth = 10;
        // P010 stores 10-bit codes in the high bits of a 16-bit word, but an
        // external YUV sampler has already normalized those codes. Keeping the
        // software P010 shift here would divide Y/Cb/Cr by 64 a second time,
        // collapsing both chroma channels towards zero (bright green output).
        mapper->dst_params.repr.bits.bit_shift = 0;
    }

    struct ra_tex_params params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = fmt,
        .render_src = true,
        .src_linear = fmt && fmt->linear_filter,
        .external_oes = true,
        .external_yuv = true,
    };
    if (!fmt)
        return false;

    mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, g->texture);
    return mapper->tex[0] != NULL;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    ra_tex_free(mapper->ra, &mapper->tex[0]);
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_priv *owner = mapper->owner->priv;
    struct gl_owner *g = owner->interop_owner_priv;

    int ret = OH_NativeImage_UpdateSurfaceImage(g->surface.image);
    if (ret != 0) {
        if (ret != NATIVE_ERROR_NO_BUFFER)
            MP_ERR(mapper, "OH_NativeImage_UpdateSurfaceImage failed: %d\n", ret);
        return ret;
    }

    return NATIVE_ERROR_OK;
}

static int mapper_reuse(struct ra_hwdec_mapper *mapper)
{
    // OH_NativeImage keeps the last updated buffer bound to the OES texture
    // until UpdateSurfaceImage selects the next one.
    return NATIVE_ERROR_OK;
}

static void owner_uninit(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;
    struct gl_owner *g = p->interop_owner_priv;
    GL *gl = ra_gl_get(hw->ra_ctx->ra);
    if (!g)
        return;

    ohcodec_surface_destroy(&g->surface);
    OH_NativeImage_Destroy(&g->surface.image);
    if (gl && g->texture)
        gl->DeleteTextures(1, &g->texture);
    talloc_free(g);
    p->surface = NULL;
    p->interop_owner_priv = NULL;
}

static const struct ohcodec_interop interop = {
    .owner_uninit = owner_uninit,
    .mapper_init = mapper_init,
    .mapper_uninit = mapper_uninit,
    .map = mapper_map,
    .reuse = mapper_reuse,
};

bool ohcodec_interop_gl_init(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;
    GL *gl = ra_gl_get(hw->ra_ctx->ra);

    if (!gl || !ra_is_gl(hw->ra_ctx->ra) || !eglGetCurrentContext() ||
        !gl_check_extension(gl->extensions, "GL_EXT_YUV_target"))
        return false;

    bool essl3 = gl_check_extension(gl->extensions,
                                    "GL_OES_EGL_image_external_essl3");
    bool essl2 = gl_check_extension(gl->extensions,
                                    "GL_OES_EGL_image_external");
    if (!essl3 && !essl2)
        return false;

    struct gl_owner *g = talloc_zero(hw, struct gl_owner);
    gl->GenTextures(1, &g->texture);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, g->texture);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    OH_NativeImage *image = OH_NativeImage_Create(g->texture,
                                                   GL_TEXTURE_EXTERNAL_OES);
    if (!image || !ohcodec_surface_init(&g->surface, image)) {
        if (image)
            OH_NativeImage_Destroy(&image);
        gl->DeleteTextures(1, &g->texture);
        talloc_free(g);
        return false;
    }

    static const char *exts2[] = {
        "GL_OES_EGL_image_external", "GL_EXT_YUV_target", NULL,
    };
    static const char *exts3[] = {
        "GL_OES_EGL_image_external_essl3", "GL_EXT_YUV_target", NULL,
    };
    hw->glsl_extensions = essl3 ? exts3 : exts2;
    p->surface = &g->surface;
    p->interop_owner_priv = g;
    p->interop = &interop;
    MP_VERBOSE(hw, "OHCodec is using NativeImage OES raw-YUV zero-copy\n");
    return true;
}
