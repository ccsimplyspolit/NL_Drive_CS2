//
// IsValveDS Spoofer - user-mode console.
//
// Opens the SHM section published by IsValveDS_Driver and the named stop
// event. Auto-polls the SHM every 3 seconds, displays the current value, and
// lets the user request a write by typing 0/1. Writes are issued by setting
// desired_value + bumping write_request_id; the driver re-reads target memory,
// performs the write (only if needed), then publishes write_handled_id +
// write_result_value, which we surface as feedback.
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <conio.h>
#include <atomic>
#include <mutex>
#include <thread>

#include "shared.h"

static constexpr DWORD POLL_INTERVAL_MS = 3000;

static std::mutex        g_ioMutex;
static HANDLE            g_shmHandle = nullptr;
static ISVALVEDS_STATE*  g_state     = nullptr;
static HANDLE            g_stopEvent = nullptr;
static std::atomic<bool> g_stop      { false };
static std::atomic<unsigned int> g_nextReqId { 1 };

// --------------------------------------------------------------------------

static const char* ErrToString(unsigned int e)
{
    switch (e) {
    case VDS_ERR_OK:             return "OK";
    case VDS_ERR_NO_CS2:         return "cs2.exe not running";
    case VDS_ERR_NO_CLIENT_DLL:  return "client.dll not loaded";
    case VDS_ERR_GAMERULES_NULL: return "GameRules NULL (main menu)";
    case VDS_ERR_READ_FAILED:    return "memory read failed";
    case VDS_ERR_WRITE_FAILED:   return "memory write failed";
    case VDS_ERR_BAD_VALUE:      return "bad value";
    case VDS_ERR_TIMEOUT:        return "driver response timeout";
    default:                     return "unknown";
    }
}

static void Timestamp(char* buf, size_t cap)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snprintf_s(buf, cap, _TRUNCATE, "%02u:%02u:%02u",
                t.wHour, t.wMinute, t.wSecond);
}

// Read SHM atomically: snapshot magic before & after, retry if torn.
struct StateSnap {
    bool          valid;
    int           current_value;
    unsigned int  current_error;
    unsigned long long current_address;
    unsigned long long client_base;
    unsigned int  driver_tick;
    unsigned int  cs2_pid;
};

static bool ReadSnap(StateSnap& s)
{
    s = {};
    if (!g_state) return false;
    for (int attempt = 0; attempt < 4; attempt++) {
        unsigned int m1 = g_state->magic;
        if (m1 != ISVALVEDS_MAGIC) {
            // Either not initialized yet, or driver is mid-update.
            Sleep(1);
            continue;
        }
        _ReadWriteBarrier();
        s.current_value   = g_state->current_value;
        s.current_error   = g_state->current_error;
        s.current_address = g_state->current_address;
        s.client_base     = g_state->client_base;
        s.driver_tick     = g_state->driver_tick;
        s.cs2_pid         = g_state->cs2_pid;
        _ReadWriteBarrier();
        unsigned int m2 = g_state->magic;
        if (m1 == m2 && m2 == ISVALVEDS_MAGIC) {
            s.valid = true;
            return true;
        }
        Sleep(1);
    }
    return false;
}

// --------------------------------------------------------------------------

static void PrintLine(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_ioMutex);
    printf("\r%s\n> ", buf);
    fflush(stdout);
}

static void Prompt()
{
    std::lock_guard<std::mutex> lk(g_ioMutex);
    printf("> ");
    fflush(stdout);
}

static void PrintSnap(const StateSnap& s, const char* tag)
{
    char ts[16]; Timestamp(ts, sizeof(ts));
    char body[256];
    if (s.current_error == VDS_ERR_OK && s.current_value >= 0) {
        _snprintf_s(body, sizeof(body), _TRUNCATE,
                    "value=%d @ 0x%016llX  (tick=%u pid=%u)",
                    s.current_value, s.current_address,
                    s.driver_tick, s.cs2_pid);
    } else {
        _snprintf_s(body, sizeof(body), _TRUNCATE,
                    "unavailable: %s  (tick=%u pid=%u)",
                    ErrToString(s.current_error),
                    s.driver_tick, s.cs2_pid);
    }
    std::lock_guard<std::mutex> lk(g_ioMutex);
    printf("\r[%s] %s %s\n> ", ts, tag, body);
    fflush(stdout);
}

