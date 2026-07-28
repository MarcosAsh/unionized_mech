#pragma once

// Internal to the asset module. Posed bounds for rigged meshes.

#include "asset/asset.h"
#include "asset_skin.h"

#include "anim/anim.h"
#include "core/arena.h"
#include "core/span.h"

namespace asset {

/// Widen each submesh's bounds over the rest pose and a sweep of every clip, so
/// a rigged model's cull box is the space its vertices actually reach rather
/// than the bind pose it happens to be stored in. A fully skinned mesh can bind
/// to an identity transform and measure centimetres across, and culling against
/// that hides the model from almost everywhere.
void sweep_posed_bounds(core::Arena& arena, const anim::Skeleton& skeleton,
                        core::Span<const anim::Clip> clips, core::Span<const MeshVertex> vertices,
                        core::Span<const SkinVertex> skin, core::Span<const u32> indices,
                        core::Span<Submesh> submeshes);

}  // namespace asset
