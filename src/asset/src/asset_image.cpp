#include "asset/asset.h"

#include "core/file.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <cstring>

// Loose image files, in and out. Split from asset_import.cpp, which had grown
// past the project's file limit.

namespace asset {


core::Result<core::Unit, const char*> image_write_png(const char* path, const u8* rgba,
                                                      u32 width, u32 height) {
    const int stride = static_cast<int>(width) * 4;
    if (stbi_write_png(path, static_cast<int>(width), static_cast<int>(height), 4, rgba,
                       stride) == 0) {
        return core::Result<core::Unit, const char*>::err("image_write_png: write failed");
    }
    return core::Result<core::Unit, const char*>::ok(core::Unit{});
}

core::Result<TextureData, const char*> image_import(core::Arena& arena, const char* path) {
    using ImageResult = core::Result<TextureData, const char*>;
    core::Result<core::Span<u8>, const char*> file = core::read_entire_file(arena, path);
    if (file.is_err()) {
        return ImageResult::err(file.error());
    }
    int w = 0;
    int h = 0;
    int comp = 0;
    u8* pixels = stbi_load_from_memory(file.value().data(), static_cast<int>(file.value().size()),
                                       &w, &h, &comp, 4);
    if (pixels == nullptr) {
        return ImageResult::err("image_import: not a decodable image");
    }
    const u64 bytes = static_cast<u64>(w) * static_cast<u64>(h) * 4u;
    const core::Span<u8> copy = arena.alloc_n<u8>(bytes);
    std::memcpy(copy.data(), pixels, bytes);
    stbi_image_free(pixels);

    TextureData out;
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.rgba = core::Span<const u8>(copy.data(), bytes);
    return ImageResult::ok(out);
}

}  // namespace asset
