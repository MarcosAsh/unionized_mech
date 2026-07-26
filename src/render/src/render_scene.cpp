#define _POSIX_C_SOURCE 200809L

#include "render/render.h"
#include "render_math.h"
#include "render_model.h"

#include "core/array.h"
#include "core/log.h"

#include <volk.h>

#include <cmath>
#include <sys/stat.h>

namespace render {

namespace {

struct Vertex {
    f32 pos[4];
    f32 color[4];
};

struct PushConstants {
    f32 view_proj[16];
    u32 vbuf;
};

struct DuckSpot {
    core::Vec3 pos;
    f32 yaw;
    f32 scale;
};

// Ducks on the plaza. Visual decoration only; they have no collision.
constexpr DuckSpot DUCKS[3] = {
    {{4.0f, 0.0f, 4.0f}, 0.6f, 1.5f},
    {{-6.0f, 2.0f, 6.0f}, 2.4f, 1.0f},
    {{2.0f, 0.0f, -9.0f}, -1.2f, 2.5f},
};

// The Sponza atrium, placed south of the plaza. Collision boxes matching its
// main walls live in sim's level so it is walkable and wallrunnable.
constexpr core::Vec3 SPONZA_POS{0.0f, 0.0f, -90.0f};

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

void add_vertex(core::Array<Vertex>& verts, f32 x, f32 y, f32 z, f32 r, f32 g, f32 b) {
    Vertex v;
    v.pos[0] = x;
    v.pos[1] = y;
    v.pos[2] = z;
    v.pos[3] = 1.0f;
    v.color[0] = r;
    v.color[1] = g;
    v.color[2] = b;
    v.color[3] = 1.0f;
    verts.push(v);
}

void add_quad(core::Array<Vertex>& verts, core::Array<u32>& indices, f32 x, f32 z, f32 cell, f32 r,
              f32 g, f32 b) {
    const u32 base = static_cast<u32>(verts.size());
    add_vertex(verts, x, 0.0f, z, r, g, b);
    add_vertex(verts, x + cell, 0.0f, z, r, g, b);
    add_vertex(verts, x + cell, 0.0f, z + cell, r, g, b);
    add_vertex(verts, x, 0.0f, z + cell, r, g, b);
    const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    for (u32 i = 0; i < 6; ++i) {
        indices.push(quad[i]);
    }
}

void add_box(core::Array<Vertex>& verts, core::Array<u32>& indices, f32 x0, f32 y0, f32 z0, f32 x1,
             f32 y1, f32 z1, f32 r, f32 g, f32 b) {
    const u32 base = static_cast<u32>(verts.size());
    const f32 xs[8] = {x0, x1, x1, x0, x0, x1, x1, x0};
    const f32 ys[8] = {y0, y0, y1, y1, y0, y0, y1, y1};
    const f32 zs[8] = {z0, z0, z0, z0, z1, z1, z1, z1};
    for (u32 i = 0; i < 8; ++i) {
        add_vertex(verts, xs[i], ys[i], zs[i], r, g, b);
    }
    const u32 faces[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 4, 0, 3, 4, 3, 7,
                           1, 5, 6, 1, 6, 2, 3, 2, 6, 3, 6, 7, 4, 5, 1, 4, 1, 0};
    for (u32 i = 0; i < 36; ++i) {
        indices.push(base + faces[i]);
    }
}

void build_scene(core::Array<Vertex>& verts, core::Array<u32>& indices) {
    constexpr i32 N = 100;
    constexpr f32 CELL = 2.0f;
    const f32 half = static_cast<f32>(N) * CELL * 0.5f;
    for (i32 i = 0; i < N; ++i) {
        for (i32 j = 0; j < N; ++j) {
            const f32 x = -half + static_cast<f32>(i) * CELL;
            const f32 z = -half + static_cast<f32>(j) * CELL;
            const bool light = ((i + j) & 1) != 0;
            f32 r = light ? 0.38f : 0.24f;
            f32 g = r;
            f32 b = r + 0.04f;
            // A warm walkway from the plaza south to the Sponza atrium.
            if ((i == 49 || i == 50) && z < -16.0f) {
                r = light ? 0.52f : 0.44f;
                g = light ? 0.42f : 0.34f;
                b = 0.26f;
            }
            add_quad(verts, indices, x, z, CELL, r, g, b);
        }
    }

    const f32 palette[6][3] = {{0.85f, 0.3f, 0.3f}, {0.3f, 0.75f, 0.4f}, {0.35f, 0.5f, 0.9f},
                               {0.9f, 0.75f, 0.3f}, {0.7f, 0.4f, 0.85f}, {0.3f, 0.8f, 0.8f}};
    const core::Span<const sim::Aabb> boxes = sim::visible_boxes();
    for (u64 c = 0; c < boxes.size(); ++c) {
        const sim::Aabb& b = boxes[c];
        const f32* col = palette[c % 6];
        add_box(verts, indices, b.min_x, b.min_y, b.min_z, b.max_x, b.max_y, b.max_z, col[0],
                col[1], col[2]);
    }
}

}  // namespace

/// The imported models, held in the permanent arena so the public header can
/// keep them behind a forward declaration.
struct SceneModels {
    RenderModel duck;
    RenderModel sponza;
    SkinnedModel fox;
    RenderModel blob_shadow;
    u32 white_texture = 0;
};

namespace {

// A dark disc laid on the ground under characters. Until real shadows arrive
// in M5, this is what visually glues a moving thing to the floor.
RenderModel make_blob_shadow(gpu::Renderer& gpu, u32 fallback_texture) {
    constexpr u32 SEGMENTS = 16;
    static asset::MeshVertex verts[SEGMENTS + 1];
    static u32 indices[SEGMENTS * 3];
    verts[0].pos = core::Vec3{0.0f, 0.0f, 0.0f};
    verts[0].normal = core::Vec3{0.0f, 1.0f, 0.0f};
    for (u32 i = 0; i < SEGMENTS; ++i) {
        const f32 a = static_cast<f32>(i) * (6.2831853f / SEGMENTS);
        verts[i + 1].pos = core::Vec3{std::cos(a), 0.0f, std::sin(a)};
        verts[i + 1].normal = core::Vec3{0.0f, 1.0f, 0.0f};
        indices[i * 3] = 0;
        indices[i * 3 + 1] = 1 + (i + 1) % SEGMENTS;
        indices[i * 3 + 2] = 1 + i;
    }
    static asset::Submesh sub;
    sub.index_offset = 0;
    sub.index_count = SEGMENTS * 3;
    sub.texture = asset::NO_TEXTURE;
    sub.color[0] = 0.10f;
    sub.color[1] = 0.10f;
    sub.color[2] = 0.12f;
    sub.color[3] = 1.0f;
    sub.bounds_min[0] = -1.0f;
    sub.bounds_min[1] = -0.1f;
    sub.bounds_min[2] = -1.0f;
    sub.bounds_max[0] = 1.0f;
    sub.bounds_max[1] = 0.1f;
    sub.bounds_max[2] = 1.0f;

    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, SEGMENTS + 1);
    mesh.indices = core::Span<const u32>(indices, SEGMENTS * 3);
    mesh.submeshes = core::Span<const asset::Submesh>(&sub, 1);
    return model_from_data(gpu, mesh, fallback_texture);
}

}  // namespace

