// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include "common/arch.h"
#include "common/assert.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(ARCH_X86_64)
#define Crash() __asm__ __volatile__("int $3")
#elif defined(ARCH_ARM64)
#define Crash() __asm__ __volatile__("brk 0")
#else
#error "Missing Crash() implementation for target CPU architecture."
#endif

/// Appends the failed assertion to crash_report.txt in the working folder: the log is written
/// asynchronously and the process ends before the message reaches it. On Windows the stack is
/// given as offsets into shadPS4.exe (llvm-symbolizer --obj=shadPS4.exe 0x<offset>).
static void WriteAssertReport(const char* msg) {
    std::FILE* file = std::fopen("crash_report.txt", "a");
    if (!file) {
        return;
    }
    std::fprintf(file, "assertion failed: %s\n", msg ? msg : "(no message)");
#ifdef _WIN32
    const auto base = reinterpret_cast<unsigned long long>(GetModuleHandleW(nullptr));
    std::fprintf(file, "thread %lu, stack:", GetCurrentThreadId());
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        const auto frame = reinterpret_cast<unsigned long long>(frames[i]);
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(frames[i]), &module) &&
            reinterpret_cast<unsigned long long>(module) == base) {
            std::fprintf(file, " exe+%#llx", frame - base);
        } else {
            std::fprintf(file, " %#llx", frame);
        }
    }
    std::fprintf(file, "\n");
#endif
    std::fclose(file);
}

void assert_fail_impl() {
    WriteAssertReport(nullptr);
    Core::Signals::Instance()->RemoveHandlers();
    Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    Crash();
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    WriteAssertReport(msg);
    assert_fail_impl();
}
