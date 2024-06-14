// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

// This file contains things that we want in all of our files.

#pragma once

#include "config.h" // generated by the build system

// ==========================================================================================================
// As Clang is our only supported compiler, we can just use the predefined macros rather than including the
// standard headers; making this slightly more lightweight.
using u8 = __UINT8_TYPE__;
using s8 = __INT8_TYPE__;
using u16 = __UINT16_TYPE__;
using s16 = __INT16_TYPE__;
using u32 = __UINT32_TYPE__;
using s32 = __INT32_TYPE__;
using u64 = __UINT64_TYPE__;
using s64 = __INT64_TYPE__;
using u128 = unsigned __int128;
using s128 = __int128;
using usize = __SIZE_TYPE__;
using ssize = __PTRDIFF_TYPE__;
using uintptr = __UINTPTR_TYPE__;

using f32 = float;
using f64 = double;

using u14 = unsigned _BitInt(14);
using s14 = signed _BitInt(14);
using u7 = unsigned _BitInt(7);
using s7 = signed _BitInt(7);
using u4 = unsigned _BitInt(4);
using s4 = signed _BitInt(4);

// We rely on Clang's vector extensions for both convenience (we don't have to define operator overloads for
// +, -, *, etc.) and for the core of our SIMD code generation.
// https://clang.llvm.org/docs/LanguageExtensions.html#vectors-and-extended-vectors
//
// Some miscellaneous things to note:
// - You can automatically use all the same operators as you would with a scalar type: +, -, *, /, etc.
// - You can access the elements of the vector using member access syntax: v.x, v.y, v.z, v.w, or with
//   array-like access: v[0], v[1], v[2], v[3].
// - If you are regularly accessing the elements of the vector, it's quite possible the code will be less
//   efficient than non-vector code. Accessing vector elements requires additional unpacking/packing
//   instructions.
// - You cannot take the address of a vector element and pass that along to other code.
// - There are builtin functions for SIMD-specific operations: shuffle, convert, etc. Also maths operations:
//   min, max, round, sqrt, etc.
// - If you assign a vector to a scalar, it will broadcast that scalar to all elements of the vector. You can
//   also use a 'constructor' with brackets: vec(scalar) to do the same. Curly braces work the same as an
//   array initializer: you may omit elements and the rest will be zeroed.
//   f32x4 v(0); // v = {0, 0, 0, 0}
//   f32x4 v = 0; // v = {0, 0, 0, 0}
//   f32x4 v = {1, 2}; // v = {1, 2, 0, 0}
// - The generated assembly is dependent on the target architecture. If the target architecture doesn't
//   support SIMD operations of the width you're using, it won't generate particularly fast code. For now, we
//   are assuming that the target supports SIMD of at least 128 bits (four 32-bit lanes): SSE2 or NEON. If we
//   want to make use of wider SIMD operations we'll need to, at runtime, determine what the running system
//   supports, and based on that select the appropriate codepath that uses the intrinsics for that
//   architecture. For example for supporting AVX2 on x86_64, we'd use the <immintrin.h> header. NOTE: we
//   might consider using __builtin_cpu_supports to detect CPU features on x86_64.

using f32x2 = __attribute__((ext_vector_type(2))) f32;
using f32x4 = __attribute__((ext_vector_type(4))) f32;
using u8x4 = __attribute__((ext_vector_type(4))) u8;

// ==========================================================================================================
enum class Arch {
    X86_64, // NOLINT(readability-identifier-naming)
    Aarch64,
};
#ifdef __aarch64__
constexpr auto k_arch = Arch::Aarch64;
#elif defined(__x86_64__)
constexpr auto k_arch = Arch::X86_64;
#endif

enum class Endianness {
    Little,
    Big,
};
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
constexpr auto k_endianness = Endianness::Little;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
constexpr auto k_endianness = Endianness::Big;
#endif

// ==========================================================================================================
struct SourceLocation {
    // NOTE: could use __builtin_FILE_NAME() instead?
    static SourceLocation Current(char const* file = __builtin_FILE(),
                                  int line = __builtin_LINE(),
                                  char const* function = __builtin_FUNCTION()) {
        return {function, file, line};
    }

    char const* function;
    char const* file;
    int line;
};