// --------------------------------------------------------------------------

static void PollerThread()
{
    int           lastShownValue = -2;
    unsigned int  lastShownError = (unsigned)-1;
    while (!g_stop.load(std::memory_order_relaxed)) {
        StateSnap s{};
        if (ReadSnap(s)) {
            bool changed = (s.current_value != lastShownValue)
                        || (s.current_error != lastShownError);
            PrintSnap(s, changed ? "poll *" : "poll  ");
            lastShownValue = s.current_value;
            lastShownError = s.current_error;
        } else {
            PrintLine("[poll  ] SHM not yet readable (driver loaded?)");
        }
        for (DWORD slept = 0; slept < POLL_INTERVAL_MS && !g_stop.load(); slept += 100)
            Sleep(100);
    }
}

// --------------------------------------------------------------------------

static bool RequestWrite(unsigned int desired, unsigned int timeoutMs,
                         unsigned int& outErr, int& outReadback)
{
    if (!g_state) {
        outErr = VDS_ERR_NO_CS2;
        outReadback = -1;
        return false;
    }

    unsigned int reqId = g_nextReqId.fetch_add(1, std::memory_order_relaxed);
    if (reqId == 0) reqId = g_nextReqId.fetch_add(1, std::memory_order_relaxed); // skip 0

    // Publish desired value + request id. Driver polls SHM in its inner loop.
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_state->desired_value),
                        desired ? 1L : 0L);
    _ReadWriteBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_state->write_request_id),
                        static_cast<LONG>(reqId));

    // Wait for driver to acknowledge.
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        unsigned int handled = g_state->write_handled_id;
        if (handled == reqId) {
            outErr      = g_state->write_error;
            outReadback = g_state->write_result_value;
            return true;
        }
        Sleep(20);
    }
    outErr      = VDS_ERR_TIMEOUT;
    outReadback = -1;
    return false;
}

// --------------------------------------------------------------------------

static void Banner()
{
    printf("==============================================\n");
    printf(" IsValveDS Spoofer - console (SHM, no IRP)\n");
    printf(" shm   : %s\n", ISVALVEDS_SHM_USER_NAME);
    printf(" event : %s\n", ISVALVEDS_STOP_USER_NAME);
    printf(" poll  : every %lu ms\n", (unsigned long)POLL_INTERVAL_MS);
    printf("==============================================\n");
}

static void Help()
{
    std::lock_guard<std::mutex> lk(g_ioMutex);
    printf("\nCommands:\n");
    printf("  r        - force re-read now (latest snapshot from SHM)\n");
    printf("  0        - write 0 (not Valve DS)\n");
    printf("  1        - write 1 (is Valve DS)\n");
    printf("  w <0|1>  - same as `0` / `1`\n");
    printf("  s        - same as `r`\n");
    printf("  stop     - signal the driver to release SHM/event (full unload)\n");
    printf("  h, ?     - this help\n");
    printf("  q        - quit (driver stays loaded)\n\n> ");
    fflush(stdout);
}

// --------------------------------------------------------------------------

