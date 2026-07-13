/* HarmonyOS ConsumerSurface + Vulkan NativeBuffer zero-copy interop. */

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include <libplacebo/vulkan.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <native_window/graphic_error_code.h>

#include "hwdec_ohcodec.h"
#include "video/out/placebo/ra_pl.h"

#if defined(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS)
#define OH_NATIVE_BUFFER_HANDLE_TYPE \
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS
#else
#define OH_NATIVE_BUFFER_HANDLE_TYPE \
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS
#endif

#define MAX_CACHED_IMPORTS 16

struct vk_ycbcr {
    struct vk_ycbcr *next;
    uint64_t external_format;
    VkFormat format;
    VkSamplerYcbcrConversion conversion;
    VkSampler sampler;
};

struct vk_owner {
    struct ohcodec_surface surface;
    pl_vulkan vk;
    struct vk_ycbcr *ycbcr;
};

struct vk_import {
    struct vk_import *next;
    uint32_t seq;
    OH_NativeBuffer *native_buffer;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    struct vk_ycbcr *ycbcr;
    VkSemaphore acquire_sem;
    VkSemaphore release_sem;
    pl_tex pltex;
    struct ra_tex *ratex;
    bool busy;
};

struct vk_mapper {
    pl_gpu gpu;
    pl_vulkan vk;
    PFN_vkGetNativeBufferPropertiesOHOS get_props;
    PFN_vkAcquireImageOHOS acquire_image;
    PFN_vkQueueSignalReleaseImageOHOS signal_release;
    VkQueue queue;
    uint32_t qf;
    pl_fmt rgba8;
    pl_fmt rgba16;
    struct vk_import *imports;
    int num_imports;
    struct vk_import *current;
    OHNativeWindowBuffer *window_buffer;
};

static bool has_extension(pl_vulkan vk, const char *name)
{
    for (int i = 0; i < vk->num_extensions; i++) {
        if (strcmp(vk->extensions[i], name) == 0)
            return true;
    }
    return false;
}

static bool find_memory_type(pl_vulkan vk, uint32_t bits, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(vk->phys_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            *index = i;
            return true;
        }
    }
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (bits & (1u << i)) {
            *index = i;
            return true;
        }
    }
    return false;
}

static void destroy_import(struct ra_hwdec_mapper *mapper,
                           struct vk_import *entry)
{
    if (!entry)
        return;
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = p->priv;
    VkDevice dev = v->vk->device;

    if (mapper->tex[0] == entry->ratex)
        mapper->tex[0] = NULL;
    ra_tex_free(mapper->ra, &entry->ratex);
    entry->pltex = NULL;
    vkDestroySemaphore(dev, entry->release_sem, NULL);
    vkDestroySemaphore(dev, entry->acquire_sem, NULL);
    vkDestroyImageView(dev, entry->view, NULL);
    vkDestroyImage(dev, entry->image, NULL);
    vkFreeMemory(dev, entry->memory, NULL);
    if (entry->native_buffer)
        OH_NativeBuffer_Unreference(entry->native_buffer);
    talloc_free(entry);
}

static VkFilter choose_filter(const VkNativeBufferFormatPropertiesOHOS *props)
{
    return props->formatFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT
         ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

static struct vk_ycbcr *get_ycbcr(struct ra_hwdec_mapper *mapper,
    const VkNativeBufferFormatPropertiesOHOS *props)
{
    struct ohcodec_priv *owner = mapper->owner->priv;
    struct vk_owner *o = owner->interop_owner_priv;
    VkDevice dev = o->vk->device;

    for (struct vk_ycbcr *y = o->ycbcr; y; y = y->next) {
        if (y->external_format == props->externalFormat &&
            y->format == props->format)
            return y;
    }

    struct vk_ycbcr *y = talloc_zero(o, struct vk_ycbcr);
    if (!y)
        return NULL;
    y->external_format = props->externalFormat;
    y->format = props->format;

    VkExternalFormatOHOS external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
        .externalFormat = props->format == VK_FORMAT_UNDEFINED
                        ? props->externalFormat : 0,
    };
    VkSamplerYcbcrConversionCreateInfo conv = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .pNext = props->format == VK_FORMAT_UNDEFINED ? &external : NULL,
        .format = props->format,
        // Do not apply a matrix or range expansion. The immutable conversion
        // sampler exposes the encoded components for mpv's own color/Dolby
        // pipeline. Vulkan defines these post-swizzle channels as Cr/Y/Cb.
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
        .components = props->samplerYcbcrConversionComponents,
        .xChromaOffset = props->suggestedXChromaOffset,
        .yChromaOffset = props->suggestedYChromaOffset,
        .chromaFilter = choose_filter(props),
    };
    VkResult res = vkCreateSamplerYcbcrConversion(dev, &conv, NULL,
                                                   &y->conversion);
    if (res != VK_SUCCESS)
        goto error;

    VkSamplerYcbcrConversionInfo link = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = y->conversion,
    };
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &link,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    res = vkCreateSampler(dev, &sampler, NULL, &y->sampler);
    if (res != VK_SUCCESS)
        goto error;

    y->next = o->ycbcr;
    o->ycbcr = y;
    return y;

