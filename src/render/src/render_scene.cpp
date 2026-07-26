#include "render/render.h"
#include "render_math.h"
#include "render_props.h"
#include "render_scene_state.h"
#include "render_sun.h"

#include <volk.h>

#include <cmath>

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

}  // namespace

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
    model_begin(models_->viewmodel);
    model_begin(models_->trooper);
    model_begin(models_->tracer);
    model_begin(models_->hitmarker);
    model_begin(models_->overlay);
    for (const DuckSpot& spot : DUCKS) {
        const f32 half = spot.yaw * 0.5f;
        const core::Quat rot = core::Quat::from_axis_half(core::Vec3{0.0f, 1.0f, 0.0f},
                                                          std::sin(half), std::cos(half));
        model_queue(models_->duck, frame.slot, spot.pos, rot, spot.scale);
    }
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

    // Tracers: a bright beam along each recent shot, from the muzzle for the
    // player and from the eyes for everyone else.
    for (u32 i = 0; i < sim::MAX_PLAYERS; ++i) {
        const sim::Character& shooter = curr.chars[i];
        if (shooter.alive == 0 || shooter.shot_age > 2) {
            continue;
        }
        // The impact point is frozen at fire time; the player's beam origin
        // tracks the gun muzzle like the gun itself, so the tracer visually
        // leaves the barrel then converges on the true shot line.
        const f32 scp = std::cos(shooter.shot_pitch);
        const core::Vec3 shot_dir{std::sin(shooter.shot_yaw) * scp, std::sin(shooter.shot_pitch),
                                  -std::cos(shooter.shot_yaw) * scp};
        const core::Vec3 impact = core::Vec3{shooter.shot_x, shooter.shot_y, shooter.shot_z} +
                                  shot_dir * shooter.last_shot_t;
        core::Vec3 origin;
        if (i == 0) {
            const f32 ccp = std::cos(pitch);
            const core::Vec3 cam_dir{std::sin(yaw) * ccp, std::sin(pitch), -std::cos(yaw) * ccp};
            const core::Vec3 cam_right{std::cos(yaw), 0.0f, std::sin(yaw)};
            origin = core::Vec3{cam_x, cam_y + eye_height, cam_z} + cam_right * 0.17f +
                     cam_dir * 0.5f + core::Vec3{0.0f, -0.12f, 0.0f};
        } else {
            origin = core::Vec3{shooter.shot_x, shooter.shot_y, shooter.shot_z};
        }
        const core::Vec3 span = impact - origin;
        const f32 beam_len = span.length();
        if (beam_len < 0.1f) {
            continue;
        }
        const core::Vec3 dir = span * (1.0f / beam_len);
        const f32 beam_pitch =
            std::asin(dir.y > 1.0f ? 1.0f : (dir.y < -1.0f ? -1.0f : dir.y));
        const f32 beam_yaw = std::atan2(dir.x, -dir.z);
        const core::Quat q_yaw = core::Quat::from_axis_half(
            core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(beam_yaw * 0.5f), std::cos(beam_yaw * 0.5f));
        const core::Quat q_pitch = core::Quat::from_axis_half(
            core::Vec3{1.0f, 0.0f, 0.0f}, std::sin(beam_pitch * 0.5f),
            std::cos(beam_pitch * 0.5f));
        const f32 fade[4] = {1.0f - 0.3f * static_cast<f32>(shooter.shot_age),
                             1.0f - 0.3f * static_cast<f32>(shooter.shot_age), 1.0f, 1.0f};
        model_queue_stretched(models_->tracer, frame.slot, origin, q_yaw * q_pitch,
                              core::Vec3{0.02f, 0.02f, beam_len}, fade);
    }

    // The first-person viewmodel lives in camera space and is drawn with its
    // own fixed-FOV projection, so it stays rigidly glued to the view no
    // matter what the world camera does (speed FOV, roll, interpolation). The
    // muzzle kicks back for a few frames after firing.
    const f32 kick =
        curr.player().shot_age < 4 ? (4.0f - static_cast<f32>(curr.player().shot_age)) * 0.012f
                                   : 0.0f;
    model_queue(models_->viewmodel, frame.slot, core::Vec3{0.17f, -0.14f, -0.33f + kick},
                core::Quat{}, 1.0f);
    if (curr.player().shot_hit != 0 && curr.player().shot_age < 8) {
        model_queue(models_->hitmarker, frame.slot, core::Vec3{0.0f, 0.0f, -1.2f}, core::Quat{},
                    1.0f);
    }

    // Screen feedback: a red flash when hurt, a dark red shroud while dead,
    // and the match banner tint during the end phase.
    if (curr.player().alive != 0 && curr.player().hurt_age < 12) {
        const f32 a = 0.38f * (1.0f - static_cast<f32>(curr.player().hurt_age) / 12.0f);
        const f32 tint[4] = {0.9f, 0.08f, 0.08f, a};
        model_queue_tinted(models_->overlay, frame.slot, core::Vec3{}, core::Quat{}, 1.0f, tint);
    }
    if (curr.player().alive == 0) {
        const f32 tint[4] = {0.35f, 0.02f, 0.02f, 0.5f};
        model_queue_tinted(models_->overlay, frame.slot, core::Vec3{}, core::Quat{}, 1.0f, tint);
    }
    if (curr.winner != 0) {
        const bool won = (curr.winner - 1) == curr.player().team;
        const f32 tint[4] = {won ? 0.1f : 0.6f, won ? 0.5f : 0.08f, 0.12f, 0.28f};
        model_queue_tinted(models_->overlay, frame.slot, core::Vec3{}, core::Quat{}, 1.0f, tint);
    }

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

    const RenderModel* fill_models[6] = {&models_->duck,   &models_->viewmodel,
                                         &models_->trooper, &models_->tracer,
                                         &models_->hitmarker, &models_->overlay};
    for (u32 i = 0; i < 6; ++i) {
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
    // The viewmodel is always on screen by construction: cull it with planes
    // that accept everything, since its record is in camera space.
    model_cull(models_->viewmodel, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    model_cull(models_->trooper, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    model_cull(models_->tracer, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    model_cull(models_->hitmarker, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    model_cull(models_->overlay, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    // Shadow casters are culled permissively: the sun sees the whole map.
    model_cull(models_->duck, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, accept_all,
               frame.slot, PASS_SHADOW);
    model_cull(models_->trooper, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    memory_barrier(frame.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    if (rt_) {
        // Ray traced sun: rebuild the TLAS from the live instance transforms.
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
        record_rt_sun(*gpu_, frame.cmd,
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
    sun_in.casters[1] = &models_->trooper;
    sun_in.caster_count = 2;
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
    model_draw_culled(models_->trooper, frame.cmd, mesh_layout_, view_proj, globals_idx,
                      frame.slot);
    model_draw_culled(models_->tracer, frame.cmd, mesh_layout_, view_proj, globals_idx,
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
    model_draw_culled(models_->hitmarker, frame.cmd, mesh_layout_, vm_proj, globals_idx,
                      frame.slot);

    // The overlay pass draws last: unlit, blended, in NDC.
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_pipeline_);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_layout_, 0, 1,
                            &bindless_set_, 0, nullptr);
    model_draw_culled(models_->overlay, frame.cmd, overlay_layout_, Mat4{}, globals_idx,
                      frame.slot);
    vm_viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &vm_viewport);

    vkCmdEndRendering(frame.cmd);
}
}  // namespace render
