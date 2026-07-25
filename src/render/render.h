#pragma once

#include "core/arena.h"
#include "core/types.h"
#include "gpu/gpu.h"
#include "sim/sim.h"

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

    /// Record the scene into `frame`, viewed from a first-person camera
    /// interpolated between the `prev` and `curr` snapshots by `alpha` in [0, 1].
    void draw(const gpu::Frame& frame, const sim::World& prev, const sim::World& curr, f32 alpha);

    /// Rebuild the pipeline if either compiled shader changed on disk. Cheap to
    /// call every frame; it only rebuilds when a SPIR-V file's timestamp moves.
    void maybe_reload();

private:
    Scene() = default;

    static void build_pipeline(VkDevice device, VkFormat color_format, VkFormat depth_format,
                               VkDescriptorSetLayout bindless_layout, VkPipeline* out_pipeline,
                               VkPipelineLayout* out_layout);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    gpu::Buffer vertices_{};
    gpu::Buffer indices_{};
    gpu::Buffer indirect_{};
    u32 index_count_ = 0;
    i64 vert_mtime_ = 0;
    i64 frag_mtime_ = 0;
};

}  // namespace render
