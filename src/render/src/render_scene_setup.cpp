#define _POSIX_C_SOURCE 200809L

// Scene setup and lifetime: resource creation, live reload, and teardown. The
// per-frame draw lives in render_scene.cpp.

#include "render/render.h"
#include "render_props.h"
#include "render_scene_state.h"
#include "render_text.h"

#include "core/array.h"
#include "core/log.h"

#include <volk.h>

#include <sys/stat.h>

namespace render {

i64 scene_file_mtime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return static_cast<i64>(st.st_mtim.tv_sec) * 1000000000 + static_cast<i64>(st.st_mtim.tv_nsec);
}

Scene Scene::create(gpu::Renderer& gpu, core::Arena& permanent, core::Arena& scratch) {
    core::Array<LevelVertex> verts = scratch.make_array<LevelVertex>(65536);
    core::Array<u32> indices = scratch.make_array<u32>(131072);
    build_level(verts, indices);

    Scene scene;
    scene.device_ = gpu.device_handle();
    scene.gpu_ = &gpu;
    scene.bindless_set_ = gpu.bindless_set();
    scene.shadow_image_ = gpu.shadow_image();
    scene.shadow_view_ = gpu.shadow_view();
    scene.shadow_tex_ = gpu.shadow_bindless();
    scene.level_tex_ = make_level_texture(gpu);
    scene.bindless_layout_ = gpu.bindless_layout();
    scene.color_format_ = gpu.color_format();
    scene.depth_format_ = gpu.depth_format();
    scene.index_count_ = static_cast<u32>(indices.size());

    // Level buffers are sized to array capacity so a map reload can re-upload
    // in place, whatever geometry the new map brings.
    scene.vertices_ = gpu.create_device_buffer(verts.as_span().data(),
                                               verts.capacity() * sizeof(LevelVertex),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    scene.indices_ = gpu.create_device_buffer(indices.as_span().data(),
                                              indices.capacity() * sizeof(u32),
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);

    VkDrawIndexedIndirectCommand draw{};
    draw.indexCount = scene.index_count_;
    draw.instanceCount = 1;
    scene.indirect_ = gpu.create_device_buffer(&draw, sizeof(draw),
                                               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);

    // Imported models. Missing files degrade to boxes-only, so the app still
    // runs from a build without the sample downloads.
    scene.models_ = permanent.alloc_one<SceneModels>();
    *scene.models_ = SceneModels{};
    const u8 white[4] = {255, 255, 255, 255};
    scene.models_->white_texture = gpu.create_texture(white, 1, 1).bindless_index;
    scene.models_->duck =
        model_load(gpu, scratch, ASSET_DIR "/duck", scene.models_->white_texture);
    scene.models_->gun =
        model_load(gpu, scratch, ASSET_DIR "/blaster", scene.models_->white_texture);
    scene.models_->viewmodel = make_viewmodel(gpu, scene.models_->white_texture);
    scene.models_->trooper = skinned_model_load(gpu, permanent, scratch, ASSET_DIR "/robot",
                                                scene.models_->white_texture, sim::MAX_PLAYERS);
    if (!scene.models_->trooper.base.loaded) {
        scene.models_->trooper.base = make_trooper(gpu, scene.models_->white_texture);
    }
    scene.models_->mech = make_mech(gpu, scene.models_->white_texture);
    scene.models_->tracer = make_tracer(gpu, scene.models_->white_texture);
    scene.models_->tracer_vm = make_tracer(gpu, scene.models_->white_texture);
    scene.models_->hitmarker = make_hitmarker(gpu, scene.models_->white_texture);
    scene.models_->overlay = make_overlay_quad(gpu, scene.models_->white_texture);
    scene.models_->font = font_build(gpu);

    // On ray tracing hardware, every caster gets a BLAS and the sun shadows
    // trace against the scene instead of sampling the shadow map.
    scene.rt_ = gpu.rt_available();
    if (scene.rt_) {
        scene.models_->level_blas =
            gpu.create_blas(scene.vertices_.handle, static_cast<u32>(verts.size()),
                            sizeof(LevelVertex), scene.indices_.handle,
                            static_cast<u32>(indices.size()));
        if (scene.models_->duck.loaded) {
            scene.models_->duck_blas = gpu.create_blas(
                scene.models_->duck.vertices.handle, scene.models_->duck.total_vertices,
                sizeof(asset::MeshVertex), scene.models_->duck.indices.handle,
                scene.models_->duck.total_indices);
        }
        scene.models_->trooper_blas = gpu.create_blas(
            scene.models_->trooper.base.vertices.handle,
            scene.models_->trooper.base.total_vertices, sizeof(asset::MeshVertex),
            scene.models_->trooper.base.indices.handle,
            scene.models_->trooper.base.total_indices);
        scene.models_->mech_blas = gpu.create_blas(
            scene.models_->mech.vertices.handle, scene.models_->mech.total_vertices,
            sizeof(asset::MeshVertex), scene.models_->mech.indices.handle,
            scene.models_->mech.total_indices);
        gpu.create_tlas(16);
        core::log_info("render: ray traced sun shadows active");
    }
    void* globals_mapped = nullptr;
    scene.models_->globals = gpu.create_mapped_buffer(
        gpu::Renderer::frames_in_flight() * sizeof(SceneGlobals), &globals_mapped);
    scene.models_->globals_mapped = static_cast<SceneGlobals*>(globals_mapped);

    scene.build_pipelines();
    scene.vert_mtime_ = scene_file_mtime(SHADER_DIR "/scene.vert.spv") +
                        scene_file_mtime(SHADER_DIR "/mesh.vert.spv");
    scene.frag_mtime_ = scene_file_mtime(SHADER_DIR "/scene.frag.spv") +
                        scene_file_mtime(SHADER_DIR "/mesh.frag.spv");
    return scene;
}

void Scene::reload_level(gpu::Renderer& gpu, core::Arena& scratch) {
    const u64 marker = scratch.marker();
    core::Array<LevelVertex> verts = scratch.make_array<LevelVertex>(65536);
    core::Array<u32> indices = scratch.make_array<u32>(131072);
    build_level(verts, indices);

    gpu.update_device_buffer(vertices_, verts.as_span().data(),
                             verts.size() * sizeof(LevelVertex));
    gpu.update_device_buffer(indices_, indices.as_span().data(), indices.size() * sizeof(u32));
    index_count_ = static_cast<u32>(indices.size());
    VkDrawIndexedIndirectCommand draw{};
    draw.indexCount = index_count_;
    draw.instanceCount = 1;
    gpu.update_device_buffer(indirect_, &draw, sizeof(draw));
    scratch.rewind(marker);
    core::log_info("level reloaded");
}

void Scene::maybe_reload() {
    const i64 vert = scene_file_mtime(SHADER_DIR "/scene.vert.spv") +
                     scene_file_mtime(SHADER_DIR "/mesh.vert.spv") +
                     scene_file_mtime(SHADER_DIR "/shadow_level.vert.spv") +
                     scene_file_mtime(SHADER_DIR "/shadow_mesh.vert.spv");
    const i64 frag = scene_file_mtime(SHADER_DIR "/scene.frag.spv") +
                     scene_file_mtime(SHADER_DIR "/mesh.frag.spv");
    if (vert == vert_mtime_ && frag == frag_mtime_) {
        return;
    }
    vkDeviceWaitIdle(device_);  // reload is an event, not the steady frame path
    destroy_pipelines();
    build_pipelines();
    vert_mtime_ = vert;
    frag_mtime_ = frag;
    core::log_info("shaders reloaded");
}

Scene::~Scene() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);  // shutdown: the pipelines may still be in flight
    destroy_pipelines();
}

