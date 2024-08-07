// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#define Rect MacRect
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wextra-semi"
#include <AppKit/AppKit.h>
#include <CoreServices/CoreServices.h>
#include <dirent.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sys/event.h>
#include <sys/stat.h>
#pragma clang diagnostic pop
#undef Rect

#include "foundation/foundation.hpp"
#include "os/misc.hpp"
#include "os/misc_mac.hpp"
#include "os/threading.hpp"
#include "utils/debug/debug.hpp"

#include "filesystem.hpp"

__attribute__((visibility("default")))
@interface MAKE_UNIQUE_OBJC_NAME(ClassForGettingBundle) : NSObject {}
@end
@implementation MAKE_UNIQUE_OBJC_NAME (ClassForGettingBundle)
@end

static ErrorCode FilesystemErrorFromNSError(NSError* error,
                                            char const* extra_debug_info = nullptr,
                                            SourceLocation loc = SourceLocation::Current()) {
    ASSERT(error != nil);
    if ([error.domain isEqualToString:NSCocoaErrorDomain]) {
        if (error.code == NSFileNoSuchFileError || error.code == NSFileReadNoSuchFileError)
            return ErrorCode(FilesystemError::PathDoesNotExist, extra_debug_info, loc);
        else if (error.code == NSURLErrorUserAuthenticationRequired)
            return ErrorCode(FilesystemError::AccessDenied, extra_debug_info, loc);
    } else if ([error.domain isEqualToString:NSURLErrorDomain]) {
        if (error.code == NSURLErrorUserAuthenticationRequired)
            return ErrorCode(FilesystemError::AccessDenied, extra_debug_info, loc);
    }
    return ErrorFromNSError(error, extra_debug_info, loc);
}

ErrorCodeOr<void> MoveFile(String from, String to, ExistingDestinationHandling existing) {
    if (existing == ExistingDestinationHandling::Skip || existing == ExistingDestinationHandling::Overwrite) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:StringToNSString(to)]) {
            if (existing == ExistingDestinationHandling::Skip) return k_success;

            TRY(Delete(to, {}));
        }
    }

    NSError* error = nil;
    if (![[NSFileManager defaultManager] moveItemAtPath:StringToNSString(from)
                                                 toPath:StringToNSString(to)
                                                  error:&error]) {
        return FilesystemErrorFromNSError(error);
    }
    return k_success;
}

ErrorCodeOr<void> CopyFile(String from, String to, ExistingDestinationHandling existing) {
    auto dest = StringToNSString(to);
    if (existing == ExistingDestinationHandling::Fail || existing == ExistingDestinationHandling::Skip) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:dest]) {
            return existing == ExistingDestinationHandling::Fail
                       ? ErrorCodeOr<void> {ErrorCode(FilesystemError::PathAlreadyExists)}
                       : k_success;
        }
    }

    NSError* error = nil;
    if (![[NSFileManager defaultManager] copyItemAtPath:StringToNSString(from) toPath:dest error:&error])
        return FilesystemErrorFromNSError(error);
    return k_success;
}

ErrorCodeOr<MutableString> ConvertToAbsolutePath(Allocator& a, String path) {
    ASSERT(path.size);
    auto nspath = StringToNSString(path);
    if (StartsWith(path, '~')) nspath = nspath.stringByExpandingTildeInPath;
    auto url = [NSURL fileURLWithPath:nspath];
    if (url == nil) return FilesystemErrorFromNSError(nullptr);
    auto abs_string = [url path];
    if (abs_string == nil) return FilesystemErrorFromNSError(nullptr);

    auto result = NSStringToString(abs_string).Clone(a);
    ASSERT(path::IsAbsolute(result));
    return result;
}

// NOTE: on macOS there are some 'firmlinks' which are like symlinks but are resolved by the kernel.
// These firmlinks are not resolved if we use the NSString resolving functions, so we use realpath.
// e.g. /var -> /private/var only happens with realpath
ErrorCodeOr<MutableString> ResolveSymlinks(Allocator& a, String path) {
    ASSERT(path.size);
    auto nspath = StringToNSString(path);
    char resolved_path[PATH_MAX * 2];
    if (realpath([nspath fileSystemRepresentation], resolved_path) == nullptr)
        return FilesystemErrnoErrorCode(errno);
    return a.Clone(FromNullTerminated(resolved_path));
}