// Type manipulation
// ================================================================================================
// clang-format off
template <typename T> using RemoveReference = __remove_reference_t(T);
template <typename T> using RemoveVolatile = __remove_volatile(T);
template <typename T> using RemoveCV = __remove_cv(T);
template <typename T> using RemoveConst = __remove_const(T);
template <typename T> using RemoveCVReference = __remove_cvref(T);
template <typename T> using MakeUnsigned = __make_unsigned(T);
template <typename T> using MakeSigned = __make_signed(T);
template <typename T> using RemoveExtent = __remove_extent(T);
template <typename T> using AddConst = const T;
template <typename T> using RemovePointer = __remove_pointer(T);
template <typename T> using AddPointer = __add_pointer(T);
template <typename T> using AddLvalueReference = __add_lvalue_reference(T);
template <typename T> using AddRvalueReference = __add_rvalue_reference(T);
template <typename T> using Decay = __decay(T);
template <typename T> using UnderlyingType = __underlying_type(T); // enum only
// clang-format on

// Concepts
// ================================================================================================
// clang-format off
template <typename T> auto Declval() -> T; 
template <typename T> concept Enum = __is_enum(T);
template <typename T> concept ScopedEnum = __is_scoped_enum(T);
template <typename T> concept LvalueReference = __is_lvalue_reference(T);
template <typename T> concept RvalueReference = __is_rvalue_reference(T);
template <typename T> concept Reference = __is_reference(T);
template <typename T> concept Integral = __is_integral(T) && !__is_same(T, bool);
template <typename T> concept FloatingPoint = __is_floating_point(T);
template <typename T, typename U> concept Same = __is_same(T, U);
template <typename T> concept Signed = __is_signed(T); // floating point are signed too
template <typename T> concept SignedInt = __is_integral(T) && __is_signed(T);
template <typename T> concept UnsignedInt = __is_unsigned(T);
template <typename T> concept Const = __is_const(T);
template <typename T> concept CharacterType = Same<RemoveCV<T>, char> || Same<RemoveCV<T>, wchar_t>;
template <typename T, typename... Args> concept ConstructibleWithArgs = __is_constructible(T, Args...);
template <typename T, typename... Args> concept CallableWithArguments = requires(T t) { t(Declval<Args>()...); };
template <typename From, typename To> concept Convertible = requires { Declval<void (*)(To)>()(Declval<From>()); };
template <typename T> concept CArray = __is_array(T);
template <typename T> concept Pointer = __is_pointer(T);
template <typename T> concept Void = __is_void(T);
template <typename T> concept Nullptr = __is_nullptr(T);
template <typename T> concept FunctionType = __is_function(T);
template <typename T> concept Arithmetic = __is_arithmetic(T);
template <typename T> concept Scalar = __is_scalar(T);
template <typename T> concept Fundamental = __is_fundamental(T);
template <typename T> concept MoveConstructible = ConstructibleWithArgs<T, AddRvalueReference<T>>;
template <typename T> concept CopyConstructible = ConstructibleWithArgs<T, AddLvalueReference<AddConst<T>>>;
template <typename T> concept Trivial = __is_trivial(T);
template <typename T> concept TriviallyCopyable = __is_trivially_copyable(T);
template <typename T> concept TriviallyRelocatable = __is_trivially_relocatable(T);
template <typename T> concept TriviallyAssignable = __is_trivially_assignable(T, T);
template <typename T> concept TriviallyCopyAssignable = __is_trivially_assignable(AddLvalueReference<T>, AddLvalueReference<const T>);
template <typename T> concept DefaultConstructible = __is_constructible(T);
template <typename T> concept TriviallyDestructible = __is_trivially_destructible(T);
template <typename T> concept FunctionPointer = Pointer<T> && FunctionType<RemovePointer<T>>;
template <typename F> concept FunctionObject = (!FunctionPointer<F> && RvalueReference<F &&>);
// clang-format on

template <typename T>
concept Vector = requires(T v) { __builtin_shufflevector(v, v, 0, 0); };

template <Vector T>
struct UnderlyingTypeOfVecHelper {
    static constexpr T k_x; // For some reason we need to use an extra variable to get the type
    using Type = RemoveCVReference<decltype(k_x[0])>;
};
template <Vector T>
using UnderlyingTypeOfVec = typename UnderlyingTypeOfVecHelper<T>::Type;

