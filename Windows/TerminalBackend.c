// TerminalBackend for Windows. It serves the same web app and HTTP API as the
// macOS/Linux backend (see Backend/main.c) on Outer Shell's named pipe for the
// app, and runs each session in a keeper process (this program, started with
// --keeper) that owns a pseudoconsole and outlives the backend.
//
// Keepers listen on a named pipe that only the current user can open. The
// pipe's name ends with a random suffix and is recorded in
// %LOCALAPPDATA%\outershell\terminal\sessions\ID.session, a directory only
// the user can read.
#include "Runtime.h"
#include "../Backend/Session.h"

#include <bcrypt.h>
#include <process.h>
#include <sddl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

static HANDLE app_stop;
static char *app_web;
static wchar_t app_executable[32768];

static BOOL WINAPI stop_app(DWORD event) {
    (void)event;
    SetEvent(app_stop);
    return TRUE;
}

static bool random_hex(char *out, size_t bytes) {
    uint8_t random[32];
    if (bytes > sizeof(random) || BCryptGenRandom(NULL, random, (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    for (size_t i = 0; i < bytes; i++) snprintf(out + 2 * i, 3, "%02x", random[i]);
    return true;
}

static char *sessions_directory(void) {
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    if (!local) return NULL;
    char *outershell = win_join(local, "outershell");
    char *terminal = outershell ? win_join(outershell, "terminal") : NULL;
    char *sessions = terminal ? win_join(terminal, "sessions") : NULL;
    const char *levels[] = {outershell, terminal, sessions};
    for (size_t i = 0; i < 3 && levels[i]; i++) {
        wchar_t *wide = win_wide(levels[i]);
        if (wide) CreateDirectoryW(wide, NULL);
        free(wide);
    }
    free(local);
    free(outershell);
    free(terminal);
    return sessions;
}

static char *metadata_path(const char *id) {
    char *directory = sessions_directory();
    if (!directory) return NULL;
    char name[64];
    snprintf(name, sizeof(name), "%s.session", id);
    char *path = win_join(directory, name);
    free(directory);
    return path;
}

static bool read_endpoint(const char *path, char *endpoint, size_t capacity) {
    StringBuilder text = {0};
    bool ok = win_read_file(path, capacity - 1, &text) && text.data;
    if (ok) {
        snprintf(endpoint, capacity, "%s", text.data);
        endpoint[strcspn(endpoint, "\r\n")] = 0;
        ok = !_strnicmp(endpoint, "\\\\.\\pipe\\org.outershell.terminal.", 33);
    }
    free(text.data);
    return ok;
}

// Overlapped I/O on a pipe, so one thread can read while another writes.
static bool pipe_transfer(HANDLE pipe, bool write, void *data, DWORD size, DWORD *done, DWORD timeout) {
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) return false;
    BOOL ok = write ? WriteFile(pipe, data, size, NULL, &overlapped) : ReadFile(pipe, data, size, NULL, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(overlapped.hEvent, timeout) != WAIT_OBJECT_0) CancelIoEx(pipe, &overlapped);
        ok = TRUE;
    }
    ok = ok && GetOverlappedResult(pipe, &overlapped, done, TRUE);
    CloseHandle(overlapped.hEvent);
    return ok && *done > 0;
}

static bool pipe_write_all(HANDLE pipe, const char *data, size_t size, DWORD timeout) {
    while (size > 0) {
        DWORD written = 0;
        DWORD chunk = size > 65536 ? 65536 : (DWORD)size;
        if (!pipe_transfer(pipe, true, (void *)data, chunk, &written, timeout)) return false;
        data += written;
        size -= written;
    }
    return true;
}

static HANDLE connect_keeper(const char *endpoint, bool *stale) {
    *stale = false;
    wchar_t *wide = win_wide(endpoint);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; wide && attempt < 20; attempt++) {
        pipe = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
        if (pipe != INVALID_HANDLE_VALUE) break;
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            *stale = true;
            break;
        }
        if (error != ERROR_PIPE_BUSY || !WaitNamedPipeW(wide, 250)) Sleep(50);
    }
    free(wide);
    return pipe;
}

static bool keeper_request(const char *endpoint, uint8_t purpose, uint8_t expected, Bytes *payload, bool *stale) {
    HANDLE pipe = connect_keeper(endpoint, stale);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    Bytes hello = {0};
    bool ok = session_append_hello(&hello, purpose, 0, 0, 0) && pipe_write_all(pipe, hello.data, hello.size, 2000);
    bytes_free(&hello);
    SessionFrameReader reader = {0};
    bool found = false;
    char buffer[16384];
    while (ok && !found) {
        DWORD count = 0;
        if (!pipe_transfer(pipe, false, buffer, sizeof(buffer), &count, 2000) || !bytes_append(&reader.pending, buffer, count)) break;
        uint8_t type;
        const char *data;
        uint32_t size;
        int next;
        while (!found && (next = session_next_frame(&reader, &type, &data, &size)) == 1) {
            if (type == expected) {
                found = true;
                if (payload) bytes_append(payload, data, size);
            } else if (type == SESSION_FRAME_ENDED) {
                found = expected == SESSION_FRAME_ENDED;
                ok = found;
            }
        }
        if (next < 0) ok = false;
    }
    CloseHandle(pipe);
    bytes_free(&reader.pending);
    return ok && found;
}

