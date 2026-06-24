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
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

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
// File logger. Writes to IsValveDS_Console.log next to the exe. Every
// non-trivial event in this process should go through LOG so that when the
// console silently exits on a customer machine we can hand the user one log
// file that explains exactly which step failed and why. The console is also
// echoed (LOG_T -> stdout AND log file).
// --------------------------------------------------------------------------
static FILE*       g_logFp     = nullptr;
static std::mutex  g_logMutex;
static char        g_logPath[MAX_PATH] = {};

static void LogOpen()
{
    char exePath[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (n == 0 || n == MAX_PATH) {
        strcpy_s(exePath, "IsValveDS_Console.exe");
    }
    char* slash = strrchr(exePath, '\\');
    if (slash) {
        size_t base = (size_t)(slash - exePath + 1);
        _snprintf_s(g_logPath, sizeof(g_logPath), _TRUNCATE,
                    "%.*sIsValveDS_Console.log", (int)base, exePath);
    } else {
        strcpy_s(g_logPath, "IsValveDS_Console.log");
    }

    fopen_s(&g_logFp, g_logPath, "ab");  // append, binary so \r\n is literal
    if (g_logFp) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(g_logFp,
            "\r\n========================================================\r\n"
            "IsValveDS_Console started %04u-%02u-%02u %02u:%02u:%02u.%03u\r\n"
            "PID=%lu  exe=%s\r\n"
            "========================================================\r\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (unsigned long)GetCurrentProcessId(), exePath);
        fflush(g_logFp);
    }
}

static void LogClose()
{
    if (g_logFp) {
        std::lock_guard<std::mutex> lk(g_logMutex);
        fprintf(g_logFp, "----- end -----\r\n");
        fclose(g_logFp);
        g_logFp = nullptr;
    }
}

static void LogWriteV(const char* level, const char* fmt, va_list ap)
{
    char body[1024];
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);

    SYSTEMTIME t; GetLocalTime(&t);

    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFp) {
        fprintf(g_logFp, "[%02u:%02u:%02u.%03u] %-5s %s\r\n",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, body);
        fflush(g_logFp);
    }
}

// LOG -> file only.
static void LOG(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    LogWriteV("INFO", fmt, ap);
    va_end(ap);
}

// LOG_W / LOG_E -> file only, tagged.
static void LOG_W(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    LogWriteV("WARN", fmt, ap);
    va_end(ap);
}

static void LOG_E(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    LogWriteV("ERROR", fmt, ap);
    va_end(ap);
}

// Format Win32 error code -> human string. Caller frees nothing.
static void FormatWinErr(DWORD code, char* buf, size_t cap)
{
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, (DWORD)cap, NULL);
    if (n == 0) {
        _snprintf_s(buf, cap, _TRUNCATE, "(no system text for code %lu)", code);
        return;
    }
    // Trim CR/LF tail FormatMessage adds.
    while (n && (buf[n-1] == '\r' || buf[n-1] == '\n' || buf[n-1] == ' '))
        buf[--n] = 0;
}

