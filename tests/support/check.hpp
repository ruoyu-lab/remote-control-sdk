// Minimal assertion helpers: each test is a standalone executable.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition);                         \
            /* stdout and stderr only: flushing every stream waits on one another thread reads from. */             \
            std::fflush(stdout);                                                                                       \
            std::fflush(stderr);                                                                                       \
            std::_Exit(1); /* Not exit(): SDK threads may still run; static destructors must not race them. */       \
        }                                                                                                              \
    } while (false)

// True when `action` throws an exception of type E.
template <class E, class Action>
bool throws(Action&& action) {
    try {
        action();
    } catch (const E&) {
        return true;
    }
    return false;
}

// Runs `test`, reporting an escaping exception as a failure.
inline int runTest(const char* name, const std::function<void()>& test) {
    try {
        test();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: unexpected exception: %s\n", name, error.what());
        return 1;
    }
    std::printf("%s passed\n", name);
    return 0;
}
