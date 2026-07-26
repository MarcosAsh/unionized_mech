#include "asset/asset.h"

#include "core/file.h"

#include <cstring>

namespace asset {

namespace {

// Native formats: a header then tightly packed payload. MeshVertex's layout is
// static_asserted file-stable in asset.h, so raw bytes are the format.
struct MeshHeader {
    u32 magic;
    u32 vertex_count;
    u32 index_count;
};

struct TextureHeader {
    u32 magic;
    u32 width;
    u32 height;
};

constexpr u32 MESH_MAGIC = 0x48534D55u;  // "UMSH" little-endian
constexpr u32 TEX_MAGIC = 0x58544D55u;   // "UMTX" little-endian

}  // namespace

core::Result<core::Unit, const char*> mesh_save(const char* path, const MeshData& mesh) {
    using SaveResult = core::Result<core::Unit, const char*>;

    const u64 verts_bytes = mesh.vertices.size() * sizeof(MeshVertex);
    const u64 index_bytes = mesh.indices.size() * sizeof(u32);
    const u64 total = sizeof(MeshHeader) + verts_bytes + index_bytes;

    core::Arena scratch = core::Arena::with_capacity(total + 64);
    const core::Span<u8> bytes = scratch.alloc_n<u8>(total);

    MeshHeader header{};
    header.magic = MESH_MAGIC;
    header.vertex_count = static_cast<u32>(mesh.vertices.size());
    header.index_count = static_cast<u32>(mesh.indices.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), mesh.vertices.data(), verts_bytes);
    std::memcpy(bytes.data() + sizeof(header) + verts_bytes, mesh.indices.data(), index_bytes);

    core::Result<core::Unit, const char*> written =
        core::write_entire_file(path, core::Span<const u8>(bytes.data(), bytes.size()));
    if (written.is_err()) {
        return SaveResult::err(written.error());
    }
    return SaveResult::ok(core::Unit{});
}

core::Result<MeshData, const char*> mesh_load(core::Arena& arena, const char* path) {
    using LoadResult = core::Result<MeshData, const char*>;

    core::Result<core::Span<u8>, const char*> read = core::read_entire_file(arena, path);
    if (read.is_err()) {
        return LoadResult::err(read.error());
    }
    const core::Span<u8> bytes = read.value();
    if (bytes.size() < sizeof(MeshHeader)) {
        return LoadResult::err("mesh file too small for header");
    }
    MeshHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != MESH_MAGIC) {
        return LoadResult::err("not a native mesh");
    }
    const u64 verts_bytes = static_cast<u64>(header.vertex_count) * sizeof(MeshVertex);
    const u64 index_bytes = static_cast<u64>(header.index_count) * sizeof(u32);
    if (bytes.size() != sizeof(MeshHeader) + verts_bytes + index_bytes) {
        return LoadResult::err("mesh length does not match header");
    }

    // Copied into aligned storage rather than aliased into the file bytes.
    const core::Span<MeshVertex> verts = arena.alloc_n<MeshVertex>(header.vertex_count);
    const core::Span<u32> indices = arena.alloc_n<u32>(header.index_count);
    std::memcpy(verts.data(), bytes.data() + sizeof(header), verts_bytes);
    std::memcpy(indices.data(), bytes.data() + sizeof(header) + verts_bytes, index_bytes);

    MeshData out;
    out.vertices = core::Span<const MeshVertex>(verts.data(), verts.size());
    out.indices = core::Span<const u32>(indices.data(), indices.size());
    return LoadResult::ok(out);
}

core::Result<core::Unit, const char*> texture_save(const char* path, const TextureData& texture) {
    using SaveResult = core::Result<core::Unit, const char*>;

    const u64 pixel_bytes = texture.rgba.size();
    core::Arena scratch = core::Arena::with_capacity(sizeof(TextureHeader) + pixel_bytes + 64);
    const core::Span<u8> bytes = scratch.alloc_n<u8>(sizeof(TextureHeader) + pixel_bytes);

    TextureHeader header{};
    header.magic = TEX_MAGIC;
    header.width = texture.width;
    header.height = texture.height;
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), texture.rgba.data(), pixel_bytes);

    core::Result<core::Unit, const char*> written =
        core::write_entire_file(path, core::Span<const u8>(bytes.data(), bytes.size()));
    if (written.is_err()) {
        return SaveResult::err(written.error());
    }
    return SaveResult::ok(core::Unit{});
}

core::Result<TextureData, const char*> texture_load(core::Arena& arena, const char* path) {
    using LoadResult = core::Result<TextureData, const char*>;

    core::Result<core::Span<u8>, const char*> read = core::read_entire_file(arena, path);
    if (read.is_err()) {
        return LoadResult::err(read.error());
    }
    const core::Span<u8> bytes = read.value();
    if (bytes.size() < sizeof(TextureHeader)) {
        return LoadResult::err("texture file too small for header");
    }
    TextureHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != TEX_MAGIC) {
        return LoadResult::err("not a native texture");
    }
    const u64 pixel_bytes = static_cast<u64>(header.width) * header.height * 4u;
    if (bytes.size() != sizeof(TextureHeader) + pixel_bytes) {
        return LoadResult::err("texture length does not match header");
    }

    TextureData out;
    out.width = header.width;
    out.height = header.height;
    out.rgba = core::Span<const u8>(bytes.data() + sizeof(header), pixel_bytes);
    return LoadResult::ok(out);
}

}  // namespace asset
