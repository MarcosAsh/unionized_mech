#include "asset/asset.h"

#include <cgltf.h>
#include <stb_image.h>

#include <cstring>

namespace asset {

using ImportResult = core::Result<Model, const char*>;

namespace {

struct Counts {
    u64 vertices = 0;
    u64 indices = 0;
};

bool is_triangles(const cgltf_primitive& prim) {
    return prim.type == cgltf_primitive_type_triangles && prim.indices != nullptr;
}

void count_node(const cgltf_node* node, Counts* counts) {
    if (node->mesh != nullptr) {
        for (u64 p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node->mesh->primitives[p];
            if (!is_triangles(prim)) {
                continue;
            }
            for (u64 a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position) {
                    counts->vertices += prim.attributes[a].data->count;
                    break;
                }
            }
            counts->indices += prim.indices->count;
        }
    }
    for (u64 c = 0; c < node->children_count; ++c) {
        count_node(node->children[c], counts);
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
    u64 vert_cursor = 0;
    u64 index_cursor = 0;
};

void append_node(const cgltf_node* node, Writer* w) {
    if (node->mesh != nullptr) {
        f32 world[16];
        cgltf_node_transform_world(node, world);

        for (u64 p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node->mesh->primitives[p];
            if (!is_triangles(prim)) {
                continue;
            }

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* normal = nullptr;
            const cgltf_accessor* uv = nullptr;
            for (u64 a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) {
                    pos = attr.data;
                } else if (attr.type == cgltf_attribute_type_normal) {
                    normal = attr.data;
                } else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
                    uv = attr.data;
                }
            }
            if (pos == nullptr) {
                continue;
            }

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
                w->vertices[w->vert_cursor] = vertex;
                ++w->vert_cursor;
            }

            for (u64 i = 0; i < prim.indices->count; ++i) {
                w->indices[w->index_cursor] =
                    static_cast<u32>(base + cgltf_accessor_read_index(prim.indices, i));
                ++w->index_cursor;
            }
        }
    }
    for (u64 c = 0; c < node->children_count; ++c) {
        append_node(node->children[c], w);
    }
}

// The first base color texture with pixel data reachable in a buffer view.
const cgltf_buffer_view* find_base_color(const cgltf_data* data) {
    for (u64 m = 0; m < data->materials_count; ++m) {
        const cgltf_material& mat = data->materials[m];
        if (!mat.has_pbr_metallic_roughness) {
            continue;
        }
        const cgltf_texture* tex = mat.pbr_metallic_roughness.base_color_texture.texture;
        if (tex != nullptr && tex->image != nullptr && tex->image->buffer_view != nullptr) {
            return tex->image->buffer_view;
        }
    }
    return nullptr;
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

    Counts counts;
    for (u64 n = 0; n < scene->nodes_count; ++n) {
        count_node(scene->nodes[n], &counts);
    }
    if (counts.vertices == 0 || counts.indices == 0) {
        cgltf_free(data);
        return ImportResult::err("glTF has no triangle geometry");
    }

    Writer writer;
    writer.vertices = arena.alloc_n<MeshVertex>(counts.vertices);
    writer.indices = arena.alloc_n<u32>(counts.indices);
    for (u64 n = 0; n < scene->nodes_count; ++n) {
        append_node(scene->nodes[n], &writer);
    }

    Model model;
    model.mesh.vertices =
        core::Span<const MeshVertex>(writer.vertices.data(), writer.vert_cursor);
    model.mesh.indices = core::Span<const u32>(writer.indices.data(), writer.index_cursor);

    const cgltf_buffer_view* image_view = find_base_color(data);
    if (image_view != nullptr && image_view->buffer != nullptr &&
        image_view->buffer->data != nullptr) {
        const u8* encoded = static_cast<const u8*>(image_view->buffer->data) + image_view->offset;
        int w = 0;
        int h = 0;
        int comp = 0;
        u8* pixels = stbi_load_from_memory(encoded, static_cast<int>(image_view->size), &w, &h,
                                           &comp, 4);
        if (pixels != nullptr) {
            const u64 bytes = static_cast<u64>(w) * static_cast<u64>(h) * 4u;
            const core::Span<u8> copy = arena.alloc_n<u8>(bytes);
            std::memcpy(copy.data(), pixels, bytes);
            stbi_image_free(pixels);
            model.base_color.width = static_cast<u32>(w);
            model.base_color.height = static_cast<u32>(h);
            model.base_color.rgba = core::Span<const u8>(copy.data(), bytes);
            model.has_texture = true;
        }
    }

    cgltf_free(data);
    return ImportResult::ok(model);
}

}  // namespace asset
