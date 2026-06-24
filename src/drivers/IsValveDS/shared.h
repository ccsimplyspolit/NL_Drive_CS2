#pragma once

//
// Shared definitions between kernel driver and user-mode console.
// Keep identical copies in IsValveDS_Driver/ and IsValveDS_Console/.
//
// Architecture (F20Driver-style, kdmapper-safe):
//   - NO IRP / IoCreateDevice. kdmapper-mapped drivers should not register
//     IRP dispatch because their DRIVER_OBJECT pointer goes stale once
//     DriverEntry returns.
//   - Driver creates a named SECTION (shared memory) and a named EVENT.
//   - User mode opens both by name (OpenFileMapping + MapViewOfFile,
//     OpenEvent + SetEvent).
//

// Kernel-side names: create the objects directly under \BaseNamedObjects,
// NOT under \BaseNamedObjects\Global. The user-mode "Global\X" prefix is a
// Win32 shorthand that resolves to \BaseNamedObjects\X (without an extra
// "Global" path component). On stock Windows there is also a symlink
// \BaseNamedObjects\Global -> \BaseNamedObjects, which is why the old paths
// happened to work, but on hardened / modified Windows builds (one user
// reported "Windows 10 22H2 ProMod UEFI") that symlink may not be present,
// and the user-mode console would silently fail to open SHM. Matching what
// F20Driver already does keeps both kits behavior-consistent.
#define ISVALVEDS_SHM_KERNEL_NAME    L"\\BaseNamedObjects\\IsValveDSState"
#define ISVALVEDS_STOP_KERNEL_NAME   L"\\BaseNamedObjects\\IsValveDSStop"
#define ISVALVEDS_DONE_KERNEL_NAME   L"\\BaseNamedObjects\\IsValveDSStopped"

// User-mode names (Win32 layer translates "Global\X" -> \BaseNamedObjects\X).
#define ISVALVEDS_SHM_USER_NAME      "Global\\IsValveDSState"
#define ISVALVEDS_STOP_USER_NAME     "Global\\IsValveDSStop"
#define ISVALVEDS_DONE_USER_NAME     "Global\\IsValveDSStopped"

// Sanity marker; driver writes this *last* after updating current_* fields.
// Consumer must verify magic==ISVALVEDS_MAGIC after reading.
#define ISVALVEDS_MAGIC              0x1DEA1D5Cu

// Error codes shared by current_error and write_error.
#define VDS_ERR_OK                   0
#define VDS_ERR_NO_CS2               1
#define VDS_ERR_NO_CLIENT_DLL        2
#define VDS_ERR_GAMERULES_NULL       3
#define VDS_ERR_READ_FAILED          4
#define VDS_ERR_WRITE_FAILED         5
#define VDS_ERR_BAD_VALUE            6
#define VDS_ERR_TIMEOUT              7

#pragma pack(push, 1)
typedef struct _ISVALVEDS_STATE {
    unsigned int       magic;               // = ISVALVEDS_MAGIC when current_* valid

    // ---- driver-owned (user reads, never writes) ------------------------
    int                current_value;        // 0 / 1, or -1 if unavailable
    unsigned int       current_error;        // VDS_ERR_*
    unsigned long long current_address;      // abs addr of m_bIsValveDS in cs2
    unsigned long long last_poll_systime;    // KeQuerySystemTime() snapshot
    unsigned int       driver_tick;          // monotonically increasing per poll
    unsigned int       cs2_pid;              // 0 if cs2 not found
    unsigned long long client_base;          // client.dll base in cs2 (or 0)

    // ---- user-owned (driver reads, never writes) ------------------------
    unsigned int       desired_value;        // 0 or 1
    unsigned int       write_request_id;     // user increments to request write

    // ---- driver-owned write response (user reads) -----------------------
    unsigned int       write_handled_id;     // matches write_request_id when done
    unsigned int       write_error;          // VDS_ERR_* for the last write
    int                write_result_value;   // value after the last write attempt
    unsigned long long write_handled_systime;// KeQuerySystemTime at completion

    unsigned int       pad[16];
} ISVALVEDS_STATE;
#pragma pack(pop)