typedef struct {
    SessionInfo info;
    char endpoint[256];
} SessionEntry;

static int compare_entries(const void *a, const void *b) {
    const SessionEntry *left = a;
    const SessionEntry *right = b;
    if (left->info.active != right->info.active) return left->info.active > right->info.active ? -1 : 1;
    return strcmp(left->info.id, right->info.id);
}

static bool describe_session(const char *path, SessionEntry *entry) {
    char endpoint[256];
    if (!read_endpoint(path, endpoint, sizeof(endpoint))) {
        wchar_t *wide = win_wide(path);
        if (wide) DeleteFileW(wide);
        free(wide);
        return false;
    }
    Bytes payload = {0};
    bool stale = false;
    bool ok = keeper_request(endpoint, SESSION_PURPOSE_INFO, SESSION_FRAME_INFO, &payload, &stale);
    if (ok) {
        memset(entry, 0, sizeof(*entry));
        session_parse_info(payload.data, payload.size, &entry->info);
        snprintf(entry->endpoint, sizeof(entry->endpoint), "%s", endpoint);
        ok = session_valid_id(entry->info.id);
    } else if (stale) {
        wchar_t *wide = win_wide(path);
        if (wide) DeleteFileW(wide);
        free(wide);
    }
    bytes_free(&payload);
    return ok;
}

static bool list_sessions(SessionEntry **entries, size_t *count) {
    *entries = NULL;
    *count = 0;
    char *directory = sessions_directory();
    char *pattern = directory ? win_join(directory, "*.session") : NULL;
    wchar_t *wide = pattern ? win_wide(pattern) : NULL;
    free(pattern);
    if (!wide) {
        free(directory);
        return false;
    }
    WIN32_FIND_DATAW item;
    HANDLE find = FindFirstFileW(wide, &item);
    free(wide);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char *name = win_utf8(item.cFileName);
            char *path = name ? win_join(directory, name) : NULL;
            SessionEntry entry;
            if (path && describe_session(path, &entry)) {
                SessionEntry *grown = realloc(*entries, (*count + 1) * sizeof(SessionEntry));
                if (grown) {
                    *entries = grown;
                    (*entries)[(*count)++] = entry;
                }
            }
            free(name);
            free(path);
        } while (FindNextFileW(find, &item));
        FindClose(find);
    }
    free(directory);
    if (*count > 1) qsort(*entries, *count, sizeof(SessionEntry), compare_entries);
    return true;
}

static bool find_session(const char *id, SessionEntry *entry) {
    if (!session_valid_id(id)) return false;
    char *path = metadata_path(id);
    bool ok = path && describe_session(path, entry) && !strcmp(entry->info.id, id);
    free(path);
    return ok;
}

static void append_argument(StringBuilder *line, const wchar_t *argument) {
    char *utf8 = win_utf8(argument);
    if (!utf8) return;
    if (line->length) sb_append(line, " ");
    if (utf8[0] && !strpbrk(utf8, " \t\"")) {
        sb_append(line, utf8);
    } else {
        sb_append(line, "\"");
        size_t backslashes = 0;
        for (const char *p = utf8; *p; p++) {
            if (*p == '\\') {
                backslashes++;
                continue;
            }
            for (size_t i = 0; i < (*p == '"' ? backslashes * 2 + 1 : backslashes); i++) sb_append(line, "\\");
            backslashes = 0;
            sb_append_n(line, p, 1);
        }
        for (size_t i = 0; i < backslashes * 2; i++) sb_append(line, "\\");
        sb_append(line, "\"");
    }
    free(utf8);
}