template <typename T>
concept F32Vector = FloatingPoint<UnderlyingTypeOfVec<T>>;

template <typename Functor, typename ReturnType, typename... Args>
concept FunctionWithSignature = requires(Functor f) {
    { f(Declval<Args>()...) } -> Same<ReturnType>;
};

// Vector helpers
// ================================================================================================
// Like doing a cast for each element. The target type must have the same number of elements as the source.
#define ConvertVector(vec, type) __builtin_convertvector(vec, type)

template <typename VecType>
constexpr auto NumVectorElements() {
    return sizeof(VecType) / sizeof(UnderlyingTypeOfVec<VecType>);
}

// Template helpers
// ================================================================================================
// This section contains code from SerenityOS's file: serenity/AK/StdLibExtraDetails.h
// Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
// Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
// Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
// SPDX-License-Identifier: BSD-2-Clause

template <usize arg1, usize... others>
struct LargestValueInTemplateArgs;
template <usize arg>
struct LargestValueInTemplateArgs<arg> {
    static usize const value = arg;
};

template <usize arg1, usize arg2, usize... others>
struct LargestValueInTemplateArgs<arg1, arg2, others...> {
    static usize const value = arg1 >= arg2 ? LargestValueInTemplateArgs<arg1, others...>::value
                                            : LargestValueInTemplateArgs<arg2, others...>::value;
};

template <bool B, typename T, typename F>
struct ConditionalHelper {
    using Type = T;
};
template <typename T, typename F>
struct ConditionalHelper<false, T, F> {
    using Type = F;
};

template <bool B, typename T, typename F>
using Conditional = typename ConditionalHelper<B, T, F>::Type;

template <typename Type, typename... Args>
struct TypeIsPresentInVariadicArgsStruct {
    static constexpr bool k_value {(Same<Type, Args> || ...)};
};

template <typename Type, typename... Args>
concept IsTypePresentInVariadicArgs = TypeIsPresentInVariadicArgsStruct<Type, Args...>::k_value;

template <typename...>
struct InvokeResultHelper {};

template <typename MethodDefBaseType, typename MethodType, typename InstanceType, typename... Args>
struct InvokeResultHelper<MethodType MethodDefBaseType::*, InstanceType, Args...> {
    using Type =
        decltype((Declval<InstanceType>().*Declval<MethodType MethodDefBaseType::*>())(Declval<Args>()...));
};

template <typename F, typename... Args>
struct InvokeResultHelper<F, Args...> {
    using Type = decltype((Declval<F>())(Declval<Args>()...));
};

template <typename F, typename... Args>
using InvokeResult = typename InvokeResultHelper<F, Args...>::type;

template <typename T, template <typename...> typename U>
inline constexpr bool IsSpecializationOf = false; // NOLINT(readability-identifier-naming)

template <template <typename...> typename U, typename... Us>
inline constexpr bool IsSpecializationOf<U<Us...>, U> = true; // NOLINT(readability-identifier-naming)

template <typename T, typename... U>
concept IsAllSame = (... && Same<T, U>);

template <typename T, T... Vals>
struct IntegerSequence {
    static_assert(Integral<T>);
    using ValueType = T;
    static constexpr usize Size() { return sizeof...(Vals); }
};

template <typename T, T Size>
using MakeIntegerSequence = __make_integer_seq<IntegerSequence, T, Size>;

template <usize... Vals>
using IndexSequence = IntegerSequence<usize, Vals...>;

template <usize Size>
using MakeIndexSequence = MakeIntegerSequence<usize, Size>;

template <typename... Types>
using IndexSequenceFor = MakeIndexSequence<sizeof...(Types)>;

// ==========================================================================================================
#define ALWAYS_INLINE __attribute__((always_inline))

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x)        STRINGIFY_HELPER(x)

#define CONCAT_HELPER(x, y) x##y
#define CONCAT(x, y)        CONCAT_HELPER(x, y)

// ==========================================================================================================
[[noreturn]] void AssertionFailed(char const* expression, SourceLocation loc, char const* message = nullptr);
[[noreturn]] void Panic(char const* message, SourceLocation loc = SourceLocation::Current());
extern void (*g_panic_handler)(char const* message, SourceLocation loc);

