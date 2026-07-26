#include "anim/anim.h"

#include "core/file.h"

#include <cstring>

namespace anim {

namespace {

constexpr u32 SKEL_MAGIC = 0x4B534D55u;  // "UMSK" little-endian
constexpr u32 CLIP_MAGIC = 0x4C434D55u;  // "UMCL" little-endian

struct SkelHeader {
    u32 magic;
    u32 joint_count;
};

struct ClipHeader {
    u32 magic;
    u32 joint_count;
    f32 duration;
};

// Serialized tracks carry a key count then packed times and values.
u64 track_bytes(const Track& t) { return sizeof(u32) + (t.times.size() + t.values.size()) * 4; }

u8* write_track(u8* cursor, const Track& t) {
    const u32 keys = static_cast<u32>(t.times.size());
    std::memcpy(cursor, &keys, sizeof(keys));
    cursor += sizeof(keys);
    if (keys > 0) {
        std::memcpy(cursor, t.times.data(), t.times.size() * 4);
        cursor += t.times.size() * 4;
        std::memcpy(cursor, t.values.data(), t.values.size() * 4);
        cursor += t.values.size() * 4;
    }
    return cursor;
}

const u8* read_track(const u8* cursor, const u8* end, core::Arena& arena, u32 stride, Track* out) {
    if (cursor == nullptr || cursor + sizeof(u32) > end) {
        return nullptr;
    }
    u32 keys = 0;
    std::memcpy(&keys, cursor, sizeof(keys));
    cursor += sizeof(keys);
    if (keys == 0) {
        *out = Track{};
        return cursor;
    }
    const u64 floats = static_cast<u64>(keys) * (1 + stride);
    if (cursor + floats * 4 > end) {
        return nullptr;
    }
    const core::Span<f32> times = arena.alloc_n<f32>(keys);
    const core::Span<f32> values = arena.alloc_n<f32>(static_cast<u64>(keys) * stride);
    std::memcpy(times.data(), cursor, keys * 4);
    cursor += keys * 4;
    std::memcpy(values.data(), cursor, values.size() * 4);
    cursor += values.size() * 4;
    out->times = core::Span<const f32>(times.data(), times.size());
    out->values = core::Span<const f32>(values.data(), values.size());
    return cursor;
}

}  // namespace

core::Result<core::Unit, const char*> skeleton_save(const char* path, const Skeleton& skeleton) {
    using SaveResult = core::Result<core::Unit, const char*>;

    const u32 n = skeleton.joint_count;
    const u64 total = sizeof(SkelHeader) + n * (sizeof(i16) + 16 * 4 + 3 * 4 + 4 * 4 + 3 * 4);
    core::Arena scratch = core::Arena::with_capacity(total + 64);
    const core::Span<u8> bytes = scratch.alloc_n<u8>(total);

    SkelHeader header{SKEL_MAGIC, n};
    u8* cursor = bytes.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    std::memcpy(cursor, skeleton.parents, n * sizeof(i16));
    cursor += n * sizeof(i16);
    for (u32 j = 0; j < n; ++j) {
        std::memcpy(cursor, skeleton.inverse_bind[j].m, 16 * 4);
        cursor += 16 * 4;
    }
    std::memcpy(cursor, skeleton.rest_pos, n * sizeof(core::Vec3));
    cursor += n * sizeof(core::Vec3);
    std::memcpy(cursor, skeleton.rest_rot, n * sizeof(core::Quat));
    cursor += n * sizeof(core::Quat);
    std::memcpy(cursor, skeleton.rest_scale, n * sizeof(core::Vec3));

    core::Result<core::Unit, const char*> written =
        core::write_entire_file(path, core::Span<const u8>(bytes.data(), bytes.size()));
    if (written.is_err()) {
        return SaveResult::err(written.error());
    }
    return SaveResult::ok(core::Unit{});
}

core::Result<Skeleton, const char*> skeleton_load(core::Arena& arena, const char* path) {
    using LoadResult = core::Result<Skeleton, const char*>;

    core::Result<core::Span<u8>, const char*> read = core::read_entire_file(arena, path);
    if (read.is_err()) {
        return LoadResult::err(read.error());
    }
    const core::Span<u8> bytes = read.value();
    if (bytes.size() < sizeof(SkelHeader)) {
        return LoadResult::err("skeleton file too small");
    }
    SkelHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != SKEL_MAGIC || header.joint_count > MAX_JOINTS) {
        return LoadResult::err("not a native skeleton");
    }
    const u32 n = header.joint_count;
    const u64 expected = sizeof(SkelHeader) + n * (sizeof(i16) + 16 * 4 + 3 * 4 + 4 * 4 + 3 * 4);
    if (bytes.size() != expected) {
        return LoadResult::err("skeleton length does not match header");
    }

