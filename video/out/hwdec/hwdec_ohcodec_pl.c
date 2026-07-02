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

#include <inttypes.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include <libavutil/hwcontext_oh.h>
#include <libplacebo/vulkan.h>

#include "hwdec_ohcodec.h"

#include "video/out/gpu/spirv.h"
#include "video/out/placebo/ra_pl.h"

#if defined(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS)
#define OH_NATIVE_BUFFER_HANDLE_TYPE \
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS
#else
#define OH_NATIVE_BUFFER_HANDLE_TYPE \
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS
#endif

#define DESCRIPTOR_POOL_SIZE 64

struct pl_import {
    struct pl_import *next;
    struct mp_image *src;
    struct ra_tex *ratex;
    pl_tex pltex;

    OH_NativeBuffer *native_buffer;
    VkImage input_image;
    VkDeviceMemory input_memory;
    VkImageView input_view;

    VkImage output_image;
    VkImageView output_view;
    VkDescriptorSet descriptor_set;
    VkCommandBuffer command_buffer;
    bool output_held;
    uint64_t wait_value;
};

struct pl_mapper_priv {
    pl_gpu gpu;
    pl_vulkan vk;
    PFN_vkGetNativeBufferPropertiesOHOS get_native_buffer_properties;
    VkQueue queue;
    uint32_t queue_family;

    VkSemaphore timeline;
    uint64_t timeline_value;
    VkCommandPool command_pool;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkSamplerYcbcrConversion conversion;
    VkSampler input_sampler;
    VkFormat input_format;
    uint64_t input_external_format;
    VkSamplerYcbcrModelConversion input_ycbcr_model;
    VkSamplerYcbcrRange input_ycbcr_range;
    VkComponentMapping input_components;
    VkChromaLocation input_x_chroma_offset;
    VkChromaLocation input_y_chroma_offset;
    VkFilter input_chroma_filter;
    bool conversion_ready;
    pl_fmt output_fmt;

    struct pl_import *current;
    struct pl_import *pending;
};

static const char vertex_shader[] =
    "#version 450\n"
    "layout(location=0) out vec2 texcoord;\n"
    "void main() {\n"
    "    const vec2 pos[3] = vec2[3](vec2(-1.0, -1.0),\n"
    "                                vec2( 3.0, -1.0),\n"
    "                                vec2(-1.0,  3.0));\n"
    "    const vec2 uv[3] = vec2[3](vec2(0.0, 0.0),\n"
    "                               vec2(2.0, 0.0),\n"
    "                               vec2(0.0, 2.0));\n"
    "    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);\n"
    "    texcoord = uv[gl_VertexIndex];\n"
    "}\n";

static const char fragment_shader[] =
    "#version 450\n"
    "layout(set=0, binding=0) uniform sampler2D source;\n"
    "layout(location=0) in vec2 texcoord;\n"
    "layout(location=0) out vec4 color;\n"
    "void main() {\n"
    "    color = texture(source, texcoord);\n"
    "}\n";

static struct pl_mapper_priv *pl_priv(struct ohcodec_mapper_priv *p)
{
    return p->priv;
}

static bool has_extension(pl_vulkan vk, const char *name)
{
    for (int n = 0; n < vk->num_extensions; n++) {
        if (strcmp(vk->extensions[n], name) == 0)
            return true;
    }
    return false;
}

static bool find_memory_type(pl_vulkan vk, uint32_t type_bits,
                             uint32_t *type_index)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(vk->phys_device, &props);

    for (uint32_t n = 0; n < props.memoryTypeCount; n++) {
        if ((type_bits & (1u << n)) &&
            (props.memoryTypes[n].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            *type_index = n;
            return true;
        }
    }

    for (uint32_t n = 0; n < props.memoryTypeCount; n++) {
        if (type_bits & (1u << n)) {
            *type_index = n;
            return true;
        }
    }

    return false;
}

