#pragma once

#include "core/arena.h"
#include "core/types.h"
#include "gpu/gpu.h"
#include "sim/sim.h"

namespace render {

struct SceneModels;

/// The scene pass. Owns the level geometry pipeline, a textured mesh pipeline,
/// and the imported models, all drawn bindless through indirect calls. It
/// reads camera state from the caller and never mutates the world.
class Scene {
public:
    /// Build the pipelines and upload the scene. Imported model bookkeeping
    /// lives in `permanent`; `scratch` is used only during construction and may
    /// be reset afterwards.
    [[nodiscard]] static Scene create(gpu::Renderer& gpu, core::Arena& permanent,
                                      core::Arena& scratch);

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
                               VkDescriptorSetLayout bindless_layout, const char* vert_spv,
                               const char* frag_spv, u32 push_bytes, VkPipeline* out_pipeline,
                               VkPipelineLayout* out_layout);
    static void build_compute(VkDevice device, VkDescriptorSetLayout bindless_layout,
                              const char* comp_spv, u32 push_bytes, VkPipeline* out_pipeline,
                              VkPipelineLayout* out_layout);
    void build_pipelines();
    void destroy_pipelines();

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE;
    VkPipeline cull_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout cull_layout_ = VK_NULL_HANDLE;
    VkPipeline skin_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout skin_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    gpu::Buffer vertices_{};
    gpu::Buffer indices_{};
    gpu::Buffer indirect_{};
    u32 index_count_ = 0;

    SceneModels* models_ = nullptr;  ///< Imported models, in the permanent arena.
    i64 vert_mtime_ = 0;
    i64 frag_mtime_ = 0;
    f32 cur_roll_ = 0.0f;    ///< Eased camera roll, cosmetic state only.
    f32 cur_fov_ = 1.2217f;  ///< Eased vertical field of view in radians.
};

}  // namespace render