ErrorCodeOr<void> Delete(String path, DeleteOptions options) {
    PathArena path_arena;
    switch (options.type) {
        case DeleteOptions::Type::Any:
        case DeleteOptions::Type::File:
        case DeleteOptions::Type::DirectoryRecursively: {
            NSError* error = nil;
            if (![[NSFileManager defaultManager] removeItemAtPath:StringToNSString(path) error:&error]) {
                auto const err = FilesystemErrorFromNSError(error);
                if (err == FilesystemError::PathDoesNotExist && !options.fail_if_not_exists) return k_success;
                return err;
            }
            return k_success;
        }
        case DeleteOptions::Type::DirectoryOnlyIfEmpty: {
            if (rmdir(NullTerminated(path, path_arena)) == 0)
                return k_success;
            else {
                if (errno == ENOENT && !options.fail_if_not_exists) return k_success;
                return FilesystemErrnoErrorCode(errno);
            }
            break;
        }
    }
    return k_success;
}

ErrorCodeOr<bool> DeleteDirectoryIfMacBundle(String dir) {
    auto bundle = [NSBundle bundleWithPath:StringToNSString(dir)];
    if (bundle != nil) {
        DebugLn("Deleting mac bundle");
        TRY(Delete(dir, {}));
        return true;
    }
    return false;
}

ErrorCodeOr<void> CreateDirectory(String path, CreateDirectoryOptions options) {
    if (options.create_intermediate_directories && options.fail_if_exists) {
        auto o = GetFileType(path);
        if (o.HasValue() && o.Value() == FileType::Directory)
            return ErrorCode {FilesystemError::PathAlreadyExists};
    }

    // returns YES if createIntermediates is set and the directory already exists
    NSError* error = nil;
    if (![[NSFileManager defaultManager]
                  createDirectoryAtPath:StringToNSString(path)
            withIntermediateDirectories:options.create_intermediate_directories ? YES : NO
                             attributes:nil
                                  error:&error]) {
        auto const ec = FilesystemErrorFromNSError(error);
        if (ec == FilesystemError::PathAlreadyExists && !options.fail_if_exists) return k_success;
    }
    return k_success;
}

static ErrorCodeOr<MutableString>
OSXGetSystemFilepath(Allocator& a, NSSearchPathDirectory type, NSSearchPathDomainMask domain) {
    NSError* error = nil;
    auto url = [[NSFileManager defaultManager] URLForDirectory:type
                                                      inDomain:domain
                                             appropriateForURL:nullptr
                                                        create:YES
                                                         error:&error];

    if (url == nil) return FilesystemErrorFromNSError(error);
    return a.Clone(NSStringToString(url.path));
}

