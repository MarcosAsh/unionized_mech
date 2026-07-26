#include "sim/sim.h"

#include "core/file.h"

#include <cstring>

namespace sim {

namespace {

// Tape layout: header then tick_count tightly packed InputCmds. InputCmd's
// layout is static_asserted wire-stable in sim.h, so raw bytes are the format.
struct TapeHeader {
    u32 magic;
    u32 tick_count;
};

constexpr u32 TAPE_MAGIC = 0x30544D55u;  // "UMT0" little-endian

}  // namespace

core::Result<core::Unit, const char*> tape_save(const char* path,
                                                core::Span<const InputCmd> cmds) {
    using SaveResult = core::Result<core::Unit, const char*>;

    const u64 bytes_len = sizeof(TapeHeader) + cmds.size() * sizeof(InputCmd);
    // Tape saving is tooling, not the frame loop, so a scratch arena here is fine.
    core::Arena scratch = core::Arena::with_capacity(bytes_len + 64);
    const core::Span<u8> bytes = scratch.alloc_n<u8>(bytes_len);

    TapeHeader header{};
    header.magic = TAPE_MAGIC;
    header.tick_count = static_cast<u32>(cmds.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!cmds.empty()) {
        std::memcpy(bytes.data() + sizeof(header), cmds.data(), cmds.size() * sizeof(InputCmd));
    }

    core::Result<core::Unit, const char*> written =
        core::write_entire_file(path, core::Span<const u8>(bytes.data(), bytes.size()));
    if (written.is_err()) {
        return SaveResult::err(written.error());
    }
    return SaveResult::ok(core::Unit{});
}

core::Result<core::Span<const InputCmd>, const char*> tape_load(core::Arena& arena,
                                                                const char* path) {
    using LoadResult = core::Result<core::Span<const InputCmd>, const char*>;

    core::Result<core::Span<u8>, const char*> read = core::read_entire_file(arena, path);
    if (read.is_err()) {
        return LoadResult::err(read.error());
    }
    const core::Span<u8> bytes = read.value();

    if (bytes.size() < sizeof(TapeHeader)) {
        return LoadResult::err("tape too small for header");
    }
    TapeHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != TAPE_MAGIC) {
        return LoadResult::err("not an input tape");
    }
    const u64 expected = sizeof(TapeHeader) + static_cast<u64>(header.tick_count) * sizeof(InputCmd);
    if (bytes.size() != expected) {
        return LoadResult::err("tape length does not match header");
    }

    // The command payload is memcpy'd into aligned storage rather than aliased,
    // since the file bytes have no alignment guarantee.
    const core::Span<InputCmd> cmds = arena.alloc_n<InputCmd>(header.tick_count);
    if (header.tick_count > 0) {
        std::memcpy(cmds.data(), bytes.data() + sizeof(header),
                    static_cast<u64>(header.tick_count) * sizeof(InputCmd));
    }
    return LoadResult::ok(core::Span<const InputCmd>(cmds.data(), cmds.size()));
}

}  // namespace sim
