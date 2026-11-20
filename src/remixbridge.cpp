/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified by TheAutomatic (2026) for AMD Radeon GPU compatibility (clean Vulkan
 * child swapchain, Win32 input/focus forwarding, SEH crash guard, and direct
 * OptiScaler.dll preloading). Upstream Apache-2.0 terms are preserved in
 * THIRD_PARTY_LICENSES/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt and NOTICE.
 * These modifications and the compiled binary are licensed under
 * PolyForm-Noncommercial-1.0.0 (see LICENSE): free for personal noncommercial
 * use; commercial resale and paid installation/setup services are prohibited.
 */

/*
 * RTX Remix JNI Bridge: remixbridge.dll
 *
 * Single-file JNI shim: loads d3d9.dll, calls remixapi_InitializeLibrary to
 * obtain the remixapi_Interface vtable, and exposes every Remix API method as
 * a JNI native function consumed by com.nvidia.rtxremix.RemixBridge.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <unknwn.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <malloc.h>
#include <jni.h>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// HACKHACK: link to OpenGL to expose 'glSemaphoreParameterui64vEXT',
//           reason: LWJGL does not expose that function
#include <gl/GL.h>
#pragma comment(lib, "opengl32.lib")

#include "remix_c.h"

// ════════════════════════════════════════════════════════════════════════════
// Crash dump: VEH that writes a minidump and a human-readable crash report.
//
// The minidump is for deep debugging when PDBs are available.
// The text report is immediately readable: exception info, registers,
// faulting module, loaded module list, and native stack walk for every
// thread, so you can triage a crash without loading the dump in VS.
//
// Both land in ./crash/ relative to the working directory (typically run/).
// ════════════════════════════════════════════════════════════════════════════

static const char* exceptionCodeName(DWORD code) {
    switch (code) {
        case 0xC0000005: return "EXCEPTION_ACCESS_VIOLATION";
        case 0xC00000FD: return "EXCEPTION_STACK_OVERFLOW";
        case 0xC000008C: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case 0xC000001D: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case 0xC0000094: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case 0xC0000096: return "EXCEPTION_PRIV_INSTRUCTION";
        case 0xC0000006: return "EXCEPTION_IN_PAGE_ERROR";
        case 0x80000001: return "EXCEPTION_GUARD_PAGE";
        case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
        case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (__fastfail)";
        default:         return "UNKNOWN";
    }
}

// Resolve an address to "module.dll+0xOFFSET" or "??? (JIT/heap)" if unmapped
static void formatAddress(FILE* f, uintptr_t addr) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        char name[MAX_PATH];
        if (GetModuleFileNameA(mod, name, sizeof(name))) {
            const char* base = strrchr(name, '\\');
            base = base ? base + 1 : name;
            fprintf(f, "%s+0x%llX", base, (unsigned long long)(addr - (uintptr_t)mod));
            return;
        }
    }
    fprintf(f, "0x%016llX (JIT/heap)", (unsigned long long)addr);
}

static void writeModuleList(FILE* f) {
    fprintf(f, "\n=== Loaded Modules ===\n");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            fprintf(f, "  %016llX - %016llX  %s\n",
                    (unsigned long long)(uintptr_t)me.modBaseAddr,
                    (unsigned long long)((uintptr_t)me.modBaseAddr + me.modBaseSize),
                    me.szModule);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
}

static void writeRegisters(FILE* f, const CONTEXT* ctx) {
    fprintf(f, "\n=== Registers ===\n");
#if defined(_M_X64)
    fprintf(f, "  RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX\n",
            ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    fprintf(f, "  RSI=%016llX  RDI=%016llX  RSP=%016llX  RBP=%016llX\n",
            ctx->Rsi, ctx->Rdi, ctx->Rsp, ctx->Rbp);
    fprintf(f, "  R8 =%016llX  R9 =%016llX  R10=%016llX  R11=%016llX\n",
            ctx->R8, ctx->R9, ctx->R10, ctx->R11);
    fprintf(f, "  R12=%016llX  R13=%016llX  R14=%016llX  R15=%016llX\n",
            ctx->R12, ctx->R13, ctx->R14, ctx->R15);
    fprintf(f, "  RIP=%016llX  EFLAGS=%08lX\n", ctx->Rip, ctx->EFlags);
#elif defined(_M_ARM64)
    for (int i = 0; i < 28; i += 4)
        fprintf(f, "  X%-2d=%016llX  X%-2d=%016llX  X%-2d=%016llX  X%-2d=%016llX\n",
                i, ctx->X[i], i+1, ctx->X[i+1], i+2, ctx->X[i+2], i+3, ctx->X[i+3]);
    fprintf(f, "  X28=%016llX\n", ctx->X[28]);
    fprintf(f, "  FP =%016llX  LR =%016llX  SP =%016llX  PC =%016llX\n",
            ctx->Fp, ctx->Lr, ctx->Sp, ctx->Pc);
    fprintf(f, "  CPSR=%08lX\n", ctx->Cpsr);
#endif
}

static void walkStack(FILE* f, HANDLE process, HANDLE thread, CONTEXT ctx) {
    STACKFRAME64 frame = {};
#if defined(_M_X64)
    DWORD machineType     = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
    DWORD machineType     = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = ctx.Pc;
    frame.AddrFrame.Offset = ctx.Fp;
    frame.AddrStack.Offset = ctx.Sp;
#endif
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Mode   = AddrModeFlat;

    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machineType, process, thread,
                         &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0) break;

        fprintf(f, "    [%2d] ", i);
        formatAddress(f, (uintptr_t)frame.AddrPC.Offset);
        fprintf(f, "\n");
    }
}

static void writeAllThreadStacks(FILE* f, DWORD faultingTid, EXCEPTION_POINTERS* ep) {
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);

    // Faulting thread first: use the exception context directly
    fprintf(f, "\n=== Faulting Thread (TID %lu) ===\n", faultingTid);
    walkStack(f, process, GetCurrentThread(), *ep->ContextRecord);

    fprintf(f, "\n=== Other Threads ===\n");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        DWORD pid = GetCurrentProcessId();
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                if (te.th32ThreadID == faultingTid) continue;

                HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                                       THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!th) continue;

                SuspendThread(th);
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &ctx)) {
                    fprintf(f, "\n  Thread %lu:\n", te.th32ThreadID);
                    walkStack(f, process, th, ctx);
                }
                ResumeThread(th);
                CloseHandle(th);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    SymCleanup(process);
}

// Set from Java through RemixBridge.setCrashDumpDetail; see writeCrashFiles.
static volatile bool g_crashDumpDetail = false;

// Writes both a .dmp and a .txt crash report for the given exception.
static void writeCrashFiles(EXCEPTION_POINTERS* ep) {
    static volatile LONG dumping = 0;
    if (InterlockedCompareExchange(&dumping, 1, 0) != 0)
        return;

    CreateDirectoryA("crash", nullptr);

    char tag[32];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);
    snprintf(tag, sizeof(tag), "%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    // ── Minidump ──
    char dmpPath[MAX_PATH];
    snprintf(dmpPath, sizeof(dmpPath), "crash\\minidump_%s.dmp", tag);

    HANDLE dmpFile = CreateFileA(dmpPath, GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dmpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;

        // MiniDumpWithIndirectlyReferencedMemory only on request: it copies
        // the memory that stack values point at, and the game's main thread
        // points at the launch arguments for the whole session, the
        // launcher's access token among them. Players attach these files to
        // public issues.
        DWORD flags = MiniDumpWithDataSegs
                    | MiniDumpWithHandleData
                    | MiniDumpWithThreadInfo
                    | MiniDumpWithUnloadedModules
                    | MiniDumpWithFullMemoryInfo;
        if (g_crashDumpDetail)
            flags |= MiniDumpWithIndirectlyReferencedMemory;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          dmpFile, (MINIDUMP_TYPE)flags, &mei, nullptr, nullptr);
        CloseHandle(dmpFile);
    }

    // ── Text crash report ──
    char txtPath[MAX_PATH];
    snprintf(txtPath, sizeof(txtPath), "crash\\crash_report_%s.txt", tag);

    DWORD code = ep->ExceptionRecord->ExceptionCode;

    FILE* f = fopen(txtPath, "w");
    if (f) {
        fprintf(f, "=== RTX Remix for Minecraft: Crash Report ===\n");
        fprintf(f, "Minidump: %s\n\n", dmpPath);

        fprintf(f, "Exception: 0x%08lX (%s)\n",
                code, exceptionCodeName(code));
        fprintf(f, "Address:   ");
        formatAddress(f, (uintptr_t)ep->ExceptionRecord->ExceptionAddress);
        fprintf(f, "\n");

        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            const char* op = ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading"
                           : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing"
                           : "executing";
            fprintf(f, "Fault:     %s 0x%016llX\n",
                    op, (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        }

        fprintf(f, "Thread:    %lu\n", GetCurrentThreadId());
        fprintf(f, "Process:   %lu\n", GetCurrentProcessId());

        writeRegisters(f, ep->ContextRecord);
        writeAllThreadStacks(f, GetCurrentThreadId(), ep);
        writeModuleList(f);

        fclose(f);
    }
}

// Unhandled Exception Filter: only fires when NO handler (including the
// JVM's own VEH for safepoint polls, implicit null checks, and stack
// overflow detection) could handle the exception. This is a genuine crash.
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

static LONG WINAPI crashExceptionFilter(EXCEPTION_POINTERS* ep) {
    writeCrashFiles(ep);

    if (g_prevFilter)
        return g_prevFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void installCrashHandler() {
    g_prevFilter = SetUnhandledExceptionFilter(crashExceptionFilter);
}

// The runtime starts Crashpad in CreateD3D9, after installCrashHandler, and
// Crashpad's filter then replaces this one. Restore the earlier filter only
// while this one is still installed, so Crashpad keeps catching faults in the
// guarded Shutdown and after it.
static void uninstallCrashHandler() {
    LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(g_prevFilter);
    if (current != crashExceptionFilter)
        SetUnhandledExceptionFilter(current);
    g_prevFilter = nullptr;
}

// ════════════════════════════════════════════════════════════════════════════
// Remix call guard: a fault under a Remix call is reported as unhandled.
//
// Remix calls run on Java threads. On x64, HotSpot's own SEH frames sit between
// a fault in the runtime and the unhandled-exception filter. HotSpot sees such
// a fault first, writes hs_err_pid*.log, and exits with code 1, so neither
// Crashpad's filter nor crashExceptionFilter would ever run. remixCall puts a
// __try frame between the runtime and HotSpot. Its filter runs only after
// every handler inside the runtime has declined the exception, and passes a
// fatal fault to the process's top-level filter as though nothing had handled
// it. While Crashpad is up that filter never returns: its handler process
// writes the dump and terminates the game with the exception code. If it does
// return, HotSpot reports the fault as it always has.
//
// Forwarding is limited to x64, the only architecture it has been checked on;
// elsewhere the guard leaves every fault to HotSpot, as before.
// RemixBridge.setRemixFaultForwarding(false) turns it off on x64 too, so
// HotSpot writes hs_err_pid*.log with the Java frames instead.
// ════════════════════════════════════════════════════════════════════════════

static volatile LONG g_forwardRemixFaults = 1;
static uintptr_t g_jvmImageBegin = 0;
static uintptr_t g_jvmImageEnd   = 0;

// Records where jvm.dll is mapped, so the filter can tell a HotSpot fault
// without asking the loader, whose lock the faulting thread may hold.
static void recordJvmImage() {
    HMODULE jvm = GetModuleHandleW(L"jvm.dll");
    if (!jvm) return;
    auto dos = (const IMAGE_DOS_HEADER*)jvm;
    auto nt  = (const IMAGE_NT_HEADERS*)((const uint8_t*)jvm + dos->e_lfanew);
    g_jvmImageBegin = (uintptr_t)jvm;
    g_jvmImageEnd   = g_jvmImageBegin + nt->OptionalHeader.SizeOfImage;
}

// Faults HotSpot can only report. A C++ exception counts because one that
// reaches the guard has escaped the runtime: any catch inside it would have
// ended the search first. Left to HotSpot: stack overflow and guard pages,
// since HotSpot owns a Java thread's guard zones, lets native code run on into
// the yellow zone, and a filter on an exhausted stack can fault itself;
// floating-point exceptions, which it recovers from by resetting MXCSR; and
// breakpoints.
static bool isFatalRemixFault(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case 0xE06D7363: // MSVC C++ exception
            return true;
        default:
            return false;
    }
}

static LONG remixFaultFilter(EXCEPTION_POINTERS* ep) {
#if !defined(_M_X64)
    (void)ep;
    return EXCEPTION_CONTINUE_SEARCH;
#else
    if (!g_forwardRemixFaults)
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* record = ep->ExceptionRecord;
    if (!isFatalRemixFault(record->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t pc = (uintptr_t)record->ExceptionAddress;
    if (pc >= g_jvmImageBegin && pc < g_jvmImageEnd)
        return EXCEPTION_CONTINUE_SEARCH;

    // An access inside this thread's own stack reservation means the stack
    // ran out, and the forwarded filter would need stack the thread lacks.
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        ULONG_PTR stackLow = 0, stackHigh = 0;
        GetCurrentThreadStackLimits(&stackLow, &stackHigh);
        const ULONG_PTR target = record->ExceptionInformation[1];
        if (target >= stackLow && target < stackHigh)
            return EXCEPTION_CONTINUE_SEARCH;
    }

    // An unhandled exception goes to a debugger, not the top-level filter.
    if (IsDebuggerPresent())
        return EXCEPTION_CONTINUE_SEARCH;

    // Forward once. A fault inside the forwarded filter, or a second thread
    // faulting meanwhile, goes to HotSpot.
    static volatile LONG forwarded = 0;
    if (InterlockedCompareExchange(&forwarded, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    // Windows has no getter for the top-level filter, only a setter that
    // returns the one it replaces.
    LPTOP_LEVEL_EXCEPTION_FILTER topLevel = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(topLevel);
    if (topLevel)
        topLevel(ep);

    // Whatever the forwarded filter answered, HotSpot still owns this thread
    // and is the one to end the process.
    return EXCEPTION_CONTINUE_SEARCH;
#endif
}

// Runs one call into the runtime under the guard. Keep the body to the call
// itself, so a fault in the bridge's own marshalling is not reported as one in
// the runtime. The filter never returns EXCEPTION_EXECUTE_HANDLER, so the
// __except block is unreachable.
template <typename Call>
static auto remixCall(Call&& call) -> decltype(call()) {
    __try {
        return call();
    } __except (remixFaultFilter(GetExceptionInformation())) {
    }
    return decltype(call())();
}

// Tracy profiling: resolve ___tracy_* symbols from d3d9.dll at runtime so
// Java zones appear on the same timeline as dxvk-remix zones. We can't use
// TRACY_IMPORTS because we link against the Windows SDK d3d9.lib which
// doesn't contain Tracy symbols.
struct TracyCZoneCtx { uint32_t id; int active; };

using FnAllocSrclocName = uint64_t(*)(uint32_t line, const char* source, size_t sourceSz,
                                       const char* function, size_t functionSz,
                                       const char* name, size_t nameSz);
using FnEmitZoneBeginAlloc = TracyCZoneCtx(*)(uint64_t srcloc, int active);
using FnEmitZoneEnd = void(*)(TracyCZoneCtx ctx);
using FnSetThreadName = void(*)(const char* name);
using FnEmitPlot = void(*)(const char* name, double value);
using FnEmitMessage = void(*)(const char* text, size_t size, int callstack);
using FnEmitFrameMark = void(*)(const char* name);

static FnAllocSrclocName    s_allocSrclocName;
static FnEmitZoneBeginAlloc s_emitZoneBeginAlloc;
static FnEmitZoneEnd        s_emitZoneEnd;
static FnSetThreadName      s_setThreadName;
static FnEmitPlot           s_emitPlot;
static FnEmitMessage        s_emitMessage;
static FnEmitFrameMark      s_emitFrameMark;
static std::mutex           s_tracyMutex;
static bool                 s_tracyResolved = false;
static bool                 s_tracyAvailable = false;
static bool                 s_tracyEnabled = false;

static void resetTracyFunctionsLocked() {
    s_allocSrclocName = nullptr;
    s_emitZoneBeginAlloc = nullptr;
    s_emitZoneEnd = nullptr;
    s_setThreadName = nullptr;
    s_emitPlot = nullptr;
    s_emitMessage = nullptr;
    s_emitFrameMark = nullptr;
    s_tracyResolved = false;
    s_tracyAvailable = false;
}

static bool initTracyFunctionsLocked() {
    if (!s_tracyEnabled) return false;
    if (s_tracyResolved) return s_tracyAvailable;

    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) return false;

    s_allocSrclocName    = (FnAllocSrclocName)   GetProcAddress(d3d9, "___tracy_alloc_srcloc_name");
    s_emitZoneBeginAlloc = (FnEmitZoneBeginAlloc)GetProcAddress(d3d9, "___tracy_emit_zone_begin_alloc");
    s_emitZoneEnd        = (FnEmitZoneEnd)       GetProcAddress(d3d9, "___tracy_emit_zone_end");
    s_setThreadName      = (FnSetThreadName)     GetProcAddress(d3d9, "___tracy_set_thread_name");
    s_emitPlot           = (FnEmitPlot)          GetProcAddress(d3d9, "___tracy_emit_plot");
    s_emitMessage        = (FnEmitMessage)       GetProcAddress(d3d9, "___tracy_emit_message");
    s_emitFrameMark      = (FnEmitFrameMark)     GetProcAddress(d3d9, "___tracy_emit_frame_mark");

    s_tracyAvailable = s_allocSrclocName && s_emitZoneBeginAlloc && s_emitZoneEnd;
    s_tracyResolved = true;
    return s_tracyAvailable;
}

// ════════════════════════════════════════════════════════════════════════════
// Global state: filled by initialize(), read by every other function.
// ════════════════════════════════════════════════════════════════════════════

static HMODULE          g_remixDll   = nullptr;
static remixapi_Interface g_remix    = {};
static bool             g_inited     = false;

// ════════════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════════════

static void fillSurface(JNIEnv* env, remixapi_MeshInfoSurfaceTriangles& surf,
                         jobject vertBuf, jint vertCount,
                         jobject idxBuf, jint idxCount,
                         jlong material,
                         jint flags,
                         jint bonesPerVertex,
                         jobject blendWeightBuf, jint blendWeightCount,
                         jobject blendIndexBuf, jint blendIndexCount)
{
    memset(&surf, 0, sizeof(surf));

    if (vertBuf) {
        surf.vertices_values = (const remixapi_HardcodedVertex*)
            env->GetDirectBufferAddress(vertBuf);
    }
    surf.vertices_count = (uint64_t)vertCount;

    if (idxBuf) {
        surf.indices_values = (const uint32_t*)
            env->GetDirectBufferAddress(idxBuf);
    }
    surf.indices_count = (uint64_t)idxCount;

    surf.material = (remixapi_MaterialHandle)(uintptr_t)material;
    surf.flags = (remixapi_MeshInfoSurfaceTrianglesFlags)flags;

    if (bonesPerVertex > 0 && blendWeightBuf && blendIndexBuf) {
        surf.skinning_hasvalue = 1;
        surf.skinning_value.bonesPerVertex = (uint32_t)bonesPerVertex;
        surf.skinning_value.blendWeights_values = (const float*)
            env->GetDirectBufferAddress(blendWeightBuf);
        surf.skinning_value.blendWeights_count = (uint32_t)blendWeightCount;
        surf.skinning_value.blendIndices_values = (const uint32_t*)
            env->GetDirectBufferAddress(blendIndexBuf);
        surf.skinning_value.blendIndices_count = (uint32_t)blendIndexCount;
    }
}

// Build an array of surfaces from parallel Java arrays.  Caller owns the
// returned pointer (stack or heap); for small counts we use alloca,
// otherwise heap.
#define MAX_STACK_SURFACES 16

static remixapi_MeshInfoSurfaceTriangles* buildSurfaces(
    JNIEnv* env, jint surfaceCount,
    jobjectArray vertexBuffers, jintArray vertexCounts,
    jobjectArray indexBuffers, jintArray indexCounts,
    jlongArray materialHandles,
    jintArray jFlags,
    jintArray jBonesPerVertex,
    jobjectArray blendWeightBuffers, jintArray blendWeightCounts,
    jobjectArray blendIndexBuffers, jintArray blendIndexCounts,
    remixapi_MeshInfoSurfaceTriangles* stackBuf)
{
    remixapi_MeshInfoSurfaceTriangles* surfs =
        (surfaceCount <= MAX_STACK_SURFACES) ? stackBuf
        : new remixapi_MeshInfoSurfaceTriangles[surfaceCount];

    jint*  vc  = env->GetIntArrayElements(vertexCounts, nullptr);
    jint*  ic  = env->GetIntArrayElements(indexCounts, nullptr);
    jlong* mh  = env->GetLongArrayElements(materialHandles, nullptr);
    jint*  flags = env->GetIntArrayElements(jFlags, nullptr);
    jint*  bpv = jBonesPerVertex ? env->GetIntArrayElements(jBonesPerVertex, nullptr) : nullptr;
    jint*  bwc = blendWeightCounts ? env->GetIntArrayElements(blendWeightCounts, nullptr) : nullptr;
    jint*  bic = blendIndexCounts ? env->GetIntArrayElements(blendIndexCounts, nullptr) : nullptr;

    for (jint i = 0; i < surfaceCount; i++) {
        jobject vb  = env->GetObjectArrayElement(vertexBuffers, i);
        jobject ib  = env->GetObjectArrayElement(indexBuffers, i);
        jobject bwb = blendWeightBuffers ? env->GetObjectArrayElement(blendWeightBuffers, i) : nullptr;
        jobject bib = blendIndexBuffers  ? env->GetObjectArrayElement(blendIndexBuffers, i)  : nullptr;

        fillSurface(env, surfs[i], vb, vc[i], ib, ic[i], mh[i], flags[i],
                     bpv ? bpv[i] : 0, bwb, bwc ? bwc[i] : 0, bib, bic ? bic[i] : 0);

        env->DeleteLocalRef(vb);
        env->DeleteLocalRef(ib);
        if (bwb) env->DeleteLocalRef(bwb);
        if (bib) env->DeleteLocalRef(bib);
    }

    env->ReleaseIntArrayElements(vertexCounts, vc, JNI_ABORT);
    env->ReleaseIntArrayElements(indexCounts, ic, JNI_ABORT);
    env->ReleaseLongArrayElements(materialHandles, mh, JNI_ABORT);
    env->ReleaseIntArrayElements(jFlags, flags, JNI_ABORT);
    if (bpv) env->ReleaseIntArrayElements(jBonesPerVertex, bpv, JNI_ABORT);
    if (bwc) env->ReleaseIntArrayElements(blendWeightCounts, bwc, JNI_ABORT);
    if (bic) env->ReleaseIntArrayElements(blendIndexCounts, bic, JNI_ABORT);

    return surfs;
}

static void freeSurfaces(remixapi_MeshInfoSurfaceTriangles* surfs,
                          remixapi_MeshInfoSurfaceTriangles* stackBuf) {
    if (surfs != stackBuf) delete[] surfs;
}


namespace {
template <typename T>
bool setJavaInstanceField(JNIEnv *env, jclass java_class,
                          jobject java_dstInstance, const char *java_fieldName,
                          T *c_srcValue) = delete;
template <>
bool setJavaInstanceField(JNIEnv *env, jclass java_class,
                          jobject java_dstInstance, const char *java_fieldName,
                          uint64_t *c_srcValue) {
  jfieldID fid = env->GetFieldID(java_class, java_fieldName, "J");
  if (fid == nullptr) {
    return false;
  }
  static_assert(sizeof(jlong) == sizeof(*c_srcValue), "Size mismatch");
  env->SetLongField(java_dstInstance, fid, *c_srcValue);
  return true;
}
template <>
bool setJavaInstanceField(JNIEnv *env, jclass java_class,
                          jobject java_dstInstance, const char *java_fieldName,
                          int32_t *c_srcValue) {
  jfieldID fid = env->GetFieldID(java_class, java_fieldName, "I");
  if (fid == nullptr) {
    return false;
  }
  static_assert(sizeof(jint) == sizeof(*c_srcValue), "Size mismatch");
  env->SetIntField(java_dstInstance, fid, (jint)*c_srcValue);
  return true;
}
template <>
bool setJavaInstanceField(JNIEnv *env, jclass java_class,
                          jobject java_dstInstance, const char *java_fieldName,
                          remixapi_TextureFormat *c_srcValue) {
  static_assert(sizeof(*c_srcValue) == sizeof(int32_t), "Size mismatch");
  return setJavaInstanceField<int32_t>(env, java_class, java_dstInstance,
                                       java_fieldName, (int32_t *)c_srcValue);
}
} // namespace


// ════════════════════════════════════════════════════════════════════════════
// AMD RX 9070 XT Clean HWND Presentation Target & Fallback
// ════════════════════════════════════════════════════════════════════════════
static HWND s_origHwnd = nullptr;
static HWND s_cleanHwnd = nullptr;
static ATOM s_cleanWndClass = 0;
static bool s_isChild = false;
static bool s_wasHostActive = false;
static bool s_logDirReady = false;
static std::mutex s_logMutex;

static void LogBridge(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(s_logMutex);
    OutputDebugStringA(buf);
    fprintf(stderr, "%s", buf);
    fflush(stderr);

    if (!s_logDirReady) {
        CreateDirectoryA("rtx-remix", nullptr);
        CreateDirectoryA("rtx-remix/logs", nullptr);
        s_logDirReady = true;
    }
    FILE* f = fopen("rtx-remix/logs/remix-bridge-amd.log", "a");
    if (f) {
        fprintf(f, "%s", buf);
        fclose(f);
    }
}

static LRESULT CALLBACK CleanWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && !s_isChild) {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETFOCUS:
        if (s_origHwnd && IsWindow(s_origHwnd)) {
            DWORD targetTid = GetWindowThreadProcessId(s_origHwnd, nullptr);
            if (targetTid == GetCurrentThreadId()) {
                SetFocus(s_origHwnd);
            }
        }
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND EnsureCleanWindow(HWND hostHwnd) {
    if (s_cleanHwnd && IsWindow(s_cleanHwnd)) {
        return s_cleanHwnd;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!s_cleanWndClass) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = CleanWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = nullptr;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"MinecraftRemixCleanPresentationWindow";

        s_cleanWndClass = RegisterClassExW(&wc);
        if (!s_cleanWndClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            LogBridge("[RemixBridge-AMD] RegisterClassExW failed: %lu\n", GetLastError());
            return nullptr;
        }
    }

    RECT rc = { 0, 0, 1280, 720 };
    if (hostHwnd && IsWindow(hostHwnd)) {
        GetClientRect(hostHwnd, &rc);
    }
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;

    HWND hwnd = nullptr;
    if (hostHwnd && IsWindow(hostHwnd)) {
        hwnd = CreateWindowExW(
            0,
            L"MinecraftRemixCleanPresentationWindow",
            L"Minecraft RTX Remix Presenter",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0,
            width, height,
            hostHwnd,
            nullptr, hInst, nullptr
        );
        s_isChild = (hwnd != nullptr);
    }

    if (!hwnd) {
        POINT pt = { 0, 0 };
        if (hostHwnd && IsWindow(hostHwnd)) {
            ClientToScreen(hostHwnd, &pt);
        }
        hwnd = CreateWindowExW(
            WS_EX_NOACTIVATE,
            L"MinecraftRemixCleanPresentationWindow",
            L"Minecraft RTX Remix Presenter",
            WS_POPUP | WS_VISIBLE,
            pt.x, pt.y,
            width, height,
            hostHwnd,
            nullptr, hInst, nullptr
        );
        s_isChild = false;
    }

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNA);
        UpdateWindow(hwnd);
        LogBridge("[RemixBridge-AMD] Created %s presentation HWND %p (%dx%d)\n",
                  s_isChild ? "WS_CHILD" : "WS_POPUP", hwnd, width, height);
    } else {
        LogBridge("[RemixBridge-AMD] CreateWindowExW failed: %lu\n", GetLastError());
    }

    s_cleanHwnd = hwnd;
    return hwnd;
}

// ════════════════════════════════════════════════════════════════════════════
// AMD Sparse Rendering Wave32 Vulkan Hook (isolated to d3d9.dll IAT)
//
// RTX Remix's isSparseRenderingSupported() requires VkPhysicalDeviceSubgroupProperties
// ::subgroupSize == 32, and its compact_active_pixels.comp.slang compute shader
// (LocalSize 32x32x1 + OpGroupNonUniformBallot) assumes Wave32 lane masks.
// On AMD RDNA GPUs reporting default subgroupSize=64 with subgroupSizeControl
// supporting Wave32, we patch ONLY d3d9.dll's vulkan-1.dll!vkGetInstanceProcAddr
// IAT slot so d3d9.dll sees subgroupSize=32, enables subgroupSizeControl +
// computeFullSubgroups cleanly without VUID conflicts (coexisting with
// OptiScaler VulkanExtensionSpoofing and OpenRR PrepareVkDevice), and forces
// requiredSubgroupSize=32 + REQUIRE_FULL_SUBGROUPS_BIT exclusively on the
// matched compact_active_pixels compute pipeline.
//
// Kill switch: set REMIX_AMD_SPARSE_WAVE32=0 to disable completely.
// ════════════════════════════════════════════════════════════════════════════
namespace amd_sparse_wave32 {

#define VK_HOOK_CALL __stdcall

using VkResult        = int32_t;
using VkBool32        = uint32_t;
using VkFlags         = uint32_t;
using VkStructureType = uint32_t;

struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
using VkInstance       = VkInstance_T*;
using VkPhysicalDevice = VkPhysicalDevice_T*;
using VkDevice         = VkDevice_T*;

using VkShaderModule   = uint64_t;
using VkPipelineCache  = uint64_t;
using VkPipelineLayout = uint64_t;
using VkPipeline       = uint64_t;

using PFN_vkVoidFunction = void (VK_HOOK_CALL *)(void);

static constexpr VkResult VK_SUCCESS_LOCAL                     = 0;
static constexpr VkResult VK_INCOMPLETE_LOCAL                  = 5;
static constexpr VkResult VK_ERROR_INITIALIZATION_FAILED_LOCAL = -3;
static constexpr VkBool32 VK_FALSE_LOCAL                       = 0u;
static constexpr VkBool32 VK_TRUE_LOCAL                        = 1u;

static constexpr uint32_t VK_SHADER_STAGE_COMPUTE_BIT_LOCAL = 0x00000020u;
static constexpr uint32_t VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_LOCAL = 0x00000001u;
static constexpr uint32_t VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_LOCAL      = 0x00000002u;

static constexpr VkStructureType VK_STYPE_PHYSICAL_DEVICE_FEATURES_2                         = 1000059000u;
static constexpr VkStructureType VK_STYPE_PHYSICAL_DEVICE_PROPERTIES_2                       = 1000059001u;
static constexpr VkStructureType VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES                = 1000094000u;
static constexpr VkStructureType VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES   = 1000225000u;
static constexpr VkStructureType VK_STYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CI    = 1000225001u;
static constexpr VkStructureType VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES     = 1000225002u;
static constexpr VkStructureType VK_STYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES                = 53u;
static constexpr VkStructureType VK_STYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES              = 54u;

static constexpr const char* kSubgroupSizeControlExtName = "VK_EXT_subgroup_size_control";
static constexpr int kMaxPNextDepth = 128;

struct VkBaseOutStructure {
    VkStructureType     sType;
    VkBaseOutStructure* pNext;
};

struct VkBaseInStructure {
    VkStructureType          sType;
    const VkBaseInStructure* pNext;
};

struct VkApplicationInfo {
    VkStructureType sType;
    const void*     pNext;
    const char*     pApplicationName;
    uint32_t        applicationVersion;
    const char*     pEngineName;
    uint32_t        engineVersion;
    uint32_t        apiVersion;
};

struct VkInstanceCreateInfo {
    VkStructureType          sType;
    const void*              pNext;
    VkFlags                  flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t                 enabledLayerCount;
    const char* const*       ppEnabledLayerNames;
    uint32_t                 enabledExtensionCount;
    const char* const*       ppEnabledExtensionNames;
};

struct VkPhysicalDeviceProperties2Local {
    VkStructureType sType;
    void*           pNext;
    uint32_t        apiVersion;
    uint32_t        driverVersion;
    uint32_t        vendorID;
    uint32_t        deviceID;
    uint32_t        deviceType;
    char            deviceName[256];
    uint8_t         pipelineCacheUUID[16];
    uint64_t        limitsAndSparsePadding[128]; // >= sizeof(VkPhysicalDeviceLimits + VkPhysicalDeviceSparseProperties)
};

struct VkPhysicalDeviceFeatures2Local {
    VkStructureType sType;
    void*           pNext;
    VkBool32        features[64]; // >= 55 VkBool32 fields in VkPhysicalDeviceFeatures
};

struct VkPhysicalDeviceSubgroupProperties {
    VkStructureType sType;
    void*           pNext;
    uint32_t        subgroupSize;
    VkFlags         supportedStages;
    VkFlags         supportedOperations;
    VkBool32        quadOperationsInAllStages;
};

struct VkPhysicalDeviceSubgroupSizeControlProperties {
    VkStructureType sType;
    void*           pNext;
    uint32_t        minSubgroupSize;
    uint32_t        maxSubgroupSize;
    uint32_t        maxComputeWorkgroupSubgroups;
    VkFlags         requiredSubgroupSizeStages;
};

struct VkPhysicalDeviceSubgroupSizeControlFeatures {
    VkStructureType sType;
    void*           pNext;
    VkBool32        subgroupSizeControl;
    VkBool32        computeFullSubgroups;
};

struct VkPhysicalDeviceVulkan13Features {
    VkStructureType sType;
    void*           pNext;
    VkBool32        robustImageAccess;
    VkBool32        inlineUniformBlock;
    VkBool32        descriptorBindingInlineUniformBlockUpdateAfterBind;
    VkBool32        pipelineCreationCacheControl;
    VkBool32        privateData;
    VkBool32        shaderDemoteToHelperInvocation;
    VkBool32        shaderTerminateInvocation;
    VkBool32        subgroupSizeControl;
    VkBool32        computeFullSubgroups;
    VkBool32        synchronization2;
    VkBool32        textureCompressionASTC_HDR;
    VkBool32        shaderZeroInitializeWorkgroupMemory;
    VkBool32        dynamicRendering;
    VkBool32        shaderIntegerDotProduct;
    VkBool32        maintenance4;
};

struct VkDeviceCreateInfo {
    VkStructureType    sType;
    const void*        pNext;
    VkFlags            flags;
    uint32_t           queueCreateInfoCount;
    const void*        pQueueCreateInfos;
    uint32_t           enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t           enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void*        pEnabledFeatures;
};

struct VkExtensionProperties {
    char     extensionName[256];
    uint32_t specVersion;
};

struct VkShaderModuleCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    size_t          codeSize;
    const uint32_t* pCode;
};

struct VkPipelineShaderStageCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        stage;
    VkShaderModule  module;
    const char*     pName;
    const void*     pSpecializationInfo;
};

struct VkPipelineShaderStageRequiredSubgroupSizeCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    uint32_t        requiredSubgroupSize;
};

struct VkComputePipelineCreateInfo {
    VkStructureType                 sType;
    const void*                     pNext;
    VkFlags                         flags;
    VkPipelineShaderStageCreateInfo stage;
    VkPipelineLayout                layout;
    VkPipeline                      basePipelineHandle;
    int32_t                         basePipelineIndex;
};

#if defined(_M_X64) || defined(__x86_64__)
static_assert(offsetof(VkPhysicalDeviceProperties2Local, apiVersion) == 16, "Properties2 layout");
static_assert(offsetof(VkPhysicalDeviceSubgroupProperties, subgroupSize) == 16, "SubgroupProperties layout");
static_assert(offsetof(VkPhysicalDeviceSubgroupSizeControlProperties, requiredSubgroupSizeStages) == 28, "SSCProps layout");
static_assert(offsetof(VkPhysicalDeviceSubgroupSizeControlFeatures, computeFullSubgroups) == 20, "SSCFeats layout");
static_assert(offsetof(VkPhysicalDeviceVulkan13Features, subgroupSizeControl) == 44, "Vk13Feats subgroupSizeControl offset");
static_assert(offsetof(VkPhysicalDeviceVulkan13Features, computeFullSubgroups) == 48, "Vk13Feats computeFullSubgroups offset");
static_assert(sizeof(VkDeviceCreateInfo) == 72, "VkDeviceCreateInfo size");
static_assert(sizeof(VkShaderModuleCreateInfo) == 40, "VkShaderModuleCreateInfo size");
static_assert(sizeof(VkPipelineShaderStageCreateInfo) == 48, "VkPipelineShaderStageCreateInfo size");
static_assert(sizeof(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo) == 24, "RequiredSubgroupSizeCI size");
static_assert(sizeof(VkComputePipelineCreateInfo) == 96, "VkComputePipelineCreateInfo size");
#endif

using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction (VK_HOOK_CALL *)(VkInstance, const char*);
using PFN_vkGetDeviceProcAddr   = PFN_vkVoidFunction (VK_HOOK_CALL *)(VkDevice, const char*);
using PFN_vkCreateInstance      = VkResult (VK_HOOK_CALL *)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance     = void (VK_HOOK_CALL *)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = VkResult (VK_HOOK_CALL *)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties2 = void (VK_HOOK_CALL *)(VkPhysicalDevice, void*);
using PFN_vkGetPhysicalDeviceFeatures2   = void (VK_HOOK_CALL *)(VkPhysicalDevice, void*);
using PFN_vkEnumerateDeviceExtensionProperties = VkResult (VK_HOOK_CALL *)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
using PFN_vkCreateDevice        = VkResult (VK_HOOK_CALL *)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
using PFN_vkDestroyDevice       = void (VK_HOOK_CALL *)(VkDevice, const void*);
using PFN_vkCreateShaderModule  = VkResult (VK_HOOK_CALL *)(VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*);
using PFN_vkDestroyShaderModule = void (VK_HOOK_CALL *)(VkDevice, VkShaderModule, const void*);
using PFN_vkCreateComputePipelines = VkResult (VK_HOOK_CALL *)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const void*, VkPipeline*);

struct InstanceDispatch {
    uint32_t                               apiVersion = 0;
    PFN_vkDestroyInstance                  nextDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices         nextEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties2     nextGetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceProperties2     nextGetPhysicalDeviceProperties2KHR = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2       nextGetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2       nextGetPhysicalDeviceFeatures2KHR = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties nextEnumerateDeviceExtensionProperties = nullptr;
    PFN_vkCreateDevice                     nextCreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr                nextGetDeviceProcAddr = nullptr;
};

struct EligiblePhysDev {
    bool     needsExt = false;
    uint32_t physDevApiVersion = 0;
    uint32_t effectiveApiVersion = 0;
};

struct EligibleDevice {
    PFN_vkGetDeviceProcAddr      nextGetDeviceProcAddr = nullptr;
    PFN_vkCreateShaderModule     nextCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule    nextDestroyShaderModule = nullptr;
    PFN_vkCreateComputePipelines nextCreateComputePipelines = nullptr;
    PFN_vkDestroyDevice          nextDestroyDevice = nullptr;
    std::unordered_set<VkShaderModule> matchedModules;
};

struct BoolRestoreGuard {
    VkBool32* ptrA = nullptr;
    VkBool32  origA = 0;
    VkBool32* ptrB = nullptr;
    VkBool32  origB = 0;
    void saveAndSet(VkBool32& a, VkBool32& b) {
        ptrA = &a; origA = a; a = VK_TRUE_LOCAL;
        ptrB = &b; origB = b; b = VK_TRUE_LOCAL;
    }
    ~BoolRestoreGuard() {
        if (ptrA) *ptrA = origA;
        if (ptrB) *ptrB = origB;
    }
};

static std::mutex g_mutex;
static HMODULE    g_patchedModule = nullptr;
static ULONG_PTR* g_patchedIatSlot = nullptr;
static PFN_vkGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
static PFN_vkCreateInstance      g_nextCreateInstance = nullptr;
static PFN_vkGetDeviceProcAddr   g_fallbackGetDeviceProcAddr = nullptr;
static PFN_vkCreateShaderModule  g_fallbackCreateShaderModule = nullptr;
static PFN_vkDestroyShaderModule g_fallbackDestroyShaderModule = nullptr;
static PFN_vkCreateComputePipelines g_fallbackCreateComputePipelines = nullptr;
static PFN_vkDestroyDevice       g_fallbackDestroyDevice = nullptr;
static VkInstance g_latestInstance = nullptr;

static std::unordered_map<VkInstance, InstanceDispatch>     g_instances;
static std::unordered_map<VkPhysicalDevice, VkInstance>     g_physDevToInstance;
static std::unordered_map<VkPhysicalDevice, EligiblePhysDev> g_eligiblePhysDevs;
static std::unordered_set<VkPhysicalDevice>                 g_ineligiblePhysDevs;
static std::unordered_map<VkDevice, EligibleDevice>         g_eligibleDevices;

static bool isVulkan13OrNewer(uint32_t apiVersion) {
    uint32_t major = (apiVersion >> 22) & 0x7Fu;
    uint32_t minor = (apiVersion >> 12) & 0x3FFu;
    return (major > 1u) || (major == 1u && minor >= 3u);
}

static bool isEnvEnabled() {
    char buf[64] = {};
    DWORD len = GetEnvironmentVariableA("REMIX_AMD_SPARSE_WAVE32", buf, (DWORD)sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) {
        return true; // default ON
    }
    if (strcmp(buf, "0") == 0 || _stricmp(buf, "false") == 0 || _stricmp(buf, "off") == 0) {
        LogBridge("[RemixBridge-AMD] Sparse Wave32 Vulkan hook disabled via REMIX_AMD_SPARSE_WAVE32=%s\n", buf);
        return false;
    }
    return true;
}

struct SpirvMatchResult {
    bool matched = false;
    bool hasBallot = false;
    bool hasKnownName = false;
};

// Safely scan SPIR-V words to identify RTX Remix's Wave32-dependent
// compact_active_pixels compute shader (GLCompute, LocalSize(32,32,1),
// plus OpGroupNonUniformBallot or known OpName debug symbols).
static SpirvMatchResult matchSparseCompactShader(const uint32_t* words, size_t codeSize) {
    SpirvMatchResult res;
    if (!words || codeSize < 20 || (codeSize % 4) != 0) {
        return res;
    }
    static constexpr uint32_t kSpvMagic = 0x07230203u;
    static constexpr uint16_t kOpName = 5u;
    static constexpr uint16_t kOpEntryPoint = 15u;
    static constexpr uint16_t kOpExecutionMode = 16u;
    static constexpr uint16_t kOpGroupNonUniformBallot = 339u;
    static constexpr uint32_t kExecModelGLCompute = 5u;
    static constexpr uint32_t kExecModeLocalSize = 17u;

    if (words[0] != kSpvMagic) {
        return res;
    }

    const size_t wordCount = codeSize / 4;
    std::vector<uint32_t> computeEntries;
    bool isGLCompute = false;
    bool hasLocalSize32x32x1 = false;

    size_t i = 5;
    while (i < wordCount) {
        uint32_t insn = words[i];
        uint16_t opcode = (uint16_t)(insn & 0xFFFFu);
        uint16_t len = (uint16_t)(insn >> 16);
        if (len == 0 || i + (size_t)len > wordCount) {
            return SpirvMatchResult{};
        }

        if (opcode == kOpEntryPoint && len >= 4) {
            if (words[i + 1] == kExecModelGLCompute) {
                isGLCompute = true;
                computeEntries.push_back(words[i + 2]);
            }
        } else if (opcode == kOpExecutionMode && len >= 6) {
            if (words[i + 2] == kExecModeLocalSize &&
                words[i + 3] == 32u &&
                words[i + 4] == 32u &&
                words[i + 5] == 1u) {
                uint32_t targetId = words[i + 1];
                for (uint32_t epId : computeEntries) {
                    if (epId == targetId) {
                        hasLocalSize32x32x1 = true;
                        break;
                    }
                }
            }
        } else if (opcode == kOpGroupNonUniformBallot) {
            res.hasBallot = true;
        } else if (opcode == kOpName && len >= 3 && !res.hasKnownName) {
            const char* raw = reinterpret_cast<const char*>(&words[i + 2]);
            size_t maxBytes = (size_t)(len - 2u) * 4u;
            size_t strLen = strnlen(raw, maxBytes);
            std::string_view sv(raw, strLen);
            if (sv.find("s_blockActiveMask") != std::string_view::npos ||
                sv.find("TraceRaysIndirectArgs") != std::string_view::npos) {
                res.hasKnownName = true;
            }
        }

        i += (size_t)len;
    }

    res.matched = isGLCompute && hasLocalSize32x32x1 && (res.hasBallot || res.hasKnownName);
    return res;
}

static bool hasDeviceExtension(PFN_vkEnumerateDeviceExtensionProperties enumExtFn,
                               VkPhysicalDevice physDev,
                               const char* targetExt) {
    if (!enumExtFn || !targetExt) return false;
    uint32_t count = 0;
    VkResult rc = enumExtFn(physDev, nullptr, &count, nullptr);
    if (rc != VK_SUCCESS_LOCAL || count == 0) return false;
    std::vector<VkExtensionProperties> exts(count);
    rc = enumExtFn(physDev, nullptr, &count, exts.data());
    if (rc != VK_SUCCESS_LOCAL && rc != VK_INCOMPLETE_LOCAL) return false;
    for (uint32_t i = 0; i < count; ++i) {
        exts[i].extensionName[255] = '\0';
        if (strcmp(exts[i].extensionName, targetExt) == 0) {
            return true;
        }
    }
    return false;
}

static InstanceDispatch* findInstanceForPhysDevLocked(VkPhysicalDevice physDev, VkInstance* outInst) {
    VkInstance inst = nullptr;
    auto mapIt = g_physDevToInstance.find(physDev);
    if (mapIt != g_physDevToInstance.end()) {
        inst = mapIt->second;
    } else {
        inst = g_latestInstance;
    }
    if (outInst) *outInst = inst;
    auto it = g_instances.find(inst);
    if (it != g_instances.end()) return &it->second;
    if (!g_instances.empty()) {
        if (outInst) *outInst = g_instances.begin()->first;
        return &g_instances.begin()->second;
    }
    return nullptr;
}

static void handleGetPhysicalDeviceProperties2(VkPhysicalDevice physDev, void* pProperties, bool isKhr) {
    PFN_vkGetPhysicalDeviceProperties2 nextProps2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2   nextFeats2 = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties nextEnumDevExt = nullptr;
    PFN_vkGetInstanceProcAddr nextGipa = nullptr;
    VkInstance inst = nullptr;
    uint32_t instApiVersion = 0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        nextGipa = g_nextGetInstanceProcAddr;
        if (InstanceDispatch* d = findInstanceForPhysDevLocked(physDev, &inst)) {
            nextProps2 = isKhr ? d->nextGetPhysicalDeviceProperties2KHR
                               : d->nextGetPhysicalDeviceProperties2;
            if (!nextProps2) {
                nextProps2 = d->nextGetPhysicalDeviceProperties2
                                 ? d->nextGetPhysicalDeviceProperties2
                                 : d->nextGetPhysicalDeviceProperties2KHR;
            }
            nextFeats2 = d->nextGetPhysicalDeviceFeatures2
                             ? d->nextGetPhysicalDeviceFeatures2
                             : d->nextGetPhysicalDeviceFeatures2KHR;
            nextEnumDevExt = d->nextEnumerateDeviceExtensionProperties;
            instApiVersion = d->apiVersion;
        }
    }

    if (!nextProps2) return;
    nextProps2(physDev, pProperties);
    if (!pProperties) return;

    auto* props2 = reinterpret_cast<VkPhysicalDeviceProperties2Local*>(pProperties);
    VkPhysicalDeviceSubgroupProperties* subgroupProps = nullptr;
    const VkPhysicalDeviceSubgroupSizeControlProperties* existingSscProps = nullptr;

    VkBaseOutStructure* cur = reinterpret_cast<VkBaseOutStructure*>(props2->pNext);
    for (int depth = 0; cur && depth < kMaxPNextDepth; cur = cur->pNext, ++depth) {
        if (cur->sType == VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
            subgroupProps = reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(cur);
        } else if (cur->sType == VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES ||
                   cur->sType == VK_STYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES) {
            // Note: VkPhysicalDeviceVulkan13Properties begins with the exact same 4
            // subgroupSizeControl fields at offsets +16..+28.
            existingSscProps = reinterpret_cast<const VkPhysicalDeviceSubgroupSizeControlProperties*>(cur);
        }
    }

    if (!subgroupProps) return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_eligiblePhysDevs.find(physDev) != g_eligiblePhysDevs.end()) {
            subgroupProps->subgroupSize = 32u;
            return;
        }
        if (g_ineligiblePhysDevs.find(physDev) != g_ineligiblePhysDevs.end()) {
            return;
        }
    }

    if (subgroupProps->subgroupSize != 64u) {
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            shouldLog = g_ineligiblePhysDevs.insert(physDev).second;
        }
        if (shouldLog) {
            LogBridge("[RemixBridge-AMD] Sparse Wave32: physDev=%p already reports subgroupSize=%u (no patch needed)\n",
                      (void*)physDev, subgroupProps->subgroupSize);
        }
        return;
    }

    // Lazily resolve helper instance procs outside our mutex if needed.
    if (nextGipa && inst) {
        if (!nextFeats2) {
            nextFeats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                nextGipa(inst, "vkGetPhysicalDeviceFeatures2"));
            if (!nextFeats2) {
                nextFeats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                    nextGipa(inst, "vkGetPhysicalDeviceFeatures2KHR"));
            }
        }
        if (!nextEnumDevExt) {
            nextEnumDevExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                nextGipa(inst, "vkEnumerateDeviceExtensionProperties"));
        }
    }

    VkPhysicalDeviceSubgroupSizeControlProperties sscProps = {};
    if (existingSscProps) {
        sscProps = *existingSscProps;
    } else {
        sscProps.sType = VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
        sscProps.pNext = nullptr;
        VkPhysicalDeviceProperties2Local localProps2 = {};
        localProps2.sType = VK_STYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        localProps2.pNext = &sscProps;
        nextProps2(physDev, &localProps2);
    }

    VkPhysicalDeviceSubgroupSizeControlFeatures sscFeats = {};
    if (nextFeats2) {
        sscFeats.sType = VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
        sscFeats.pNext = nullptr;
        VkPhysicalDeviceFeatures2Local localFeats2 = {};
        localFeats2.sType = VK_STYPE_PHYSICAL_DEVICE_FEATURES_2;
        localFeats2.pNext = &sscFeats;
        nextFeats2(physDev, &localFeats2);
    }

    const uint32_t physDevApiVersion = props2->apiVersion;
    const uint32_t effectiveApiVersion =
        (instApiVersion != 0 && instApiVersion < physDevApiVersion)
            ? instApiVersion
            : physDevApiVersion;
    const bool needsExt = !isVulkan13OrNewer(effectiveApiVersion);
    const bool extOk = !needsExt || hasDeviceExtension(nextEnumDevExt, physDev, kSubgroupSizeControlExtName);

    const bool propsOk =
        sscProps.minSubgroupSize > 0u &&
        sscProps.minSubgroupSize <= 32u &&
        sscProps.maxSubgroupSize >= 32u &&
        sscProps.maxComputeWorkgroupSubgroups >= 32u &&
        (sscProps.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT_LOCAL) != 0u;
    const bool featsOk =
        sscFeats.subgroupSizeControl != VK_FALSE_LOCAL &&
        sscFeats.computeFullSubgroups != VK_FALSE_LOCAL;

    if (propsOk && featsOk && extOk) {
        subgroupProps->subgroupSize = 32u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            EligiblePhysDev state;
            state.needsExt = needsExt;
            state.physDevApiVersion = physDevApiVersion;
            state.effectiveApiVersion = effectiveApiVersion;
            shouldLog = g_eligiblePhysDevs.emplace(physDev, state).second;
        }
        if (shouldLog) {
            LogBridge("[RemixBridge-AMD] Sparse Wave32: physDev=%p subgroupSize 64 -> 32 "
                      "(min=%u max=%u maxWorkgroupSubgroups=%u stages=0x%x ssc=%u full=%u api=0x%x needsExt=%d)\n",
                      (void*)physDev,
                      sscProps.minSubgroupSize,
                      sscProps.maxSubgroupSize,
                      sscProps.maxComputeWorkgroupSubgroups,
                      sscProps.requiredSubgroupSizeStages,
                      sscFeats.subgroupSizeControl,
                      sscFeats.computeFullSubgroups,
                      effectiveApiVersion,
                      needsExt ? 1 : 0);
        }
    } else {
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            shouldLog = g_ineligiblePhysDevs.insert(physDev).second;
        }
        if (shouldLog) {
            LogBridge("[RemixBridge-AMD] Sparse Wave32: skipped physDev=%p (subgroupSize=64, "
                      "min=%u max=%u maxWorkgroupSubgroups=%u stages=0x%x ssc=%u full=%u api=0x%x extOk=%d)\n",
                      (void*)physDev,
                      sscProps.minSubgroupSize,
                      sscProps.maxSubgroupSize,
                      sscProps.maxComputeWorkgroupSubgroups,
                      sscProps.requiredSubgroupSizeStages,
                      sscFeats.subgroupSizeControl,
                      sscFeats.computeFullSubgroups,
                      effectiveApiVersion,
                      extOk ? 1 : 0);
        }
    }
}

static void VK_HOOK_CALL Hook_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physDev, void* pProperties) {
    handleGetPhysicalDeviceProperties2(physDev, pProperties, false);
}

static void VK_HOOK_CALL Hook_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physDev, void* pProperties) {
    handleGetPhysicalDeviceProperties2(physDev, pProperties, true);
}

static VkResult VK_HOOK_CALL Hook_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const void* pAllocator,
    VkInstance* pInstance)
{
    PFN_vkCreateInstance nextFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        nextFn = g_nextCreateInstance;
    }
    if (!nextFn) return VK_ERROR_INITIALIZATION_FAILED_LOCAL;

    VkResult rc = nextFn(pCreateInfo, pAllocator, pInstance);
    if (rc == VK_SUCCESS_LOCAL && pInstance && *pInstance) {
        uint32_t apiVersion = 0x00400000u; // VK_API_VERSION_1_0 default
        if (pCreateInfo && pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion != 0) {
            apiVersion = pCreateInfo->pApplicationInfo->apiVersion;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[*pInstance].apiVersion = apiVersion;
        g_latestInstance = *pInstance;
    }
    return rc;
}

static void VK_HOOK_CALL Hook_vkDestroyInstance(VkInstance instance, const void* pAllocator) {
    PFN_vkDestroyInstance nextFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) {
            nextFn = it->second.nextDestroyInstance;
            g_instances.erase(it);
        }
        for (auto pit = g_physDevToInstance.begin(); pit != g_physDevToInstance.end(); ) {
            if (pit->second == instance) {
                g_eligiblePhysDevs.erase(pit->first);
                g_ineligiblePhysDevs.erase(pit->first);
                pit = g_physDevToInstance.erase(pit);
            } else {
                ++pit;
            }
        }
        if (g_latestInstance == instance) {
            g_latestInstance = g_instances.empty() ? nullptr : g_instances.begin()->first;
        }
    }
    if (nextFn) nextFn(instance, pAllocator);
}

static VkResult VK_HOOK_CALL Hook_vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices)
{
    PFN_vkEnumeratePhysicalDevices nextFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) {
            nextFn = it->second.nextEnumeratePhysicalDevices;
        }
    }
    if (!nextFn) return VK_ERROR_INITIALIZATION_FAILED_LOCAL;

    VkResult rc = nextFn(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if ((rc == VK_SUCCESS_LOCAL || rc == VK_INCOMPLETE_LOCAL) &&
        pPhysicalDeviceCount && pPhysicalDevices) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i) {
            if (pPhysicalDevices[i]) {
                g_physDevToInstance[pPhysicalDevices[i]] = instance;
            }
        }
    }
    return rc;
}

static VkResult VK_HOOK_CALL Hook_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const void* pAllocator,
    VkDevice* pDevice)
{
    PFN_vkCreateDevice nextCreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr nextGDPA = nullptr;
    PFN_vkGetInstanceProcAddr nextGipa = nullptr;
    VkInstance inst = nullptr;
    bool isEligible = false;
    EligiblePhysDev physState = {};

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        nextGipa = g_nextGetInstanceProcAddr;
        if (InstanceDispatch* d = findInstanceForPhysDevLocked(physicalDevice, &inst)) {
            nextCreateDevice = d->nextCreateDevice;
            nextGDPA = d->nextGetDeviceProcAddr;
        }
        auto eit = g_eligiblePhysDevs.find(physicalDevice);
        if (eit != g_eligiblePhysDevs.end()) {
            isEligible = true;
            physState = eit->second;
        }
    }

    if (!nextCreateDevice) return VK_ERROR_INITIALIZATION_FAILED_LOCAL;
    if (!isEligible || !pCreateInfo) {
        return nextCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    VkPhysicalDeviceVulkan13Features* existingVk13 = nullptr;
    VkPhysicalDeviceSubgroupSizeControlFeatures* existingSsc = nullptr;

    const VkBaseInStructure* cur = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
    for (int depth = 0; cur && depth < kMaxPNextDepth; cur = cur->pNext, ++depth) {
        if (cur->sType == VK_STYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            existingVk13 = const_cast<VkPhysicalDeviceVulkan13Features*>(
                reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(cur));
        } else if (cur->sType == VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES) {
            existingSsc = const_cast<VkPhysicalDeviceSubgroupSizeControlFeatures*>(
                reinterpret_cast<const VkPhysicalDeviceSubgroupSizeControlFeatures*>(cur));
        }
    }

    VkDeviceCreateInfo modifiedCI = *pCreateInfo;
    VkPhysicalDeviceSubgroupSizeControlFeatures localSsc = {};
    BoolRestoreGuard restoreGuard;
    const char* mode = "unknown";

    if (existingVk13) {
        // Vulkan 1.3 VUID: never chain VkPhysicalDeviceSubgroupSizeControlFeatures
        // alongside VkPhysicalDeviceVulkan13Features in the same pNext.
        if (existingVk13->subgroupSizeControl == VK_FALSE_LOCAL ||
            existingVk13->computeFullSubgroups == VK_FALSE_LOCAL) {
            restoreGuard.saveAndSet(existingVk13->subgroupSizeControl, existingVk13->computeFullSubgroups);
            mode = "existing-vk13-enabled";
        } else {
            mode = "existing-vk13-already-on";
        }
    } else if (existingSsc) {
        // Avoid duplicating sType = 1000225002.
        if (existingSsc->subgroupSizeControl == VK_FALSE_LOCAL ||
            existingSsc->computeFullSubgroups == VK_FALSE_LOCAL) {
            restoreGuard.saveAndSet(existingSsc->subgroupSizeControl, existingSsc->computeFullSubgroups);
            mode = "existing-ssc-enabled";
        } else {
            mode = "existing-ssc-already-on";
        }
    } else {
        localSsc.sType = VK_STYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
        localSsc.pNext = const_cast<void*>(modifiedCI.pNext);
        localSsc.subgroupSizeControl = VK_TRUE_LOCAL;
        localSsc.computeFullSubgroups = VK_TRUE_LOCAL;
        modifiedCI.pNext = &localSsc;
        mode = "chained-ssc";
    }

    std::vector<const char*> extensions;
    bool addedExt = false;
    if (physState.needsExt && !existingVk13) {
        bool hasExt = false;
        if (modifiedCI.ppEnabledExtensionNames) {
            for (uint32_t i = 0; i < modifiedCI.enabledExtensionCount; ++i) {
                if (modifiedCI.ppEnabledExtensionNames[i] &&
                    strcmp(modifiedCI.ppEnabledExtensionNames[i], kSubgroupSizeControlExtName) == 0) {
                    hasExt = true;
                    break;
                }
            }
        }
        if (!hasExt) {
            extensions.reserve((size_t)modifiedCI.enabledExtensionCount + 1u);
            for (uint32_t i = 0; i < modifiedCI.enabledExtensionCount; ++i) {
                extensions.push_back(modifiedCI.ppEnabledExtensionNames[i]);
            }
            extensions.push_back(kSubgroupSizeControlExtName);
            modifiedCI.enabledExtensionCount = (uint32_t)extensions.size();
            modifiedCI.ppEnabledExtensionNames = extensions.data();
            addedExt = true;
        }
    }

    VkResult rc = nextCreateDevice(physicalDevice, &modifiedCI, pAllocator, pDevice);

    if (rc == VK_SUCCESS_LOCAL && pDevice && *pDevice) {
        if (!nextGDPA && nextGipa && inst) {
            nextGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                nextGipa(inst, "vkGetDeviceProcAddr"));
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!nextGDPA) nextGDPA = g_fallbackGetDeviceProcAddr;
            g_eligibleDevices[*pDevice].nextGetDeviceProcAddr = nextGDPA;
        }
        LogBridge("[RemixBridge-AMD] Sparse Wave32: vkCreateDevice ok (physDev=%p -> device=%p, mode=%s, addedExt=%d)\n",
                  (void*)physicalDevice, (void*)*pDevice, mode, addedExt ? 1 : 0);
    } else {
        LogBridge("[RemixBridge-AMD] Sparse Wave32: vkCreateDevice failed (physDev=%p, mode=%s, addedExt=%d, VkResult=%d)\n",
                  (void*)physicalDevice, mode, addedExt ? 1 : 0, (int)rc);
    }

    return rc;
}

static VkResult VK_HOOK_CALL Hook_vkCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const void* pAllocator,
    VkShaderModule* pShaderModule)
{
    PFN_vkCreateShaderModule nextFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_eligibleDevices.find(device);
        if (it != g_eligibleDevices.end()) {
            nextFn = it->second.nextCreateShaderModule;
        }
        if (!nextFn) nextFn = g_fallbackCreateShaderModule;
    }
    if (!nextFn) return VK_ERROR_INITIALIZATION_FAILED_LOCAL;

    VkResult rc = nextFn(device, pCreateInfo, pAllocator, pShaderModule);
    if (rc == VK_SUCCESS_LOCAL && pShaderModule && *pShaderModule != 0 &&
        pCreateInfo && pCreateInfo->pCode) {
        SpirvMatchResult match = matchSparseCompactShader(pCreateInfo->pCode, pCreateInfo->codeSize);
        if (match.matched) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_eligibleDevices.find(device);
                if (it != g_eligibleDevices.end()) {
                    it->second.matchedModules.insert(*pShaderModule);
                }
            }
            LogBridge("[RemixBridge-AMD] Sparse Wave32: matched compact compute shader module=0x%llx "
                      "(codeSize=%zu, ballot=%d, knownName=%d)\n",
                      (unsigned long long)*pShaderModule,
                      pCreateInfo->codeSize,
                      match.hasBallot ? 1 : 0,
                      match.hasKnownName ? 1 : 0);
        } else {
            // Defend against driver handle reuse if a prior matched handle was ever destroyed untracked.
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_eligibleDevices.find(device);
            if (it != g_eligibleDevices.end() && !it->second.matchedModules.empty()) {
                it->second.matchedModules.erase(*pShaderModule);
            }
        }
    }
    return rc;
}

static void VK_HOOK_CALL Hook_vkDestroyShaderModule(
    VkDevice device,
    VkShaderModule shaderModule,
    const void* pAllocator)
{
    PFN_vkDestroyShaderModule nextFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_eligibleDevices.find(device);
        if (it != g_eligibleDevices.end()) {
            nextFn = it->second.nextDestroyShaderModule;
            if (shaderModule != 0) {
                it->second.matchedModules.erase(shaderModule);
            }
        }
        if (!nextFn) nextFn = g_fallbackDestroyShaderModule;
    }
    if (nextFn) nextFn(device, shaderModule, pAllocator);
}

static VkResult VK_HOOK_CALL Hook_vkCreateComputePipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos,
    const void* pAllocator,
    VkPipeline* pPipelines)
{
    PFN_vkCreateComputePipelines nextFn = nullptr;
    std::vector<uint32_t> matchedIndices;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_eligibleDevices.find(device);
        if (it != g_eligibleDevices.end()) {
            nextFn = it->second.nextCreateComputePipelines;
            if (pCreateInfos && createInfoCount > 0 && !it->second.matchedModules.empty()) {
                for (uint32_t i = 0; i < createInfoCount; ++i) {
                    if (pCreateInfos[i].stage.stage == VK_SHADER_STAGE_COMPUTE_BIT_LOCAL &&
                        pCreateInfos[i].stage.module != 0 &&
                        it->second.matchedModules.count(pCreateInfos[i].stage.module) > 0) {
                        matchedIndices.push_back(i);
                    }
                }
            }
        }
        if (!nextFn) nextFn = g_fallbackCreateComputePipelines;
    }
    if (!nextFn) return VK_ERROR_INITIALIZATION_FAILED_LOCAL;

    if (matchedIndices.empty()) {
        return nextFn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    }

    std::vector<VkComputePipelineCreateInfo> clonedInfos(pCreateInfos, pCreateInfos + createInfoCount);
    std::vector<VkPipelineShaderStageRequiredSubgroupSizeCreateInfo> reqSubgroupInfos;
    // Reserve upfront so vector backing store never reallocates and pNext pointers remain valid.
    reqSubgroupInfos.reserve(matchedIndices.size());

    std::vector<bool> chainedFlag(matchedIndices.size(), false);
    for (size_t k = 0; k < matchedIndices.size(); ++k) {
        auto& stage = clonedInfos[matchedIndices[k]].stage;
        bool alreadyHasReq = false;
        const VkBaseInStructure* cur = reinterpret_cast<const VkBaseInStructure*>(stage.pNext);
        for (int depth = 0; cur && depth < kMaxPNextDepth; cur = cur->pNext, ++depth) {
            if (cur->sType == VK_STYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CI) {
                alreadyHasReq = true;
                break;
            }
        }
        if (!alreadyHasReq) {
            // VUID-VkPipelineShaderStageCreateInfo-flags-02755: REQUIRE_FULL_SUBGROUPS_BIT
            // and ALLOW_VARYING_SUBGROUP_SIZE_BIT cannot both be set with requiredSubgroupSize.
            stage.flags = (stage.flags & ~VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_LOCAL) |
                          VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_LOCAL;
            VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req = {};
            req.sType = VK_STYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CI;
            req.pNext = stage.pNext;
            req.requiredSubgroupSize = 32u;
            reqSubgroupInfos.push_back(req);
            stage.pNext = &reqSubgroupInfos.back();
            chainedFlag[k] = true;
        }
    }

    VkResult rc = nextFn(device, pipelineCache, createInfoCount, clonedInfos.data(), pAllocator, pPipelines);

    for (size_t k = 0; k < matchedIndices.size(); ++k) {
        const uint32_t idx = matchedIndices[k];
        const auto& stage = clonedInfos[idx].stage;
        LogBridge("[RemixBridge-AMD] Sparse Wave32: vkCreateComputePipelines idx=%u/%u module=0x%llx "
                  "entry=%s patched=%d requiredSubgroupSize=32 flags=0x%x -> VkResult=%d\n",
                  idx, createInfoCount,
                  (unsigned long long)stage.module,
                  stage.pName ? stage.pName : "<null>",
                  chainedFlag[k] ? 1 : 0,
                  stage.flags,
                  (int)rc);
    }

    return rc;
}

static void VK_HOOK_CALL Hook_vkDestroyDevice(VkDevice device, const void* pAllocator) {
    PFN_vkDestroyDevice nextFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_eligibleDevices.find(device);
        if (it != g_eligibleDevices.end()) {
            nextFn = it->second.nextDestroyDevice;
            g_eligibleDevices.erase(it);
        }
        if (!nextFn) nextFn = g_fallbackDestroyDevice;
    }
    if (nextFn) nextFn(device, pAllocator);
}

static PFN_vkVoidFunction VK_HOOK_CALL Hook_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    PFN_vkGetDeviceProcAddr nextGDPA = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_eligibleDevices.find(device);
        if (it != g_eligibleDevices.end() && it->second.nextGetDeviceProcAddr) {
            nextGDPA = it->second.nextGetDeviceProcAddr;
        } else {
            nextGDPA = g_fallbackGetDeviceProcAddr;
        }
    }
    if (!nextGDPA) return nullptr;

    PFN_vkVoidFunction nextFn = nextGDPA(device, pName);
    if (!nextFn || !pName) return nextFn;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_eligibleDevices.find(device);
    if (it == g_eligibleDevices.end()) {
        // Non-eligible device (e.g., NVIDIA GPU): zero overhead pass-through.
        return nextFn;
    }

    if (strcmp(pName, "vkCreateShaderModule") == 0) {
        it->second.nextCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(nextFn);
        g_fallbackCreateShaderModule = it->second.nextCreateShaderModule;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateShaderModule);
    }
    if (strcmp(pName, "vkDestroyShaderModule") == 0) {
        it->second.nextDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(nextFn);
        g_fallbackDestroyShaderModule = it->second.nextDestroyShaderModule;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyShaderModule);
    }
    if (strcmp(pName, "vkCreateComputePipelines") == 0) {
        it->second.nextCreateComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(nextFn);
        g_fallbackCreateComputePipelines = it->second.nextCreateComputePipelines;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateComputePipelines);
    }
    if (strcmp(pName, "vkDestroyDevice") == 0) {
        it->second.nextDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(nextFn);
        g_fallbackDestroyDevice = it->second.nextDestroyDevice;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyDevice);
    }
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkGetDeviceProcAddr);
    }
    return nextFn;
}

static PFN_vkVoidFunction VK_HOOK_CALL Hook_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    PFN_vkGetInstanceProcAddr nextGipa = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        nextGipa = g_nextGetInstanceProcAddr;
    }
    if (!nextGipa) return nullptr;

    PFN_vkVoidFunction nextFn = nextGipa(instance, pName);
    if (!nextFn || !pName) return nextFn;

    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkGetInstanceProcAddr);
    }
    if (strcmp(pName, "vkCreateInstance") == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_nextCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(nextFn);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateInstance);
    }
    if (strcmp(pName, "vkDestroyInstance") == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[instance].nextDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(nextFn);
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkDestroyInstance);
    }
    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[instance].nextEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(nextFn);
        if (instance) g_latestInstance = instance;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkEnumeratePhysicalDevices);
    }
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0) {
        PFN_vkGetPhysicalDeviceFeatures2 f2 = nullptr;
        PFN_vkGetPhysicalDeviceFeatures2 f2khr = nullptr;
        PFN_vkEnumerateDeviceExtensionProperties enumExt = nullptr;
        if (instance) {
            f2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(nextGipa(instance, "vkGetPhysicalDeviceFeatures2"));
            if (!f2) {
                f2khr = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(nextGipa(instance, "vkGetPhysicalDeviceFeatures2KHR"));
            }
            enumExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(nextGipa(instance, "vkEnumerateDeviceExtensionProperties"));
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& d = g_instances[instance];
        d.nextGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(nextFn);
        if (f2) d.nextGetPhysicalDeviceFeatures2 = f2;
        if (f2khr) d.nextGetPhysicalDeviceFeatures2KHR = f2khr;
        if (enumExt) d.nextEnumerateDeviceExtensionProperties = enumExt;
        if (instance) g_latestInstance = instance;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkGetPhysicalDeviceProperties2);
    }
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0) {
        PFN_vkGetPhysicalDeviceFeatures2 f2 = nullptr;
        PFN_vkGetPhysicalDeviceFeatures2 f2khr = nullptr;
        PFN_vkEnumerateDeviceExtensionProperties enumExt = nullptr;
        if (instance) {
            f2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(nextGipa(instance, "vkGetPhysicalDeviceFeatures2"));
            if (!f2) {
                f2khr = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(nextGipa(instance, "vkGetPhysicalDeviceFeatures2KHR"));
            }
            enumExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(nextGipa(instance, "vkEnumerateDeviceExtensionProperties"));
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& d = g_instances[instance];
        d.nextGetPhysicalDeviceProperties2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(nextFn);
        if (f2) d.nextGetPhysicalDeviceFeatures2 = f2;
        if (f2khr) d.nextGetPhysicalDeviceFeatures2KHR = f2khr;
        if (enumExt) d.nextEnumerateDeviceExtensionProperties = enumExt;
        if (instance) g_latestInstance = instance;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkGetPhysicalDeviceProperties2KHR);
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[instance].nextGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(nextFn);
        return nextFn;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[instance].nextGetPhysicalDeviceFeatures2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(nextFn);
        return nextFn;
    }
    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[instance].nextEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(nextFn);
        return nextFn;
    }
    if (strcmp(pName, "vkCreateDevice") == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[instance].nextCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(nextFn);
        if (instance) g_latestInstance = instance;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateDevice);
    }
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto fn = reinterpret_cast<PFN_vkGetDeviceProcAddr>(nextFn);
        g_instances[instance].nextGetDeviceProcAddr = fn;
        g_fallbackGetDeviceProcAddr = fn;
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkGetDeviceProcAddr);
    }

    return nextFn;
}

static void resetStateLocked() {
    g_patchedModule = nullptr;
    g_patchedIatSlot = nullptr;
    g_nextGetInstanceProcAddr = nullptr;
    g_nextCreateInstance = nullptr;
    g_fallbackGetDeviceProcAddr = nullptr;
    g_fallbackCreateShaderModule = nullptr;
    g_fallbackDestroyShaderModule = nullptr;
    g_fallbackCreateComputePipelines = nullptr;
    g_fallbackDestroyDevice = nullptr;
    g_latestInstance = nullptr;
    g_instances.clear();
    g_physDevToInstance.clear();
    g_eligiblePhysDevs.clear();
    g_ineligiblePhysDevs.clear();
    g_eligibleDevices.clear();
}

static ULONG_PTR* findModuleIatSlot(HMODULE mod, const char* targetDll, const char* targetFunc) {
    if (!mod || !targetDll || !targetFunc) return nullptr;
    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_DATA_DIRECTORY& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0 || impDir.Size == 0) return nullptr;

    auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dllName, targetDll) != 0) continue;
        if (desc->OriginalFirstThunk == 0 || desc->FirstThunk == 0) continue;

        auto* origThunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), targetFunc) != 0) continue;

            return reinterpret_cast<ULONG_PTR*>(&iatThunk->u1.Function);
        }
    }
    return nullptr;
}

static void install(HMODULE remixDll) {
    if (!remixDll) return;
    if (!isEnvEnabled()) return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_patchedModule == remixDll && g_patchedIatSlot != nullptr) {
            return;
        }
        resetStateLocked();
    }

    ULONG_PTR* slot = findModuleIatSlot(remixDll, "vulkan-1.dll", "vkGetInstanceProcAddr");
    if (!slot) {
        LogBridge("[RemixBridge-AMD] Sparse Wave32: d3d9.dll IAT vulkan-1.dll!vkGetInstanceProcAddr not found (skipped)\n");
        return;
    }

    void* origFn = reinterpret_cast<void*>((uintptr_t)*slot);
    if (!origFn || origFn == reinterpret_cast<void*>(&Hook_vkGetInstanceProcAddr)) {
        return;
    }

    // Publish next pointer before writing IAT slot so no concurrent caller can see null.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_patchedModule = remixDll;
        g_patchedIatSlot = slot;
        g_nextGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(origFn);
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProt)) {
        DWORD err = GetLastError();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            resetStateLocked();
        }
        LogBridge("[RemixBridge-AMD] Sparse Wave32: VirtualProtect on d3d9.dll IAT failed (err=%lu)\n", err);
        return;
    }

    *slot = (ULONG_PTR)&Hook_vkGetInstanceProcAddr;
    DWORD dummyProt = 0;
    VirtualProtect(slot, sizeof(*slot), oldProt, &dummyProt);

    LogBridge("[RemixBridge-AMD] Sparse Wave32: patched d3d9.dll IAT vulkan-1.dll!vkGetInstanceProcAddr (%p -> %p)\n",
              origFn, (void*)&Hook_vkGetInstanceProcAddr);
}

static void uninstall(HMODULE remixDll) {
    ULONG_PTR* slot = nullptr;
    PFN_vkGetInstanceProcAddr origFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (remixDll && g_patchedModule == remixDll && g_patchedIatSlot && g_nextGetInstanceProcAddr) {
            slot = g_patchedIatSlot;
            origFn = g_nextGetInstanceProcAddr;
        }
        resetStateLocked();
    }
    if (slot && origFn) {
        DWORD oldProt = 0;
        if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProt)) {
            if (*slot == (ULONG_PTR)&Hook_vkGetInstanceProcAddr) {
                *slot = (ULONG_PTR)origFn;
            }
            DWORD dummyProt = 0;
            VirtualProtect(slot, sizeof(*slot), oldProt, &dummyProt);
        }
    }
}

#undef VK_HOOK_CALL

} // namespace amd_sparse_wave32

static jint initializeRuntime(const wchar_t* dllPath)
{
    // Extract parent directory for SetDllDirectoryW
    wchar_t parentDir[MAX_PATH];
    wcscpy_s(parentDir, dllPath);
    wchar_t* lastSlash = wcsrchr(parentDir, L'\\');
    if (!lastSlash) lastSlash = wcsrchr(parentDir, L'/');
    if (lastSlash) *lastSlash = L'\0';
    else parentDir[0] = L'\0';

    if (parentDir[0] != L'\0') {
        SetDllDirectoryW(parentDir);

        // Pre-load Vulkan loader so Vulkan symbols are resident in the process
        HMODULE hVk = LoadLibraryW(L"vulkan-1.dll");
        LogBridge("[RemixBridge-AMD] Pre-loaded vulkan-1.dll (handle: %p)\n", (void*)hVk);

        // Explicitly pre-load single master OptiScaler.dll from parentDir
        wchar_t optiPath[MAX_PATH];
        swprintf_s(optiPath, L"%s\\OptiScaler.dll", parentDir);
        if (GetFileAttributesW(optiPath) != INVALID_FILE_ATTRIBUTES) {
            HMODULE hOpti = LoadLibraryW(optiPath);
            LogBridge("[RemixBridge-AMD] Pre-loaded OptiScaler: %ls (handle: %p, err: %lu)\n",
                      optiPath, (void*)hOpti, hOpti ? 0 : GetLastError());
        } else {
            LogBridge("[RemixBridge-AMD] OptiScaler.dll not found at %ls, skipping pre-load\n", optiPath);
        }
    }

    g_remixDll = LoadLibraryW(dllPath);
    if (!g_remixDll) {
        return REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE;
    }

    auto pfnInit = (PFN_remixapi_InitializeLibrary)
        GetProcAddress(g_remixDll, "remixapi_InitializeLibrary");
    if (!pfnInit) {
        FreeLibrary(g_remixDll);
        g_remixDll = nullptr;
        return REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE;
    }

    // Install AMD Sparse Rendering Wave32 hook on d3d9.dll's Vulkan IAT before Remix init.
    amd_sparse_wave32::install(g_remixDll);

    remixapi_InitializeLibraryInfo initInfo = {};
    initInfo.sType   = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    initInfo.version = REMIXAPI_VERSION_MAKE(
        REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);

    memset(&g_remix, 0, sizeof(g_remix));

    recordJvmImage();
    remixapi_ErrorCode rc = remixCall([&] { return pfnInit(&initInfo, &g_remix); });
    if (rc == REMIXAPI_ERROR_CODE_SUCCESS) {
        // ABI sanity check: every function pointer we rely on at runtime must be
        // populated. A partially-filled vtable indicates the loaded d3d9.dll is
        // from an older minor version that predates a tail-of-struct addition.
        // isVersionCompatible on the runtime side should catch this first, but
        // keep a belt-and-suspenders check for custom/forked runtimes.
        if (!g_remix.UpdateTextureFromMemory || !g_remix.UpdateMaterial
                || !g_remix.GetConfigVariableResolved) {
            fprintf(stderr,
                "[remixbridge] FATAL: loaded Remix runtime is missing "
                "required vtable tail entries. "
                "This indicates an ABI mismatch: expected REMIXAPI v%d.%d.%d "
                "or newer.\n",
                REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
            fflush(stderr);
            memset(&g_remix, 0, sizeof(g_remix));
            amd_sparse_wave32::uninstall(g_remixDll);
            FreeLibrary(g_remixDll);
            g_remixDll = nullptr;
            return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
        }
        g_inited = true;
        {
            std::lock_guard<std::mutex> lock(s_tracyMutex);
            s_tracyEnabled = true;
            resetTracyFunctionsLocked();
        }
        installCrashHandler();
    }
    return (jint)rc;
}

// ════════════════════════════════════════════════════════════════════════════
// JNI exports: com_nvidia_rtxremix_RemixBridge_*
// ════════════════════════════════════════════════════════════════════════════

extern "C" {

// ─── initialize ──────────────────────────────────────────────────────────

// Plain C entry point used by the developer tools in a short-lived subprocess.
// It deliberately follows the same initialization path as Java so this bridge,
// not the launcher, remains the authority on Remix API compatibility.
__declspec(dllexport) jint remixbridge_probe_runtime(const wchar_t* dllPath)
{
    if (!dllPath || !dllPath[0]) {
        return REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE;
    }
    return initializeRuntime(dllPath);
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_initialize(JNIEnv* env, jclass, jstring jDllPath)
{
    const char* path = env->GetStringUTFChars(jDllPath, nullptr);

    // Convert UTF-8 path to wide string for SetDllDirectoryW / LoadLibraryW
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    wchar_t* wpath = (wchar_t*)_alloca(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    env->ReleaseStringUTFChars(jDllPath, path);

    return initializeRuntime(wpath);
}

// ─── process command line ────────────────────────────────────────────────

// Crash dumps copy the process parameters out of the PEB, so this is the copy
// a redaction must reach.
static UNICODE_STRING& pebCommandLine() {
    return NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters->CommandLine;
}

JNIEXPORT jstring JNICALL
Java_com_nvidia_rtxremix_RemixBridge_processCommandLine(JNIEnv* env, jclass)
{
    const UNICODE_STRING& commandLine = pebCommandLine();
    return env->NewString((const jchar*)commandLine.Buffer,
        (jsize)(commandLine.Length / sizeof(WCHAR)));
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_overwriteProcessCommandLine(JNIEnv* env, jclass,
    jstring jReplacement)
{
    UNICODE_STRING& commandLine = pebCommandLine();
    const jsize length = (jsize)(commandLine.Length / sizeof(WCHAR));
    if (!jReplacement || env->GetStringLength(jReplacement) != length)
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;

    std::vector<WCHAR> replacement(length);
    env->GetStringRegion(jReplacement, 0, length, (jchar*)replacement.data());

    // GetCommandLineA returns an ANSI conversion made at process start, sized
    // for the original line. Converting the replacement must fit in it, and is
    // shorter whenever a blanked character took more than one byte.
    char* ansi = GetCommandLineA();
    const size_t originalAnsiLength = strlen(ansi);
    const int ansiLength = WideCharToMultiByte(CP_ACP, 0, replacement.data(), length,
        nullptr, 0, nullptr, nullptr);
    if (ansiLength <= 0 || (size_t)ansiLength > originalAnsiLength)
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;

    // GetCommandLineW normally returns the PEB buffer itself. A separate copy
    // of the same line is kept in step.
    WCHAR* wide = GetCommandLineW();
    const bool separateWideCopy = wide != commandLine.Buffer
        && wcslen(wide) == (size_t)length
        && wmemcmp(wide, commandLine.Buffer, length) == 0;

    wmemcpy(commandLine.Buffer, replacement.data(), length);
    if (separateWideCopy)
        wmemcpy(wide, replacement.data(), length);
    WideCharToMultiByte(CP_ACP, 0, replacement.data(), length, ansi, ansiLength, nullptr, nullptr);
    // Zero through the original terminator, so no byte of a blanked value
    // survives past the new end of the line.
    SecureZeroMemory(ansi + ansiLength, originalAnsiLength - ansiLength + 1);
    return REMIXAPI_ERROR_CODE_SUCCESS;
}

// ─── crash dumps ─────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_com_nvidia_rtxremix_RemixBridge_setCrashDumpDetail(JNIEnv*, jclass, jboolean full)
{
    g_crashDumpDetail = full;
}

// ─── lifecycle ───────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_startup(JNIEnv*, jclass,
    jlong hwnd, jboolean disableSrgb, jboolean forceNoVkSwapchain,
    jboolean editorMode, jboolean combineGui, jboolean interopImage,
    jboolean useRemixOwnedChildWindow)
{
    if (!g_inited || !g_remix.Startup) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    s_origHwnd = (HWND)(uintptr_t)hwnd;

    remixapi_StartupInfo info = {};
    info.sType                         = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    info.disableSrgbConversionForOutput = disableSrgb;
    info.forceNoVkSwapchain            = forceNoVkSwapchain;
    info.editorModeEnabled             = editorMode;
    info.combineGuiInFinalColor        = combineGui;
    info.interopImage                  = interopImage;
    info.useRemixOwnedChildWindow      = useRemixOwnedChildWindow;

    HWND targetHwnd = s_origHwnd;
    if (!useRemixOwnedChildWindow) {
        targetHwnd = EnsureCleanWindow(s_origHwnd);
        if (!targetHwnd) targetHwnd = s_origHwnd;
    }
    info.hwnd = targetHwnd;

    remixapi_ErrorCode res = remixCall([&] { return g_remix.Startup(&info); });
    LogBridge("[RemixBridge-AMD] g_remix.Startup returned %d (targetHwnd=%p, useRemixOwnedChildWindow=%d)\n",
              (int)res, targetHwnd, (int)useRemixOwnedChildWindow);

    if (res != REMIXAPI_ERROR_CODE_SUCCESS && useRemixOwnedChildWindow) {
        LogBridge("[RemixBridge-AMD] Native child window Startup failed, retrying with s_cleanHwnd fallback...\n");
        info.useRemixOwnedChildWindow = false;
        targetHwnd = EnsureCleanWindow(s_origHwnd);
        info.hwnd = targetHwnd ? targetHwnd : s_origHwnd;
        res = remixCall([&] { return g_remix.Startup(&info); });
        LogBridge("[RemixBridge-AMD] Fallback g_remix.Startup returned %d\n", (int)res);
    }

    return (jint)res;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_shutdown(JNIEnv*, jclass)
{
    if (!g_inited || !g_remix.Shutdown) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    auto shutdown = g_remix.Shutdown;
    g_inited = false;
    uninstallCrashHandler();
    {
        std::lock_guard<std::mutex> lock(s_tracyMutex);
        s_tracyEnabled = false;
        resetTracyFunctionsLocked();
    }
    if (s_cleanHwnd && IsWindow(s_cleanHwnd)) {
        ShowWindow(s_cleanHwnd, SW_HIDE);
        DestroyWindow(s_cleanHwnd);
        s_cleanHwnd = nullptr;
    }
    s_wasHostActive = false;
    jint rc = (jint)remixCall([&] { return shutdown(); });
    if (g_remixDll) {
        amd_sparse_wave32::uninstall(g_remixDll);
        FreeLibrary(g_remixDll);
        g_remixDll = nullptr;
    }
    memset(&g_remix, 0, sizeof(g_remix));
    return rc;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_submit(JNIEnv*, jclass,
    jint interopFlags)
{
    if (!g_inited || !g_remix.Submit) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    remixapi_SubmitInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_SUBMIT_INFO;
    info.interopFlags = (uint32_t)interopFlags;
    return (jint)remixCall([&] { return g_remix.Submit(&info); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_present(JNIEnv*, jclass,
    jlong hwndOverride,
    jint skipVulkanSwapchain,
    jint interopFlipY,
    jlong clientDoneSemaphore_ValueToWait,
    jint disableCpuWaitForExternalSemaphores)
{
    if (!g_inited || !g_remix.Present) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

    if (s_cleanHwnd && IsWindow(s_cleanHwnd) && s_origHwnd && IsWindow(s_origHwnd)) {
        MSG msg;
        while (PeekMessageW(&msg, s_cleanHwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        RECT hostRc = {};
        GetClientRect(s_origHwnd, &hostRc);
        int hostW = hostRc.right - hostRc.left;
        int hostH = hostRc.bottom - hostRc.top;

        if (hostW > 0 && hostH > 0) {
            if (s_isChild) {
                RECT curRc = {};
                GetClientRect(s_cleanHwnd, &curRc);
                if ((curRc.right - curRc.left) != hostW || (curRc.bottom - curRc.top) != hostH) {
                    SetWindowPos(s_cleanHwnd, HWND_TOP, 0, 0, hostW, hostH,
                                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
                }
            } else {
                POINT pt = { 0, 0 };
                ClientToScreen(s_origHwnd, &pt);
                RECT curRc = {};
                GetWindowRect(s_cleanHwnd, &curRc);

                HWND fg = GetForegroundWindow();
                bool isHostActive = (fg == s_origHwnd || fg == s_cleanHwnd ||
                                     (s_origHwnd && IsChild(s_origHwnd, fg)));
                bool isHostIconic = (s_origHwnd && IsIconic(s_origHwnd));

                if (isHostIconic) {
                    if (IsWindowVisible(s_cleanHwnd)) {
                        ShowWindow(s_cleanHwnd, SW_HIDE);
                    }
                    s_wasHostActive = false;
                } else {
                    bool needMove = (curRc.left != pt.x || curRc.top != pt.y ||
                                     (curRc.right - curRc.left) != hostW || (curRc.bottom - curRc.top) != hostH);
                    bool needShow = !IsWindowVisible(s_cleanHwnd);
                    bool activeChanged = (isHostActive != s_wasHostActive);
                    bool isTopmost = (GetWindowLongPtrW(s_cleanHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                    s_wasHostActive = isHostActive;

                    if (needMove || needShow || (activeChanged && isHostActive) || (!isHostActive && isTopmost)) {
                        HWND insertAfter = isHostActive ? HWND_TOP : HWND_NOTOPMOST;
                        UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
                        if (!needMove) flags |= (SWP_NOMOVE | SWP_NOSIZE);
                        if (!isHostActive && !isTopmost) flags |= SWP_NOZORDER;

                        SetWindowPos(s_cleanHwnd, insertAfter, pt.x, pt.y, hostW, hostH, flags);
                    }
                }
            }
        }
    }

    HWND targetHwnd = (HWND)(uintptr_t)hwndOverride;
    if (s_cleanHwnd && IsWindow(s_cleanHwnd) &&
        (targetHwnd == nullptr || targetHwnd == s_origHwnd)) {
        targetHwnd = s_cleanHwnd;
    }

    remixapi_PresentInfo info = {};
    info.sType        = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    info.hwndOverride = targetHwnd;
    info.skipVulkanSwapchain = (bool)skipVulkanSwapchain;
    info.interopFlipY = (bool)interopFlipY;
    info.clientDoneSemaphore_ValueToWait = (uint64_t)clientDoneSemaphore_ValueToWait;
    info.disableCpuWaitForExternalSemaphores = (bool)disableCpuWaitForExternalSemaphores;
    return (jint)remixCall([&] { return g_remix.Present(&info); });
}

// Faults under the Remix call guard, to check where such a crash is reported.
// By default DrawInstance gets an unmapped pointer, which the runtime reads on
// this thread before it queues any work, so d3d9.dll faults and Crashpad
// should receive it. insideJvm faults in jvm.dll instead, which the guard must
// leave to HotSpot. Returns only if nothing faulted.
JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_faultInRemixCall(JNIEnv* env, jclass, jboolean insideJvm)
{
    if (!g_inited || !g_remix.DrawInstance) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    const uintptr_t unmapped = 0x100;
    if (insideJvm)
        return remixCall([&] { return env->GetArrayLength((jarray)unmapped); });
    return (jint)remixCall([&] { return g_remix.DrawInstance((const remixapi_InstanceInfo*)unmapped); });
}

// Whether the Remix call guard hands a fatal fault in the runtime to the
// runtime's crash reporter. On by default; callable before initialize.
JNIEXPORT void JNICALL
Java_com_nvidia_rtxremix_RemixBridge_setRemixFaultForwarding(JNIEnv*, jclass, jboolean enabled)
{
    InterlockedExchange(&g_forwardRemixFaults, enabled ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_dxvkGetInteropInfo(JNIEnv* env, jclass, jobject jOutInteropInfo)
{
    if (!g_inited || !g_remix.dxvk_GetInteropInfo) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

    remixapi_dxvk_InteropInfo info = {};
    remixapi_ErrorCode r = remixCall([&] { return g_remix.dxvk_GetInteropInfo(&info); });
    if (r != REMIXAPI_ERROR_CODE_SUCCESS) return (jint)r;

    // Get class of Java interop info object
    jclass cls = env->GetObjectClass(jOutInteropInfo);
    if (cls == nullptr) {
        return REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE;
    }

#define setJavaFieldInteropInfo(fieldName)                                     \
    if (!setJavaInstanceField(env, cls, jOutInteropInfo, #fieldName,            \
                              &info.fieldName)) {                              \
      return REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE;                     \
    }

    // Set fields of Java interop info object
    setJavaFieldInteropInfo(dxvkDoneSemaphore_Handle);
    setJavaFieldInteropInfo(dxvkDoneSemaphore_ValueToWait);
    setJavaFieldInteropInfo(clientDoneSemaphore_Handle);
    setJavaFieldInteropInfo(imageMemory_Handle);
    setJavaFieldInteropInfo(allocationSize);
    setJavaFieldInteropInfo(width);
    setJavaFieldInteropInfo(height);
    setJavaFieldInteropInfo(interopImageFormat);
    setJavaFieldInteropInfo(depthImageMemory_Handle);
    setJavaFieldInteropInfo(depthAllocationSize);
    setJavaFieldInteropInfo(depthWidth);
    setJavaFieldInteropInfo(depthHeight);
    setJavaFieldInteropInfo(interopDepthImageFormat);

    return (jint)r;
}


// ─── materials ───────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_createMaterial(JNIEnv*, jclass,
    jlong hash,
    jlong albedoTex, jlong normalTex, jlong tangentTex, jlong emissiveTex,
    jfloat emIntensity, jfloat emR, jfloat emG, jfloat emB,
    jint spriteRow, jint spriteCol, jint spriteFps,
    jint filterMode, jint wrapU, jint wrapV,
    jlong pNext)
{
    if (!g_inited || !g_remix.CreateMaterial) return 0;

    remixapi_MaterialInfo info = {};
    info.sType              = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext              = (void*)(uintptr_t)pNext;
    info.hash               = (uint64_t)hash;
    info.albedoTexture      = (remixapi_TextureHandle)(uintptr_t)albedoTex;
    info.normalTexture      = (remixapi_TextureHandle)(uintptr_t)normalTex;
    info.tangentTexture     = (remixapi_TextureHandle)(uintptr_t)tangentTex;
    info.emissiveTexture    = (remixapi_TextureHandle)(uintptr_t)emissiveTex;
    info.emissiveIntensity  = emIntensity;
    info.emissiveColorConstant = { emR, emG, emB };
    info.spriteSheetRow     = (uint8_t)spriteRow;
    info.spriteSheetCol     = (uint8_t)spriteCol;
    info.spriteSheetFps     = (uint8_t)spriteFps;
    info.filterMode         = (uint8_t)filterMode;
    info.wrapModeU          = (uint8_t)wrapU;
    info.wrapModeV          = (uint8_t)wrapV;

    remixapi_MaterialHandle handle = nullptr;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.CreateMaterial(&info, &handle); });
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)(uintptr_t)handle : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_updateMaterial(JNIEnv*, jclass,
    jlong h, jlong hash,
    jlong albedoTex, jlong normalTex, jlong tangentTex, jlong emissiveTex,
    jfloat emIntensity, jfloat emR, jfloat emG, jfloat emB,
    jint spriteRow, jint spriteCol, jint spriteFps,
    jint filterMode, jint wrapU, jint wrapV,
    jlong pNext)
{
    if (!g_inited || !g_remix.UpdateMaterial) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

    remixapi_MaterialInfo info = {};
    info.sType              = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext              = (void*)(uintptr_t)pNext;
    info.hash               = (uint64_t)hash;
    info.albedoTexture      = (remixapi_TextureHandle)(uintptr_t)albedoTex;
    info.normalTexture      = (remixapi_TextureHandle)(uintptr_t)normalTex;
    info.tangentTexture     = (remixapi_TextureHandle)(uintptr_t)tangentTex;
    info.emissiveTexture    = (remixapi_TextureHandle)(uintptr_t)emissiveTex;
    info.emissiveIntensity  = emIntensity;
    info.emissiveColorConstant = { emR, emG, emB };
    info.spriteSheetRow     = (uint8_t)spriteRow;
    info.spriteSheetCol     = (uint8_t)spriteCol;
    info.spriteSheetFps     = (uint8_t)spriteFps;
    info.filterMode         = (uint8_t)filterMode;
    info.wrapModeU          = (uint8_t)wrapU;
    info.wrapModeV          = (uint8_t)wrapV;

    return (jint)remixCall([&] { return g_remix.UpdateMaterial(
        (remixapi_MaterialHandle)(uintptr_t)h, &info); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_destroyMaterial(JNIEnv*, jclass, jlong h)
{
    if (!g_inited || !g_remix.DestroyMaterial) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.DestroyMaterial((remixapi_MaterialHandle)(uintptr_t)h); });
}

// ─── textures ────────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_createTextureFromMemory(JNIEnv* env, jclass,
    jlong hash, jobject pixelData, jint width, jint height, jint format, jint purpose)
{
    if (!g_inited || !g_remix.CreateTextureFromMemory) return 0;

    remixapi_TextureFromMemoryInfo info = {};
    info.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_FROM_MEMORY_INFO;
    info.hash      = (uint64_t)hash;
    info.pixelData = (const uint8_t*)env->GetDirectBufferAddress(pixelData);
    info.width     = (uint32_t)width;
    info.height    = (uint32_t)height;
    info.format    = (remixapi_TextureFormat)format;
    info.purpose   = (remixapi_TexturePurpose)purpose;

    remixapi_TextureHandle handle = nullptr;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.CreateTextureFromMemory(&info, &handle); });
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)(uintptr_t)handle : 0;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_createTextureFromFile(JNIEnv* env, jclass, jstring jPath)
{
    if (!g_inited || !g_remix.CreateTextureFromFile) return 0;

    const char* utf8 = env->GetStringUTFChars(jPath, nullptr);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    wchar_t* wpath = (wchar_t*)_alloca(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wpath, wlen);
    env->ReleaseStringUTFChars(jPath, utf8);

    remixapi_TextureHandle handle = nullptr;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.CreateTextureFromFile(wpath, &handle); });
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)(uintptr_t)handle : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_destroyTexture(JNIEnv*, jclass, jlong h)
{
    if (!g_inited || !g_remix.DestroyTexture) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.DestroyTexture((remixapi_TextureHandle)(uintptr_t)h); });
}

// Returns bytes-per-pixel for a given remixapi_TextureFormat, or 0 for an
// unrecognized format. Mirrors the table in the dxvk-side textureBytesPerPixel
// helper; keep these in sync if the format enum grows.
static uint32_t bytesPerPixel(remixapi_TextureFormat fmt) {
    switch (fmt) {
        case REMIXAPI_TEXTURE_FORMAT_RGBA8_UNORM:
        case REMIXAPI_TEXTURE_FORMAT_RGBA8_SRGB:
        case REMIXAPI_TEXTURE_FORMAT_BGRA8_UNORM:
        case REMIXAPI_TEXTURE_FORMAT_BGRA8_SRGB:   return 4;
        case REMIXAPI_TEXTURE_FORMAT_R8_UNORM:     return 1;
        case REMIXAPI_TEXTURE_FORMAT_RG8_UNORM:    return 2;
        case REMIXAPI_TEXTURE_FORMAT_RGBA16_SFLOAT: return 8;
        case REMIXAPI_TEXTURE_FORMAT_RGBA32_SFLOAT: return 16;
        default: return 0;
    }
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_updateTextureFromMemory(JNIEnv* env, jclass,
    jlongArray jHandles,
    jintArray jEntryPatchCounts,
    jintArray jDstXs, jintArray jDstYs,
    jintArray jWidths, jintArray jHeights,
    jobjectArray jPixelBuffers,
    jintArray jRowPitches, jintArray jFormats)
{
    if (!g_inited) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    // Defense-in-depth: the initialize path already hard-fails when this
    // pointer is null, but re-check here so a null vtable entry never silently
    // drops texture updates.
    if (!g_remix.UpdateTextureFromMemory) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    if (!jHandles || !jEntryPatchCounts ||
        !jDstXs || !jDstYs || !jWidths || !jHeights ||
        !jPixelBuffers || !jRowPitches || !jFormats) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    jsize entryCount = env->GetArrayLength(jHandles);
    if (entryCount <= 0 ||
        env->GetArrayLength(jEntryPatchCounts) != entryCount) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    std::vector<jlong> handles((size_t)entryCount);
    std::vector<jint>  entryPatchCounts((size_t)entryCount);
    env->GetLongArrayRegion(jHandles, 0, entryCount, handles.data());
    env->GetIntArrayRegion(jEntryPatchCounts, 0, entryCount, entryPatchCounts.data());

    // Reject zero-size entries and sum to the expected flat-patch count.
    // Accumulating into 64-bit before the INT32_MAX check guards against a
    // caller that overflows the intermediate sum with pathological inputs.
    uint64_t totalPatches64 = 0;
    for (jsize e = 0; e < entryCount; e++) {
        if (!handles[(size_t)e] || entryPatchCounts[(size_t)e] <= 0) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }
        totalPatches64 += (uint64_t)(uint32_t)entryPatchCounts[(size_t)e];
        if (totalPatches64 > (uint64_t)INT32_MAX) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }
    }
    jsize totalPatches = (jsize)totalPatches64;

    if (env->GetArrayLength(jDstXs) != totalPatches ||
        env->GetArrayLength(jDstYs) != totalPatches ||
        env->GetArrayLength(jWidths) != totalPatches ||
        env->GetArrayLength(jHeights) != totalPatches ||
        env->GetArrayLength(jPixelBuffers) != totalPatches ||
        env->GetArrayLength(jRowPitches) != totalPatches ||
        env->GetArrayLength(jFormats) != totalPatches) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    std::vector<jint> dstXs(totalPatches), dstYs(totalPatches),
        widths(totalPatches), heights(totalPatches),
        rowPitches(totalPatches), formats(totalPatches);
    env->GetIntArrayRegion(jDstXs, 0, totalPatches, dstXs.data());
    env->GetIntArrayRegion(jDstYs, 0, totalPatches, dstYs.data());
    env->GetIntArrayRegion(jWidths, 0, totalPatches, widths.data());
    env->GetIntArrayRegion(jHeights, 0, totalPatches, heights.data());
    env->GetIntArrayRegion(jRowPitches, 0, totalPatches, rowPitches.data());
    env->GetIntArrayRegion(jFormats, 0, totalPatches, formats.data());

    // Upper bound on any single dimension. Prevents uint32 overflow when
    // multiplying width * bpp (bpp is at most 16, so 65535*16 = 1,048,560,
    // comfortably within INT_MAX) and is far larger than any reasonable
    // dynamic-texture patch.
    static constexpr jint kMaxDim = 65535;

    // Pre-scan + marshal all patches into one flat vector. We validate every
    // patch up-front so a bad entry aborts the whole batch before any native
    // call runs: no partial submissions.
    std::vector<remixapi_TexturePatch> patches((size_t)totalPatches);
    for (jsize i = 0; i < totalPatches; i++) {
        // Reject negative coordinates, non-positive / oversize dimensions.
        // (Java ints are signed; casting a negative to uint32_t would yield a
        // huge unsigned value that the native side cannot sanity-check.)
        if (dstXs[(size_t)i] < 0 || dstYs[(size_t)i] < 0 ||
            widths[(size_t)i] <= 0 || heights[(size_t)i] <= 0 ||
            widths[(size_t)i] > kMaxDim || heights[(size_t)i] > kMaxDim ||
            rowPitches[(size_t)i] < 0) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }

        uint32_t bpp = bytesPerPixel((remixapi_TextureFormat)formats[(size_t)i]);
        if (bpp == 0) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }

        // Compute row size in 64-bit to dodge overflow, then verify it fits in
        // the signed range we compare against later.
        uint64_t rowBytes = (uint64_t)(uint32_t)widths[(size_t)i] * (uint64_t)bpp;
        if (rowBytes > (uint64_t)INT32_MAX) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }
        // rowPitch must cover at least one full row of pixel data.
        if ((uint64_t)(uint32_t)rowPitches[(size_t)i] < rowBytes) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }

        jobject pixelBuffer = env->GetObjectArrayElement(jPixelBuffers, i);
        if (!pixelBuffer) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }

        void* pixelData = env->GetDirectBufferAddress(pixelBuffer);
        jlong bufCapacity = env->GetDirectBufferCapacity(pixelBuffer);
        env->DeleteLocalRef(pixelBuffer);
        if (!pixelData || bufCapacity < 0) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }

        // Required bytes: (height - 1) full-pitch rows plus one partial row of
        // actual pixel data. The tail row does not need rowPitch padding.
        uint64_t requiredBytes =
            (uint64_t)((uint32_t)heights[(size_t)i] - 1u) *
                (uint64_t)(uint32_t)rowPitches[(size_t)i] +
            rowBytes;
        if ((uint64_t)bufCapacity < requiredBytes) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }

        remixapi_TexturePatch& patch = patches[(size_t)i];
        patch.dstX = (uint32_t)dstXs[(size_t)i];
        patch.dstY = (uint32_t)dstYs[(size_t)i];
        patch.width = (uint32_t)widths[(size_t)i];
        patch.height = (uint32_t)heights[(size_t)i];
        patch.pixelData = (const uint8_t*)pixelData;
        patch.rowPitch = (uint32_t)rowPitches[(size_t)i];
        patch.format = (remixapi_TextureFormat)formats[(size_t)i];
    }

    // Carve the flat patches[] buffer into per-entry sub-ranges. Entry pointers
    // reference patches[] in place, with no extra copy, so patches must outlive
    // the native call (it does).
    std::vector<remixapi_TextureBatchEntry> entries((size_t)entryCount);
    size_t flat = 0;
    for (jsize e = 0; e < entryCount; e++) {
        remixapi_TextureBatchEntry& entry = entries[(size_t)e];
        entry.handle = (remixapi_TextureHandle)(uintptr_t)handles[(size_t)e];
        entry.patchCount = (uint32_t)entryPatchCounts[(size_t)e];
        entry.patches = patches.data() + flat;
        flat += (size_t)entryPatchCounts[(size_t)e];
    }

    remixapi_TexturePatchInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_PATCH_INFO;
    info.entryCount = (uint32_t)entryCount;
    info.entries = entries.data();
    return (jint)remixCall([&] { return g_remix.UpdateTextureFromMemory(&info); });
}

// ─── meshes ──────────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_createMesh(JNIEnv* env, jclass,
    jlong hash, jint surfaceCount,
    jobjectArray vertexBuffers, jintArray vertexCounts,
    jobjectArray indexBuffers, jintArray indexCounts,
    jlongArray materialHandles,
    jintArray flags,
    jintArray bonesPerVertex,
    jobjectArray blendWeightBuffers, jintArray blendWeightCounts,
    jobjectArray blendIndexBuffers, jintArray blendIndexCounts)
{
    if (!g_inited || !g_remix.CreateMesh) return 0;

    remixapi_MeshInfoSurfaceTriangles stackBuf[MAX_STACK_SURFACES];
    remixapi_MeshInfoSurfaceTriangles* surfs = buildSurfaces(
        env, surfaceCount,
        vertexBuffers, vertexCounts, indexBuffers, indexCounts,
        materialHandles, flags, bonesPerVertex,
        blendWeightBuffers, blendWeightCounts, blendIndexBuffers, blendIndexCounts,
        stackBuf);

    remixapi_MeshInfo info = {};
    info.sType          = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash           = (uint64_t)hash;
    info.surfaces_values = surfs;
    info.surfaces_count = (uint32_t)surfaceCount;

    remixapi_MeshHandle handle = nullptr;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.CreateMesh(&info, &handle); });

    freeSurfaces(surfs, stackBuf);
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)(uintptr_t)handle : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_destroyMesh(JNIEnv*, jclass, jlong h)
{
    if (!g_inited || !g_remix.DestroyMesh) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.DestroyMesh((remixapi_MeshHandle)(uintptr_t)h); });
}

// ─── camera ──────────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_setupCamera(JNIEnv* env, jclass,
    jint type, jfloatArray jView, jfloatArray jProj, jlong pNext)
{
    if (!g_inited || !g_remix.SetupCamera) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

    remixapi_CameraInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    info.pNext = (void*)(uintptr_t)pNext;
    info.type  = (remixapi_CameraType)type;

    jfloat* v = (jfloat*)env->GetPrimitiveArrayCritical(jView, nullptr);
    memcpy(&info.view, v, 16 * sizeof(float));
    env->ReleasePrimitiveArrayCritical(jView, v, JNI_ABORT);

    jfloat* p = (jfloat*)env->GetPrimitiveArrayCritical(jProj, nullptr);
    memcpy(&info.projection, p, 16 * sizeof(float));
    env->ReleasePrimitiveArrayCritical(jProj, p, JNI_ABORT);

    return (jint)remixCall([&] { return g_remix.SetupCamera(&info); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_setCameraMediumMaterial(JNIEnv*, jclass, jlong materialHandle)
{
    if (!g_inited || !g_remix.SetCameraMediumMaterial) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

    remixapi_CameraMediumInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO;
    info.medium = (remixapi_MaterialHandle)(uintptr_t)materialHandle;

    return (jint)remixCall([&] { return g_remix.SetCameraMediumMaterial(&info); });
}

// ─── instances ───────────────────────────────────────────────────────────

static void fillInstanceInfo(JNIEnv* env, remixapi_InstanceInfo& info,
    jlong meshHandle, jfloatArray jTransform, jint categoryFlags,
    jint doubleSided, jlong pNext)
{
    memset(&info, 0, sizeof(info));
    info.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    info.pNext         = (void*)(uintptr_t)pNext;
    info.categoryFlags = (uint32_t)categoryFlags;
    info.mesh          = (remixapi_MeshHandle)(uintptr_t)meshHandle;
    info.doubleSided   = (uint32_t)doubleSided;

    jfloat* t = (jfloat*)env->GetPrimitiveArrayCritical(jTransform, nullptr);
    memcpy(&info.transform, t, 12 * sizeof(float));
    env->ReleasePrimitiveArrayCritical(jTransform, t, JNI_ABORT);
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_drawInstance(JNIEnv* env, jclass,
    jlong meshHandle, jfloatArray jTransform, jint categoryFlags,
    jint doubleSided, jlong pNext)
{
    if (!g_inited || !g_remix.DrawInstance) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

    remixapi_InstanceInfo info;
    fillInstanceInfo(env, info, meshHandle, jTransform, categoryFlags,
                     doubleSided, pNext);
    return (jint)remixCall([&] { return g_remix.DrawInstance(&info); });
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_sceneInstanceAdd(JNIEnv* env, jclass,
    jlong meshHandle, jfloatArray jTransform, jint categoryFlags,
    jint doubleSided, jlong pNext)
{
    if (!g_inited || !g_remix.SceneInstanceAdd) return 0;

    remixapi_InstanceInfo info;
    fillInstanceInfo(env, info, meshHandle, jTransform, categoryFlags,
                     doubleSided, pNext);

    remixapi_SceneInstanceHandle handle = nullptr;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.SceneInstanceAdd(&info, &handle); });
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)(uintptr_t)handle : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_sceneInstanceUpdate(JNIEnv* env, jclass,
    jlong sceneInstanceHandle, jlong meshHandle, jfloatArray jTransform,
    jint categoryFlags, jint doubleSided, jlong pNext)
{
    if (!g_inited || !g_remix.SceneInstanceUpdate) {
        return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }

    remixapi_InstanceInfo info;
    fillInstanceInfo(env, info, meshHandle, jTransform, categoryFlags,
                     doubleSided, pNext);
    return (jint)remixCall([&] { return g_remix.SceneInstanceUpdate(
        (remixapi_SceneInstanceHandle)(uintptr_t)sceneInstanceHandle, &info); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_sceneInstanceRemove(JNIEnv*, jclass,
    jlong sceneInstanceHandle)
{
    if (!g_inited || !g_remix.SceneInstanceRemove) {
        return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    return (jint)remixCall([&] { return g_remix.SceneInstanceRemove(
        (remixapi_SceneInstanceHandle)(uintptr_t)sceneInstanceHandle); });
}

// ─── lights ──────────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_createLight(JNIEnv*, jclass,
    jlong hash, jfloat radR, jfloat radG, jfloat radB, jlong pNext)
{
    if (!g_inited || !g_remix.CreateLight) return 0;

    remixapi_LightInfo info = {};
    info.sType    = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext    = (void*)(uintptr_t)pNext;
    info.hash     = (uint64_t)hash;
    info.radiance = { radR, radG, radB };

    remixapi_LightHandle handle = nullptr;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.CreateLight(&info, &handle); });
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)(uintptr_t)handle : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_destroyLight(JNIEnv*, jclass, jlong h)
{
    if (!g_inited || !g_remix.DestroyLight) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.DestroyLight((remixapi_LightHandle)(uintptr_t)h); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_drawLightInstance(JNIEnv*, jclass, jlong h)
{
    if (!g_inited || !g_remix.DrawLightInstance) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.DrawLightInstance((remixapi_LightHandle)(uintptr_t)h); });
}

// ─── config ──────────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_setConfigVariable(JNIEnv* env, jclass,
    jstring jKey, jstring jValue)
{
    if (!g_inited || !g_remix.SetConfigVariable) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    const char* key   = env->GetStringUTFChars(jKey, nullptr);
    const char* value = env->GetStringUTFChars(jValue, nullptr);
    jint rc = (jint)remixCall([&] { return g_remix.SetConfigVariable(key, value); });
    env->ReleaseStringUTFChars(jKey, key);
    env->ReleaseStringUTFChars(jValue, value);
    return rc;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_getConfigVariableResolved(JNIEnv* env, jclass,
    jstring jKey, jbyteArray jValue)
{
    if (!g_inited || !g_remix.GetConfigVariableResolved) return -REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    if (!jKey) return -REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    const char* key = env->GetStringUTFChars(jKey, nullptr);
    jbyte* value = jValue ? env->GetByteArrayElements(jValue, nullptr) : nullptr;
    remixapi_ConfigVariableResolvedInfo info = remixapi_ConfigVariableResolvedInfo_init();
    info.key = key;
    info.value = (char*)value;
    info.valueSize = jValue ? (uint32_t)env->GetArrayLength(jValue) : 0;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.GetConfigVariableResolved(&info); });
    if (value) {
        env->ReleaseByteArrayElements(jValue, value,
            rc == REMIXAPI_ERROR_CODE_SUCCESS ? 0 : JNI_ABORT);
    }
    env->ReleaseStringUTFChars(jKey, key);
    // Returning the size, rather than filling an out-array, lets a per-frame caller read without allocating.
    if (rc == REMIXAPI_ERROR_CODE_SUCCESS || rc == REMIXAPI_ERROR_CODE_BUFFER_TOO_SMALL) {
        return (jint)info.valueSize;
    }
    return -(jint)rc;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_acquireConfigLayer(JNIEnv* env, jclass,
    jstring jPath, jfloat priority, jfloat blendThreshold)
{
    if (!g_inited || !g_remix.AcquireConfigLayer) return 0;
    const char* path = env->GetStringUTFChars(jPath, nullptr);
    remixapi_ConfigLayerHandle handle = nullptr;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.AcquireConfigLayer(
        path, (float)priority, (float)blendThreshold, &handle); });
    env->ReleaseStringUTFChars(jPath, path);
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)(uintptr_t)handle : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_setConfigLayerBlending(JNIEnv*, jclass,
    jlong handle, jfloat strength, jfloat blendThreshold)
{
    if (!g_inited || !g_remix.SetConfigLayerBlending) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.SetConfigLayerBlending(
        (remixapi_ConfigLayerHandle)(uintptr_t)handle,
        (float)strength,
        (float)blendThreshold); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_releaseConfigLayer(JNIEnv*, jclass, jlong handle)
{
    if (!g_inited || !g_remix.ReleaseConfigLayer) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.ReleaseConfigLayer((remixapi_ConfigLayerHandle)(uintptr_t)handle); });
}

/** @return JNI_TRUE when the loaded runtime exposes the config-layer API (API >= 0.9.0) */
JNIEXPORT jboolean JNICALL
Java_com_nvidia_rtxremix_RemixBridge_isConfigLayersSupported(JNIEnv*, jclass)
{
    return (g_inited
        && g_remix.AcquireConfigLayer
        && g_remix.SetConfigLayerBlending
        && g_remix.ReleaseConfigLayer) ? JNI_TRUE : JNI_FALSE;
}

