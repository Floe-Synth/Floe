// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "foundation/foundation.hpp"
#include "utils/reader.hpp"

#include "audio_data.hpp"
#include "common/constants.hpp"
#include "mdata.hpp"

namespace sample_lib {

struct Range {
    constexpr bool operator==(Range const& other) const = default;
    constexpr u8 Size() const {
        ASSERT(end >= start);
        return end - start;
    }
    constexpr bool Contains(u8 v) const { return v >= start && v < end; }
    u8 start;
    u8 end; // non-inclusive, A.K.A. one-past the last
};

enum class TriggerEvent { NoteOn, NoteOff, Count };

// start and end can be negative meaning they're indexed from the end of the sample.
// e.g. -1 == num_frames, -2 == (num_frames - 1), etc.
struct Loop {
    s64 start_frame {};
    s64 end_frame {};
    u32 crossfade_frames {};
    bool ping_pong {};
};

struct Region {
    struct File {
        String path {};
        u8 root_key {};
        Optional<Loop> loop {};
    };

    struct TriggerCriteria {
        TriggerEvent event {TriggerEvent::NoteOn};
        Range key_range {0, 128};
        Range velocity_range {0, 100};
        Optional<u32> round_robin_index {};
    };

    struct Options {
        Optional<Range> timbre_crossfade_region {};
        bool feather_overlapping_velocity_regions {};

        // private
        Optional<String> auto_map_key_range_group {};
    };

    File file {};
    TriggerCriteria trigger {};
    Options options {};
};

struct Library;

struct Instrument {
    Library const& library;

    String name {};
    Optional<String> folders {};
    Optional<String> description {};
    Span<String> tags {};
    String audio_file_path_for_waveform {};
    Span<Region> regions {};
    usize regions_allocated_capacity {}; // private

    u32 max_rr_pos {};
};

// An instrument that has all it's audio data loaded into memory.
struct LoadedInstrument {
    Instrument const& instrument;
    Span<AudioData const*> audio_datas {}; // parallel to instrument.regions
    AudioData const* file_for_gui_waveform {};
};

struct ImpulseResponse {
    Library const& library;

