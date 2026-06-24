// ============================================================================
// F20Driver - kernel-mode CS2 round-kill detector + kbdclass keyboard inject.
//
// Behavior:
//   - Polls round_kills counter in cs2 every POLL_INTERVAL_MS via MmCopyVirtualMemory.
//   - On each detected kill (delta > 0):
//       * presses F20 (held statically for KILL_HOLD_MS = 2.5 sec)
//       * taps one Numpad key for NUMPAD_TAP_MS, ALTERNATING between the
//         POSITIVE and NEGATIVE yaw pools (rule #3: "1 kill positive value,
//         1 kill negative value and so on"); inside the chosen pool the key
//         is uniform-random over 5 candidates, never the previous one.
//   - Kills detected while F20 is still held are dropped (== 2.5 sec cooldown).
//
// Keyboard injection uses kbdclass!KeyboardClassServiceCallback located via
// a usermode analyzer (analyze_kbdclass.exe) that writes the RVA to
// HKLM\SOFTWARE\F20Driver. If the registry entry is missing/stale the driver
// runs in monitor-only mode (no inject, no BSOD).
//
// kdmapper-style manual map (no DriverUnload). No SHM, no overlay - this
// version is headless. Stopped via Global\F20DriverStop named event.
// ============================================================================

#pragma warning(disable: 4505 4100 28251 28252 28253)

#pragma warning(push, 0)
#include <ntifs.h>
#include <ntddk.h>
#include <ntdef.h>
#include <intrin.h>
#include <ntimage.h>
#include <ntddkbd.h>      // IOCTL_KEYBOARD_QUERY_INDICATORS, KEYBOARD_INDICATOR_PARAMETERS, KEYBOARD_NUM_LOCK_ON
#include <bcrypt.h>
#pragma warning(pop)

#define F20DRIVER_VERSION_STRING "v9 (alternating POS/NEG yaw pools per OPSEC rule #3)"

// ---- CS2 offsets fallback (a2x/cs2-dumper main) ---------------------------
// START.bat updates these from github:a2x/cs2-dumper into
// HKLM\SOFTWARE\F20Driver. These constants are only fallback values for
// offline/no-network runs.
#define DEFAULT_OFF_CTRL          0x2320720ULL
#define DEFAULT_OFF_TRACK         0x818ULL
#define DEFAULT_OFF_ROUND_KILLS   0x128ULL

// ---- Settings -------------------------------------------------------------
// Kill behavior is now strictly: F20 held exactly KILL_HOLD_MS, then released.
// Further kills while F20 is still held are ignored (== 2.5s cooldown).
// On every accepted kill we also tap one random Numpad 0..9 key, but never
// the same one twice in a row. The tap is held briefly so games polling input
// once per frame do not miss a zero-length make/break pair.
#define KILL_HOLD_MS        2500          // static F20 hold
#define KILL_SCAN_CODE      0x6B          // F20 set-1 scan code
#define NUMLOCK_SCAN_CODE   0x45          // NumLock set-1 scan code (no E0 prefix)
#define NUMPAD_TAP_MS       55            // random Numpad key hold
#define POLL_INTERVAL_MS    10

// Numpad set-1 scan codes (no E0 prefix, NumLock-on style), split into two
// pools by the sign of the yaw offset that the user wired in their cheat
// loadout. The picker below ALTERNATES between pools on every kill so that we
// never produce two consecutive positive (or two consecutive negative) yaw
// changes -- this is the rule "1 kill positive value, 1 kill negative value
// and so on" from the OPSEC checklist (rule #3, "Change Yaw Local view with
// MOUSE OVERRIDE after every kill, recommended between -15..15").
//
// Layout matches the loadout shown in the user's bind table:
//
//   Pool        Numpad   Scan   Bound yaw
//   POSITIVE    Num1     0x4F   +16
//               Num2     0x50   +12
//               Num4     0x4B    +8
//               Num6     0x4D    +4
//               Num8     0x48    +1
//   NEGATIVE    Num0     0x52   -16
//               Num3     0x51   -12
//               Num5     0x4C    -8
//               Num7     0x47    -4
//               Num9     0x49    -1
//
// If the user changes their yaw bind layout, this table must be updated.
#define NUMPAD_POOL_SIZE 5
static const USHORT g_NumpadScansPositive[NUMPAD_POOL_SIZE] = {
    0x4F, // Numpad1  -> +16
    0x50, // Numpad2  -> +12
    0x4B, // Numpad4  -> +8
    0x4D, // Numpad6  -> +4
    0x48, // Numpad8  -> +1
};
static const USHORT g_NumpadScansNegative[NUMPAD_POOL_SIZE] = {
    0x52, // Numpad0  -> -16
    0x51, // Numpad3  -> -12
    0x4C, // Numpad5  -> -8
    0x47, // Numpad7  -> -4
    0x49, // Numpad9  -> -1
};

// ---- Logging --------------------------------------------------------------
#define LOG_INFO(fmt, ...)  DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "[F20Drv] "       fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "[F20Drv] WARN: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "[F20Drv] ERR:  " fmt "\n", ##__VA_ARGS__)

// ---- External kernel APIs -------------------------------------------------
extern "C" {
    NTKERNELAPI PPEB NTAPI PsGetProcessPeb(_In_ PEPROCESS Process);
    NTSTATUS NTAPI ZwQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
    NTSTATUS NTAPI MmCopyVirtualMemory(PEPROCESS, PVOID, PEPROCESS, PVOID,
                                       SIZE_T, KPROCESSOR_MODE, PSIZE_T);
    NTKERNELAPI POBJECT_TYPE* IoDriverObjectType;
    NTKERNELAPI NTSTATUS NTAPI ObReferenceObjectByName(
        PUNICODE_STRING ObjectName, ULONG Attributes, PACCESS_STATE AccessState,
        ACCESS_MASK DesiredAccess, POBJECT_TYPE ObjectType,
        KPROCESSOR_MODE AccessMode, PVOID ParseContext, PVOID* Object);
}

// KEYBOARD_INPUT_DATA, KEY_MAKE, KEY_BREAK come from <ntddkbd.h>.

typedef VOID(NTAPI* PKBD_SERVICE_CALLBACK)(
    PDEVICE_OBJECT       DeviceObject,
    PKEYBOARD_INPUT_DATA InputDataStart,
    PKEYBOARD_INPUT_DATA InputDataEnd,
    PULONG               InputDataConsumed);

// ---- Module/process structs -----------------------------------------------
typedef struct _RTL_PROCESS_MODULE_INFORMATION {
    HANDLE Section; PVOID MappedBase; PVOID ImageBase; ULONG ImageSize;
    ULONG Flags; USHORT LoadOrderIndex; USHORT InitOrderIndex; USHORT LoadCount;
    USHORT OffsetToFileName; UCHAR FullPathName[256];
} RTL_PROCESS_MODULE_INFORMATION;
typedef struct _RTL_PROCESS_MODULES {
    ULONG NumberOfModules;
    RTL_PROCESS_MODULE_INFORMATION Modules[1];
} RTL_PROCESS_MODULES;

typedef struct _SYSTEM_PROCESS_INFORMATION_S {
    ULONG NextEntryOffset; ULONG NumberOfThreads; UCHAR Reserved1[48];
    UNICODE_STRING ImageName; KPRIORITY BasePriority; HANDLE UniqueProcessId;
    PVOID Reserved2; ULONG HandleCount; ULONG SessionId; PVOID Reserved3;
    SIZE_T PeakVirtualSize; SIZE_T VirtualSize; ULONG Reserved4;
    SIZE_T PeakWorkingSetSize; SIZE_T WorkingSetSize; PVOID Reserved5;
    SIZE_T QuotaPagedPoolUsage; PVOID Reserved6; SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage; SIZE_T PeakPagefileUsage; SIZE_T PrivatePageCount;
    LARGE_INTEGER Reserved7[6];
} SYSTEM_PROCESS_INFORMATION_S, *PSYSTEM_PROCESS_INFORMATION_S;