error:
    vkDestroySampler(dev, y->sampler, NULL);
    vkDestroySamplerYcbcrConversion(dev, y->conversion, NULL);
    talloc_free(y);
    MP_ERR(mapper, "Failed creating immutable YCbCr sampler: %d\n", res);
    return NULL;
}

static bool create_import(struct ra_hwdec_mapper *mapper,
                          OH_NativeBuffer *buffer, struct vk_import **out)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = p->priv;
    VkDevice dev = v->vk->device;
    struct vk_import *e = talloc_zero(v, struct vk_import);
    OH_NativeBuffer_Config config = {0};
    VkResult res;

    if (!e || OH_NativeBuffer_Reference(buffer) != NATIVE_ERROR_OK)
        goto error;
    e->native_buffer = buffer;
    e->seq = OH_NativeBuffer_GetSeqNum(buffer);
    OH_NativeBuffer_GetConfig(buffer, &config);
    if (config.width <= 0 || config.height <= 0)
        goto error;
    VkNativeBufferFormatPropertiesOHOS fmt_props = {
        .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS,
    };
    VkNativeBufferPropertiesOHOS props = {
        .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS,
        .pNext = &fmt_props,
    };
    res = v->get_props(dev, buffer, &props);
    if (res != VK_SUCCESS || !props.allocationSize || !props.memoryTypeBits ||
        !(fmt_props.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        (fmt_props.format == VK_FORMAT_UNDEFINED && !fmt_props.externalFormat))
    {
        MP_ERR(mapper, "NativeBuffer is not Vulkan sampleable (res=%d "
                       "format=%d external=%"PRIu64" features=0x%x)\n",
               res, fmt_props.format, fmt_props.externalFormat,
               fmt_props.formatFeatures);
        goto error;
    }
    VkExternalFormatOHOS external_fmt = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
        .externalFormat = fmt_props.format == VK_FORMAT_UNDEFINED
                        ? fmt_props.externalFormat : 0,
    };
    VkExternalMemoryImageCreateInfo external_mem = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = fmt_props.format == VK_FORMAT_UNDEFINED ? &external_fmt : NULL,
        .handleTypes = OH_NATIVE_BUFFER_HANDLE_TYPE,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_mem,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt_props.format,
        .extent = { config.width, config.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if ((res = vkCreateImage(dev, &image_info, NULL, &e->image)) != VK_SUCCESS)
        goto vk_error;

    uint32_t memory_type;
    if (!find_memory_type(v->vk, props.memoryTypeBits, &memory_type))
        goto error;
    VkImportNativeBufferInfoOHOS import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS,
        .buffer = buffer,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import,
        .image = e->image,
    };
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = memory_type,
    };
    if ((res = vkAllocateMemory(dev, &alloc, NULL, &e->memory)) != VK_SUCCESS ||
        (res = vkBindImageMemory(dev, e->image, e->memory, 0)) != VK_SUCCESS)
        goto vk_error;

    e->ycbcr = get_ycbcr(mapper, &fmt_props);
    if (!e->ycbcr)
        goto error;

    VkSamplerYcbcrConversionInfo conv_link = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = e->ycbcr->conversion,
    };
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &conv_link,
        .image = e->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt_props.format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    if ((res = vkCreateImageView(dev, &view_info, NULL, &e->view)) != VK_SUCCESS)
        goto vk_error;

    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    if ((res = vkCreateSemaphore(dev, &sem_info, NULL, &e->acquire_sem)) != VK_SUCCESS ||
        (res = vkCreateSemaphore(dev, &sem_info, NULL, &e->release_sem)) != VK_SUCCESS)
        goto vk_error;

    bool high_depth = config.format == NATIVEBUFFER_PIXEL_FMT_YCBCR_P010 ||
                      config.format == NATIVEBUFFER_PIXEL_FMT_YCRCB_P010;
    pl_fmt virtual_fmt = high_depth ? v->rgba16 : v->rgba8;
    e->pltex = pl_vulkan_wrap(v->gpu, pl_vulkan_wrap_params(
        .image = e->image,
        .width = config.width,
        .height = config.height,
        .format = fmt_props.format,
        .pl_format = virtual_fmt,
        .image_view = e->view,
        .sampler = e->ycbcr->sampler,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .debug_tag = "OHCodec NativeBuffer",
    ));
    if (!e->pltex)
        goto error;
    e->ratex = talloc_zero(e, struct ra_tex);
    if (!e->ratex || !mppl_wrap_tex(mapper->ra, e->pltex, e->ratex))
        goto error;

    e->next = v->imports;
    v->imports = e;
    v->num_imports++;
    *out = e;
    MP_TRACE(mapper, "Imported NativeBuffer %u (%d cached)\n",
             e->seq, v->num_imports);
    return true;