    String name {};
    String path {};
};

// An impulse response that has all it's audio data loaded into memory.
struct LoadedIr {
    ImpulseResponse const& ir;
    AudioData const* audio_data;
};

enum class FileFormat { Mdata, Lua };

struct MdataSpecifics {
    HashTable<String, mdata::FileInfo const*> files_by_path;
    Span<mdata::FileInfo> file_infos {};
    String string_pool {};
    u64 file_data_pool_offset {}; // byte offset within the whole file
    Span<u8 const> file_data {}; // if the file from in-memory
};

struct LuaSpecifics {};

using FileFormatSpecifics = TaggedUnion<FileFormat,
                                        TypeAndTag<MdataSpecifics, FileFormat::Mdata>,
                                        TypeAndTag<LuaSpecifics, FileFormat::Lua>>;

struct LibraryIdRef {
    bool operator==(LibraryIdRef const& other) const = default;
    LibraryIdRef Clone(Allocator& arena, CloneType _ = CloneType::Shallow) const {
        return {.author = arena.Clone(author), .name = arena.Clone(name)};
    }
    String author;
    String name;
};

struct Library {
    LibraryIdRef Id() const { return {.author = author, .name = name}; }
    String name {};
    String tagline {};
    Optional<String> url {};
    String author {};
    u32 minor_version {1};
    Optional<String> background_image_path {};
    Optional<String> icon_image_path {};
    HashTable<String, Instrument*> insts_by_name {};
    HashTable<String, ImpulseResponse*> irs_by_name {};
    String path {}; // .mdata or .lua
    u64 file_hash {};
    ErrorCodeOr<Reader> (*create_file_reader)(Library const&, String path) {};
    FileFormatSpecifics file_format_specifics;
};

constexpr LibraryIdRef k_builtin_library_id = {.author = "Floe"_s, .name = "Built-in"};

// MDATA libraries didn't have an author field
constexpr String k_mdata_library_author = "FrozenPlain"_s;
constexpr LibraryIdRef k_mirage_compat_library_id = {.author = k_mdata_library_author,
                                                     .name = "Mirage Compatibility"};

struct LibraryId {
    static LibraryId FromRef(LibraryIdRef const& ref) { return {.author = ref.author, .name = ref.name}; }
    static LibraryId FromLibrary(Library const& lib) { return {.author = lib.author, .name = lib.name}; }
    LibraryIdRef Ref() const { return {.author = author, .name = name}; }
    bool operator==(LibraryId const& other) const = default;
    operator LibraryIdRef() const { return Ref(); }
    DynamicArrayInline<char, k_max_library_author_size> author;
    DynamicArrayInline<char, k_max_library_name_size> name;
};

struct InstrumentId {
    bool operator==(InstrumentId const& other) const = default;
    bool operator==(LoadedInstrument const& inst) const {
        return library == LibraryId::FromLibrary(inst.instrument.library) &&
               inst_name == inst.instrument.name;
    }
    LibraryId library;
    DynamicArrayInline<char, k_max_instrument_name_size> inst_name;
};

struct IrId {
    bool operator==(IrId const& other) const = default;
    bool operator==(LoadedIr const& ir) const {
        return library == LibraryId::FromLibrary(ir.ir.library) && ir_name == ir.ir.name;
    }
    LibraryId library;
    DynamicArrayInline<char, k_max_ir_name_size> ir_name;
};

// only honoured by the lua system
struct Options {
    usize max_memory_allowed = Mb(128);
    f64 max_seconds_allowed = 20;
};

enum class LuaErrorCode {
    Memory,
    Syntax,
    Runtime,
    Timeout,
    Unexpected,
};
extern ErrorCodeCategory const lua_error_category;
inline ErrorCodeCategory const& ErrorCategoryForEnum(LuaErrorCode) { return lua_error_category; }

ErrorCodeOr<u64> MdataHash(Reader& reader);
ErrorCodeOr<u64> LuaHash(Reader& reader);
inline ErrorCodeOr<u64> Hash(Reader& reader, FileFormat format) {
    switch (format) {
        case FileFormat::Mdata: return MdataHash(reader);
        case FileFormat::Lua: return LuaHash(reader);
    }
    PanicIfReached();
    return {};
}

struct Error {
    ErrorCode code;
    String message;
};

struct TryHelpersOutcomeToError {
    TRY_HELPER_INHERIT(IsError, TryHelpers)
    TRY_HELPER_INHERIT(ExtractValue, TryHelpers)
    template <typename T>
    static sample_lib::Error ExtractError(ErrorCodeOr<T> const& o) {
        return {o.Error(), ""_s};
    }
};

using LibraryPtrOrError = ValueOrError<Library*, Error>;

inline bool FilenameIsFloeLuaFile(String path) {
    return IsEqualToCaseInsensitiveAscii(path, "floe.lua") ||
           EndsWithCaseInsensitiveAscii(path, ".floe.lua"_s);
}

LibraryPtrOrError ReadLua(Reader& reader,
                          String lua_filepath,
                          ArenaAllocator& result_arena,
                          ArenaAllocator& scatch_arena,
                          Options options = {});

LibraryPtrOrError
ReadMdata(Reader& reader, String filepath, ArenaAllocator& result_arena, ArenaAllocator& scratch_arena);

inline LibraryPtrOrError Read(Reader& reader,
                              FileFormat format,
                              String filepath,
                              ArenaAllocator& result_arena,
                              ArenaAllocator& scatch_arena,
                              Options options = {}) {
    switch (format) {
        case FileFormat::Mdata: return ReadMdata(reader, filepath, result_arena, scatch_arena);
        case FileFormat::Lua: return ReadLua(reader, filepath, result_arena, scatch_arena, options);
    }
    PanicIfReached();
}

ErrorCodeOr<void> WriteDocumentedLuaExample(Writer writer, bool include_comments = true);

} // namespace sample_lib

PUBLIC ErrorCodeOr<void>
CustomValueToString(Writer writer, sample_lib::LibraryIdRef id, fmt::FormatOptions options) {
    auto const sep = " - "_s;
    TRY(PadToRequiredWidthIfNeeded(writer, options, id.author.size + sep.size + id.name.size));
    TRY(writer.WriteChars(id.author));
    TRY(writer.WriteChars(sep));
    return writer.WriteChars(id.name);
}

PUBLIC u64 Hash(sample_lib::LibraryIdRef const& id) {
    // FNV-1a https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1a_hash
    u64 hash = 0xcbf29ce484222325;
    for (auto str : Array {id.author, "|", id.name}) {
        for (auto c : str) {
            hash ^= (u8)c;
            hash *= 0x100000001b3;
        }
    }
    return hash;
}
