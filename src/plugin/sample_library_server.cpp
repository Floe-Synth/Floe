// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sample_library_server.hpp"

#include "foundation/foundation.hpp"
#include "os/filesystem.hpp"
#include "os/threading.hpp"
#include "tests/framework.hpp"
#include "utils/debug/debug.hpp"
#include "utils/reader.hpp"

#include "build_resources/embedded_files.h"
#include "common/common_errors.hpp"
#include "sample_library/audio_file.hpp"
#include "sample_library/sample_library.hpp"
#include "xxhash/xxhash.h"

inline String ToString(EmbeddedString s) { return {s.data, s.size}; }

// not threadsafe
static sample_lib::Library* BuiltinLibrary() {
    static sample_lib::Library builtin_library {
        .name = k_builtin_library_name,
        .tagline = "Built-in library",
        .url = FLOE_URL,
        .author = FLOE_VENDOR,
        .minor_version = 1,
        .background_image_path = nullopt,
        .icon_image_path = nullopt,
        .insts_by_name = {},
        .irs_by_name = {},
        .path = ":memory:",
        .file_hash = 100,
        .create_file_reader = [](sample_lib::Library const&, String path) -> ErrorCodeOr<Reader> {
            auto const embedded_irs = EmbeddedIrs();
            for (auto& ir : embedded_irs.irs)
                if (ToString(ir.filename) == path) return Reader::FromMemory({ir.data, ir.size});
            return ErrorCode(FilesystemError::PathDoesNotExist);
        },
        .file_format_specifics = sample_lib::LuaSpecifics {}, // unused
    };

    static bool init = false;
    if (!Exchange(init, true)) {
        static Array<sample_lib::ImpulseResponse, EmbeddedIr_Count> irs;
        for (auto const i : Range(ToInt(EmbeddedIr_Count))) {
            auto const& embedded = EmbeddedIrs().irs[i];
            irs[i] = sample_lib::ImpulseResponse {
                .name = ToString(embedded.name),
                .path = ToString(embedded.filename),
            };
        }

        static FixedSizeAllocator<1000> alloc;
        builtin_library.irs_by_name =
            decltype(builtin_library.irs_by_name)::Create(alloc, ToInt(EmbeddedIr_Count));

        for (auto& ir : irs)
            builtin_library.irs_by_name.InsertWithoutGrowing(ir.name, &ir);
    }

    return &builtin_library;
}

namespace sample_lib_server {

namespace detail {
u32 g_inst_debug_id = 0;
}

using namespace detail;

constexpr String k_trace_category = "SLL";
constexpr u32 k_trace_colour = 0xfcba03;

ListedAudioData::~ListedAudioData() {
    ZoneScoped;
    auto const s = state.Load();
    ASSERT(s == LoadingState::CompletedCancelled || s == LoadingState::CompletedWithError ||
           s == LoadingState::CompletedSucessfully);
    if (audio_data.interleaved_samples.size)
        AudioDataAllocator::Instance().Free(audio_data.interleaved_samples.ToByteSpan());
}

ListedInstrument::~ListedInstrument() {
    ZoneScoped;
    for (auto a : audio_data_set)
        a->refs.FetchSub(1);
    library_refs.FetchSub(1);
}

// NOTE: it's important that we pass this around by value because the point at which num_thread_pool_jobs == 0
// the original object will be destroyed. But if it's passed by value then it doesn't matter because the value
// is copied and the references within the object are still valid.
struct ThreadPoolContext {
    ThreadPool& pool;
    AtomicCountdown& num_thread_pool_jobs;
    WorkSignaller& completed_signaller;
};

struct LoadAudioAsyncArgs {
    ListedAudioData& audio_data;
};

static void LoadAudioAsync(ListedAudioData& audio_data,
                           sample_lib::Library const& lib,
                           ThreadPoolContext thread_pool_ctx) {
    thread_pool_ctx.num_thread_pool_jobs.Increase();
    thread_pool_ctx.pool.AddJob([&, thread_pool_ctx]() {
        ZoneScoped;
        DEFER {
            thread_pool_ctx.num_thread_pool_jobs.CountDown();
            thread_pool_ctx.completed_signaller.Signal();
        };

        {
            auto state = audio_data.state.Load();
            LoadingState new_state;
            do {
                if (state == LoadingState::PendingLoad)
                    new_state = LoadingState::Loading;
                else if (state == LoadingState::PendingCancel)
                    new_state = LoadingState::CompletedCancelled;
                else
                    PanicIfReached();
            } while (!audio_data.state.CompareExchangeWeak(state, new_state));

            if (new_state == LoadingState::CompletedCancelled) return;
        }

        ASSERT(audio_data.state.Load() == LoadingState::Loading);

        auto const outcome = [&audio_data, &lib]() -> ErrorCodeOr<AudioData> {
            auto reader = TRY(lib.create_file_reader(lib, audio_data.path));
            return DecodeAudioFile(reader, audio_data.path, AudioDataAllocator::Instance());
        }();

        LoadingState result;
        if (outcome.HasValue()) {
            audio_data.audio_data = outcome.Value();
            result = LoadingState::CompletedSucessfully;
        } else {
            audio_data.error = outcome.Error();
            result = LoadingState::CompletedWithError;
        }
        audio_data.state.Store(result);
    });
}

// if the audio load is cancelled, or pending-cancel, then queue up a load again
static void TriggerReloadIfAudioIsCancelled(ListedAudioData& audio_data,
                                            sample_lib::Library const& lib,
                                            ThreadPoolContext thread_pool_ctx,
                                            u32 debug_inst_id) {
    auto expected = LoadingState::PendingCancel;
    if (!audio_data.state.CompareExchangeStrong(expected, LoadingState::PendingLoad)) {
        if (expected == LoadingState::CompletedCancelled) {
            audio_data.state.Store(LoadingState::PendingLoad);
            TracyMessageEx({k_trace_category, k_trace_colour, -1u},
                           "instID:{}, reloading CompletedCancelled audio",
                           debug_inst_id);
            LoadAudioAsync(audio_data, lib, thread_pool_ctx);
        } else {
            TracyMessageEx({k_trace_category, k_trace_colour, -1u},
                           "instID:{}, reusing audio which is in state: {}",
                           debug_inst_id,
                           EnumToString(expected));
        }
    } else {
        TracyMessageEx({k_trace_category, k_trace_colour, -1u},
                       "instID:{}, audio swapped PendingCancel with PendingLoad",
                       debug_inst_id);
    }

    ASSERT(audio_data.state.Load() != LoadingState::CompletedCancelled &&
           audio_data.state.Load() != LoadingState::PendingCancel);
}

static ListedAudioData* FetchOrCreateAudioData(ArenaList<ListedAudioData, true>& audio_datas,
                                               sample_lib::Library const& lib,
                                               String path,
                                               ThreadPoolContext thread_pool_ctx,
                                               u32 debug_inst_id) {
    for (auto& d : audio_datas) {
        if (lib.name == d.library_name && d.path == path) {
            TriggerReloadIfAudioIsCancelled(d, lib, thread_pool_ctx, debug_inst_id);
            return &d;
        }
    }

    auto audio_data = audio_datas.PrependUninitialised();
    PLACEMENT_NEW(audio_data)
    ListedAudioData {
        .library_name = lib.name,
        .path = path,
        .audio_data = {},
        .refs = 0u,
        .state = LoadingState::PendingLoad,
        .error = {},
    };

    LoadAudioAsync(*audio_data, lib, thread_pool_ctx);
    return audio_data;
}

static ListedInstrument* FetchOrCreateInstrument(LibrariesList::Node& lib_node,
                                                 ArenaList<ListedAudioData, true>& audio_datas,
                                                 sample_lib::Instrument const& inst,
                                                 ThreadPoolContext thread_pool_ctx) {
    auto& lib = lib_node.value;
    ASSERT(&inst.library == lib.lib);

    for (auto& i : lib.instruments)
        if (i.inst.instrument.name == inst.name) {
            for (auto d : i.audio_data_set)
                TriggerReloadIfAudioIsCancelled(*d, *lib.lib, thread_pool_ctx, i.debug_id);
            return &i;
        }

    auto new_inst = lib.instruments.PrependUninitialised();
    PLACEMENT_NEW(new_inst)
    ListedInstrument {
        .inst = {inst},
        .refs = 0u,
        .library_refs = lib_node.reader_uses,
    };
    lib_node.reader_uses.FetchAdd(1);

    DynamicArray<ListedAudioData*> audio_data_set {new_inst->arena};

    new_inst->inst.audio_datas =
        new_inst->arena.AllocateExactSizeUninitialised<AudioData const*>(inst.regions.size);
    for (auto region_index : Range(inst.regions.size)) {
        auto& region_info = inst.regions[region_index];
        auto& audio_data = new_inst->inst.audio_datas[region_index];

        auto ref_audio_data = FetchOrCreateAudioData(audio_datas,
                                                     *lib.lib,
                                                     region_info.file.path,
                                                     thread_pool_ctx,
                                                     new_inst->debug_id);
        audio_data = &ref_audio_data->audio_data;

        dyn::AppendIfNotAlreadyThere(audio_data_set, ref_audio_data);

        if (inst.audio_file_path_for_waveform == region_info.file.path)
            new_inst->inst.file_for_gui_waveform = &ref_audio_data->audio_data;
    }

    for (auto d : audio_data_set)
        d->refs.FetchAdd(1);

    ASSERT(audio_data_set.size);
    new_inst->audio_data_set = audio_data_set.ToOwnedSpan();

    return new_inst;
}

// short-lived helper for tracking asynchronous library scanning/reading
struct LibrariesAsyncContext {
    struct Job {
        enum class Type { ReadLibrary, ScanFolder };

