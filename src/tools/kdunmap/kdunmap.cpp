//
// kdunmap.exe - frees a kernel allocation that kdmap.exe mapped earlier.
//
// Reads tracking record from HKLM\SOFTWARE\kdmap_tracker\<key>, signals the
// stored stop event, waits for the driver worker thread to release its kernel
// objects, then loads the Intel driver (iqvw64e.sys) and calls
// ExFreePool / MmFreeIndependentPages on the kernel allocation.
//
// Usage:
//   kdunmap.exe --key NAME [--skipWait] [--alreadyStopped]
//
//   --key NAME    Required. The same identifier kdmap.exe was called with.
//   --skipWait    Don't wait after signaling stop event (faster, but worker
//                 might still be touching the allocation -> potential BSOD).
//   --alreadyStopped
//                 Driver was already stopped by unload_f20.ps1. Do not signal
//                 the event again; just apply a final safety pause before free.
//
// Exit codes:
//   0   success: memory freed, registry cleaned
//   1   bad usage
//   2   no tracking record found (driver was not loaded by kdmap.exe)
//   3   Intel driver load failed
//   4   FreePool / FreeIndependentPages returned false
//   5   unsafe to free: stop event missing or worker did not confirm exit
//

#include <Windows.h>

#include <cstdio>
#include <string>

#include "intel_driver.hpp"

#include "../common.h"

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* xi) {
    if (xi && xi->ExceptionRecord) {
        fwprintf(stderr, L"[!!] Crash @ %p code=0x%08X\n",
                 xi->ExceptionRecord->ExceptionAddress,
                 xi->ExceptionRecord->ExceptionCode);
    }
    if (intel_driver::hDevice) intel_driver::Unload();
    return EXCEPTION_EXECUTE_HANDLER;
}

static void Usage() {
    fwprintf(stderr, L"\n[!] Usage:\n");
    fwprintf(stderr, L"    kdunmap.exe --key NAME [--skipWait] [--alreadyStopped]\n\n");
}

// Signal a named event and poll for cleanup (named event removed when worker
// closes its handle - that's how we know cleanup is complete).
static bool SignalAndWait(const std::wstring& eventName, DWORD maxWaitMs) {
    wprintf(L"[+] signaling event: %ls\n", eventName.c_str());
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (!h) {
        wprintf(L"[!] event not found - refusing to free blindly\n");
        return false;
    }
    SetEvent(h);
    CloseHandle(h);

    DWORD slept = 0;
    while (slept < maxWaitMs) {
        Sleep(100);
        slept += 100;
        HANDLE h2 = OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
        if (!h2) {
            wprintf(L"[+] driver released event handle after %u ms (worker exited)\n", slept);
            return true;
        }
        CloseHandle(h2);
    }
    wprintf(L"[!] event still present after %u ms - worker may still be busy\n", maxWaitMs);
    return false;
}

int wmain(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(CrashHandler);

    int keyIdx     = KdmapFindArg(argc, argv, L"key");
    bool skipWait       = KdmapFindArg(argc, argv, L"skipWait") > 0;
    bool alreadyStopped = KdmapFindArg(argc, argv, L"alreadyStopped") > 0;

    if (keyIdx < 0 || keyIdx + 1 >= argc) {
        Usage();
        return 1;
    }
    std::wstring keyName = argv[keyIdx + 1];
    std::wstring subkey  = KdmapBuildRegSubkey(keyName);

    // ---- Read tracking record ----------------------------------------------
    HKEY hk = NULL;
    LONG rs = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &hk);
    if (rs != ERROR_SUCCESS) {
        wprintf(L"[!] no tracking record at HKLM\\%ls (was kdmap.exe used?)\n",
                subkey.c_str());
        return 2;
    }

    ULONGLONG base = 0;
    ULONG sz = 0;
    std::wstring mode;
    std::wstring stopEvent;
    std::wstring driverPath;

    bool okBase = KdmapRegReadQword(hk, KDMAP_VAL_KERNEL_BASE, &base);
    bool okSize = KdmapRegReadDword(hk, KDMAP_VAL_KERNEL_SIZE, &sz);
    KdmapRegReadSz(hk, KDMAP_VAL_ALLOC_MODE,  &mode);
    KdmapRegReadSz(hk, KDMAP_VAL_STOP_EVENT,  &stopEvent);
    KdmapRegReadSz(hk, KDMAP_VAL_DRIVER_PATH, &driverPath);
    RegCloseKey(hk);

    if (!okBase || base == 0) {
        fwprintf(stderr, L"[-] tracking record missing KernelBase\n");
        return 2;
    }

    wprintf(L"[+] tracking record:\n");
    wprintf(L"    base       = 0x%llX\n", (unsigned long long)base);
    wprintf(L"    size       = 0x%X\n", sz);
    wprintf(L"    mode       = %ls\n", mode.c_str());
    wprintf(L"    stopEvent  = %ls\n", stopEvent.c_str());
    wprintf(L"    driverPath = %ls\n", driverPath.c_str());

    // ---- Tell the driver to release kernel objects and exit worker ----------
    if (alreadyStopped) {
        wprintf(L"[+] alreadyStopped set - caller confirmed worker exit\n");
        Sleep(1500);
    } else if (!stopEvent.empty() && !skipWait) {
        if (!SignalAndWait(stopEvent, 10000)) {
            fwprintf(stderr, L"[-] unsafe to free: worker stop was not confirmed\n");
            return 5;
        }
        // Additional safety pause - any final return path after worker cleanup
        // gets time to leave the manually mapped allocation before free.
        Sleep(1500);
    } else if (!stopEvent.empty() && skipWait) {
        HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, stopEvent.c_str());
        if (h) { SetEvent(h); CloseHandle(h); }
        wprintf(L"[+] stop event signaled (--skipWait; not waiting)\n");
    } else {
        fwprintf(stderr, L"[-] no stop event in tracking record - refusing blind free\n");
        return 5;
    }

    // ---- Load Intel driver, free, unload Intel driver -----------------------
    if (!NT_SUCCESS(intel_driver::Load())) {
        fwprintf(stderr, L"[-] intel_driver::Load failed\n");
        return 3;
    }

    bool freed = false;
    if (mode == L"AllocateIndependentPages") {
        if (sz == 0) {
            fwprintf(stderr, L"[-] IndependentPages free needs size, but size=0 in record\n");
            intel_driver::Unload();
            return 4;
        }
        wprintf(L"[+] calling MmFreeIndependentPages(0x%llX, 0x%X)\n",
                (unsigned long long)base, sz);
        freed = intel_driver::MmFreeIndependentPages(base, sz);
    } else {
        // Default to ExFreePool when mode unknown / "AllocatePool".
        wprintf(L"[+] calling FreePool(0x%llX)\n", (unsigned long long)base);
        freed = intel_driver::FreePool(base);
    }

    if (!intel_driver::Unload()) {
        fwprintf(stderr, L"[!] intel_driver::Unload returned non-success\n");
    }

    if (!freed) {
        fwprintf(stderr, L"[-] free FAILED - driver code remains in kernel memory\n");
        return 4;
    }

    wprintf(L"[+] free succeeded - kernel allocation released\n");

    // ---- Clean up tracking record ------------------------------------------
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, subkey.c_str());
    wprintf(L"[+] tracking record removed\n");
    wprintf(L"[+] full unmap complete (no reboot required)\n");
    return 0;
}