// SEH filter -> dump exception info to log before crashing.
static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord) {
        LOG_E("unhandled exception with no ExceptionRecord");
        return EXCEPTION_EXECUTE_HANDLER;
    }
    LOG_E("UNHANDLED EXCEPTION code=0x%08X flags=0x%08X addr=%p numparams=%u",
          ep->ExceptionRecord->ExceptionCode,
          ep->ExceptionRecord->ExceptionFlags,
          ep->ExceptionRecord->ExceptionAddress,
          ep->ExceptionRecord->NumberParameters);
    for (DWORD i = 0; i < ep->ExceptionRecord->NumberParameters && i < 4; i++) {
        LOG_E("  param[%lu] = 0x%llX", i,
              (unsigned long long)ep->ExceptionRecord->ExceptionInformation[i]);
    }
    if (g_logFp) {
        fflush(g_logFp);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// CRT runtime error dump (pure virtual, invalid parameter, etc.). All of these
// abort the process by default with no message.
static void __cdecl OnInvalidParameter(
    const wchar_t* expression, const wchar_t* function, const wchar_t* file,
    unsigned int line, uintptr_t /*reserved*/)
{
    char e[256] = {}, f[256] = {}, fn[256] = {};
    if (expression) WideCharToMultiByte(CP_UTF8, 0, expression, -1, e, sizeof(e), NULL, NULL);
    if (function)   WideCharToMultiByte(CP_UTF8, 0, function,   -1, f, sizeof(f), NULL, NULL);
    if (file)       WideCharToMultiByte(CP_UTF8, 0, file,       -1, fn, sizeof(fn), NULL, NULL);
    LOG_E("CRT invalid parameter: expr=\"%s\" fn=\"%s\" file=\"%s\" line=%u",
          e[0] ? e : "(none)", f[0] ? f : "(none)", fn[0] ? fn : "(none)", line);
    if (g_logFp) fflush(g_logFp);
}

static void OnPureCall()
{
    LOG_E("CRT pure virtual function call");
    if (g_logFp) fflush(g_logFp);
}

static void OnTerminate()
{
    LOG_E("std::terminate called (uncaught C++ exception)");
    if (g_logFp) fflush(g_logFp);
    std::abort();
}

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
    LOG("PollerThread enter");
    int           lastShownValue = -2;
    unsigned int  lastShownError = (unsigned)-1;
    unsigned int  ticks          = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        StateSnap s{};
        if (ReadSnap(s)) {
            bool changed = (s.current_value != lastShownValue)
                        || (s.current_error != lastShownError);
            PrintSnap(s, changed ? "poll *" : "poll  ");
            if (changed) {
                LOG("poll change: value=%d err=%u addr=0x%llX tick=%u pid=%u",
                    s.current_value, s.current_error,
                    (unsigned long long)s.current_address,
                    s.driver_tick, s.cs2_pid);
            }
            lastShownValue = s.current_value;
            lastShownError = s.current_error;
        } else {
            PrintLine("[poll  ] SHM not yet readable (driver loaded?)");
            LOG_W("poll: SHM magic torn or zero (tick=%u)", ticks);
        }
        ticks++;
        for (DWORD slept = 0; slept < POLL_INTERVAL_MS && !g_stop.load(); slept += 100)
            Sleep(100);
    }
    LOG("PollerThread exit (ticks=%u)", ticks);
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
    // Open the log file BEFORE anything else, so even fatally-early crashes
    // (CRT runtime missing, DLL load failure stub) at least leave a trace via
    // SetUnhandledExceptionFilter -> file. atexit handles normal exit paths.
    LogOpen();
    atexit(LogClose);

    SetUnhandledExceptionFilter(UnhandledFilter);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
    std::set_terminate(OnTerminate);

    LOG("argc=%d", argc);
    for (int i = 0; i < argc; i++) LOG("  argv[%d]=%s", i, argv[i] ? argv[i] : "(null)");

    // Quick environment dump so the log shows whether the user is admin, what
    // session they're in, what cwd, etc. -- the kind of info we routinely have
    // to ask for via Telegram screenshots.
    {
        char cwd[MAX_PATH] = {}; GetCurrentDirectoryA(MAX_PATH, cwd);
        DWORD sid = 0; ProcessIdToSessionId(GetCurrentProcessId(), &sid);
        char user[256] = {}; DWORD ulen = sizeof(user); GetUserNameA(user, &ulen);
        char comp[256] = {}; DWORD clen = sizeof(comp); GetComputerNameA(comp, &clen);
        LOG("env: cwd=%s session=%lu user=%s computer=%s", cwd, sid, user, comp);

        OSVERSIONINFOEXW osv{}; osv.dwOSVersionInfoSize = sizeof(osv);
        // Use RtlGetVersion via ntdll so the values are not lied to by manifest.
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        typedef LONG (WINAPI *FnRtlGetVersion)(LPOSVERSIONINFOEXW);
        if (nt) {
            FnRtlGetVersion fn = (FnRtlGetVersion)GetProcAddress(nt, "RtlGetVersion");
            if (fn && fn(&osv) == 0) {
                LOG("env: Windows %lu.%lu build %lu (sp=%u, type=%u)",
                    osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber,
                    osv.wServicePackMajor, osv.wProductType);
            }
        }
    }

    Banner();
    LOG("Banner printed");

    // CLI: --unload triggers stop and exits.
    bool unloadOnly = false;
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--unload") || !_stricmp(argv[i], "-u")) {
            unloadOnly = true;
        }
    }
    LOG("unloadOnly=%d", unloadOnly ? 1 : 0);

    LOG("OpenEventA(%s)", ISVALVEDS_STOP_USER_NAME);
    g_stopEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, ISVALVEDS_STOP_USER_NAME);
    if (!g_stopEvent) {
        DWORD e = GetLastError();
        char m[256]; FormatWinErr(e, m, sizeof(m));
        LOG_W("OpenEventA failed: %lu (%s)", e, m);
    } else {
        LOG("stop event opened ok: handle=%p", (void*)g_stopEvent);
    }

    if (unloadOnly) {
        if (!g_stopEvent) {
            printf("[!] Driver not loaded (event %s not found).\n",
                   ISVALVEDS_STOP_USER_NAME);
            return 1;
        }
        SetEvent(g_stopEvent);
        CloseHandle(g_stopEvent);
        printf("[+] Stop signal sent.\n");
        LOG("--unload completed, exit 0");
        return 0;
    }

    LOG("OpenFileMappingA(%s)", ISVALVEDS_SHM_USER_NAME);
    g_shmHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, ISVALVEDS_SHM_USER_NAME);
    if (!g_shmHandle) {
        DWORD e = GetLastError();
        char m[256]; FormatWinErr(e, m, sizeof(m));
        LOG_E("OpenFileMapping failed: %lu (%s)", e, m);
        printf("[!] Cannot open SHM %s (GetLastError=%lu: %s).\n",
               ISVALVEDS_SHM_USER_NAME, e, m);
        printf("    Make sure IsValveDS_Driver.sys is loaded (run.bat / kdmapper).\n");
        printf("    Log: %s\n", g_logPath);
        if (g_stopEvent) CloseHandle(g_stopEvent);
        printf("\nPress any key to exit...");
        _getch();
        return 1;
    }
    LOG("SHM handle ok: %p", (void*)g_shmHandle);

    LOG("MapViewOfFile size=%zu", sizeof(ISVALVEDS_STATE));
    g_state = (ISVALVEDS_STATE*)MapViewOfFile(g_shmHandle, FILE_MAP_ALL_ACCESS,
                                               0, 0, sizeof(ISVALVEDS_STATE));
    if (!g_state) {
        DWORD e = GetLastError();
        char m[256]; FormatWinErr(e, m, sizeof(m));
        LOG_E("MapViewOfFile failed: %lu (%s)", e, m);
        printf("[!] MapViewOfFile failed (GetLastError=%lu: %s).\n", e, m);
        printf("    Log: %s\n", g_logPath);
        CloseHandle(g_shmHandle);
        if (g_stopEvent) CloseHandle(g_stopEvent);
        return 1;
    }
    LOG("SHM mapped at %p", (void*)g_state);
    printf("[+] SHM mapped at %p.\n", (void*)g_state);

    {
        StateSnap s{};
        if (ReadSnap(s)) {
            PrintSnap(s, "init  ");
            LOG("initial snap: value=%d err=%u addr=0x%llX tick=%u pid=%u",
                s.current_value, s.current_error,
                (unsigned long long)s.current_address,
                s.driver_tick, s.cs2_pid);
        } else {
            printf("[i] SHM not yet initialized; first poll will fill it.\n");
            LOG("initial snap not yet readable (driver mid-publish or just loaded)");
        }
    }
    printf("[+] auto-poll started; type 'h' for commands.\n");
    LOG("starting poller thread");

    std::thread poller(PollerThread);

    // Open CONIN$ directly. fgets(stdin) silently returns NULL on systems
    // where the parent process delivered an unusual stdin handle (observed
    // on a modified Windows 10 22H2 "ProMod" build): the loop would exit
    // immediately, the user just saw the banner and then "Console exited."
    // CONIN$ talks to the console buffer directly and is independent of
    // stdin redirection.
    LOG("CreateFileA(CONIN$)");
    HANDLE conin = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (conin == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        char m[256]; FormatWinErr(e, m, sizeof(m));
        LOG_W("CONIN$ open failed: %lu (%s) - entering headless mode", e, m);
        // No console at all (truly detached / piped). Fall back to
        // headless mode: poller keeps printing, main thread waits on the
        // stop event or Ctrl+C. This is better than exiting in 5ms.
        printf("[i] no console input handle available; running headless\n");
        printf("    (press Ctrl+C to quit; poller still runs every %lu ms)\n",
               (unsigned long)POLL_INTERVAL_MS);
        printf("    Log: %s\n", g_logPath);
        fflush(stdout);
        while (!g_stop.load(std::memory_order_relaxed)) Sleep(500);
        LOG("headless mode: g_stop set, joining poller");
        if (poller.joinable()) poller.join();
        if (g_state) UnmapViewOfFile(g_state);
        if (g_shmHandle) CloseHandle(g_shmHandle);
        if (g_stopEvent) CloseHandle(g_stopEvent);
        LOG("exit 0 from headless");
        return 0;
    }
    LOG("CONIN$ opened: %p", (void*)conin);

    Prompt();
    LOG("entering command loop");

    char line[64];
    while (true) {
        DWORD nread = 0;
        // ReadConsoleA blocks until a line is committed (Enter) or the
        // console is closed. It does NOT depend on the CRT's stdin state.
        BOOL rc = ReadConsoleA(conin, line, (DWORD)sizeof(line) - 1, &nread, NULL);
        if (!rc) {
            DWORD e = GetLastError();
            char m[256]; FormatWinErr(e, m, sizeof(m));
            LOG_E("ReadConsoleA failed: rc=0 err=%lu (%s) - exiting cmd loop", e, m);
            break;
        }
        if (nread == 0) {
            LOG("ReadConsoleA returned 0 bytes - exiting cmd loop");
            break;
        }
        line[nread] = 0;
        LOG("cmd in: \"%s\" (nread=%lu)", line, nread);
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

    LOG("command loop exited, stopping poller");
    g_stop.store(true);
    if (poller.joinable()) poller.join();

    if (conin != INVALID_HANDLE_VALUE) CloseHandle(conin);
    if (g_state) UnmapViewOfFile(g_state);
    if (g_shmHandle) CloseHandle(g_shmHandle);
    if (g_stopEvent) CloseHandle(g_stopEvent);
    LOG("graceful exit 0");
    return 0;
}
