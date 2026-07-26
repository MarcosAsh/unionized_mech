#include "render_model.h"

#include "core/log.h"

#include <volk.h>

#include <cstdio>
#include <cstring>

namespace render {

RenderModel model_load(gpu::Renderer& gpu, core::Arena& scratch, const char* base,
                       u32 fallback_texture) {
    RenderModel model;

    char path[1024];
    std::snprintf(path, sizeof(path), "%s.umesh", base);
    core::Result<asset::MeshData, const char*> mesh_result = asset::mesh_load(scratch, path);
    if (mesh_result.is_err()) {
        core::log_infof("render: no model at %s (%s)", path, mesh_result.error());
        return model;
    }
    const asset::MeshData& mesh = mesh_result.value();
    if (mesh.submeshes.size() > MAX_MODEL_SUBMESHES) {
        core::log_errorf("render: %s has too many submeshes", path);
        return model;
    }

    model.vertices = gpu.create_device_buffer(mesh.vertices.data(),
                                              mesh.vertices.size() * sizeof(asset::MeshVertex),
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    model.indices = gpu.create_device_buffer(mesh.indices.data(), mesh.indices.size() * sizeof(u32),
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    for (u64 i = 0; i < mesh.submeshes.size(); ++i) {
        model.submeshes[i] = mesh.submeshes[i];
    }
    model.submesh_count = static_cast<u32>(mesh.submeshes.size());

    // The GPU-driven draw plumbing: CPU-written records, GPU-written commands
    // and survivor counts, all sliced per frame in flight.
    const u32 frames = gpu::Renderer::frames_in_flight();
    void* mapped = nullptr;
    model.records = gpu.create_mapped_buffer(
        static_cast<u64>(frames) * MAX_MODEL_DRAWS * sizeof(DrawRecord), &mapped);
    model.records_mapped = static_cast<DrawRecord*>(mapped);
    model.commands = gpu.create_gpu_buffer(
        static_cast<u64>(frames) * MAX_MODEL_DRAWS * sizeof(VkDrawIndexedIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    model.counts = gpu.create_gpu_buffer(static_cast<u64>(frames) * sizeof(u32),
                                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // Textures are optional one by one; a missing file falls back to white.
    for (u32 t = 0; t < MAX_MODEL_TEXTURES; ++t) {
        std::snprintf(path, sizeof(path), "%s.%u.utex", base, t);
        const u64 marker = scratch.marker();
        core::Result<asset::TextureData, const char*> tex_result =
            asset::texture_load(scratch, path);
        if (tex_result.is_err()) {
            break;
        }
        const asset::TextureData& tex = tex_result.value();
        model.texture_slots[t] = gpu.create_texture(tex.rgba.data(), tex.width, tex.height)
                                     .bindless_index;
        ++model.texture_count;
        scratch.rewind(marker);  // pixel data is on the GPU now
    }

    // Point submeshes at bindless slots, falling back for missing textures.
    for (u32 i = 0; i < model.submesh_count; ++i) {
        const u32 t = model.submeshes[i].texture;
        model.submeshes[i].texture =
            (t != asset::NO_TEXTURE && t < model.texture_count) ? model.texture_slots[t]
                                                                : fallback_texture;
    }

    model.loaded = true;
    core::log_infof("render: %s loaded, %llu vertices, %u submeshes, %u textures", base,
                    static_cast<unsigned long long>(mesh.vertices.size()), model.submesh_count,
                    model.texture_count);
    return model;
}

void model_begin(RenderModel& model) { model.queued = 0; }

void model_queue(RenderModel& model, u32 slot, core::Vec3 pos, core::Quat rot, f32 scale) {
    if (!model.loaded) {
        return;
    }
    core::Mat4 world = core::Mat4::trs(pos, rot);
    world = world * core::Mat4::scale(core::Vec3{scale, scale, scale});

    for (u32 i = 0; i < model.submesh_count; ++i) {
        if (model.queued >= MAX_MODEL_DRAWS) {
            return;  // queue full; the rest of this instance is dropped
        }
        const asset::Submesh& sub = model.submeshes[i];
        DrawRecord& rec = model.records_mapped[slot * MAX_MODEL_DRAWS + model.queued];
        std::memcpy(rec.model, world.m, sizeof(rec.model));
        rec.rot[0] = rot.x;
        rec.rot[1] = rot.y;
        rec.rot[2] = rot.z;
        rec.rot[3] = rot.w;
        for (u32 c = 0; c < 4; ++c) {
            rec.color[c] = sub.color[c];
        }
        for (u32 c = 0; c < 3; ++c) {
            rec.bounds_min[c] = sub.bounds_min[c];
            rec.bounds_max[c] = sub.bounds_max[c];
        }
        rec.bounds_min[3] = 1.0f;
        rec.bounds_max[3] = 1.0f;
        rec.vbuf = model.vertices.bindless_index;
        rec.tex = sub.texture;
        rec.index_count = sub.index_count;
        rec.first_index = sub.index_offset;
        ++model.queued;
    }
}

void model_cull(const RenderModel& model, VkCommandBuffer cmd, VkPipeline pipeline,
                VkPipelineLayout layout, VkDescriptorSet bindless, const f32 planes[6][4],
                u32 slot) {
    if (!model.loaded || model.queued == 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &bindless, 0,
                            nullptr);

    CullPush push{};
    std::memcpy(push.planes, planes, sizeof(push.planes));
    push.draw_count = model.queued;
    push.records_buf = model.records.bindless_index;
    push.records_base = slot * MAX_MODEL_DRAWS;
    push.commands_buf = model.commands.bindless_index;
    push.commands_base = slot * MAX_MODEL_DRAWS;
    push.count_buf = model.counts.bindless_index;
    push.count_slot = slot;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (model.queued + 63) / 64, 1, 1);
}

void model_draw_culled(const RenderModel& model, VkCommandBuffer cmd, VkPipelineLayout layout,
                       const core::Mat4& view_proj, u32 slot) {
    if (!model.loaded || model.queued == 0) {
        return;
    }
    vkCmdBindIndexBuffer(cmd, model.indices.handle, 0, VK_INDEX_TYPE_UINT32);

    MeshPush push{};
    std::memcpy(push.view_proj, view_proj.m, sizeof(push.view_proj));
    push.records_buf = model.records.bindless_index;
    push.records_base = slot * MAX_MODEL_DRAWS;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    vkCmdDrawIndexedIndirectCount(cmd, model.commands.handle,
                                  static_cast<u64>(slot) * MAX_MODEL_DRAWS *
                                      sizeof(VkDrawIndexedIndirectCommand),
                                  model.counts.handle, static_cast<u64>(slot) * sizeof(u32),
                                  MAX_MODEL_DRAWS, sizeof(VkDrawIndexedIndirectCommand));
}

}  // namespace render