    Skeleton out;
    out.joint_count = n;
    const u8* cursor = bytes.data() + sizeof(header);
    std::memcpy(out.parents, cursor, n * sizeof(i16));
    cursor += n * sizeof(i16);
    for (u32 j = 0; j < n; ++j) {
        std::memcpy(out.inverse_bind[j].m, cursor, 16 * 4);
        cursor += 16 * 4;
    }
    std::memcpy(out.rest_pos, cursor, n * sizeof(core::Vec3));
    cursor += n * sizeof(core::Vec3);
    std::memcpy(out.rest_rot, cursor, n * sizeof(core::Quat));
    cursor += n * sizeof(core::Quat);
    std::memcpy(out.rest_scale, cursor, n * sizeof(core::Vec3));
    return LoadResult::ok(out);
}

core::Result<core::Unit, const char*> clip_save(const char* path, const Clip& clip,
                                                u32 joint_count) {
    using SaveResult = core::Result<core::Unit, const char*>;

    u64 total = sizeof(ClipHeader);
    for (u32 j = 0; j < joint_count; ++j) {
        total += track_bytes(clip.translation[j]) + track_bytes(clip.rotation[j]) +
                 track_bytes(clip.scale[j]);
    }
    core::Arena scratch = core::Arena::with_capacity(total + 64);
    const core::Span<u8> bytes = scratch.alloc_n<u8>(total);

    ClipHeader header{CLIP_MAGIC, joint_count, clip.duration};
    u8* cursor = bytes.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    for (u32 j = 0; j < joint_count; ++j) {
        cursor = write_track(cursor, clip.translation[j]);
        cursor = write_track(cursor, clip.rotation[j]);
        cursor = write_track(cursor, clip.scale[j]);
    }

    core::Result<core::Unit, const char*> written =
        core::write_entire_file(path, core::Span<const u8>(bytes.data(), bytes.size()));
    if (written.is_err()) {
        return SaveResult::err(written.error());
    }
    return SaveResult::ok(core::Unit{});
}

core::Result<Clip, const char*> clip_load(core::Arena& arena, const char* path) {
    using LoadResult = core::Result<Clip, const char*>;

    core::Result<core::Span<u8>, const char*> read = core::read_entire_file(arena, path);
    if (read.is_err()) {
        return LoadResult::err(read.error());
    }
    const core::Span<u8> bytes = read.value();
    if (bytes.size() < sizeof(ClipHeader)) {
        return LoadResult::err("clip file too small");
    }
    ClipHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != CLIP_MAGIC || header.joint_count > MAX_JOINTS) {
        return LoadResult::err("not a native clip");
    }

    Clip out;
    out.duration = header.duration;
    const u8* cursor = bytes.data() + sizeof(header);
    const u8* end = bytes.data() + bytes.size();
    for (u32 j = 0; j < header.joint_count; ++j) {
        cursor = read_track(cursor, end, arena, 3, &out.translation[j]);
        cursor = read_track(cursor, end, arena, 4, &out.rotation[j]);
        cursor = read_track(cursor, end, arena, 3, &out.scale[j]);
        if (cursor == nullptr) {
            return LoadResult::err("clip truncated");
        }
    }
    return LoadResult::ok(out);
}

}  // namespace anim