// NOTE: the expression may be discarded so it mustn't have side effects
#define ASSERT(expression, ...)                                                                              \
    do {                                                                                                     \
        if constexpr (RUNTIME_SAFETY_CHECKS_ON) {                                                            \
            if (!(expression)) AssertionFailed(#expression, SourceLocation::Current(), ##__VA_ARGS__);       \
        } else                                                                                               \
            _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wassume\"")                \
                __builtin_assume(!!(expression));                                                            \
        _Pragma("clang diagnostic pop")                                                                      \
    } while (0)

// For use in hot code paths - this will be removed in production builds
#define ASSERT_HOT(expression, ...)                                                                          \
    do {                                                                                                     \
        if constexpr (RUNTIME_SAFETY_CHECKS_ON && !PRODUCTION_BUILD) {                                       \
            if (!(expression)) AssertionFailed(#expression, SourceLocation::Current(), ##__VA_ARGS__);       \
        } else                                                                                               \
            _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wassume\"")                \
                __builtin_assume(!!(expression));                                                            \
        _Pragma("clang diagnostic pop")                                                                      \
    } while (0)

#define TODO(message) Panic("TODO: " message)

[[noreturn]] [[clang::always_inline]] inline void
PanicIfReached(SourceLocation loc = SourceLocation::Current()) {
    Panic("unreachable code reached", loc);
}

// ================================================================================================
template <Integral T>
consteval T LargestRepresentableValue() {
    if constexpr (SignedInt<T>)
        return (1ull << ((sizeof(T) * 8ull) - 1ull)) - 1;
    else if constexpr (UnsignedInt<T>)
        return (T)(-1);
    else
        static_assert(false, "Unsupported type");
    return {};
}

template <Integral T>
consteval T SmallestRepresentableValue() {
    if constexpr (SignedInt<T>)
        return (T)(-(1ull << ((sizeof(T) * 8ull) - 1)));
    else if constexpr (UnsignedInt<T>)
        return 0;
    else
        static_assert(false, "Unsupported type");
    return {};
}

template <Scalar ToType, Scalar FromType>
ALWAYS_INLINE constexpr bool NumberCastIsSafe(FromType val) {
    if constexpr (Same<ToType, FromType>) return true;

    // both unsigned
    else if constexpr (UnsignedInt<FromType> && UnsignedInt<ToType>)
        return val <= LargestRepresentableValue<ToType>();

    // both signed
    else if constexpr (Signed<FromType> && Signed<ToType>)
        return val >= SmallestRepresentableValue<ToType>() && val <= LargestRepresentableValue<ToType>();

    // from unsigned to signed int
    else if constexpr (UnsignedInt<FromType> && SignedInt<ToType>)
        return val <= (MakeUnsigned<ToType>)LargestRepresentableValue<ToType>();

    // from unsigned to float
    else if constexpr (UnsignedInt<FromType> && FloatingPoint<ToType>)
        return val <= LargestRepresentableValue<ToType>();

    // from signed int to unsigned
    else if constexpr (SignedInt<FromType> && UnsignedInt<ToType>)
        return val >= 0 && (MakeUnsigned<FromType>)val <= LargestRepresentableValue<ToType>();

    // from float to unsigned
    else if constexpr (FloatingPoint<FromType> && UnsignedInt<ToType>)
        return val >= 0 && val <= LargestRepresentableValue<ToType>();

    // both enum
    else if constexpr (Enum<FromType> && Enum<ToType>)
        return NumberCastIsSafe<UnderlyingType<ToType>>((UnderlyingType<FromType>)val);

    // enum to number
    else if constexpr (Enum<FromType>)
        return NumberCastIsSafe<ToType>((UnderlyingType<FromType>)val);

    // number to enum
    else if constexpr (Enum<ToType>)
        return NumberCastIsSafe<UnderlyingType<ToType>>(val);

    else
        static_assert(false, "unhandled case");
}

template <Scalar T1, Scalar T2>
ALWAYS_INLINE inline T1 CheckedCast(T2 v) {
    ASSERT(NumberCastIsSafe<T1>(v));
    return (T1)v;
}

