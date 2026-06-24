#pragma once

//
// Common helpers shared by kdmap.exe and kdunmap.exe.
//
// Both tools wrap kdmapper's static lib (kdmapper_lib-Release.lib).
//
// kdmap.exe loads a driver and writes tracking info to:
//   HKLM\SOFTWARE\kdmap_tracker\<key>\
//       KernelBase       REG_QWORD  - kernel allocation base from MapDriver()
//       KernelSize       REG_DWORD  - allocation size (already minus stripped header)
//       AllocationMode   REG_SZ     - "AllocateIndependentPages" or "AllocatePool"
//       StopEventName    REG_SZ     - named event the driver listens for clean shutdown
//       DriverPath       REG_SZ     - absolute path to the .sys (informational)
//       MappedAt         REG_QWORD  - FILETIME of map operation
//
// kdunmap.exe reads them and frees the kernel allocation via the same Intel
// driver exploit (kdmapper's intel_driver namespace).
//

#include <Windows.h>
#include <string>

#define KDMAP_REG_ROOT_W       L"SOFTWARE\\kdmap_tracker"
#define KDMAP_VAL_KERNEL_BASE  L"KernelBase"
#define KDMAP_VAL_KERNEL_SIZE  L"KernelSize"
#define KDMAP_VAL_ALLOC_MODE   L"AllocationMode"
#define KDMAP_VAL_STOP_EVENT   L"StopEventName"
#define KDMAP_VAL_DRIVER_PATH  L"DriverPath"
#define KDMAP_VAL_MAPPED_AT    L"MappedAt"

inline std::wstring KdmapBuildRegSubkey(const std::wstring& key) {
    std::wstring s = KDMAP_REG_ROOT_W;
    s += L"\\";
    s += key;
    return s;
}

inline bool KdmapRegWriteQword(HKEY hk, LPCWSTR name, ULONGLONG value) {
    return RegSetValueExW(hk, name, 0, REG_QWORD,
                          (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS;
}
inline bool KdmapRegWriteDword(HKEY hk, LPCWSTR name, ULONG value) {
    return RegSetValueExW(hk, name, 0, REG_DWORD,
                          (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS;
}
inline bool KdmapRegWriteSz(HKEY hk, LPCWSTR name, const std::wstring& s) {
    DWORD bytes = (DWORD)((s.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(hk, name, 0, REG_SZ,
                          (const BYTE*)s.c_str(), bytes) == ERROR_SUCCESS;
}

inline bool KdmapRegReadQword(HKEY hk, LPCWSTR name, ULONGLONG* out) {
    DWORD type = 0; DWORD sz = sizeof(*out);
    if (RegQueryValueExW(hk, name, NULL, &type, (BYTE*)out, &sz) != ERROR_SUCCESS) return false;
    return type == REG_QWORD && sz == sizeof(*out);
}
inline bool KdmapRegReadDword(HKEY hk, LPCWSTR name, ULONG* out) {
    DWORD type = 0; DWORD sz = sizeof(*out);
    if (RegQueryValueExW(hk, name, NULL, &type, (BYTE*)out, &sz) != ERROR_SUCCESS) return false;
    return type == REG_DWORD && sz == sizeof(*out);
}
inline bool KdmapRegReadSz(HKEY hk, LPCWSTR name, std::wstring* out) {
    DWORD type = 0; DWORD sz = 0;
    if (RegQueryValueExW(hk, name, NULL, &type, NULL, &sz) != ERROR_SUCCESS) return false;
    if (type != REG_SZ || sz == 0) return false;
    std::wstring s(sz / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hk, name, NULL, NULL, (BYTE*)s.data(), &sz) != ERROR_SUCCESS) return false;
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    *out = std::move(s);
    return true;
}

inline int KdmapFindArg(int argc, wchar_t** argv, const wchar_t* name) {
    size_t nlen = wcslen(name);
    for (int i = 1; i < argc; i++) {
        if (wcslen(argv[i]) == nlen + 2 &&
            argv[i][0] == L'-' && argv[i][1] == L'-' &&
            _wcsicmp(&argv[i][2], name) == 0) {
            return i;
        }
        if (wcslen(argv[i]) == nlen + 1 &&
            argv[i][0] == L'/' &&
            _wcsicmp(&argv[i][1], name) == 0) {
            return i;
        }
    }
    return -1;
}
