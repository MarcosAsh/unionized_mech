#define _POSIX_C_SOURCE 200809L

#include "render/render.h"
#include "render_math.h"
#include "render_model.h"
#include "render_props.h"

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

struct ShadowLevelPush {
    f32 sun_view_proj[16];
    u32 vbuf;
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

// A depth image layout transition for the shadow pass.
void shadow_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                    VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
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
    FoxCompanion fox_state;
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
    const f32 cam_x = lerp(prev.cam_x, curr.cam_x, alpha);
    const f32 cam_y = lerp(prev.cam_y, curr.cam_y, alpha);
    const f32 cam_z = lerp(prev.cam_z, curr.cam_z, alpha);
    const f32 yaw = angle_lerp(prev.cam_yaw, curr.cam_yaw, alpha);
    const f32 pitch = lerp(prev.cam_pitch, curr.cam_pitch, alpha);

    // The eye dips on landing by the stored impact, easing back as it decays.
    f32 eye_height = curr.ducked != 0 ? 0.9f : 1.7f;
    f32 impact = curr.land_impact;
    if (impact > 12.0f) {
        impact = 12.0f;
    }
    eye_height -= impact * 0.02f;

    // Camera roll eases toward the wall so engaging and leaving read as a lean
    // rather than a snap. A nearby wall while airborne gets a partial lean as
    // anticipation before the run starts. Cosmetic and render-side only.
    f32 target_roll = 0.0f;
    const bool near_wall = curr.wall_nx != 0.0f || curr.wall_nz != 0.0f;
    if (near_wall) {
        const f32 facing_x = std::sin(yaw);
        const f32 facing_z = -std::cos(yaw);
        const f32 side = facing_x * curr.wall_nz - facing_z * curr.wall_nx;
        target_roll = side * (curr.state == sim::MoveState::Wallrun ? 0.15f : 0.07f);
    }
    cur_roll_ += (target_roll - cur_roll_) * 0.2f;

    // Speed-linked field of view, the strongest "I am fast" signal. Widens as
    // horizontal speed climbs past run speed, eased to avoid pumping.
    const f32 hspeed = std::sqrt(curr.vel_x * curr.vel_x + curr.vel_z * curr.vel_z);
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

    // The first-person viewmodel lives in camera space and is drawn with its
    // own fixed-FOV projection, so it stays rigidly glued to the view no
    // matter what the world camera does (speed FOV, roll, interpolation).
    model_queue(models_->viewmodel, frame.slot, core::Vec3{0.17f, -0.14f, -0.33f}, core::Quat{},
                1.0f);

    // The sun: a fixed direction over the map, orthographic shadow volume
    // covering the whole play area. The per-frame globals carry it to shaders.
    const Vec3 sun_dir = Vec3{0.4f, 1.0f, 0.25f}.normalized();
    const Vec3 sun_center{0.0f, 0.0f, -20.0f};
    const Mat4 sun_view = look_along(sun_center + sun_dir * 120.0f, sun_dir * -1.0f);
    const Mat4 sun_proj = orthographic(-130.0f, 130.0f, -130.0f, 130.0f, 1.0f, 300.0f);
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

    const RenderModel* fill_models[4] = {&models_->duck, &models_->sponza, &models_->fox.base,
                                         &models_->viewmodel};
    for (u32 i = 0; i < 4; ++i) {
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
    // Shadow casters are culled permissively: the sun sees the whole map.
    model_cull(models_->duck, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, accept_all,
               frame.slot, PASS_SHADOW);
    model_cull(models_->sponza, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    model_cull(models_->fox.base, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    skinned_model_update(models_->fox, frame.cmd, skin_pipeline_, skin_layout_, bindless_set_,
                         fox_clip_a, fox_clip_b, fox_blend, anim_time, frame.slot);
    memory_barrier(frame.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    // The sun shadow pass: depth only, from the sun's orthographic view. The
    // previous frame's read finished before this slot was reacquired, so the
    // transition can come from undefined.
    shadow_barrier(frame.cmd, shadow_image_, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    {
        VkRenderingAttachmentInfo shadow_depth{};
        shadow_depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        shadow_depth.imageView = shadow_view_;
        shadow_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        shadow_depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        shadow_depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        shadow_depth.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo shadow_pass{};
        shadow_pass.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        shadow_pass.renderArea.extent = {gpu::Renderer::shadow_size(),
                                         gpu::Renderer::shadow_size()};
        shadow_pass.layerCount = 1;
        shadow_pass.pDepthAttachment = &shadow_depth;
        vkCmdBeginRendering(frame.cmd, &shadow_pass);

        VkViewport sun_viewport{};
        sun_viewport.width = static_cast<f32>(gpu::Renderer::shadow_size());
        sun_viewport.height = static_cast<f32>(gpu::Renderer::shadow_size());
        sun_viewport.maxDepth = 1.0f;
        vkCmdSetViewport(frame.cmd, 0, 1, &sun_viewport);
        VkRect2D sun_scissor{};
        sun_scissor.extent = shadow_pass.renderArea.extent;
        vkCmdSetScissor(frame.cmd, 0, 1, &sun_scissor);

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_level_pipeline_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_level_layout_,
                                0, 1, &bindless_set_, 0, nullptr);
        vkCmdBindIndexBuffer(frame.cmd, indices_.handle, 0, VK_INDEX_TYPE_UINT32);
        ShadowLevelPush sp{};
        for (u32 i = 0; i < 16; ++i) {
            sp.sun_view_proj[i] = sun_view_proj.m[i];
        }
        sp.vbuf = vertices_.bindless_index;
        vkCmdPushConstants(frame.cmd, shadow_level_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(sp), &sp);
        vkCmdDrawIndexedIndirect(frame.cmd, indirect_.handle, 0, 1,
                                 sizeof(VkDrawIndexedIndirectCommand));

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_mesh_pipeline_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_mesh_layout_,
                                0, 1, &bindless_set_, 0, nullptr);
        model_draw_shadow(models_->duck, frame.cmd, shadow_mesh_layout_, sun_view_proj,
                          frame.slot);
        model_draw_shadow(models_->sponza, frame.cmd, shadow_mesh_layout_, sun_view_proj,
                          frame.slot);
        model_draw_shadow(models_->fox.base, frame.cmd, shadow_mesh_layout_, sun_view_proj,
                          frame.slot);
        vkCmdEndRendering(frame.cmd);
    }
    shadow_barrier(frame.cmd, shadow_image_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

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