vk_error:
    MP_ERR(mapper, "Vulkan NativeBuffer import failed: %d\n", res);
error:
    destroy_import(mapper, e);
    return false;
}

static struct vk_import *find_import(struct vk_mapper *v, uint32_t seq)
{
    for (struct vk_import **link = &v->imports; *link;
         link = &(*link)->next) {
        struct vk_import *e = *link;
        // FromNativeWindowBuffer may return a different wrapper for the same
        // queue buffer. The sequence number is its system-wide unique ID.
        if (e->seq == seq && !e->busy) {
            *link = e->next;
            e->next = v->imports;
            v->imports = e;
            return e;
        }
    }
    return NULL;
}

static bool evict_idle_import(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = p->priv;
    struct vk_import **victim = NULL;
    for (struct vk_import **link = &v->imports; *link;
         link = &(*link)->next) {
        if (!(*link)->busy)
            victim = link;
    }
    if (!victim)
        return false;

    struct vk_import *entry = *victim;
    *victim = entry->next;
    // The release fence transfers ownership back to the Surface, but the
    // Vulkan submission may still reference these objects. Eviction is rare
    // (normally only after resize), so finish once before destroying them.
    MP_TRACE(mapper, "Evicting NativeBuffer %u; waiting for GPU\n", entry->seq);
    pl_gpu_finish(v->gpu);
    destroy_import(mapper, entry);
    v->num_imports--;
    return true;
}

static void release_acquired_buffer(struct ra_hwdec_mapper *mapper,
                                    OHNativeWindowBuffer *buffer, int fence_fd)
{
    struct ohcodec_priv *owner = mapper->owner->priv;
    if (!buffer)
        return;

    OH_NativeWindow_NativeObjectUnreference(buffer);
    int ret = OH_NativeImage_ReleaseNativeWindowBuffer(owner->surface->image,
                                                        buffer, fence_fd);
    if (ret != NATIVE_ERROR_OK) {
        if (fence_fd >= 0)
            close(fence_fd);
        MP_ERR(mapper, "ReleaseNativeWindowBuffer failed: %d\n", ret);
    }
}

static void release_window_buffer(struct ra_hwdec_mapper *mapper, int fence_fd)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = p->priv;
    release_acquired_buffer(mapper, v->window_buffer, fence_fd);
    v->window_buffer = NULL;
}

static void mapper_release(struct ra_hwdec_mapper *mapper);

