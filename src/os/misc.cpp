// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/misc.hpp"
#ifdef _WIN32
#include <windows.h>

#include "os/undef_windows_macros.h"
#endif
#include <string.h> // strerror

#include "os/threading.hpp"

static constexpr ErrorCodeCategory k_errno_category {
    .category_id = "PX",
    .message = [](Writer const& writer, ErrorCode code) -> ErrorCodeOr<void> {
#ifdef _WIN32
        char buffer[200];
        auto strerror_return_code = strerror_s(buffer, ArraySize(buffer), (int)code.code);
        if (strerror_return_code != 0) {
            PanicIfReached();
            return fmt::FormatToWriter(writer, "strerror failed: {}", strerror_return_code);
        } else {
            return writer.WriteChars(FromNullTerminated(buffer));
        }
#else
        char buffer[200] {};
        strerror_r((int)code.code, buffer, ArraySize(buffer));
        return writer.WriteChars(FromNullTerminated(buffer));
#endif
    },
};

ErrorCode ErrnoErrorCode(s64 error_code, char const* extra_debug_info, SourceLocation loc) {
    return ErrorCode {k_errno_category, error_code, extra_debug_info, loc};
}

Mutex& StdStreamMutex(StdStream stream) {
    switch (stream) {
        case StdStream::Out: {
            [[clang::no_destroy]] static Mutex out_mutex;
            return out_mutex;
        }
        case StdStream::Err: {
            [[clang::no_destroy]] static Mutex err_mutex;
            return err_mutex;
        }
    }
    PanicIfReached();
}

#if !IS_WINDOWS
bool IsRunningUnderWine() { return false; }
#endif
