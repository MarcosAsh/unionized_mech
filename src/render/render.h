#pragma once

#include "core/arena.h"
#include "core/types.h"
#include "gpu/gpu.h"

namespace render {

/// The M0 scene pass. Owns a graphics pipeline and the scene geometry, which
/// lives in a bindless storage buffer drawn with an indexed indirect call. It
/// reads camera state from the caller and never mutates the world.
class Scene {
public:
    /// Build the pipeline and upload the scene. `scratch` is used only during
    /// construction to assemble geometry and may be reset afterwards.
    [[nodiscard]] static Scene create(gpu::Renderer& gpu, core::Arena& scratch);

    ~Scene();
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&& other) noexcept;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    /// Record the scene into `frame`, viewed from a first-person camera at the
    /// given position and orientation.
    void draw(const gpu::Frame& frame, f32 cam_x, f32 cam_y, f32 cam_z, f32 yaw, f32 pitch);

private:
    Scene() = default;

    static void build_pipeline(VkDevice device, VkFormat color_format, VkFormat depth_format,
                               VkDescriptorSetLayout bindless_layout, VkPipeline* out_pipeline,
                               VkPipelineLayout* out_layout);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    gpu::Buffer vertices_{};
    gpu::Buffer indices_{};
    gpu::Buffer indirect_{};
    u32 index_count_ = 0;
};

}  // namespace render