static bool create_shader_module(struct ra_hwdec_mapper *mapper,
                                 enum glsl_shader type, const char *source,
                                 VkShaderModule *module)
{
    struct ra_ctx *ctx = mapper->owner->ra_ctx;
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    void *tmp = talloc_new(NULL);
    struct bstr spirv = {0};

    if (!ctx->spirv && !spirv_compiler_init(ctx))
        goto error;
    if (!ctx->spirv->fns->compile_glsl(ctx->spirv, tmp, type, source, &spirv))
        goto error;

    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv.len,
        .pCode = (const uint32_t *)spirv.start,
    };
    VkResult res = vkCreateShaderModule(v->vk->device, &info, NULL, module);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkCreateShaderModule failed: %d\n", res);
        goto error;
    }

    talloc_free(tmp);
    return true;

error:
    talloc_free(tmp);
    return false;
}

static bool create_pipeline(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    VkDevice device = v->vk->device;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &v->input_sampler,
    };
    VkDescriptorSetLayoutCreateInfo descriptor_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    res = vkCreateDescriptorSetLayout(device, &descriptor_info, NULL,
                                       &v->descriptor_layout);
    if (res != VK_SUCCESS)
        goto error;

    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &v->descriptor_layout,
    };
    res = vkCreatePipelineLayout(device, &layout_info, NULL,
                                  &v->pipeline_layout);
    if (res != VK_SUCCESS)
        goto error;

    if (!create_shader_module(mapper, GLSL_SHADER_VERTEX, vertex_shader, &vert) ||
        !create_shader_module(mapper, GLSL_SHADER_FRAGMENT, fragment_shader, &frag))
        goto error;

    VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
        },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                          VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = MP_ARRAY_SIZE(dynamic_states),
        .pDynamicStates = dynamic_states,
    };
    VkFormat output_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &output_format,
    };
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = MP_ARRAY_SIZE(stages),
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = v->pipeline_layout,
    };
    res = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                    NULL, &v->pipeline);
    if (res != VK_SUCCESS)
        goto error;

    vkDestroyShaderModule(device, frag, NULL);
    vkDestroyShaderModule(device, vert, NULL);
    return true;

error:
    MP_ERR(mapper, "Failed creating OHCodec conversion pipeline: %d\n", res);
    vkDestroyShaderModule(device, frag, NULL);
    vkDestroyShaderModule(device, vert, NULL);
    return false;
}

static void destroy_conversion_resources(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    if (!v || !v->vk)
        return;

    VkDevice device = v->vk->device;
    vkDestroyPipeline(device, v->pipeline, NULL);
    vkDestroyPipelineLayout(device, v->pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, v->descriptor_layout, NULL);
    vkDestroySampler(device, v->input_sampler, NULL);
    vkDestroySamplerYcbcrConversion(device, v->conversion, NULL);

    v->pipeline = VK_NULL_HANDLE;
    v->pipeline_layout = VK_NULL_HANDLE;
    v->descriptor_layout = VK_NULL_HANDLE;
    v->input_sampler = VK_NULL_HANDLE;
    v->conversion = VK_NULL_HANDLE;
    v->conversion_ready = false;
}

