// ============================================================================
// IsValveDS_Driver - kernel-mode CCSGameRules::m_bIsValveDS spoofer.
//
// kdmapper-style manual map (no DriverUnload, no IoCreateDevice, no IRP).
// Communicates with the user-mode console via:
//   - shared memory section (\BaseNamedObjects\IsValveDSState)
//   - notification event    (\BaseNamedObjects\IsValveDSStop)
//   - done event            (\BaseNamedObjects\IsValveDSStopped)
// User-mode opens the same objects as Global\IsValveDS*.
//
// Re-resolves cs2.exe / client.dll / CCSGameRules pointer every iteration,
// because all of them can change at runtime:
//   - cs2.exe can restart (PID changes)
//   - client.dll base survives across maps but is re-validated by MZ check
//   - CCSGameRules instance pointer is NULL in the main menu, re-populated
//     in matches, and may move on map / server change.
//
// Before honoring a write request we re-read the current byte at the target
// address. If it already matches the desired value, the write is skipped.
// Every write is followed by a readback that the user can see.
//
// Built on the same patterns as F20Driver to avoid common BSOD vectors:
//   - All cross-process reads via MmCopyVirtualMemory (no APC attach).
//   - PsGetProcessExitStatus + ProcessExitCallback gating around every read.
//   - PASSIVE_LEVEL only.
//   - SEH around all PEB / kernel structure walks.
//   - WaitOrStop() for responsive interruption from a named event.
// ============================================================================

#pragma warning(disable: 4505 4100 28251 28252 28253)

#pragma warning(push, 0)
#include <ntifs.h>
#include <ntddk.h>
#include <ntdef.h>
#include <intrin.h>
#pragma warning(pop)

#include "shared.h"

// ---- CS2 offsets -----------------------------------------------------------
// Defaults are only a fallback. run.bat refreshes these from a2x/cs2-dumper
// into HKLM\SOFTWARE\IsValveDS before mapping the driver.
#define DEFAULT_OFF_DWGAMERULES   0x2341158ULL    // client.dll!dwGameRules
#define DEFAULT_OFF_ISVALVEDS     0xA4ULL         // C_CSGameRules::m_bIsValveDS

#define ISVALVEDS_REG_PATH        L"\\Registry\\Machine\\SOFTWARE\\IsValveDS"
#define ISVALVEDS_REG_DWGAMERULES L"Cs2DwGameRules"
#define ISVALVEDS_REG_ISVALVEDS   L"Cs2M_bIsValveDS"

#ifndef REG_QWORD
#define REG_QWORD 11
#endif

// ---- Poll cadence ---------------------------------------------------------
#define POLL_INTERVAL_MS  200             // re-read cs2 memory this often

#define POOL_TAG          'sDvI'

// ---- Logging --------------------------------------------------------------
#define LOG_INFO(fmt, ...)  DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "[IsVDS] "       fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "[IsVDS] WARN: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "[IsVDS] ERR:  " fmt "\n", ##__VA_ARGS__)

// ---- External kernel APIs --------------------------------------------------
extern "C" {
    NTKERNELAPI PPEB NTAPI PsGetProcessPeb(_In_ PEPROCESS Process);
    NTSTATUS    NTAPI ZwQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
    NTSTATUS    NTAPI MmCopyVirtualMemory(PEPROCESS, PVOID, PEPROCESS, PVOID,
                                          SIZE_T, KPROCESSOR_MODE, PSIZE_T);
}

// ---- PEB / loader structures (minimal subset we touch) ---------------------
typedef struct _PEB_LDR_DATA_S {
    ULONG       Length;
    UCHAR       Initialized;
    PVOID       SsHandle;
    LIST_ENTRY  InLoadOrderModuleList;
} PEB_LDR_DATA_S, *PPEB_LDR_DATA_S;

typedef struct _LDR_DATA_TABLE_ENTRY_S {
    LIST_ENTRY      InLoadOrderLinks;
    LIST_ENTRY      InMemoryOrderLinks;
    LIST_ENTRY      InInitializationOrderLinks;
    PVOID           DllBase;
    PVOID           EntryPoint;
    ULONG           SizeOfImage;
    UNICODE_STRING  FullDllName;
    UNICODE_STRING  BaseDllName;
} LDR_DATA_TABLE_ENTRY_S, *PLDR_DATA_TABLE_ENTRY_S;

