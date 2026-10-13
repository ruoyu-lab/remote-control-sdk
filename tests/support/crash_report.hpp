// Windows: an access violation in the test, on any thread, prints where it
// happened and the stack (function names and lines where the program's PDB is
// at hand) before anything handles it. Elsewhere it does nothing.
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <dbghelp.h>

#include <cstdio>

namespace rcsdk::test {

inline LONG WINAPI reportCrash(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::fprintf(stderr, "crash: exception 0x%08lX at %p on thread %lu, %s address %p\n",
                 exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress,
                 GetCurrentThreadId(), exception->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                 reinterpret_cast<void*>(exception->ExceptionRecord->ExceptionInformation[1]));
    std::fflush(stderr);
    HANDLE process = GetCurrentProcess();
    static const bool symbols = [&] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        return SymInitialize(process, nullptr, TRUE) != FALSE;
    }();
    (void)symbols;
    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame{};
#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int depth = 0;
         depth < 32 && StackWalk64(machine, process, GetCurrentThread(), &frame, &context, nullptr,
                                   SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++depth) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 offset = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD column = 0;
        const bool named = SymFromAddr(process, frame.AddrPC.Offset, &offset, symbol);
        const bool placed = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &column, &line);
        char module[MAX_PATH] = "?";
        HMODULE base = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frame.AddrPC.Offset), &base)) {
            GetModuleFileNameA(base, module, sizeof(module));
        }
        std::fprintf(stderr, "  #%d %s+0x%llx %s:%lu (%s)\n", depth, named ? symbol->Name : "?",
                     (unsigned long long)offset, placed ? line.FileName : "?", placed ? line.LineNumber : 0, module);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

// First: an access violation is reported where it happens, on whatever
// thread, before anything may swallow it.
inline void reportCrashes() {
    AddVectoredExceptionHandler(1, reportCrash);
}

} // namespace rcsdk::test
#else
namespace rcsdk::test {
inline void reportCrashes() {}
} // namespace rcsdk::test
#endif