Scene::Scene(Scene&& other) noexcept { *this = static_cast<Scene&&>(other); }

Scene& Scene::operator=(Scene&& other) noexcept {
    if (this != &other) {
        device_ = other.device_;
        pipeline_ = other.pipeline_;
        layout_ = other.layout_;
        mesh_pipeline_ = other.mesh_pipeline_;
        mesh_layout_ = other.mesh_layout_;
        bindless_set_ = other.bindless_set_;
        bindless_layout_ = other.bindless_layout_;
        color_format_ = other.color_format_;
        depth_format_ = other.depth_format_;
        vertices_ = other.vertices_;
        indices_ = other.indices_;
        indirect_ = other.indirect_;
        index_count_ = other.index_count_;
        models_ = other.models_;
        vert_mtime_ = other.vert_mtime_;
        frag_mtime_ = other.frag_mtime_;
        cur_roll_ = other.cur_roll_;
        cur_fov_ = other.cur_fov_;
        other.device_ = VK_NULL_HANDLE;
        other.pipeline_ = VK_NULL_HANDLE;
        other.layout_ = VK_NULL_HANDLE;
        other.mesh_pipeline_ = VK_NULL_HANDLE;
        other.mesh_layout_ = VK_NULL_HANDLE;
    }
    return *this;
}
}  // namespace render
