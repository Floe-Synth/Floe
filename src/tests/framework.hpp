// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "foundation/container/dynamic_array.hpp"
#include "foundation/container/span.hpp"
#include "foundation/utils/maths.hpp"
#include "foundation/utils/string.hpp"
#include "os/misc.hpp"
#include "utils/debug/debug.hpp"
#include "utils/logger/logger.hpp"

#include "config.h"

// Overview:
// - In general, it's similar to Catch2 or doctest
// - For each system you want to test:
//   - Create test functions using the TEST_CASE macro
//   - Register each TEST_CASE using REGISTER_TEST
// - Doesn't use global state; test cases have to be manually registered
// - SUBCASEs work the same as Catch2/doctest: the test case is repeatidly called, with each time a different
//   branch of SUBCASEs are executed.
// - You can install fixtures; these are persistent for every iteration of a test case.
//
// Example of how the SUBCASE system repeatidly calls the test case:
// (based on doctest's example)
// clang-format off
// ---------------------------------------------------------------------------
// Example:                                                     |   Output:
// ---------------------------------------------------------------------------              
//                                                              |   root
//                                                              |   1
//                                                              |   1.1
//  TEST_CASE(MyTest) {                                         |          
//      printf("root\n");                                       |   root
//      SUBCASE("") {                                           |   2
//          printf("1\n");                                      |   2.1
//          SUBCASE("") { printf("1.1\n"); }                    |          
//      }                                                       |   root
//      SUBCASE("") {                                           |   2
//          printf("2\n");                                      |   2.2
//          SUBCASE("") { printf("2.1\n"); }                    |   2.2.1
//          SUBCASE("") {                                       |   2.2.1.1
//              printf("2.2\n");                                |          
//              SUBCASE("") {                                   |   root
//                  printf("2.2.1\n");                          |   2
//                  SUBCASE("") { printf("2.2.1.1\n"); }        |   2.2
//                  SUBCASE("") { printf("2.2.1.2\n"); }        |   2.2.1
//              }                                               |   2.2.1.2
//          }                                                   |          
//          SUBCASE("") { printf("2.3\n"); }                    |   root
//          SUBCASE("") { printf("2.4\n"); }                    |   2
//      }                                                       |   2.3
//  }                                                           |          
//                                                              |   root
//                                                              |   2
//                                                              |   2.4    
// ---------------------------------------------------------------------------
// clang-format on

template <typename FloatType>
constexpr bool ApproxEqual(FloatType a, FloatType b, FloatType epsilon) {
    return Abs(a - b) < epsilon * (1 + Max<FloatType>(Abs(a), Abs(b)));
}

namespace tests {

struct Result {
    Result(ErrorCode ec) : outcome(ec) { stacktrace = CurrentStacktrace(); }
    Result(SuccessType s) : outcome(s) { stacktrace = StacktraceStack {}; }
    Optional<StacktraceStack> stacktrace;
    ErrorCodeOr<void> outcome;
};

struct Tester;
using TestFunction = Result (*)(Tester&);
using CreateFixturePointer = void* (*)(Allocator& a, Tester& tester);
using DeleteFixturePointer = void (*)(void*, Allocator& a);

struct TestCase {
    TestFunction f;
    String title;
    bool failed = false;
};

struct SubcaseSignature {
    DynamicArrayInline<char, 128> name;
    String file;
    int line;
};

struct Subcase {
    Subcase(Tester& tester,
            String name,
            String file = FromNullTerminated(__builtin_FILE()),
            int line = __builtin_LINE());
    ~Subcase();
    operator bool() const { return entered; }

    Tester& tester;
    bool entered;
};

enum class FailureAction { FailAndExitTest, FailAndContinue, LogWarningAndContinue };

class PassedSubcaseStacks {
  public:
    PassedSubcaseStacks(Allocator& a) : m_hashes(a) {}
    void Clear() { dyn::Clear(m_hashes); }
    void Add(DynamicArray<SubcaseSignature> const& v) { dyn::Append(m_hashes, Hash(v)); }
    bool Contains(DynamicArray<SubcaseSignature> const& v) const { return ::Contains(m_hashes, Hash(v)); }

  private:
    static u64 Hash(DynamicArray<SubcaseSignature> const& v) {
        u64 result = 0;
        for (auto const& s : v)
            result += ::Hash(s.file) + (u64)s.line + ::Hash(s.name.Items());
        return result;
    }

    DynamicArray<u64> m_hashes;
};

struct TestLogger : Logger {
    TestLogger(Tester& tester) : tester(tester) { max_level_allowed = LogLevel::Info; }
    void LogFunction(String str, LogLevel level, bool add_newline) override;
    Tester& tester;
};

struct Tester {
    // public
    TestLogger log {*this};
    ArenaAllocator scratch_arena {PageAllocator::Instance()};
    FixedSizeAllocator<Kb(8)> capture_buffer;