static bool mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = talloc_zero(mapper, struct vk_mapper);
    p->priv = v;
    if (!v)
        return false;

    v->gpu = ra_pl_get(mapper->ra);
    v->vk = v->gpu ? pl_vulkan_get(v->gpu) : NULL;
    if (!v->vk || !has_extension(v->vk, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME) ||
        !has_extension(v->vk, VK_OHOS_NATIVE_BUFFER_EXTENSION_NAME))
        return false;

    v->get_props = (PFN_vkGetNativeBufferPropertiesOHOS)
        vkGetDeviceProcAddr(v->vk->device, "vkGetNativeBufferPropertiesOHOS");
    v->acquire_image = (PFN_vkAcquireImageOHOS)
        vkGetDeviceProcAddr(v->vk->device, "vkAcquireImageOHOS");
    v->signal_release = (PFN_vkQueueSignalReleaseImageOHOS)
        vkGetDeviceProcAddr(v->vk->device, "vkQueueSignalReleaseImageOHOS");
    if (!v->get_props || !v->acquire_image || !v->signal_release)
        return false;

    v->qf = v->vk->queue_graphics.index;
    vkGetDeviceQueue(v->vk->device, v->qf, 0, &v->queue);
    v->rgba8 = pl_find_named_fmt(v->gpu, "rgba8");
    v->rgba16 = pl_find_named_fmt(v->gpu, "rgba16");
    if (!v->queue || !v->rgba8 || !v->rgba16)
        return false;

    mapper->dst_params = mapper->src_params;
    bool high_depth = mapper->src_params.hw_subfmt == IMGFMT_P010;
    mapper->dst_params.imgfmt = high_depth ? IMGFMT_OHCODEC_YUV_VK16
                                           : IMGFMT_OHCODEC_YUV_VK8;
    mapper->dst_params.hw_subfmt = 0;
    if (high_depth) {
        mapper->dst_params.repr.bits.sample_depth = 10;
        mapper->dst_params.repr.bits.color_depth = 10;
        // The sampler returns normalized values. The six-bit storage shift
        // belongs to CPU-visible P010 words and must not be applied again.
        mapper->dst_params.repr.bits.bit_shift = 0;
    }
    return true;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = p->priv;
    if (!v)
        return;

    if (v->current)
        mapper_release(mapper);
    else if (v->window_buffer)
        release_window_buffer(mapper, -1);
    if (v->gpu)
        pl_gpu_finish(v->gpu);
    while (v->imports) {
        struct vk_import *e = v->imports;
        v->imports = e->next;
        destroy_import(mapper, e);
    }
    talloc_free(v);
    p->priv = NULL;
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = p->priv;
    struct ohcodec_priv *owner = mapper->owner->priv;
    OHNativeWindowBuffer *wb = NULL;
    OH_NativeBuffer *buffer = NULL;
    int fence_fd = -1;

    int ret = OH_NativeImage_AcquireNativeWindowBuffer(owner->surface->image,
                                                        &wb, &fence_fd);
    if (ret != NATIVE_ERROR_OK || !wb) {
        if (ret != NATIVE_ERROR_NO_BUFFER)
            MP_ERR(mapper, "AcquireNativeWindowBuffer failed: %d\n", ret);
        return ret != NATIVE_ERROR_OK ? ret : NATIVE_ERROR_UNKNOWN;
    }
    if (OH_NativeWindow_NativeObjectReference(wb) != NATIVE_ERROR_OK) {
        int release = OH_NativeImage_ReleaseNativeWindowBuffer(
            owner->surface->image, wb, fence_fd);
        if (release != NATIVE_ERROR_OK && fence_fd >= 0)
            close(fence_fd);
        return NATIVE_ERROR_UNKNOWN;
    }
    if (OH_NativeBuffer_FromNativeWindowBuffer(wb, &buffer) != NATIVE_ERROR_OK ||
        !buffer) {
        release_acquired_buffer(mapper, wb, fence_fd);
        return NATIVE_ERROR_UNKNOWN;
    }
    uint32_t seq = OH_NativeBuffer_GetSeqNum(buffer);
    struct vk_import *e = find_import(v, seq);
    if (!e) {
        if ((v->num_imports >= MAX_CACHED_IMPORTS &&
             !evict_idle_import(mapper)) ||
            !create_import(mapper, buffer, &e))
        {
            MP_ERR(mapper, "OHCodec Vulkan import cache exhausted\n");
            release_acquired_buffer(mapper, wb, fence_fd);
            return NATIVE_ERROR_UNKNOWN;
        }
    }

    VkResult vkres = v->acquire_image(v->vk->device, e->image, fence_fd,
                                      e->acquire_sem, VK_NULL_HANDLE);
    if (vkres != VK_SUCCESS) {
        if (fence_fd >= 0)
            close(fence_fd);
        MP_ERR(mapper, "vkAcquireImageOHOS failed: %d\n", vkres);
        release_acquired_buffer(mapper, wb, -1);
        return NATIVE_ERROR_UNKNOWN;
    }

    pl_vulkan_release_ex(v->gpu, pl_vulkan_release_params(
        .tex = e->pltex,
        .layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .qf = VK_QUEUE_FAMILY_EXTERNAL,
        .semaphore = { .sem = e->acquire_sem },
    ));

    // Match the official consumer pipeline: acquire the replacement first,
    // then return the previously displayed buffer to the Surface queue.
    if (v->current)
        mapper_release(mapper);

    e->busy = true;
    v->current = e;
    v->window_buffer = wb;
    mapper->tex[0] = e->ratex;
    return NATIVE_ERROR_OK;
}