// The shells a new session can run. PowerShell 7 is used when installed.
static bool shell_command(const char *kind, wchar_t *program, size_t capacity, const wchar_t **argument) {
    wchar_t system[MAX_PATH];
    if (!GetSystemDirectoryW(system, MAX_PATH)) return false;
    *argument = NULL;
    if (!strcmp(kind, "cmd")) {
        _snwprintf(program, capacity, L"%ls\\cmd.exe", system);
        return true;
    }
    if (!strcmp(kind, "powershell")) {
        if (!SearchPathW(NULL, L"pwsh.exe", NULL, (DWORD)capacity, program, NULL)) {
            _snwprintf(program, capacity, L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", system);
        }
        *argument = L"-NoLogo";
        return true;
    }
    return false;
}

static bool start_session(const char *kind, uint16_t columns, uint16_t rows, char id[17]) {
    wchar_t program[MAX_PATH];
    const wchar_t *argument = NULL;
    wchar_t home[MAX_PATH];
    if (!shell_command(kind, program, MAX_PATH, &argument) || !random_hex(id, 8) ||
        !GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) {
        return false;
    }
    wchar_t wide_id[17], column_text[16], row_text[16];
    _snwprintf(wide_id, 17, L"%hs", id);
    _snwprintf(column_text, 16, L"%u", columns);
    _snwprintf(row_text, 16, L"%u", rows);
    const wchar_t *arguments[] = {app_executable, L"--keeper", L"--id", wide_id, L"--columns", column_text, L"--rows", row_text,
                                  L"--cwd", home, L"--", program, argument};
    StringBuilder line = {0};
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]) && arguments[i]; i++) append_argument(&line, arguments[i]);
    wchar_t *command_line = line.data ? win_wide(line.data) : NULL;
    free(line.data);
    if (!command_line) return false;
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    // The keeper leaves the app's job object, so sessions outlive the backend.
    DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT;
    BOOL started = CreateProcessW(app_executable, command_line, NULL, NULL, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, NULL, home, &startup, &process);
    if (!started && GetLastError() == ERROR_ACCESS_DENIED) {
        started = CreateProcessW(app_executable, command_line, NULL, NULL, FALSE, flags, NULL, home, &startup, &process);
    }
    free(command_line);
    if (!started) return false;
    CloseHandle(process.hThread);
    char *path = metadata_path(id);
    bool ready = false;
    for (int attempt = 0; path && attempt < 100 && !ready; attempt++) {
        char endpoint[256];
        if (read_endpoint(path, endpoint, sizeof(endpoint))) ready = true;
        else if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) break;
    }
    free(path);
    CloseHandle(process.hProcess);
    return ready;
}

static bool same_origin(const HttpRequest *r) {
    if (!r->origin) return true;
    return r->host && !strncmp(r->origin, "http://", 7) && !_stricmp(r->origin + 7, r->host);
}

static void respond(Connection *c, const char *status, const char *type, const void *body, size_t length, const char *headers) {
    char head[2048];
    int size = snprintf(head, sizeof(head),
                        "HTTP/1.1 %s\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\n"
                        "Connection: close\r\n%s%s%s%s\r\n",
                        status, length, type ? "Content-Type: " : "", type ? type : "", type ? "\r\n" : "", headers ? headers : "");
    c->responded = true;
    if (size > 0 && (size_t)size < sizeof(head) && connection_write(c, head, (size_t)size) && length) connection_write(c, body, length);
}

static void append_field(StringBuilder *out, const char *value) {
    for (const char *p = value; *p; p++) {
        if (*p == '\t') sb_append(out, "\\t");
        else if (*p == '\n') sb_append(out, "\\n");
        else if (*p == '\r') sb_append(out, "\\r");
        else if (*p == '\\') sb_append(out, "\\\\");
        else sb_append_n(out, p, 1);
    }
}