typedef struct _PEB_LDR_DATA_S {
    ULONG Length; UCHAR Initialized; PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
} PEB_LDR_DATA_S, *PPEB_LDR_DATA_S;
typedef struct _LDR_DATA_TABLE_ENTRY_S {
    LIST_ENTRY InLoadOrderLinks; LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks; PVOID DllBase; PVOID EntryPoint;
    ULONG SizeOfImage; UNICODE_STRING FullDllName; UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY_S, *PLDR_DATA_TABLE_ENTRY_S;
typedef struct _PEB_S {
    UCHAR InheritedAddressSpace; UCHAR ReadImageFileExecOptions;
    UCHAR BeingDebugged; UCHAR BitField; PVOID Mutant; PVOID ImageBaseAddress;
    PPEB_LDR_DATA_S Ldr;
} PEB_S, *PPEB_S;

// ---- Globals --------------------------------------------------------------
// Single-entry guard: kdmapper temporary DRIVER_OBJECT is not reusable, but
// nothing prevents a second kdmap of the same image from re-running DriverEntry,
// which would re-register PsSetCreateProcessNotifyRoutine, alias the named
// events via OBJ_OPENIF and spawn a duplicate worker. Fail fast in that case.
static LONG    g_DriverEntered     = 0;

static KEVENT  g_StopEvent;
static HANDLE  g_UnloadEventHandle = NULL;
static PKEVENT g_UnloadEvent       = NULL;
static HANDLE  g_DoneEventHandle   = NULL;
static PKEVENT g_DoneEvent         = NULL;

static volatile LONG g_RngSeed = 0;
// Picker state for alternating yaw injection.
//   g_LastSignPositive   - sign of the previous tap. We flip this every kill so
//                          the new tap is from the opposite-sign pool. Initial
//                          value TRUE means the very first kill picks NEGATIVE
//                          (matching the "1 kill positive, 1 kill negative"
//                          phrasing where positive is the leading example).
//   g_LastPositiveIdx /  - per-pool last index, so we never repeat the same
//   g_LastNegativeIdx      Numpad key twice in a row inside the same pool.
//                          0xFFFFFFFF = no previous pick for that pool yet.
static BOOLEAN g_LastSignPositive = TRUE;
static ULONG   g_LastPositiveIdx  = 0xFFFFFFFF;
static ULONG   g_LastNegativeIdx  = 0xFFFFFFFF;

// Runtime-detected OS version (set in DriverEntry via RtlGetVersion).
static BOOLEAN g_IsWin10        = FALSE;
static ULONG   g_OsMajor        = 0;
static ULONG   g_OsMinor        = 0;
static ULONG   g_OsBuild        = 0;

static volatile HANDLE g_cs2Pid     = NULL;
static volatile LONG   g_cs2Exiting = 0;
static BOOLEAN         g_ProcessNotifyRegistered = FALSE;

static ULONG_PTR g_OffCtrl       = DEFAULT_OFF_CTRL;
static ULONG     g_OffTrack      = DEFAULT_OFF_TRACK;
static ULONG     g_OffRoundKills = DEFAULT_OFF_ROUND_KILLS;

// Keyboard inject (gated by g_KbdSafe — FALSE = inject is a no-op)
#define MAX_KBD_TARGETS 16
typedef struct _KBD_INJECT_TARGET {
    PDEVICE_OBJECT Device;
    USHORT         UnitId;
} KBD_INJECT_TARGET;

static BOOLEAN               g_KbdSafe       = FALSE;
static PKBD_SERVICE_CALLBACK g_KbdCallback   = NULL;
static KBD_INJECT_TARGET     g_KbdTargets[MAX_KBD_TARGETS] = {};
static ULONG                 g_KbdTargetCount = 0;
static PVOID                 g_KbdTextStart   = NULL;
static ULONG                 g_KbdTextSize    = 0;

static HANDLE ReadTrackedCs2Pid(void) {
    return (HANDLE)InterlockedCompareExchangePointer((PVOID volatile*)&g_cs2Pid, NULL, NULL);
}

static VOID SetTrackedCs2Pid(HANDLE Pid) {
    InterlockedExchangePointer((PVOID volatile*)&g_cs2Pid, (PVOID)Pid);
}

static VOID ClearTrackedCs2Pid(void) {
    InterlockedExchange(&g_cs2Exiting, 0);
    SetTrackedCs2Pid(NULL);
}

static VOID ProcessExitCallback(HANDLE Parent, HANDLE Pid, BOOLEAN Create) {
    UNREFERENCED_PARAMETER(Parent);
    HANDLE trackedPid = ReadTrackedCs2Pid();
    if (!Create && trackedPid && Pid == trackedPid)
        InterlockedExchange(&g_cs2Exiting, 1);
}

// ============================================================================
// Build a security descriptor that grants EVENT_ALL_ACCESS only to SYSTEM and
// the local Administrators group. Replaces the previous NULL-DACL pattern which
// allowed any process to signal F20DriverStop and kill the driver.
//
// Caller must NOT free the returned allocation while the SD is in use; pass it
// back to FreeAdminOnlySd after the OBJECT_ATTRIBUTES is no longer referenced.
// ============================================================================
typedef struct _ADMIN_ONLY_SD {
    SECURITY_DESCRIPTOR sd;
    PACL                dacl;
    PSID                adminSid;
    PSID                systemSid;
} ADMIN_ONLY_SD, *PADMIN_ONLY_SD;

static NTSTATUS BuildAdminOnlySd(PADMIN_ONLY_SD ctx) {
    if (!ctx) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(ctx, sizeof(*ctx));

    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    ULONG adminSidSize  = RtlLengthRequiredSid(2);
    ULONG systemSidSize = RtlLengthRequiredSid(1);

    ctx->adminSid  = (PSID)ExAllocatePool2(POOL_FLAG_NON_PAGED, adminSidSize,  'f20s');
    ctx->systemSid = (PSID)ExAllocatePool2(POOL_FLAG_NON_PAGED, systemSidSize, 'f20s');
    if (!ctx->adminSid || !ctx->systemSid) goto fail;

    NTSTATUS s;
    s = RtlInitializeSid(ctx->adminSid, &ntAuth, 2);
    if (!NT_SUCCESS(s)) goto fail;
    *RtlSubAuthoritySid(ctx->adminSid, 0) = SECURITY_BUILTIN_DOMAIN_RID;
    *RtlSubAuthoritySid(ctx->adminSid, 1) = DOMAIN_ALIAS_RID_ADMINS;

    s = RtlInitializeSid(ctx->systemSid, &ntAuth, 1);
    if (!NT_SUCCESS(s)) goto fail;
    *RtlSubAuthoritySid(ctx->systemSid, 0) = SECURITY_LOCAL_SYSTEM_RID;

    ULONG daclSize = sizeof(ACL) +
                     2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG)) +
                     adminSidSize + systemSidSize;
    ctx->dacl = (PACL)ExAllocatePool2(POOL_FLAG_NON_PAGED, daclSize, 'f20s');
    if (!ctx->dacl) goto fail;

    s = RtlCreateAcl(ctx->dacl, daclSize, ACL_REVISION);
    if (!NT_SUCCESS(s)) goto fail;
    s = RtlAddAccessAllowedAce(ctx->dacl, ACL_REVISION, EVENT_ALL_ACCESS, ctx->systemSid);
    if (!NT_SUCCESS(s)) goto fail;
    s = RtlAddAccessAllowedAce(ctx->dacl, ACL_REVISION, EVENT_ALL_ACCESS, ctx->adminSid);
    if (!NT_SUCCESS(s)) goto fail;

    s = RtlCreateSecurityDescriptor(&ctx->sd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(s)) goto fail;
    s = RtlSetDaclSecurityDescriptor(&ctx->sd, TRUE, ctx->dacl, FALSE);
    if (!NT_SUCCESS(s)) goto fail;

    return STATUS_SUCCESS;

fail:
    if (ctx->dacl)      { ExFreePoolWithTag(ctx->dacl, 'f20s');      ctx->dacl = NULL; }
    if (ctx->adminSid)  { ExFreePoolWithTag(ctx->adminSid, 'f20s');  ctx->adminSid = NULL; }
    if (ctx->systemSid) { ExFreePoolWithTag(ctx->systemSid, 'f20s'); ctx->systemSid = NULL; }
    return STATUS_UNSUCCESSFUL;
}

static VOID FreeAdminOnlySd(PADMIN_ONLY_SD ctx) {
    if (!ctx) return;
    if (ctx->dacl)      { ExFreePoolWithTag(ctx->dacl, 'f20s');      ctx->dacl = NULL; }
    if (ctx->adminSid)  { ExFreePoolWithTag(ctx->adminSid, 'f20s');  ctx->adminSid = NULL; }
    if (ctx->systemSid) { ExFreePoolWithTag(ctx->systemSid, 'f20s'); ctx->systemSid = NULL; }
}

// ============================================================================
// Wait / stop
// ============================================================================
static BOOLEAN StopRequested(void) {
    if (KeReadStateEvent(&g_StopEvent) != 0) return TRUE;
    if (g_UnloadEvent && KeReadStateEvent(g_UnloadEvent) != 0) {
        KeSetEvent(&g_StopEvent, IO_NO_INCREMENT, FALSE);
        return TRUE;
    }
    return FALSE;
}
static BOOLEAN WaitOrStop(LONGLONG Ms) {
    if (Ms <= 0) return StopRequested();
    if (!g_UnloadEvent) {
        LARGE_INTEGER d; d.QuadPart = -(Ms * 10000LL);
        NTSTATUS s = KeWaitForSingleObject(&g_StopEvent, Executive, KernelMode, FALSE, &d);
        return s != STATUS_TIMEOUT;
    }
    PVOID objects[2] = { &g_StopEvent, g_UnloadEvent };
    LARGE_INTEGER d; d.QuadPart = -(Ms * 10000LL);
    NTSTATUS s = KeWaitForMultipleObjects(2, objects, WaitAny, Executive,
                                          KernelMode, FALSE, &d, NULL);
    if (s == STATUS_TIMEOUT) return FALSE;
    if (g_UnloadEvent && KeReadStateEvent(g_UnloadEvent) != 0)
        KeSetEvent(&g_StopEvent, IO_NO_INCREMENT, FALSE);
    return TRUE;
}

// ============================================================================
// Memory helpers
// ============================================================================
static BOOLEAN IsAddrValid(PVOID a) {
    if (!a) return FALSE;
    if ((ULONG_PTR)a < 0x10000) return FALSE;
    if ((ULONG_PTR)a > 0x7FFFFFFFFFFF) return FALSE;
    return TRUE;
}
static NTSTATUS ReadProcMem(PEPROCESS Process, PVOID Address, PVOID Buf, SIZE_T Size) {
    if (!Process || !Address || !Buf || !Size) return STATUS_INVALID_PARAMETER;
    if (!IsAddrValid(Address)) return STATUS_INVALID_ADDRESS;
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) return STATUS_UNSUCCESSFUL;
    if (InterlockedOr(&g_cs2Exiting, 0)) return STATUS_PROCESS_IS_TERMINATING;
    if (PsGetProcessExitStatus(Process) != STATUS_PENDING)
        return STATUS_PROCESS_IS_TERMINATING;
    SIZE_T copied = 0;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    __try {
        st = MmCopyVirtualMemory(Process, Address, PsGetCurrentProcess(),
                                 Buf, Size, KernelMode, &copied);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
        LOG_WARN("ReadProcMem SEH 0x%X addr=%p size=%llu", st, Address,
                 (unsigned long long)Size);
    }
    return st;
}

static BOOLEAN QueryRegU64(HANDLE hKey, PCWSTR name, ULONGLONG* out) {
    if (!hKey || !name || !out) return FALSE;

    UCHAR buf[128];
    ULONG retLen = 0;
    UNICODE_STRING vn;
    RtlInitUnicodeString(&vn, name);

    NTSTATUS s = ZwQueryValueKey(hKey, &vn, KeyValuePartialInformation,
                                 buf, sizeof(buf), &retLen);
    if (!NT_SUCCESS(s)) return FALSE;

    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    if (info->Type == REG_QWORD && info->DataLength == sizeof(ULONGLONG)) {
        *out = *(ULONGLONG*)info->Data;
        return TRUE;
    }
    if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
        *out = *(ULONG*)info->Data;
        return TRUE;
    }
    return FALSE;
}