static int mapper_reuse(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = p->priv;
    struct vk_import *e = v->current;
    if (!e || !v->window_buffer)
        return NATIVE_ERROR_NO_BUFFER;

    // The image remains owned by libplacebo between presentation passes. Only
    // the first acquire imports the Surface fence; repeated presentations just
    // reuse the same sampled texture.
    mapper->tex[0] = e->ratex;
    return NATIVE_ERROR_OK;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    // Keep libplacebo ownership until the decoded frame is replaced. Calling
    // hold on every presentation races libplacebo's internal pass lifetime and
    // caused "Attempting to hold an already held image".
}

static void mapper_release(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct vk_mapper *v = p->priv;
    if (!v || !v->current)
        return;

    struct vk_import *e = v->current;
    int fence_fd = -1;
    bool held = pl_vulkan_hold_ex(v->gpu, pl_vulkan_hold_params(
        .tex = e->pltex,
        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .qf = VK_QUEUE_FAMILY_EXTERNAL,
        .semaphore = { .sem = e->release_sem },
    ));
    if (held) {
        v->vk->lock_queue(v->vk, v->qf, 0);
        VkResult res = v->signal_release(v->queue, 1, &e->release_sem,
                                         e->image, &fence_fd);
        v->vk->unlock_queue(v->vk, v->qf, 0);
        if (res != VK_SUCCESS) {
            MP_ERR(mapper, "vkQueueSignalReleaseImageOHOS failed: %d\n", res);
            fence_fd = -1;
            pl_gpu_finish(v->gpu);
        }
    } else {
        MP_ERR(mapper, "Failed to hold OHCodec external image for release\n");
        pl_gpu_finish(v->gpu);
    }

    release_window_buffer(mapper, fence_fd);
    mapper->tex[0] = NULL;
    v->current->busy = false;
    v->current = NULL;
}

static void owner_uninit(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;
    struct vk_owner *v = p->interop_owner_priv;
    if (!v)
        return;
    ohcodec_surface_destroy(&v->surface);
    OH_NativeImage_Destroy(&v->surface.image);
    while (v->ycbcr) {
        struct vk_ycbcr *y = v->ycbcr;
        v->ycbcr = y->next;
        vkDestroySampler(v->vk->device, y->sampler, NULL);
        vkDestroySamplerYcbcrConversion(v->vk->device, y->conversion, NULL);
        talloc_free(y);
    }
    talloc_free(v);
    p->surface = NULL;
    p->interop_owner_priv = NULL;
}

static const struct ohcodec_interop interop = {
    .owner_uninit = owner_uninit,
    .mapper_init = mapper_init,
    .mapper_uninit = mapper_uninit,
    .map = mapper_map,
    .reuse = mapper_reuse,
    .unmap = mapper_unmap,
    .release = mapper_release,
};

bool ohcodec_interop_pl_init(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;
    pl_gpu gpu = ra_pl_get(hw->ra_ctx->ra);
    pl_vulkan vk = gpu ? pl_vulkan_get(gpu) : NULL;
    if (!vk || !has_extension(vk, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME) ||
        !has_extension(vk, VK_OHOS_NATIVE_BUFFER_EXTENSION_NAME))
        return false;

    struct vk_owner *v = talloc_zero(hw, struct vk_owner);
    if (v)
        v->vk = vk;
    OH_NativeImage *image = OH_ConsumerSurface_Create();
    if (!v || !image ||
        OH_ConsumerSurface_SetDefaultUsage(image, NATIVEBUFFER_USAGE_HW_TEXTURE) !=
            NATIVE_ERROR_OK ||
        !ohcodec_surface_init(&v->surface, image))
    {
        if (image)
            OH_NativeImage_Destroy(&image);
        talloc_free(v);
        return false;
    }

    p->surface = &v->surface;
    p->interop_owner_priv = v;
    p->interop = &interop;
    MP_VERBOSE(hw, "OHCodec is using ConsumerSurface Vulkan raw-YUV zero-copy\n");
    return true;
}