static void handle_list(Connection *c) {
    SessionEntry *entries = NULL;
    size_t count = 0;
    if (!list_sessions(&entries, &count)) {
        respond(c, "500 Internal Server Error", "text/plain; charset=utf-8", "Could not list terminal sessions.\n", 34, NULL);
        return;
    }
    StringBuilder tsv = {0};
    sb_append(&tsv, "id\tcommand\ttitle\tcwd\tcreated\tactive\tclients\n");
    for (size_t i = 0; i < count; i++) {
        const SessionInfo *info = &entries[i].info;
        char numbers[96];
        append_field(&tsv, info->id);
        sb_append(&tsv, "\t");
        append_field(&tsv, info->command);
        sb_append(&tsv, "\t");
        append_field(&tsv, info->title);
        sb_append(&tsv, "\t");
        append_field(&tsv, info->cwd);
        snprintf(numbers, sizeof(numbers), "\t%lld\t%lld\t%u\n", (long long)info->created, (long long)info->active, info->clients);
        sb_append(&tsv, numbers);
    }
    free(entries);
    wchar_t home[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    char *home_text = win_utf8(home);
    StringBuilder headers = {0};
    sb_append(&headers, "Terminal-Home: ");
    append_field(&headers, home_text ? home_text : "");
    sb_append(&headers, "\r\nTerminal-Shells: powershell\tPowerShell\tcmd\tCommand Prompt\r\n");
    free(home_text);
    respond(c, "200 OK", "text/tab-separated-values; charset=utf-8", tsv.data ? tsv.data : "", tsv.length, headers.data);
    free(tsv.data);
    free(headers.data);
}

static unsigned query_number(const HttpRequest *r, const char *name, unsigned fallback) {
    char *value = http_query(r, name);
    unsigned long number = value ? strtoul(value, NULL, 10) : 0;
    free(value);
    return number > 0 && number <= 1000 ? (unsigned)number : fallback;
}

static void handle_create(Connection *c, const HttpRequest *r) {
    char *shell = http_query(r, "shell");
    char id[17];
    bool ok = start_session(shell && *shell ? shell : "powershell", (uint16_t)query_number(r, "columns", 80),
                            (uint16_t)query_number(r, "rows", 24), id);
    free(shell);
    if (ok) respond(c, "201 Created", "text/plain; charset=utf-8", id, strlen(id), NULL);
    else respond(c, "500 Internal Server Error", "text/plain; charset=utf-8", "Could not start a terminal session.\n", 36, NULL);
}

static void handle_close(Connection *c, const char *id) {
    SessionEntry entry;
    bool stale = false;
    if (find_session(id, &entry) && keeper_request(entry.endpoint, SESSION_PURPOSE_CLOSE, SESSION_FRAME_ENDED, NULL, &stale)) {
        respond(c, "204 No Content", NULL, NULL, 0, NULL);
    } else {
        respond(c, "404 Not Found", "text/plain; charset=utf-8", "No such terminal session.\n", 26, NULL);
    }
}

typedef struct {
    Connection *page;
    HANDLE keeper;
    SRWLOCK write_lock;
    HANDLE done;
} Bridge;

static bool send_page(Bridge *bridge, uint8_t opcode, int type, const void *payload, size_t length) {
    uint8_t header[11];
    size_t header_length = session_websocket_header(header, opcode, length + (type >= 0 ? 1 : 0));
    if (type >= 0) header[header_length++] = (uint8_t)type;
    AcquireSRWLockExclusive(&bridge->write_lock);
    bool ok = connection_write(bridge->page, header, header_length) && (!length || connection_write(bridge->page, payload, length));
    ReleaseSRWLockExclusive(&bridge->write_lock);
    return ok;
}

static unsigned __stdcall pump_keeper_to_page(void *argument) {
    Bridge *bridge = argument;
    SessionFrameReader reader = {0};
    char buffer[65536];
    bool open = true;
    while (open) {
        DWORD count = 0;
        if (!pipe_transfer(bridge->keeper, false, buffer, sizeof(buffer), &count, INFINITE) || !bytes_append(&reader.pending, buffer, count)) break;
        uint8_t type;
        const char *data;
        uint32_t size;
        int next;
        while (open && (next = session_next_frame(&reader, &type, &data, &size)) == 1) {
            if (type == SESSION_FRAME_OUTPUT || (type == SESSION_FRAME_POSITION && size == 8)) {
                open = send_page(bridge, 0x2, type, data, size);
            } else if (type == SESSION_FRAME_ENDED) {
                send_page(bridge, 0x2, SESSION_FRAME_ENDED, NULL, 0);
                open = false;
            }
        }
        if (next < 0) break;
    }
    bytes_free(&reader.pending);
    uint8_t close_frame[2] = {0x03, 0xe8};
    send_page(bridge, 0x8, -1, close_frame, sizeof(close_frame));
    SetEvent(bridge->done);
    return 0;
}

// Reads page messages and forwards them to the keeper until the page closes
// the connection or the session ends.
static void pump_page_to_keeper(Bridge *bridge) {
    Connection *c = bridge->page;
    StringBuilder message = {0};
    uint8_t message_opcode = 0;
    for (;;) {
        uint8_t header[2];
        if (!connection_read(c, header, 2) || !(header[1] & 0x80)) break;
        bool final = header[0] & 0x80;
        uint8_t opcode = header[0] & 0x0f;
        uint64_t length = header[1] & 0x7f;
        if (length == 126) {
            uint8_t extended[2];
            if (!connection_read(c, extended, 2)) break;
            length = (uint64_t)extended[0] << 8 | extended[1];
        } else if (length == 127) {
            uint8_t extended[8];
            if (!connection_read(c, extended, 8)) break;
            length = 0;
            for (int i = 0; i < 8; i++) length = length << 8 | extended[i];
        }
        uint8_t mask[4];
        if (length > SESSION_MAX_FRAME || !connection_read(c, mask, 4)) break;
        char *payload = malloc(length ? (size_t)length : 1);
        if (!payload || (length && !connection_read(c, payload, (size_t)length))) {
            free(payload);
            break;
        }
        for (uint64_t i = 0; i < length; i++) payload[i] ^= (char)mask[i % 4];
        if (opcode == 0x8) {
            free(payload);
            break;
        }
        if (opcode == 0x9) {
            send_page(bridge, 0xA, -1, payload, (size_t)length);
            free(payload);
            continue;
        }
        if (opcode == 0xA) {
            free(payload);
            continue;
        }
        if (opcode != 0x0) {
            message.length = 0;
            message_opcode = opcode;
        }
        bool fits = message.length + length <= SESSION_MAX_FRAME && (!length || sb_append_n(&message, payload, (size_t)length));
        free(payload);
        if (!fits) break;
        if (!final) continue;
        const uint8_t *bytes = (const uint8_t *)message.data;
        Bytes frame = {0};
        if (message_opcode == 0x2 && message.length > 1 && bytes[0] == SESSION_FRAME_INPUT) {
            session_append_frame(&frame, SESSION_FRAME_INPUT, message.data + 1, message.length - 1);
        } else if (message_opcode == 0x2 && message.length == 5 && bytes[0] == SESSION_FRAME_RESIZE) {
            unsigned columns = (unsigned)bytes[1] | (unsigned)bytes[2] << 8;
            unsigned rows = (unsigned)bytes[3] | (unsigned)bytes[4] << 8;
            if (columns && columns <= 1000 && rows && rows <= 1000) session_append_resize(&frame, (uint16_t)columns, (uint16_t)rows);
        }
        bool ok = !frame.size || pipe_write_all(bridge->keeper, frame.data, frame.size, INFINITE);
        bytes_free(&frame);
        message.length = 0;
        if (!ok) break;
    }
    free(message.data);
}

static void handle_attach(Connection *c, const HttpRequest *r, const char *id) {
    SessionEntry entry;
    bool stale = false;
    HANDLE keeper = find_session(id, &entry) ? connect_keeper(entry.endpoint, &stale) : INVALID_HANDLE_VALUE;
    if (keeper == INVALID_HANDLE_VALUE) {
        respond(c, "404 Not Found", "text/plain; charset=utf-8", "No such terminal session.\n", 26, NULL);
        return;
    }
    char *from = http_query(r, "from");
    bool resume = from && *from;
    Bytes hello = {0};
    bool sent = session_append_hello(&hello, resume ? SESSION_PURPOSE_RESUME : SESSION_PURPOSE_ATTACH,
                                     (uint16_t)query_number(r, "columns", 80), (uint16_t)query_number(r, "rows", 24),
                                     resume ? _strtoui64(from, NULL, 10) : 0) &&
        pipe_write_all(keeper, hello.data, hello.size, 2000);
    bytes_free(&hello);
    free(from);
    if (!sent) {
        CloseHandle(keeper);
        respond(c, "502 Bad Gateway", "text/plain; charset=utf-8", "Could not reach the terminal session.\n", 38, NULL);
        return;
    }
    char accept[32];
    session_websocket_accept(r->websocket_key, accept);
    char head[256];
    int size = snprintf(head, sizeof(head),
                        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
    c->responded = true;
    c->unlimited_io_wait = true;
    HANDLE cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    Bridge bridge = {c, keeper, SRWLOCK_INIT, cancel};
    if (cancel && connection_write(c, head, (size_t)size)) {
        c->cancel = cancel;
        HANDLE pump = (HANDLE)_beginthreadex(NULL, 0, pump_keeper_to_page, &bridge, 0, NULL);
        if (pump) {
            pump_page_to_keeper(&bridge);
            CancelIoEx(keeper, NULL);
            WaitForSingleObject(pump, INFINITE);
            CloseHandle(pump);
        }
        c->cancel = NULL;
    }
    if (cancel) CloseHandle(cancel);
    CloseHandle(keeper);
}

static void serve(Connection *c, void *context) {
    (void)context;
    HttpRequest r = {0};
    if (!http_read(c, &r) || !http_authorize(c, &r)) {
        http_free(&r);
        return;
    }
    bool get = !strcmp(r.method, "GET");
    if (!strncmp(r.path, "/api/", 5) && !same_origin(&r)) {
        respond(c, "403 Forbidden", "text/plain; charset=utf-8", "Cross-origin requests are not allowed.\n", 39, NULL);
    } else if (!strcmp(r.path, "/api/sessions")) {
        if (get) handle_list(c);
        else handle_create(c, &r);
    } else if (!strncmp(r.path, "/api/sessions/", 14)) {
        char id[64];
        const char *rest = r.path + 14;
        const char *slash = strchr(rest, '/');
        size_t length = slash ? (size_t)(slash - rest) : strlen(rest);
        snprintf(id, sizeof(id), "%.*s", (int)(length < sizeof(id) ? length : sizeof(id) - 1), rest);
        if (!session_valid_id(id)) {
            respond(c, "404 Not Found", "text/plain; charset=utf-8", "No such terminal session.\n", 26, NULL);
        } else if (slash && !strcmp(slash, "/close") && !strcmp(r.method, "POST")) {
            handle_close(c, id);
        } else if (slash && !strcmp(slash, "/attach") && get && r.upgrade && !_stricmp(r.upgrade, "websocket") && r.websocket_key) {
            handle_attach(c, &r, id);
        } else {
            respond(c, "404 Not Found", "text/plain; charset=utf-8", "Not found.\n", 11, NULL);
        }
    } else if (!strncmp(r.path, "/api/", 5)) {
        respond(c, "404 Not Found", "text/plain; charset=utf-8", "Not found.\n", 11, NULL);
    } else {
        http_static(c, &r, app_web);
    }
    http_free(&r);
}

// The keeper.

typedef HRESULT(WINAPI *CreatePseudoConsoleFunction)(COORD, HANDLE, HANDLE, DWORD, void **);
typedef HRESULT(WINAPI *ResizePseudoConsoleFunction)(void *, COORD);
typedef void(WINAPI *ClosePseudoConsoleFunction)(void *);

typedef struct {
    HANDLE pipe;
    bool attached;
    bool dead;
} KeeperClient;

static struct {
    CRITICAL_SECTION lock;
    CRITICAL_SECTION input_lock;
    SessionTerminal *terminal;
    KeeperClient *clients[SESSION_MAX_CLIENTS];
    size_t client_count;
    void *console;
    HANDLE input;
    HANDLE output;
    HANDLE process;
    uint16_t columns;
    uint16_t rows;
    int64_t created;
    int64_t active;
    bool ended;
    char id[40];
    char command[256];
    char cwd[1024];
    wchar_t *pipe_name;
    SECURITY_ATTRIBUTES attributes;
    HANDLE next_pipe;
    ResizePseudoConsoleFunction resize;
} keeper;

static void send_client(KeeperClient *client, const Bytes *frames) {
    if (client->dead || !frames->size) return;
    if (!pipe_write_all(client->pipe, frames->data, frames->size, 3000)) {
        client->dead = true;
        CancelIoEx(client->pipe, NULL);
    }
}

static void resize_console(uint16_t columns, uint16_t rows) {
    COORD size = {(SHORT)columns, (SHORT)rows};
    keeper.resize(keeper.console, size);
}

static unsigned __stdcall keeper_output_thread(void *argument) {
    (void)argument;
    char buffer[65536];
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(keeper.output, buffer, sizeof(buffer), &count, NULL) || count == 0) return 0;
        Bytes frame = {0};
        session_append_frame(&frame, SESSION_FRAME_OUTPUT, buffer, count);
        EnterCriticalSection(&keeper.lock);
        session_terminal_record(keeper.terminal, buffer, count);
        keeper.active = (int64_t)time(NULL);
        for (size_t i = 0; i < keeper.client_count; i++) {
            if (keeper.clients[i]->attached) send_client(keeper.clients[i], &frame);
        }
        LeaveCriticalSection(&keeper.lock);
        bytes_free(&frame);
    }
}

