#include "asset/asset.h"
#include "asset_bounds.h"
#include "asset_skin.h"

#include "core/file.h"

#include <cgltf.h>
#include <stb_image.h>

#include <cstring>

namespace asset {

using ImportResult = core::Result<Model, const char*>;

namespace {

constexpr u32 MAX_MATERIALS = 256;
constexpr u32 MAX_IMAGES = 256;

// One bucket per distinct material (nullptr is the no-material bucket). Each
// bucket becomes a submesh: a contiguous index range in the shared pools.
struct MaterialBucket {
    const cgltf_material* material = nullptr;
    u64 vertex_count = 0;
    u64 index_count = 0;
    u64 index_offset = 0;  // start of this bucket's region in the index pool
    u64 index_cursor = 0;  // write position within the region
    u32 texture = NO_TEXTURE;
};

struct Buckets {
    MaterialBucket items[MAX_MATERIALS];
    u32 count = 0;

    MaterialBucket* find_or_add(const cgltf_material* material) {
        for (u32 i = 0; i < count; ++i) {
            if (items[i].material == material) {
                return &items[i];
            }
        }
        if (count >= MAX_MATERIALS) {
            return nullptr;
        }
        items[count].material = material;
        return &items[count++];
    }
};

// Triangle primitives, indexed or not. Non-indexed ones get identity indices.
bool is_triangles(const cgltf_primitive& prim) {
    return prim.type == cgltf_primitive_type_triangles;
}

u64 prim_index_count(const cgltf_primitive& prim, const cgltf_accessor* pos) {
    return prim.indices != nullptr ? prim.indices->count : pos->count;
}

const cgltf_accessor* find_attr(const cgltf_primitive& prim, cgltf_attribute_type type) {
    for (u64 a = 0; a < prim.attributes_count; ++a) {
        if (prim.attributes[a].type == type && prim.attributes[a].index == 0) {
            return prim.attributes[a].data;
        }
    }
    return nullptr;
}

void count_node(const cgltf_node* node, Buckets* buckets, bool* overflow) {
    if (node->mesh != nullptr) {
        for (u64 p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node->mesh->primitives[p];
            if (!is_triangles(prim)) {
                continue;
            }
            const cgltf_accessor* pos = find_attr(prim, cgltf_attribute_type_position);
            if (pos == nullptr) {
                continue;
            }
            MaterialBucket* bucket = buckets->find_or_add(prim.material);
            if (bucket == nullptr) {
                *overflow = true;
                return;
            }
            bucket->vertex_count += pos->count;
            bucket->index_count += prim_index_count(prim, pos);
        }
    }
    for (u64 c = 0; c < node->children_count; ++c) {
        count_node(node->children[c], buckets, overflow);
    }
}

void transform_point(const f32 m[16], core::Vec3 p, core::Vec3* out) {
    out->x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    out->y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    out->z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
}

void transform_dir(const f32 m[16], core::Vec3 d, core::Vec3* out) {
    out->x = m[0] * d.x + m[4] * d.y + m[8] * d.z;
    out->y = m[1] * d.x + m[5] * d.y + m[9] * d.z;
    out->z = m[2] * d.x + m[6] * d.y + m[10] * d.z;
}

struct Writer {
    core::Span<MeshVertex> vertices;
    core::Span<u32> indices;
    core::Span<SkinVertex> skin;      // empty when the model is unskinned
    const u32* joint_remap = nullptr;  // glTF joint index to engine joint index
    u64 vert_cursor = 0;
};

void read_skin_vertex(const cgltf_primitive& prim, u64 i, const u32* remap, SkinVertex* out) {
    const cgltf_accessor* joints = find_attr(prim, cgltf_attribute_type_joints);
    const cgltf_accessor* weights = find_attr(prim, cgltf_attribute_type_weights);
    if (joints == nullptr || weights == nullptr) {
        *out = SkinVertex{};
        out->weights[0] = 1.0f;  // fully bound to joint 0 so it still deforms
        return;
    }
    cgltf_uint j[4] = {0, 0, 0, 0};
    f32 w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cgltf_accessor_read_uint(joints, i, j, 4);
    cgltf_accessor_read_float(weights, i, w, 4);
    for (u32 k = 0; k < 4; ++k) {
        out->joints[k] = static_cast<u16>(remap[j[k] < anim::MAX_JOINTS ? j[k] : 0]);
        out->weights[k] = w[k];
    }
}

void append_node(const cgltf_node* node, Buckets* buckets, Writer* w) {
    if (node->mesh != nullptr) {
        f32 world[16];
        cgltf_node_transform_world(node, world);
        // Per the glTF spec, a skinned mesh ignores its node transform: joints
        // pose it instead. Use identity so bind-pose data stays in skin space.
        if (node->skin != nullptr && !w->skin.empty()) {
            const f32 identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(world, identity, sizeof(world));
        }

        for (u64 p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node->mesh->primitives[p];
            if (!is_triangles(prim)) {
                continue;
            }
            const cgltf_accessor* pos = find_attr(prim, cgltf_attribute_type_position);
            if (pos == nullptr) {
                continue;
            }
            const cgltf_accessor* normal = find_attr(prim, cgltf_attribute_type_normal);
            const cgltf_accessor* uv = find_attr(prim, cgltf_attribute_type_texcoord);
            MaterialBucket* bucket = buckets->find_or_add(prim.material);

            const u64 base = w->vert_cursor;
            for (u64 i = 0; i < pos->count; ++i) {
                MeshVertex vertex{};
                f32 tmp[3] = {0.0f, 0.0f, 0.0f};
                cgltf_accessor_read_float(pos, i, tmp, 3);
                transform_point(world, core::Vec3{tmp[0], tmp[1], tmp[2]}, &vertex.pos);
                if (normal != nullptr) {
                    cgltf_accessor_read_float(normal, i, tmp, 3);
                    core::Vec3 n;
                    // Upper 3x3 only. Correct for the rigid and uniformly scaled
                    // transforms game assets use.
                    transform_dir(world, core::Vec3{tmp[0], tmp[1], tmp[2]}, &n);
                    vertex.normal = n.normalized();
                } else {
                    vertex.normal = core::Vec3{0.0f, 1.0f, 0.0f};
                }
                if (uv != nullptr) {
                    f32 st[2] = {0.0f, 0.0f};
                    cgltf_accessor_read_float(uv, i, st, 2);
                    vertex.u = st[0];
                    vertex.v = st[1];
                }
                if (!w->skin.empty()) {
                    read_skin_vertex(prim, i, w->joint_remap, &w->skin[w->vert_cursor]);
                }
                w->vertices[w->vert_cursor] = vertex;
                ++w->vert_cursor;
            }

            const u64 prim_indices = prim_index_count(prim, pos);
            for (u64 i = 0; i < prim_indices; ++i) {
                const u64 slot = bucket->index_offset + bucket->index_cursor;
                const u64 index = prim.indices != nullptr
                                      ? cgltf_accessor_read_index(prim.indices, i)
                                      : i;
                w->indices[slot] = static_cast<u32>(base + index);
                ++bucket->index_cursor;
            }
        }
    }
    for (u64 c = 0; c < node->children_count; ++c) {
        append_node(node->children[c], buckets, w);
    }
}

bool load_image(core::Arena& arena, const cgltf_image* image, const char* gltf_path,
                TextureData* out) {
    const u8* encoded = nullptr;
    u64 encoded_size = 0;

    if (image->buffer_view != nullptr && image->buffer_view->buffer != nullptr &&
        image->buffer_view->buffer->data != nullptr) {
        encoded = static_cast<const u8*>(image->buffer_view->buffer->data) +
                  image->buffer_view->offset;
        encoded_size = image->buffer_view->size;
    } else if (image->uri != nullptr && std::strncmp(image->uri, "data:", 5) != 0) {
        // Resolve the URI relative to the glTF file's directory.
        char resolved[1024];
        const char* slash = std::strrchr(gltf_path, '/');
        if (slash != nullptr) {
            const u64 dir_len = static_cast<u64>(slash - gltf_path) + 1;
            if (dir_len + std::strlen(image->uri) + 1 > sizeof(resolved)) {
                return false;
            }
            std::memcpy(resolved, gltf_path, dir_len);
            std::strcpy(resolved + dir_len, image->uri);
        } else {
            if (std::strlen(image->uri) + 1 > sizeof(resolved)) {
                return false;
            }
            std::strcpy(resolved, image->uri);
        }
        core::Result<core::Span<u8>, const char*> file = core::read_entire_file(arena, resolved);
        if (file.is_err()) {
            return false;
        }
        encoded = file.value().data();
        encoded_size = file.value().size();
    } else {
        return false;
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    u8* pixels =
        stbi_load_from_memory(encoded, static_cast<int>(encoded_size), &w, &h, &comp, 4);
    if (pixels == nullptr) {
        return false;
    }
    const u64 byte_count = static_cast<u64>(w) * static_cast<u64>(h) * 4u;
    const core::Span<u8> copy = arena.alloc_n<u8>(byte_count);
    std::memcpy(copy.data(), pixels, byte_count);
    stbi_image_free(pixels);

    out->width = static_cast<u32>(w);
    out->height = static_cast<u32>(h);
    out->rgba = core::Span<const u8>(copy.data(), byte_count);
    return true;
}

}  // namespace

core::Result<Model, const char*> import_gltf(core::Arena& arena, const char* path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
        return ImportResult::err("could not parse glTF");
    }
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        cgltf_free(data);
        return ImportResult::err("could not load glTF buffers");
    }
    const cgltf_scene* scene = data->scene != nullptr ? data->scene
                               : data->scenes_count > 0 ? &data->scenes[0]
                                                        : nullptr;
    if (scene == nullptr) {
        cgltf_free(data);
        return ImportResult::err("glTF has no scene");
    }