    // private
    ArenaAllocator arena {PageAllocator::Instance()};
    DynamicArray<TestCase> test_cases {arena};
    DynamicArray<SubcaseSignature> subcases_stack {arena};
    PassedSubcaseStacks subcases_passed {arena};
    usize subcases_current_max_level {};
    bool should_reenter {};
    TestCase* current_test_case {};
    usize num_assertions = 0;
    usize num_warnings = 0;
    Optional<String> test_output_folder;
    Optional<String> test_files_folder;
    Optional<Optional<String>> build_resources_folder;
    ArenaAllocator fixture_arena {PageAllocator::Instance()};
    void* fixture_pointer {};
    DeleteFixturePointer delete_fixture {};
};

void RegisterTest(Tester& tester, TestFunction f, String title);
int RunAllTests(Tester& tester, Optional<String> filter_pattern = {});
void Check(Tester& tester,
           bool expression,
           String message,
           FailureAction failure_action,
           String file = FromNullTerminated(__builtin_FILE()),
           int line = __builtin_LINE());

constexpr auto k_build_resources_subdir = "build_resources"_s;

String TempFolder(Tester& tester);
String TestFilesFolder(Tester& tester);
Optional<String> BuildResourcesFolder(Tester& tester);

// Create some data that persists for all SUBCASEs rather than being created and destroyed every iteration.
// You can only have one of these; create a struct with ctor+dtor if you want to wrap multiple objects into
// one: struct Fixture {
//     Fixture(TestTester &tester) { /* ... */ }
//     ~Fixture() { /* ... */ }
// };
void* CreateOrFetchFixturePointer(Tester& tester,
                                  CreateFixturePointer create_fixture,
                                  DeleteFixturePointer delete_fixture);

template <typename Type>
Type& CreateOrFetchFixtureObject(Tester& tester) {
    auto ptr = CreateOrFetchFixturePointer(
        tester,
        [](Allocator& a, Tester& tester) { return (void*)a.New<Type>(tester); },
        [](void* ptr, Allocator& a) {
            auto t = (Type*)ptr;
            a.Delete(t);
        });
    return *(Type*)ptr;
}

} // namespace tests