static unsigned __stdcall keeper_client_thread(void *argument) {
    KeeperClient *client = argument;
    SessionFrameReader reader = {0};
    char buffer[65536];
    bool open = true;
    while (open && !client->dead) {
        DWORD count = 0;
        if (!pipe_transfer(client->pipe, false, buffer, sizeof(buffer), &count, INFINITE) || !bytes_append(&reader.pending, buffer, count)) break;
        uint8_t type;
        const char *data;
        uint32_t size;
        int next;
        while (open && (next = session_next_frame(&reader, &type, &data, &size)) == 1) {
            uint8_t purpose = 0;
            SessionAttachRequest request;
            uint16_t columns, rows;
            if (type == SESSION_FRAME_HELLO && session_parse_hello(data, size, &purpose, &request)) {
                if (purpose == SESSION_PURPOSE_ATTACH || purpose == SESSION_PURPOSE_RESUME) {
                    Bytes frames = {0};
                    bool resumed = false;
                    EnterCriticalSection(&keeper.lock);
                    if (!keeper.ended) {
                        resumed = session_terminal_attach(keeper.terminal, &request, &frames);
                        send_client(client, &frames);
                        client->attached = true;
                    }
                    bool same_size = request.columns == keeper.columns && request.rows == keeper.rows;
                    keeper.columns = request.columns;
                    keeper.rows = request.rows;
                    LeaveCriticalSection(&keeper.lock);
                    bytes_free(&frames);
                    if (!resumed && same_size && request.columns > 2) {
                        resize_console((uint16_t)(request.columns - 1), request.rows);
                        Sleep(150);
                    }
                    if (!resumed || !same_size) resize_console(request.columns, request.rows);
                } else if (purpose == SESSION_PURPOSE_INFO) {
                    SessionInfo info = {0};
                    Bytes frame = {0};
                    EnterCriticalSection(&keeper.lock);
                    snprintf(info.id, sizeof(info.id), "%s", keeper.id);
                    snprintf(info.title, sizeof(info.title), "%s", session_terminal_title(keeper.terminal));
                    snprintf(info.command, sizeof(info.command), "%s", keeper.command);
                    snprintf(info.cwd, sizeof(info.cwd), "%s", keeper.cwd);
                    info.created = keeper.created;
                    info.active = keeper.active;
                    for (size_t i = 0; i < keeper.client_count; i++) info.clients += keeper.clients[i]->attached && !keeper.clients[i]->dead;
                    LeaveCriticalSection(&keeper.lock);
                    session_append_info(&frame, &info);
                    send_client(client, &frame);
                    bytes_free(&frame);
                    open = false;
                } else if (purpose == SESSION_PURPOSE_CLOSE) {
                    EnterCriticalSection(&keeper.lock);
                    client->attached = true;
                    LeaveCriticalSection(&keeper.lock);
                    TerminateProcess(keeper.process, 0);
                }
            } else if (type == SESSION_FRAME_INPUT && client->attached) {
                EnterCriticalSection(&keeper.input_lock);
                DWORD written = 0;
                for (uint32_t offset = 0; offset < size; offset += written) {
                    if (!WriteFile(keeper.input, data + offset, size - offset, &written, NULL) || !written) break;
                }
                LeaveCriticalSection(&keeper.input_lock);
                EnterCriticalSection(&keeper.lock);
                keeper.active = (int64_t)time(NULL);
                LeaveCriticalSection(&keeper.lock);
            } else if (type == SESSION_FRAME_RESIZE && client->attached && session_parse_size(data, size, &columns, &rows)) {
                EnterCriticalSection(&keeper.lock);
                keeper.columns = columns;
                keeper.rows = rows;
                LeaveCriticalSection(&keeper.lock);
                resize_console(columns, rows);
            }
        }
        if (next < 0) break;
    }
    EnterCriticalSection(&keeper.lock);
    for (size_t i = 0; i < keeper.client_count; i++) {
        if (keeper.clients[i] == client) {
            keeper.clients[i] = keeper.clients[--keeper.client_count];
            break;
        }
    }
    LeaveCriticalSection(&keeper.lock);
    FlushFileBuffers(client->pipe);
    DisconnectNamedPipe(client->pipe);
    CloseHandle(client->pipe);
    free(client);
    bytes_free(&reader.pending);
    return 0;
}

