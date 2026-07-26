#define _POSIX_C_SOURCE 200809L

#include "render/render.h"
#include "render_math.h"
#include "render_model.h"
#include "render_props.h"
#include "render_sun.h"

#include "core/array.h"
#include "core/log.h"

#include <volk.h>

#include <cmath>
#include <sys/stat.h>

namespace render {

namespace {

struct PushConstants {
    f32 view_proj[16];
    u32 vbuf;
    u32 globals;
    u32 gslot;
};


// A global execution and memory barrier between the cull pre-pass stages.
void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

i64 file_mtime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return static_cast<i64>(st.st_mtim.tv_sec) * 1000000000 + static_cast<i64>(st.st_mtim.tv_nsec);
}

}  // namespace

/// The imported models, held in the permanent arena so the public header can
/// keep them behind a forward declaration.
struct SceneModels {
    RenderModel duck;
    RenderModel sponza;
    SkinnedModel fox;
    RenderModel viewmodel;
    RenderModel trooper;
    FoxCompanion fox_state;
    gpu::Blas level_blas;
    gpu::Blas trooper_blas;
    gpu::Blas duck_blas;
    gpu::Blas sponza_blas;
    gpu::Blas fox_blas;
    gpu::Buffer globals;
    SceneGlobals* globals_mapped = nullptr;
    f32 last_anim_time = 0.0f;
    u32 white_texture = 0;
};



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
    scene.models_->sponza =
        model_load(gpu, scratch, ASSET_DIR "/sponza", scene.models_->white_texture);
    scene.models_->fox = skinned_model_load(gpu, permanent, scratch, ASSET_DIR "/fox",
                                            scene.models_->white_texture);
    scene.models_->viewmodel = make_viewmodel(gpu, scene.models_->white_texture);
    scene.models_->trooper = make_trooper(gpu, scene.models_->white_texture);

    // On ray tracing hardware, every caster gets a BLAS and the sun shadows
    // trace against the scene instead of sampling the shadow map. The fox BLAS
    // builds from bind-pose vertices here and rebuilds per frame from the
    // skinned slice.
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
        if (scene.models_->sponza.loaded) {
            scene.models_->sponza_blas = gpu.create_blas(
                scene.models_->sponza.vertices.handle, scene.models_->sponza.total_vertices,
                sizeof(asset::MeshVertex), scene.models_->sponza.indices.handle,
                scene.models_->sponza.total_indices);
        }
        scene.models_->trooper_blas = gpu.create_blas(
            scene.models_->trooper.vertices.handle, scene.models_->trooper.total_vertices,
            sizeof(asset::MeshVertex), scene.models_->trooper.indices.handle,
            scene.models_->trooper.total_indices);
        if (scene.models_->fox.base.loaded && scene.models_->fox.vertex_count > 0) {
            scene.models_->fox_blas = gpu.create_blas(
                scene.models_->fox.base.vertices.handle, scene.models_->fox.vertex_count,
                sizeof(asset::MeshVertex), scene.models_->fox.base.indices.handle,
                scene.models_->fox.base.total_indices);
        }
        gpu.create_tlas(16);
        core::log_info("render: ray traced sun shadows active");
    }
    void* globals_mapped = nullptr;
    scene.models_->globals = gpu.create_mapped_buffer(
        gpu::Renderer::frames_in_flight() * sizeof(SceneGlobals), &globals_mapped);
    scene.models_->globals_mapped = static_cast<SceneGlobals*>(globals_mapped);

    scene.build_pipelines();
    scene.vert_mtime_ = file_mtime(SHADER_DIR "/scene.vert.spv") +
                        file_mtime(SHADER_DIR "/mesh.vert.spv");
    scene.frag_mtime_ = file_mtime(SHADER_DIR "/scene.frag.spv") +
                        file_mtime(SHADER_DIR "/mesh.frag.spv");
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
    const i64 vert = file_mtime(SHADER_DIR "/scene.vert.spv") +
                     file_mtime(SHADER_DIR "/mesh.vert.spv") +
                     file_mtime(SHADER_DIR "/shadow_level.vert.spv") +
                     file_mtime(SHADER_DIR "/shadow_mesh.vert.spv");
    const i64 frag = file_mtime(SHADER_DIR "/scene.frag.spv") +
                     file_mtime(SHADER_DIR "/mesh.frag.spv");
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

