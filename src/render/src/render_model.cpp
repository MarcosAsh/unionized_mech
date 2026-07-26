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
    model.total_vertices = static_cast<u32>(mesh.vertices.size());
    model.total_indices = static_cast<u32>(mesh.indices.size());

    // The GPU-driven draw plumbing: CPU-written records, GPU-written commands
    // and survivor counts, all sliced per frame in flight.
    const u32 frames = gpu::Renderer::frames_in_flight();
    void* mapped = nullptr;
    model.records = gpu.create_mapped_buffer(
        static_cast<u64>(frames) * MAX_MODEL_DRAWS * sizeof(DrawRecord), &mapped);
    model.records_mapped = static_cast<DrawRecord*>(mapped);
    model.commands = gpu.create_gpu_buffer(static_cast<u64>(frames) * PASS_COUNT *
                                               MAX_MODEL_DRAWS *
                                               sizeof(VkDrawIndexedIndirectCommand),
                                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    model.counts = gpu.create_gpu_buffer(static_cast<u64>(frames) * PASS_COUNT * sizeof(u32),
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

RenderModel model_from_data(gpu::Renderer& gpu, const asset::MeshData& mesh,
                            u32 fallback_texture) {
    RenderModel model;
    if (mesh.submeshes.size() > MAX_MODEL_SUBMESHES) {
        return model;
    }
    model.vertices = gpu.create_device_buffer(mesh.vertices.data(),
                                              mesh.vertices.size() * sizeof(asset::MeshVertex),
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    model.indices = gpu.create_device_buffer(mesh.indices.data(), mesh.indices.size() * sizeof(u32),
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    for (u64 i = 0; i < mesh.submeshes.size(); ++i) {
        model.submeshes[i] = mesh.submeshes[i];
        if (model.submeshes[i].texture == asset::NO_TEXTURE) {
            model.submeshes[i].texture = fallback_texture;
        }
    }
    model.submesh_count = static_cast<u32>(mesh.submeshes.size());
    model.total_vertices = static_cast<u32>(mesh.vertices.size());
    model.total_indices = static_cast<u32>(mesh.indices.size());

    const u32 frames = gpu::Renderer::frames_in_flight();
    void* mapped = nullptr;
    model.records = gpu.create_mapped_buffer(
        static_cast<u64>(frames) * MAX_MODEL_DRAWS * sizeof(DrawRecord), &mapped);
    model.records_mapped = static_cast<DrawRecord*>(mapped);
    model.commands = gpu.create_gpu_buffer(static_cast<u64>(frames) * PASS_COUNT *
                                               MAX_MODEL_DRAWS *
                                               sizeof(VkDrawIndexedIndirectCommand),
                                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    model.counts = gpu.create_gpu_buffer(static_cast<u64>(frames) * PASS_COUNT * sizeof(u32),
                                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    model.loaded = true;
    return model;
}

void model_begin(RenderModel& model) { model.queued = 0; }

namespace {

// Shared record writer. `bounds_pad` widens the cull box, which skinned models
// need because animation moves vertices beyond the bind-pose bounds.
void queue_records(RenderModel& model, u32 slot, core::Vec3 pos, core::Quat rot, f32 scale,
                   u32 vbuf, u32 vertex_base, f32 bounds_pad) {
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
            const f32 pad = bounds_pad * (sub.bounds_max[c] - sub.bounds_min[c]);
            rec.bounds_min[c] = sub.bounds_min[c] - pad;
            rec.bounds_max[c] = sub.bounds_max[c] + pad;
        }
        rec.bounds_min[3] = 1.0f;
        rec.bounds_max[3] = 1.0f;
        rec.vbuf = vbuf;
        rec.tex = sub.texture;
        rec.index_count = sub.index_count;
        rec.first_index = sub.index_offset;
        rec.vertex_base = vertex_base;
        rec.pad[0] = 0;
        rec.pad[1] = 0;
        rec.pad[2] = 0;
        ++model.queued;
    }
}

}  // namespace

void model_queue(RenderModel& model, u32 slot, core::Vec3 pos, core::Quat rot, f32 scale) {
    queue_records(model, slot, pos, rot, scale, model.vertices.bindless_index, 0, 0.0f);
}

SkinnedModel skinned_model_load(gpu::Renderer& gpu, core::Arena& permanent, core::Arena& scratch,
                                const char* base, u32 fallback_texture) {
    SkinnedModel model;
    model.base = model_load(gpu, scratch, base, fallback_texture);
    if (!model.base.loaded) {
        return model;
    }

    char path[1024];
    std::snprintf(path, sizeof(path), "%s.uskel", base);
    core::Result<anim::Skeleton, const char*> skel = anim::skeleton_load(scratch, path);
    std::snprintf(path, sizeof(path), "%s.uskin", base);
    core::Result<core::Span<const asset::SkinVertex>, const char*> skin =
        asset::skin_load(scratch, path);
    if (skel.is_err() || skin.is_err()) {
        core::log_infof("render: %s has no rig, loaded static", base);
        return model;
    }
    model.skeleton = skel.value();
    const core::Span<const asset::SkinVertex> skin_data = skin.value();
    model.vertex_count = static_cast<u32>(skin_data.size());

    // Clip storage lives in the permanent arena, sampled every frame.
    for (u32 c = 0; c < 8; ++c) {
        std::snprintf(path, sizeof(path), "%s.%u.uclip", base, c);
        core::Result<anim::Clip, const char*> clip = anim::clip_load(permanent, path);
        if (clip.is_err()) {
            break;
        }
        model.clips[model.clip_count++] = clip.value();
    }

    const u32 frames = gpu::Renderer::frames_in_flight();
    model.skin_verts = gpu.create_device_buffer(skin_data.data(),
                                                skin_data.size() * sizeof(asset::SkinVertex),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    void* mapped = nullptr;
    model.matrices = gpu.create_mapped_buffer(
        static_cast<u64>(frames) * anim::MAX_JOINTS * sizeof(core::Mat4), &mapped);
    model.matrices_mapped = static_cast<core::Mat4*>(mapped);
    model.skinned_verts = gpu.create_gpu_buffer(
        static_cast<u64>(frames) * model.vertex_count * sizeof(asset::MeshVertex), 0);

    core::log_infof("render: %s rigged, %u joints, %u clips", base, model.skeleton.joint_count,
                    model.clip_count);
    return model;
}

void skinned_model_update(SkinnedModel& model, VkCommandBuffer cmd, VkPipeline pipeline,
                          VkPipelineLayout layout, VkDescriptorSet bindless, u32 clip_a,
                          u32 clip_b, f32 blend, f32 time, u32 slot) {
    if (!model.base.loaded || model.clip_count == 0 || model.matrices_mapped == nullptr) {
        return;
    }
    if (clip_a >= model.clip_count) {
        clip_a = model.clip_count - 1;
    }
    if (clip_b >= model.clip_count) {
        clip_b = model.clip_count - 1;
    }

    anim::Pose pose;
    if (clip_a == clip_b || blend <= 0.0f) {
        anim::sample_clip(model.skeleton, model.clips[clip_a], time, &pose);
    } else if (blend >= 1.0f) {
        anim::sample_clip(model.skeleton, model.clips[clip_b], time, &pose);
    } else {
        anim::Pose pose_a;
        anim::Pose pose_b;
        anim::sample_clip(model.skeleton, model.clips[clip_a], time, &pose_a);
        anim::sample_clip(model.skeleton, model.clips[clip_b], time, &pose_b);
        anim::blend_poses(pose_a, pose_b, blend, model.skeleton.joint_count, &pose);
    }
    core::Mat4 world[anim::MAX_JOINTS];
    anim::pose_matrices(model.skeleton, pose, world);
    // Skinning matrices land straight in this frame's mapped slice.
    anim::skinning_matrices(model.skeleton, world,
                            model.matrices_mapped + slot * anim::MAX_JOINTS);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &bindless, 0,
                            nullptr);
    SkinPush push{};
    push.vertex_count = model.vertex_count;
    push.in_vbuf = model.base.vertices.bindless_index;
    push.skin_buf = model.skin_verts.bindless_index;
    push.matrices_buf = model.matrices.bindless_index;
    push.matrices_base = slot * anim::MAX_JOINTS;
    push.out_vbuf = model.skinned_verts.bindless_index;
    push.out_base = slot * model.vertex_count;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (model.vertex_count + 63) / 64, 1, 1);
}

void skinned_model_queue(SkinnedModel& model, u32 slot, core::Vec3 pos, core::Quat rot,
                         f32 scale) {
    queue_records(model.base, slot, pos, rot, scale, model.skinned_verts.bindless_index,
                  slot * model.vertex_count, 0.5f);
}

void model_cull(const RenderModel& model, VkCommandBuffer cmd, VkPipeline pipeline,
                VkPipelineLayout layout, VkDescriptorSet bindless, const f32 planes[6][4],
                u32 slot, u32 pass) {
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
    push.commands_base = (slot * PASS_COUNT + pass) * MAX_MODEL_DRAWS;
    push.count_buf = model.counts.bindless_index;
    push.count_slot = slot * PASS_COUNT + pass;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (model.queued + 63) / 64, 1, 1);
}

void model_draw_culled(const RenderModel& model, VkCommandBuffer cmd, VkPipelineLayout layout,
                       const core::Mat4& view_proj, u32 globals, u32 slot) {
    if (!model.loaded || model.queued == 0) {
        return;
    }
    vkCmdBindIndexBuffer(cmd, model.indices.handle, 0, VK_INDEX_TYPE_UINT32);

    MeshPush push{};
    std::memcpy(push.view_proj, view_proj.m, sizeof(push.view_proj));
    push.records_buf = model.records.bindless_index;
    push.records_base = slot * MAX_MODEL_DRAWS;
    push.globals = globals;
    push.gslot = slot;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    const u64 pass_index = static_cast<u64>(slot) * PASS_COUNT + PASS_CAMERA;
    vkCmdDrawIndexedIndirectCount(cmd, model.commands.handle,
                                  pass_index * MAX_MODEL_DRAWS *
                                      sizeof(VkDrawIndexedIndirectCommand),
                                  model.counts.handle, pass_index * sizeof(u32), MAX_MODEL_DRAWS,
                                  sizeof(VkDrawIndexedIndirectCommand));
}

void model_draw_shadow(const RenderModel& model, VkCommandBuffer cmd, VkPipelineLayout layout,
                       const core::Mat4& sun_view_proj, u32 slot) {
    if (!model.loaded || model.queued == 0) {
        return;
    }
    vkCmdBindIndexBuffer(cmd, model.indices.handle, 0, VK_INDEX_TYPE_UINT32);

    ShadowMeshPush push{};
    std::memcpy(push.sun_view_proj, sun_view_proj.m, sizeof(push.sun_view_proj));
    push.records_buf = model.records.bindless_index;
    push.records_base = slot * MAX_MODEL_DRAWS;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    const u64 pass_index = static_cast<u64>(slot) * PASS_COUNT + PASS_SHADOW;
    vkCmdDrawIndexedIndirectCount(cmd, model.commands.handle,
                                  pass_index * MAX_MODEL_DRAWS *
                                      sizeof(VkDrawIndexedIndirectCommand),
                                  model.counts.handle, pass_index * sizeof(u32), MAX_MODEL_DRAWS,
                                  sizeof(VkDrawIndexedIndirectCommand));
}

}  // namespace render
