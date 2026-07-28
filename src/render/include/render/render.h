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

    /// Spot the one-tick events the scene has to remember for longer than a
    /// tick, currently grenade detonations. Called once per simulated tick
    /// alongside the audio, not once per frame: several ticks can run between
    /// frames, and a blast that lit and died inside one of the earlier ones
    /// would never be drawn.
    void note_events(const sim::World& before, const sim::World& after);

    /// Rebuild the pipeline if either compiled shader changed on disk. Cheap to
    /// call every frame; it only rebuilds when a SPIR-V file's timestamp moves.
    void maybe_reload();

    /// Rebuild the level geometry from the active sim level after a map reload.
    /// An editor event: re-uploads the level buffers in place.
    void reload_level(gpu::Renderer& gpu, core::Arena& scratch);

private:
    Scene() = default;

    static void build_pipeline(VkDevice device, VkFormat color_format, VkFormat depth_format,
                               VkDescriptorSetLayout bindless_layout, const char* vert_spv,
                               const char* frag_spv, u32 push_bytes, bool overlay,
                               VkPipeline* out_pipeline, VkPipelineLayout* out_layout);
    static void build_compute(VkDevice device, VkDescriptorSetLayout bindless_layout,
                              const char* comp_spv, u32 push_bytes, VkPipeline* out_pipeline,
                              VkPipelineLayout* out_layout);
    static void build_shadow_pipeline(VkDevice device, VkFormat depth_format,
                                      VkDescriptorSetLayout bindless_layout, const char* vert_spv,
                                      u32 push_bytes, VkPipeline* out_pipeline,
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
    VkPipeline shadow_level_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadow_level_layout_ = VK_NULL_HANDLE;
    VkPipeline shadow_mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadow_mesh_layout_ = VK_NULL_HANDLE;
    VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout sky_layout_ = VK_NULL_HANDLE;
    VkPipeline overlay_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout overlay_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    gpu::Buffer vertices_{};
    gpu::Buffer indices_{};
    gpu::Buffer indirect_{};
    u32 index_count_ = 0;

    SceneModels* models_ = nullptr;  ///< Imported models, in the permanent arena.
    VkImage shadow_image_ = VK_NULL_HANDLE;   ///< Borrowed from the renderer.
    VkImageView shadow_view_ = VK_NULL_HANDLE;
    u32 shadow_tex_ = 0;  ///< Bindless slot of the shadow map.
    u32 level_tex_ = 0;   ///< Bindless slot of the level surface texture.
    u32 level_normal_tex_ = 0;  ///< Its tangent-space normal map.
    u32 level_rough_tex_ = 0;   ///< Its roughness map.
    bool rt_ = false;     ///< Ray-query sun shadows instead of the shadow map.
    gpu::Renderer* gpu_ = nullptr;  ///< Borrowed; outlives the scene.
    i64 vert_mtime_ = 0;
    i64 frag_mtime_ = 0;
    f32 cur_roll_ = 0.0f;     ///< Eased camera roll, cosmetic state only.
    f32 cur_vm_lean_ = 0.0f;  ///< Eased viewmodel wall lean, -1 to 1.
    f32 cur_fov_ = 1.2217f;   ///< Eased vertical field of view in radians.
    /// Eased wallrun bank per character, in radians. Presentation state the sim
    /// does not carry, so it lives for exactly as long as the scene does.
    f32 trooper_bank_[sim::MAX_PLAYERS] = {};

    /// A fireball, kept alive for its own short lifetime after the grenade that
    /// made it has gone. Aged in ticks off the world clock rather than in
    /// seconds off the frame clock, so it needs no frame delta and survives a
    /// stall the same way the simulation does.
    struct Blast {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        u32 tick = 0;
        bool live = false;
    };
    Blast blasts_[sim::MAX_GRENADES] = {};
};

}  // namespace render