static bool same_components(VkComponentMapping a, VkComponentMapping b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static bool conversion_matches(struct pl_mapper_priv *v,
                               const VkNativeBufferFormatPropertiesOHOS *props,
                               VkFilter chroma_filter)
{
    return v->conversion_ready &&
           v->input_format == props->format &&
           v->input_external_format == props->externalFormat &&
           v->input_ycbcr_model == props->suggestedYcbcrModel &&
           v->input_ycbcr_range == props->suggestedYcbcrRange &&
           same_components(v->input_components,
                           props->samplerYcbcrConversionComponents) &&
           v->input_x_chroma_offset == props->suggestedXChromaOffset &&
           v->input_y_chroma_offset == props->suggestedYChromaOffset &&
           v->input_chroma_filter == chroma_filter;
}

static bool create_conversion_resources(
    struct ra_hwdec_mapper *mapper,
    const VkNativeBufferFormatPropertiesOHOS *props,
    VkFilter chroma_filter)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    VkDevice device = v->vk->device;

    VkExternalFormatOHOS external_format = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
        .externalFormat = props->format == VK_FORMAT_UNDEFINED
                        ? props->externalFormat : 0,
    };
    VkSamplerYcbcrConversionCreateInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .pNext = props->format == VK_FORMAT_UNDEFINED
               ? &external_format : NULL,
        .format = props->format,
        .ycbcrModel = props->suggestedYcbcrModel,
        .ycbcrRange = props->suggestedYcbcrRange,
        .components = props->samplerYcbcrConversionComponents,
        .xChromaOffset = props->suggestedXChromaOffset,
        .yChromaOffset = props->suggestedYChromaOffset,
        .chromaFilter = chroma_filter,
    };
    VkResult res = vkCreateSamplerYcbcrConversion(device, &conversion_info,
                                                   NULL, &v->conversion);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkCreateSamplerYcbcrConversion failed: %d\n", res);
        goto error;
    }

    VkSamplerYcbcrConversionInfo conversion_link = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = v->conversion,
    };
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &conversion_link,
        // Match Huawei's external-memory sample: image filtering stays nearest,
        // while chroma reconstruction is selected in conversion_info above.
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0,
        .compareOp = VK_COMPARE_OP_NEVER,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    res = vkCreateSampler(device, &sampler_info, NULL, &v->input_sampler);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkCreateSampler for OHCodec buffer failed: %d\n", res);
        goto error;
    }

    // A sampler using VkSamplerYcbcrConversion must be immutable in the
    // descriptor set layout. create_pipeline() consumes input_sampler here.
    if (!create_pipeline(mapper))
        goto error;

    v->input_format = props->format;
    v->input_external_format = props->externalFormat;
    v->input_ycbcr_model = props->suggestedYcbcrModel;
    v->input_ycbcr_range = props->suggestedYcbcrRange;
    v->input_components = props->samplerYcbcrConversionComponents;
    v->input_x_chroma_offset = props->suggestedXChromaOffset;
    v->input_y_chroma_offset = props->suggestedYChromaOffset;
    v->input_chroma_filter = chroma_filter;
    v->conversion_ready = true;

    MP_VERBOSE(mapper, "Created immutable OHCodec YCbCr sampler "
               "(format=%d external=%" PRIu64 " model=%d range=%d "
               "components=%d/%d/%d/%d chroma=%d/%d filter=%d)\n",
               props->format, props->externalFormat,
               props->suggestedYcbcrModel, props->suggestedYcbcrRange,
               props->samplerYcbcrConversionComponents.r,
               props->samplerYcbcrConversionComponents.g,
               props->samplerYcbcrConversionComponents.b,
               props->samplerYcbcrConversionComponents.a,
               props->suggestedXChromaOffset,
               props->suggestedYChromaOffset, chroma_filter);
    return true;

error:
    destroy_conversion_resources(mapper);
    return false;
}

static void destroy_import(struct ra_hwdec_mapper *mapper,
                           struct pl_import *entry)
{
    if (!entry)
        return;

    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    VkDevice device = v->vk->device;

    if (entry->command_buffer)
        vkFreeCommandBuffers(device, v->command_pool, 1, &entry->command_buffer);
    if (entry->descriptor_set)
        vkFreeDescriptorSets(device, v->descriptor_pool, 1,
                             &entry->descriptor_set);

    vkDestroyImageView(device, entry->output_view, NULL);
    if (mapper->tex[0] == entry->ratex)
        mapper->tex[0] = NULL;
    if (entry->ratex) {
        ra_tex_free(mapper->ra, &entry->ratex);
        entry->pltex = NULL;
    } else if (entry->pltex) {
        pl_tex_destroy(v->gpu, &entry->pltex);
    }

