#include "render_model.h"

#include "core/log.h"

#include <volk.h>

#include <cstdio>

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

    VkDrawIndexedIndirectCommand draws[MAX_MODEL_SUBMESHES]{};
    for (u64 i = 0; i < mesh.submeshes.size(); ++i) {
        model.submeshes[i] = mesh.submeshes[i];
        draws[i].indexCount = mesh.submeshes[i].index_count;
        draws[i].instanceCount = 1;
        draws[i].firstIndex = mesh.submeshes[i].index_offset;
    }
    model.submesh_count = static_cast<u32>(mesh.submeshes.size());
    model.indirect = gpu.create_device_buffer(
        draws, model.submesh_count * sizeof(VkDrawIndexedIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);

    // Textures are optional one by one; a missing file falls back to white.
    u64 total_texels = 0;
    for (u32 t = 0; t < MAX_MODEL_TEXTURES; ++t) {
        std::snprintf(path, sizeof(path), "%s.%u.utex", base, t);
        const u64 marker = scratch.marker();
        core::Result<asset::TextureData, const char*> tex_result =
            asset::texture_load(scratch, path);
        if (tex_result.is_err()) {
            break;
        }
        const asset::TextureData& tex = tex_result.value();
        const gpu::Texture uploaded = gpu.create_texture(tex.rgba.data(), tex.width, tex.height);
        model.texture_slots[t] = uploaded.bindless_index;
        ++model.texture_count;
        total_texels += static_cast<u64>(tex.width) * tex.height;
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
    core::log_infof("render: %s loaded, %llu vertices, %u submeshes, %u textures (%llu texels)",
                    base, static_cast<unsigned long long>(mesh.vertices.size()),
                    model.submesh_count, model.texture_count,
                    static_cast<unsigned long long>(total_texels));
    return model;
}

void model_draw(const RenderModel& model, VkCommandBuffer cmd, VkPipelineLayout layout,
                const core::Mat4& view_proj, core::Vec3 pos, core::Quat rot, f32 scale) {
    if (!model.loaded) {
        return;
    }
    core::Mat4 world = core::Mat4::trs(pos, rot);
    world = world * core::Mat4::scale(core::Vec3{scale, scale, scale});
    const core::Mat4 mvp = view_proj * world;

    vkCmdBindIndexBuffer(cmd, model.indices.handle, 0, VK_INDEX_TYPE_UINT32);

    MeshPush push{};
    for (u32 i = 0; i < 16; ++i) {
        push.mvp[i] = mvp.m[i];
    }
    push.rot[0] = rot.x;
    push.rot[1] = rot.y;
    push.rot[2] = rot.z;
    push.rot[3] = rot.w;
    push.vbuf = model.vertices.bindless_index;

    for (u32 i = 0; i < model.submesh_count; ++i) {
        const asset::Submesh& sub = model.submeshes[i];
        for (u32 c = 0; c < 4; ++c) {
            push.color[c] = sub.color[c];
        }
        push.tex = sub.texture;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(MeshPush), &push);
        vkCmdDrawIndexedIndirect(cmd, model.indirect.handle,
                                 i * sizeof(VkDrawIndexedIndirectCommand), 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
}

}  // namespace render
