#include "render/render.h"
#include "render_model.h"

#include "core/assert.h"

#include <volk.h>

#include <cstdio>

namespace render {

namespace {

VkShaderModule load_shader_module(VkDevice device, const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    ASSERT_MSG(file != nullptr, "shader SPIR-V not found");
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    ASSERT_MSG(size > 0 && (size % 4) == 0, "bad SPIR-V size");

    alignas(4) static u8 buffer[1 << 16];
    ASSERT_MSG(static_cast<u64>(size) <= sizeof(buffer), "SPIR-V too large");
    const u64 read = std::fread(buffer, 1, static_cast<u64>(size), file);
    std::fclose(file);
    ASSERT_MSG(read == static_cast<u64>(size), "short SPIR-V read");

    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = static_cast<u64>(size);
    ci.pCode = reinterpret_cast<const u32*>(buffer);
    VkShaderModule module = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateShaderModule(device, &ci, nullptr, &module) == VK_SUCCESS,
               "vkCreateShaderModule");
    return module;
}

}  // namespace

void Scene::build_pipeline(VkDevice device, VkFormat color_format, VkFormat depth_format,
                           VkDescriptorSetLayout bindless_layout, const char* vert_spv,
                           const char* frag_spv, u32 push_bytes, bool overlay,
                           VkPipeline* out_pipeline, VkPipelineLayout* out_layout) {
    const VkShaderModule vert = load_shader_module(device, vert_spv);
    const VkShaderModule frag = load_shader_module(device, frag_spv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Overlay draws are unoccluded and translucent; world draws are opaque.
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = overlay ? VK_FALSE : VK_TRUE;
    depth.depthWriteEnable = overlay ? VK_FALSE : VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (overlay) {
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = push_bytes;

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &bindless_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push;
    ASSERT_MSG(vkCreatePipelineLayout(device, &layout_ci, nullptr, out_layout) == VK_SUCCESS,
               "vkCreatePipelineLayout");

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = *out_layout;
    ASSERT_MSG(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, out_pipeline) ==
                   VK_SUCCESS,
               "vkCreateGraphicsPipelines");

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
}

void Scene::build_compute(VkDevice device, VkDescriptorSetLayout bindless_layout,
                          const char* comp_spv, u32 push_bytes, VkPipeline* out_pipeline,
                          VkPipelineLayout* out_layout) {
    const VkShaderModule comp = load_shader_module(device, comp_spv);

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = push_bytes;

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &bindless_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push;
    ASSERT_MSG(vkCreatePipelineLayout(device, &layout_ci, nullptr, out_layout) == VK_SUCCESS,
               "compute pipeline layout");

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = comp;
    ci.stage.pName = "main";
    ci.layout = *out_layout;
    ASSERT_MSG(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, out_pipeline) ==
                   VK_SUCCESS,
               "vkCreateComputePipelines");

    vkDestroyShaderModule(device, comp, nullptr);
}

void Scene::build_shadow_pipeline(VkDevice device, VkFormat depth_format,
                                  VkDescriptorSetLayout bindless_layout, const char* vert_spv,
                                  u32 push_bytes, VkPipeline* out_pipeline,
                                  VkPipelineLayout* out_layout) {
    const VkShaderModule vert = load_shader_module(device, vert_spv);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vert;
    stage.pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    // Depth bias pushes casters slightly away from the sun to fight acne.
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = VK_TRUE;
    raster.depthBiasConstantFactor = 1.5f;
    raster.depthBiasSlopeFactor = 2.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = push_bytes;
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &bindless_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push;
    ASSERT_MSG(vkCreatePipelineLayout(device, &layout_ci, nullptr, out_layout) == VK_SUCCESS,
               "shadow pipeline layout");

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering;
    ci.stageCount = 1;
    ci.pStages = &stage;
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = *out_layout;
    ASSERT_MSG(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, out_pipeline) ==
                   VK_SUCCESS,
               "shadow pipeline");

    vkDestroyShaderModule(device, vert, nullptr);
}

void Scene::build_pipelines() {
    // Push sizes match the shaders: the level pass takes mvp plus a buffer
    // index, the mesh pass reads per-draw records, the cull pass fills them.
    build_pipeline(device_, color_format_, depth_format_, bindless_layout_,
                   SHADER_DIR "/scene.vert.spv",
                   rt_ ? SHADER_DIR "/scene_rt.frag.spv" : SHADER_DIR "/scene.frag.spv",
                   sizeof(f32) * 16 + sizeof(u32) * 3, false, &pipeline_, &layout_);
    build_pipeline(device_, color_format_, depth_format_, bindless_layout_,
                   SHADER_DIR "/mesh.vert.spv",
                   rt_ ? SHADER_DIR "/mesh_rt.frag.spv" : SHADER_DIR "/mesh.frag.spv",
                   sizeof(MeshPush), false, &mesh_pipeline_, &mesh_layout_);
    build_pipeline(device_, color_format_, depth_format_, bindless_layout_,
                   SHADER_DIR "/mesh.vert.spv", SHADER_DIR "/overlay.frag.spv",
                   sizeof(MeshPush), true, &overlay_pipeline_, &overlay_layout_);
    // The sky reuses the depth-less overlay setup: it is drawn first and covers
    // the screen, so it needs no depth test, and blending an opaque colour is a
    // plain write.
    build_pipeline(device_, color_format_, depth_format_, bindless_layout_,
                   SHADER_DIR "/sky.vert.spv", SHADER_DIR "/sky.frag.spv", sizeof(SkyPush),
                   true, &sky_pipeline_, &sky_layout_);
    build_compute(device_, bindless_layout_, SHADER_DIR "/cull.comp.spv", sizeof(CullPush),
                  &cull_pipeline_, &cull_layout_);
    build_compute(device_, bindless_layout_, SHADER_DIR "/skin.comp.spv", sizeof(SkinPush),
                  &skin_pipeline_, &skin_layout_);
    build_shadow_pipeline(device_, depth_format_, bindless_layout_,
                          SHADER_DIR "/shadow_level.vert.spv", sizeof(f32) * 16 + sizeof(u32),
                          &shadow_level_pipeline_, &shadow_level_layout_);
    build_shadow_pipeline(device_, depth_format_, bindless_layout_,
                          SHADER_DIR "/shadow_mesh.vert.spv", sizeof(ShadowMeshPush),
                          &shadow_mesh_pipeline_, &shadow_mesh_layout_);
}

void Scene::destroy_pipelines() {
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyPipeline(device_, mesh_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
    vkDestroyPipeline(device_, cull_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, cull_layout_, nullptr);
    vkDestroyPipeline(device_, skin_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, skin_layout_, nullptr);
    vkDestroyPipeline(device_, shadow_level_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, shadow_level_layout_, nullptr);
    vkDestroyPipeline(device_, shadow_mesh_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, shadow_mesh_layout_, nullptr);
    vkDestroyPipeline(device_, overlay_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, overlay_layout_, nullptr);
    vkDestroyPipeline(device_, sky_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, sky_layout_, nullptr);
}

}  // namespace render