int main(int argc, char** argv)
{
    Banner();

    // CLI: --unload triggers stop and exits.
    bool unloadOnly = false;
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--unload") || !_stricmp(argv[i], "-u")) {
            unloadOnly = true;
        }
    }

    g_stopEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, ISVALVEDS_STOP_USER_NAME);
    if (unloadOnly) {
        if (!g_stopEvent) {
            printf("[!] Driver not loaded (event %s not found).\n",
                   ISVALVEDS_STOP_USER_NAME);
            return 1;
        }
        SetEvent(g_stopEvent);
        CloseHandle(g_stopEvent);
        printf("[+] Stop signal sent.\n");
        return 0;
    }

    g_shmHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, ISVALVEDS_SHM_USER_NAME);
    if (!g_shmHandle) {
        DWORD e = GetLastError();
        printf("[!] Cannot open SHM %s (GetLastError=%lu).\n",
               ISVALVEDS_SHM_USER_NAME, e);
        printf("    Make sure IsValveDS_Driver.sys is loaded (run.bat / kdmapper).\n");
        if (g_stopEvent) CloseHandle(g_stopEvent);
        printf("\nPress any key to exit...");
        _getch();
        return 1;
    }
    g_state = (ISVALVEDS_STATE*)MapViewOfFile(g_shmHandle, FILE_MAP_ALL_ACCESS,
                                               0, 0, sizeof(ISVALVEDS_STATE));
    if (!g_state) {
        DWORD e = GetLastError();
        printf("[!] MapViewOfFile failed (GetLastError=%lu).\n", e);
        CloseHandle(g_shmHandle);
        if (g_stopEvent) CloseHandle(g_stopEvent);
        return 1;
    }
    printf("[+] SHM mapped at %p.\n", (void*)g_state);

    {
        StateSnap s{};
        if (ReadSnap(s)) {
            PrintSnap(s, "init  ");
        } else {
            printf("[i] SHM not yet initialized; first poll will fill it.\n");
        }
    }
    printf("[+] auto-poll started; type 'h' for commands.\n");

    std::thread poller(PollerThread);
    Prompt();

    char line[64];
    while (fgets(line, (int)sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r' ||
                     line[n-1] == ' '  || line[n-1] == '\t')) line[--n] = 0;
        char* p = line; while (*p == ' ' || *p == '\t') p++;
        if (!*p) { Prompt(); continue; }

        if (!_stricmp(p, "q") || !_stricmp(p, "quit") || !_stricmp(p, "exit")) break;

        if (!_stricmp(p, "h") || !_stricmp(p, "?") || !_stricmp(p, "help")) {
            Help(); continue;
        }

        if (!_stricmp(p, "r") || !_stricmp(p, "read") ||
            !_stricmp(p, "s") || !_stricmp(p, "show") || !_stricmp(p, "status")) {
            StateSnap s{};
            if (ReadSnap(s)) PrintSnap(s, "show  ");
            else             PrintLine("[show  ] SHM not readable");
            continue;
        }

        if (!_stricmp(p, "stop") || !_stricmp(p, "unload")) {
            if (g_stopEvent) {
                SetEvent(g_stopEvent);
                PrintLine("[stop  ] stop signal sent to driver");
            } else {
                PrintLine("[stop  ] stop event not opened");
            }
            continue;
        }

        const char* arg = nullptr;
        if (p[0] == '0' && p[1] == 0) arg = "0";
        else if (p[0] == '1' && p[1] == 0) arg = "1";
        else if (!_strnicmp(p, "w ", 2) || !_strnicmp(p, "write ", 6)) {
            arg = strchr(p, ' ');
            if (arg) { while (*arg == ' ') arg++; }
        }

        if (arg && (arg[0] == '0' || arg[0] == '1') && arg[1] == 0) {
            unsigned int v = (unsigned int)(arg[0] - '0');
            unsigned int werr = 0;
            int readback = -1;
            if (RequestWrite(v, 2000, werr, readback)) {
                if (werr == VDS_ERR_OK) {
                    PrintLine("[write ] OK: requested=%u, readback=%d", v, readback);
                } else {
                    PrintLine("[write ] FAIL: %s (readback=%d)",
                              ErrToString(werr), readback);
                }
            } else {
                PrintLine("[write ] FAIL: %s", ErrToString(werr));
            }
            continue;
        }

        PrintLine("unknown command '%s' (type 'h')", p);
    }

    g_stop.store(true);
    if (poller.joinable()) poller.join();

    if (g_state) UnmapViewOfFile(g_state);
    if (g_shmHandle) CloseHandle(g_shmHandle);
    if (g_stopEvent) CloseHandle(g_stopEvent);
    return 0;
}