typedef struct _PEB_S {
    UCHAR           InheritedAddressSpace;
    UCHAR           ReadImageFileExecOptions;
    UCHAR           BeingDebugged;
    UCHAR           BitField;
    PVOID           Mutant;
    PVOID           ImageBaseAddress;
    PPEB_LDR_DATA_S Ldr;
} PEB_S, *PPEB_S;

typedef struct _SYSTEM_PROCESS_INFORMATION_S {
    ULONG          NextEntryOffset;
    ULONG          NumberOfThreads;
    UCHAR          Reserved1[48];
    UNICODE_STRING ImageName;
    KPRIORITY      BasePriority;
    HANDLE         UniqueProcessId;
    PVOID          Reserved2;
    ULONG          HandleCount;
    ULONG          SessionId;
    PVOID          Reserved3;
    SIZE_T         PeakVirtualSize;
    SIZE_T         VirtualSize;
    ULONG          Reserved4;
    SIZE_T         PeakWorkingSetSize;
    SIZE_T         WorkingSetSize;
    PVOID          Reserved5;
    SIZE_T         QuotaPagedPoolUsage;
    PVOID          Reserved6;
    SIZE_T         QuotaNonPagedPoolUsage;
    SIZE_T         PagefileUsage;
    SIZE_T         PeakPagefileUsage;
    SIZE_T         PrivatePageCount;
    LARGE_INTEGER  Reserved7[6];
} SYSTEM_PROCESS_INFORMATION_S, *PSYSTEM_PROCESS_INFORMATION_S;

// ---- Globals ---------------------------------------------------------------
static LONG            g_DriverEntered   = 0;
static KEVENT          g_StopEvent;          // internal stop
static HANDLE          g_UnloadEventHandle = NULL;
static PKEVENT         g_UnloadEvent       = NULL;   // user-mode signalled
static HANDLE          g_DoneEventHandle   = NULL;
static PKEVENT         g_DoneEvent         = NULL;   // worker-exit signal

static HANDLE          g_StateSection = NULL;
static PVOID           g_StateView    = NULL;
static ISVALVEDS_STATE* g_State       = NULL;

static volatile HANDLE g_cs2Pid     = NULL;
static volatile LONG   g_cs2Exiting = 0;
static BOOLEAN         g_ProcessNotifyRegistered = FALSE;

static ULONGLONG       g_OffDwGameRules = DEFAULT_OFF_DWGAMERULES;
static ULONGLONG       g_OffIsValveDS   = DEFAULT_OFF_ISVALVEDS;
static BOOLEAN         g_OffsetsFromReg = FALSE;

// ============================================================================
// Registry-backed CS2 offsets. This mirrors F20Kit's "download before load,
// consume in kernel" model, with built-in offsets as a conservative fallback.
// ============================================================================
static BOOLEAN IsSaneGameRulesOffset(ULONGLONG Value) {
    return Value >= 0x100000ULL && Value <= 0x40000000ULL && ((Value & 0x7ULL) == 0);
}

static BOOLEAN IsSaneFieldOffset(ULONGLONG Value) {
    return Value > 0 && Value <= 0x10000ULL;
}

static NTSTATUS RegReadFixedValue(HANDLE Key, PCWSTR Name, ULONG Type,
                                  PVOID Out, ULONG OutSize)
{
    if (!Key || !Name || !Out || !OutSize) return STATUS_INVALID_PARAMETER;

    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, Name);

    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONGLONG)] = {};
    ULONG bytes = 0;
    NTSTATUS st = ZwQueryValueKey(Key, &valueName, KeyValuePartialInformation,
                                  buf, sizeof(buf), &bytes);
    if (!NT_SUCCESS(st)) return st;

    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    if (info->Type != Type || info->DataLength != OutSize)
        return STATUS_INVALID_PARAMETER;

    RtlCopyMemory(Out, info->Data, OutSize);
    return STATUS_SUCCESS;
}