        struct ReadLibrary {
            struct Args {
                PathOrMemory path_or_memory;
                sample_lib::FileFormat format;
                LibrariesList& libraries;
            };
            struct Result {
                ArenaAllocator arena {PageAllocator::Instance()};
                Optional<sample_lib::LibraryPtrOrError> result {};
            };

            Args args;
            Result result {};
        };

        struct ScanFolder {
            struct Args {
                ScanFolderList::Node* folder;
            };
            struct Result {
                ErrorCodeOr<void> outcome {};
            };

            Args args;
            Result result {};
        };

        using DataUnion = TaggedUnion<Type,
                                      TypeAndTag<ReadLibrary*, Type::ReadLibrary>,
                                      TypeAndTag<ScanFolder*, Type::ScanFolder>>;

        DataUnion data;
        Atomic<Job*> next {nullptr};
        Atomic<bool> completed {false};
        bool handled {};
    };

    ThreadPool& thread_pool;
    WorkSignaller& work_signaller;

    Mutex job_mutex;
    ArenaAllocator job_arena {PageAllocator::Instance()};
    Atomic<Job*> jobs {};
    Atomic<u32> num_uncompleted_jobs {0};
};

static void ReadLibraryAsync(LibrariesAsyncContext& async_ctx,
                             LibrariesList& lib_list,
                             PathOrMemory path_or_memory,
                             sample_lib::FileFormat format);

// any thread
static void AddAsyncJob(LibrariesAsyncContext& async_ctx,
                        LibrariesList& lib_list,
                        LibrariesAsyncContext::Job::DataUnion data) {
    ZoneNamed(add_job, true);
    LibrariesAsyncContext::Job* job;
    {
        async_ctx.job_mutex.Lock();
        DEFER { async_ctx.job_mutex.Unlock(); };

        job = async_ctx.job_arena.NewUninitialised<LibrariesAsyncContext::Job>();
        PLACEMENT_NEW(job)
        LibrariesAsyncContext::Job {
            .data = data,
            .next = async_ctx.jobs.Load(MemoryOrder::Relaxed),
            .handled = false,
        };
        async_ctx.jobs.Store(job, MemoryOrder::Release);
    }

    async_ctx.num_uncompleted_jobs.FetchAdd(1, MemoryOrder::AcquireRelease);
    async_ctx.thread_pool.AddJob([&async_ctx, &job = *job, &lib_list]() {
        ZoneNamed(do_job, true);
        ArenaAllocator scratch_arena {PageAllocator::Instance()};
        switch (job.data.tag) {
            case LibrariesAsyncContext::Job::Type::ReadLibrary: {
                auto& j = *job.data.Get<LibrariesAsyncContext::Job::ReadLibrary*>();
                auto& args = j.args;
                ZoneScopedN("read library");
                String path =
                    args.path_or_memory.Is<String>() ? args.path_or_memory.Get<String>() : ":memory:";
                ZoneText(path.data, path.size);
                auto const try_job = [&]() -> Optional<sample_lib::LibraryPtrOrError> {
                    using H = sample_lib::TryHelpersOutcomeToError;
                    if (args.format == sample_lib::FileFormat::Lua && args.path_or_memory.Is<String>()) {
                        // it will be more efficient to just load the whole lua into memory
                        args.path_or_memory =
                            TRY_H(ReadEntireFile(args.path_or_memory.Get<String>(), scratch_arena))
                                .ToConstByteSpan();
                    }

                    auto reader = TRY_H(Reader::FromPathOrMemory(args.path_or_memory));
                    auto const file_hash = TRY_H(sample_lib::Hash(reader, args.format));

                    for (auto& node : args.libraries) {
                        if (auto l = node.TryScoped()) {
                            if (l->lib->file_hash == file_hash) return nullopt;
                        }
                    }

                    auto lib =
                        TRY(sample_lib::Read(reader, args.format, path, j.result.arena, scratch_arena));
                    lib->file_hash = file_hash;
                    return lib;
                };
                j.result.result = try_job();
                break;
            }
            case LibrariesAsyncContext::Job::Type::ScanFolder: {
                ZoneScopedN("scan folder");
                auto& j = *job.data.Get<LibrariesAsyncContext::Job::ScanFolder*>();
                if (auto folder = j.args.folder->TryScoped()) {
                    auto const& path = folder->path;
                    ZoneText(path.data, path.size);

                    auto const try_job = [&]() -> ErrorCodeOr<void> {
                        auto it = TRY(DirectoryIterator::Create(scratch_arena, path, "*"));
                        while (it.HasMoreFiles()) {
                            auto const& entry = it.Get();
                            auto const ext = path::Extension(entry.path);
                            if (ext == ".mdata") {
                                ReadLibraryAsync(async_ctx,
                                                 lib_list,
                                                 String(entry.path),
                                                 sample_lib::FileFormat::Mdata);
                            } else if (entry.type == FileType::Directory) {
                                String const lua_path =
                                    path::Join(scratch_arena, Array {String(entry.path), "config.lua"});
                                if (auto const ft_outcome = GetFileType(lua_path);
                                    ft_outcome.HasValue() && ft_outcome.Value() == FileType::File) {
                                    ReadLibraryAsync(async_ctx,
                                                     lib_list,
                                                     lua_path,
                                                     sample_lib::FileFormat::Lua);
                                }
                            }
                            TRY(it.Increment());
                        }
                        return k_success;
                    };

                    j.result.outcome = try_job();
                } else {
                    j.result.outcome = k_success;
                }
                break;
            }
        }

        job.completed.Store(true, MemoryOrder::SequentiallyConsistent);
        async_ctx.work_signaller.Signal();
    });
}

// any thread
static void ReadLibraryAsync(LibrariesAsyncContext& async_ctx,
                             LibrariesList& lib_list,
                             PathOrMemory path_or_memory,
                             sample_lib::FileFormat format) {
    LibrariesAsyncContext::Job::ReadLibrary* read_job;
    {
        async_ctx.job_mutex.Lock();
        DEFER { async_ctx.job_mutex.Unlock(); };
        read_job = async_ctx.job_arena.NewUninitialised<LibrariesAsyncContext::Job::ReadLibrary>();
        PLACEMENT_NEW(read_job)
        LibrariesAsyncContext::Job::ReadLibrary {
            .args =
                {
                    .path_or_memory =
                        path_or_memory.Is<String>()
                            ? PathOrMemory(String(async_ctx.job_arena.Clone(path_or_memory.Get<String>())))
                            : path_or_memory,
                    .format = format,
                    .libraries = lib_list,
                },
            .result = {},
        };
    }

    AddAsyncJob(async_ctx, lib_list, read_job);
}

static void
RereadLibraryAsync(LibrariesAsyncContext& async_ctx, LibrariesList& lib_list, LibrariesList::Node* lib_node) {
    ReadLibraryAsync(async_ctx,
                     lib_list,
                     lib_node->value.lib->path,
                     lib_node->value.lib->file_format_specifics.tag);
}

// server-thread
static void UpdateLoadingThread(Server& server,
                                LibrariesAsyncContext& async_ctx,
                                ArenaAllocator& scratch_arena,
                                Optional<DirectoryWatcher>& watcher) {
    ZoneNamed(outer, true);

    // trigger folder scanning if any are marked as 'rescan-requested'
    for (auto& node : server.scan_folders) {
        if (auto f = node.TryScoped()) {
            auto expected = ScanFolder::State::RescanRequested;
            auto const exchanged = f->state.CompareExchangeStrong(expected, ScanFolder::State::Scanning);
            if (!exchanged) continue;
        }

        LibrariesAsyncContext::Job::ScanFolder* scan_job;
        {
            async_ctx.job_mutex.Lock();
            DEFER { async_ctx.job_mutex.Unlock(); };
            scan_job = async_ctx.job_arena.NewUninitialised<LibrariesAsyncContext::Job::ScanFolder>();
            PLACEMENT_NEW(scan_job)
            LibrariesAsyncContext::Job::ScanFolder {
                .args =
                    {
                        .folder = &node,
                    },
                .result = {},
            };
        }

        AddAsyncJob(async_ctx, server.libraries, scan_job);
    }

    // handle async jobs that have completed
    for (auto node = async_ctx.jobs.Load(MemoryOrder::Acquire); node != nullptr;
         node = node->next.Load(MemoryOrder::Relaxed)) {
        if (node->handled) continue;
        if (!node->completed.Load(MemoryOrder::Acquire)) continue;

        DEFER {
            node->handled = true;
            async_ctx.num_uncompleted_jobs.FetchSub(1, MemoryOrder::AcquireRelease);
        };
        auto const& job = *node;
        switch (job.data.tag) {
            case LibrariesAsyncContext::Job::Type::ReadLibrary: {
                auto& j = *job.data.Get<LibrariesAsyncContext::Job::ReadLibrary*>();
                auto& args = j.args;
                String const path =
                    args.path_or_memory.Is<String>() ? args.path_or_memory.Get<String>() : ":memory:";
                ZoneScopedN("job completed: library read");
                ZoneText(path.data, path.size);
                if (!j.result.result) {
                    TracyMessageEx({k_trace_category, k_trace_colour, {}},
                                   "skipping {}, it already exists",
                                   path::Filename(path));
                    return;
                }
                auto const& outcome = *j.result.result;

                auto const error_id = ThreadsafeErrorNotifications::Id("libs", path);
                switch (outcome.tag) {
                    case ResultType::Value: {
                        auto lib = outcome.GetFromTag<ResultType::Value>();
                        TracyMessageEx({k_trace_category, k_trace_colour, {}},
                                       "adding new library {}",
                                       path::Filename(path));

                        // only allow one with the same name or path, and only if it isn't already present
                        bool already_exists = false;
                        for (auto it = server.libraries.begin(); it != server.libraries.end();) {
                            if (it->value.lib->file_hash == lib->file_hash) already_exists = true;
                            if (it->value.lib->name == lib->name ||
                                path::Equal(it->value.lib->path, lib->path))
                                it = server.libraries.Remove(it);
                            else
                                ++it;
                        }
                        if (already_exists) break;

                        auto new_node = server.libraries.AllocateUninitialised();
                        PLACEMENT_NEW(&new_node->value)
                        ListedLibrary {.arena = Move(j.result.arena), .lib = lib};
                        server.libraries.Insert(new_node);

                        server.error_notifications.RemoveError(error_id);
                        break;
                    }
                    case ResultType::Error: {
                        auto const error = outcome.GetFromTag<ResultType::Error>();
                        if (error.code == FilesystemError::PathDoesNotExist) return;

                        auto item = server.error_notifications.NewError();
                        item->value = {
                            .title = "Failed to read library"_s,
                            .message = {},
                            .error_code = error.code,
                            .id = error_id,
                        };
                        if (j.args.path_or_memory.Is<String>())
                            fmt::Append(item->value.message, "{}\n", j.args.path_or_memory.Get<String>());
                        if (error.message.size) fmt::Append(item->value.message, "{}\n", error.message);
                        server.error_notifications.AddOrUpdateError(item);
                        break;
                    }
                }

                break;
            }
            case LibrariesAsyncContext::Job::Type::ScanFolder: {
                auto const& j = *job.data.Get<LibrariesAsyncContext::Job::ScanFolder*>();
                if (auto folder = j.args.folder->TryScoped()) {
                    auto const& path = folder->path;
                    ZoneScopedN("job completed: folder scanned");
                    ZoneText(path.data, path.size);

                    auto const folder_error_id = ThreadsafeErrorNotifications::Id("libs", path);

                    if (j.result.outcome.HasError()) {
                        auto const is_always_scanned_folder =
                            folder->source == ScanFolder::Source::AlwaysScannedFolder;
                        if (!(is_always_scanned_folder &&
                              j.result.outcome.Error() == FilesystemError::PathDoesNotExist)) {
                            auto item = server.error_notifications.NewError();
                            item->value = {
                                .title = "Failed to scan library folder"_s,
                                .message = String(path),
                                .error_code = j.result.outcome.Error(),
                                .id = folder_error_id,
                            };
                            server.error_notifications.AddOrUpdateError(item);
                        }
                        folder->state.Store(ScanFolder::State::ScanFailed, MemoryOrder::Release);
                    } else {
                        server.error_notifications.RemoveError(folder_error_id);
                        folder->state.Store(ScanFolder::State::ScannedSuccessfully, MemoryOrder::Release);
                    }
                }
                break;
            }
        }
    }

    // check if the scan-folders have changed
    if (watcher) {
        ZoneNamedN(fs_watch, "fs watch", true);

        auto const dirs_to_watch = ({
            DynamicArray<DirectoryToWatch> dirs {scratch_arena};
            for (auto& node : server.scan_folders) {
                if (auto f = node.TryRetain()) {
                    if (f->state.Load(MemoryOrder::Relaxed) == ScanFolder::State::ScannedSuccessfully)
                        dyn::Append(dirs,
                                    {
                                        .path = f->path,
                                        .recursive = true,
                                        .user_data = &node,
                                    });
                    else
                        node.Release();
                }
            }
            dirs.ToOwnedSpan();
        });
        DEFER {
            for (auto& d : dirs_to_watch)
                ((ScanFolderList::Node*)d.user_data)->Release();
        };

        if (auto const outcome = PollDirectoryChanges(*watcher,
                                                      {
                                                          .dirs_to_watch = dirs_to_watch,
                                                          .retry_failed_directories = false,
                                                          .result_arena = scratch_arena,
                                                          .scratch_arena = scratch_arena,
                                                      });
            outcome.HasError()) {
            // IMPROVE: handle error
            DebugLn("Reading directory changes failed: {}", outcome.Error());
        } else {
            auto const dir_changes_span = outcome.Value();
            for (auto const& dir_changes : dir_changes_span) {
                auto& scan_folder =
                    ((ScanFolderList::Node*)dir_changes.linked_dir_to_watch->user_data)->value;

                if (dir_changes.error) {
                    // IMPROVE: handle this
                    DebugLn("Reading directory changes failed for {}: {}",
                            scan_folder.path,
                            dir_changes.error);
                    continue;
                }

                for (auto const& subpath_changeset : dir_changes.subpath_changesets) {
                    if (subpath_changeset.changes & DirectoryWatcher::ChangeType::ManualRescanNeeded) {
                        scan_folder.state.Store(ScanFolder::State::RescanRequested);
                        continue;
                    }

                    // changes to the watched directory itself
                    if (subpath_changeset.subpath.size == 0) continue;

                    DebugLn("Scan-folder change: {} {} in {}",
                            subpath_changeset.subpath,
                            DirectoryWatcher::ChangeType::ToString(subpath_changeset.changes),
                            scan_folder.path);

                    auto const full_path =
                        path::Join(scratch_arena,
                                   Array {(String)scan_folder.path, subpath_changeset.subpath});

                    if (path::Depth(subpath_changeset.subpath) == 0) {
                        bool modified_existing_lib = false;
                        if (subpath_changeset.changes & DirectoryWatcher::ChangeType::Modified)
                            for (auto& lib_node : server.libraries) {
                                auto const& lib = *lib_node.value.lib;
                                if (path::Equal(lib.path, full_path)) {
                                    DebugLn("  Rereading library: {}", lib.name);
                                    RereadLibraryAsync(async_ctx, server.libraries, &lib_node);
                                    modified_existing_lib = true;
                                    break;
                                }
                            }
                        if (!modified_existing_lib) {
                            DebugLn("  Rescanning folder: {}", scan_folder.path);
                            scan_folder.state.Store(ScanFolder::State::RescanRequested);
                        }
                    } else {
                        for (auto& lib_node : server.libraries) {
                            auto const& lib = *lib_node.value.lib;
                            if (lib.file_format_specifics.tag == sample_lib::FileFormat::Lua) {
                                // get the directory of the library (the directory of the config.lua)
                                auto const dir = path::Directory(lib.path);
                                if (dir && path::IsWithinDirectory(full_path, *dir)) {
                                    DebugLn("  Rereading library: {}", lib.name);
                                    RereadLibraryAsync(async_ctx, server.libraries, &lib_node);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // TODO(1.0): if a library/instrument has changed trigger a reload for all clients of this loader so it
    // feels totally seamless

    // remove libraries that are not in any active scan-folders
    for (auto it = server.libraries.begin(); it != server.libraries.end();) {
        auto const& lib = *it->value.lib;

        bool within_any_folder = false;
        if (lib.name == k_builtin_library_name)
            within_any_folder = true;
        else
            for (auto& sn : server.scan_folders) {
                if (auto folder = sn.TryScoped()) {
                    if (path::IsWithinDirectory(lib.path, folder->path)) {
                        within_any_folder = true;
                        break;
                    }
                }
            }

        if (!within_any_folder)
            it = server.libraries.Remove(it);
        else
            ++it;
    }

    // update libraries_by_name
    {
        ZoneNamedN(rebuild_htab, "rehash", true);
        server.libraries_by_name_mutex.Lock();
        DEFER { server.libraries_by_name_mutex.Unlock(); };
        auto& libs_by_name = server.libraries_by_name;
        libs_by_name.DeleteAll();
        for (auto& n : server.libraries) {
            auto const& lib = *n.value.lib;
            auto const inserted = libs_by_name.Insert(lib.name, &n);
            ASSERT(inserted);
        }
    }

    // remove scan-folders that are no longer used
    {
        server.scan_folders_writer_mutex.Lock();
        DEFER { server.scan_folders_writer_mutex.Unlock(); };
        server.scan_folders.DeleteRemovedAndUnreferenced();
    }
}

static void RemoveUnreferencedObjects(Server& server, ArenaList<ListedAudioData, true>& audio_datas) {
    ZoneScoped;
    server.channels.Use([](auto& channels) {
        channels.RemoveIf([](AsyncCommsChannel const& h) { return !h.used.Load(MemoryOrder::Relaxed); });
    });

    for (auto& l : server.libraries)
        l.value.instruments.RemoveIf([](ListedInstrument const& i) { return i.refs.Load() == 0; });
    for (auto n = server.libraries.dead_list; n != nullptr; n = n->writer_next)
        n->value.instruments.RemoveIf([](ListedInstrument const& i) { return i.refs.Load() == 0; });

    audio_datas.RemoveIf([](ListedAudioData const& a) { return a.refs.Load() == 0; });

    server.libraries.DeleteRemovedAndUnreferenced();
}

static void CancelLoadingAudioForInstrumentIfPossible(ListedInstrument* i, uintptr_t trace_id) {
    ZoneScoped;
    TracyMessageEx({k_trace_category, k_trace_colour, trace_id},
                   "cancel instID:{}, num audio: {}",
                   i->debug_id,
                   i->audio_data_set.size);

    usize num_cancelled = 0;
    for (auto audio_data : i->audio_data_set) {
        ASSERT(audio_data->refs.Load() != 0);
        if (audio_data->refs.Load() == 1) {
            auto expected = LoadingState::PendingLoad;
            audio_data->state.CompareExchangeStrong(expected, LoadingState::PendingCancel);

            TracyMessageEx({k_trace_category, k_trace_colour, trace_id},
                           "instID:{} cancelled audio from state: {}",
                           i->debug_id,
                           EnumToString(expected));

            ++num_cancelled;
        }
    }

    TracyMessageEx({k_trace_category, k_trace_colour, trace_id},
                   "instID:{} num audio cancelled: {}",
                   i->debug_id,
                   num_cancelled);
}

static void LoadingThreadLoop(Server& server) {
    ZoneScoped;
    ArenaAllocator scratch_arena {PageAllocator::Instance(), Kb(128)};
    ArenaList<ListedAudioData, true> audio_datas {PageAllocator::Instance()};
    uintptr_t debug_result_id = 0;

    Optional<DirectoryWatcher> watcher = {};
    {
        auto watcher_outcome = CreateDirectoryWatcher(PageAllocator::Instance());
        auto const error_id = U64FromChars("libwatch");
        if (!watcher_outcome.HasError()) {
            server.error_notifications.RemoveError(error_id);
            watcher.Emplace(watcher_outcome.ReleaseValue());
        } else {
            DebugLn("Failed to create directory watcher: {}", watcher_outcome.Error());
            auto node = server.error_notifications.NewError();
            node->value = {
                .title = "Warning: unable to monitor library folders"_s,
                .message = {},
                .error_code = watcher_outcome.Error(),
                .id = error_id,
            };
            server.error_notifications.AddOrUpdateError(node);
        }
    }
    DEFER {
        if (watcher) DestoryDirectoryWatcher(*watcher);
    };

    {
        for (auto& n : server.scan_folders)
            if (auto f = n.TryScoped()) f->state.Store(ScanFolder::State::NotScanned);

        {
            auto node = server.libraries.AllocateUninitialised();
            PLACEMENT_NEW(&node->value)
            ListedLibrary {.arena = PageAllocator::Instance(), .lib = BuiltinLibrary()};
            server.libraries.Insert(node);

            server.libraries_by_name.Insert(BuiltinLibrary()->name, node);
        }
    }

    while (!server.end_thread.Load()) {
        struct PendingResult {
            enum class State {
                AwaitingLibrary,
                AwaitingAudio,
                Cancelled,
                Failed,
                CompletedSuccessfully,
            };

            using LoadingAsset = TaggedUnion<LoadRequestType,
                                             TypeAndTag<ListedInstrument*, LoadRequestType::Instrument>,
                                             TypeAndTag<ListedAudioData*, LoadRequestType::Ir>>;

            using StateUnion = TaggedUnion<State,
                                           TypeAndTag<LoadingAsset, State::AwaitingAudio>,
                                           TypeAndTag<ErrorCode, State::Failed>,
                                           TypeAndTag<RefUnion, State::CompletedSuccessfully>>;

            u32 LayerIndex() const {
                if (auto i = request.request.TryGet<LoadRequestInstrumentIdWithLayer>())
                    return i->layer_index;
                PanicIfReached();
                return 0;
            }
            bool IsDesired() const {
                return state.Get<LoadingAsset>().Get<ListedInstrument*>() ==
                       request.async_comms_channel.desired_inst[LayerIndex()];
            }
            auto& LoadingPercent() {
                return request.async_comms_channel.instrument_loading_percents[LayerIndex()];
            }

            StateUnion state {State::AwaitingLibrary};
            QueuedRequest request;
            uintptr_t debug_id;

            PendingResult* next = nullptr;
        };

        LibrariesAsyncContext libs_async_ctx {
            .thread_pool = server.thread_pool,
            .work_signaller = server.work_signaller,
        };

        IntrusiveSinglyLinkedList<PendingResult> pending_results {};
        AtomicCountdown thread_pool_jobs {0};

        ThreadPoolContext thread_pool_ctx {
            .pool = server.thread_pool,
            .num_thread_pool_jobs = thread_pool_jobs,
            .completed_signaller = server.work_signaller,
        };

        do {
            server.work_signaller.WaitUntilSignalledOrSpurious(250u);

            if (server.request_debug_dump_current_state.Exchange(false)) {
                ZoneNamedN(dump, "dump", true);
                DebugLn("Dumping current state of loading thread");
                DebugLn("Libraries currently loading: {}", libs_async_ctx.num_uncompleted_jobs.Load());
                DebugLn("Thread pool jobs: {}", thread_pool_jobs.counter.Load());
                DebugLn("\nPending results:");
                for (auto& pending_result : pending_results) {
                    DebugLn("  Pending result: {}", pending_result.debug_id);
                    switch (pending_result.state.tag) {
                        case PendingResult::State::AwaitingLibrary: DebugLn("    Awaiting library"); break;
                        case PendingResult::State::AwaitingAudio: {
                            auto& asset = pending_result.state.Get<PendingResult::LoadingAsset>();
                            switch (asset.tag) {
                                case LoadRequestType::Instrument: {
                                    auto inst = asset.Get<ListedInstrument*>();
                                    DebugLn("    Awaiting audio for instrument {}",
                                            inst->inst.instrument.name);
                                    for (auto& audio_data : inst->audio_data_set) {
                                        DebugLn("      Audio data: {}, {}",
                                                audio_data->audio_data.hash,
                                                EnumToString(audio_data->state.Load()));
                                    }
                                    break;
                                }
                                case LoadRequestType::Ir: {
                                    auto ir = asset.Get<ListedAudioData*>();
                                    DebugLn("    Awaiting audio for IR {}", ir->path);
                                    DebugLn("      Audio data: {}, {}",
                                            ir->audio_data.hash,
                                            EnumToString(ir->state.Load()));
                                    break;
                                }
                            }
                            break;
                        }
                        case PendingResult::State::Cancelled: DebugLn("    Cancelled"); break;
                        case PendingResult::State::Failed: DebugLn("    Failed"); break;
                        case PendingResult::State::CompletedSuccessfully:
                            DebugLn("    Completed successfully");
                            break;
                    }
                }
                DebugLn("\nAvailable Libraries:");
                for (auto& lib : server.libraries) {
                    DebugLn("  Library: {}", lib.value.lib->name);
                    for (auto& inst : lib.value.instruments)
                        DebugLn("    Instrument: {}", inst.inst.instrument.name);
                }
            }

            ZoneNamedN(working, "working", true);

            TracyMessageEx({k_trace_category, k_trace_colour, {}},
                           "poll, thread_pool_jobs: {}",
                           thread_pool_jobs.counter.Load());

            // consume any incoming requests
            while (auto queued_request = server.request_queue.TryPop()) {
                ZoneNamedN(req, "request", true);

                if (!queued_request->async_comms_channel.used.Load(MemoryOrder::Relaxed)) continue;

                // Only once we have a request do we initiate the scanning
                for (auto& n : server.scan_folders) {
                    if (auto f = n.TryScoped()) {
                        auto expected = ScanFolder::State::NotScanned;
                        f->state.CompareExchangeStrong(expected, ScanFolder::State::RescanRequested);
                    }
                }

                auto pending_result = scratch_arena.NewUninitialised<PendingResult>();
                PLACEMENT_NEW(pending_result)
                PendingResult {
                    .state = PendingResult::State::AwaitingLibrary,
                    .request = *queued_request,
                    .debug_id = debug_result_id++,
                };
                SinglyLinkedListPrepend(pending_results.first, pending_result);

                TracyMessageEx({k_trace_category, k_trace_colour, pending_result->debug_id},
                               "pending result added");
            }

            UpdateLoadingThread(server, libs_async_ctx, scratch_arena, watcher);

            if (!pending_results.Empty()) {
                // Fill in library
                for (auto& pending_result : pending_results) {
                    if (pending_result.state != PendingResult::State::AwaitingLibrary) continue;

                    auto const library_name = ({
                        String n {};
                        switch (pending_result.request.request.tag) {
                            case LoadRequestType::Instrument:
                                n = pending_result.request.request.Get<LoadRequestInstrumentIdWithLayer>()
                                        .id.library_name;
                                break;
                            case LoadRequestType::Ir:
                                n = pending_result.request.request.Get<sample_lib::IrId>().library_name;
                                break;
                        }
                        n;
                    });
                    ASSERT(library_name.size != 0);

                    LibrariesList::Node* lib {};
                    if (auto l_ptr = server.libraries_by_name.Find(library_name)) lib = *l_ptr;

                    if (!lib) {
                        if (libs_async_ctx.num_uncompleted_jobs.Load(MemoryOrder::AcquireRelease) == 0) {
                            {
                                auto item =
                                    pending_result.request.async_comms_channel.error_notifications.NewError();
                                item->value = {
                                    .title = {},
                                    .message = {},
                                    .error_code = CommonError::NotFound,
                                    .id = ThreadsafeErrorNotifications::Id("lib ", library_name),
                                };
                                fmt::Append(item->value.title, "{} not found", library_name);
                                pending_result.request.async_comms_channel.error_notifications
                                    .AddOrUpdateError(item);
                            }
                            pending_result.state = ErrorCode {CommonError::NotFound};
                        }
                    } else {
                        switch (pending_result.request.request.tag) {
                            case LoadRequestType::Instrument: {
                                auto& load_inst =
                                    pending_result.request.request.Get<LoadRequestInstrumentIdWithLayer>();
                                auto const inst_name = load_inst.id.inst_name;

                                ASSERT(inst_name.size != 0);

                                if (auto const i = lib->value.lib->insts_by_name.Find(inst_name)) {
                                    pending_result.request.async_comms_channel
                                        .instrument_loading_percents[load_inst.layer_index]
                                        .Store(0);

                                    auto inst =
                                        FetchOrCreateInstrument(*lib, audio_datas, **i, thread_pool_ctx);
                                    ASSERT(inst);

                                    pending_result.request.async_comms_channel
                                        .desired_inst[load_inst.layer_index] = inst;
                                    pending_result.state = PendingResult::LoadingAsset {inst};

                                    TracyMessageEx(
                                        {k_trace_category, k_trace_colour, pending_result.debug_id},
                                        "option: instID:{} load Sampler inst[{}], {}, {}, {}",
                                        inst->debug_id,
                                        load_inst.layer_index,
                                        (void const*)inst,
                                        lib->value.lib->name,
                                        inst_name);
                                } else {
                                    {
                                        auto const item = pending_result.request.async_comms_channel
                                                              .error_notifications.NewError();
                                        item->value = {
                                            .title = {},
                                            .message = {},
                                            .error_code = CommonError::NotFound,
                                            .id = ThreadsafeErrorNotifications::Id("inst", inst_name),
                                        };
                                        fmt::Append(item->value.title,
                                                    "Cannot find instrument \"{}\"",
                                                    inst_name);
                                        pending_result.request.async_comms_channel.error_notifications
                                            .AddOrUpdateError(item);
                                    }
                                    pending_result.state = ErrorCode {CommonError::NotFound};
                                }
                                break;
                            }
                            case LoadRequestType::Ir: {
                                auto const ir = pending_result.request.request.Get<sample_lib::IrId>();

                                auto const ir_path = lib->value.lib->irs_by_name.Find(ir.ir_name);

                                if (ir_path) {
                                    auto audio_data = FetchOrCreateAudioData(audio_datas,
                                                                             *lib->value.lib,
                                                                             (*ir_path)->path,
                                                                             thread_pool_ctx,
                                                                             999999);

                                    pending_result.state = PendingResult::LoadingAsset {audio_data};

                                    TracyMessageEx(
                                        {k_trace_category, k_trace_colour, pending_result.debug_id},
                                        "option: load IR, {}, {}",
                                        ir.library_name,
                                        ir.ir_name);
                                } else {
                                    auto const err = pending_result.request.async_comms_channel
                                                         .error_notifications.NewError();
                                    err->value = {
                                        .title = "Failed to find IR"_s,
                                        .message = String(ir.ir_name),
                                        .error_code = CommonError::NotFound,
                                        .id = ThreadsafeErrorNotifications::Id("ir  ", ir.ir_name),
                                    };
                                    pending_result.request.async_comms_channel.error_notifications
                                        .AddOrUpdateError(err);
                                    pending_result.state = ErrorCode {CommonError::NotFound};
                                }
                                break;
                            }
                        }
                    }
                }

                // For each inst, check for errors
                for (auto& pending_result : pending_results) {
                    if (pending_result.state.tag != PendingResult::State::AwaitingAudio) continue;
                    auto i_ptr =
                        pending_result.state.Get<PendingResult::LoadingAsset>().TryGet<ListedInstrument*>();
                    if (!i_ptr) continue;
                    auto i = *i_ptr;

                    ASSERT(i->audio_data_set.size);

                    Optional<ErrorCode> error {};
                    for (auto a : i->audio_data_set) {
                        if (a->state.Load() == LoadingState::CompletedWithError) {
                            error = a->error;
                            break;
                        }
                    }

                    if (error) {
                        {
                            auto item =
                                pending_result.request.async_comms_channel.error_notifications.NewError();
                            item->value = {
                                .title = "Failed to load audio"_s,
                                .message = i->inst.instrument.name,
                                .error_code = *error,
                                .id = ThreadsafeErrorNotifications::Id("audi", i->inst.instrument.name),
                            };
                            pending_result.request.async_comms_channel.error_notifications.AddOrUpdateError(
                                item);
                        }

                        CancelLoadingAudioForInstrumentIfPossible(i, pending_result.debug_id);
                        if (pending_result.IsDesired()) pending_result.LoadingPercent().Store(-1);
                        pending_result.state = *error;
                    }
                }

                // For each inst, check if it's still needed, and cancel if not. And update percent
                // markers
                for (auto& pending_result : pending_results) {
                    if (pending_result.state.tag != PendingResult::State::AwaitingAudio) continue;
                    auto i_ptr =
                        pending_result.state.Get<PendingResult::LoadingAsset>().TryGet<ListedInstrument*>();
                    if (!i_ptr) continue;
                    auto i = *i_ptr;

                    if (pending_result.IsDesired()) {
                        auto const num_completed = ({
                            u32 n = 0;
                            for (auto& a : i->audio_data_set) {
                                auto const state = a->state.Load();
                                if (state == LoadingState::CompletedSucessfully) ++n;
                            }
                            n;
                        });
                        if (num_completed == i->audio_data_set.size) {
                            pending_result.LoadingPercent().Store(-1);
                            pending_result.state = RefUnion {
                                RefCounted<LoadedInstrument> {i->inst, i->refs, &server.work_signaller}};
                        } else {
                            f32 const percent = 100.0f * ((f32)num_completed / (f32)i->audio_data_set.size);
                            pending_result.LoadingPercent().Store(RoundPositiveFloat(percent));
                        }
                    } else {
                        // If it's not desired by any others it can be cancelled
                        bool const is_desired_by_another = ({
                            bool desired = false;
                            for (auto& other_pending_result : pending_results) {
                                for (auto other_desired :
                                     other_pending_result.request.async_comms_channel.desired_inst) {
                                    if (other_desired == i) {
                                        desired = true;
                                        break;
                                    }
                                }
                                if (desired) break;
                            }
                            desired;
                        });
                        if (!is_desired_by_another)
                            CancelLoadingAudioForInstrumentIfPossible(i, pending_result.debug_id);

                        pending_result.state = PendingResult::State::Cancelled;
                    }
                }

                // Store the result of the IR load in the result, if needed
                for (auto& pending_result : pending_results) {
                    if (pending_result.state.tag != PendingResult::State::AwaitingAudio) continue;
                    auto a_ptr =
                        pending_result.state.Get<PendingResult::LoadingAsset>().TryGet<ListedAudioData*>();
                    if (!a_ptr) continue;
                    auto a = *a_ptr;

                    auto const& ir_data = *a;
                    switch (ir_data.state.Load()) {
                        case LoadingState::CompletedSucessfully: {
                            pending_result.state = RefUnion {
                                RefCounted<AudioData> {a->audio_data, a->refs, &server.work_signaller}};
                            break;
                        }
                        case LoadingState::CompletedWithError: {
                            auto const ir_index = pending_result.request.request.Get<sample_lib::IrId>();
                            {
                                auto item =
                                    pending_result.request.async_comms_channel.error_notifications.NewError();
                                item->value = {
                                    .title = "Failed to load IR"_s,
                                    .message = {},
                                    .error_code = *ir_data.error,
                                    .id = Hash("ir  "_s) + Hash(ir_index.library_name.Items()) +
                                          Hash(ir_index.ir_name.Items()),
                                };
                                pending_result.request.async_comms_channel.error_notifications
                                    .AddOrUpdateError(item);
                            }
                            pending_result.state = *ir_data.error;
                            break;
                        }
                        case LoadingState::PendingLoad:
                        case LoadingState::Loading: break;
                        case LoadingState::PendingCancel:
                        case LoadingState::CompletedCancelled: PanicIfReached(); break;
                        case LoadingState::Count: PanicIfReached(); break;
                    }
                }

                // For each inst, check if all loading has completed and if so, dispatch the result
                // and remove it from the pending list
                SinglyLinkedListRemoveIf(
                    pending_results.first,
                    [&](PendingResult const& pending_result) {
                        switch (pending_result.state.tag) {
                            case PendingResult::State::AwaitingLibrary:
                            case PendingResult::State::AwaitingAudio: return false;
                            case PendingResult::State::Cancelled:
                            case PendingResult::State::Failed:
                            case PendingResult::State::CompletedSuccessfully: break;
                        }

                        LoadResult result {
                            .id = pending_result.request.id,
                            .result = ({
                                LoadResult::Result r {LoadResult::ResultType::Cancelled};
                                switch (pending_result.state.tag) {
                                    case PendingResult::State::AwaitingLibrary:
                                    case PendingResult::State::AwaitingAudio: PanicIfReached(); break;
                                    case PendingResult::State::Cancelled: break;
                                    case PendingResult::State::Failed:
                                        r = pending_result.state.Get<ErrorCode>();
                                        break;
                                    case PendingResult::State::CompletedSuccessfully:
                                        r = pending_result.state.Get<RefUnion>();
                                        break;
                                }
                                r;
                            }),
                        };

                        server.channels.Use([&](auto&) {
                            if (pending_result.request.async_comms_channel.used.Load(MemoryOrder::Relaxed)) {
                                result.Retain();
                                pending_result.request.async_comms_channel.results.Push(result);
                                pending_result.request.async_comms_channel.result_added_callback();
                            }
                        });
                        return true;
                    },
                    [](PendingResult*) {
                        // delete function
                    });
            }

            {
                u32 num_insts_loaded = 0;
                u32 num_samples_loaded = 0;
                u64 total_bytes_used = 0;
                for (auto& i : server.libraries) {
                    for (auto& inst : i.value.instruments) {
                        (void)inst;
                        ++num_insts_loaded;
                    }
                }
                for (auto& audio : audio_datas) {
                    ++num_samples_loaded;
                    if (audio.state.Load() == LoadingState::CompletedSucessfully)
                        total_bytes_used += audio.audio_data.RamUsageBytes();
                }
                server.num_insts_loaded.Store(num_insts_loaded);
                server.num_samples_loaded.Store(num_samples_loaded);
                server.total_bytes_used_by_samples.Store(total_bytes_used);
            }
        } while (!pending_results.Empty() ||
                 libs_async_ctx.num_uncompleted_jobs.Load(MemoryOrder::AcquireRelease));

        ZoneNamedN(post_inner, "post inner", true);

        TracyMessageEx({k_trace_category, k_trace_colour, -1u}, "poll completed");

        // We have completed all of the asset loading requests, but there might still be audio data that
        // is in the thread pool. We need for them to finish before we potentially delete the memory that
        // they rely on.
        thread_pool_jobs.WaitUntilZero();

        RemoveUnreferencedObjects(server, audio_datas);
        scratch_arena.ResetCursorAndConsolidateRegions();
    }

    DebugLn("Ending server thread loop");

    // It's necessary to do this at the end of this function because it is not guaranteed to be called in the
    // loop; the 'end' boolean can be changed at a point where the loop ends before calling this.
    RemoveUnreferencedObjects(server, audio_datas);

    server.libraries.RemoveAll();
    server.libraries.DeleteRemovedAndUnreferenced();
    server.libraries_by_name.DeleteAll();
}

Server::Server(ThreadPool& pool,
               Span<String const> always_scanned_folders,
               ThreadsafeErrorNotifications& error_notifications)
    : error_notifications(error_notifications)
    , thread_pool(pool) {
    for (auto e : always_scanned_folders) {
        auto node = scan_folders.AllocateUninitialised();
        PLACEMENT_NEW(&node->value) ScanFolder();
        dyn::Assign(node->value.path, e);
        node->value.source = ScanFolder::Source::AlwaysScannedFolder;
        node->value.state.raw = ScanFolder::State::NotScanned;
        scan_folders.Insert(node);
    }
    loading_thread.Start([this]() { LoadingThreadLoop(*this); }, "Sample lib loading");
}

Server::~Server() {
    end_thread.Store(true);
    work_signaller.Signal();
    loading_thread.Join();
    ASSERT(channels.Use([](auto& h) { return h.Empty(); }), "missing channel close");

    scan_folders.RemoveAll();
    scan_folders.DeleteRemovedAndUnreferenced();
}

AsyncCommsChannel& OpenAsyncCommsChannel(Server& server,
                                         ThreadsafeErrorNotifications& error_notifications,
                                         LoadCompletedCallback&& callback) {
    return server.channels.Use([&](auto& channels) -> AsyncCommsChannel& {
        auto channel = channels.PrependUninitialised();
        PLACEMENT_NEW(channel)
        AsyncCommsChannel {
            .error_notifications = error_notifications,
            .result_added_callback = Move(callback),
            .used = true,
        };
        for (auto& p : channel->instrument_loading_percents)
            p.raw = -1;
        return *channel;
    });
}

void CloseAsyncCommsChannel(Server& server, AsyncCommsChannel& channel) {
    server.channels.Use([&channel](auto& channels) {
        (void)channels;
        channel.used.Store(false, MemoryOrder::Relaxed);
        while (auto r = channel.results.TryPop())
            r->Release();
    });
}

RequestId SendAsyncLoadRequest(Server& server, AsyncCommsChannel& channel, LoadRequest const& request) {
    QueuedRequest const queued_request {
        .id = server.request_id_counter.FetchAdd(1),
        .request = request,
        .async_comms_channel = channel,
    };
    server.request_queue.Push(queued_request);
    server.work_signaller.Signal();
    return queued_request.id;
}

void SetExtraScanFolders(Server& server, Span<String const> extra_folders) {
    server.scan_folders_writer_mutex.Lock();
    DEFER { server.scan_folders_writer_mutex.Unlock(); };

    for (auto it = server.scan_folders.begin(); it != server.scan_folders.end();)
        if (it->value.source == ScanFolder::Source::ExtraFolder && !Find(extra_folders, it->value.path))
            it = server.scan_folders.Remove(it);
        else
            ++it;

    for (auto e : extra_folders) {
        bool already_present = false;
        for (auto& l : server.scan_folders)
            if (l.value.path == e) already_present = true;
        if (already_present) continue;

        auto node = server.scan_folders.AllocateUninitialised();
        PLACEMENT_NEW(&node->value) ScanFolder();
        dyn::Assign(node->value.path, e);
        node->value.source = ScanFolder::Source::ExtraFolder;
        node->value.state.raw = ScanFolder::State::NotScanned;
        server.scan_folders.Insert(node);
    }
}

static bool RequestScanningIfNeeded(ScanFolderList& scan_folders) {
    bool any_rescan_requested = false;
    for (auto& n : scan_folders)
        if (auto f = n.TryScoped()) {
            auto expected = ScanFolder::State::NotScanned;
            if (f->state.CompareExchangeStrong(expected, ScanFolder::State::RescanRequested))
                any_rescan_requested = true;
        }
    return any_rescan_requested;
}

Span<RefCounted<sample_lib::Library>> AllLibrariesRetained(Server& server, ArenaAllocator& arena) {
    if (RequestScanningIfNeeded(server.scan_folders)) server.work_signaller.Signal();

    DynamicArray<RefCounted<sample_lib::Library>> result(arena);
    for (auto& i : server.libraries) {
        if (i.TryRetain()) {
            auto ref = RefCounted<sample_lib::Library>(*i.value.lib, i.reader_uses, nullptr);
            dyn::Append(result, ref);
        }
    }
    return result.ToOwnedSpan();
}

RefCounted<sample_lib::Library> FindLibraryRetained(Server& server, String name) {
    if (RequestScanningIfNeeded(server.scan_folders)) server.work_signaller.Signal();

    server.libraries_by_name_mutex.Lock();
    DEFER { server.libraries_by_name_mutex.Unlock(); };
    auto l = server.libraries_by_name.Find(name);
    if (!l) return {};
    auto& node = **l;
    if (!node.TryRetain()) return {};
    return RefCounted<sample_lib::Library>(*node.value.lib, node.reader_uses, nullptr);
}

void LoadResult::ChangeRefCount(RefCountChange t) const {
    if (auto asset_union = result.TryGet<RefUnion>()) {
        switch (asset_union->tag) {
            case LoadRequestType::Instrument:
                asset_union->Get<RefCounted<LoadedInstrument>>().ChangeRefCount(t);
                break;
            case LoadRequestType::Ir:
                break;
                asset_union->Get<RefCounted<AudioData>>().ChangeRefCount(t);
                break;
        }
    }
}

//=================================================
//  _______        _
// |__   __|      | |
//    | | ___  ___| |_ ___
//    | |/ _ \/ __| __/ __|
//    | |  __/\__ \ |_\__ \
//    |_|\___||___/\__|___/
//
//=================================================

template <typename Type>
static Type& ExtractSuccess(tests::Tester& tester, LoadResult const& result, LoadRequest const& request) {
    switch (request.tag) {
        case LoadRequestType::Instrument: {
            auto inst = request.Get<LoadRequestInstrumentIdWithLayer>();
            tester.log.DebugLn("Instrument: {} - {}", inst.id.library_name, inst.id.inst_name);
            break;
        }
        case LoadRequestType::Ir: {
            auto ir = request.Get<sample_lib::IrId>();
            tester.log.DebugLn("Ir: {} - {}", ir.library_name, ir.ir_name);
            break;
        }
    }

    if (auto err = result.result.TryGet<ErrorCode>()) DebugLn("Error: {}", *err);
    REQUIRE_EQ(result.result.tag, LoadResult::ResultType::Success);
    auto opt_r = result.result.Get<RefUnion>().TryGetMut<Type>();
    REQUIRE(opt_r);
    return *opt_r;
}

TEST_CASE(TestSampleLibraryLoader) {
    struct Fixture {
        Fixture(tests::Tester&) { thread_pool.Init("Thread Pool", 8u); }
        bool initialised = false;
        ArenaAllocatorWithInlineStorage<2000> arena;
        String test_lib_path;
        ThreadPool thread_pool;
        ThreadsafeErrorNotifications error_notif {};
        DynamicArrayInline<String, 2> scan_folders;
    };

    auto& fixture = CreateOrFetchFixtureObject<Fixture>(tester);
    if (!fixture.initialised) {
        fixture.initialised = true;

        auto const lib_dir = (String)path::Join(tester.scratch_arena,
                                                Array {
                                                    tests::TempFolder(tester),
                                                    "floe libraries",
                                                });
        // We copy the test library files to a temp directory so that we can modify them without messing up
        // our test data. And also on Windows WSL, we can watch for directory changes - which doesn't work on
        // the WSL filesystem.
        auto _ =
            Delete(lib_dir, {.type = DeleteOptions::Type::DirectoryRecursively, .fail_if_not_exists = false});
        {
            auto const source = (String)path::Join(
                tester.scratch_arena,
                ConcatArrays(Array {TestFilesFolder(tester)}, k_repo_subdirs_floe_test_libraries));

            auto it = TRY(RecursiveDirectoryIterator::Create(tester.scratch_arena, source));
            while (it.HasMoreFiles()) {
                auto const& entry = it.Get();

                auto const relative_path =
                    path::TrimDirectorySeparatorsEnd(entry.path.Items().SubSpan(source.size));
                auto const dest_file = path::Join(tester.scratch_arena, Array {lib_dir, relative_path});
                if (entry.type == FileType::File) {
                    if (auto const dir = path::Directory(dest_file)) {
                        TRY(CreateDirectory(
                            *dir,
                            {.create_intermediate_directories = true, .fail_if_exists = false}));
                    }
                    TRY(CopyFile(entry.path, dest_file, ExistingDestinationHandling::Overwrite));
                } else {
                    TRY(CreateDirectory(dest_file,
                                        {.create_intermediate_directories = true, .fail_if_exists = false}));
                }

                TRY(it.Increment());
            }
        }

        fixture.test_lib_path = path::Join(fixture.arena, Array {lib_dir, "shared_files_test_lib.mdata"_s});

        DynamicArrayInline<String, 2> scan_folders;
        dyn::Append(scan_folders, fixture.arena.Clone(lib_dir));
        if (auto dir = tests::BuildResourcesFolder(tester))
            dyn::Append(scan_folders, fixture.arena.Clone(*dir));

        fixture.scan_folders = scan_folders;
    }

    auto& scratch_arena = tester.scratch_arena;
    Server server {fixture.thread_pool, {}, fixture.error_notif};
    SetExtraScanFolders(server, fixture.scan_folders);

    SUBCASE("single channel") {
        auto& channel = OpenAsyncCommsChannel(server, fixture.error_notif, []() {});
        CloseAsyncCommsChannel(server, channel);
    }

    SUBCASE("multiple channels") {
        auto& channel1 = OpenAsyncCommsChannel(server, fixture.error_notif, []() {});
        auto& channel2 = OpenAsyncCommsChannel(server, fixture.error_notif, []() {});
        CloseAsyncCommsChannel(server, channel1);
        CloseAsyncCommsChannel(server, channel2);
    }

    SUBCASE("registering again after unregistering all") {
        auto& channel1 = OpenAsyncCommsChannel(server, fixture.error_notif, []() {});
        auto& channel2 = OpenAsyncCommsChannel(server, fixture.error_notif, []() {});
        CloseAsyncCommsChannel(server, channel1);
        CloseAsyncCommsChannel(server, channel2);
        auto& channel3 = OpenAsyncCommsChannel(server, fixture.error_notif, []() {});
        CloseAsyncCommsChannel(server, channel3);
    }

    SUBCASE("unregister a channel directly after sending a request") {
        auto& channel = OpenAsyncCommsChannel(server, fixture.error_notif, [&]() {});

        SendAsyncLoadRequest(server,
                             channel,
                             LoadRequestInstrumentIdWithLayer {
                                 .id =
                                     {
                                         .library_name = "Test Lua"_s,
                                         .inst_name = "Auto Mapped Samples"_s,
                                     },
                                 .layer_index = 0,
                             });
        CloseAsyncCommsChannel(server, channel);
    }

    SUBCASE("loading works") {
        struct Request {
            LoadRequest request;
            TrivialFixedSizeFunction<24, void(LoadResult const&, LoadRequest const& request)> check_result;
            RequestId request_id; // filled in later
        };
        DynamicArray<Request> requests {scratch_arena};

        SUBCASE("ir") {
            auto const builtin_ir = EmbeddedIrs().irs[0];
            dyn::Append(
                requests,
                {
                    .request =
                        sample_lib::IrId {.library_name = k_builtin_library_name,
                                          .ir_name = String {builtin_ir.name.data, builtin_ir.name.size}},
                    .check_result =
                        [&](LoadResult const& r, LoadRequest const& request) {
                            auto audio_data = ExtractSuccess<RefCounted<AudioData>>(tester, r, request);
                            CHECK(audio_data->interleaved_samples.size);
                        },
                });
        }

        SUBCASE("library and instrument") {
            dyn::Append(requests,
                        {
                            .request =
                                LoadRequestInstrumentIdWithLayer {
                                    .id =
                                        {
                                            .library_name = "SharedFilesMdata"_s,
                                            .inst_name = "Groups And Refs"_s,
                                        },
                                    .layer_index = 0,
                                },
                            .check_result =
                                [&](LoadResult const& r, LoadRequest const& request) {
                                    auto inst =
                                        ExtractSuccess<RefCounted<LoadedInstrument>>(tester, r, request);
                                    CHECK(inst->audio_datas.size);
                                },
                        });
        }

        SUBCASE("library and instrument (lua)") {
            dyn::Append(requests,
                        {
                            .request =
                                LoadRequestInstrumentIdWithLayer {
                                    .id =
                                        {
                                            .library_name = "Test Lua"_s,
                                            .inst_name = "Single Sample"_s,
                                        },
                                    .layer_index = 0,
                                },
                            .check_result =
                                [&](LoadResult const& r, LoadRequest const& request) {
                                    auto inst =
                                        ExtractSuccess<RefCounted<LoadedInstrument>>(tester, r, request);
                                    CHECK(inst->audio_datas.size);
                                },
                        });
        }

        SUBCASE("audio file shared across insts") {
            dyn::Append(requests,
                        {
                            .request =
                                LoadRequestInstrumentIdWithLayer {
                                    .id =
                                        {
                                            .library_name = "SharedFilesMdata"_s,
                                            .inst_name = "Groups And Refs"_s,
                                        },
                                    .layer_index = 0,
                                },
                            .check_result =
                                [&](LoadResult const& r, LoadRequest const& request) {
                                    auto i = ExtractSuccess<RefCounted<LoadedInstrument>>(tester, r, request);
                                    CHECK_EQ(i->instrument.name, "Groups And Refs"_s);
                                    CHECK_EQ(i->audio_datas.size, 4u);
                                    for (auto& d : i->audio_datas)
                                        CHECK_NEQ(d->interleaved_samples.size, 0u);
                                },
                        });
            dyn::Append(requests,
                        {
                            .request =
                                LoadRequestInstrumentIdWithLayer {
                                    .id =
                                        {
                                            .library_name = "SharedFilesMdata"_s,
                                            .inst_name = "Groups And Refs (copy)"_s,
                                        },
                                    .layer_index = 1,
                                },
                            .check_result =
                                [&](LoadResult const& r, LoadRequest const& request) {
                                    auto i = ExtractSuccess<RefCounted<LoadedInstrument>>(tester, r, request);
                                    CHECK_EQ(i->instrument.name, "Groups And Refs (copy)"_s);
                                    CHECK_EQ(i->audio_datas.size, 4u);
                                    for (auto& d : i->audio_datas)
                                        CHECK_NEQ(d->interleaved_samples.size, 0u);
                                },
                        });
            dyn::Append(requests,
                        {
                            .request =
                                LoadRequestInstrumentIdWithLayer {
                                    .id =
                                        {
                                            .library_name = "SharedFilesMdata"_s,
                                            .inst_name = "Single Sample"_s,
                                        },
                                    .layer_index = 2,
                                },
                            .check_result =
                                [&](LoadResult const& r, LoadRequest const& request) {
                                    auto i = ExtractSuccess<RefCounted<LoadedInstrument>>(tester, r, request);
                                    CHECK_EQ(i->instrument.name, "Single Sample"_s);
                                    CHECK_EQ(i->audio_datas.size, 1u);
                                    for (auto& d : i->audio_datas)
                                        CHECK_NEQ(d->interleaved_samples.size, 0u);
                                },
                        });
        }

        SUBCASE("audio files shared within inst") {
            dyn::Append(requests,
                        {
                            .request =
                                LoadRequestInstrumentIdWithLayer {
                                    .id =
                                        {
                                            .library_name = "SharedFilesMdata"_s,
                                            .inst_name = "Same Sample Twice"_s,
                                        },
                                    .layer_index = 0,
                                },
                            .check_result =
                                [&](LoadResult const& r, LoadRequest const& request) {
                                    auto i = ExtractSuccess<RefCounted<LoadedInstrument>>(tester, r, request);
                                    CHECK_EQ(i->instrument.name, "Same Sample Twice"_s);
                                    CHECK_EQ(i->audio_datas.size, 2u);
                                    for (auto& d : i->audio_datas)
                                        CHECK_NEQ(d->interleaved_samples.size, 0u);
                                },
                        });
        };

        SUBCASE("core library") {
            dyn::Append(
                requests,
                {
                    .request =
                        LoadRequestInstrumentIdWithLayer {
                            .id =
                                {
                                    .library_name = "Core"_s,
                                    .inst_name = "bar"_s,
                                },
                            .layer_index = 0,
                        },
                    .check_result =
                        [&](LoadResult const& r, LoadRequest const&) {
                            auto err = r.result.TryGet<ErrorCode>();
                            REQUIRE(err);
                            if (*err != CommonError::NotFound)
                                LOG_WARNING(
                                    "Unable to properly test Core library, not expecting error: {}. The test program scans upwards from its executable path for a folder named '{}' and scans that for the core library",
                                    tests::k_build_resources_subdir,
                                    *err);
                            for (auto& n : fixture.error_notif.items)
                                if (auto e = n.TryScoped())
                                    tester.log.DebugLn("Error: {}: {}: {}",
                                                       e->title,
                                                       e->message,
                                                       e->error_code);
                        },
                });
        }

        SUBCASE("invalid lib+path") {
            dyn::Append(requests,
                        {
                            .request =
                                LoadRequestInstrumentIdWithLayer {
                                    .id =
                                        {
                                            .library_name = "foo"_s,
                                            .inst_name = "bar"_s,
                                        },
                                    .layer_index = 0,
                                },
                            .check_result =
                                [&](LoadResult const& r, LoadRequest const&) {
                                    auto err = r.result.TryGet<ErrorCode>();
                                    REQUIRE(err);
                                    REQUIRE(*err == CommonError::NotFound);
                                },
                        });
        }
        SUBCASE("invalid path only") {
            dyn::Append(requests,
                        {
                            .request =
                                LoadRequestInstrumentIdWithLayer {
                                    .id =
                                        {
                                            .library_name = "SharedFilesMdata"_s,
                                            .inst_name = "bar"_s,
                                        },
                                    .layer_index = 0,
                                },
                            .check_result =
                                [&](LoadResult const& r, LoadRequest const&) {
                                    auto err = r.result.TryGet<ErrorCode>();
                                    REQUIRE(err);
                                    REQUIRE(*err == CommonError::NotFound);
                                },
                        });
        }

        AtomicCountdown countdown {(u32)requests.size};
        auto& channel = OpenAsyncCommsChannel(server, fixture.error_notif, [&]() { countdown.CountDown(); });
        DEFER { CloseAsyncCommsChannel(server, channel); };

        if (requests.size) {
            for (auto& j : requests)
                j.request_id = SendAsyncLoadRequest(server, channel, j.request);

            u32 const timeout_secs = 15;
            auto const countdown_result = countdown.WaitUntilZero(timeout_secs * 1000);

            if (countdown_result == WaitResult::TimedOut) {
                tester.log.ErrorLn("Timed out waiting for asset loading to complete");
                DumpCurrentStackTraceToStderr();
                server.request_debug_dump_current_state.Store(true);
                server.work_signaller.Signal();
                SleepThisThread(1000);
                // We need to hard-exit without cleaning up because the loading thread is probably deadlocked
                __builtin_abort();
            }

            usize num_results = 0;
            while (auto r = channel.results.TryPop()) {
                DEFER { r->Release(); };
                for (auto const& request : requests) {
                    if (r->id == request.request_id) {
                        for (auto& n : fixture.error_notif.items) {
                            if (auto e = n.TryScoped()) {
                                tester.log.DebugLn("Error Notification  {}: {}: {}",
                                                   e->title,
                                                   e->message,
                                                   e->error_code);
                            }
                        }
                        request.check_result(*r, request.request);
                    }
                }
                ++num_results;
            }
            REQUIRE_EQ(num_results, requests.size);
        }
    }

    SUBCASE("randomly send lots of requests") {
        sample_lib::InstrumentId const inst_ids[] {
            {
                .library_name = "SharedFilesMdata"_s,
                .inst_name = "Groups And Refs"_s,
            },
            {
                .library_name = "SharedFilesMdata"_s,
                .inst_name = "Groups And Refs (copy)"_s,
            },
            {
                .library_name = "SharedFilesMdata"_s,
                .inst_name = "Single Sample"_s,
            },
            {
                .library_name = "Test Lua"_s,
                .inst_name = "Auto Mapped Samples"_s,
            },
        };
        auto const builtin_irs = EmbeddedIrs();

        constexpr u32 k_num_calls = 200;
        u64 random_seed = SeedFromTime();
        AtomicCountdown countdown {k_num_calls};

        auto& channel = OpenAsyncCommsChannel(server, fixture.error_notif, [&]() { countdown.CountDown(); });
        DEFER { CloseAsyncCommsChannel(server, channel); };

        // We sporadically rename the library file to test the error handling of the loading thread
        DynamicArray<char> temp_rename {fixture.test_lib_path, scratch_arena};
        dyn::AppendSpan(temp_rename, ".foo");
        bool is_renamed = false;

        for (auto _ : Range(k_num_calls)) {
            SendAsyncLoadRequest(
                server,
                channel,
                (RandomIntInRange(random_seed, 0, 2) == 0)
                    ? LoadRequest {sample_lib::IrId {.library_name = k_builtin_library_name,
                                                     .ir_name = ({
                                                         auto const ele = RandomElement(
                                                             Span<BinaryData const> {builtin_irs.irs},
                                                             random_seed);
                                                         String {ele.name.data, ele.name.size};
                                                     })}}
                    : LoadRequest {LoadRequestInstrumentIdWithLayer {
                          .id = RandomElement(Span<sample_lib::InstrumentId const> {inst_ids}, random_seed),
                          .layer_index = RandomIntInRange<u32>(random_seed, 0, k_num_layers - 1)}});

            SleepThisThread(RandomIntInRange(random_seed, 0, 3));

            // Let's make this a bit more interesting by simulating a file rename mid-move
            if (RandomIntInRange(random_seed, 0, 4) == 0) {
                if (is_renamed)
                    TRY(MoveFile(temp_rename, fixture.test_lib_path, ExistingDestinationHandling::Fail));
                else
                    TRY(MoveFile(fixture.test_lib_path, temp_rename, ExistingDestinationHandling::Fail));
                is_renamed = !is_renamed;
            }

            // Additionally, let's release one the results to test ref-counting/reuse
            if (auto r = channel.results.TryPop()) r->Release();
        }

        constexpr u32 k_timeout_secs = 25;
        auto const countdown_result = countdown.WaitUntilZero(k_timeout_secs * 1000);

        if (countdown_result == WaitResult::TimedOut) {
            tester.log.ErrorLn("Timed out waiting for asset loading to complete");
            DumpCurrentStackTraceToStderr();
            server.request_debug_dump_current_state.Store(true);
            SleepThisThread(1000);
            // We need to hard-exit without cleaning up because the loading thread is probably deadlocked
            __builtin_abort();
        }
    }

    return k_success;
}

} // namespace sample_lib_server

TEST_REGISTRATION(RegisterSampleLibraryLoaderTests) {
    REGISTER_TEST(sample_lib_server::TestSampleLibraryLoader);
}