static HANDLE create_keeper_pipe(bool first) {
    return CreateNamedPipeW(keeper.pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                            65536, 65536, 0, &keeper.attributes);
}

static unsigned __stdcall keeper_accept_thread(void *argument) {
    (void)argument;
    for (;;) {
        HANDLE pipe = keeper.next_pipe;
        keeper.next_pipe = INVALID_HANDLE_VALUE;
        if (pipe == INVALID_HANDLE_VALUE) pipe = create_keeper_pipe(false);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(250);
            continue;
        }
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        DWORD ignored = 0;
        bool connected = overlapped.hEvent &&
            (ConnectNamedPipe(pipe, &overlapped) || GetLastError() == ERROR_PIPE_CONNECTED ||
             (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult(pipe, &overlapped, &ignored, TRUE)));
        if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
        KeeperClient *client = connected ? calloc(1, sizeof(KeeperClient)) : NULL;
        bool added = false;
        if (client) {
            client->pipe = pipe;
            EnterCriticalSection(&keeper.lock);
            if (keeper.client_count < SESSION_MAX_CLIENTS && !keeper.ended) {
                keeper.clients[keeper.client_count++] = client;
                added = true;
            }
            LeaveCriticalSection(&keeper.lock);
        }
        HANDLE thread = added ? (HANDLE)_beginthreadex(NULL, 0, keeper_client_thread, client, 0, NULL) : NULL;
        if (thread) {
            CloseHandle(thread);
        } else {
            if (added) {
                EnterCriticalSection(&keeper.lock);
                keeper.client_count--;
                LeaveCriticalSection(&keeper.lock);
            }
            free(client);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }
}