static void LoadOffsetsFromRegistry(void) {
    g_OffDwGameRules = DEFAULT_OFF_DWGAMERULES;
    g_OffIsValveDS   = DEFAULT_OFF_ISVALVEDS;
    g_OffsetsFromReg = FALSE;

    UNICODE_STRING path;
    RtlInitUnicodeString(&path, ISVALVEDS_REG_PATH);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);

    HANDLE key = NULL;
    NTSTATUS st = ZwOpenKey(&key, KEY_QUERY_VALUE, &oa);
    if (!NT_SUCCESS(st)) {
        LOG_WARN("CS2 offsets registry missing/unreadable (0x%X), using fallback", st);
        return;
    }

    ULONGLONG dwGameRules = 0;
    ULONG     mIsValveDS  = 0;
    NTSTATUS s1 = RegReadFixedValue(key, ISVALVEDS_REG_DWGAMERULES,
                                    REG_QWORD, &dwGameRules, sizeof(dwGameRules));
    NTSTATUS s2 = RegReadFixedValue(key, ISVALVEDS_REG_ISVALVEDS,
                                    REG_DWORD, &mIsValveDS, sizeof(mIsValveDS));
    ZwClose(key);

    if (!NT_SUCCESS(s1) || !NT_SUCCESS(s2)) {
        LOG_WARN("CS2 registry offsets incomplete (dw=0x%X field=0x%X), using fallback",
                 s1, s2);
        return;
    }
    if (!IsSaneGameRulesOffset(dwGameRules) || !IsSaneFieldOffset(mIsValveDS)) {
        LOG_WARN("CS2 registry offsets failed sanity (dw=0x%llX field=0x%X), using fallback",
                 dwGameRules, mIsValveDS);
        return;
    }

    g_OffDwGameRules = dwGameRules;
    g_OffIsValveDS   = (ULONGLONG)mIsValveDS;
    g_OffsetsFromReg = TRUE;
}

// ============================================================================
// Process exit notification - flips g_cs2Exiting so in-flight reads abort.
// ============================================================================
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
    if (!Create && trackedPid && Pid == trackedPid) {
        InterlockedExchange(&g_cs2Exiting, 1);
    }
}

// ============================================================================
// Wait helpers (F20Driver pattern).
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
    if ((ULONG_PTR)a > 0x7FFFFFFFFFFFULL) return FALSE;
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

static NTSTATUS WriteProcMem(PEPROCESS Process, PVOID Address, PVOID Buf, SIZE_T Size) {
    if (!Process || !Address || !Buf || !Size) return STATUS_INVALID_PARAMETER;
    if (!IsAddrValid(Address)) return STATUS_INVALID_ADDRESS;
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) return STATUS_UNSUCCESSFUL;
    if (InterlockedOr(&g_cs2Exiting, 0)) return STATUS_PROCESS_IS_TERMINATING;
    if (PsGetProcessExitStatus(Process) != STATUS_PENDING)
        return STATUS_PROCESS_IS_TERMINATING;
    SIZE_T copied = 0;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    __try {
        st = MmCopyVirtualMemory(PsGetCurrentProcess(), Buf, Process, Address,
                                 Size, KernelMode, &copied);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
        LOG_WARN("WriteProcMem SEH 0x%X addr=%p size=%llu", st, Address,
                 (unsigned long long)Size);
    }
    return st;
}

static BOOLEAN IsProcessAlive(PEPROCESS p) {
    if (!p) return FALSE;
    __try {
        if (PsGetProcessExitStatus(p) != STATUS_PENDING) return FALSE;
        HANDLE pid = PsGetProcessId(p);
        if (!pid || pid == (HANDLE)-1) return FALSE;
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

// ============================================================================
// Find PEPROCESS by image name (e.g. L"cs2.exe").
// ============================================================================
static PEPROCESS FindProcessByName(LPCWSTR Name) {
    ULONG bytes = 0;
    ZwQuerySystemInformation(5, NULL, 0, &bytes);
    if (!bytes) return NULL;
    PVOID buf = NULL;
    NTSTATUS s = STATUS_UNSUCCESSFUL;
    for (int a = 0; a < 5; a++) {
        ULONG sz = bytes + 8192;
        if (sz > 64u * 1024u * 1024u) return NULL;
        buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, POOL_TAG);
        if (!buf) return NULL;
        s = ZwQuerySystemInformation(5, buf, sz, &bytes);
        if (NT_SUCCESS(s)) break;
        ExFreePoolWithTag(buf, POOL_TAG);
        buf = NULL;
        if (s != STATUS_INFO_LENGTH_MISMATCH) return NULL;
    }
    if (!buf) return NULL;

    PSYSTEM_PROCESS_INFORMATION_S p = (PSYSTEM_PROCESS_INFORMATION_S)buf;
    PEPROCESS proc = NULL;
    UNICODE_STRING uName;
    RtlInitUnicodeString(&uName, Name);
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
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        proc = NULL;
    }
    ExFreePoolWithTag(buf, POOL_TAG);
    return proc;
}