// ─── DXVK interop ────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_dxvkCreateD3D9(JNIEnv*, jclass, jboolean editorMode)
{
    if (!g_inited || !g_remix.dxvk_CreateD3D9) return 0;
    IDirect3D9Ex* pD3D9 = nullptr;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.dxvk_CreateD3D9(editorMode ? 1 : 0, &pD3D9); });
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)(uintptr_t)pD3D9 : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_dxvkRegisterD3D9Device(JNIEnv*, jclass, jlong device)
{
    if (!g_inited || !g_remix.dxvk_RegisterD3D9Device) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.dxvk_RegisterD3D9Device((IDirect3DDevice9Ex*)(uintptr_t)device); });
}

JNIEXPORT jlongArray JNICALL
Java_com_nvidia_rtxremix_RemixBridge_dxvkGetExternalSwapchain(JNIEnv* env, jclass)
{
    if (!g_inited || !g_remix.dxvk_GetExternalSwapchain) return nullptr;
    uint64_t vkImage = 0, semDone = 0, semResume = 0;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.dxvk_GetExternalSwapchain(&vkImage, &semDone, &semResume); });
    if (rc != REMIXAPI_ERROR_CODE_SUCCESS) return nullptr;

    jlongArray arr = env->NewLongArray(3);
    jlong vals[3] = { (jlong)vkImage, (jlong)semDone, (jlong)semResume };
    env->SetLongArrayRegion(arr, 0, 3, vals);
    return arr;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_dxvkGetVkImage(JNIEnv*, jclass, jlong surface)
{
    if (!g_inited || !g_remix.dxvk_GetVkImage) return 0;
    uint64_t vkImage = 0;
    remixapi_ErrorCode rc = remixCall([&] { return g_remix.dxvk_GetVkImage(
        (IDirect3DSurface9*)(uintptr_t)surface, &vkImage); });
    return (rc == REMIXAPI_ERROR_CODE_SUCCESS) ? (jlong)vkImage : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_dxvkCopyRenderingOutput(JNIEnv*, jclass,
    jlong surface, jint type)
{
    if (!g_inited || !g_remix.dxvk_CopyRenderingOutput) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    return (jint)remixCall([&] { return g_remix.dxvk_CopyRenderingOutput(
        (IDirect3DSurface9*)(uintptr_t)surface,
        (remixapi_dxvk_CopyRenderingOutputType)type); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_dxvkSetDefaultOutput(JNIEnv*, jclass,
    jint type, jfloat r, jfloat g, jfloat b, jfloat a)
{
    if (!g_inited || !g_remix.dxvk_SetDefaultOutput) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    remixapi_Float4D color = { r, g, b, a };
    return (jint)remixCall([&] { return g_remix.dxvk_SetDefaultOutput(
        (remixapi_dxvk_CopyRenderingOutputType)type, &color); });
}

// ─── D3D9 COM helpers ────────────────────────────────────────────────────
// These replicate D3D9Device.java's vtable calls in C++ where they belong.

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_d3d9CreateDevice(JNIEnv*, jclass,
    jlong pD3D9ExJ, jlong hwndJ, jint width, jint height)
{
    auto pD3D9Ex = (IDirect3D9Ex*)(uintptr_t)pD3D9ExJ;
    HWND hwnd    = (HWND)(uintptr_t)hwndJ;

    // Build D3DPRESENT_PARAMETERS on the stack
    struct {
        UINT  BackBufferWidth;
        UINT  BackBufferHeight;
        UINT  BackBufferFormat;       // D3DFMT_UNKNOWN = 0
        UINT  BackBufferCount;
        UINT  MultiSampleType;        // D3DMULTISAMPLE_NONE = 0
        DWORD MultiSampleQuality;
        UINT  SwapEffect;             // D3DSWAPEFFECT_DISCARD = 1
        HWND  hDeviceWindow;
        BOOL  Windowed;
        BOOL  EnableAutoDepthStencil;
        UINT  AutoDepthStencilFormat;
        DWORD Flags;
        UINT  FullScreen_RefreshRateInHz;
        UINT  PresentationInterval;
    } pp = {};
    pp.BackBufferWidth  = (UINT)width;
    pp.BackBufferHeight = (UINT)height;
    pp.SwapEffect       = 1; // D3DSWAPEFFECT_DISCARD
    pp.hDeviceWindow    = hwnd;
    pp.Windowed         = TRUE;

    DWORD behaviorFlags = 0x04 /*MULTITHREADED*/ | 0x20 /*SOFTWARE_VP*/ | 0x800 /*NOWINDOWCHANGES*/;

    // IDirect3D9Ex vtable[20] = CreateDeviceEx
    void** vtable = *(void***)pD3D9Ex;
    typedef HRESULT(__stdcall* PFN_CreateDeviceEx)(
        IDirect3D9Ex*, UINT, UINT, HWND, DWORD, void*, void*, void**);
    auto pfnCreate = (PFN_CreateDeviceEx)vtable[20];

    IDirect3DDevice9Ex* pDevice = nullptr;
    HRESULT hr = remixCall([&] {
        return pfnCreate(pD3D9Ex, 0, 1/*HAL*/, nullptr, behaviorFlags, &pp, nullptr, (void**)&pDevice);
    });
    return (SUCCEEDED(hr) && pDevice) ? (jlong)(uintptr_t)pDevice : 0;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_d3d9CreateRenderTarget(JNIEnv*, jclass,
    jlong pDeviceJ, jint width, jint height, jint format)
{
    auto pDevice = (IDirect3DDevice9Ex*)(uintptr_t)pDeviceJ;
    void** vtable = *(void***)pDevice;

    // IDirect3DDevice9 vtable[28] = CreateRenderTarget
    typedef HRESULT(__stdcall* PFN_CreateRT)(
        IDirect3DDevice9Ex*, UINT, UINT, UINT, UINT, DWORD, BOOL, void**, void*);
    auto pfnCreateRT = (PFN_CreateRT)vtable[28];

    IDirect3DSurface9* pSurface = nullptr;
    HRESULT hr = remixCall([&] {
        return pfnCreateRT(pDevice, (UINT)width, (UINT)height,
                           (UINT)format, 0/*NONE*/, 0, FALSE,
                           (void**)&pSurface, nullptr);
    });
    return (SUCCEEDED(hr) && pSurface) ? (jlong)(uintptr_t)pSurface : 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_d3d9Reset(JNIEnv*, jclass,
    jlong pDeviceJ, jint width, jint height)
{
    auto pDevice = (IDirect3DDevice9Ex*)(uintptr_t)pDeviceJ;
    void** vtable = *(void***)pDevice;

    struct {
        UINT  BackBufferWidth;
        UINT  BackBufferHeight;
        UINT  BackBufferFormat;
        UINT  BackBufferCount;
        UINT  MultiSampleType;
        DWORD MultiSampleQuality;
        UINT  SwapEffect;
        HWND  hDeviceWindow;
        BOOL  Windowed;
        BOOL  EnableAutoDepthStencil;
        UINT  AutoDepthStencilFormat;
        DWORD Flags;
        UINT  FullScreen_RefreshRateInHz;
        UINT  PresentationInterval;
    } pp = {};
    pp.BackBufferWidth  = (UINT)width;
    pp.BackBufferHeight = (UINT)height;
    pp.BackBufferFormat = 22; // D3DFMT_X8R8G8B8
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = 1;
    pp.Windowed         = TRUE;

    // vtable[16] = Reset
    typedef HRESULT(__stdcall* PFN_Reset)(IDirect3DDevice9Ex*, void*);
    auto pfnReset = (PFN_Reset)vtable[16];
    return (jint)remixCall([&] { return pfnReset(pDevice, &pp); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_d3d9Present(JNIEnv*, jclass, jlong pDeviceJ)
{
    auto pDevice = (IDirect3DDevice9Ex*)(uintptr_t)pDeviceJ;
    void** vtable = *(void***)pDevice;
    // vtable[17] = Present
    typedef HRESULT(__stdcall* PFN_Present)(IDirect3DDevice9Ex*, void*, void*, HWND, void*);
    auto pfnPresent = (PFN_Present)vtable[17];
    return (jint)remixCall([&] { return pfnPresent(pDevice, nullptr, nullptr, nullptr, nullptr); });
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_d3d9Release(JNIEnv*, jclass, jlong comPtr)
{
    if (comPtr == 0) return 0;
    auto pUnk = (IUnknown*)(uintptr_t)comPtr;
    void** vtable = *(void***)pUnk;
    // IUnknown vtable[2] = Release
    typedef ULONG(__stdcall* PFN_Release)(IUnknown*);
    auto pfnRelease = (PFN_Release)vtable[2];
    return (jint)remixCall([&] { return pfnRelease(pUnk); });
}

// ─── extension allocation ────────────────────────────────────────────────

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocOpaqueExt(JNIEnv*, jclass,
    jlong roughTex, jlong metalTex, jfloat anisotropy,
    jfloat albR, jfloat albG, jfloat albB,
    jfloat opacity, jfloat roughness, jfloat metallic,
    jboolean hasThinFilm, jfloat thinFilmThickness, jboolean alphaIsThinFilm,
    jlong heightTex, jfloat displaceIn,
    jboolean useDrawCallAlpha,
    jboolean hasBlendType, jint blendType, jboolean invertedBlend,
    jint alphaTestType, jint alphaRefValue, jfloat displaceOut,
    jlong chainNext)
{
    auto* ext = new remixapi_MaterialInfoOpaqueEXT{};
    ext->sType                      = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    ext->pNext                      = (void*)(uintptr_t)chainNext;
    ext->roughnessTexture           = (remixapi_TextureHandle)(uintptr_t)roughTex;
    ext->metallicTexture            = (remixapi_TextureHandle)(uintptr_t)metalTex;
    ext->anisotropy                 = anisotropy;
    ext->albedoConstant             = { albR, albG, albB };
    ext->opacityConstant            = opacity;
    ext->roughnessConstant          = roughness;
    ext->metallicConstant           = metallic;
    ext->thinFilmThickness_hasvalue = hasThinFilm ? 1u : 0u;
    ext->thinFilmThickness_value    = thinFilmThickness;
    ext->alphaIsThinFilmThickness   = alphaIsThinFilm ? 1u : 0u;
    ext->heightTexture              = (remixapi_TextureHandle)(uintptr_t)heightTex;
    ext->displaceIn                 = displaceIn;
    ext->useDrawCallAlphaState      = useDrawCallAlpha ? 1u : 0u;
    ext->blendType_hasvalue         = hasBlendType ? 1u : 0u;
    ext->blendType_value            = blendType;
    ext->invertedBlend              = invertedBlend ? 1u : 0u;
    ext->alphaTestType              = alphaTestType;
    ext->alphaReferenceValue        = (uint8_t)alphaRefValue;
    ext->displaceOut                = displaceOut;
    // DLSS NR enabled for all materials
    ext->enableDlssControlMask             = 1u;
    ext->dlssControlMaskIntensity          = 1.0f;
    ext->dlssControlMaskToneStrength       = 0.3f;
    ext->dlssControlMaskStructuralStrength = 0.7f;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocOpaqueSubsurfaceExt(JNIEnv*, jclass,
    jlong transTex, jlong thickTex, jlong ssaTex,
    jfloat transR, jfloat transG, jfloat transB,
    jfloat measDist,
    jfloat ssaR, jfloat ssaG, jfloat ssaB,
    jfloat volAniso, jboolean diffProfile,
    jfloat radR, jfloat radG, jfloat radB,
    jfloat radScale, jfloat maxSampleRad,
    jlong radTex,
    jlong chainNext)
{
    auto* ext = new remixapi_MaterialInfoOpaqueSubsurfaceEXT{};
    ext->sType                                = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT;
    ext->pNext                                = (void*)(uintptr_t)chainNext;
    ext->subsurfaceTransmittanceTexture       = (remixapi_TextureHandle)(uintptr_t)transTex;
    ext->subsurfaceThicknessTexture           = (remixapi_TextureHandle)(uintptr_t)thickTex;
    ext->subsurfaceSingleScatteringAlbedoTexture = (remixapi_TextureHandle)(uintptr_t)ssaTex;
    ext->subsurfaceTransmittanceColor         = { transR, transG, transB };
    ext->subsurfaceMeasurementDistance         = measDist;
    ext->subsurfaceSingleScatteringAlbedo     = { ssaR, ssaG, ssaB };
    ext->subsurfaceVolumetricAnisotropy       = volAniso;
    ext->subsurfaceDiffusionProfile           = diffProfile ? 1u : 0u;
    ext->subsurfaceRadius                     = { radR, radG, radB };
    ext->subsurfaceRadiusScale                = radScale;
    ext->subsurfaceMaxSampleRadius            = maxSampleRad;
    ext->subsurfaceRadiusTexture              = (remixapi_TextureHandle)(uintptr_t)radTex;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocTranslucentExt(JNIEnv*, jclass,
    jlong transTex, jfloat ior,
    jfloat transR, jfloat transG, jfloat transB,
    jfloat measDist,
    jboolean hasThinWall, jfloat thinWall,
    jboolean useDiffuse,
    jlong chainNext)
{
    auto* ext = new remixapi_MaterialInfoTranslucentEXT{};
    ext->sType                          = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    ext->pNext                          = (void*)(uintptr_t)chainNext;
    ext->transmittanceTexture           = (remixapi_TextureHandle)(uintptr_t)transTex;
    ext->refractiveIndex                = ior;
    ext->transmittanceColor             = { transR, transG, transB };
    ext->transmittanceMeasurementDistance = measDist;
    ext->thinWallThickness_hasvalue     = hasThinWall ? 1u : 0u;
    ext->thinWallThickness_value        = thinWall;
    ext->useDiffuseLayer                = useDiffuse ? 1u : 0u;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocBlendExt(JNIEnv*, jclass,
    jboolean alphaTestEnabled, jint alphaTestRef, jint alphaTestCompareOp,
    jboolean alphaBlendEnabled,
    jint srcColor, jint dstColor, jint colorOp,
    jint tcArg1, jint tcArg2, jint tcOp,
    jint taArg1, jint taArg2, jint taOp,
    jint tFactor, jboolean isTexFactor,
    jint srcAlpha, jint dstAlpha, jint alphaOp,
    jint writeMask, jboolean isVertexBaked,
    jlong chainNext)
{
    auto* ext = new remixapi_InstanceInfoBlendEXT{};
    ext->sType                       = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    ext->pNext                       = (void*)(uintptr_t)chainNext;
    ext->alphaTestEnabled            = alphaTestEnabled ? 1u : 0u;
    ext->alphaTestReferenceValue     = (uint8_t)alphaTestRef;
    ext->alphaTestCompareOp          = (uint32_t)alphaTestCompareOp;
    ext->alphaBlendEnabled           = alphaBlendEnabled ? 1u : 0u;
    ext->srcColorBlendFactor         = (uint32_t)srcColor;
    ext->dstColorBlendFactor         = (uint32_t)dstColor;
    ext->colorBlendOp                = (uint32_t)colorOp;
    ext->textureColorArg1Source      = (uint32_t)tcArg1;
    ext->textureColorArg2Source      = (uint32_t)tcArg2;
    ext->textureColorOperation       = (uint32_t)tcOp;
    ext->textureAlphaArg1Source      = (uint32_t)taArg1;
    ext->textureAlphaArg2Source      = (uint32_t)taArg2;
    ext->textureAlphaOperation       = (uint32_t)taOp;
    ext->tFactor                     = (uint32_t)tFactor;
    ext->isTextureFactorBlend        = isTexFactor ? 1u : 0u;
    ext->srcAlphaBlendFactor         = (uint32_t)srcAlpha;
    ext->dstAlphaBlendFactor         = (uint32_t)dstAlpha;
    ext->alphaBlendOp                = (uint32_t)alphaOp;
    ext->writeMask                   = (uint32_t)writeMask;
    ext->isVertexColorBakedLighting  = isVertexBaked ? 1u : 0u;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocBoneTransformsExt(JNIEnv* env, jclass,
    jobject transforms, jint count, jlong chainNext)
{
    auto* ext = new remixapi_InstanceInfoBoneTransformsEXT{};
    ext->sType                = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
    ext->pNext                = (void*)(uintptr_t)chainNext;
    ext->boneTransforms_count = (uint32_t)count;

    // Copy the transform data so the ByteBuffer can be reused immediately
    size_t bytes = (size_t)count * 48; // 12 floats * 4 bytes
    auto* copy = new uint8_t[bytes];
    memcpy(copy, env->GetDirectBufferAddress(transforms), bytes);
    ext->boneTransforms_values = (const remixapi_Transform*)copy;

    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocGpuInstancingExt(JNIEnv* env, jclass,
    jobject transforms, jint count, jlong chainNext)
{
    auto* ext = new remixapi_InstanceInfoGpuInstancingEXT{};
    ext->sType                    = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
    ext->pNext                    = (void*)(uintptr_t)chainNext;
    ext->instanceTransforms_count = (uint32_t)count;

    size_t bytes = (size_t)count * 48;
    auto* copy = new uint8_t[bytes];
    memcpy(copy, env->GetDirectBufferAddress(transforms), bytes);
    ext->instanceTransforms_values = (const remixapi_Transform*)copy;

    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocSphereExt(JNIEnv*, jclass,
    jfloat px, jfloat py, jfloat pz, jfloat radius,
    jboolean hasShaping,
    jfloat sdx, jfloat sdy, jfloat sdz,
    jfloat coneAngle, jfloat coneSoft, jfloat focusExp,
    jfloat volRadScale,
    jlong chainNext)
{
    auto* ext = new remixapi_LightInfoSphereEXT{};
    ext->sType                  = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    ext->pNext                  = (void*)(uintptr_t)chainNext;
    ext->position               = { px, py, pz };
    ext->radius                 = radius;
    ext->shaping_hasvalue       = hasShaping ? 1u : 0u;
    ext->shaping_value.direction     = { sdx, sdy, sdz };
    ext->shaping_value.coneAngleDegrees = coneAngle;
    ext->shaping_value.coneSoftness  = coneSoft;
    ext->shaping_value.focusExponent = focusExp;
    ext->volumetricRadianceScale = volRadScale;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocDistantExt(JNIEnv*, jclass,
    jfloat dx, jfloat dy, jfloat dz,
    jfloat angDiam, jfloat volRadScale,
    jlong chainNext)
{
    auto* ext = new remixapi_LightInfoDistantEXT{};
    ext->sType                   = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    ext->pNext                   = (void*)(uintptr_t)chainNext;
    ext->direction               = { dx, dy, dz };
    ext->angularDiameterDegrees  = angDiam;
    ext->volumetricRadianceScale = volRadScale;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocRectExt(JNIEnv*, jclass,
    jfloat px, jfloat py, jfloat pz,
    jfloat xax, jfloat xay, jfloat xaz, jfloat xSize,
    jfloat yax, jfloat yay, jfloat yaz, jfloat ySize,
    jfloat dx, jfloat dy, jfloat dz,
    jboolean hasShaping,
    jfloat sdx, jfloat sdy, jfloat sdz,
    jfloat coneAngle, jfloat coneSoft, jfloat focusExp,
    jfloat volRadScale,
    jlong chainNext)
{
    auto* ext = new remixapi_LightInfoRectEXT{};
    ext->sType              = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT;
    ext->pNext              = (void*)(uintptr_t)chainNext;
    ext->position           = { px, py, pz };
    ext->xAxis              = { xax, xay, xaz };
    ext->xSize              = xSize;
    ext->yAxis              = { yax, yay, yaz };
    ext->ySize              = ySize;
    ext->direction          = { dx, dy, dz };
    ext->shaping_hasvalue   = hasShaping ? 1u : 0u;
    ext->shaping_value.direction     = { sdx, sdy, sdz };
    ext->shaping_value.coneAngleDegrees = coneAngle;
    ext->shaping_value.coneSoftness  = coneSoft;
    ext->shaping_value.focusExponent = focusExp;
    ext->volumetricRadianceScale = volRadScale;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocDiskExt(JNIEnv*, jclass,
    jfloat px, jfloat py, jfloat pz,
    jfloat xax, jfloat xay, jfloat xaz, jfloat xRadius,
    jfloat yax, jfloat yay, jfloat yaz, jfloat yRadius,
    jfloat dx, jfloat dy, jfloat dz,
    jboolean hasShaping,
    jfloat sdx, jfloat sdy, jfloat sdz,
    jfloat coneAngle, jfloat coneSoft, jfloat focusExp,
    jfloat volRadScale,
    jlong chainNext)
{
    auto* ext = new remixapi_LightInfoDiskEXT{};
    ext->sType              = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT;
    ext->pNext              = (void*)(uintptr_t)chainNext;
    ext->position           = { px, py, pz };
    ext->xAxis              = { xax, xay, xaz };
    ext->xRadius            = xRadius;
    ext->yAxis              = { yax, yay, yaz };
    ext->yRadius            = yRadius;
    ext->direction          = { dx, dy, dz };
    ext->shaping_hasvalue   = hasShaping ? 1u : 0u;
    ext->shaping_value.direction     = { sdx, sdy, sdz };
    ext->shaping_value.coneAngleDegrees = coneAngle;
    ext->shaping_value.coneSoftness  = coneSoft;
    ext->shaping_value.focusExponent = focusExp;
    ext->volumetricRadianceScale = volRadScale;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocCylinderExt(JNIEnv*, jclass,
    jfloat px, jfloat py, jfloat pz,
    jfloat radius,
    jfloat ax, jfloat ay, jfloat az,
    jfloat axisLength,
    jfloat volRadScale,
    jlong chainNext)
{
    auto* ext = new remixapi_LightInfoCylinderEXT{};
    ext->sType                   = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT;
    ext->pNext                   = (void*)(uintptr_t)chainNext;
    ext->position                = { px, py, pz };
    ext->radius                  = radius;
    ext->axis                    = { ax, ay, az };
    ext->axisLength              = axisLength;
    ext->volumetricRadianceScale = volRadScale;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocDomeExt(JNIEnv* env, jclass,
    jfloatArray jTransform, jlong colorTex, jfloat opacity, jint blendType,
    jlong chainNext)
{
    auto* ext = new remixapi_LightInfoDomeEXT{};
    ext->sType        = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    ext->pNext        = (void*)(uintptr_t)chainNext;
    ext->colorTexture = (remixapi_TextureHandle)(uintptr_t)colorTex;
    ext->opacity      = opacity;
    ext->blendType    = blendType;

    jfloat* t = (jfloat*)env->GetPrimitiveArrayCritical(jTransform, nullptr);
    memcpy(&ext->transform, t, 12 * sizeof(float));
    env->ReleasePrimitiveArrayCritical(jTransform, t, JNI_ABORT);

    return (jlong)(uintptr_t)ext;
}

JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RemixBridge_allocCameraParamExt(JNIEnv*, jclass,
    jfloat px, jfloat py, jfloat pz,
    jfloat fx, jfloat fy, jfloat fz,
    jfloat ux, jfloat uy, jfloat uz,
    jfloat rx, jfloat ry, jfloat rz,
    jfloat fovY, jfloat aspect, jfloat nearP, jfloat farP,
    jlong chainNext)
{
    auto* ext = new remixapi_CameraInfoParameterizedEXT{};
    ext->sType          = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    ext->pNext          = (void*)(uintptr_t)chainNext;
    ext->position       = { px, py, pz };
    ext->forward        = { fx, fy, fz };
    ext->up             = { ux, uy, uz };
    ext->right          = { rx, ry, rz };
    ext->fovYInDegrees  = fovY;
    ext->aspect         = aspect;
    ext->nearPlane      = nearP;
    ext->farPlane       = farP;
    return (jlong)(uintptr_t)ext;
}

JNIEXPORT void JNICALL
Java_com_nvidia_rtxremix_RemixBridge_freeExt(JNIEnv*, jclass, jlong extPtr)
{
    struct ExtHeader {
        uint32_t sType;
        void* pNext;
    };

    void* current = (void*)(uintptr_t)extPtr;
    while (current != nullptr) {
        auto* header = (ExtHeader*)current;
        uint32_t sType = header->sType;
        void* next = header->pNext;

        // For bone/gpu-instancing extensions, also free the copied transform array.
        if (sType == REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT) {
            auto* bone = (remixapi_InstanceInfoBoneTransformsEXT*)current;
            delete[] (uint8_t*)bone->boneTransforms_values;
        } else if (sType == REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT) {
            auto* gpu = (remixapi_InstanceInfoGpuInstancingEXT*)current;
            delete[] (uint8_t*)gpu->instanceTransforms_values;
        }

        // All extension structs are allocated with operator new, so this is safe
        // regardless of the actual type -- they're all POD-like.
        operator delete(current);
        current = next;
    }
}

// ─── Tracy profiling zones callable from Java ─────────────────────────

JNIEXPORT jlong JNICALL Java_com_nvidia_rtxremix_RemixBridge_tracyZoneBegin(
    JNIEnv* env, jclass, jstring name)
{
    std::lock_guard<std::mutex> lock(s_tracyMutex);
    if (!initTracyFunctionsLocked()) return 0;
    const char* cname = env->GetStringUTFChars(name, nullptr);
    if (!cname) return 0;
    const size_t nameLength = strlen(cname);
    uint64_t srcloc = remixCall([&] { return s_allocSrclocName(
        0, "Java", 4, cname, nameLength, cname, nameLength); });
    env->ReleaseStringUTFChars(name, cname);
    TracyCZoneCtx ctx = remixCall([&] { return s_emitZoneBeginAlloc(srcloc, 1); });
    return (jlong)ctx.id | ((jlong)ctx.active << 32);
}

JNIEXPORT void JNICALL Java_com_nvidia_rtxremix_RemixBridge_tracyZoneEnd(
    JNIEnv*, jclass, jlong packed)
{
    std::lock_guard<std::mutex> lock(s_tracyMutex);
    if (!packed || !s_emitZoneEnd) return;
    TracyCZoneCtx ctx;
    ctx.id     = (uint32_t)(packed & 0xFFFFFFFF);
    ctx.active = (int32_t)(packed >> 32);
    remixCall([&] { s_emitZoneEnd(ctx); });
}

JNIEXPORT void JNICALL Java_com_nvidia_rtxremix_RemixBridge_tracySetThreadName(
    JNIEnv* env, jclass, jstring name)
{
    std::lock_guard<std::mutex> lock(s_tracyMutex);
    if (!initTracyFunctionsLocked() || !s_setThreadName) return;
    const char* cname = env->GetStringUTFChars(name, nullptr);
    if (!cname) return;
    remixCall([&] { s_setThreadName(cname); });
    env->ReleaseStringUTFChars(name, cname);
}

// Tracy identifies a plot by the address of its name and fetches the characters
// lazily, so the pointer handed to ___tracy_emit_plot must stay valid for the life
// of the process. Names are interned once here; the set's nodes never move.
static std::set<std::string> s_plotNames;

static const char* internPlotName(JNIEnv* env, jstring name)
{
    const char* utf8 = env->GetStringUTFChars(name, nullptr);
    if (!utf8) return nullptr;
    const char* stable = s_plotNames.emplace(utf8).first->c_str();
    env->ReleaseStringUTFChars(name, utf8);
    return stable;
}

JNIEXPORT void JNICALL Java_com_nvidia_rtxremix_RemixBridge_tracyPlot(
    JNIEnv* env, jclass, jstring name, jdouble value)
{
    std::lock_guard<std::mutex> lock(s_tracyMutex);
    if (!initTracyFunctionsLocked() || !s_emitPlot) return;
    const char* stable = internPlotName(env, name);
    if (stable) remixCall([&] { s_emitPlot(stable, (double)value); });
}

JNIEXPORT void JNICALL Java_com_nvidia_rtxremix_RemixBridge_tracyMessage(
    JNIEnv* env, jclass, jstring message)
{
    std::lock_guard<std::mutex> lock(s_tracyMutex);
    if (!initTracyFunctionsLocked() || !s_emitMessage) return;
    const char* cmessage = env->GetStringUTFChars(message, nullptr);
    if (!cmessage) return;
    const size_t messageLength = strlen(cmessage);
    remixCall([&] { s_emitMessage(cmessage, messageLength, 0); });
    env->ReleaseStringUTFChars(message, cmessage);
}

JNIEXPORT void JNICALL Java_com_nvidia_rtxremix_RemixBridge_tracyFrameMark(
    JNIEnv* env, jclass, jstring name)
{
    std::lock_guard<std::mutex> lock(s_tracyMutex);
    if (!initTracyFunctionsLocked() || !s_emitFrameMark) return;
    const char* cname = nullptr;
    if (name) {
        cname = env->GetStringUTFChars(name, nullptr);
    }
    remixCall([&] { s_emitFrameMark(cname); });
    if (name) {
        env->ReleaseStringUTFChars(name, cname);
    }
}

typedef uint64_t GLuint64;
typedef void (*PFNGLCREATESEMAPHORESNVPROC)(GLsizei n, GLuint* semaphores);
typedef void (*PFNGLSEMAPHOREPARAMETERIVNVPROC)(GLuint semaphore, GLenum pname, const GLint* params);
typedef void (*PFNGLSEMAPHOREPARAMETERUI64VEXTPROC)(GLuint semaphore, GLenum pname, const GLuint64* params);

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_openglSemaphoreParameterui64vEXT(JNIEnv* env, jclass, jint semaphore, jint pname, jlong param0)
{
    if (!g_inited) return -1;

    static PFNGLSEMAPHOREPARAMETERUI64VEXTPROC glSemaphoreParameterui64vEXT = nullptr;
    if (!glSemaphoreParameterui64vEXT) {
      glSemaphoreParameterui64vEXT = (PFNGLSEMAPHOREPARAMETERUI64VEXTPROC)wglGetProcAddress("glSemaphoreParameterui64vEXT");
      if (!glSemaphoreParameterui64vEXT) {
        return -2; // not found
      };
    }

    glSemaphoreParameterui64vEXT((GLuint)semaphore, (GLenum)pname,
                                 (const GLuint64 *)&param0);

    return 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_openglSemaphoreParameterivNV(JNIEnv* env, jclass, jint semaphore, jint pname, jint param0){
    if (!g_inited) return -1;

    static PFNGLSEMAPHOREPARAMETERIVNVPROC glSemaphoreParameterivNV = nullptr;
    if (!glSemaphoreParameterivNV) {
      glSemaphoreParameterivNV = (PFNGLSEMAPHOREPARAMETERIVNVPROC)wglGetProcAddress("glSemaphoreParameterivNV");
      if (!glSemaphoreParameterivNV) {
        return -2; // not found
      };
    }

    glSemaphoreParameterivNV((GLuint)semaphore, (GLenum)pname,
                             (const GLint *)&param0);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RemixBridge_openglCreateSemaphoresNV(JNIEnv* env, jclass)
{
    if (!g_inited) return 0;

    static PFNGLCREATESEMAPHORESNVPROC glCreateSemaphoresNV = nullptr;
    if (!glCreateSemaphoresNV) {
      glCreateSemaphoresNV = (PFNGLCREATESEMAPHORESNVPROC)wglGetProcAddress("glCreateSemaphoresNV");
      if (!glCreateSemaphoresNV) {
        return 0; // not found
      };
    }

    GLuint semaphore = 0;
    glCreateSemaphoresNV(1, &semaphore);
    return (jint)semaphore;
}

} // extern "C"