Scene Scene::create(gpu::Renderer& gpu, core::Arena& permanent, core::Arena& scratch) {
    core::Array<Vertex> verts = scratch.make_array<Vertex>(65536);
    core::Array<u32> indices = scratch.make_array<u32>(131072);
    build_scene(verts, indices);

    Scene scene;
    scene.device_ = gpu.device_handle();
    scene.bindless_set_ = gpu.bindless_set();
    scene.bindless_layout_ = gpu.bindless_layout();
    scene.color_format_ = gpu.color_format();
    scene.depth_format_ = gpu.depth_format();
    scene.index_count_ = static_cast<u32>(indices.size());

    scene.vertices_ = gpu.create_device_buffer(verts.as_span().data(), verts.size() * sizeof(Vertex),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    scene.indices_ = gpu.create_device_buffer(indices.as_span().data(), indices.size() * sizeof(u32),
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
    scene.models_->blob_shadow = make_blob_shadow(gpu, scene.models_->white_texture);

    scene.build_pipelines();
    scene.vert_mtime_ = file_mtime(SHADER_DIR "/scene.vert.spv") +
                        file_mtime(SHADER_DIR "/mesh.vert.spv");
    scene.frag_mtime_ = file_mtime(SHADER_DIR "/scene.frag.spv") +
                        file_mtime(SHADER_DIR "/mesh.frag.spv");
    return scene;
}

void Scene::maybe_reload() {
    const i64 vert = file_mtime(SHADER_DIR "/scene.vert.spv") +
                     file_mtime(SHADER_DIR "/mesh.vert.spv");
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
    model_begin(models_->blob_shadow);
    for (const DuckSpot& spot : DUCKS) {
        const f32 half = spot.yaw * 0.5f;
        const core::Quat rot = core::Quat::from_axis_half(core::Vec3{0.0f, 1.0f, 0.0f},
                                                          std::sin(half), std::cos(half));
        model_queue(models_->duck, frame.slot, spot.pos, rot, spot.scale);
    }
    model_queue(models_->sponza, frame.slot, SPONZA_POS, core::Quat{}, 1.0f);

    // The fox runs a circle around the plaza, animation clocked off sim time so
    // it stays smooth under interpolation. Clip 2 is Run in the Fox's clip list.
    const f32 anim_time =
        static_cast<f32>(curr.tick.raw) * sim::SIM_DT + alpha * sim::SIM_DT;
    const f32 circle_angle = anim_time * 0.35f;
    const core::Vec3 fox_pos{12.0f * std::cos(circle_angle), 0.0f,
                             12.0f * std::sin(circle_angle)};
    const core::Vec3 fox_dir{-std::sin(circle_angle), 0.0f, std::cos(circle_angle)};
    const f32 fox_yaw = std::atan2(fox_dir.x, fox_dir.z);
    const core::Quat fox_rot = core::Quat::from_axis_half(
        core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(fox_yaw * 0.5f), std::cos(fox_yaw * 0.5f));
    skinned_model_queue(models_->fox, frame.slot, fox_pos, fox_rot, 0.02f);

    // Blob shadows glue the animated characters to the floor until M5 shadows.
    const core::Vec3 lift{0.0f, 0.02f, 0.0f};  // above the floor, below the feet
    model_queue(models_->blob_shadow, frame.slot, fox_pos + lift, core::Quat{}, 1.1f);
    for (const DuckSpot& spot : DUCKS) {
        model_queue(models_->blob_shadow, frame.slot, spot.pos + lift, core::Quat{},
                    0.55f * spot.scale);
    }

    const RenderModel* fill_models[4] = {&models_->duck, &models_->sponza, &models_->fox.base,
                                         &models_->blob_shadow};
    for (u32 i = 0; i < 4; ++i) {
        if (fill_models[i]->loaded) {
            vkCmdFillBuffer(frame.cmd, fill_models[i]->counts.handle,
                            static_cast<u64>(frame.slot) * sizeof(u32), sizeof(u32), 0);
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
    model_cull(models_->duck, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot);
    model_cull(models_->sponza, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot);
    model_cull(models_->fox.base, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_, planes,
               frame.slot);
    model_cull(models_->blob_shadow, frame.cmd, cull_pipeline_, cull_layout_, bindless_set_,
               planes, frame.slot);
    skinned_model_update(models_->fox, frame.cmd, skin_pipeline_, skin_layout_, bindless_set_, 2,
                         anim_time, frame.slot);
    memory_barrier(frame.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

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
    vkCmdPushConstants(frame.cmd, layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(PushConstants), &pc);

    vkCmdDrawIndexedIndirect(frame.cmd, indirect_.handle, 0, 1,
                             sizeof(VkDrawIndexedIndirectCommand));

    // The textured mesh pass draws whatever the cull pass kept.
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1,
                            &bindless_set_, 0, nullptr);
    model_draw_culled(models_->duck, frame.cmd, mesh_layout_, view_proj, frame.slot);
    model_draw_culled(models_->sponza, frame.cmd, mesh_layout_, view_proj, frame.slot);
    model_draw_culled(models_->blob_shadow, frame.cmd, mesh_layout_, view_proj, frame.slot);
    model_draw_culled(models_->fox.base, frame.cmd, mesh_layout_, view_proj, frame.slot);

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