// ============================================================================
// Find module base in *target* process by walking its PEB loader list.
// All reads go through MmCopyVirtualMemory - no KeStackAttachProcess.
// ============================================================================
static PVOID GetModuleBaseInTarget(PEPROCESS Process, UNICODE_STRING Name) {
    if (!Process) return NULL;
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) return NULL;
    if (InterlockedOr(&g_cs2Exiting, 0)) return NULL;
    if (PsGetProcessExitStatus(Process) != STATUS_PENDING) return NULL;

    PPEB_S pebAddr = (PPEB_S)PsGetProcessPeb(Process);
    if (!pebAddr) return NULL;

    PVOID base = NULL;
    __try {
        PEB_S peb = {};
        if (!NT_SUCCESS(ReadProcMem(Process, pebAddr, &peb, sizeof(peb))))
            return NULL;
        if (!peb.Ldr) return NULL;

        PEB_LDR_DATA_S ldr = {};
        if (!NT_SUCCESS(ReadProcMem(Process, peb.Ldr, &ldr, sizeof(ldr))))
            return NULL;

        PLIST_ENTRY head = &peb.Ldr->InLoadOrderModuleList;
        PLIST_ENTRY curN = ldr.InLoadOrderModuleList.Flink;
        for (ULONG i = 0; i < 512 && curN && curN != head; i++) {
            LDR_DATA_TABLE_ENTRY_S e = {};
            PVOID entryAddr = CONTAINING_RECORD(curN, LDR_DATA_TABLE_ENTRY_S, InLoadOrderLinks);
            if (!NT_SUCCESS(ReadProcMem(Process, entryAddr, &e, sizeof(e))))
                break;
            if (e.BaseDllName.Length && e.BaseDllName.Buffer) {
                WCHAR nbuf[64] = {};
                SIZE_T nb = min((SIZE_T)e.BaseDllName.Length, sizeof(nbuf) - sizeof(WCHAR));
                if (NT_SUCCESS(ReadProcMem(Process, e.BaseDllName.Buffer, nbuf, nb))) {
                    UNICODE_STRING en;
                    en.Buffer        = nbuf;
                    en.Length        = (USHORT)nb;
                    en.MaximumLength = sizeof(nbuf);
                    if (RtlCompareUnicodeString(&en, &Name, TRUE) == 0) {
                        base = e.DllBase;
                        break;
                    }
                }
            }
            curN = e.InLoadOrderLinks.Flink;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("GetModuleBaseInTarget SEH 0x%X", GetExceptionCode());
        base = NULL;
    }
    return base;
}

// ============================================================================
// SHM publishing helpers (driver-side writers).
// ============================================================================
static void PublishCurrent(int value, unsigned int err,
                           ULONGLONG addr, ULONGLONG clientBase, ULONG cs2pid)
{
    if (!g_State) return;
    __try {
        g_State->magic = 0;
        KeMemoryBarrier();
        g_State->current_value     = value;
        g_State->current_error     = err;
        g_State->current_address   = addr;
        g_State->client_base       = clientBase;
        g_State->cs2_pid           = cs2pid;
        LARGE_INTEGER t; KeQuerySystemTimePrecise(&t);
        g_State->last_poll_systime = (ULONGLONG)t.QuadPart;
        g_State->driver_tick++;
        KeMemoryBarrier();
        g_State->magic = ISVALVEDS_MAGIC;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("PublishCurrent exception 0x%X", GetExceptionCode());
    }
}