    vkDestroyImageView(device, entry->input_view, NULL);
    vkDestroyImage(device, entry->input_image, NULL);
    vkFreeMemory(device, entry->input_memory, NULL);

    if (entry->native_buffer)
        OH_NativeBuffer_Unreference(entry->native_buffer);
    mp_image_unrefp(&entry->src);
    talloc_free(entry);
}

static void cleanup_imports(struct ra_hwdec_mapper *mapper, bool force)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    uint64_t completed = 0;

    if (!force) {
        VkResult res = vkGetSemaphoreCounterValue(v->vk->device, v->timeline,
                                                   &completed);
        if (res != VK_SUCCESS) {
            MP_WARN(mapper, "vkGetSemaphoreCounterValue failed: %d\n", res);
            return;
        }
    }

    struct pl_import **link = &v->pending;
    while (*link) {
        struct pl_import *entry = *link;
        if (!force && entry->wait_value > completed) {
            link = &entry->next;
            continue;
        }

        *link = entry->next;
        destroy_import(mapper, entry);
    }
}

static bool ensure_conversion_resources(
    struct ra_hwdec_mapper *mapper,
    const VkNativeBufferFormatPropertiesOHOS *props,
    VkFilter chroma_filter)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);

    if (conversion_matches(v, props, chroma_filter))
        return true;

    if (v->conversion_ready) {
        MP_WARN(mapper, "OHCodec NativeBuffer YCbCr properties changed; "
                "recreating immutable sampler and pipeline\n");
        pl_gpu_finish(v->gpu);
        cleanup_imports(mapper, true);
        destroy_conversion_resources(mapper);
    }

    return create_conversion_resources(mapper, props, chroma_filter);
}

static bool allocate_frame_resources(struct ra_hwdec_mapper *mapper,
                                     struct pl_import *entry,
                                     int width, int height)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    VkDevice device = v->vk->device;
    VkResult res;

    entry->pltex = pl_tex_create(v->gpu, pl_tex_params(
        .w = width,
        .h = height,
        .format = v->output_fmt,
        .sampleable = true,
        .renderable = true,
    ));
    if (!entry->pltex) {
        MP_ERR(mapper, "Failed creating OHCodec conversion texture\n");
        return false;
    }

    VkFormat output_format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags output_usage = 0;
    entry->output_image = pl_vulkan_unwrap(v->gpu, entry->pltex,
                                            &output_format, &output_usage);
    if (!entry->output_image || output_format != VK_FORMAT_R8G8B8A8_UNORM ||
        !(output_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    {
        MP_ERR(mapper, "Unexpected OHCodec conversion texture format=%d usage=0x%x\n",
               output_format, output_usage);
        return false;
    }

    VkImageViewCreateInfo output_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = entry->output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = output_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    res = vkCreateImageView(device, &output_view_info, NULL, &entry->output_view);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkCreateImageView for conversion output failed: %d\n", res);
        return false;
    }

    VkDescriptorSetAllocateInfo descriptor_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = v->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &v->descriptor_layout,
    };
    res = vkAllocateDescriptorSets(device, &descriptor_alloc,
                                    &entry->descriptor_set);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkAllocateDescriptorSets failed: %d\n", res);
        return false;
    }

    VkCommandBufferAllocateInfo command_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = v->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    res = vkAllocateCommandBuffers(device, &command_alloc,
                                    &entry->command_buffer);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkAllocateCommandBuffers failed: %d\n", res);
        return false;
    }

    entry->ratex = talloc_ptrtype(NULL, entry->ratex);
    if (!entry->ratex ||
        !mppl_wrap_tex(mapper->ra, entry->pltex, entry->ratex))
    {
        MP_ERR(mapper, "Failed wrapping OHCodec conversion texture in mpv RA\n");
        return false;
    }

    return true;
}

