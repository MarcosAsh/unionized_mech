#include "render/render.h"
#include "render_math.h"
#include "render_props.h"
#include "render_scene_state.h"
#include "render_sun.h"
#include "render_text.h"

#include <volk.h>

#include <cmath>

namespace render {

namespace {

/// The sky. One value drives the clear colour, what fog fades toward, and the
/// upper half of the ambient term, so the horizon cannot drift from the haze in
/// front of it.
constexpr f32 SKY_COLOR[3] = {0.45f, 0.62f, 0.85f};

/// Resting vertical field of view in radians: 70 degrees, the pilot value.
constexpr f32 BASE_FOV = 1.2217f;

/// How far the field of view opens at top speed, as a fraction of BASE_FOV.
constexpr f32 FOV_SPEED_SCALE = 0.1f;




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

// Queue one tracer beam from `origin` along `span` into `model`.

}  // namespace

void Scene::note_events(const sim::World& before, const sim::World& after) {
    // A grenade that was live and is not any more went off where it last was:
    // the sim clears the slot in the same tick it applies the damage, so `after`
    // no longer knows where it happened.
    for (u32 i = 0; i < sim::MAX_GRENADES; ++i) {
        if (before.grenades[i].active == 0 || after.grenades[i].active != 0) {
            continue;
        }
        blasts_[i].x = before.grenades[i].x;
        blasts_[i].y = before.grenades[i].y;
        blasts_[i].z = before.grenades[i].z;
        blasts_[i].tick = after.tick.raw;
        blasts_[i].live = true;
    }
}

void Scene::draw(const gpu::Frame& frame, const sim::World& prev, const sim::World& curr,
                 f32 alpha) {
    const f32 cam_x = lerp(prev.player().x, curr.player().x, alpha);
    const f32 cam_y = lerp(prev.player().y, curr.player().y, alpha);
    const f32 cam_z = lerp(prev.player().z, curr.player().z, alpha);
    const f32 yaw = angle_lerp(prev.player().yaw, curr.player().yaw, alpha);
    const f32 pitch = lerp(prev.player().pitch, curr.player().pitch, alpha);

    // The eye dips on landing by the stored impact, easing back as it decays.
    // A merged robot looks out of the chassis head; must match sim MECH_EYE.
    f32 eye_height = curr.player().merged != 0 ? 3.9f
                     : curr.player().ducked != 0 ? 0.9f
                                                 : 1.7f;
    f32 impact = curr.player().land_impact;
    if (impact > 12.0f) {
        impact = 12.0f;
    }
    eye_height -= impact * 0.02f;

    // Camera roll eases toward the wall so engaging and leaving read as a lean
    // rather than a snap. A nearby wall while airborne gets a partial lean as
    // anticipation before the run starts. Cosmetic and render-side only.
    f32 target_roll = 0.0f;
    f32 target_vm_lean = 0.0f;
    const bool near_wall = curr.player().wall_nx != 0.0f || curr.player().wall_nz != 0.0f;
    const bool wallrunning = curr.player().state == sim::MoveState::Wallrun;
    if (near_wall) {
        const f32 side = wall_side(yaw, curr.player().wall_nx, curr.player().wall_nz);
        target_roll = side * (wallrunning ? 0.15f : 0.07f);
        // The hands only join in on the run itself, not on the approach.
        target_vm_lean = wallrunning ? side : 0.0f;
    }
    cur_roll_ += (target_roll - cur_roll_) * LEAN_EASE;
    cur_vm_lean_ += (target_vm_lean - cur_vm_lean_) * LEAN_EASE;

    // Speed-linked field of view, the strongest "I am fast" signal. Widens as
    // horizontal speed climbs from run speed to the slide ceiling, eased to
    // avoid pumping. The bounds come from sim so retuning the controller cannot
    // silently decalibrate the cue.
    const f32 hspeed = std::sqrt(curr.player().vx * curr.player().vx + curr.player().vz * curr.player().vz);
    f32 speed_frac = (hspeed - sim::RUN_SPEED) / (sim::TOP_SPEED - sim::RUN_SPEED);
    if (speed_frac < 0.0f) {
        speed_frac = 0.0f;
    }
    if (speed_frac > 1.0f) {
        speed_frac = 1.0f;
    }
    const f32 target_fov = BASE_FOV + BASE_FOV * FOV_SPEED_SCALE * speed_frac;
    cur_fov_ += (target_fov - cur_fov_) * 0.08f;

    const f32 aspect =
        static_cast<f32>(frame.extent.width) / static_cast<f32>(frame.extent.height);
    const Mat4 proj = perspective(cur_fov_, aspect, 0.1f, 500.0f);
    const Mat4 view = view_fps(cam_x, cam_y + eye_height, cam_z, yaw, pitch, cur_roll_);
    const Mat4 view_proj = proj * view;

    FrameView frame_view{};
    frame_view.eye = core::Vec3{cam_x, cam_y + eye_height, cam_z};
    frame_view.aspect = aspect;
    TrooperPose poses[sim::MAX_PLAYERS] = {};
    queue_frame(frame, curr, alpha, frame_view, poses);
    const sim::Mech& mech = curr.mech;


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
    globals.level_tex = level_tex_;
    globals.level_normal_tex = level_normal_tex_;
    globals.level_rough_tex = level_rough_tex_;
    globals.sky_color[0] = SKY_COLOR[0];
    globals.sky_color[1] = SKY_COLOR[1];
    globals.sky_color[2] = SKY_COLOR[2];
    globals.sky_color[3] = 1.0f;
    globals.cam_pos[0] = cam_x;
    globals.cam_pos[1] = cam_y + eye_height;
    globals.cam_pos[2] = cam_z;
    globals.cam_pos[3] = 0.0f;

    const RenderModel* fill_models[11] = {&models_->gun,       &models_->viewmodel,
                                          &models_->trooper.base, &models_->bot_gun,
                                          &models_->mech,      &models_->grenade,
                                          &models_->blast,     &models_->tracer,
                                          &models_->tracer_vm, &models_->hitmarker,
                                          &models_->overlay};
    for (u32 i = 0; i < 11; ++i) {
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
    model_cull(models_->gun, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    // The viewmodel is always on screen by construction: cull it with planes
    // that accept everything, since its record is in camera space.
    model_cull(models_->gun, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, accept_all,
               frame.slot, PASS_CAMERA);
    model_cull(models_->viewmodel, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    model_cull(models_->trooper.base, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               planes, frame.slot, PASS_CAMERA);
    model_cull(models_->bot_gun, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    model_cull(models_->mech, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    model_cull(models_->grenade, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    model_cull(models_->blast, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot, PASS_CAMERA);
    model_cull(models_->tracer, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    model_cull(models_->tracer_vm, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    model_cull(models_->hitmarker, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    model_cull(models_->overlay, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_CAMERA);
    // Shadow casters are culled permissively: the sun sees the whole map.
    model_cull(models_->trooper.base, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    model_cull(models_->bot_gun, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    model_cull(models_->mech, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               accept_all, frame.slot, PASS_SHADOW);
    for (u32 i = 1; i < sim::MAX_PLAYERS; ++i) {
        if (poses[i].posed) {
            skinned_model_skin(models_->trooper, frame.cmd, skin_pipeline_, skin_layout_,
                               bindless_set_, i, frame.slot);
        }
    }
    memory_barrier(frame.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    if (rt_) {
        // Ray traced sun: rebuild the TLAS from the live instance transforms.
        VkAccelerationStructureInstanceKHR instances[16];
        u32 instance_count = 0;
        instances[instance_count++] = make_rt_instance(Mat4{}, models_->level_blas.address);
        if (models_->mech_blas.handle != VK_NULL_HANDLE && mech.alive != 0 &&
            mech.pilot != 0) {
            const core::Quat mech_rot = core::Quat::from_axis_half(
                core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(mech.yaw * 0.5f),
                std::cos(mech.yaw * 0.5f));
            instances[instance_count++] = make_rt_instance(
                core::Mat4::trs(core::Vec3{mech.x, mech.y, mech.z}, mech_rot),
                models_->mech_blas.address);
        }
        if (models_->trooper_blas.handle != VK_NULL_HANDLE) {
            for (u32 i = 1; i < sim::MAX_PLAYERS; ++i) {
                const sim::Character& other = curr.chars[i];
                if (other.alive == 0) {
                    continue;
                }
                const f32 half = (other.yaw + TROOPER_RIG.yaw_offset) * 0.5f;
                const core::Quat rot = core::Quat::from_axis_half(
                    core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(half), std::cos(half));
                core::Mat4 world = core::Mat4::trs(core::Vec3{other.x, other.y, other.z}, rot);
                world = world * core::Mat4::scale(
                                    core::Vec3{TROOPER_RIG.scale, TROOPER_RIG.scale, TROOPER_RIG.scale});
                instances[instance_count++] =
                    make_rt_instance(world, models_->trooper_blas.address);
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
    sun_in.casters[0] = &models_->trooper.base;
    sun_in.casters[1] = &models_->mech;
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
    color.clearValue.color = {{SKY_COLOR[0], SKY_COLOR[1], SKY_COLOR[2], 1.0f}};

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

    // Sky first: it covers every pixel, so the level and everything after it
    // draw over whatever it leaves behind and the clear colour never shows.
    {
        Vec3 cam_right;
        Vec3 cam_up;
        Vec3 cam_fwd;
        camera_basis(yaw, pitch, cur_roll_, &cam_right, &cam_up, &cam_fwd);
        const f32 tan_half = std::tan(cur_fov_ * 0.5f);
        SkyPush sky{};
        sky.ray_right[0] = cam_right.x * tan_half * aspect;
        sky.ray_right[1] = cam_right.y * tan_half * aspect;
        sky.ray_right[2] = cam_right.z * tan_half * aspect;
        sky.ray_up[0] = cam_up.x * tan_half;
        sky.ray_up[1] = cam_up.y * tan_half;
        sky.ray_up[2] = cam_up.z * tan_half;
        sky.ray_forward[0] = cam_fwd.x;
        sky.ray_forward[1] = cam_fwd.y;
        sky.ray_forward[2] = cam_fwd.z;
        sky.globals = models_->globals.bindless_index;
        sky.gslot = frame.slot;
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_layout_, 0, 1,
                                &bindless_set_, 0, nullptr);
        vkCmdPushConstants(frame.cmd, sky_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(SkyPush), &sky);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    }

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
    model_draw_culled(models_->bot_gun, frame.cmd, mesh_layout_, view_proj, globals_idx,
                      frame.slot);
    model_draw_culled(models_->trooper.base, frame.cmd, mesh_layout_, view_proj, globals_idx,
                      frame.slot);
    model_draw_culled(models_->mech, frame.cmd, mesh_layout_, view_proj, globals_idx,
                      frame.slot);
    model_draw_culled(models_->grenade, frame.cmd, mesh_layout_, view_proj, globals_idx,
                      frame.slot);
    model_draw_culled(models_->blast, frame.cmd, mesh_layout_, view_proj, globals_idx,
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
    // The far plane reaches the tracer's endpoint, which sits at the real
    // impact distance; depth precision near the gun is unaffected.
    const Mat4 vm_proj = perspective(1.05f, aspect, 0.05f, 300.0f);
    model_draw_culled(models_->gun, frame.cmd, mesh_layout_, vm_proj, globals_idx, frame.slot);
    model_draw_culled(models_->viewmodel, frame.cmd, mesh_layout_, vm_proj, globals_idx,
                      frame.slot);
    model_draw_culled(models_->tracer_vm, frame.cmd, mesh_layout_, vm_proj, globals_idx,
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
