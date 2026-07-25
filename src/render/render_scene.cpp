#define _POSIX_C_SOURCE 200809L

#include "render/render.h"
#include "render/render_math.h"

#include "core/array.h"
#include "core/log.h"

#include <volk.h>

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
    constexpr i32 N = 24;
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
    core::Array<Vertex> verts = scratch.make_array<Vertex>(8192);
    core::Array<u32> indices = scratch.make_array<u32>(16384);
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

    build_pipeline(scene.device_, scene.color_format_, scene.depth_format_, scene.bindless_layout_,
                   &scene.pipeline_, &scene.layout_);
    scene.vert_mtime_ = file_mtime(SHADER_DIR "/scene.vert.spv");
    scene.frag_mtime_ = file_mtime(SHADER_DIR "/scene.frag.spv");
    return scene;
}

void Scene::maybe_reload() {
    const i64 vert = file_mtime(SHADER_DIR "/scene.vert.spv");
    const i64 frag = file_mtime(SHADER_DIR "/scene.frag.spv");
    if (vert == vert_mtime_ && frag == frag_mtime_) {
        return;
    }
    vkDeviceWaitIdle(device_);  // reload is an event, not the steady frame path
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    build_pipeline(device_, color_format_, depth_format_, bindless_layout_, &pipeline_, &layout_);
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

    const f32 aspect =
        static_cast<f32>(frame.extent.width) / static_cast<f32>(frame.extent.height);
    const Mat4 proj = perspective(1.2217f, aspect, 0.1f, 500.0f);
    const Mat4 view = view_fps(cam_x, cam_y + 1.7f, cam_z, yaw, pitch);
    const Mat4 view_proj = mat4_mul(proj, view);

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
    vkCmdPushConstants(frame.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants),
                       &pc);

    vkCmdDrawIndexedIndirect(frame.cmd, indirect_.handle, 0, 1,
                             sizeof(VkDrawIndexedIndirectCommand));

    vkCmdEndRendering(frame.cmd);
}

Scene::~Scene() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);  // shutdown: the pipeline may still be in flight
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
}

Scene::Scene(Scene&& other) noexcept { *this = static_cast<Scene&&>(other); }

Scene& Scene::operator=(Scene&& other) noexcept {
    if (this != &other) {
        device_ = other.device_;
        pipeline_ = other.pipeline_;
        layout_ = other.layout_;
        bindless_set_ = other.bindless_set_;
        vertices_ = other.vertices_;
        indices_ = other.indices_;
        indirect_ = other.indirect_;
        index_count_ = other.index_count_;
        other.device_ = VK_NULL_HANDLE;
        other.pipeline_ = VK_NULL_HANDLE;
        other.layout_ = VK_NULL_HANDLE;
    }
    return *this;
}

}  // namespace render
