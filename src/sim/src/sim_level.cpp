#include "sim_level.h"

#include "core/file.h"
#include "core/parse.h"

#include <cstring>

namespace sim {

bool hull_overlaps(f32 x, f32 y, f32 z, f32 height, const Aabb& b) {
    return x - HULL_HALF_WIDTH < b.max_x && x + HULL_HALF_WIDTH > b.min_x && y < b.max_y &&
           y + height > b.min_y && z - HULL_HALF_WIDTH < b.max_z && z + HULL_HALF_WIDTH > b.min_z;
}

bool find_wall(f32 x, f32 y, f32 z, f32 height, core::Span<const Aabb> boxes, f32* out_nx,
               f32* out_nz, f32* out_top) {
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (!(y < b.max_y && y + height > b.min_y)) {
            continue;  // no vertical overlap with this box
        }
        const bool z_overlap = z - HULL_HALF_WIDTH < b.max_z && z + HULL_HALF_WIDTH > b.min_z;
        const bool x_overlap = x - HULL_HALF_WIDTH < b.max_x && x + HULL_HALF_WIDTH > b.min_x;
        if (z_overlap) {
            if ((x - HULL_HALF_WIDTH) - b.max_x >= -0.01f &&
                (x - HULL_HALF_WIDTH) - b.max_x <= WALL_DETECT_DIST) {
                *out_nx = 1.0f;
                *out_nz = 0.0f;
                *out_top = b.max_y;
                return true;
            }
            if (b.min_x - (x + HULL_HALF_WIDTH) >= -0.01f &&
                b.min_x - (x + HULL_HALF_WIDTH) <= WALL_DETECT_DIST) {
                *out_nx = -1.0f;
                *out_nz = 0.0f;
                *out_top = b.max_y;
                return true;
            }
        }
        if (x_overlap) {
            if ((z - HULL_HALF_WIDTH) - b.max_z >= -0.01f &&
                (z - HULL_HALF_WIDTH) - b.max_z <= WALL_DETECT_DIST) {
                *out_nx = 0.0f;
                *out_nz = 1.0f;
                *out_top = b.max_y;
                return true;
            }
            if (b.min_z - (z + HULL_HALF_WIDTH) >= -0.01f &&
                b.min_z - (z + HULL_HALF_WIDTH) <= WALL_DETECT_DIST) {
                *out_nx = 0.0f;
                *out_nz = -1.0f;
                *out_top = b.max_y;
                return true;
            }
        }
    }
    return false;
}

namespace {

// The active level, populated by load_level and immutable between loads. There
// is deliberately no built-in fallback: the map file is the only description of
// the level that exists, so the geometry the game plays, the geometry the tests
// assert against, and the geometry the determinism harness hashes cannot drift
// apart. A second hand-maintained copy did exactly that. This is environment
// data standing in for the compile-time constant it replaces, not hidden
// simulation state.
constexpr u64 MAX_LEVEL_BOXES = 256;
Aabb loaded_boxes[MAX_LEVEL_BOXES];
u64 loaded_total = 0;
u64 loaded_visible = 0;
bool level_loaded = false;
Spawn loaded_spawn;

}  // namespace

core::Span<const Aabb> level_boxes() {
    return core::Span<const Aabb>(loaded_boxes, loaded_total);
}

core::Span<const Aabb> visible_boxes() {
    return core::Span<const Aabb>(loaded_boxes, loaded_visible);
}

Spawn level_spawn() { return level_loaded ? loaded_spawn : Spawn{}; }

Spawn team_spawn(u32 team, u32 slot) {
    // The teams muster on opposite edges of the plaza, facing each other.
    Spawn spawn;
    const f32 spread = static_cast<f32>(slot % 5) * 3.0f - 6.0f;
    if (team == 0) {
        spawn.x = spread;
        spawn.z = 38.0f;
        spawn.yaw = 0.0f;  // yaw 0 faces -Z, toward the far team
    } else {
        spawn.x = spread;
        spawn.z = -41.0f;
        spawn.yaw = 3.14159265f;  // faces +Z, toward the plaza
    }
    return spawn;
}