static void PublishWriteResult(unsigned int reqId, unsigned int err, int readback)
{
    if (!g_State) return;
    __try {
        g_State->write_error          = err;
        g_State->write_result_value   = readback;
        LARGE_INTEGER t; KeQuerySystemTimePrecise(&t);
        g_State->write_handled_systime = (ULONGLONG)t.QuadPart;
        KeMemoryBarrier();
        g_State->write_handled_id = reqId;   // last - this is the gate
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("PublishWriteResult exception 0x%X", GetExceptionCode());
    }
}

// ============================================================================
// Resolve target absolute address:
// client.dll + dwGameRules -> *ptr + C_CSGameRules::m_bIsValveDS.
// Returns VDS_ERR_* and writes target/clientBase out-params on success.
// ============================================================================
static unsigned int ResolveTarget(PEPROCESS proc, PVOID clientBase,
                                  PVOID* OutTarget)
{
    *OutTarget = NULL;
    if (!proc || !clientBase) return VDS_ERR_NO_CLIENT_DLL;

    PVOID ptrToGameRules = (PVOID)((ULONG_PTR)clientBase + g_OffDwGameRules);
    PVOID gameRules      = NULL;
    NTSTATUS st = ReadProcMem(proc, ptrToGameRules, &gameRules, sizeof(gameRules));
    if (!NT_SUCCESS(st)) return VDS_ERR_READ_FAILED;
    if (!gameRules || !IsAddrValid(gameRules)) return VDS_ERR_GAMERULES_NULL;

    *OutTarget = (PVOID)((ULONG_PTR)gameRules + g_OffIsValveDS);
    return VDS_ERR_OK;
}

