// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "foundation/foundation.hpp"

#include "threading.hpp"

ErrorCode ErrnoErrorCode(s64 error_code,
                         char const* info_for_developer = nullptr,
                         SourceLocation source_location = SourceLocation::Current());

DynamicArrayInline<char, 64> OperatingSystemName();
String GetFileBrowserAppName();

struct SystemStats {
    u32 num_logical_cpus = 0;
    u32 page_size = 0;
};

SystemStats GetSystemStats();

void OpenFolderInFileBrowser(String path);
void OpenUrlInBrowser(String url);

void* AllocatePages(usize bytes);
void FreePages(void* ptr, usize bytes);
void TryShrinkPages(void* ptr, usize old_size, usize new_size);

// Allocate whole pages at a time - often 4kb each; this is the smallest size that the OS gives out.
class PageAllocator : public Allocator {
    static usize AlignUpToPageSize(usize size) { return AlignForward(size, GetSystemStats().page_size); }

  public:
    Span<u8> DoCommand(AllocatorCommandUnion const& command_union) {
        CheckAllocatorCommandIsValid(command_union);

        switch (command_union.tag) {
            case AllocatorCommand::Allocate: {
                auto const& cmd = command_union.Get<AllocateCommand>();

                Span<u8> result;
                auto const request_page_size = AlignUpToPageSize(cmd.size);
                auto mem = AllocatePages(request_page_size);
                if (mem == nullptr) Panic("out of memory");
                result = {(u8*)mem, request_page_size};

                ASSERT(__builtin_align_up(result.data, cmd.alignment) == result.data);

                return {result.data, cmd.allow_oversized_result ? result.size : cmd.size};
            }

            case AllocatorCommand::Free: {
                auto const& cmd = command_union.Get<FreeCommand>();
                if (cmd.allocation.size == 0) return {};

                FreePages(cmd.allocation.data, AlignUpToPageSize(cmd.allocation.size));
                return {};
            }

            case AllocatorCommand::Resize: {
                auto const& cmd = command_union.Get<ResizeCommand>();

                if (cmd.new_size < cmd.allocation.size) {
                    TryShrinkPages(cmd.allocation.data, AlignUpToPageSize(cmd.allocation.size), cmd.new_size);
                    return {cmd.allocation.data, cmd.new_size};
                } else if (cmd.new_size > cmd.allocation.size) {
                    // IMPROVE: can the OS grow the page?

                    return ResizeUsingNewAllocation(cmd, k_max_alignment);
                } else {
                    return cmd.allocation;
                }
            }
        }
        return {};
    }

    static Allocator& Instance() {
        [[clang::no_destroy]] static PageAllocator a;
        return a;
    }
};

void StartupCrashHandler();
void ShutdownCrashHandler();

enum class StdStream { Out, Err };

// Unbuffered
void StdPrint(StdStream stream, String str);

Mutex& StdStreamMutex(StdStream stream);

// A point in time. It has no defined reference. You can't get seconds-from-Epoch from it, for example.
class TimePoint {
  public:
    TimePoint() {}
    explicit TimePoint(s64 t) : m_time(t) {}
    static TimePoint Now();

    f64 SecondsFromNow() const { return Now() - *this; }
    s64 Raw() const { return m_time; }

    friend f64 operator-(TimePoint lhs, TimePoint rhs);
    friend TimePoint operator+(TimePoint t, f64 s);

    explicit operator bool() const { return m_time != 0; }

    constexpr bool operator<(TimePoint const& other) const { return m_time < other.m_time; }
    constexpr bool operator<=(TimePoint const& other) const { return m_time <= other.m_time; }
    constexpr bool operator>(TimePoint const& other) const { return m_time > other.m_time; }
    constexpr bool operator>=(TimePoint const& other) const { return m_time >= other.m_time; }

  private:
    s64 m_time {};
};

f64 operator-(TimePoint lhs, TimePoint rhs);
TimePoint operator+(TimePoint t, f64 s);

struct Stopwatch {
    Stopwatch() : start(TimePoint::Now()) {}
    f64 SecondsElapsed() const { return TimePoint::Now() - start; }
    f64 MicrosecondsElapsed() const { return SecondsToMicroseconds(SecondsElapsed()); }
    f64 MillisecondsElapsed() const { return SecondsToMilliseconds(SecondsElapsed()); }
    void Reset() { start = TimePoint::Now(); }

    TimePoint start;
};

namespace fmt {

PUBLIC ErrorCodeOr<void> CustomValueToString(Writer writer, Stopwatch value, FormatOptions options) {
    char buffer[32];
    auto const size =
        CheckedCast<usize>(stbsp_snprintf(buffer, sizeof(buffer), "%.4f ms", value.MillisecondsElapsed()));
    TRY(PadToRequiredWidthIfNeeded(writer, options, size));
    return writer.WriteChars({buffer, size});
}

} // namespace fmt