ErrorCodeOr<MutableString> KnownDirectory(Allocator& a, KnownDirectories type) {
    NSSearchPathDirectory osx_type {};
    NSSearchPathDomainMask domain {};
    Optional<String> extra_subdirs {};
    Optional<String> fallback {};
    switch (type) {
        case KnownDirectories::Temporary: return a.Clone(NSStringToString(NSTemporaryDirectory()));
        case KnownDirectories::Logs:
            osx_type = NSLibraryDirectory;
            domain = NSUserDomainMask;
            extra_subdirs = "Logs";
            break;
        case KnownDirectories::PluginSettings:
            osx_type = NSMusicDirectory;
            domain = NSUserDomainMask;
            extra_subdirs = "Audio Music Apps/Plug-In Settings";
            break;
        case KnownDirectories::Documents:
            osx_type = NSDocumentDirectory;
            domain = NSUserDomainMask;
            break;
        case KnownDirectories::Downloads:
            osx_type = NSDownloadsDirectory;
            domain = NSUserDomainMask;
            break;
        case KnownDirectories::Prefs:
            osx_type = NSApplicationSupportDirectory;
            domain = NSUserDomainMask;
            break;
        case KnownDirectories::Data:
            osx_type = NSApplicationSupportDirectory;
            domain = NSUserDomainMask;
            break;
        case KnownDirectories::AllUsersData:
            osx_type = NSApplicationSupportDirectory;
            domain = NSLocalDomainMask;
            fallback = "/Library/Application Support";
            break;
        case KnownDirectories::AllUsersSettings:
            osx_type = NSApplicationSupportDirectory;
            domain = NSLocalDomainMask;
            fallback = "/Library/Application Support";
            break;
        case KnownDirectories::ClapPlugin:
            osx_type = NSLibraryDirectory;
            domain = NSLocalDomainMask;
            extra_subdirs = "Audio/Plug-Ins/CLAP";
            fallback = "/Library";
            break;
        case KnownDirectories::Vst3Plugin:
            osx_type = NSLibraryDirectory;
            domain = NSLocalDomainMask;
            extra_subdirs = "Audio/Plug-Ins/VST3";
            fallback = "/Library";
            break;
        case KnownDirectories::Count: PanicIfReached();
    }

    auto path = ({
        MutableString p {};
        if (auto outcome = OSXGetSystemFilepath(a, osx_type, domain); outcome.HasError())
            if (!fallback)
                return outcome.Error();
            else
                p = a.Clone(*fallback);
        else
            p = outcome.Value();
        p;
    });

    if (extra_subdirs) {
        auto p = DynamicArray<char>::FromOwnedSpan(path, a);
        path::JoinAppend(p, *extra_subdirs);
        path = p.ToOwnedSpan();
    }

    ASSERT(path::IsAbsolute(path));
    return path;
}

ErrorCodeOr<MutableString> CurrentExecutablePath(Allocator& a) {
    u32 size = 0;
    constexpr int k_buffer_not_large_enough = -1;
    char unused_buffer[1];
    errno = 0;
    if (_NSGetExecutablePath(unused_buffer, &size) == k_buffer_not_large_enough) {
        auto result = a.AllocateExactSizeUninitialised<char>(size);
        errno = 0;
        if (_NSGetExecutablePath(result.data, &size) == 0)
            return result;
        else
            a.Free(result.ToByteSpan());
    }
    if (errno != 0) return FilesystemErrnoErrorCode(errno);
    PanicIfReached();
    return MutableString {};
}

ErrorCodeOr<DynamicArrayInline<char, 200>> NameOfRunningExecutableOrLibrary() {
    if (NSBundle* bundle = [NSBundle bundleForClass:[MAKE_UNIQUE_OBJC_NAME(ClassForGettingBundle) class]])
        return path::Filename(NSStringToString(bundle.bundlePath));

    u32 size = 0;
    constexpr int k_buffer_not_large_enough = -1;
    char unused_buffer[1];
    errno = 0;
    if (_NSGetExecutablePath(unused_buffer, &size) == k_buffer_not_large_enough) {
        DynamicArrayInline<char, 200> result;
        dyn::Resize(result, size);
        errno = 0;
        if (_NSGetExecutablePath(result.data, &size) == 0) return result;
    }
    if (errno != 0) return FilesystemErrnoErrorCode(errno);
    PanicIfReached();
    return ""_s;
}

Optional<Version> MacosBundleVersion(String path) {
    auto bundle = [NSBundle bundleWithPath:StringToNSString(path)];
    if (bundle == nil) return {};
    NSString* version_string = bundle.infoDictionary[@"CFBundleVersion"];
    NSString* beta_version = bundle.infoDictionary[@"Beta_Version"];
    FixedSizeAllocator<64> temp_mem;
    DynamicArray<char> str {FromNullTerminated(version_string.UTF8String), temp_mem};
    if (beta_version != nil && beta_version.length) fmt::Append(str, "-Beta{}", beta_version.UTF8String);
    return ParseVersionString(str);
}

