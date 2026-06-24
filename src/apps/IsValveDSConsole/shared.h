#pragma once

//
// Shared definitions between kernel driver and user-mode console.
// Keep identical copies in IsValveDS_Driver/ and IsValveDS_Console/.
//

#define ISVALVEDS_SHM_KERNEL_NAME    L"\\BaseNamedObjects\\Global\\IsValveDSState"
#define ISVALVEDS_STOP_KERNEL_NAME   L"\\BaseNamedObjects\\Global\\IsValveDSStop"
#define ISVALVEDS_DONE_KERNEL_NAME   L"\\BaseNamedObjects\\Global\\IsValveDSStopped"

#define ISVALVEDS_SHM_USER_NAME      "Global\\IsValveDSState"
#define ISVALVEDS_STOP_USER_NAME     "Global\\IsValveDSStop"
#define ISVALVEDS_DONE_USER_NAME     "Global\\IsValveDSStopped"

#define ISVALVEDS_MAGIC              0x1DEA1D5Cu

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
    unsigned int       magic;

    int                current_value;
    unsigned int       current_error;
    unsigned long long current_address;
    unsigned long long last_poll_systime;
    unsigned int       driver_tick;
    unsigned int       cs2_pid;
    unsigned long long client_base;

    unsigned int       desired_value;
    unsigned int       write_request_id;

    unsigned int       write_handled_id;
    unsigned int       write_error;
    int                write_result_value;
    unsigned long long write_handled_systime;

    unsigned int       pad[16];
} ISVALVEDS_STATE;
#pragma pack(pop)