#define REQUIRE_APPROX_EQ_HELPER(a, b, epsilon, level)                                                       \
    {                                                                                                        \
        auto x = a;                                                                                          \
        auto y = b;                                                                                          \
        const bool condition = ApproxEqual(x, y, epsilon);                                                   \
        tests::Check(                                                                                        \
            tester,                                                                                          \
            condition,                                                                                       \
            condition                                                                                        \
                ? MutableString {}                                                                           \
                : fmt::Format(tester.scratch_arena, "Expected: {} ~ {}\n          {} ~ {}", #a, #b, x, y),   \
            tests::FailureAction::level);                                                                    \
    }

#define REQUIRE_HELPER(a, b, op, level)                                                                      \
    {                                                                                                        \
        auto x = a;                                                                                          \
        auto y = b;                                                                                          \
        const bool condition = x op y;                                                                       \
        tests::Check(tester,                                                                                 \
                     condition,                                                                              \
                     condition ? MutableString {}                                                            \
                               : fmt::Format(tester.scratch_arena,                                           \
                                             "Expected: {} {} {}\n          {} {} {}",                       \
                                             #a,                                                             \
                                             #op,                                                            \
                                             #b,                                                             \
                                             x,                                                              \
                                             #op,                                                            \
                                             y),                                                             \
                     tests::FailureAction::level);                                                           \
        if constexpr (tests::FailureAction::level == tests::FailureAction::FailAndExitTest)                  \
            if (!condition) PanicIfReached();                                                                \
    }

#define REQUIRE(...)                                                                                         \
    tests::Check(tester, !!(__VA_ARGS__), #__VA_ARGS__, tests::FailureAction::FailAndExitTest)
#define REQUIRE_OP()                     REQUIRE_HELPER(a, p, op, FailAndExitTest)
#define REQUIRE_EQ(a, b)                 REQUIRE_HELPER(a, b, ==, FailAndExitTest)
#define REQUIRE_NEQ(a, b)                REQUIRE_HELPER(a, b, !=, FailAndExitTest)
#define REQUIRE_LT(a, b)                 REQUIRE_HELPER(a, b, <, FailAndExitTest)
#define REQUIRE_LTE(a, b)                REQUIRE_HELPER(a, b, <=, FailAndExitTest)
#define REQUIRE_GT(a, b)                 REQUIRE_HELPER(a, b, >, FailAndExitTest)
#define REQUIRE_GTE(a, b)                REQUIRE_HELPER(a, b, >=, FailAndExitTest)
#define REQUIRE_APPROX_EQ(a, b, epsilon) REQUIRE_APPROX_EQ_HELPER(a, b, epsilon, FailAndExitTest)
#define REQUIRE_UNWRAP(expr)                                                                                 \
    ({                                                                                                       \
        auto outcome = expr;                                                                                 \
        if (outcome.HasError())                                                                              \
            tests::Check(tester,                                                                             \
                         false,                                                                              \
                         fmt::Format(tester.scratch_arena, "ErrorCodeOr has an error: {}", outcome.Error()), \
                         tests::FailureAction::FailAndExitTest);                                             \
        outcome.ReleaseValue();                                                                              \
    })

#define CHECK(...)                     tests::Check(tester, !!(__VA_ARGS__), #__VA_ARGS__, tests::FailureAction::FailAndContinue)
#define CHECK_OP(a, op, b)             REQUIRE_HELPER(a, b, op, FailAndContinue)
#define CHECK_EQ(a, b)                 REQUIRE_HELPER(a, b, ==, FailAndContinue)
#define CHECK_NEQ(a, b)                REQUIRE_HELPER(a, b, !=, FailAndContinue)
#define CHECK_LT(a, b)                 REQUIRE_HELPER(a, b, <, FailAndContinue)
#define CHECK_LTE(a, b)                REQUIRE_HELPER(a, b, <=, FailAndContinue)
#define CHECK_GT(a, b)                 REQUIRE_HELPER(a, b, >, FailAndContinue)
#define CHECK_GTE(a, b)                REQUIRE_HELPER(a, b, >=, FailAndContinue)
#define CHECK_APPROX_EQ(a, b, epsilon) REQUIRE_APPROX_EQ_HELPER(a, b, epsilon, FailAndContinue)

#define CHECK_PANICS(...)                                                                                    \
    {                                                                                                        \
        auto initial_panic_handler = g_panic_handler;                                                        \
        g_panic_handler = [](const char*, SourceLocation) { throw "panicked"; };                             \
        bool panicked = false;                                                                               \
        try {                                                                                                \
            __VA_ARGS__;                                                                                     \
        } catch (...) {                                                                                      \
            panicked = true;                                                                                 \
        }                                                                                                    \
        g_panic_handler = initial_panic_handler;                                                             \
        tests::Check(tester, panicked, "Expected to panic", tests::FailureAction::FailAndContinue);          \
    }

#define TEST_FAILED(format, ...)                                                                             \
    tests::Check(tester,                                                                                     \
                 false,                                                                                      \
                 fmt::Format(tester.scratch_arena, format, ##__VA_ARGS__),                                   \
                 tests::FailureAction::FailAndExitTest)

#define LOG_WARNING(...)                                                                                     \
    tests::Check(tester,                                                                                     \
                 false,                                                                                      \
                 fmt::Format(tester.scratch_arena, __VA_ARGS__),                                             \
                 tests::FailureAction::LogWarningAndContinue)

// The name doesn't have to be a string literal, it any runtime string.
#define SUBCASE(name) if (auto CONCAT(subcase_, __COUNTER__) = tests::Subcase(tester, name))

// If you capture too much information (8kb at the moment), the output will be truncated at that threshold.
// This works as a stack though - when the capture goes out of scope the stack is popped.
#define CAPTURE_IMPL(string_name, value_to_capture)                                                          \
    DynamicArray<char> string_name {tester.capture_buffer};                                                  \
    fmt::Append(string_name, "  With {} := {}\n", #value_to_capture, value_to_capture);                      \
    string_name.ShrinkToFit()

#define CAPTURE(value_to_capture) CAPTURE_IMPL(CONCAT(capture_string_, __COUNTER__), value_to_capture)

#if !PRODUCTION_BUILD

#define REGISTER_TEST(func)     tests::RegisterTest(tester, func, #func)
#define TEST_REGISTRATION(name) void name(tests::Tester& tester)

// clang-format off
#define TEST_CASE(func) \
    static tests::Result func([[maybe_unused]] tests::Tester &tester)
// clang-format on
//
#define TEST_CASE2(func) static tests::Result func(tests::Tester& tester)

#else

#define REGISTER_TEST(func)
#define TEST_REGISTRATION(name)                                                                              \
    template <typename Unused>                                                                               \
    void name(tests::Tester&)

#define TEST_CASE(func) __attribute__((unused)) tests::Result func([[maybe_unused]] tests::Tester& tester)

#endif