@interface DialogDelegate : NSObject <NSOpenSavePanelDelegate>
@property Span<DialogOptions::FileFilter const> filters; // NOLINT
@end

@implementation DialogDelegate
- (BOOL)panel:(id)sender shouldEnableURL:(NSURL*)url {
    NSString* ns_filename = [url lastPathComponent];
    auto const filename = NSStringToString(ns_filename);

    NSNumber* is_directory = nil;

    BOOL outcome = [url getResourceValue:&is_directory forKey:NSURLIsDirectoryKey error:nil];
    if (!outcome) return YES;
    if (is_directory) return YES;

    for (auto filter : self.filters)
        if (MatchWildcard(filter.wildcard_filter, filename)) return YES;

    return NO;
}
@end

ErrorCodeOr<Optional<MutableString>> FilesystemDialog(DialogOptions options) {
    auto delegate = [[DialogDelegate alloc] init];
    delegate.filters = options.filters;
    switch (options.type) {
        case DialogOptions::Type::SelectFolder: {
            NSOpenPanel* open_panel = [NSOpenPanel openPanel];
            open_panel.delegate = delegate;

            open_panel.title = StringToNSString(options.title);
            [open_panel setLevel:NSModalPanelWindowLevel];
            open_panel.showsResizeIndicator = YES;
            open_panel.showsHiddenFiles = NO;
            open_panel.canChooseDirectories = YES;
            open_panel.canChooseFiles = NO;
            open_panel.canCreateDirectories = YES;
            open_panel.allowsMultipleSelection = NO;
            if (options.default_path)
                open_panel.directoryURL = [NSURL fileURLWithPath:StringToNSString(*options.default_path)];

            auto const response = [open_panel runModal];
            if (response == NSModalResponseOK) {
                NSURL* selection = open_panel.URLs[0];
                NSString* path = [[selection path] stringByResolvingSymlinksInPath];
                auto utf8 = FromNullTerminated(path.UTF8String);
                if (path::IsAbsolute(utf8)) return utf8.Clone(options.allocator);
            }
            return nullopt;
        }
        case DialogOptions::Type::OpenFile: {
#if 0
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = StringViewToNSString(title);
    panel.directoryURL =
        [NSURL fileURLWithPath:StringViewToNSString(Legacy::Directory(default_path_and_file))
                   isDirectory:YES];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;

    [panel setLevel:NSModalPanelWindowLevel];
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
      if (result == NSModalResponseOK) {
          callback_function(std::string([[[[panel URLs] objectAtIndex:0] path] UTF8String]));
      }
    }];
#endif
            TODO(); // TODO(1.0)
            return nullopt;
        }
        case DialogOptions::Type::SaveFile: {
            NSSavePanel* panel = [NSSavePanel savePanel];
            panel.title = StringToNSString(options.title);
            [panel setLevel:NSModalPanelWindowLevel];
            if (options.default_path) {
                if (auto const dir = path::Directory(*options.default_path))
                    panel.directoryURL = [NSURL fileURLWithPath:StringToNSString(*dir) isDirectory:YES];
            }

            auto const response = [panel runModal];
            if (response == NSModalResponseOK) {
                auto const path = NSStringToString([[panel URL] path]);
                if (path::IsAbsolute(path)) return path.Clone(options.allocator);
            }
            return nullopt;
        }
    }

    return nullopt;
}

// NOTE: It seems you can receive filesystem events from before you start watching the directory even if you
// use the kFSEventStreamEventIdSinceNow flag. There could be some sort of buffering going on.

// NOTE: FSEvents is a callback based API. You receive events in a separate thread and there doesn't seem to
// be a way around that. FSEventStreamFlushSync might flush the events but you can still receive others at any
// time in the other thread. Therefore we use a queue to pass the buffer the events from the background thread
// to the PollDirectoryChanges thread.

// References:
// https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/UsingtheFSEventsFramework/UsingtheFSEventsFramework.html
// CHOC library: https://github.com/Tracktion/choc/blob/main/platform/choc_FileWatcher.h
// EFSW library: https://github.com/SpartanJ/efsw/blob/master/src/efsw/WatcherFSEvents.cpp
// Atom file watcher: https://github.com/atom/watcher/blob/master/docs/macos.md

