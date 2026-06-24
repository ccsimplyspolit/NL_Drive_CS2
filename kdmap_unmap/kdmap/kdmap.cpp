//
// kdmap.exe - kdmapper wrapper that records allocation metadata so that a
// later kdunmap.exe invocation can call ExFreePool / MmFreeIndependentPages
// to fully reclaim the kernel memory WITHOUT a reboot.
//
// Usage:
//   kdmap.exe --key F20Driver --stopEvent "Global\F20DriverStop" [--indPages]
//             [--copy-header] [--PassAllocationPtr] driver.sys
//
//   --key NAME            Required. Identifier used as the subkey under
//                         HKLM\SOFTWARE\kdmap_tracker\<NAME>. kdunmap.exe
//                         must be called with the same --key.
//
//   --stopEvent NAME      Optional. Named event to signal at unmap-time.
//                         Stored in registry so kdunmap.exe knows what to
//                         signal before freeing.
//
//   --indPages            Use MmAllocateIndependentPagesEx (recommended).
//   --copy-header         Keep PE header in kernel memory (default: strip).
//   --PassAllocationPtr   Pass allocation pointer as first DriverEntry arg.
//

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "kdmapper.hpp"
#include "utils.hpp"
#include "intel_driver.hpp"

#include "../common.h"

static void Usage() {
    fwprintf(stderr, L"\n[!] Usage:\n");
    fwprintf(stderr, L"    kdmap.exe --key NAME [--stopEvent NAME] [--indPages]\n");
    fwprintf(stderr, L"              [--copy-header] [--PassAllocationPtr] driver.sys\n\n");
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* xi) {
    if (xi && xi->ExceptionRecord) {
        fwprintf(stderr, L"[!!] Crash @ %p code=0x%08X\n",
                 xi->ExceptionRecord->ExceptionAddress,
                 xi->ExceptionRecord->ExceptionCode);
    }
    if (intel_driver::hDevice) intel_driver::Unload();
    return EXCEPTION_EXECUTE_HANDLER;
}

// State captured from kdmapper's callback (runs in usermode just before
// kdmapper jumps to DriverEntry).
struct CallbackState {
    ULONG64 allocationPtr = 0;
    ULONG64 allocationSize = 0;
    bool    captured = false;
};
static CallbackState* g_cbState = nullptr;