static VOID LoadCs2OffsetsFromRegistry(void) {
    g_OffCtrl       = DEFAULT_OFF_CTRL;
    g_OffTrack      = DEFAULT_OFF_TRACK;
    g_OffRoundKills = DEFAULT_OFF_ROUND_KILLS;

    UNICODE_STRING keyPath;
    RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\SOFTWARE\\F20Driver");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    HANDLE hKey = NULL;
    NTSTATUS s = ZwOpenKey(&hKey, KEY_READ, &oa);
    if (!NT_SUCCESS(s)) {
        LOG_WARN("CS2 offsets registry missing - using built-in fallback");
        LOG_INFO("CS2 offsets: ctrl=0x%llX track=0x%X kills=0x%X",
                 (unsigned long long)g_OffCtrl, g_OffTrack, g_OffRoundKills);
        return;
    }

    ULONGLONG ctrl = 0, track = 0, kills = 0;
    BOOLEAN ok = QueryRegU64(hKey, L"Cs2DwLocalPlayerController", &ctrl) &&
                 QueryRegU64(hKey, L"Cs2M_pActionTrackingServices", &track) &&
                 QueryRegU64(hKey, L"Cs2M_iNumRoundKills", &kills);
    ZwClose(hKey);

    if (!ok ||
        ctrl < 0x100000 || ctrl > 0x10000000 ||
        track == 0 || track > 0x100000 ||
        kills == 0 || kills > 0x10000) {
        LOG_WARN("CS2 offsets registry invalid/missing - using built-in fallback");
        LOG_INFO("CS2 offsets: ctrl=0x%llX track=0x%X kills=0x%X",
                 (unsigned long long)g_OffCtrl, g_OffTrack, g_OffRoundKills);
        return;
    }

    g_OffCtrl       = (ULONG_PTR)ctrl;
    g_OffTrack      = (ULONG)track;
    g_OffRoundKills = (ULONG)kills;
    LOG_INFO("CS2 offsets from registry: ctrl=0x%llX track=0x%X kills=0x%X",
             (unsigned long long)g_OffCtrl, g_OffTrack, g_OffRoundKills);
}