static bool record_conversion(struct ra_hwdec_mapper *mapper,
                              struct pl_import *entry,
                              int width, int height)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    VkCommandBuffer cmd = entry->command_buffer;
    VkResult res;

    VkDescriptorImageInfo image_info = {
        .sampler = v->input_sampler,
        .imageView = entry->input_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet descriptor_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = entry->descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
    };
    vkUpdateDescriptorSets(v->vk->device, 1, &descriptor_write, 0, NULL);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    res = vkBeginCommandBuffer(cmd, &begin_info);
    if (res != VK_SUCCESS)
        goto error;

    VkImageMemoryBarrier2 input_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = entry->input_image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &input_barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkRenderingAttachmentInfo color_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = entry->output_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {
            .extent = {width, height},
        },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment,
    };
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            v->pipeline_layout, 0, 1,
                            &entry->descriptor_set, 0, NULL);

    VkViewport viewport = {
        .width = width,
        .height = height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = {width, height},
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS)
        goto error;
    return true;

error:
    MP_ERR(mapper, "Failed recording OHCodec conversion: %d\n", res);
    return false;
}

static bool submit_conversion(struct ra_hwdec_mapper *mapper,
                              struct pl_import *entry)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);

    uint64_t acquire_value = ++v->timeline_value;
    bool ok = pl_vulkan_hold_ex(v->gpu, pl_vulkan_hold_params(
        .tex = entry->pltex,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .qf = v->queue_family,
        .semaphore = (pl_vulkan_sem) {
            .sem = v->timeline,
            .value = acquire_value,
        },
    ));
    if (!ok) {
        MP_ERR(mapper, "Failed acquiring OHCodec conversion output\n");
        return false;
    }
    entry->output_held = true;

    uint64_t converted_value = ++v->timeline_value;
    VkSemaphoreSubmitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = v->timeline,
        .value = acquire_value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkCommandBufferSubmitInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = entry->command_buffer,
    };
    VkSemaphoreSubmitInfo signal_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = v->timeline,
        .value = converted_value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait_info,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_info,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal_info,
    };

    v->vk->lock_queue(v->vk, v->queue_family, 0);
    VkResult res = vkQueueSubmit2(v->queue, 1, &submit, VK_NULL_HANDLE);
    v->vk->unlock_queue(v->vk, v->queue_family, 0);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkQueueSubmit2 for OHCodec conversion failed: %d\n", res);
        return false;
    }

    pl_vulkan_release_ex(v->gpu, pl_vulkan_release_params(
        .tex = entry->pltex,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .qf = v->queue_family,
        .semaphore = (pl_vulkan_sem) {
            .sem = v->timeline,
            .value = converted_value,
        },
    ));
    entry->output_held = false;
    return true;
}