constexpr bool k_debug_fsevents = false;

struct MacWatcher {
    FSEventStreamRef stream {};
    dispatch_queue_t queue {};

    struct Event {
        String path;
        FSEventStreamEventFlags flags;
    };
    Mutex event_mutex;
    ArenaAllocator event_arena {PageAllocator::Instance()};
    ArenaStack<Event> events;
};

ErrorCodeOr<DirectoryWatcher> CreateDirectoryWatcher(Allocator& a) {
    ZoneScoped;
    DirectoryWatcher result {
        .allocator = a,
        .watched_dirs = {a},
        .native_data = {.pointer = a.New<MacWatcher>()},
    };
    return result;
}

void DestoryDirectoryWatcher(DirectoryWatcher& watcher) {
    ZoneScoped;

    auto mac_w = (MacWatcher*)watcher.native_data.pointer;
    if (mac_w->stream) {
        FSEventStreamStop(mac_w->stream);
        FSEventStreamInvalidate(mac_w->stream);
        FSEventStreamRelease(mac_w->stream);
    }
    watcher.watched_dirs.Clear();
    watcher.allocator.Delete(mac_w);
}

void EventCallback([[maybe_unused]] ConstFSEventStreamRef stream_ref,
                   [[maybe_unused]] void* user_data,
                   size_t num_events,
                   void* event_paths,
                   FSEventStreamEventFlags const event_flags[],
                   [[maybe_unused]] FSEventStreamEventId const event_ids[]) {
    auto& watcher = *(MacWatcher*)user_data;
    auto** paths = (char**)event_paths;

    if constexpr (!PRODUCTION_BUILD && k_debug_fsevents) {
        DynamicArrayInline<char, 4000> info;
        struct Flag {
            FSEventStreamEventFlags flag;
            String name;
        };
        constexpr Flag k_flags[] = {
            {kFSEventStreamEventFlagMustScanSubDirs, "MustScanSubDirs"},
            {kFSEventStreamEventFlagUserDropped, "UserDropped"},
            {kFSEventStreamEventFlagKernelDropped, "KernelDropped"},
            {kFSEventStreamEventFlagEventIdsWrapped, "EventIdsWrapped"},
            {kFSEventStreamEventFlagHistoryDone, "HistoryDone"},
            {kFSEventStreamEventFlagRootChanged, "RootChanged"},
            {kFSEventStreamEventFlagMount, "Mount"},
            {kFSEventStreamEventFlagUnmount, "Unmount"},
            {kFSEventStreamEventFlagItemChangeOwner, "ItemChangeOwner"},
            {kFSEventStreamEventFlagItemCreated, "ItemCreated"},
            {kFSEventStreamEventFlagItemFinderInfoMod, "ItemFinderInfoMod"},
            {kFSEventStreamEventFlagItemInodeMetaMod, "ItemInodeMetaMod"},
            {kFSEventStreamEventFlagItemIsDir, "ItemIsDir"},
            {kFSEventStreamEventFlagItemIsFile, "ItemIsFile"},
            {kFSEventStreamEventFlagItemIsHardlink, "ItemIsHardlink"},
            {kFSEventStreamEventFlagItemIsLastHardlink, "ItemIsLastHardlink"},
            {kFSEventStreamEventFlagItemIsSymlink, "ItemIsSymlink"},
            {kFSEventStreamEventFlagItemModified, "ItemModified"},
            {kFSEventStreamEventFlagItemRemoved, "ItemRemoved"},
            {kFSEventStreamEventFlagItemRenamed, "ItemRenamed"},
            {kFSEventStreamEventFlagItemXattrMod, "ItemXattrMod"},
            {kFSEventStreamEventFlagOwnEvent, "OwnEvent"},
            {kFSEventStreamEventFlagItemCloned, "ItemCloned"},
        };
        auto writer = dyn::WriterFor(info);

        auto _ = fmt::AppendLine(writer, "FSEvent received {} events:", num_events);

        for (size_t i = 0; i < num_events; i++) {
            MAYBE_UNUSED auto u1 = fmt::AppendLine(writer, "  {{");
            MAYBE_UNUSED auto u2 = fmt::AppendLine(writer, "    path: {}", paths[i]);

            DynamicArrayInline<char, 1000> flags_str;
            for (auto const& flag : k_flags)
                if (event_flags[i] & flag.flag) {
                    dyn::AppendSpan(flags_str, flag.name);
                    dyn::AppendSpan(flags_str, ", ");
                }
            if (flags_str.size) flags_str.size -= 2;
            MAYBE_UNUSED auto u3 = fmt::AppendLine(writer, "    flags: {}", flags_str);
            MAYBE_UNUSED auto u4 = fmt::AppendLine(writer, "    id: {}", event_ids[i]);

            MAYBE_UNUSED auto u5 = fmt::AppendLine(writer, "  }}");
        }

        StdPrint(StdStream::Err, info);
    }

    {
        watcher.event_mutex.Lock();
        DEFER { watcher.event_mutex.Unlock(); };
        for (size_t i = 0; i < num_events; i++) {
            watcher.events.Append(
                {
                    .path = watcher.event_arena.Clone(FromNullTerminated(paths[i])),
                    .flags = event_flags[i],
                },
                watcher.event_arena);
        }
    }
}

