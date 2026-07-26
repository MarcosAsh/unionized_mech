#include "sim_level.h"

// Bot waypoint navigation. Nodes are authored in the map; links are computed
// once on load (near, similar height, clear line of sight); paths are answered
// per query with a breadth-first search over the links. Stateless on purpose:
// no per-bot path storage, so the graph reloads live with the map and the
// characters stay wire-format stable.

namespace sim {

namespace {

constexpr u32 MAX_NAV_NODES = 64;
constexpr f32 LINK_RANGE = 20.0f;    // horizontal reach of one link
constexpr f32 LINK_MAX_RISE = 4.5f;  // vertical reach: a jump, mantle, or climb

struct NavNode {
    f32 x;
    f32 y;
    f32 z;
};

NavNode nodes[MAX_NAV_NODES];
u64 links[MAX_NAV_NODES];  // adjacency bitmask, one row per node
u32 node_count = 0;

u32 nearest_node(f32 x, f32 y, f32 z) {
    u32 best = 0;
    f32 best_d2 = 1e30f;
    for (u32 i = 0; i < node_count; ++i) {
        const f32 dx = nodes[i].x - x;
        const f32 dy = nodes[i].y - y;
        const f32 dz = nodes[i].z - z;
        // Height differences weigh heavier so a node on this floor beats a
        // slightly nearer one on a ledge above.
        const f32 d2 = dx * dx + dy * dy * 4.0f + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

}  // namespace

void nav_reset() { node_count = 0; }

bool nav_add_node(f32 x, f32 y, f32 z) {
    if (node_count >= MAX_NAV_NODES) {
        return false;
    }
    nodes[node_count++] = NavNode{x, y, z};
    return true;
}

void nav_build_links() {
    static_assert(MAX_NAV_NODES <= 64);  // adjacency rows are one u64
    for (u32 i = 0; i < node_count; ++i) {
        links[i] = 0;
    }
    for (u32 i = 0; i < node_count; ++i) {
        for (u32 j = i + 1; j < node_count; ++j) {
            const f32 dx = nodes[j].x - nodes[i].x;
            const f32 dy = nodes[j].y - nodes[i].y;
            const f32 dz = nodes[j].z - nodes[i].z;
            if (dy > LINK_MAX_RISE || dy < -LINK_MAX_RISE) {
                continue;
            }
            if (dx * dx + dz * dz > LINK_RANGE * LINK_RANGE) {
                continue;
            }
            if (!segment_clear(nodes[i].x, nodes[i].y + 1.2f, nodes[i].z, nodes[j].x,
                               nodes[j].y + 1.2f, nodes[j].z)) {
                continue;
            }
            links[i] |= 1ull << j;
            links[j] |= 1ull << i;
        }
    }
}

u32 nav_count() { return node_count; }

bool nav_next_hop(f32 x, f32 y, f32 z, u32 goal, f32* out_x, f32* out_y, f32* out_z) {
    if (goal >= node_count) {
        return false;
    }
    const u32 start = nearest_node(x, y, z);
    u32 hop = goal;
    if (start != goal) {
        u8 parent[MAX_NAV_NODES];
        for (u32 i = 0; i < node_count; ++i) {
            parent[i] = 0xFF;
        }
        u8 queue[MAX_NAV_NODES];
        u32 head = 0;
        u32 tail = 0;
        queue[tail++] = static_cast<u8>(start);
        parent[start] = static_cast<u8>(start);
        while (head < tail && parent[goal] == 0xFF) {
            const u32 n = queue[head++];
            u64 m = links[n];
            while (m != 0) {
                const u32 j = static_cast<u32>(__builtin_ctzll(m));
                m &= m - 1;
                if (parent[j] == 0xFF) {
                    parent[j] = static_cast<u8>(n);
                    queue[tail++] = static_cast<u8>(j);
                }
            }
        }
        if (parent[goal] == 0xFF) {
            return false;  // no route from here to the goal
        }
        u32 n = goal;
        while (parent[n] != start) {
            n = parent[n];
        }
        hop = n;
    }
    *out_x = nodes[hop].x;
    *out_y = nodes[hop].y;
    *out_z = nodes[hop].z;
    return true;
}

}  // namespace sim