void Scene::draw(const gpu::Frame& frame, const sim::World& prev, const sim::World& curr,
                 f32 alpha) {
    const f32 cam_x = lerp(prev.player().x, curr.player().x, alpha);
    const f32 cam_y = lerp(prev.player().y, curr.player().y, alpha);
    const f32 cam_z = lerp(prev.player().z, curr.player().z, alpha);
    const f32 yaw = angle_lerp(prev.player().yaw, curr.player().yaw, alpha);
    const f32 pitch = lerp(prev.player().pitch, curr.player().pitch, alpha);

    // The eye dips on landing by the stored impact, easing back as it decays.
    f32 eye_height = curr.player().ducked != 0 ? 0.9f : 1.7f;
    f32 impact = curr.player().land_impact;
    if (impact > 12.0f) {
        impact = 12.0f;
    }
    eye_height -= impact * 0.02f;

    // Camera roll eases toward the wall so engaging and leaving read as a lean
    // rather than a snap. A nearby wall while airborne gets a partial lean as
    // anticipation before the run starts. Cosmetic and render-side only.
    f32 target_roll = 0.0f;
    const bool near_wall = curr.player().wall_nx != 0.0f || curr.player().wall_nz != 0.0f;
    if (near_wall) {
        const f32 facing_x = std::sin(yaw);
        const f32 facing_z = -std::cos(yaw);
        const f32 side = facing_x * curr.player().wall_nz - facing_z * curr.player().wall_nx;
        target_roll = side * (curr.player().state == sim::MoveState::Wallrun ? 0.15f : 0.07f);
    }
    cur_roll_ += (target_roll - cur_roll_) * 0.2f;

    // Speed-linked field of view, the strongest "I am fast" signal. Widens as
    // horizontal speed climbs past run speed, eased to avoid pumping.
    const f32 hspeed = std::sqrt(curr.player().vx * curr.player().vx + curr.player().vz * curr.player().vz);
    f32 speed_frac = (hspeed - 8.0f) / 8.0f;
    if (speed_frac < 0.0f) {
        speed_frac = 0.0f;
    }
    if (speed_frac > 1.0f) {
        speed_frac = 1.0f;
    }
    const f32 target_fov = 1.2217f + 0.24f * speed_frac;
    cur_fov_ += (target_fov - cur_fov_) * 0.08f;

    const f32 aspect =
        static_cast<f32>(frame.extent.width) / static_cast<f32>(frame.extent.height);
    const Mat4 proj = perspective(cur_fov_, aspect, 0.1f, 500.0f);
    const Mat4 view = view_fps(cam_x, cam_y + eye_height, cam_z, yaw, pitch, cur_roll_);
    const Mat4 view_proj = proj * view;

    // Cull pre-pass, before the rendering pass begins: queue this frame's draw
    // records, zero the survivor counters, and let compute build the indirect
    // command buffers from whatever the frustum keeps.
    model_begin(models_->duck);
    model_begin(models_->sponza);
    model_begin(models_->fox.base);
    model_begin(models_->viewmodel);
    model_begin(models_->trooper);
    for (const DuckSpot& spot : DUCKS) {
        const f32 half = spot.yaw * 0.5f;
        const core::Quat rot = core::Quat::from_axis_half(core::Vec3{0.0f, 1.0f, 0.0f},
                                                          std::sin(half), std::cos(half));
        model_queue(models_->duck, frame.slot, spot.pos, rot, spot.scale);
    }
    model_queue(models_->sponza, frame.slot, SPONZA_POS, core::Quat{}, 1.0f);

    // The fox is a companion now: it follows the player, and its clips are
    // chosen by its actual speed, a state-driven 1D blend tree over Survey,
    // Walk, and Run. Animation is clocked off sim time so it stays smooth
    // under interpolation.
    const f32 anim_time =
        static_cast<f32>(curr.tick.raw) * sim::SIM_DT + alpha * sim::SIM_DT;
    const f32 frame_dt =
        anim_time > models_->last_anim_time ? anim_time - models_->last_anim_time : 0.0f;
    models_->last_anim_time = anim_time;

    fox_companion_update(models_->fox_state, cam_x, cam_z, frame_dt);
    u32 fox_clip_a = 0;
    u32 fox_clip_b = 0;
    f32 fox_blend = 0.0f;
    fox_gait(models_->fox_state.speed, &fox_clip_a, &fox_clip_b, &fox_blend);
    const core::Vec3 fox_pos{models_->fox_state.x, 0.0f, models_->fox_state.z};
    const core::Quat fox_rot = core::Quat::from_axis_half(
        core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(models_->fox_state.yaw * 0.5f),
        std::cos(models_->fox_state.yaw * 0.5f));
    skinned_model_queue(models_->fox, frame.slot, fox_pos, fox_rot, 0.02f);

    // Everyone else in the match, tinted by team.
    constexpr f32 TEAM_TINTS[2][4] = {{0.30f, 0.50f, 1.00f, 1.0f}, {1.00f, 0.42f, 0.22f, 1.0f}};
    for (u32 i = 1; i < sim::MAX_PLAYERS; ++i) {
        const sim::Character& other = curr.chars[i];
        if (other.alive == 0) {
            continue;
        }
        const core::Quat rot = core::Quat::from_axis_half(
            core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(other.yaw * 0.5f),
            std::cos(other.yaw * 0.5f));
        model_queue_tinted(models_->trooper, frame.slot, core::Vec3{other.x, other.y, other.z},
                           rot, 1.0f, TEAM_TINTS[other.team & 1]);
    }

    // The first-person viewmodel lives in camera space and is drawn with its
    // own fixed-FOV projection, so it stays rigidly glued to the view no
    // matter what the world camera does (speed FOV, roll, interpolation).
    model_queue(models_->viewmodel, frame.slot, core::Vec3{0.17f, -0.14f, -0.33f}, core::Quat{},
                1.0f);

    // The sun volume follows the camera: a tight orthographic box gives dense
    // shadow texels where the player is looking, and the border-white sampler
    // leaves everything beyond it lit. Snapping the sun view to whole texels
    // stops shadow edges shimmering as the camera moves.
    const Vec3 sun_dir = Vec3{0.4f, 1.0f, 0.25f}.normalized();
    constexpr f32 SUN_EXTENT = 55.0f;
    const Vec3 sun_center{cam_x, 0.0f, cam_z};
    Mat4 sun_view = look_along(sun_center + sun_dir * 100.0f, sun_dir * -1.0f);
    const f32 texel_world =
        (2.0f * SUN_EXTENT) / static_cast<f32>(gpu::Renderer::shadow_size());
    sun_view.m[12] = std::floor(sun_view.m[12] / texel_world) * texel_world;
    sun_view.m[13] = std::floor(sun_view.m[13] / texel_world) * texel_world;
    const Mat4 sun_proj =
        orthographic(-SUN_EXTENT, SUN_EXTENT, -SUN_EXTENT, SUN_EXTENT, 1.0f, 260.0f);
    const Mat4 sun_view_proj = sun_proj * sun_view;

    SceneGlobals& globals = models_->globals_mapped[frame.slot];
    for (u32 i = 0; i < 16; ++i) {
        globals.sun_view_proj[i] = sun_view_proj.m[i];
    }
    globals.sun_dir[0] = sun_dir.x;
    globals.sun_dir[1] = sun_dir.y;
    globals.sun_dir[2] = sun_dir.z;
    globals.sun_dir[3] = 0.0f;
    globals.shadow_tex = shadow_tex_;

    const RenderModel* fill_models[5] = {&models_->duck, &models_->sponza, &models_->fox.base,
                                         &models_->viewmodel, &models_->trooper};
    for (u32 i = 0; i < 5; ++i) {
        if (fill_models[i]->loaded) {
            vkCmdFillBuffer(frame.cmd, fill_models[i]->counts.handle,
                            static_cast<u64>(frame.slot) * PASS_COUNT * sizeof(u32),
                            PASS_COUNT * sizeof(u32), 0);
        }
    }
    memory_barrier(frame.cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const Frustum frustum = frustum_from(view_proj);
    f32 planes[6][4];
    for (u32 i = 0; i < 6; ++i) {
        planes[i][0] = frustum.planes[i].x;
        planes[i][1] = frustum.planes[i].y;
        planes[i][2] = frustum.planes[i].z;
        planes[i][3] = frustum.planes[i].w;
    }
    f32 accept_all[6][4] = {};
    for (u32 i = 0; i < 6; ++i) {
        accept_all[i][3] = 1.0f;
    }
    model_cull(models_->duck, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    model_cull(models_->sponza, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    model_cull(models_->fox.base, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    // The viewmodel is always on screen by construction: cull it with planes
    // that accept everything, since its record is in camera space.
    model_cull(models_->viewmodel, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    model_cull(models_->trooper, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    // Shadow casters are culled permissively: the sun sees the whole map.
    model_cull(models_->duck, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, accept_all,
               frame.slot, PASS_SHADOW);
    model_cull(models_->sponza, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    model_cull(models_->fox.base, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    model_cull(models_->trooper, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    skinned_model_update(models_->fox, frame.cmd, skin_pipeline_, skin_layout_, bindless_set_,
                         fox_clip_a, fox_clip_b, fox_blend, anim_time, frame.slot);
    memory_barrier(frame.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    if (rt_) {
        // Ray traced sun: rebuild the fox BLAS from this frame's skinned
        // vertices and the TLAS from the live instance transforms.
        VkAccelerationStructureInstanceKHR instances[16];
        u32 instance_count = 0;
        instances[instance_count++] = make_rt_instance(Mat4{}, models_->level_blas.address);
        if (models_->duck_blas.handle != VK_NULL_HANDLE) {
            for (const DuckSpot& spot : DUCKS) {
                const f32 half = spot.yaw * 0.5f;
                const core::Quat rot = core::Quat::from_axis_half(
                    core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(half), std::cos(half));
                core::Mat4 world = core::Mat4::trs(spot.pos, rot);
                world = world * core::Mat4::scale(
                                    core::Vec3{spot.scale, spot.scale, spot.scale});
                instances[instance_count++] =
                    make_rt_instance(world, models_->duck_blas.address);
            }
        }
        if (models_->sponza_blas.handle != VK_NULL_HANDLE) {
            instances[instance_count++] = make_rt_instance(
                core::Mat4::translation(SPONZA_POS), models_->sponza_blas.address);
        }
        if (models_->trooper_blas.handle != VK_NULL_HANDLE) {
            for (u32 i = 1; i < sim::MAX_PLAYERS; ++i) {
                const sim::Character& other = curr.chars[i];
                if (other.alive == 0) {
                    continue;
                }
                const core::Quat rot = core::Quat::from_axis_half(
                    core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(other.yaw * 0.5f),
                    std::cos(other.yaw * 0.5f));
                instances[instance_count++] = make_rt_instance(
                    core::Mat4::trs(core::Vec3{other.x, other.y, other.z}, rot),
                    models_->trooper_blas.address);
            }
        }
        if (models_->fox_blas.handle != VK_NULL_HANDLE) {
            core::Mat4 fox_world = core::Mat4::trs(fox_pos, fox_rot);
            fox_world = fox_world * core::Mat4::scale(core::Vec3{0.02f, 0.02f, 0.02f});
            instances[instance_count++] =
                make_rt_instance(fox_world, models_->fox_blas.address);
        }
        record_rt_sun(*gpu_, frame.cmd, models_->fox_blas,
                      models_->fox.skinned_verts.handle,
                      static_cast<u64>(frame.slot) * models_->fox.vertex_count *
                          sizeof(asset::MeshVertex),
                      core::Span<const VkAccelerationStructureInstanceKHR>(instances,
                                                                           instance_count),
                      frame.slot);
    } else {
    SunShadowInputs sun_in;
    sun_in.cmd = frame.cmd;
    sun_in.image = shadow_image_;
    sun_in.view = shadow_view_;
    sun_in.level_pipeline = shadow_level_pipeline_;
    sun_in.level_layout = shadow_level_layout_;
    sun_in.mesh_pipeline = shadow_mesh_pipeline_;
    sun_in.mesh_layout = shadow_mesh_layout_;
    sun_in.bindless = bindless_set_;
    sun_in.level_indices = indices_.handle;
    sun_in.level_indirect = indirect_.handle;
    sun_in.level_vbuf = vertices_.bindless_index;
    sun_in.casters[0] = &models_->duck;
    sun_in.casters[1] = &models_->sponza;
    sun_in.casters[2] = &models_->fox.base;
    sun_in.casters[3] = &models_->trooper;
    sun_in.caster_count = 4;
    sun_in.sun_view_proj = sun_view_proj;
    sun_in.slot = frame.slot;
    record_sun_shadow(sun_in);
    }

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = frame.color_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.45f, 0.62f, 0.85f, 1.0f}};

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = frame.depth_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = frame.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(frame.cmd, &rendering);

    VkViewport viewport{};
    viewport.width = static_cast<f32>(frame.extent.width);
    viewport.height = static_cast<f32>(frame.extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = frame.extent;
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                            &bindless_set_, 0, nullptr);
    vkCmdBindIndexBuffer(frame.cmd, indices_.handle, 0, VK_INDEX_TYPE_UINT32);

    PushConstants pc{};
    for (u32 i = 0; i < 16; ++i) {
        pc.view_proj[i] = view_proj.m[i];
    }
    pc.vbuf = vertices_.bindless_index;
    pc.globals = models_->globals.bindless_index;
    pc.gslot = frame.slot;
    vkCmdPushConstants(frame.cmd, layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(PushConstants), &pc);

    vkCmdDrawIndexedIndirect(frame.cmd, indirect_.handle, 0, 1,
                             sizeof(VkDrawIndexedIndirectCommand));

    // The textured mesh pass draws whatever the cull pass kept.
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1,
                            &bindless_set_, 0, nullptr);
    const u32 globals_idx = models_->globals.bindless_index;
    model_draw_culled(models_->duck, frame.cmd, mesh_layout_, view_proj, globals_idx, frame.slot);
    model_draw_culled(models_->sponza, frame.cmd, mesh_layout_, view_proj, globals_idx,
                      frame.slot);
    model_draw_culled(models_->fox.base, frame.cmd, mesh_layout_, view_proj, globals_idx,
                      frame.slot);
    model_draw_culled(models_->trooper, frame.cmd, mesh_layout_, view_proj, globals_idx,
                      frame.slot);
    // The viewmodel draws into a compressed near slice of the depth range so
    // world geometry can never occlude it, the classic viewmodel depth hack.
    VkViewport vm_viewport{};
    vm_viewport.width = static_cast<f32>(frame.extent.width);
    vm_viewport.height = static_cast<f32>(frame.extent.height);
    vm_viewport.maxDepth = 0.05f;
    vkCmdSetViewport(frame.cmd, 0, 1, &vm_viewport);
    const Mat4 vm_proj = perspective(1.05f, aspect, 0.05f, 10.0f);
    model_draw_culled(models_->viewmodel, frame.cmd, mesh_layout_, vm_proj, globals_idx,
                      frame.slot);
    vm_viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &vm_viewport);

    vkCmdEndRendering(frame.cmd);
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