ErrorCodeOr<Span<DirectoryWatcher::DirectoryChanges const>>
PollDirectoryChanges(DirectoryWatcher& watcher, PollDirectoryChangesArgs args) {
    auto const any_states_changed =
        watcher.HandleWatchedDirChanges(args.dirs_to_watch, args.retry_failed_directories);

    for (auto& dir : watcher.watched_dirs)
        dir.directory_changes.Clear();

    auto& mac_watcher = *(MacWatcher*)watcher.native_data.pointer;
    if (any_states_changed) {
        if (mac_watcher.stream) {
            FSEventStreamStop(mac_watcher.stream);
            FSEventStreamInvalidate(mac_watcher.stream);
            FSEventStreamRelease(mac_watcher.stream);
        }

        CFMutableArrayRef paths = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        DEFER { CFRelease(paths); };
        for (auto& dir : watcher.watched_dirs) {
            if (dir.state != DirectoryWatcher::WatchedDirectory::State::Watching &&
                dir.state != DirectoryWatcher::WatchedDirectory::State::NeedsWatching)
                continue;

            CFStringRef cf_path = CFStringCreateWithBytes(nullptr,
                                                          (u8 const*)dir.path.data,
                                                          (CFIndex)dir.path.size,
                                                          kCFStringEncodingUTF8,
                                                          false);
            DEFER { CFRelease(cf_path); };

            auto ns_string = (__bridge NSString*)cf_path;
            if ([[NSFileManager defaultManager] fileExistsAtPath:ns_string isDirectory:nil]) {
                CFArrayAppendValue(paths, cf_path);
            } else {
                dir.state = DirectoryWatcher::WatchedDirectory::State::WatchingFailed;
                dir.directory_changes.error = ErrorCode {FilesystemError::PathDoesNotExist};
            }
        }

        if (CFArrayGetCount(paths) != 0) {
            bool success = false;
            DEFER {
                if (!success) {
                    mac_watcher.stream = {};
                    for (auto& dir : watcher.watched_dirs)
                        dir.state = DirectoryWatcher::WatchedDirectory::State::WatchingFailed;
                }
            };

            FSEventStreamContext context {};
            context.info = &mac_watcher;

            mac_watcher.stream =
                FSEventStreamCreate(kCFAllocatorDefault,
                                    &EventCallback,
                                    &context,
                                    paths,
                                    kFSEventStreamEventIdSinceNow,
                                    args.coalesce_latency_ms / 1000.0,
                                    kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot |
                                        kFSEventStreamCreateFlagNoDefer);
            if (!mac_watcher.stream) return ErrorCode {FilesystemError::FileWatcherCreationFailed};

            // FSEvents is a callback based API where you always receive events in a separate thread.
            if (!mac_watcher.queue) {
                auto attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                    QOS_CLASS_USER_INITIATED,
                                                                    -10);
                mac_watcher.queue = dispatch_queue_create("com.floe.fseventsqueue", attr);
            }
            FSEventStreamSetDispatchQueue(mac_watcher.stream, mac_watcher.queue);

            if (!FSEventStreamStart(mac_watcher.stream)) {
                FSEventStreamInvalidate(mac_watcher.stream);
                FSEventStreamRelease(mac_watcher.stream);
                return ErrorCode {FilesystemError::FileWatcherCreationFailed};
            }

            success = true;
        }
    }

    if (mac_watcher.stream) FSEventStreamFlushAsync(mac_watcher.stream);

    {
        mac_watcher.event_mutex.Lock();
        DEFER { mac_watcher.event_mutex.Unlock(); };

        for (auto const& event : mac_watcher.events) {
            for (auto& dir : watcher.watched_dirs) {
                if (StartsWithSpan(event.path, dir.resolved_path)) {
                    String subpath {};
                    if (event.path.size != dir.resolved_path.size)
                        subpath = event.path.SubSpan(dir.resolved_path.size);
                    if (subpath.size && subpath[0] == '/') subpath = subpath.SubSpan(1);
                    subpath = args.result_arena.Clone(subpath);

                    // FSEvents ONLY supports recursive watching so we just have to ignore subdirectory
                    // events
                    if (!dir.recursive && Contains(subpath, '/')) {
                        DebugLn("Ignoring subdirectory event: {} because {} is watched non-recursively",
                                subpath,
                                dir.path);
                        continue;
                    }

                    auto const file_type = (event.flags & kFSEventStreamEventFlagItemIsDir)
                                               ? FileType::Directory
                                               : FileType::File;

                    if (event.flags & kFSEventStreamEventFlagMustScanSubDirs) {
                        dir.directory_changes.Add(
                            {
                                .subpath = subpath,
                                .file_type = file_type,
                                .changes = DirectoryWatcher::ChangeType::ManualRescanNeeded,
                            },
                            args.result_arena);
                        continue;
                    }

                    // There is no way of knowing the exact ordering of events from FSEvents. We can lstat on
                    // a file to see if it exists at the time we receive the event which helps in some
                    // situations. But some situations are still ambiguous. Consider the case where we have 3
                    // flags set 'created', 'renamed' and 'removed' and we have checked that the file does not
                    // currently exist: we can't know if it was (1) removed -> created -> renamed (old-name)
                    // or (2) renamed (old-name) -> created -> removed.

                    // Additionally, checking the filesystem (with lstat() for example) is not at all reliable
                    // because what we are doing is tagging an event with the current state of the filesystem
                    // rather than the state at the time of the event.

                    DirectoryWatcher::ChangeTypeFlags changes {};
                    if (event.flags & kFSEventStreamEventFlagItemCreated)
                        changes |= DirectoryWatcher::ChangeType::Added;
                    if (event.flags & kFSEventStreamEventFlagItemRemoved)
                        changes |= DirectoryWatcher::ChangeType::Deleted;
                    if (event.flags & kFSEventStreamEventFlagItemRenamed)
                        changes |= DirectoryWatcher::ChangeType::RenamedOldOrNewName;
                    if (event.flags & kFSEventStreamEventFlagItemModified)
                        changes |= DirectoryWatcher::ChangeType::Modified;

                    if (changes)
                        dir.directory_changes.Add(
                            {
                                .subpath = subpath,
                                .file_type = file_type,
                                .changes = changes,
                            },
                            args.result_arena);
                }
            }
        }

        mac_watcher.events.Clear();
        mac_watcher.event_arena.ResetCursorAndConsolidateRegions();
    }

    watcher.RemoveAllNotWatching();

    return watcher.AllDirectoryChanges(args.result_arena);
}