static bool create_import(struct ra_hwdec_mapper *mapper,
                          const AVOHCodecFrameDescriptor *desc)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    VkDevice device = v->vk->device;
    VkResult res;

    struct pl_import *entry = talloc_zero(v, struct pl_import);
    if (!entry)
        return false;

    entry->native_buffer = ohcodec_get_native_buffer(mapper, desc);
    if (!entry->native_buffer)
        goto error;

    OH_NativeBuffer_Config config = {0};
    OH_NativeBuffer_GetConfig(entry->native_buffer, &config);
    if (config.width <= 0 || config.height <= 0) {
        MP_ERR(mapper, "Invalid OHCodec buffer size %dx%d\n",
               config.width, config.height);
        goto error;
    }

    VkNativeBufferFormatPropertiesOHOS format_props = {
        .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS,
    };
    VkNativeBufferPropertiesOHOS buffer_props = {
        .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS,
        .pNext = &format_props,
    };
    res = v->get_native_buffer_properties(device, entry->native_buffer,
                                           &buffer_props);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkGetNativeBufferPropertiesOHOS failed: %d\n", res);
        goto error;
    }

    if (!buffer_props.allocationSize || !buffer_props.memoryTypeBits ||
        !(format_props.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        (format_props.format == VK_FORMAT_UNDEFINED && !format_props.externalFormat))
    {
        MP_ERR(mapper, "OHCodec NativeBuffer is not Vulkan-sampleable "
               "(format=%d external=%" PRIu64 " features=0x%x)\n",
               format_props.format, format_props.externalFormat,
               format_props.formatFeatures);
        goto error;
    }

    VkExternalFormatOHOS external_format = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
        .externalFormat = format_props.format == VK_FORMAT_UNDEFINED
                        ? format_props.externalFormat : 0,
    };
    VkExternalMemoryImageCreateInfo external_image = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &external_format,
        .handleTypes = OH_NATIVE_BUFFER_HANDLE_TYPE,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format_props.format,
        .extent = {config.width, config.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    res = vkCreateImage(device, &image_info, NULL, &entry->input_image);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkCreateImage for OHCodec buffer failed: %d\n", res);
        goto error;
    }

    uint32_t memory_type = 0;
    if (!find_memory_type(v->vk, buffer_props.memoryTypeBits, &memory_type)) {
        MP_ERR(mapper, "No compatible Vulkan memory type for OHCodec buffer\n");
        goto error;
    }

    VkImportNativeBufferInfoOHOS import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS,
        .buffer = entry->native_buffer,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = entry->input_image,
    };
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = buffer_props.allocationSize,
        .memoryTypeIndex = memory_type,
    };
    res = vkAllocateMemory(device, &alloc_info, NULL, &entry->input_memory);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkAllocateMemory for OHCodec buffer failed: %d\n", res);
        goto error;
    }

    res = vkBindImageMemory(device, entry->input_image, entry->input_memory, 0);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkBindImageMemory for OHCodec buffer failed: %d\n", res);
        goto error;
    }

    VkFilter filter =
        format_props.formatFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT
        ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    if (!ensure_conversion_resources(mapper, &format_props, filter))
        goto error;

    VkSamplerYcbcrConversionInfo conversion_link = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = v->conversion,
    };
    VkImageViewCreateInfo input_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &conversion_link,
        .image = entry->input_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format_props.format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    res = vkCreateImageView(device, &input_view_info, NULL, &entry->input_view);
    if (res != VK_SUCCESS) {
        MP_ERR(mapper, "vkCreateImageView for OHCodec buffer failed: %d\n", res);
        goto error;
    }

    if (!allocate_frame_resources(mapper, entry, config.width, config.height) ||
        !record_conversion(mapper, entry, config.width, config.height) ||
        !submit_conversion(mapper, entry))
        goto error;

    mapper->tex[0] = entry->ratex;
    v->current = entry;
    MP_TRACE(mapper, "Converted OHCodec NativeBuffer %u on GPU "
             "(format=%d external=%" PRIu64 ")\n",
             OH_NativeBuffer_GetSeqNum(entry->native_buffer),
             format_props.format, format_props.externalFormat);
    return true;

error:
    if (entry->output_held)
        pl_gpu_finish(v->gpu);
    destroy_import(mapper, entry);
    return false;
}