    Buckets buckets;
    bool overflow = false;
    for (u64 n = 0; n < scene->nodes_count; ++n) {
        count_node(scene->nodes[n], &buckets, &overflow);
    }
    if (overflow) {
        cgltf_free(data);
        return ImportResult::err("glTF has too many materials");
    }

    u64 total_verts = 0;
    u64 total_indices = 0;
    for (u32 i = 0; i < buckets.count; ++i) {
        buckets.items[i].index_offset = total_indices;
        total_verts += buckets.items[i].vertex_count;
        total_indices += buckets.items[i].index_count;
    }
    if (total_verts == 0 || total_indices == 0) {
        cgltf_free(data);
        return ImportResult::err("glTF has no triangle geometry");
    }

    SkinImport skin_import;
    (void)import_skeleton(data, &skin_import);

    Writer writer;
    writer.vertices = arena.alloc_n<MeshVertex>(total_verts);
    writer.indices = arena.alloc_n<u32>(total_indices);
    if (skin_import.has) {
        writer.skin = arena.alloc_n<SkinVertex>(total_verts);
        writer.joint_remap = skin_import.gltf_to_engine;
    }
    for (u64 n = 0; n < scene->nodes_count; ++n) {
        append_node(scene->nodes[n], &buckets, &writer);
    }

