// Offline asset importer: glTF in, native engine formats out.
//
//   um_import <model.gltf|.glb> <out_base>
//
// Writes <out_base>.umesh and, when the model has a base color texture,
// <out_base>.utex.

#include "asset/asset.h"
#include "core/arena.h"
#include "core/log.h"
#include "core/types.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 3) {
        core::log_error("usage: um_import <model.gltf|.glb> <out_base>");
        return 1;
    }
    const char* in_path = argv[1];
    const char* out_base = argv[2];

    core::Arena arena = core::Arena::with_capacity(256ull << 20);

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
    core::log_infof("um_import: wrote %s (%llu vertices, %llu indices)", path,
                    static_cast<unsigned long long>(model.mesh.vertices.size()),
                    static_cast<unsigned long long>(model.mesh.indices.size()));

    if (model.has_texture) {
        std::snprintf(path, sizeof(path), "%s.utex", out_base);
        core::Result<core::Unit, const char*> tex_saved = asset::texture_save(path, model.base_color);
        if (tex_saved.is_err()) {
            core::log_errorf("um_import: %s: %s", path, tex_saved.error());
            return 1;
        }
        core::log_infof("um_import: wrote %s (%ux%u)", path, model.base_color.width,
                        model.base_color.height);
    }
    return 0;
}