static VOID CleanupRuntimeResources(BOOLEAN SignalDone) {
    PKEVENT doneToSignal = SignalDone ? g_DoneEvent : NULL;
    BOOLEAN cleanupOk = TRUE;

    __try {
        if (g_ProcessNotifyRegistered) {
            PsSetCreateProcessNotifyRoutine(ProcessExitCallback, TRUE);
            g_ProcessNotifyRegistered = FALSE;
        }

        ClearTrackedCs2Pid();

        if (g_StateView) {
            MmUnmapViewInSystemSpace(g_StateView);
            g_StateView = NULL;
            g_State = NULL;
        }
        if (g_StateSection) {
            ZwClose(g_StateSection);
            g_StateSection = NULL;
        }

        if (g_UnloadEvent)       { ObDereferenceObject(g_UnloadEvent); g_UnloadEvent = NULL; }
        if (g_UnloadEventHandle) { ZwClose(g_UnloadEventHandle);       g_UnloadEventHandle = NULL; }

        if (g_DoneEvent && !doneToSignal) {
            ObDereferenceObject(g_DoneEvent);
            g_DoneEvent = NULL;
        }
        if (g_DoneEventHandle) {
            ZwClose(g_DoneEventHandle);
            g_DoneEventHandle = NULL;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cleanupOk = FALSE;
        LOG_ERROR("CleanupRuntimeResources SEH 0x%X - not signaling done",
                  GetExceptionCode());
    }

    if (doneToSignal && cleanupOk) {
        KeSetEvent(doneToSignal, IO_NO_INCREMENT, FALSE);
        LARGE_INTEGER d; d.QuadPart = -(50LL * 10000LL); // 50 ms grace
        KeDelayExecutionThread(KernelMode, FALSE, &d);
        ObDereferenceObject(doneToSignal);
        g_DoneEvent = NULL;
    }
}

// ============================================================================
// Worker thread - the single owner of all cs2 memory access.
// ============================================================================
_Function_class_(KSTART_ROUTINE)
VOID WorkerThread(_In_ PVOID Context) {
    UNREFERENCED_PARAMETER(Context);
    LOG_INFO("======= WorkerThread START =======");

    UNICODE_STRING uClient; RtlInitUnicodeString(&uClient, L"client.dll");

    unsigned int lastHandledId = 0;

    while (!WaitOrStop(0)) {
        PEPROCESS proc = NULL;
        __try {
            proc = FindProcessByName(L"cs2.exe");
            if (!proc || !IsProcessAlive(proc)) {
                if (proc) { ObDereferenceObject(proc); proc = NULL; }
                PublishCurrent(-1, VDS_ERR_NO_CS2, 0, 0, 0);
                if (WaitOrStop(1500)) break;
                continue;
            }
            InterlockedExchange(&g_cs2Exiting, 0);
            SetTrackedCs2Pid(PsGetProcessId(proc));
            HANDLE trackedPid = ReadTrackedCs2Pid();
            LOG_INFO("cs2.exe PID=%p", trackedPid);

            // Give cs2 a moment to load DLLs.
            if (WaitOrStop(1000)) {
                ClearTrackedCs2Pid();
                ObDereferenceObject(proc);
                break;
            }
            if (!IsProcessAlive(proc) || InterlockedOr(&g_cs2Exiting, 0)) {
                ClearTrackedCs2Pid();
                ObDereferenceObject(proc);
                continue;
            }

            // Resolve client.dll once per cs2 session; re-validate via MZ each
            // iteration; if MZ goes bad we abandon this session.
            PVOID clientBase = GetModuleBaseInTarget(proc, uClient);
            if (!clientBase) {
                PublishCurrent(-1, VDS_ERR_NO_CLIENT_DLL, 0, 0,
                               (ULONG)(ULONG_PTR)ReadTrackedCs2Pid());
                ClearTrackedCs2Pid();
                ObDereferenceObject(proc);
                if (WaitOrStop(2000)) break;
                continue;
            }
            USHORT mz = 0;
            if (!NT_SUCCESS(ReadProcMem(proc, clientBase, &mz, sizeof(mz))) ||
                mz != 0x5A4D) {
                LOG_WARN("client.dll MZ check failed");
                PublishCurrent(-1, VDS_ERR_NO_CLIENT_DLL, 0,
                               (ULONGLONG)clientBase,
                               (ULONG)(ULONG_PTR)ReadTrackedCs2Pid());
                ClearTrackedCs2Pid();
                ObDereferenceObject(proc);
                if (WaitOrStop(2000)) break;
                continue;
            }
            LOG_INFO("client.dll=%p", clientBase);

            // ---- inner loop: poll + handle writes ----
            while (IsProcessAlive(proc) && !WaitOrStop(0)) {
                if (InterlockedOr(&g_cs2Exiting, 0)) break;

                // Re-resolve every iteration. GameRules can vanish (main menu)
                // and reappear (match start), and may move when the map / server
                // changes. dwGameRules slot is fixed; the pointer in it is not.
                PVOID target = NULL;
                unsigned int err = ResolveTarget(proc, clientBase, &target);
                int currentValue = -1;
                ULONGLONG addr   = (ULONGLONG)target;

                if (err == VDS_ERR_OK) {
                    UCHAR b = 0;
                    NTSTATUS rs = ReadProcMem(proc, target, &b, sizeof(b));
                    if (NT_SUCCESS(rs)) {
                        currentValue = (b ? 1 : 0);
                    } else {
                        err = VDS_ERR_READ_FAILED;
                    }
                }

                PublishCurrent(currentValue, err, addr,
                               (ULONGLONG)clientBase,
                               (ULONG)(ULONG_PTR)ReadTrackedCs2Pid());

                // ---- write request handling --------------------------------
                unsigned int reqId = 0;
                unsigned int desired = 0;
                if (g_State) {
                    __try {
                        reqId   = g_State->write_request_id;
                        desired = g_State->desired_value & 1u;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        reqId = 0;
                    }
                }

                if (reqId != 0 && reqId != lastHandledId) {
                    LOG_INFO("Write request id=%u desired=%u", reqId, desired);

                    // Re-resolve fresh, even though we just did - GameRules
                    // pointer could have changed mid-iteration on map flip.
                    PVOID target2 = NULL;
                    unsigned int werr = ResolveTarget(proc, clientBase, &target2);
                    int readback = -1;

                    if (werr == VDS_ERR_OK) {
                        // Verify before write: re-read current.
                        UCHAR b = 0;
                        NTSTATUS rs = ReadProcMem(proc, target2, &b, sizeof(b));
                        if (!NT_SUCCESS(rs)) {
                            werr = VDS_ERR_READ_FAILED;
                        } else if (b == (UCHAR)desired) {
                            // Already matches - no write needed.
                            readback = desired;
                            werr = VDS_ERR_OK;
                            LOG_INFO("Write skipped: already=%u", desired);
                        } else {
                            UCHAR v = (UCHAR)desired;
                            NTSTATUS ws = WriteProcMem(proc, target2, &v, sizeof(v));
                            if (!NT_SUCCESS(ws)) {
                                werr = VDS_ERR_WRITE_FAILED;
                            } else {
                                // Verify after write.
                                UCHAR after = 0xFF;
                                NTSTATUS rs2 = ReadProcMem(proc, target2, &after, sizeof(after));
                                if (!NT_SUCCESS(rs2)) {
                                    werr = VDS_ERR_READ_FAILED;
                                } else {
                                    readback = (after ? 1 : 0);
                                    werr = (after == (UCHAR)desired) ? VDS_ERR_OK
                                                                      : VDS_ERR_WRITE_FAILED;
                                }
                            }
                        }
                    }

                    PublishWriteResult(reqId, werr, readback);
                    lastHandledId = reqId;
                }

                if (WaitOrStop(POLL_INTERVAL_MS)) break;
            }

            ClearTrackedCs2Pid();
            ObDereferenceObject(proc);
            proc = NULL;
            LOG_INFO("cs2 session ended");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("Worker SEH 0x%X", GetExceptionCode());
            if (proc) { ObDereferenceObject(proc); proc = NULL; }
        }
    }

    // ---- cleanup ----------------------------------------------------------
    LOG_INFO("WorkerThread cleanup");
    CleanupRuntimeResources(TRUE);
    LOG_INFO("======= WorkerThread EXIT =======");
}

// ============================================================================
// DriverEntry - kdmapper-style. NO DriverUnload, NO IoCreateDevice.
// ============================================================================
extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT D, _In_ PUNICODE_STRING R) {
    UNREFERENCED_PARAMETER(D);
    UNREFERENCED_PARAMETER(R);

    if (InterlockedCompareExchange(&g_DriverEntered, 1, 0) != 0) {
        LOG_ERROR("DriverEntry called twice in the same kernel session - REJECT");
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    LoadOffsetsFromRegistry();

    LOG_INFO("======================================");
    LOG_INFO(" IsValveDS_Driver  (SHM-based, no IRP)");
    LOG_INFO(" offsets source = %s", g_OffsetsFromReg ? "registry" : "built-in fallback");
    LOG_INFO(" dwGameRules    = 0x%llX", (ULONGLONG)g_OffDwGameRules);
    LOG_INFO(" m_bIsValveDS   = 0x%llX", (ULONGLONG)g_OffIsValveDS);
    LOG_INFO("======================================");

    KeInitializeEvent(&g_StopEvent, NotificationEvent, FALSE);

    // ---- create shared state section ----
    {
        UNICODE_STRING shmName;
        RtlInitUnicodeString(&shmName, ISVALVEDS_SHM_KERNEL_NAME);
        SECURITY_DESCRIPTOR sd;
        if (NT_SUCCESS(RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) &&
            NT_SUCCESS(RtlSetDaclSecurityDescriptor(&sd, TRUE, NULL, FALSE))) {
            OBJECT_ATTRIBUTES oa;
            InitializeObjectAttributes(&oa, &shmName,
                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_OPENIF, NULL, &sd);
            LARGE_INTEGER shmSize; shmSize.QuadPart = PAGE_SIZE;
            NTSTATUS shmSt = ZwCreateSection(&g_StateSection, SECTION_ALL_ACCESS,
                                              &oa, &shmSize, PAGE_READWRITE,
                                              SEC_COMMIT, NULL);
            if (NT_SUCCESS(shmSt)) {
                PVOID secObj = NULL;
                if (NT_SUCCESS(ObReferenceObjectByHandle(g_StateSection, SECTION_ALL_ACCESS,
                                                          NULL, KernelMode, &secObj, NULL))) {
                    SIZE_T vs = PAGE_SIZE;
                    if (NT_SUCCESS(MmMapViewInSystemSpace(secObj, &g_StateView, &vs))) {
                        g_State = (ISVALVEDS_STATE*)g_StateView;
                        __try {
                            RtlZeroMemory(g_State, sizeof(ISVALVEDS_STATE));
                            g_State->current_value = -1;
                            g_State->current_error = VDS_ERR_NO_CS2;
                            LOG_INFO("SHM ready");
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            g_State = NULL;
                            LOG_ERROR("SHM init SEH 0x%X", GetExceptionCode());
                        }
                    } else {
                        LOG_ERROR("MmMapViewInSystemSpace failed");
                    }
                    ObDereferenceObject(secObj);
                }
            } else {
                LOG_ERROR("ZwCreateSection failed 0x%X", shmSt);
            }
        }
    }

    // ---- create unload event ----
    {
        UNICODE_STRING evName;
        RtlInitUnicodeString(&evName, ISVALVEDS_STOP_KERNEL_NAME);
        SECURITY_DESCRIPTOR sd;
        if (NT_SUCCESS(RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) &&
            NT_SUCCESS(RtlSetDaclSecurityDescriptor(&sd, TRUE, NULL, FALSE))) {
            OBJECT_ATTRIBUTES oa;
            InitializeObjectAttributes(&oa, &evName,
                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_OPENIF, NULL, &sd);
            NTSTATUS es = ZwCreateEvent(&g_UnloadEventHandle, EVENT_ALL_ACCESS, &oa,
                                         NotificationEvent, FALSE);
            if (NT_SUCCESS(es)) {
                PVOID evObj = NULL;
                if (NT_SUCCESS(ObReferenceObjectByHandle(g_UnloadEventHandle, EVENT_ALL_ACCESS,
                                                          *ExEventObjectType, KernelMode, &evObj, NULL))) {
                    g_UnloadEvent = (PKEVENT)evObj;
                    KeClearEvent(g_UnloadEvent);
                    LOG_INFO("Unload event ready");
                }
            } else {
                LOG_ERROR("ZwCreateEvent failed 0x%X", es);
            }
        }
    }

    // ---- create worker-exit done event ----
    {
        UNICODE_STRING evName;
        RtlInitUnicodeString(&evName, ISVALVEDS_DONE_KERNEL_NAME);
        SECURITY_DESCRIPTOR sd;
        if (NT_SUCCESS(RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) &&
            NT_SUCCESS(RtlSetDaclSecurityDescriptor(&sd, TRUE, NULL, FALSE))) {
            OBJECT_ATTRIBUTES oa;
            InitializeObjectAttributes(&oa, &evName,
                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_OPENIF, NULL, &sd);
            NTSTATUS es = ZwCreateEvent(&g_DoneEventHandle, EVENT_ALL_ACCESS, &oa,
                                         NotificationEvent, FALSE);
            if (NT_SUCCESS(es)) {
                PVOID evObj = NULL;
                if (NT_SUCCESS(ObReferenceObjectByHandle(g_DoneEventHandle, EVENT_ALL_ACCESS,
                                                          *ExEventObjectType, KernelMode, &evObj, NULL))) {
                    g_DoneEvent = (PKEVENT)evObj;
                    KeClearEvent(g_DoneEvent);
                    LOG_INFO("Done event ready");
                }
            } else {
                LOG_ERROR("ZwCreateEvent(done) failed 0x%X", es);
            }
        }
    }

    if (!g_State || !g_UnloadEvent || !g_DoneEvent) {
        LOG_ERROR("Control plane incomplete (state=%p stop=%p done=%p) - aborting load",
                  g_State, g_UnloadEvent, g_DoneEvent);
        CleanupRuntimeResources(FALSE);
        return STATUS_UNSUCCESSFUL;
    }

    NTSTATUS ps = PsSetCreateProcessNotifyRoutine(ProcessExitCallback, FALSE);
    if (NT_SUCCESS(ps)) { g_ProcessNotifyRegistered = TRUE; LOG_INFO("ProcessExitCallback registered"); }
    else                LOG_WARN("PsSetCreateProcessNotifyRoutine: 0x%X", ps);

    HANDLE h = NULL;
    NTSTATUS ts = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                        WorkerThread, NULL);
    if (NT_SUCCESS(ts)) {
        ZwClose(h);
        LOG_INFO("Driver loaded");
        return STATUS_SUCCESS;
    }
    LOG_ERROR("PsCreateSystemThread failed: 0x%X", ts);
    CleanupRuntimeResources(FALSE);
    return ts;
}