bool MapperCallback(ULONG64* /*p1*/, ULONG64* /*p2*/,
                    ULONG64 allocPtr, ULONG64 allocSize)
{
    if (g_cbState) {
        g_cbState->allocationPtr  = allocPtr;
        g_cbState->allocationSize = allocSize;
        g_cbState->captured       = true;
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(CrashHandler);

    int keyIdx        = KdmapFindArg(argc, argv, L"key");
    int stopIdx       = KdmapFindArg(argc, argv, L"stopEvent");
    bool useIndPages  = KdmapFindArg(argc, argv, L"indPages") > 0;
    bool copyHeader   = KdmapFindArg(argc, argv, L"copy-header") > 0;
    bool passAllocPtr = KdmapFindArg(argc, argv, L"PassAllocationPtr") > 0;

    if (keyIdx < 0 || keyIdx + 1 >= argc) {
        fwprintf(stderr, L"[!] --key NAME is required\n");
        Usage();
        return 1;
    }
    std::wstring keyName = argv[keyIdx + 1];

    std::wstring stopEventName;
    if (stopIdx > 0 && stopIdx + 1 < argc) stopEventName = argv[stopIdx + 1];

    int drvIdx = -1;
    for (int i = 1; i < argc; i++) {
        auto ext = std::filesystem::path(argv[i]).extension().string();
        if (ext == ".sys") { drvIdx = i; break; }
    }
    if (drvIdx < 0) {
        fwprintf(stderr, L"[!] driver.sys path missing\n");
        Usage();
        return 1;
    }
    std::wstring driverPath = argv[drvIdx];
    if (!std::filesystem::exists(driverPath)) {
        fwprintf(stderr, L"[!] file not found: %ls\n", driverPath.c_str());
        return 1;
    }

    wprintf(L"[+] kdmap: key=%ls, mode=%ls, stopEvent=%ls, driver=%ls\n",
            keyName.c_str(),
            useIndPages ? L"IndependentPages" : L"Pool",
            stopEventName.empty() ? L"(none)" : stopEventName.c_str(),
            driverPath.c_str());

    // ---- Load Intel driver, read .sys, map ----------------------------------
    if (!NT_SUCCESS(intel_driver::Load())) {
        fwprintf(stderr, L"[-] intel_driver::Load failed\n");
        return 2;
    }

    std::vector<uint8_t> raw;
    if (!kdmUtils::ReadFileToMemory(driverPath, &raw)) {
        fwprintf(stderr, L"[-] read .sys failed\n");
        intel_driver::Unload();
        return 2;
    }

    CallbackState cbs;
    g_cbState = &cbs;

    kdmapper::AllocationMode mode = useIndPages
        ? kdmapper::AllocationMode::AllocateIndependentPages
        : kdmapper::AllocationMode::AllocatePool;

    NTSTATUS driverEntryStatus = 0;
    ULONG64 base = kdmapper::MapDriver(
        raw.data(),
        0, 0,
        /*free*/ false,
        /*destroyHeader*/ !copyHeader,
        mode,
        passAllocPtr,
        MapperCallback,
        &driverEntryStatus);

    if (base == 0) {
        fwprintf(stderr, L"[-] MapDriver failed\n");
        intel_driver::Unload();
        return 3;
    }

    if (!cbs.captured) {
        // Shouldn't happen - callback runs unconditionally inside MapDriver.
        // Fall back to base as allocationPtr, but we can't recover size.
        cbs.allocationPtr  = base;
        cbs.allocationSize = 0;
        fwprintf(stderr, L"[!] callback didn't fire; size unknown - kdunmap will fail\n");
    }

    wprintf(L"[+] mapped: base=0x%llX size=0x%llX entry-status=0x%08X\n",
            (unsigned long long)cbs.allocationPtr,
            (unsigned long long)cbs.allocationSize,
            (unsigned)driverEntryStatus);

    // ---- Write tracking record to HKLM\SOFTWARE\kdmap_tracker\<key> --------
    std::wstring subkey = KdmapBuildRegSubkey(keyName);
    HKEY hk = NULL;
    DWORD disp = 0;
    LONG rs = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, NULL,
                              REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                              &hk, &disp);
    if (rs != ERROR_SUCCESS) {
        fwprintf(stderr, L"[-] RegCreateKeyEx failed (%ld) for HKLM\\%ls\n",
                 rs, subkey.c_str());
        // The driver IS mapped successfully; just no tracking. User can still
        // use it; kdunmap won't know how to free it.
        intel_driver::Unload();
        return 4;
    }

    KdmapRegWriteQword(hk, KDMAP_VAL_KERNEL_BASE, cbs.allocationPtr);
    KdmapRegWriteDword(hk, KDMAP_VAL_KERNEL_SIZE, (ULONG)cbs.allocationSize);
    KdmapRegWriteSz(hk, KDMAP_VAL_ALLOC_MODE,
                    useIndPages ? L"AllocateIndependentPages" : L"AllocatePool");
    if (!stopEventName.empty()) {
        KdmapRegWriteSz(hk, KDMAP_VAL_STOP_EVENT, stopEventName);
    }
    KdmapRegWriteSz(hk, KDMAP_VAL_DRIVER_PATH,
                    std::filesystem::absolute(driverPath).wstring());

    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG nowFt = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    KdmapRegWriteQword(hk, KDMAP_VAL_MAPPED_AT, nowFt);

    RegCloseKey(hk);

    wprintf(L"[+] tracking record written to HKLM\\%ls\n", subkey.c_str());

    // ---- Unload Intel driver, exit ------------------------------------------
    if (!NT_SUCCESS(intel_driver::Unload())) {
        fwprintf(stderr, L"[!] intel_driver::Unload returned non-success\n");
    }
    wprintf(L"[+] success\n");
    return 0;
}
