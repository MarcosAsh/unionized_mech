#pragma once

// Internal to the render module. The scene's model set and shared GPU
// resources, visible to both scene setup and the per-frame draw.

#include "render_model.h"
#include "render_text.h"

#include <gpu/gpu.h>

namespace render {

/// The imported models and shared buffers, held in the permanent arena so the
/// public header can keep them behind a forward declaration.
struct SceneModels {
    RenderModel duck;
    RenderModel gun;
    RenderModel viewmodel;
    RenderModel trooper;
    RenderModel mech;
    RenderModel tracer;
    RenderModel tracer_vm;
    RenderModel hitmarker;
    RenderModel overlay;
    gpu::Blas level_blas;
    gpu::Blas trooper_blas;
    gpu::Blas mech_blas;
    gpu::Blas duck_blas;
    gpu::Buffer globals;
    SceneGlobals* globals_mapped = nullptr;
    Font font{};
    u32 white_texture = 0;
};

/// Modification time of `path` in nanoseconds, zero when missing. Drives the
/// shader and map live-reload polls.
[[nodiscard]] i64 scene_file_mtime(const char* path);

}  // namespace render