core::Result<core::Unit, const char*> load_level(core::Arena& scratch, const char* path) {
    using LoadResult = core::Result<core::Unit, const char*>;

    const u64 marker = scratch.marker();
    core::Result<core::Span<u8>, const char*> read = core::read_entire_file(scratch, path);
    if (read.is_err()) {
        return LoadResult::err(read.error());
    }
    const core::Span<u8> bytes = read.value();

    // Visible boxes fill from the front, collision-only volumes from the back,
    // then the back run is copied after the front so the visible prefix holds.
    Aabb visible[MAX_LEVEL_BOXES];
    Aabb collision[MAX_LEVEL_BOXES];
    f32 nodes[64][3];
    u64 visible_count = 0;
    u64 collision_count = 0;
    u64 node_count = 0;
    Spawn spawn;

    core::Cursor cursor{reinterpret_cast<const char*>(bytes.data()),
                        reinterpret_cast<const char*>(bytes.data()) + bytes.size()};
    char word[32];
    while (core::next_token(cursor, word, sizeof(word))) {
        if (std::strcmp(word, "spawn") == 0) {
            if (!core::parse_f32(cursor, &spawn.x) || !core::parse_f32(cursor, &spawn.y) ||
                !core::parse_f32(cursor, &spawn.z) || !core::parse_f32(cursor, &spawn.yaw)) {
                scratch.rewind(marker);
                return LoadResult::err("map: bad spawn line");
            }
        } else if (std::strcmp(word, "box") == 0 || std::strcmp(word, "cbox") == 0) {
            const bool is_visible = word[0] == 'b';
            Aabb box{};
            if (!core::parse_f32(cursor, &box.min_x) || !core::parse_f32(cursor, &box.min_y) ||
                !core::parse_f32(cursor, &box.min_z) || !core::parse_f32(cursor, &box.max_x) ||
                !core::parse_f32(cursor, &box.max_y) || !core::parse_f32(cursor, &box.max_z)) {
                scratch.rewind(marker);
                return LoadResult::err("map: bad box line");
            }
            if (visible_count + collision_count >= MAX_LEVEL_BOXES) {
                scratch.rewind(marker);
                return LoadResult::err("map: too many boxes");
            }
            if (is_visible) {
                visible[visible_count++] = box;
            } else {
                collision[collision_count++] = box;
            }
        } else if (std::strcmp(word, "node") == 0) {
            if (node_count >= 64) {
                scratch.rewind(marker);
                return LoadResult::err("map: too many nodes");
            }
            f32* n = nodes[node_count];
            if (!core::parse_f32(cursor, &n[0]) || !core::parse_f32(cursor, &n[1]) ||
                !core::parse_f32(cursor, &n[2])) {
                scratch.rewind(marker);
                return LoadResult::err("map: bad node line");
            }
            ++node_count;
        } else {
            scratch.rewind(marker);
            return LoadResult::err("map: unknown directive");
        }
    }
    scratch.rewind(marker);

    for (u64 i = 0; i < visible_count; ++i) {
        loaded_boxes[i] = visible[i];
    }
    for (u64 i = 0; i < collision_count; ++i) {
        loaded_boxes[visible_count + i] = collision[i];
    }
    loaded_visible = visible_count;
    loaded_total = visible_count + collision_count;
    loaded_spawn = spawn;
    level_loaded = true;

    // The waypoint graph links against the boxes just installed, so it builds
    // last. A failed load above leaves the previous graph untouched.
    nav_reset();
    for (u64 i = 0; i < node_count; ++i) {
        (void)nav_add_node(nodes[i][0], nodes[i][1], nodes[i][2]);
    }
    nav_build_links();
    return LoadResult::ok(core::Unit{});
}

}  // namespace sim
