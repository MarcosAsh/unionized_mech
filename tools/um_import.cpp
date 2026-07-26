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
        core::log_error("usage: um_import <model.gltf|.glb> <out_base>");
        return 1;
    }
    const char* in_path = argv[1];
    const char* out_base = argv[2];

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

    core::Vec3 lo = model.mesh.vertices[0].pos;
    core::Vec3 hi = lo;
    for (u64 i = 1; i < model.mesh.vertices.size(); ++i) {
        const core::Vec3 p = model.mesh.vertices[i].pos;
        lo.x = p.x < lo.x ? p.x : lo.x;
        lo.y = p.y < lo.y ? p.y : lo.y;
        lo.z = p.z < lo.z ? p.z : lo.z;
        hi.x = p.x > hi.x ? p.x : hi.x;
        hi.y = p.y > hi.y ? p.y : hi.y;
        hi.z = p.z > hi.z ? p.z : hi.z;
    }
    core::log_infof("um_import: bounds min (%.2f, %.2f, %.2f) max (%.2f, %.2f, %.2f)",
                    static_cast<f64>(lo.x), static_cast<f64>(lo.y), static_cast<f64>(lo.z),
                    static_cast<f64>(hi.x), static_cast<f64>(hi.y), static_cast<f64>(hi.z));
    return 0;
}
