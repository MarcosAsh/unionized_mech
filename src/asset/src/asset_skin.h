#pragma once

// Internal to the asset module. Skeleton and animation extraction from glTF.

#include "anim/anim.h"
#include "asset/asset.h"
#include "core/arena.h"
#include "core/span.h"
#include "core/types.h"

struct cgltf_data;

namespace asset {

/// The imported skeleton and the mapping from glTF skin joint order to engine
/// joint order (which is topologically sorted, parents first).
struct SkinImport {
    bool has = false;
    anim::Skeleton skeleton;
    u32 gltf_to_engine[anim::MAX_JOINTS] = {};
};

/// Extract the first skin's skeleton, reordered parent-before-child.
[[nodiscard]] bool import_skeleton(const cgltf_data* data, SkinImport* out);

/// Extract every animation as a clip over the imported skeleton's joints.
[[nodiscard]] core::Span<const anim::Clip> import_clips(const cgltf_data* data,
                                                        const SkinImport& skin,
                                                        core::Arena& arena);

}  // namespace asset
