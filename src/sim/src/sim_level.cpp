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

const Aabb LEVEL[] = {
    // Central plaza: varied blocks for jumping, sliding, and short wallruns.
    {-15.0f, 0.0f, -15.0f, -13.0f, 2.0f, -13.0f}, {12.0f, 0.0f, -16.0f, 16.0f, 4.0f, -12.0f},
    {-17.0f, 0.0f, 11.0f, -11.0f, 6.0f, 17.0f},   {10.0f, 0.0f, 10.0f, 18.0f, 8.0f, 18.0f},
    {-1.0f, 0.0f, -19.0f, 1.0f, 2.0f, -17.0f},    {-2.0f, 0.0f, 16.0f, 2.0f, 4.0f, 20.0f},
    {-21.0f, 0.0f, -3.0f, -15.0f, 6.0f, 3.0f},    {14.0f, 0.0f, -4.0f, 22.0f, 8.0f, 4.0f},
    {-7.0f, 0.0f, 5.0f, -5.0f, 2.0f, 7.0f},       {6.0f, 0.0f, -6.0f, 10.0f, 4.0f, -2.0f},
    // Wallrun gauntlet, east. Parallel offset walls: run the inner face, wall
    // jump across the gap to the next, zig-zag down the line.
    {20.0f, 0.0f, 33.0f, 30.0f, 6.0f, 34.5f},
    {32.0f, 0.0f, 25.5f, 42.0f, 6.0f, 27.0f},
    {44.0f, 0.0f, 33.0f, 54.0f, 6.0f, 34.5f},
    // Mantle stairs, south. Each step is vault height above the last.
    {-30.0f, 0.0f, -40.0f, -26.0f, 1.0f, -36.0f},
    {-25.0f, 0.0f, -40.0f, -21.0f, 2.0f, -36.0f},
    {-20.0f, 0.0f, -40.0f, -16.0f, 3.0f, -36.0f},
    {-15.0f, 0.0f, -40.0f, -11.0f, 4.0f, -36.0f},
    // Tower, north-west. Tall faces for long wallruns that end in a mantle.
    {-40.0f, 0.0f, 30.0f, -30.0f, 10.0f, 40.0f},
    // Practice wall, west. One long clean face for learning the wallrun.
    {-55.0f, 0.0f, -10.0f, -53.0f, 7.0f, 20.0f},

    // Everything below is collision-only, standing in for imported scenery.
    // The Sponza atrium sits at (0, 0, -90); these four volumes are its outer
    // walls so the interior is walkable and wallrunnable. Columns and railings
    // deliberately have no collision yet.
    {-15.4f, 0.0f, -83.8f, 14.4f, 12.0f, -81.2f},
    {-15.4f, 0.0f, -99.5f, 14.4f, 12.0f, -96.2f},
    {-15.4f, 0.0f, -99.5f, -13.0f, 12.0f, -81.2f},
    {13.0f, 0.0f, -99.5f, 14.4f, 12.0f, -81.2f},
};

constexpr u64 VISIBLE_COUNT = sizeof(LEVEL) / sizeof(LEVEL[0]) - 4;

// The active level. Immutable between loads: the built-in arrays above until a
// map file is loaded, then the loaded copy. This is environment data standing
// in for the compile-time constant it replaces, not hidden simulation state.
constexpr u64 MAX_LEVEL_BOXES = 256;
Aabb loaded_boxes[MAX_LEVEL_BOXES];
u64 loaded_total = 0;
u64 loaded_visible = 0;
bool level_loaded = false;
Spawn loaded_spawn;

}  // namespace

core::Span<const Aabb> level_boxes() {
    if (level_loaded) {
        return core::Span<const Aabb>(loaded_boxes, loaded_total);
    }
    return core::Span<const Aabb>(LEVEL, sizeof(LEVEL) / sizeof(LEVEL[0]));
}

core::Span<const Aabb> visible_boxes() {
    if (level_loaded) {
        return core::Span<const Aabb>(loaded_boxes, loaded_visible);
    }
    return core::Span<const Aabb>(LEVEL, VISIBLE_COUNT);
}

Spawn level_spawn() { return level_loaded ? loaded_spawn : Spawn{}; }

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
    u64 visible_count = 0;
    u64 collision_count = 0;
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
    return LoadResult::ok(core::Unit{});
}

}  // namespace sim