static int keeper_main(int argc, wchar_t **argv) {
    const wchar_t *cwd = L"";
    unsigned columns = 80, rows = 24;
    int command_start = 0;
    for (int i = 2; i < argc; i++) {
        if (!wcscmp(argv[i], L"--")) {
            command_start = i + 1;
            break;
        }
        if (i + 1 >= argc) return 2;
        if (!wcscmp(argv[i], L"--id")) {
            char *id = win_utf8(argv[++i]);
            snprintf(keeper.id, sizeof(keeper.id), "%s", id ? id : "");
            free(id);
        } else if (!wcscmp(argv[i], L"--cwd")) cwd = argv[++i];
        else if (!wcscmp(argv[i], L"--columns")) columns = wcstoul(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--rows")) rows = wcstoul(argv[++i], NULL, 10);
        else return 2;
    }
    if (!session_valid_id(keeper.id) || !command_start || command_start >= argc || !columns || columns > 1000 || !rows || rows > 1000) return 2;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    CreatePseudoConsoleFunction create = (CreatePseudoConsoleFunction)(void *)GetProcAddress(kernel, "CreatePseudoConsole");
    keeper.resize = (ResizePseudoConsoleFunction)(void *)GetProcAddress(kernel, "ResizePseudoConsole");
    ClosePseudoConsoleFunction close_console = (ClosePseudoConsoleFunction)(void *)GetProcAddress(kernel, "ClosePseudoConsole");
    if (!create || !keeper.resize || !close_console) return 1;
    InitializeCriticalSection(&keeper.lock);
    InitializeCriticalSection(&keeper.input_lock);
    keeper.terminal = session_terminal_create();
    keeper.columns = (uint16_t)columns;
    keeper.rows = (uint16_t)rows;
    keeper.created = keeper.active = (int64_t)time(NULL);
    keeper.next_pipe = INVALID_HANDLE_VALUE;
    char *cwd_text = win_utf8(cwd);
    snprintf(keeper.cwd, sizeof(keeper.cwd), "%s", cwd_text ? cwd_text : "");
    free(cwd_text);
    const wchar_t *program = wcsrchr(argv[command_start], L'\\');
    char *program_name = win_utf8(program ? program + 1 : argv[command_start]);
    snprintf(keeper.command, sizeof(keeper.command), "%s", program_name ? program_name : "");
    free(program_name);
    char *extension = strrchr(keeper.command, '.');
    if (extension && !_stricmp(extension, ".exe")) *extension = 0;

    char *sid = win_sid();
    char suffix[33];
    char *metadata = metadata_path(keeper.id);
    PSECURITY_DESCRIPTOR descriptor = NULL;
    char sddl[256];
    if (!keeper.terminal || !sid || !metadata || !random_hex(suffix, 16)) return 1;
    snprintf(sddl, sizeof(sddl), "D:P(A;;GA;;;%s)", sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &descriptor, NULL)) return 1;
    char endpoint[160];
    snprintf(endpoint, sizeof(endpoint), "\\\\.\\pipe\\org.outershell.terminal.%s.%s", keeper.id, suffix);
    keeper.pipe_name = win_wide(endpoint);
    keeper.attributes.nLength = sizeof(keeper.attributes);
    keeper.attributes.lpSecurityDescriptor = descriptor;
    keeper.next_pipe = keeper.pipe_name ? create_keeper_pipe(true) : INVALID_HANDLE_VALUE;
    if (keeper.next_pipe == INVALID_HANDLE_VALUE) return 1;

    HANDLE input_read = NULL, output_write = NULL;
    if (!CreatePipe(&input_read, &keeper.input, NULL, 0) || !CreatePipe(&keeper.output, &output_write, NULL, 0)) return 1;
    COORD size = {(SHORT)columns, (SHORT)rows};
    if (create(size, input_read, output_write, 0, &keeper.console) != S_OK) return 1;
    CloseHandle(input_read);
    CloseHandle(output_write);

    StringBuilder line = {0};
    for (int i = command_start; i < argc; i++) append_argument(&line, argv[i]);
    wchar_t *command_line = line.data ? win_wide(line.data) : NULL;
    free(line.data);
    wchar_t *session_id = win_wide(keeper.id);
    SetEnvironmentVariableW(L"OUTERSHELL_TERMINAL_SESSION", session_id);
    free(session_id);
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList = malloc(attribute_size);
    PROCESS_INFORMATION process = {0};
    bool started = command_line && startup.lpAttributeList &&
        InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size) &&
        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, keeper.console, sizeof(keeper.console), NULL, NULL) &&
        CreateProcessW(NULL, command_line, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, NULL,
                       *cwd ? cwd : NULL, &startup.StartupInfo, &process);
    free(command_line);
    if (!started) {
        close_console(keeper.console);
        return 1;
    }
    CloseHandle(process.hThread);
    keeper.process = process.hProcess;

    HANDLE output_thread = (HANDLE)_beginthreadex(NULL, 0, keeper_output_thread, NULL, 0, NULL);
    HANDLE accept_thread = (HANDLE)_beginthreadex(NULL, 0, keeper_accept_thread, NULL, 0, NULL);
    bool recorded = output_thread && accept_thread && win_atomic_write(metadata, endpoint, strlen(endpoint));
    if (!recorded) TerminateProcess(keeper.process, 1);

    WaitForSingleObject(keeper.process, INFINITE);
    wchar_t *metadata_wide = win_wide(metadata);
    if (metadata_wide) DeleteFileW(metadata_wide);
    free(metadata_wide);
    EnterCriticalSection(&keeper.lock);
    keeper.ended = true;
    LeaveCriticalSection(&keeper.lock);
    close_console(keeper.console);
    if (output_thread) WaitForSingleObject(output_thread, 5000);
    Bytes ended = {0};
    session_append_frame(&ended, SESSION_FRAME_ENDED, NULL, 0);
    EnterCriticalSection(&keeper.lock);
    for (size_t i = 0; i < keeper.client_count; i++) {
        send_client(keeper.clients[i], &ended);
        FlushFileBuffers(keeper.clients[i]->pipe);
    }
    LeaveCriticalSection(&keeper.lock);
    bytes_free(&ended);
    ExitProcess(0);
}

int wmain(int argc, wchar_t **argv) {
    DWORD length = GetModuleFileNameW(NULL, app_executable, (DWORD)(sizeof(app_executable) / sizeof(app_executable[0])));
    if (!length || length >= sizeof(app_executable) / sizeof(app_executable[0])) return 1;
    if (argc >= 2 && !wcscmp(argv[1], L"--keeper")) return keeper_main(argc, argv);
    if (argc != 1) return 2;
    app_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!app_stop) return 1;
    SetConsoleCtrlHandler(stop_app, TRUE);
    char *directory = win_executable_directory();
    app_web = directory ? win_join(directory, "web") : NULL;
    free(directory);
    char *name = win_pipe("org.outershell.Terminal");
    PipeServer *server = app_web && name ? pipe_server_start(name, serve, NULL) : NULL;
    free(name);
    if (!server) return 1;
    pipe_servers_wait_idle(app_stop, 300000);
    pipe_server_stop(server);
    CloseHandle(app_stop);
    free(app_web);
    return 0;
}
