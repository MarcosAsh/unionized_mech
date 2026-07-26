#define _POSIX_C_SOURCE 200809L

#include "render/render.h"
#include "render_math.h"

#include "asset/asset.h"
#include "core/array.h"
#include "core/log.h"
#include "core/mat.h"
#include "core/quat.h"

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

struct MeshPush {
    f32 mvp[16];
    f32 rot[4];  // model rotation quaternion
    u32 vbuf;
    u32 tex;
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
    constexpr i32 N = 60;
    constexpr f32 CELL = 2.0f;
    const f32 half = static_cast<f32>(N) * CELL * 0.5f;
    for (i32 i = 0; i < N; ++i) {
        for (i32 j = 0; j < N; ++j) {
            const f32 x = -half + static_cast<f32>(i) * CELL;
            const f32 z = -half + static_cast<f32>(j) * CELL;
            const bool light = ((i + j) & 1) != 0;
            const f32 shade = light ? 0.38f : 0.24f;
            add_quad(verts, indices, x, z, CELL, shade, shade, shade + 0.04f);
        }
    }

    const f32 palette[6][3] = {{0.85f, 0.3f, 0.3f}, {0.3f, 0.75f, 0.4f}, {0.35f, 0.5f, 0.9f},
                               {0.9f, 0.75f, 0.3f}, {0.7f, 0.4f, 0.85f}, {0.3f, 0.8f, 0.8f}};
    const core::Span<const sim::Aabb> boxes = sim::level_boxes();
    for (u64 c = 0; c < boxes.size(); ++c) {
        const sim::Aabb& b = boxes[c];
        const f32* col = palette[c % 6];
        add_box(verts, indices, b.min_x, b.min_y, b.min_z, b.max_x, b.max_y, b.max_z, col[0],
                col[1], col[2]);
    }
}

}  // namespace

Scene Scene::create(gpu::Renderer& gpu, core::Arena& scratch) {
    core::Array<Vertex> verts = scratch.make_array<Vertex>(16384);
    core::Array<u32> indices = scratch.make_array<u32>(32768);
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

    // The imported sample model. Missing assets degrade to boxes-only, so the
    // app still runs from a build without the sample download.
    core::Result<asset::MeshData, const char*> duck_mesh =
        asset::mesh_load(scratch, ASSET_DIR "/duck.umesh");
    core::Result<asset::TextureData, const char*> duck_tex =
        asset::texture_load(scratch, ASSET_DIR "/duck.utex");
    if (duck_mesh.is_ok() && duck_tex.is_ok()) {
        const asset::MeshData& mesh = duck_mesh.value();
        const asset::TextureData& tex = duck_tex.value();
        scene.duck_vertices_ = gpu.create_device_buffer(
            mesh.vertices.data(), mesh.vertices.size() * sizeof(asset::MeshVertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        scene.duck_indices_ =
            gpu.create_device_buffer(mesh.indices.data(), mesh.indices.size() * sizeof(u32),
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
        VkDrawIndexedIndirectCommand duck_draw{};
        duck_draw.indexCount = static_cast<u32>(mesh.indices.size());
        duck_draw.instanceCount = 1;
        scene.duck_indirect_ = gpu.create_device_buffer(&duck_draw, sizeof(duck_draw),
                                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
        scene.duck_texture_ = gpu.create_texture(tex.rgba.data(), tex.width, tex.height);
        scene.has_duck_ = true;
        core::log_infof("scene: duck loaded, %llu vertices, %ux%u texture, %u mips",
                        static_cast<unsigned long long>(mesh.vertices.size()), tex.width,
                        tex.height, scene.duck_texture_.mip_count);
    } else {
        core::log_infof("scene: no sample model (%s), boxes only",
                        duck_mesh.is_err() ? duck_mesh.error() : duck_tex.error());
    }

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

    // The textured mesh pass: the imported sample model at a few plaza spots.
    if (has_duck_) {
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1,
                                &bindless_set_, 0, nullptr);
        vkCmdBindIndexBuffer(frame.cmd, duck_indices_.handle, 0, VK_INDEX_TYPE_UINT32);
        for (const DuckSpot& spot : DUCKS) {
            const f32 half = spot.yaw * 0.5f;
            const core::Quat rot = core::Quat::from_axis_half(core::Vec3{0.0f, 1.0f, 0.0f},
                                                              std::sin(half), std::cos(half));
            core::Mat4 model = core::Mat4::trs(spot.pos, rot);
            model = model * core::Mat4::scale(
                                core::Vec3{spot.scale, spot.scale, spot.scale});
            const core::Mat4 mvp = view_proj * model;

            MeshPush mp{};
            for (u32 i = 0; i < 16; ++i) {
                mp.mvp[i] = mvp.m[i];
            }
            mp.rot[0] = rot.x;
            mp.rot[1] = rot.y;
            mp.rot[2] = rot.z;
            mp.rot[3] = rot.w;
            mp.vbuf = duck_vertices_.bindless_index;
            mp.tex = duck_texture_.bindless_index;
            vkCmdPushConstants(frame.cmd, mesh_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(MeshPush), &mp);
            vkCmdDrawIndexedIndirect(frame.cmd, duck_indirect_.handle, 0, 1,
                                     sizeof(VkDrawIndexedIndirectCommand));
        }
    }

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
        duck_vertices_ = other.duck_vertices_;
        duck_indices_ = other.duck_indices_;
        duck_indirect_ = other.duck_indirect_;
        duck_texture_ = other.duck_texture_;
        has_duck_ = other.has_duck_;
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