template <Pointer T1, Pointer T2>
ALWAYS_INLINE inline T1 CheckedPointerCast(T2 v) {
    constexpr auto k_align = alignof(RemovePointer<T1>);
    if constexpr (k_align == 1) {
        return (T1)v;
    } else {
        ASSERT(__builtin_is_aligned(v, k_align));
        return (T1)(void*)v;
    }
}

template <Enum Type>
ALWAYS_INLINE constexpr auto ToInt(Type value) {
    return (UnderlyingType<Type>)value;
}

// ==========================================================================================================
template <typename T>
constexpr T&& Move([[clang::lifetimebound]] T& arg) {
    return static_cast<T&&>(arg);
}

template <typename T>
constexpr T&& Forward(RemoveReference<T>& param) {
    return static_cast<T&&>(param);
}

template <typename T>
constexpr T&& Forward(RemoveReference<T>&& param) {
    static_assert(!LvalueReference<T>);
    return static_cast<T&&>(param);
}

template <class T, usize N>
constexpr usize ArraySize(T const (&)[N]) {
    return N;
}

template <typename T, typename U = T>
constexpr T Exchange(T& slot, U&& value) {
    T old_value = Move(slot);
    slot = Forward<U>(value);
    return old_value;
}

template <typename T, typename U>
constexpr void Swap(T& a, U& b) {
    if (&a == &b) return;
    U temp = Move((U&)a);
    a = (T&&)Move(b);
    b = Move(temp);
}

// Helpers for looping over things:
// ==========================================================================================================
// Range:             for (auto index : Range(10)) { ... }
// Enumerate:         for (auto [index, elem] : Enumerate(array)) { ... }

template <Integral T = usize>
struct Range {
    ALWAYS_INLINE constexpr Range(T start, T stop) : m_start(start), m_stop(stop) {}

    ALWAYS_INLINE constexpr Range(T stop) : Range(0, stop) {}

    struct Iterator {
        ALWAYS_INLINE constexpr T operator*() const { return value; }
        ALWAYS_INLINE constexpr Iterator& operator++() {
            ++value;
            return *this;
        }
        ALWAYS_INLINE constexpr bool operator!=(Iterator const& other) const {
            return value < other.value && value >= boundary;
        }

        T value;
        T boundary;
    };

    ALWAYS_INLINE constexpr Iterator begin() { return {m_start, m_start}; }
    ALWAYS_INLINE constexpr Iterator end() { return {m_stop, m_start}; }

    T const m_start;
    T const m_stop;
};

// Enumerate is from Folly with slight modifications to fit our style.
// https://github.com/facebook/folly/blob/main/folly/container/Enumerate.h
// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
namespace detail {

template <Integral IndexType, class Iterator>
class Enumerator {
  public:
    constexpr explicit Enumerator(Iterator it) : m_it(Move(it)) {}

    class Proxy {
      public:
        using ReferenceType = decltype(*Declval<Iterator&>());
        using PointerType = decltype(Declval<Iterator>());

        ALWAYS_INLINE constexpr explicit Proxy(Enumerator const& e) : index(e.m_idx), element(*e.m_it) {}

        // Non-const Proxy: Forward constness from Iterator.
        ALWAYS_INLINE constexpr ReferenceType operator*() { return element; }
        ALWAYS_INLINE constexpr PointerType operator->() { return &element; }

        // Const Proxy: Force const references.
        ALWAYS_INLINE constexpr AddConst<ReferenceType> operator*() const { return element; }
        ALWAYS_INLINE constexpr AddConst<PointerType> operator->() const { return &element; }

      public:
        IndexType const index;
        ReferenceType element;
    };

    ALWAYS_INLINE constexpr Proxy operator*() const { return Proxy(*this); }

    ALWAYS_INLINE constexpr Enumerator& operator++() {
        ++m_it;
        ++m_idx;
        return *this;
    }

    template <Integral OtherIndexType, typename OtherIterator>
    ALWAYS_INLINE constexpr bool operator==(Enumerator<OtherIndexType, OtherIterator> const& rhs) const {
        return m_it == rhs.m_it;
    }