    // Load each distinct image once and hand out its texture index.
    const cgltf_image* loaded_images[MAX_IMAGES];
    core::Span<TextureData> textures = arena.alloc_n<TextureData>(MAX_IMAGES);
    u32 texture_count = 0;
    for (u32 i = 0; i < buckets.count; ++i) {
        const cgltf_material* mat = buckets.items[i].material;
        if (mat == nullptr || !mat->has_pbr_metallic_roughness) {
            continue;
        }
        const cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
        if (tex == nullptr || tex->image == nullptr) {
            continue;
        }
        u32 found = NO_TEXTURE;
        for (u32 t = 0; t < texture_count; ++t) {
            if (loaded_images[t] == tex->image) {
                found = t;
                break;
            }
        }
        if (found == NO_TEXTURE && texture_count < MAX_IMAGES) {
            TextureData decoded;
            if (load_image(arena, tex->image, path, &decoded)) {
                loaded_images[texture_count] = tex->image;
                textures[texture_count] = decoded;
                found = texture_count;
                ++texture_count;
            }
        }
        buckets.items[i].texture = found;
    }

    const core::Span<Submesh> submeshes = arena.alloc_n<Submesh>(buckets.count);
    for (u32 i = 0; i < buckets.count; ++i) {
        Submesh& sub = submeshes[i];
        sub.index_offset = static_cast<u32>(buckets.items[i].index_offset);
        sub.index_count = static_cast<u32>(buckets.items[i].index_count);
        sub.texture = buckets.items[i].texture;
        const cgltf_material* mat = buckets.items[i].material;
        if (mat != nullptr && mat->has_pbr_metallic_roughness) {
            for (u32 c = 0; c < 4; ++c) {
                sub.color[c] = mat->pbr_metallic_roughness.base_color_factor[c];
            }
            sub.metallic = mat->pbr_metallic_roughness.metallic_factor;
            sub.roughness = mat->pbr_metallic_roughness.roughness_factor;
        }

        // Model-space bounds over the submesh's referenced vertices, for culling.
        core::Vec3 lo{0.0f, 0.0f, 0.0f};
        core::Vec3 hi{0.0f, 0.0f, 0.0f};
        for (u64 k = 0; k < sub.index_count; ++k) {
            const core::Vec3 p = writer.vertices[writer.indices[sub.index_offset + k]].pos;
            if (k == 0) {
                lo = p;
                hi = p;
                continue;
            }
            lo.x = p.x < lo.x ? p.x : lo.x;
            lo.y = p.y < lo.y ? p.y : lo.y;
            lo.z = p.z < lo.z ? p.z : lo.z;
            hi.x = p.x > hi.x ? p.x : hi.x;
            hi.y = p.y > hi.y ? p.y : hi.y;
            hi.z = p.z > hi.z ? p.z : hi.z;
        }
        sub.bounds_min[0] = lo.x;
        sub.bounds_min[1] = lo.y;
        sub.bounds_min[2] = lo.z;
        sub.bounds_max[0] = hi.x;
        sub.bounds_max[1] = hi.y;
        sub.bounds_max[2] = hi.z;
    }

    Model model;
    model.mesh.vertices = core::Span<const MeshVertex>(writer.vertices.data(), total_verts);
    model.mesh.indices = core::Span<const u32>(writer.indices.data(), total_indices);
    model.mesh.submeshes = core::Span<const Submesh>(submeshes.data(), submeshes.size());
    model.textures = core::Span<const TextureData>(textures.data(), texture_count);
    if (skin_import.has) {
        model.skin = core::Span<const SkinVertex>(writer.skin.data(), writer.skin.size());
        model.skeleton = skin_import.skeleton;
        model.clips = import_clips(data, skin_import, arena);
        sweep_posed_bounds(arena, model.skeleton, model.clips, model.mesh.vertices, model.skin,
                           model.mesh.indices, submeshes);
    }

    cgltf_free(data);
    return ImportResult::ok(model);
}

}  // namespace asset
