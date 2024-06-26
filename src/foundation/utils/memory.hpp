// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "foundation/container/span.hpp"
#include "foundation/universal_defs.hpp"

PUBLIC constexpr usize Kb(usize kb) { return kb * 1024; }
PUBLIC constexpr usize Mb(usize mb) { return mb * 1024 * 1024; }

PUBLIC inline void ZeroMemory(Span<u8> bytes) {
    for (auto& b : bytes)
        b = 0;
}
PUBLIC inline void ZeroMemory(void* ptr, usize num_bytes) { ZeroMemory({(u8*)ptr, num_bytes}); }

PUBLIC inline void FillMemory(Span<u8> bytes, u8 value) {
    for (auto& b : bytes)
        b = value;
}
PUBLIC inline void FillMemory(void* ptr, u8 value, usize num_bytes) {
    FillMemory({(u8*)ptr, num_bytes}, value);
}

PUBLIC inline void CopyMemory(void* destination, void const* source, usize num_bytes) {
    for (auto const i : Range(num_bytes))
        ((u8*)destination)[i] = ((u8 const*)source)[i];
}

// aka memmove
PUBLIC inline void MoveMemory(void* destination, void const* source, usize num_bytes) {
    if (destination == source) return;
    if (destination < source)
        for (auto const i : Range(num_bytes))
            ((u8*)destination)[i] = ((u8 const*)source)[i];
    else
        for (usize i = num_bytes - 1; i != usize(-1); --i)
            ((u8*)destination)[i] = ((u8 const*)source)[i];
}

PUBLIC inline bool MemoryIsEqual(void const* a, void const* b, usize num_bytes) {
    auto bytes_a = (u8 const*)a;
    auto bytes_b = (u8 const*)b;
    for (auto const i : Range(num_bytes))
        if (bytes_a[i] != bytes_b[i]) return false;
    return true;
}

static constexpr usize k_max_alignment = sizeof(void*) * 2;

PUBLIC constexpr bool IsPowerOfTwo(usize v) { return !(v & (v - 1)); }
PUBLIC constexpr usize Power2Modulo(usize x, usize y) { return x & (y - 1); }
// https://graphics.stanford.edu/%7Eseander/bithacks.html#RoundUpPowerOf2
PUBLIC constexpr u32 NextPowerOf2(u32 x) {
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;
    return x;
}

// Finds the next value that is aligned to 'alignment'
PUBLIC constexpr usize AlignForward(usize value, usize alignment) {
    return __builtin_align_up(value, alignment);
}

// returns true if the pointer is aligned to a multiple of 'alignment'
PUBLIC ALWAYS_INLINE constexpr bool IsAligned(void const* pointer, usize alignment) {
    return __builtin_is_aligned(pointer, alignment);
}

PUBLIC constexpr usize BytesToAddForAlignment(uintptr ptr, usize alignment) {
    ASSERT(IsPowerOfTwo(alignment));
    auto const m1 = alignment - 1;
    auto const aligned = ((ptr + m1) & ~m1);
    return aligned - ptr;
}

template <typename Type>
PUBLIC constexpr Span<u8 const> AsBytes(Type const& obj) {
    return {(u8 const*)&obj, sizeof(Type)};
}

template <typename Type>
PUBLIC constexpr Span<u8> AsMutableBytes(Type& obj) {
    return {(u8*)&obj, sizeof(Type)};
}
