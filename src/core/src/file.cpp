#define _POSIX_C_SOURCE 200809L

#include "core/file.h"

#include <cstdio>
#include <sys/stat.h>

namespace core {

using ReadResult = Result<Span<u8>, const char*>;
using WriteResult = Result<Unit, const char*>;

ReadResult read_entire_file(Arena& arena, const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return ReadResult::err("could not open file for reading");
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(file);
        return ReadResult::err("could not size file");
    }

    const Span<u8> bytes = arena.alloc_n<u8>(static_cast<u64>(size));
    const u64 read = std::fread(bytes.data(), 1, static_cast<u64>(size), file);
    std::fclose(file);
    if (read != static_cast<u64>(size)) {
        return ReadResult::err("short read");
    }
    return ReadResult::ok(bytes);
}

WriteResult write_entire_file(const char* path, Span<const u8> bytes) {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return WriteResult::err("could not open file for writing");
    }
    const u64 written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (written != bytes.size()) {
        return WriteResult::err("short write");
    }
    return WriteResult::ok(Unit{});
}

i64 file_mtime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return static_cast<i64>(st.st_mtim.tv_sec) * 1000000000 + static_cast<i64>(st.st_mtim.tv_nsec);
}

}  // namespace core