// ============================================================================
// Process / module lookup (in target process)
// ============================================================================
static PEPROCESS FindProcessByName(LPCWSTR Name) {
    ULONG bytes = 0;
    ZwQuerySystemInformation(5, NULL, 0, &bytes);
    if (!bytes) return NULL;
    PVOID buf = NULL; NTSTATUS s = STATUS_UNSUCCESSFUL;
    for (int a = 0; a < 5; a++) {
        ULONG sz = bytes + 8192; if (sz > 64u * 1024u * 1024u) return NULL;
        buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, 'f20p');
        if (!buf) return NULL;
        s = ZwQuerySystemInformation(5, buf, sz, &bytes);
        if (NT_SUCCESS(s)) break;
        ExFreePoolWithTag(buf, 'f20p'); buf = NULL;
        if (s != STATUS_INFO_LENGTH_MISMATCH) return NULL;
    }
    if (!buf) return NULL;
    PSYSTEM_PROCESS_INFORMATION_S p = (PSYSTEM_PROCESS_INFORMATION_S)buf;
    PEPROCESS proc = NULL;
    UNICODE_STRING uName; RtlInitUnicodeString(&uName, Name);
    __try {
        for (ULONG i = 0; i < 10000; i++) {
            if (p->ImageName.Buffer &&
                RtlCompareUnicodeString(&p->ImageName, &uName, TRUE) == 0) {
                if (NT_SUCCESS(PsLookupProcessByProcessId(p->UniqueProcessId, &proc)))
                    LOG_INFO("Found %ws PID=%p", Name, p->UniqueProcessId);
                break;
            }
            if (!p->NextEntryOffset) break;
            p = (PSYSTEM_PROCESS_INFORMATION_S)((PUCHAR)p + p->NextEntryOffset);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { proc = NULL; }
    ExFreePoolWithTag(buf, 'f20p');
    return proc;
}

static PVOID GetModuleBase(PEPROCESS Process, UNICODE_STRING Name) {
    if (!Process) return NULL;
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) return NULL;
    if (InterlockedOr(&g_cs2Exiting, 0)) return NULL;
    if (PsGetProcessExitStatus(Process) != STATUS_PENDING) return NULL;
    PPEB_S pebAddr = (PPEB_S)PsGetProcessPeb(Process);
    if (!pebAddr) return NULL;
    PVOID base = NULL;
    __try {
        PEB_S peb = {};
        if (!NT_SUCCESS(ReadProcMem(Process, pebAddr, &peb, sizeof(peb)))) return NULL;
        if (!peb.Ldr) return NULL;
        PEB_LDR_DATA_S ldr = {};
        if (!NT_SUCCESS(ReadProcMem(Process, peb.Ldr, &ldr, sizeof(ldr)))) return NULL;
        PLIST_ENTRY head = &peb.Ldr->InLoadOrderModuleList;
        PLIST_ENTRY curN = ldr.InLoadOrderModuleList.Flink;
        for (ULONG i = 0; i < 512 && curN && curN != head; i++) {
            LDR_DATA_TABLE_ENTRY_S e = {};
            PVOID entryAddr = CONTAINING_RECORD(curN, LDR_DATA_TABLE_ENTRY_S, InLoadOrderLinks);
            if (!NT_SUCCESS(ReadProcMem(Process, entryAddr, &e, sizeof(e)))) break;
            if (e.BaseDllName.Length && e.BaseDllName.Buffer) {
                WCHAR nbuf[64] = {};
                SIZE_T nb = min((SIZE_T)e.BaseDllName.Length, sizeof(nbuf) - sizeof(WCHAR));
                if (NT_SUCCESS(ReadProcMem(Process, e.BaseDllName.Buffer, nbuf, nb))) {
                    UNICODE_STRING en;
                    en.Buffer = nbuf; en.Length = (USHORT)nb; en.MaximumLength = sizeof(nbuf);
                    if (RtlCompareUnicodeString(&en, &Name, TRUE) == 0) {
                        base = e.DllBase; break;
                    }
                }
            }
            curN = e.InLoadOrderLinks.Flink;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("GetModuleBase SEH 0x%X", GetExceptionCode());
        base = NULL;
    }
    return base;
}

static BOOLEAN IsProcessAlive(PEPROCESS p) {
    if (!p) return FALSE;
    __try {
        if (PsGetProcessExitStatus(p) != STATUS_PENDING) return FALSE;
        HANDLE pid = PsGetProcessId(p);
        if (!pid || pid == (HANDLE)-1) return FALSE;
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}

// ============================================================================
// kbdclass module discovery + .text section bounds
// ============================================================================
static BOOLEAN FindKbdClass(PVOID* OutBase, ULONG* OutSize) {
    ULONG bytes = 0;
    ZwQuerySystemInformation(11, NULL, 0, &bytes);
    if (!bytes) return FALSE;
    PVOID buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes + 8192, 'f20m');
    if (!buf) return FALSE;
    BOOLEAN found = FALSE;
    if (NT_SUCCESS(ZwQuerySystemInformation(11, buf, bytes + 8192, &bytes))) {
        RTL_PROCESS_MODULES* mods = (RTL_PROCESS_MODULES*)buf;
        for (ULONG i = 0; i < mods->NumberOfModules; i++) {
            const char* name = (const char*)mods->Modules[i].FullPathName
                             + mods->Modules[i].OffsetToFileName;
            if (_stricmp(name, "kbdclass.sys") == 0) {
                *OutBase = mods->Modules[i].ImageBase;
                *OutSize = mods->Modules[i].ImageSize;
                LOG_INFO("kbdclass.sys at %p size 0x%X", *OutBase, *OutSize);
                found = TRUE;
                break;
            }
        }
    }
    ExFreePoolWithTag(buf, 'f20m');
    return found;
}

// Parse kbdclass PE headers — record .text bounds AND TimeDateStamp+SizeOfImage
// for comparison against the analyzer-recorded registry fingerprint.
static ULONG g_KbdTimestamp = 0;
static ULONG g_KbdImageSize = 0;

// Forward decl (defined below) — used by TryLoadRvaFromRegistry
static BOOLEAN AddrInText(PVOID a);
static BOOLEAN ReadableTextRange(PVOID a, ULONG len);

static BOOLEAN ParseKbdTextBounds(PVOID base) {
    if (!base) return FALSE;
    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
        PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)((PUCHAR)base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
        g_KbdTimestamp = nt->FileHeader.TimeDateStamp;
        g_KbdImageSize = nt->OptionalHeader.SizeOfImage;
        PIMAGE_SECTION_HEADER sect = IMAGE_FIRST_SECTION(nt);
        for (ULONG i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (RtlCompareMemory(sect[i].Name, ".text", 5) == 5) {
                g_KbdTextStart = (PVOID)((PUCHAR)base + sect[i].VirtualAddress);
                g_KbdTextSize  = sect[i].Misc.VirtualSize;
                LOG_INFO(".text: %p size 0x%X  TS=0x%08X  ImgSz=0x%X",
                         g_KbdTextStart, g_KbdTextSize,
                         g_KbdTimestamp, g_KbdImageSize);
                return TRUE;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("ParseKbdTextBounds exception: 0x%X", GetExceptionCode());
    }
    return FALSE;
}

// Forward decl - body lives further down with the sig table.
static BOOLEAN ValidateCallbackPrologue(PVOID hit, const char** OutSigName);

// ============================================================================
// Hash-keyed RVA database. When the registry includes KbdSha256 (analyzer
// writes it for every successful run) we compare it against this table; on
// exact match we treat the RVA as gold and skip prologue validation.
//
// Entries are populated as users send us kbdclass.sys + analyzer output from
// machines that succeed (or fail). sha256 is uppercase hex, no spaces.
// ============================================================================
typedef struct _KBDCLASS_KNOWN {
    const char* sha256;     // SHA256(kbdclass.sys), uppercase hex
    ULONG       rva;        // KeyboardClassServiceCallback RVA (image-base relative)
    const char* label;
} KBDCLASS_KNOWN;

// NOTE: The first entry is a placeholder that never matches (sha256 of empty
// string is 64 chars of zeros - but actual analyzer-produced hashes will never
// be all zeros). It exists so the array always has >=1 element, which keeps
// ARRAYSIZE happy.
// Auto-extracted from WinBindex + MS Symbol Server 2026-06-23.
// Each entry covers multiple Windows builds (kbdclass.sys often unchanged
// between OS feature updates).
//
// IMPORTANT: only entries with VERIFIED-good RVAs (functioning inject without
// BSOD) are listed here. Win10 22H2 + Win11 22H2 hashes were excluded after
// a DRIVER_IRQL_NOT_LESS_OR_EQUAL BSOD on 2026-06-23 - the pattern that hit
// 0x1B50 / 0x1C20 was a similar-looking-but-wrong function in kbdclass.
// PDB-sourced analyzer results cover the excluded Win10/Win11 22H2 builds.
// Keep wrong-pattern hashes out of this table unless the RVA is verified good.
static const KBDCLASS_KNOWN g_KnownKbdclass[] = {
    // ---- Win11 24H2 (build 26100.x) - sig win11_24h2_4arg, verified working
    { "BA93360492D3076AE4D1FBD860C94AAB3942A9FEB582BB38B5DF63724C311836",
      0x22E0, "Win11 24H2 (10.0.26100.1)" },
    { "6A43DD8F7044C51654E769A8AE8C36727FDDD6AB1A647F3B030A2B949FE3FEE4",
      0x22E0, "Win11 24H2 (10.0.26100.1150)" },
    { "C3A037208BF21D29C09ADB1C16BB3B0210E5FA9351B1298778AFCBF71609E3F5",
      0x22E0, "Win11 24H2 (10.0.26100.1882)" },
    { "FAD240492829942947B860345F484DC7505186A8F95939887F745DBB806C3742",
      0x22E0, "Win11 24H2 (10.0.26100.8521)" },

    { "0000000000000000000000000000000000000000000000000000000000000000",
      0, "(placeholder - never matches)" },
};

static BOOLEAN HexCharEqual(char a, char b) {
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    return a == b;
}

// Case-insensitive sha256 hex compare. Both args must be 64 hex chars.
static BOOLEAN ShaEqualHex(const char* a, const char* b) {
    if (!a || !b) return FALSE;
    for (int i = 0; i < 64; i++) {
        if (!a[i] || !b[i]) return FALSE;
        if (!HexCharEqual(a[i], b[i])) return FALSE;
    }
    return TRUE;
}

// Returns RVA from the known-good table for given sha256, or 0 if not found.
static ULONG LookupKnownRva(const char* sha256_hex, const char** OutLabel)
{
    if (OutLabel) *OutLabel = NULL;
    if (!sha256_hex) return 0;
    for (ULONG i = 0; i < ARRAYSIZE(g_KnownKbdclass); i++) {
        if (ShaEqualHex(g_KnownKbdclass[i].sha256, sha256_hex)) {
            if (OutLabel) *OutLabel = g_KnownKbdclass[i].label;
            return g_KnownKbdclass[i].rva;
        }
    }
    return 0;
}

// ============================================================================
// Try to load KeyboardClassServiceCallback RVA from registry
// (HKLM\SOFTWARE\F20Driver, written by analyze_kbdclass.exe).
// Returns the RVA (relative to kbdclass image base) if all checks pass.
//
// Validation chain (each step is a gate; failure -> return 0 -> monitor-only):
//   1. registry exists, has CallbackRva/KbdTimestamp/KbdImageSize
//   2. TS + ImgSz match the kbdclass we just walked (fingerprint check)
//   3. RVA in range, points inside .text
//   4. EITHER sha256 from registry matches an entry in g_KnownKbdclass
//      (gold path - hardcoded RVA wins, even overrides registry RVA)
//      OR analyzer marked the RVA as PDB-sourced (Signature=pdb_symbol)
//      after resolving kbdclass!KeyboardClassServiceCallback from MS symbols
//      OR the bytes at RVA match one of the known KCSC prologues
//      (medium path - ValidateCallbackPrologue)
// ============================================================================
static ULONG TryLoadRvaFromRegistry(PVOID kbdBase) {
    if (!kbdBase || !g_KbdTimestamp || !g_KbdImageSize) return 0;

    UNICODE_STRING keyPath;
    RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\SOFTWARE\\F20Driver");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    HANDLE hKey = NULL;
    NTSTATUS s = ZwOpenKey(&hKey, KEY_READ, &oa);
    if (!NT_SUCCESS(s)) {
        LOG_INFO("Registry HKLM\\SOFTWARE\\F20Driver not found (0x%X)", s);
        return 0;
    }

    UCHAR  buf[512];
    ULONG  retLen = 0;
    ULONG  rva = 0, ts = 0, isz = 0;
    char   shaHex[65] = {};   // SHA256 hex = 64 chars + NUL
    char   signature[64] = {};
    UNICODE_STRING vn;

    auto readDword = [&](LPCWSTR name, ULONG* out) -> BOOLEAN {
        RtlInitUnicodeString(&vn, name);
        if (NT_SUCCESS(ZwQueryValueKey(hKey, &vn, KeyValuePartialInformation,
                                        buf, sizeof(buf), &retLen))) {
            PKEY_VALUE_PARTIAL_INFORMATION info =
                (PKEY_VALUE_PARTIAL_INFORMATION)buf;
            if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
                *out = *(ULONG*)info->Data;
                return TRUE;
            }
        }
        return FALSE;
    };

    auto readAsciiRegSz = [&](LPCWSTR name, char* out, ULONG outChars,
                              ULONG minChars) -> BOOLEAN {
        if (!out || outChars == 0) return FALSE;
        out[0] = 0;
        RtlInitUnicodeString(&vn, name);
        if (!NT_SUCCESS(ZwQueryValueKey(hKey, &vn, KeyValuePartialInformation,
                                         buf, sizeof(buf), &retLen)))
            return FALSE;
        PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (info->Type != REG_SZ) return FALSE;
        if (info->DataLength == 0) return FALSE;

        // New analyzer writes proper Unicode REG_SZ. Older C++ analyzer builds
        // wrote ANSI bytes via RegSetValueExA, so accept both encodings.
        if ((info->DataLength % sizeof(WCHAR)) == 0) {
            WCHAR* w = (WCHAR*)info->Data;
            ULONG nWChars = info->DataLength / sizeof(WCHAR);
            ULONG j = 0;
            BOOLEAN wideOk = TRUE;
            for (ULONG i = 0; i < nWChars && w[i] != 0; i++) {
                WCHAR c = w[i];
                if (c < 0x20 || c > 0x7E || j + 1 >= outChars) {
                    wideOk = FALSE;
                    break;
                }
                out[j++] = (char)c;
            }
            if (wideOk && j >= minChars) {
                out[j] = 0;
                return TRUE;
            }
        }

        UCHAR* a = (UCHAR*)info->Data;
        ULONG j = 0;
        for (ULONG i = 0; i < info->DataLength && a[i] != 0; i++) {
            UCHAR c = a[i];
            if (c < 0x20 || c > 0x7E || j + 1 >= outChars)
                return FALSE;
            out[j++] = (char)c;
        }
        if (j < minChars) return FALSE;
        out[j] = 0;
        return TRUE;
    };

    BOOLEAN ok = readDword(L"CallbackRva",  &rva) &&
                 readDword(L"KbdTimestamp", &ts ) &&
                 readDword(L"KbdImageSize", &isz);
    BOOLEAN haveSha = readAsciiRegSz(L"KbdSha256", shaHex, sizeof(shaHex), 64);
    BOOLEAN haveSignature = readAsciiRegSz(L"Signature", signature,
                                           sizeof(signature), 1);
    ZwClose(hKey);

    if (!ok) { LOG_WARN("Registry missing fields"); return 0; }
    if (ts != g_KbdTimestamp || isz != g_KbdImageSize) {
        LOG_WARN("Registry stale: TS=0x%08X(want 0x%08X) ImgSz=0x%X(want 0x%X)",
                 ts, g_KbdTimestamp, isz, g_KbdImageSize);
        return 0;
    }
    if (rva == 0 || rva >= g_KbdImageSize) {
        LOG_WARN("Registry RVA 0x%X out of range", rva);
        return 0;
    }

    PVOID hit = (PVOID)((PUCHAR)kbdBase + rva);
    if (!AddrInText(hit)) {
        LOG_WARN("Registry RVA 0x%X resolves outside .text - reject", rva);
        return 0;
    }

    // ---- Gate 1: known-good RVA from hash DB ------------------------------
    // If this exact kbdclass.sys hash is in our table, trust the hardcoded
    // RVA and OVERRIDE whatever analyzer wrote. (Analyzer might be wrong; the
    // db is hand-verified by us.)
    if (haveSha) {
        const char* label = NULL;
        ULONG knownRva = LookupKnownRva(shaHex, &label);
        if (knownRva != 0) {
            if (knownRva != rva) {
                LOG_WARN("Known kbdclass [%s]: db RVA=0x%X overrides registry RVA=0x%X",
                         label ? label : "?", knownRva, rva);
            } else {
                LOG_INFO("Known kbdclass [%s]: db RVA=0x%X matches registry",
                         label ? label : "?", knownRva);
            }
            return knownRva;
        }
        LOG_INFO("sha256=%s (not in known-good db)", shaHex);
    } else {
        LOG_WARN("Registry missing KbdSha256 - cannot consult known-good db");
    }

    // ---- Gate 2: PDB-sourced RVA ------------------------------------------
    // PDB symbols are the source of truth for builds where compiler-generated
    // KCSC prologues are not unique. Keep the fingerprint + .text + readable
    // code checks, but do not require a legacy byte signature.
    if (haveSignature && _stricmp(signature, "pdb_symbol") == 0) {
        if (!ReadableTextRange(hit, 64)) {
            LOG_ERROR("Registry PDB RVA 0x%X is not readable .text - REJECT", rva);
            return 0;
        }

        const char* matchedSig = NULL;
        if (ValidateCallbackPrologue(hit, &matchedSig)) {
            LOG_INFO("Registry HIT: RVA=0x%X from PDB, prologue also matches [%s]",
                     rva, matchedSig ? matchedSig : "?");
        } else {
            LOG_INFO("Registry HIT: RVA=0x%X accepted from PDB symbol [%s]",
                     rva, signature);
        }
        return rva;
    }

    // ---- Gate 3: prologue re-validation -----------------------------------
    // The registry RVA + TS/ImgSz + .text checks all passed, but we still need
    // to confirm the bytes at `hit` look like KeyboardClassServiceCallback.
    // This catches analyzer false-positives (uniqueness in usermode disasm
    // heuristic doesn't guarantee correctness).
    const char* matchedSig = NULL;
    if (!ValidateCallbackPrologue(hit, &matchedSig)) {
        LOG_ERROR("Registry RVA 0x%X prologue doesn't match any KCSC signature - REJECT", rva);
        LOG_ERROR("Driver will run in monitor-only mode (no inject, no BSOD)");
        return 0;
    }

    LOG_INFO("Registry HIT: RVA=0x%X validated by sig [%s]", rva, matchedSig);
    return rva;
}

static BOOLEAN AddrInText(PVOID a) {
    if (!a || !g_KbdTextStart || !g_KbdTextSize) return FALSE;
    return (a >= g_KbdTextStart) &&
           (a < (PVOID)((PUCHAR)g_KbdTextStart + g_KbdTextSize));
}

static BOOLEAN ReadableTextRange(PVOID a, ULONG len) {
    if (!a || len == 0) return FALSE;
    PVOID last = (PVOID)((PUCHAR)a + len - 1);
    if (!AddrInText(a) || !AddrInText(last)) return FALSE;

    __try {
        for (ULONG i = 0; i < len; i++) {
            if (!MmIsAddressValid((PVOID)((PUCHAR)a + i)))
                return FALSE;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("ReadableTextRange SEH 0x%X reading hit=%p",
                 GetExceptionCode(), a);
        return FALSE;
    }
    return TRUE;
}

// ============================================================================
// Pattern scanner — count matches, return first hit. Caller decides what
// to do when count > 1 (we reject ambiguous patterns).
// ============================================================================
static BOOLEAN PatternMatch(const UCHAR* data, const UCHAR* sig, const char* mask) {
    for (; *mask; mask++, data++, sig++)
        if (*mask == 'x' && *data != *sig) return FALSE;
    return TRUE;
}
static ULONG PatternScanCount(PVOID base, ULONG size,
                              const UCHAR* sig, const char* mask, PVOID* OutHit) {
    SIZE_T mlen = 0;
    for (const char* m = mask; *m; m++) mlen++;
    *OutHit = NULL;
    if (size < mlen) return 0;
    UCHAR* p = (UCHAR*)base;
    SIZE_T end = size - mlen;
    ULONG count = 0;
    for (SIZE_T i = 0; i <= end; i++) {
        if (PatternMatch(p + i, sig, mask)) {
            count++;
            if (!*OutHit) *OutHit = p + i;
            if (count > 1) break;
        }
    }
    return count;
}

// ============================================================================
// Multi-signature KeyboardClassServiceCallback discovery.
// MUST be kept in lockstep with analyze_kbdclass.py SIGS table (8 prologue
// variants seen across Win7 -> Win11 24H2).
//
// Used in two places:
//   1. ValidateCallbackPrologue() - re-verifies the RVA from registry actually
//      points at a known KCSC prologue, catching analyzer false-positives.
//      This is the primary BSOD-prevention guard.
//   2. (Legacy) In-kernel fallback scan - kept #if 0 for reference.
// ============================================================================
struct SigVariant {
    const UCHAR* sig;
    const char*  mask;
    const char*  desc;
    BOOLEAN      preferWin10;     // try first on Win10 if TRUE
};

// ---- Win11 24H2 (build 26100): 4-arg save + push r12..r15 ------------------
static const UCHAR sig_win11_24h2[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,  0x48, 0x89, 0x6C, 0x24, 0x00,
    0x48, 0x89, 0x74, 0x24, 0x00,  0x48, 0x89, 0x4C, 0x24, 0x00,
    0x57,  0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x83, 0xEC, 0x00,  0x4D, 0x8B, 0xE9
};
static const char mask_win11_24h2[] = "xxxx?xxxx?xxxx?xxxx?xxxxxxxxxxxx?xxx";

// ---- Win11 22H2/23H2: 3-arg + push rdi/r14/r15 + mov r14,r9 ----------------
static const UCHAR sig_win11_22h2[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,  0x48, 0x89, 0x6C, 0x24, 0x00,
    0x48, 0x89, 0x74, 0x24, 0x00,  0x57, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x83, 0xEC, 0x00,  0x4D, 0x8B, 0xF1
};
static const char mask_win11_22h2[] = "xxxx?xxxx?xxxx?xxxxxxxx?xxx";

// ---- Win10 22H2 / Server 2022: 3-arg + xor ebx,ebx + mov r14,r8 ------------
static const UCHAR sig_win10_22h2_xor[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,  0x48, 0x89, 0x6C, 0x24, 0x00,
    0x48, 0x89, 0x74, 0x24, 0x00,  0x57, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x83, 0xEC, 0x00,  0x33, 0xDB,  0x4D, 0x8B, 0xF0
};
static const char mask_win10_22h2_xor[] = "xxxx?xxxx?xxxx?xxxxxxxx?xxxxx";

// [REMOVED] win10_22h2_19045_4push - sig matched the WRONG function in kbdclass
// (caused DRIVER_IRQL_NOT_LESS_OR_EQUAL BSOD on a friend's machine on 2026-06-23).
// The byte pattern (3-arg save + push r12..r15 + xor edi,edi + mov rsi,rdx) hits
// a similar-but-not-KCSC function in Win10 22H2 / Win11 22H2 kbdclass.sys.
// Those builds must be resolved by PDB, not by this byte pattern.

// ---- Win10 1903-21H2: standard 3-arg prologue + xor ebp,ebp + mov rsi,r8 ---
static const UCHAR sig_win10_1903[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,  0x48, 0x89, 0x6C, 0x24, 0x00,
    0x48, 0x89, 0x74, 0x24, 0x00,  0x57,  0x48, 0x83, 0xEC, 0x00,
    0x33, 0xED,  0x49, 0x8B, 0xF0
};
static const char mask_win10_1903[] = "xxxx?xxxx?xxxx?xxxxx?xxxxx";

// ---- Win10 1809-: 2-arg + mov r10,r9 + xor ebx,r8 -------------------------
static const UCHAR sig_win10_1809[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,  0x48, 0x89, 0x74, 0x24, 0x00,
    0x57,  0x48, 0x83, 0xEC, 0x00,  0x4D, 0x8B, 0xD1,  0x41, 0x33, 0xD8
};
static const char mask_win10_1809[] = "xxxx?xxxx?xxxx?xxxxxx";

// ---- Win8/8.1: short 1-arg + push rdi + sub rsp + mov rdi,rdx -------------
static const UCHAR sig_win8[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,  0x57,  0x48, 0x83, 0xEC, 0x00,
    0x48, 0x8B, 0xFA,  0x48, 0x8B, 0xD9,  0x48, 0x81, 0xC1
};
static const char mask_win8[] = "xxxx?xxxx?xxxxxxxxx";

// ---- Win7 legacy: save rbx + rsi + push rdi + sub rsp + arg moves ---------
static const UCHAR sig_win7[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,  0x48, 0x89, 0x74, 0x24, 0x00,  0x57,
    0x48, 0x83, 0xEC, 0x00,  0x33, 0xED,  0x48, 0x8B, 0xFA,  0x49, 0x8B, 0xD9
};
static const char mask_win7[] = "xxxx?xxxx?xxxxx?xxxxxxxxx";

// ---- Lenient catch-all: most common opening, validated by .text bounds only
static const UCHAR sig_lenient[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,  0x48, 0x89, 0x6C, 0x24, 0x00,
    0x48, 0x89, 0x74, 0x24, 0x00,  0x57
};
static const char mask_lenient[] = "xxxx?xxxx?xxxx?x";

static const SigVariant kSigs[] = {
    { sig_win11_24h2,             mask_win11_24h2,             "win11_24h2_4arg",         FALSE },
    { sig_win11_22h2,             mask_win11_22h2,             "win11_22h2_3arg",         FALSE },
    { sig_win10_22h2_xor,         mask_win10_22h2_xor,         "win10_22h2_3arg_xor",     TRUE  },
    { sig_win10_1903,             mask_win10_1903,             "win10_1903_3arg",         TRUE  },
    { sig_win10_1809,             mask_win10_1809,             "win10_1809_2arg",         TRUE  },
    { sig_win8,                   mask_win8,                   "win8_2arg_short",         FALSE },
    { sig_win7,                   mask_win7,                   "win7_legacy",             FALSE },
    { sig_lenient,                mask_lenient,                "generic_3arg_lenient",    FALSE },
};

// ============================================================================
// ValidateCallbackPrologue - reads first 64 bytes at `hit` and checks them
// against every signature in kSigs. Returns TRUE on a match, FALSE otherwise.
//
// This is the BSOD-prevention guard for registry RVA. If the analyzer made a
// false-positive match (e.g. picked another function by disasm heuristic), the
// bytes at `hit` won't match any known KCSC prologue and we reject the RVA.
//
// Caller must hold a valid pointer into resident kbdclass memory (we check
// MmIsAddressValid and wrap in SEH).
// ============================================================================
static BOOLEAN ValidateCallbackPrologue(PVOID hit, const char** OutSigName)
{
    if (OutSigName) *OutSigName = NULL;
    if (!hit) return FALSE;
    if (!MmIsAddressValid(hit)) return FALSE;

    UCHAR buf[64];
    BOOLEAN copied = FALSE;
    __try {
        // Probe each byte to make sure the whole range is resident before we
        // touch it. kbdclass .text is non-paged, but be defensive.
        for (ULONG i = 0; i < sizeof(buf); i++) {
            PVOID b = (PVOID)((PUCHAR)hit + i);
            if (!MmIsAddressValid(b)) return FALSE;
        }
        RtlCopyMemory(buf, hit, sizeof(buf));
        copied = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("ValidateCallbackPrologue SEH 0x%X reading hit=%p",
                 GetExceptionCode(), hit);
        return FALSE;
    }
    if (!copied) return FALSE;

    // Try sigs in OS-preferred order: on Win10 try the Win10-preferred sigs
    // first, on Win11 try the others first. Final answer doesn't depend on
    // order (we accept first match), but ordering speeds up the common case.
    BOOLEAN preferWin10 = g_IsWin10;

    for (int pass = 0; pass < 2; pass++) {
        for (ULONG i = 0; i < ARRAYSIZE(kSigs); i++) {
            const SigVariant& s = kSigs[i];
            BOOLEAN inThisPass = (pass == 0)
                ? (s.preferWin10 == preferWin10)
                : (s.preferWin10 != preferWin10);
            if (!inThisPass) continue;

            // Mask length = signature length (must fit in buf).
            SIZE_T mlen = 0;
            for (const char* m = s.mask; *m; m++) mlen++;
            if (mlen > sizeof(buf)) continue;

            BOOLEAN ok = TRUE;
            for (SIZE_T k = 0; k < mlen; k++) {
                if (s.mask[k] == 'x' && buf[k] != s.sig[k]) { ok = FALSE; break; }
            }
            if (ok) {
                if (OutSigName) *OutSigName = s.desc;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static PVOID FindKbdCallback(void) {
    PVOID base = NULL; ULONG size = 0;
    if (!FindKbdClass(&base, &size)) return NULL;
    if (!ParseKbdTextBounds(base)) {
        LOG_WARN("Couldn't parse kbdclass .text — refusing inject");
        return NULL;
    }

    // ONLY PATH: registry RVA from analyze_kbdclass.exe (run by START.bat
    // before driver load). In-kernel pattern scan is DISABLED because on
    // certain Windows builds (e.g., 22H2) the strict signatures matched
    // wrong functions causing BSODs. Analyzer runs in usermode where wrong
    // matches just print "no unique sig" and the driver runs in monitor-only
    // mode — no BSOD possible.
    ULONG regRva = TryLoadRvaFromRegistry(base);
    if (regRva != 0) {
        return (PVOID)((PUCHAR)base + regRva);
    }

    LOG_WARN("No registry RVA from analyzer — inject DISABLED (monitor-only mode)");
    LOG_WARN("Run START.bat / analyze_kbdclass.exe as admin to populate registry");
    return NULL;

    // In-kernel pattern scan path — DISABLED, kept for reference only.
#if 0
    LOG_INFO("Falling back to internal pattern scan");
    for (ULONG i = 0; i < ARRAYSIZE(kSigs); i++) {
        PVOID hit = NULL;
        ULONG cnt = PatternScanCount(base, size, kSigs[i].sig, kSigs[i].mask, &hit);
        LOG_INFO("%s: %u match(es)", kSigs[i].desc, cnt);
        if (cnt == 1 && AddrInText(hit)) {
            LOG_INFO("=> Using %s @ %p", kSigs[i].desc, hit);
            return hit;
        }
        if (cnt > 1) {
            LOG_WARN("%s ambiguous (%u hits) — skip", kSigs[i].desc, cnt);
        } else if (cnt == 1 && !AddrInText(hit)) {
            LOG_WARN("%s hit %p outside .text — skip", kSigs[i].desc, hit);
        }
    }
    LOG_ERROR("No KeyboardClassServiceCallback signature matched — INJECT DISABLED");
    return NULL;
#endif  // in-kernel pattern scan path disabled (BSOD-prevention)
}

static BOOLEAN IsKernelPointer(PVOID p) {
    return p && ((ULONG_PTR)p >= 0xFFFF800000000000ULL);
}

static BOOLEAN SafeReadPointer(PVOID address, PVOID* out) {
    if (!out) return FALSE;
    *out = NULL;
    if (!IsKernelPointer(address) || !MmIsAddressValid(address)) return FALSE;
    __try {
        *out = *(PVOID*)address;
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

static int KeyboardClassNumberFromName(PUNICODE_STRING name) {
    if (!name || !name->Buffer || name->Length < sizeof(WCHAR)) return -1;
    int n = 0;
    BOOLEAN haveDigit = FALSE;
    for (USHORT i = 0; i < name->Length / sizeof(WCHAR); i++) {
        WCHAR c = name->Buffer[i];
        if (c >= L'0' && c <= L'9') {
            haveDigit = TRUE;
            n = (n * 10) + (c - L'0');
        } else if (haveDigit) {
            return n;
        }
    }
    return haveDigit ? n : -1;
}

static BOOLEAN IsCollectedClassDevice(PDEVICE_OBJECT dev,
                                      PDEVICE_OBJECT* classDevices,
                                      ULONG classCount)
{
    if (!dev) return FALSE;
    for (ULONG i = 0; i < classCount; i++) {
        if (dev == classDevices[i]) return TRUE;
    }
    return FALSE;
}

static BOOLEAN AddKbdTarget(PDEVICE_OBJECT dev, USHORT unitId, const char* why) {
    if (!dev || g_KbdTargetCount >= MAX_KBD_TARGETS) return FALSE;
    for (ULONG i = 0; i < g_KbdTargetCount; i++) {
        if (g_KbdTargets[i].Device == dev) return TRUE;
    }

    if (!MmIsAddressValid(dev) || !dev->DeviceExtension) return FALSE;
    ObReferenceObject(dev);
    g_KbdTargets[g_KbdTargetCount].Device = dev;
    g_KbdTargets[g_KbdTargetCount].UnitId = unitId;
    LOG_INFO("kbd target[%u]: dev=%p uid=%u source=%s",
             g_KbdTargetCount, dev, unitId, why ? why : "?");
    g_KbdTargetCount++;
    return TRUE;
}

static VOID ReleaseKbdTargets(void) {
    for (ULONG i = 0; i < g_KbdTargetCount; i++) {
        if (g_KbdTargets[i].Device) {
            ObDereferenceObject(g_KbdTargets[i].Device);
            g_KbdTargets[i].Device = NULL;
        }
    }
    g_KbdTargetCount = 0;
}

static VOID CleanupRuntimeResources(BOOLEAN SignalDone) {
    g_KbdSafe = FALSE;

    if (g_ProcessNotifyRegistered) {
        PsSetCreateProcessNotifyRoutine(ProcessExitCallback, TRUE);
        g_ProcessNotifyRegistered = FALSE;
    }

    ReleaseKbdTargets();

    // Capture the done event locally so we can clear our globals BEFORE
    // signaling it, and signal it as the very last step of teardown. Goal:
    // when kdunmap.exe wakes up on F20DriverStopped, every kernel resource is
    // already released, and the worker thread is one instruction away from
    // returning. We add a small KeDelayExecutionThread after the signal so
    // kdunmap.exe is guaranteed not to free our .text while the WorkerThread
    // epilogue is still executing -- otherwise this is a textbook BSOD race.
    PKEVENT doneToSignal = SignalDone ? g_DoneEvent : NULL;

    if (g_UnloadEvent)       { ObDereferenceObject(g_UnloadEvent); g_UnloadEvent = NULL; }
    if (g_UnloadEventHandle) { ZwClose(g_UnloadEventHandle);       g_UnloadEventHandle = NULL; }
    if (g_DoneEvent && !doneToSignal) {
        ObDereferenceObject(g_DoneEvent);
        g_DoneEvent = NULL;
    }
    if (g_DoneEventHandle)   { ZwClose(g_DoneEventHandle);         g_DoneEventHandle = NULL; }

    if (doneToSignal) {
        KeSetEvent(doneToSignal, IO_NO_INCREMENT, FALSE);
        // Give kdunmap a window to consume the signal while we still hold the
        // ref, then release it. After this point the WorkerThread caller
        // returns into a few KSTART_ROUTINE epilogue bytes and then
        // PspSystemThreadStartup, neither of which live in our image.
        LARGE_INTEGER d; d.QuadPart = -(50LL * 10000LL); // 50 ms
        KeDelayExecutionThread(KernelMode, FALSE, &d);
        ObDereferenceObject(doneToSignal);
        g_DoneEvent = NULL;
    }
}

static VOID ScanPortExtensionForConnectData(PDEVICE_OBJECT portDev,
                                            PDEVICE_OBJECT* classDevices,
                                            ULONG classCount)
{
    if (!portDev || !portDev->DeviceExtension || !g_KbdCallback) return;
    if (!MmIsAddressValid(portDev) || !MmIsAddressValid(portDev->DeviceExtension))
        return;

    PUCHAR ext = (PUCHAR)portDev->DeviceExtension;
    for (ULONG off = sizeof(PVOID); off < 0x2000; off += sizeof(PVOID)) {
        PVOID service = NULL;
        if (!SafeReadPointer(ext + off, &service)) continue;
        if (service != (PVOID)g_KbdCallback) continue;

        PVOID classDevRaw = NULL;
        if (!SafeReadPointer(ext + off - sizeof(PVOID), &classDevRaw)) continue;
        PDEVICE_OBJECT classDev = (PDEVICE_OBJECT)classDevRaw;
        if (!IsCollectedClassDevice(classDev, classDevices, classCount))
            continue;

        AddKbdTarget(classDev, (USHORT)g_KbdTargetCount, "CONNECT_DATA");
    }
}

static VOID ScanPortDriverForTargets(PCWSTR driverName,
                                     PDEVICE_OBJECT* classDevices,
                                     ULONG classCount)
{
    UNICODE_STRING drvName;
    RtlInitUnicodeString(&drvName, driverName);
    PDRIVER_OBJECT drv = NULL;
    NTSTATUS s = ObReferenceObjectByName(&drvName, OBJ_CASE_INSENSITIVE, NULL, 0,
                                          *IoDriverObjectType, KernelMode, NULL,
                                          (PVOID*)&drv);
    if (!NT_SUCCESS(s) || !drv) return;

    int count = 0;
    PDEVICE_OBJECT cur = drv->DeviceObject;
    while (cur && count < 32) {
        count++;
        ScanPortExtensionForConnectData(cur, classDevices, classCount);
        cur = cur->NextDevice;
    }

    ObDereferenceObject(drv);
}

// ============================================================================
// Build a list of kbdclass class-device targets. The only accepted source is
// the CONNECT_DATA pair saved inside kbdhid/i8042prt device extensions.
//
// Do not fall back to fan-out over every KeyboardClassN device: on some Win10
// builds it can call KeyboardClassServiceCallback with a class object/UnitId
// combination that is not the active port connection and crash at IRQL.
// ============================================================================
static BOOLEAN InitKbdTargets(void) {
    if (!g_KbdCallback) return FALSE;

    UNICODE_STRING drvName; RtlInitUnicodeString(&drvName, L"\\Driver\\Kbdclass");
    PDRIVER_OBJECT kbdDrv = NULL;
    NTSTATUS s = ObReferenceObjectByName(&drvName, OBJ_CASE_INSENSITIVE, NULL, 0,
                                          *IoDriverObjectType, KernelMode, NULL,
                                          (PVOID*)&kbdDrv);
    if (!NT_SUCCESS(s) || !kbdDrv) {
        LOG_ERROR("ObReferenceObjectByName(\\Driver\\Kbdclass) failed: 0x%X", s);
        return FALSE;
    }
    PDEVICE_OBJECT classDevices[MAX_KBD_TARGETS] = {};
    ULONG classCount = 0;
    PDEVICE_OBJECT cur = kbdDrv->DeviceObject;
    int count = 0;
    while (cur && count < 16) {
        count++;
        UCHAR nameBuf[512] = {};
        POBJECT_NAME_INFORMATION ni = (POBJECT_NAME_INFORMATION)nameBuf;
        ULONG retLen = 0;
        if (NT_SUCCESS(ObQueryNameString(cur, ni, sizeof(nameBuf), &retLen)) &&
            ni->Name.Buffer) {
            LOG_INFO("kbdclass dev[%d]: %p name=%wZ", count, cur, &ni->Name);
            if (classCount < MAX_KBD_TARGETS) {
                ObReferenceObject(cur);
                classDevices[classCount++] = cur;
            }
        }
        cur = cur->NextDevice;
    }

    if (classCount == 0) {
        ObDereferenceObject(kbdDrv);
        LOG_ERROR("No KeyboardClassN devices found");
        return FALSE;
    }
    ObDereferenceObject(kbdDrv);

    // Source-of-truth path: port driver extension contains CONNECT_DATA:
    // { ClassDeviceObject, ClassService = KeyboardClassServiceCallback }.
    ScanPortDriverForTargets(L"\\Driver\\kbdhid", classDevices, classCount);
    ScanPortDriverForTargets(L"\\Driver\\i8042prt", classDevices, classCount);

    for (ULONG i = 0; i < classCount; i++) {
        if (classDevices[i]) ObDereferenceObject(classDevices[i]);
    }

    if (g_KbdTargetCount == 0) {
        LOG_WARN("No CONNECT_DATA pair found - inject DISABLED (no unsafe fan-out)");
        return FALSE;
    }

    LOG_INFO("Total kbd targets: %u", g_KbdTargetCount);
    return TRUE;
}

// NumLock handling REMOVED:
// IOCTL_KEYBOARD_QUERY_INDICATORS returns the keyboard's LED indicator state,
// which on some setups doesn't stay in sync with the Win32k NumLock toggle
// state. That produced visible NumLock LED flicker (driver thought it was OFF,
// toggled ON via inject, hardware LED briefly flipped, then re-flipped).
// User must enable NumLock manually before launch. Numpad scan injects 0x47..0x52
// will fall back to nav-cluster keys (Home/End/Arrows/Insert/PgUp/PgDn) if user
// forgets - that's their problem to remember.
//
// (NUMLOCK_SCAN_CODE constant and ntddkbd.h include kept only for reference; can
// be removed entirely if you're sure NumLock toggle won't be revisited.)

// ============================================================================
// Injection — gated by g_KbdSafe. Safe-mode = no-op.
// ============================================================================
static void InjectScan(USHORT scanCode, BOOLEAN keyup) {
    if (!g_KbdSafe || !g_KbdCallback || g_KbdTargetCount == 0) return;

    for (ULONG i = 0; i < g_KbdTargetCount; i++) {
        PDEVICE_OBJECT dev = g_KbdTargets[i].Device;
        if (!dev || !MmIsAddressValid(dev) || !dev->DeviceExtension)
            continue;

        KEYBOARD_INPUT_DATA d = {};
        d.UnitId = g_KbdTargets[i].UnitId;
        d.MakeCode = scanCode;
        d.Flags = keyup ? KEY_BREAK : KEY_MAKE;
        ULONG consumed = 0;

        KIRQL old;
        KeRaiseIrql(DISPATCH_LEVEL, &old);
        __try {
            g_KbdCallback(dev, &d, &d + 1, &consumed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("Inject SEH exception 0x%X target=%u dev=%p - disabling inject",
                      GetExceptionCode(), i, dev);
            g_KbdSafe = FALSE;
        }
        KeLowerIrql(old);

        if (consumed != 1) {
            LOG_WARN("Inject scan=0x%X flags=0x%X target=%u unit=%u consumed=%u",
                     scanCode, d.Flags, i, d.UnitId, consumed);
        }

        if (!g_KbdSafe) break;
    }
}

// ============================================================================
// Random Numpad picker - alternates between the POSITIVE and NEGATIVE yaw
// pools on every kill (rule #3: "1 kill positive value, 1 kill negative value
// and so on"), then picks uniformly inside the chosen pool, never repeating
// the same Numpad key twice in a row inside that pool.
//
// Pool flip happens BEFORE the pick, so the sequence is strictly:
//   kill 1: NEG (random of 5)
//   kill 2: POS (random of 5)
//   kill 3: NEG (random of 4, excluding the one used on kill 1)
//   kill 4: POS (random of 4, excluding the one used on kill 2)
//   ...
//
// CNG entropy first, rdtsc/perf-counter/state fallback if CNG is unavailable.
// ============================================================================
static ULONG NextRandomU32(void) {
    ULONG out = 0;
    NTSTATUS cng = BCryptGenRandom(NULL, (PUCHAR)&out, sizeof(out),
                                   BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (NT_SUCCESS(cng)) return out;

    ULONG64 tsc = __rdtsc();
    LARGE_INTEGER pc = KeQueryPerformanceCounter(NULL);
    LARGE_INTEGER st;
    KeQuerySystemTimePrecise(&st);

    ULONG mixed = (ULONG)tsc ^ (ULONG)(tsc >> 32) ^ (ULONG)pc.QuadPart ^
                  (ULONG)(pc.QuadPart >> 32) ^ (ULONG)st.QuadPart ^
                  (ULONG)(st.QuadPart >> 32) ^ KeGetCurrentProcessorNumberEx(NULL);

    LONG oldState;
    LONG newState;
    do {
        oldState = g_RngSeed;
        ULONG x = (ULONG)oldState;
        if (x == 0) x = 0x9E3779B9u ^ mixed;
        x ^= mixed + 0x7F4A7C15u;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        newState = (LONG)x;
    } while (InterlockedCompareExchange(&g_RngSeed, newState, oldState) != oldState);

    return (ULONG)newState ^ mixed;
}

static USHORT PickRandomNumpadScan(void) {
    // Flip the sign first: this kill's pool is the opposite of the previous
    // kill's pool. After the flip, g_LastSignPositive holds the sign of the
    // pick we are about to make.
    g_LastSignPositive = !g_LastSignPositive;

    const USHORT* pool   = g_LastSignPositive ? g_NumpadScansPositive
                                              : g_NumpadScansNegative;
    ULONG*        lastIx = g_LastSignPositive ? &g_LastPositiveIdx
                                              : &g_LastNegativeIdx;

    ULONG r;
    if (*lastIx >= NUMPAD_POOL_SIZE) {
        // First pick from this pool: uniform over all 5 entries.
        r = NextRandomU32() % NUMPAD_POOL_SIZE;
    } else {
        // Subsequent picks: uniform over the 4 entries that are NOT the
        // previous pick from this same pool.
        r = NextRandomU32() % (NUMPAD_POOL_SIZE - 1);
        if (r >= *lastIx) r++;
    }
    *lastIx = r;
    return pool[r];
}

// ============================================================================
// Worker thread
// ============================================================================
_Function_class_(KSTART_ROUTINE)
VOID WorkerThread(_In_ PVOID Context) {
    UNREFERENCED_PARAMETER(Context);
    LOG_INFO("======= WorkerThread START =======");
    LOG_INFO("g_KbdSafe = %u (inject %s)",
             g_KbdSafe, g_KbdSafe ? "ENABLED" : "DISABLED (monitor-only)");

    UNICODE_STRING uModuleName; RtlInitUnicodeString(&uModuleName, L"client.dll");

    while (!WaitOrStop(0)) {
        PEPROCESS proc = NULL;
        __try {
            proc = FindProcessByName(L"cs2.exe");
            if (!proc || !IsProcessAlive(proc)) {
                if (proc) { ObDereferenceObject(proc); proc = NULL; }
                if (WaitOrStop(5000)) break;
                continue;
            }
            InterlockedExchange(&g_cs2Exiting, 0);
            SetTrackedCs2Pid(PsGetProcessId(proc));
            LOG_INFO("CS2 PID=%p, waiting 3s for init", ReadTrackedCs2Pid());
            if (WaitOrStop(3000)) {
                ClearTrackedCs2Pid();
                ObDereferenceObject(proc); break;
            }
            if (!IsProcessAlive(proc) || InterlockedOr(&g_cs2Exiting, 0)) {
                ClearTrackedCs2Pid();
                ObDereferenceObject(proc); continue;
            }
            PVOID clientBase = GetModuleBase(proc, uModuleName);
            if (!clientBase) {
                ClearTrackedCs2Pid();
                ObDereferenceObject(proc);
                if (WaitOrStop(5000)) break; continue;
            }
            USHORT mz = 0;
            if (!NT_SUCCESS(ReadProcMem(proc, clientBase, &mz, sizeof(mz))) || mz != 0x5A4D) {
                LOG_ERROR("client.dll MZ check failed");
                ClearTrackedCs2Pid();
                ObDereferenceObject(proc);
                if (WaitOrStop(5000)) break; continue;
            }
            LOG_INFO("client.dll=%p, entering inner loop", clientBase);

            INT32         lastRoundKills = -1;
            LARGE_INTEGER holdUntil      = {0};
            BOOLEAN       holdActive     = FALSE;
            ULONG         iter           = 0;

            while (IsProcessAlive(proc) && !WaitOrStop(0)) {
                if (InterlockedOr(&g_cs2Exiting, 0)) break;
                iter++;

                if ((iter % 1200) == 0) {
                    USHORT mzCheck = 0;
                    if (!NT_SUCCESS(ReadProcMem(proc, clientBase, &mzCheck, sizeof(mzCheck))) ||
                        mzCheck != 0x5A4D) {
                        LOG_WARN("client.dll MZ check failed mid-session"); break;
                    }
                }

                BOOLEAN reads_ok = FALSE;
                INT32 roundKills = 0;
                INT64 ctrl = 0, track = 0;
                if (NT_SUCCESS(ReadProcMem(proc, (PVOID)((ULONG_PTR)clientBase + g_OffCtrl),
                        &ctrl, sizeof(ctrl))) && ctrl) {
                    if (NT_SUCCESS(ReadProcMem(proc, (PVOID)((ULONG_PTR)ctrl + g_OffTrack),
                            &track, sizeof(track))) && track) {
                        if (NT_SUCCESS(ReadProcMem(proc, (PVOID)((ULONG_PTR)track + g_OffRoundKills),
                                &roundKills, sizeof(roundKills))))
                            reads_ok = TRUE;
                    }
                }

                if (reads_ok && roundKills >= 0 && roundKills <= 100) {
                    if (lastRoundKills < 0) {
                        lastRoundKills = roundKills;
                        LOG_INFO("Baseline RK=%d", roundKills);
                    } else if (roundKills > lastRoundKills) {
                        INT32 delta = roundKills - lastRoundKills;
                        if (delta > 5) {
                            LOG_WARN("Suspicious delta %d, resync", delta);
                            lastRoundKills = roundKills;
                        } else if (holdActive) {
                            // Gate by hold state: if F20 is still down from a
                            // previous kill (KILL_HOLD_MS window), drop the
                            // new kill entirely. This is the 2.5 sec cooldown.
                            LOG_INFO("Kill while F20 still held - skip restart");
                            lastRoundKills = roundKills;
                        } else {
                            LARGE_INTEGER startT; KeQuerySystemTime(&startT);
                            LONGLONG durTicks = (LONGLONG)KILL_HOLD_MS * 10000LL;
                            // 1) F20 down (held for KILL_HOLD_MS)
                            InjectScan(KILL_SCAN_CODE, FALSE);
                            holdActive = TRUE;
                            holdUntil.QuadPart = startT.QuadPart + durTicks;

                            // 2) One random Numpad 0..9 tap (no repeat of prev).
                            // NOTE: requires user-set NumLock=ON for scan codes
                            // 0x47..0x52 to register as Numpad rather than nav.
                            USHORT numScan = PickRandomNumpadScan();
                            InjectScan(numScan, FALSE);
                            BOOLEAN stopDuringNumTap = WaitOrStop(NUMPAD_TAP_MS);
                            InjectScan(numScan, TRUE);

                            ULONG poolIx = g_LastSignPositive ? g_LastPositiveIdx
                                                              : g_LastNegativeIdx;
                            LOG_INFO("KILL RK=%d->%d  F20 hold=%dms  Num scan=0x%X sign=%s idx=%u tap=%dms%s",
                                     lastRoundKills, roundKills,
                                     KILL_HOLD_MS, numScan,
                                     g_LastSignPositive ? "POS" : "NEG", poolIx,
                                     NUMPAD_TAP_MS, stopDuringNumTap ? " stop-pending" : "");
                            lastRoundKills = roundKills;
                        }
                    } else if (roundKills < lastRoundKills) {
                        LOG_INFO("Round/match reset RK=%d->%d", lastRoundKills, roundKills);
                        lastRoundKills = roundKills;
                    }
                }

                if (holdActive) {
                    LARGE_INTEGER now; KeQuerySystemTime(&now);
                    if (now.QuadPart >= holdUntil.QuadPart) {
                        LOG_INFO("F20 up");
                        InjectScan(KILL_SCAN_CODE, TRUE);
                        holdActive = FALSE;
                    }
                }

                if (WaitOrStop(POLL_INTERVAL_MS)) break;
            }

            if (holdActive) {
                InjectScan(KILL_SCAN_CODE, TRUE);
            }
            ClearTrackedCs2Pid();
            ObDereferenceObject(proc); proc = NULL;
            LOG_INFO("CS2 session ended");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("Exception in main loop: 0x%X", GetExceptionCode());
            ClearTrackedCs2Pid();
            if (proc) { ObDereferenceObject(proc); proc = NULL; }
        }
    }

    LOG_INFO("WorkerThread cleanup");
    CleanupRuntimeResources(TRUE);
    LOG_INFO("======= WorkerThread EXIT =======");
}

// ============================================================================
// DriverEntry
// ============================================================================
extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT D, _In_ PUNICODE_STRING R) {
    UNREFERENCED_PARAMETER(D); UNREFERENCED_PARAMETER(R);
    LOG_INFO("======================================");
    LOG_INFO("   F20Driver " F20DRIVER_VERSION_STRING);
    LOG_INFO("======================================");

    // Single-entry guard. Repeated kdmap would double the worker / event refs.
    if (InterlockedCompareExchange(&g_DriverEntered, 1, 0) != 0) {
        LOG_ERROR("DriverEntry called twice in the same kernel session - REJECT");
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    // ---- OS version: gates kbdclass sig ordering ----
    RTL_OSVERSIONINFOEXW osv = {};
    osv.dwOSVersionInfoSize = sizeof(osv);
    if (NT_SUCCESS(RtlGetVersion((PRTL_OSVERSIONINFOW)&osv))) {
        g_OsMajor = osv.dwMajorVersion;
        g_OsMinor = osv.dwMinorVersion;
        g_OsBuild = osv.dwBuildNumber;
        g_IsWin10 = (osv.dwMajorVersion == 10 && osv.dwBuildNumber < 22000);
        LOG_INFO("OS: %u.%u build %u (%s)",
                 g_OsMajor, g_OsMinor, g_OsBuild,
                 g_IsWin10 ? "Win10" : (osv.dwBuildNumber >= 22000 ? "Win11" : "unknown"));
    } else {
        LOG_WARN("RtlGetVersion failed - assuming Win11 sig ordering");
    }

    KeInitializeEvent(&g_StopEvent, NotificationEvent, FALSE);

    LARGE_INTEGER pc = KeQueryPerformanceCounter(NULL);
    ULONG seed = (ULONG)(pc.QuadPart ^ (pc.QuadPart >> 32) ^ __rdtsc());
    if (seed == 0) seed = 0xA5A5F20u;
    InterlockedExchange(&g_RngSeed, (LONG)seed);

    LoadCs2OffsetsFromRegistry();

    // ---- Unload event ----
    // Restricted DACL: SYSTEM + Administrators only. Previously the events
    // used a NULL DACL (everyone could signal them), so any unprivileged
    // process could spoof Global\F20DriverStop and force-stop the driver.
    {
        ADMIN_ONLY_SD sdCtx;
        NTSTATUS sdStatus = BuildAdminOnlySd(&sdCtx);
        if (!NT_SUCCESS(sdStatus)) {
            LOG_ERROR("BuildAdminOnlySd failed: 0x%X - aborting load", sdStatus);
        } else {
            UNICODE_STRING evName; RtlInitUnicodeString(&evName, L"\\BaseNamedObjects\\F20DriverStop");
            OBJECT_ATTRIBUTES oa;
            InitializeObjectAttributes(&oa, &evName,
                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_OPENIF, NULL, &sdCtx.sd);
            NTSTATUS es = ZwCreateEvent(&g_UnloadEventHandle, EVENT_ALL_ACCESS, &oa,
                                        NotificationEvent, FALSE);
            if (NT_SUCCESS(es)) {
                PVOID evObj = NULL;
                if (NT_SUCCESS(ObReferenceObjectByHandle(g_UnloadEventHandle, EVENT_ALL_ACCESS,
                                                         *ExEventObjectType, KernelMode, &evObj, NULL))) {
                    g_UnloadEvent = (PKEVENT)evObj;
                    KeClearEvent(g_UnloadEvent);
                    LOG_INFO("Unload event ready (DACL=SYSTEM+Administrators)");
                }
            }

            UNICODE_STRING doneName; RtlInitUnicodeString(&doneName, L"\\BaseNamedObjects\\F20DriverStopped");
            InitializeObjectAttributes(&oa, &doneName,
                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_OPENIF, NULL, &sdCtx.sd);
            NTSTATUS ds = ZwCreateEvent(&g_DoneEventHandle, EVENT_ALL_ACCESS, &oa,
                                        NotificationEvent, FALSE);
            if (NT_SUCCESS(ds)) {
                PVOID doneObj = NULL;
                if (NT_SUCCESS(ObReferenceObjectByHandle(g_DoneEventHandle, EVENT_ALL_ACCESS,
                                                         *ExEventObjectType, KernelMode, &doneObj, NULL))) {
                    g_DoneEvent = (PKEVENT)doneObj;
                    KeClearEvent(g_DoneEvent);
                    LOG_INFO("Done event ready (DACL=SYSTEM+Administrators)");
                }
            } else {
                LOG_WARN("ZwCreateEvent(F20DriverStopped) failed: 0x%X", ds);
            }
            FreeAdminOnlySd(&sdCtx);
        }
    }

    if (!g_UnloadEvent || !g_DoneEvent) {
        LOG_ERROR("Control plane incomplete (stop=%p done=%p) - aborting load",
                  g_UnloadEvent, g_DoneEvent);
        CleanupRuntimeResources(FALSE);
        return STATUS_UNSUCCESSFUL;
    }

    NTSTATUS s = PsSetCreateProcessNotifyRoutine(ProcessExitCallback, FALSE);
    if (NT_SUCCESS(s)) { g_ProcessNotifyRegistered = TRUE; LOG_INFO("ProcessExitCallback registered"); }
    else LOG_WARN("PsSetCreateProcessNotifyRoutine: 0x%X (HVCI?)", s);

    // ---- Keyboard inject setup ----
    // Resolve KCSC first, then find the class device object(s) that kbdhid /
    // i8042prt connected to it. If that source-of-truth path is unavailable,
    // stay in monitor-only mode instead of unsafe KeyboardClassN fan-out.
    BOOLEAN ok = FALSE;
    g_KbdCallback = (PKBD_SERVICE_CALLBACK)FindKbdCallback();
    if (g_KbdCallback) {
        ok = InitKbdTargets();
    }

    if (ok && g_KbdCallback && g_KbdTargetCount != 0) {
        g_KbdSafe = TRUE;
        LOG_INFO("Inject ENABLED: targets=%u cb=%p", g_KbdTargetCount, g_KbdCallback);
        LOG_WARN("Make sure NumLock=ON on your keyboard, else 0x47-0x52 inject as nav cluster");
    } else {
        LOG_WARN("Inject DISABLED — could not resolve callback safely (monitor-only mode)");
    }

    HANDLE h = NULL;
    s = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, WorkerThread, NULL);
    if (NT_SUCCESS(s)) {
        ZwClose(h);
        LOG_INFO("Driver loaded");
        return STATUS_SUCCESS;
    }
    LOG_ERROR("PsCreateSystemThread failed: 0x%X", s);
    CleanupRuntimeResources(FALSE);
    return s;
}