static bool mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = talloc_zero(mapper, struct pl_mapper_priv);
    p->priv = v;

    if (!v)
        return false;

    v->gpu = ra_pl_get(mapper->ra);
    v->vk = pl_vulkan_get(v->gpu);
    if (!v->vk || !has_extension(v->vk, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME))
        goto error;

    v->get_native_buffer_properties = (PFN_vkGetNativeBufferPropertiesOHOS)
        vkGetDeviceProcAddr(v->vk->device, "vkGetNativeBufferPropertiesOHOS");
    if (!v->get_native_buffer_properties)
        goto error;

    v->queue_family = v->vk->queue_graphics.index;
    vkGetDeviceQueue(v->vk->device, v->queue_family, 0, &v->queue);
    if (!v->queue)
        goto error;

    v->timeline = pl_vulkan_sem_create(v->gpu, pl_vulkan_sem_params(
        .type = VK_SEMAPHORE_TYPE_TIMELINE,
    ));
    if (!v->timeline)
        goto error;

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = v->queue_family,
    };
    VkResult res = vkCreateCommandPool(v->vk->device, &command_pool_info, NULL,
                                        &v->command_pool);
    if (res != VK_SUCCESS)
        goto error;

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = DESCRIPTOR_POOL_SIZE,
    };
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = DESCRIPTOR_POOL_SIZE,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    res = vkCreateDescriptorPool(v->vk->device, &descriptor_pool_info, NULL,
                                  &v->descriptor_pool);
    if (res != VK_SUCCESS)
        goto error;

    v->output_fmt = pl_find_named_fmt(v->gpu, "rgba8");
    if (!v->output_fmt || !(v->output_fmt->caps & PL_FMT_CAP_SAMPLEABLE) ||
        !(v->output_fmt->caps & PL_FMT_CAP_RENDERABLE))
    {
        MP_ERR(mapper, "Vulkan rgba8 render target is unavailable\n");
        goto error;
    }

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = IMGFMT_RGB0;
    mapper->dst_params.hw_subfmt = 0;
    mp_image_params_guess_csp(&mapper->dst_params);
    return true;

error:
    return false;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    if (!v)
        return;

    if (v->gpu)
        pl_gpu_finish(v->gpu);
    if (v->vk) {
        cleanup_imports(mapper, true);
        destroy_import(mapper, v->current);
        v->current = NULL;

        VkDevice device = v->vk->device;
        destroy_conversion_resources(mapper);
        vkDestroyDescriptorPool(device, v->descriptor_pool, NULL);
        vkDestroyCommandPool(device, v->command_pool, NULL);
    }
    if (v->gpu && v->timeline)
        pl_vulkan_sem_destroy(v->gpu, &v->timeline);
    talloc_free(v);
    p->priv = NULL;
}

static bool mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    const AVOHCodecFrameDescriptor *desc = ohcodec_mapper_frame_desc(mapper);
    if (!v || !desc)
        return false;

    cleanup_imports(mapper, false);
    return create_import(mapper, desc);
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct ohcodec_mapper_priv *p = mapper->priv;
    struct pl_mapper_priv *v = pl_priv(p);
    if (!v || !v->current)
        return;

    struct pl_import *entry = v->current;
    v->current = NULL;
    entry->src = mp_image_new_ref(mapper->src);
    entry->ratex = mapper->tex[0];
    mapper->tex[0] = NULL;

    uint64_t value = ++v->timeline_value;
    bool ok = entry->src && pl_vulkan_hold_ex(v->gpu, pl_vulkan_hold_params(
        .tex = entry->pltex,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = (pl_vulkan_sem) {
            .sem = v->timeline,
            .value = value,
        },
    ));

    if (!ok) {
        MP_ERR(mapper, "Failed synchronizing OHCodec conversion texture release\n");
        pl_gpu_finish(v->gpu);
        destroy_import(mapper, entry);
        return;
    }

    entry->wait_value = value;
    entry->next = v->pending;
    v->pending = entry;
    cleanup_imports(mapper, false);
}

bool ohcodec_interop_pl_init(struct ra_hwdec *hw)
{
    struct ohcodec_priv *p = hw->priv;
    pl_gpu gpu = ra_pl_get(hw->ra_ctx->ra);
    pl_vulkan vk = gpu ? pl_vulkan_get(gpu) : NULL;

    if (!vk || !has_extension(vk, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME))
        return false;

    MP_VERBOSE(hw, "OHCodec is using Vulkan NativeBuffer GPU conversion backend\n");

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
