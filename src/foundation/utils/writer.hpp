// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "foundation/container/array.hpp"
#include "foundation/container/span.hpp"
#include "foundation/error/error_code.hpp"
#include "foundation/universal_defs.hpp"
#include "foundation/utils/memory.hpp"

struct Writer {
    template <typename ObjectType>
    constexpr void Set(ObjectType& obj,
                       ErrorCodeOr<void> (*write_bytes)(ObjectType& obj, Span<u8 const> bytes)) {
        object = &obj;
        write_bytes_function_ptr = (void*)write_bytes;
        invoke_write_bytes = [](void* func_ptr, void* ob, Span<u8 const> bytes) -> ErrorCodeOr<void> {
            return ((decltype(write_bytes))func_ptr)(*(ObjectType*)ob, bytes);
        };
    }

    inline ErrorCodeOr<void> WriteByte(u8 byte) const { return WriteBytes({&byte, 1}); }
    inline ErrorCodeOr<void> WriteBytes(Span<u8 const> bytes) const {
        return invoke_write_bytes(write_bytes_function_ptr, object, bytes);
    }
    inline ErrorCodeOr<void> WriteChar(char c) const { return WriteByte((u8)c); }
    inline ErrorCodeOr<void> WriteChars(Span<char const> cs) const { return WriteBytes(cs.ToByteSpan()); }

    inline ErrorCodeOr<void> WriteCharRepeated(char c, usize count) const {
        Array<u8, 32> bytes = {};
        FillMemory(bytes, (u8)c);

        usize remaining = count;
        while (remaining > 0) {
            auto const to_write = Min(remaining, bytes.size);
            TRY(WriteBytes(Span<u8 const> {bytes}.SubSpan(0, to_write)));
            remaining -= to_write;
        }
        return k_success;
    }

    ErrorCodeOr<void> (*invoke_write_bytes)(void* func_ptr, void* object, Span<u8 const> bytes) = {};
    void* write_bytes_function_ptr = {};
    void* object = {};
};
