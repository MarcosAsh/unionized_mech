// Offline asset importer: glTF in, native engine formats out.
//
//   um_import <model.gltf|.glb> <out_base>
//
// Writes <out_base>.umesh and one <out_base>.<i>.utex per base color texture.
// Prints the model bounds to help author collision volumes.

#include "asset/asset.h"
#include "core/arena.h"
#include "core/log.h"
#include "core/types.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) {
        core::log_error("usage: um_import <model.gltf|.glb|image.png|.jpg> <out_base>");
        return 1;
    }
    const char* in_path = argv[1];
    const char* out_base = argv[2];

    // A loose image converts straight to one texture. Keeps material art on the
    // same download-pin-convert path as the models rather than a second one.
    const u64 in_len = __builtin_strlen(in_path);
    const bool is_model = in_len > 5 && (__builtin_strcmp(in_path + in_len - 5, ".gltf") == 0 ||
                                         __builtin_strcmp(in_path + in_len - 4, ".glb") == 0);
    if (!is_model) {
        core::Arena image_arena = core::Arena::with_capacity(256ull << 20);
        core::Result<asset::TextureData, const char*> img =
            asset::image_import(image_arena, in_path);
        if (img.is_err()) {
            core::log_errorf("um_import: %s", img.error());
            return 1;
        }
        char tex_path[512];
        std::snprintf(tex_path, sizeof(tex_path), "%s.utex", out_base);
        if (asset::texture_save(tex_path, img.value()).is_err()) {
            core::log_errorf("um_import: could not write %s", tex_path);
            return 1;
        }
        core::log_infof("um_import: %s -> %s (%ux%u)", in_path, tex_path, img.value().width,
                        img.value().height);
        return 0;
    }

    core::Arena arena = core::Arena::with_capacity(1024ull << 20);

    core::Result<asset::Model, const char*> imported = asset::import_gltf(arena, in_path);
    if (imported.is_err()) {
        core::log_errorf("um_import: %s: %s", in_path, imported.error());
        return 1;
    }
    const asset::Model& model = imported.value();

    char path[1024];
    std::snprintf(path, sizeof(path), "%s.umesh", out_base);
    core::Result<core::Unit, const char*> saved = asset::mesh_save(path, model.mesh);
    if (saved.is_err()) {
        core::log_errorf("um_import: %s: %s", path, saved.error());
        return 1;
    }
    core::log_infof("um_import: wrote %s (%llu vertices, %llu indices, %llu submeshes)", path,
                    static_cast<unsigned long long>(model.mesh.vertices.size()),
                    static_cast<unsigned long long>(model.mesh.indices.size()),
                    static_cast<unsigned long long>(model.mesh.submeshes.size()));

    for (u64 t = 0; t < model.textures.size(); ++t) {
        std::snprintf(path, sizeof(path), "%s.%llu.utex", out_base,
                      static_cast<unsigned long long>(t));
        core::Result<core::Unit, const char*> tex_saved =
            asset::texture_save(path, model.textures[t]);
        if (tex_saved.is_err()) {
            core::log_errorf("um_import: %s: %s", path, tex_saved.error());
            return 1;
        }
    }
    if (!model.textures.empty()) {
        core::log_infof("um_import: wrote %llu textures",
                        static_cast<unsigned long long>(model.textures.size()));
    }

    if (model.skeleton.joint_count > 0) {
        std::snprintf(path, sizeof(path), "%s.uskel", out_base);
        if (anim::skeleton_save(path, model.skeleton).is_err()) {
            core::log_errorf("um_import: could not write %s", path);
            return 1;
        }
        std::snprintf(path, sizeof(path), "%s.uskin", out_base);
        if (asset::skin_save(path, model.skin).is_err()) {
            core::log_errorf("um_import: could not write %s", path);
            return 1;
        }
        for (u64 c = 0; c < model.clips.size(); ++c) {
            std::snprintf(path, sizeof(path), "%s.%llu.uclip", out_base,
                          static_cast<unsigned long long>(c));
            if (anim::clip_save(path, model.clips[c], model.skeleton.joint_count).is_err()) {
                core::log_errorf("um_import: could not write %s", path);
                return 1;
            }
        }
        core::log_infof("um_import: wrote skeleton (%u joints) and %llu clips",
                        model.skeleton.joint_count,
                        static_cast<unsigned long long>(model.clips.size()));
    }

    // The union of the submesh boxes, which for a rigged model is the space its
    // animations sweep rather than its bind pose. That is the size it draws at,
    // so it is the number to size a hull or a spawn clearance against.
    const bool rigged = model.skeleton.joint_count > 0;
    core::Vec3 lo{model.mesh.submeshes[0].bounds_min[0], model.mesh.submeshes[0].bounds_min[1],
                  model.mesh.submeshes[0].bounds_min[2]};
    core::Vec3 hi{model.mesh.submeshes[0].bounds_max[0], model.mesh.submeshes[0].bounds_max[1],
                  model.mesh.submeshes[0].bounds_max[2]};
    for (u64 i = 1; i < model.mesh.submeshes.size(); ++i) {
        const asset::Submesh& sub = model.mesh.submeshes[i];
        lo.x = sub.bounds_min[0] < lo.x ? sub.bounds_min[0] : lo.x;
        lo.y = sub.bounds_min[1] < lo.y ? sub.bounds_min[1] : lo.y;
        lo.z = sub.bounds_min[2] < lo.z ? sub.bounds_min[2] : lo.z;
        hi.x = sub.bounds_max[0] > hi.x ? sub.bounds_max[0] : hi.x;
        hi.y = sub.bounds_max[1] > hi.y ? sub.bounds_max[1] : hi.y;
        hi.z = sub.bounds_max[2] > hi.z ? sub.bounds_max[2] : hi.z;
    }
    core::log_infof("um_import: %s bounds min (%.2f, %.2f, %.2f) max (%.2f, %.2f, %.2f)",
                    rigged ? "posed" : "mesh", static_cast<f64>(lo.x), static_cast<f64>(lo.y),
                    static_cast<f64>(lo.z), static_cast<f64>(hi.x), static_cast<f64>(hi.y),
                    static_cast<f64>(hi.z));
    return 0;
}