    template <Integral OtherIndexType, typename OtherIterator>
    ALWAYS_INLINE constexpr bool operator!=(Enumerator<OtherIndexType, OtherIterator> const& rhs) const {
        return !(m_it == rhs.m_it);
    }

  private:
    template <Integral OtherIndexType, typename OtherIterator>
    friend class Enumerator;

    Iterator m_it;
    IndexType m_idx = 0;
};

template <Integral IndexType, class Range>
class RangeEnumerator {
    Range m_r;
    using BeginIteratorType = decltype(Begin(Declval<Range>()));
    using EndIteratorType = decltype(End(Declval<Range>()));

  public:
    constexpr explicit RangeEnumerator(Range&& r) : m_r(Forward<Range>(r)) {}

    constexpr Enumerator<IndexType, BeginIteratorType> begin() {
        return Enumerator<IndexType, BeginIteratorType>(Begin(m_r));
    }
    constexpr Enumerator<IndexType, BeginIteratorType> end() {
        return Enumerator<IndexType, BeginIteratorType>(End(m_r));
    }
};

} // namespace detail

template <Integral IndexType = usize, class Range>
constexpr detail::RangeEnumerator<IndexType, Range> Enumerate(Range&& r) {
    return detail::RangeEnumerator<IndexType, Range>(Forward<Range>(r));
}

// DEFER
// ==========================================================================================================
template <typename T>
struct ExitScope {
    T lambda;
    ExitScope(T lambda) : lambda(lambda) {}
    ~ExitScope() { lambda(); }
    ExitScope(ExitScope const&);

  private:
    ExitScope& operator=(ExitScope const&);
};

class ExitScopeHelper {
  public:
    template <typename T>
    ExitScope<T> operator+(T t) {
        return t;
    }
};

// e.g. DEFER { free(str); };
#define DEFER [[maybe_unused]] const auto& CONCAT(defer_, __LINE__) = ExitScopeHelper() + [&]()

#define PUBLIC [[maybe_unused]] static

// [[maybe_unused]] can't be used in a few situations whereas the attribute syntax version can
#define MAYBE_UNUSED __attribute__((__unused__))

// Instead of including <new>
// ==========================================================================================================
struct PlacementNewTag {};
[[nodiscard]] constexpr void* operator new(__SIZE_TYPE__, PlacementNewTag, void* ptr) { return ptr; }
#define PLACEMENT_NEW(ptr) new (PlacementNewTag(), ptr)

// ==========================================================================================================
#define NON_COPYABLE(ClassName)                                                                              \
    ClassName(const ClassName&) = delete;                                                                    \
    ClassName& operator=(const ClassName&) = delete

#define NON_MOVEABLE(ClassName)                                                                              \
    ClassName(ClassName&&) = delete;                                                                         \
    ClassName& operator=(ClassName&&) = delete

#define NON_COPYABLE_AND_MOVEABLE(ClassName)                                                                 \
    NON_COPYABLE(ClassName);                                                                                 \
    NON_MOVEABLE(ClassName)

#define PROPAGATE_TRIVIALLY_COPYABLE(ClassName, ValueTypeName)                                               \
    constexpr ClassName(const ClassName& other) requires(TriviallyCopyable<ValueTypeName>)                   \
    = default;                                                                                               \
    constexpr ClassName(ClassName&& other) requires(TriviallyCopyable<ValueTypeName>)                        \
    = default;                                                                                               \
    constexpr ClassName& operator=(const ClassName& other) requires(TriviallyCopyable<ValueTypeName>)        \
    = default;                                                                                               \
    constexpr ClassName& operator=(ClassName&& other) requires(TriviallyCopyable<ValueTypeName>)             \
    = default;                                                                                               \
    constexpr ~ClassName() requires(TriviallyCopyable<ValueTypeName>)                                        \
    = default

#ifndef OBJC_NAME_PREFIX
#error                                                                                                       \
    "OBJC_NAME_PREFIX is missing; you must define this. When multiple bundles are loaded, the names of Objective-C classes are not namespaced, meaning that 2 different bundles could have name collisions. The only way to fix this is to make each bundle have a whole set of unique names for Objective-C classes"
#endif
#define MAKE_UNIQUE_OBJC_NAME(name) CONCAT(OBJC_NAME_PREFIX, name)
