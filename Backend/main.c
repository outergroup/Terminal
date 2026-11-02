// TerminalBackend serves the Terminal web app and connects its terminals to
// persistent terminal sessions.
//
// Each session runs in a keeper process (this program, started with --keeper;
// see Session.h and Keeper.c) that outlives the backend. A browser terminal
// attaches over a WebSocket, which this backend bridges to the session's
// keeper. Because the page keeps its screen, it can resume from the last
// output position it showed, and the keeper sends only the output the page
// missed.
//
// HTTP API:
//   GET    /api/sessions                    sessions as TSV: a header row, then
//                                           id, command, title, cwd, created,
//                                           active, clients, place; fields
//                                           escape tab, newline, carriage return
//                                           and backslash as \t \n \r \\. The
//                                           Terminal-Home header gives $HOME,
//                                           and place is the container's name
//                                           for a session in a container.
//   GET    /api/places                      running containers registered with
//                                           Outer Shell, as TSV: id (the
//                                           container's ID in Outer Shell), name
//   POST   /api/sessions?columns=C&rows=R[&place=ID[&command=COMMAND]]
//                                           start a session, on this computer
//                                           or in a container, running the
//                                           container's login shell or a command
//                                           registered in its image (see
//                                           Places.h); the body is its ID
//   POST   /api/sessions/ID/close           close a session
//   GET    /api/sessions/ID/attach?columns=C&rows=R[&from=POSITION]
//                                           WebSocket attached to the session
//
// WebSocket messages are binary and start with a type byte, like the keeper
// protocol. From the page: 'I' followed by keyboard input, and 'R' followed by
// little-endian UInt16 columns and rows. To the page: 'O' followed by terminal
// output, 'P' followed by the little-endian UInt64 output position after the
// output sent so far, and 'X' when the session's program exits.

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "Keeper.h"
#include "Places.h"
#include "Session.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
extern int launch_activate_socket(const char *name, int **fds, size_t *cnt);
#endif

#define MAX_REQUEST_HEAD (64 * 1024)
#define MAX_WEBSOCKET_MESSAGE (1024 * 1024)

static char g_web_root[PATH_MAX] = "";
static char g_executable[PATH_MAX] = "";
static char g_listen_socket_path[PATH_MAX] = "";
static char g_token[128] = "";
static char g_shell[PATH_MAX] = "/bin/sh";
static char g_home[PATH_MAX] = "/";
static bool g_listen_socket_is_owned = false;
static volatile sig_atomic_t g_shutdown_requested = 0;
static int g_listen_fd = -1;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} Buffer;

static bool buffer_reserve(Buffer *buffer, size_t capacity) {
    if (capacity + 1 <= buffer->capacity) return true;
    size_t next = buffer->capacity ? buffer->capacity : 256;
    while (next < capacity + 1) next *= 2;
    char *data = realloc(buffer->data, next);
    if (!data) return false;
    buffer->data = data;
    buffer->capacity = next;
    return true;
}

static bool buffer_append(Buffer *buffer, const void *data, size_t size) {
    if (!buffer_reserve(buffer, buffer->size + size)) return false;
    if (size) memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return true;
}

static bool buffer_append_string(Buffer *buffer, const char *value) {
    return buffer_append(buffer, value, strlen(value));
}

static bool buffer_appendf(Buffer *buffer, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    char small[512];
    int length = vsnprintf(small, sizeof(small), format, arguments);
    va_end(arguments);
    if (length < 0) return false;
    if ((size_t)length < sizeof(small)) return buffer_append(buffer, small, (size_t)length);
    if (!buffer_reserve(buffer, buffer->size + (size_t)length)) return false;
    va_start(arguments, format);
    vsnprintf(buffer->data + buffer->size, (size_t)length + 1, format, arguments);
    va_end(arguments);
    buffer->size += (size_t)length;
    return true;
}

static void buffer_free(Buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = buffer->capacity = 0;
}

static bool write_all(int fd, const void *data, size_t size) {
    const char *bytes = data;
    while (size > 0) {
        ssize_t written = send(fd, bytes, size, 0);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes += written;
        size -= (size_t)written;
    }
    return true;
}

static bool read_exact_file(int fd, void *data, size_t size) {
    char *bytes = data;
    while (size > 0) {
        ssize_t count = read(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= (size_t)count;
    }
    return true;
}

static bool read_exact(int fd, void *data, size_t size) {
    char *bytes = data;
    while (size > 0) {
        ssize_t count = recv(fd, bytes, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= (size_t)count;
    }
    return true;
}

typedef struct {
    char method[16];
    char target[2048];
    char path[2048];
    char query[2048];
    char host[256];
    char origin[512];
    char upgrade[64];
    char websocket_key[128];
    char cookie[1024];
    size_t content_length;
} Request;

static void copy_header_value(char *out, size_t capacity, const char *value, size_t length) {
    while (length > 0 && (*value == ' ' || *value == '\t')) { value++; length--; }
    while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t')) length--;
    if (length >= capacity) length = capacity - 1;
    memcpy(out, value, length);
    out[length] = '\0';
}

static bool parse_request(const char *head, Request *request) {
    memset(request, 0, sizeof(*request));
    const char *line_end = strstr(head, "\r\n");
    if (!line_end) return false;
    const char *space = memchr(head, ' ', (size_t)(line_end - head));
    if (!space || (size_t)(space - head) >= sizeof(request->method)) return false;
    memcpy(request->method, head, (size_t)(space - head));
    const char *target = space + 1;
    const char *target_end = memchr(target, ' ', (size_t)(line_end - target));
    if (!target_end || (size_t)(target_end - target) >= sizeof(request->target)) return false;
    memcpy(request->target, target, (size_t)(target_end - target));
    const char *question = strchr(request->target, '?');
    size_t path_length = question ? (size_t)(question - request->target) : strlen(request->target);
    memcpy(request->path, request->target, path_length);
    if (question) snprintf(request->query, sizeof(request->query), "%s", question + 1);
    for (const char *line = line_end + 2; *line && strncmp(line, "\r\n", 2) != 0;) {
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon) {
            size_t name_length = (size_t)(colon - line);
            const char *value = colon + 1;
            size_t value_length = (size_t)(end - value);
            if (name_length == 4 && strncasecmp(line, "Host", 4) == 0) copy_header_value(request->host, sizeof(request->host), value, value_length);
            else if (name_length == 6 && strncasecmp(line, "Origin", 6) == 0) copy_header_value(request->origin, sizeof(request->origin), value, value_length);
            else if (name_length == 7 && strncasecmp(line, "Upgrade", 7) == 0) copy_header_value(request->upgrade, sizeof(request->upgrade), value, value_length);
            else if (name_length == 17 && strncasecmp(line, "Sec-WebSocket-Key", 17) == 0) copy_header_value(request->websocket_key, sizeof(request->websocket_key), value, value_length);
            else if (name_length == 6 && strncasecmp(line, "Cookie", 6) == 0) copy_header_value(request->cookie, sizeof(request->cookie), value, value_length);
            else if (name_length == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
                char number[32];
                copy_header_value(number, sizeof(number), value, value_length);
                request->content_length = (size_t)strtoull(number, NULL, 10);
            }
        }
        line = end + 2;
    }
    return true;
}

static bool query_value(const char *query, const char *name, char *out, size_t capacity) {
    size_t name_length = strlen(name);
    for (const char *p = query; p && *p;) {
        const char *end = strchr(p, '&');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length > name_length && strncmp(p, name, name_length) == 0 && p[name_length] == '=') {
            const char *value = p + name_length + 1, *value_end = p + length;
            size_t used = 0;
            while (value < value_end && used + 1 < capacity) {
                unsigned byte;
                if (*value == '%' && value_end - value >= 3 && isxdigit((unsigned char)value[1]) &&
                    isxdigit((unsigned char)value[2]) && sscanf(value + 1, "%2x", &byte) == 1) {
                    out[used++] = (char)byte;
                    value += 3;
                } else {
                    out[used++] = *value == '+' ? ' ' : *value;
                    value++;
                }
            }
            out[used] = '\0';
            return true;
        }
        p = end ? end + 1 : NULL;
    }
    return false;
}

static unsigned query_number(const char *query, const char *name, unsigned fallback, unsigned maximum) {
    char value[32];
    if (!query_value(query, name, value, sizeof(value))) return fallback;
    char *end = NULL;
    unsigned long number = strtoul(value, &end, 10);
    if (!end || *end || number == 0 || number > maximum) return fallback;
    return (unsigned)number;
}

// A browser page may only call the API and open terminals from the same
// origin, so other sites cannot drive a terminal through the user's browser.
static bool same_origin(const Request *request) {
    if (!request->origin[0]) return true;
    const char *authority = strstr(request->origin, "://");
    authority = authority ? authority + 3 : request->origin;
    return request->host[0] && strcasecmp(authority, request->host) == 0;
}

// The development port is reachable by any local user, so it requires the
// token printed at startup, which the first request trades for a cookie.
static bool authorized(const Request *request) {
    if (!g_token[0]) return true;
    char expected[160];
    snprintf(expected, sizeof(expected), "terminal_token=%s", g_token);
    size_t length = strlen(expected);
    for (const char *p = request->cookie; (p = strstr(p, expected)) != NULL; p += length) {
        if ((p == request->cookie || p[-1] == ' ' || p[-1] == ';') && (p[length] == '\0' || p[length] == ';')) return true;
    }
    return false;
}

static void send_response(int fd, const char *status, const char *content_type, const char *body, size_t length,
                          const char *extra_headers) {
    Buffer head = {0};
    buffer_appendf(&head, "HTTP/1.1 %s\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\n"
                          "X-Content-Type-Options: nosniff\r\nConnection: close\r\n",
                   status, length);
    if (content_type) buffer_appendf(&head, "Content-Type: %s\r\n", content_type);
    if (extra_headers) buffer_append_string(&head, extra_headers);
    buffer_append_string(&head, "\r\n");
    write_all(fd, head.data, head.size);
    if (length) write_all(fd, body, length);
    buffer_free(&head);
}

static void send_text(int fd, const char *status, const char *message) {
    send_response(fd, status, "text/plain; charset=utf-8", message, strlen(message), NULL);
}

static void send_web_file(int fd, const char *name, const char *content_type) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", g_web_root, name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        send_text(fd, "404 Not Found", "Not found.\n");
        return;
    }
    Buffer body = {0};
    char chunk[65536];
    size_t count;
    while ((count = fread(chunk, 1, sizeof(chunk), file)) > 0) buffer_append(&body, chunk, count);
    fclose(file);
    send_response(fd, "200 OK", content_type, body.data, body.size,
                  "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'\r\n");
    buffer_free(&body);
}

static void append_tsv_field(Buffer *tsv, const char *value) {
    for (const char *p = value; *p; p++) {
        if (*p == '\t') buffer_append_string(tsv, "\\t");
        else if (*p == '\n') buffer_append_string(tsv, "\\n");
        else if (*p == '\r') buffer_append_string(tsv, "\\r");
        else if (*p == '\\') buffer_append_string(tsv, "\\\\");
        else buffer_append(tsv, p, 1);
    }
}

static void handle_list(int fd) {
    SessionEntry *entries = NULL;
    size_t count = 0;
    if (keeper_list(&entries, &count)) {
        Buffer tsv = {0};
        buffer_append_string(&tsv, "id\tcommand\ttitle\tcwd\tcreated\tactive\tclients\tplace\n");
        for (size_t i = 0; i < count; i++) {
            const SessionInfo *info = &entries[i].info;
            append_tsv_field(&tsv, info->id);
            buffer_append_string(&tsv, "\t");
            append_tsv_field(&tsv, info->command);
            buffer_append_string(&tsv, "\t");
            append_tsv_field(&tsv, info->title);
            buffer_append_string(&tsv, "\t");
            append_tsv_field(&tsv, info->cwd);
            buffer_appendf(&tsv, "\t%lld\t%lld\t%u\t", (long long)info->created, (long long)info->active, info->clients);
            append_tsv_field(&tsv, info->place);
            buffer_append_string(&tsv, "\n");
        }
        Buffer headers = {0};
        buffer_append_string(&headers, "Terminal-Home: ");
        append_tsv_field(&headers, g_home);
        buffer_append_string(&headers, "\r\n");
        send_response(fd, "200 OK", "text/tab-separated-values; charset=utf-8", tsv.data, tsv.size, headers.data);
        buffer_free(&headers);
        buffer_free(&tsv);
    } else {
        send_text(fd, "500 Internal Server Error", "Could not list terminal sessions.\n");
    }
    free(entries);
}

static void handle_places(int fd) {
    Place *places = NULL;
    size_t count = 0;
    places_list(&places, &count);
    Buffer tsv = {0};
    buffer_append_string(&tsv, "id\tname\n");
    for (size_t i = 0; i < count; i++) {
        append_tsv_field(&tsv, places[i].id);
        buffer_append_string(&tsv, "\t");
        append_tsv_field(&tsv, places[i].name);
        buffer_append_string(&tsv, "\n");
    }
    send_response(fd, "200 OK", "text/tab-separated-values; charset=utf-8", tsv.data, tsv.size, NULL);
    buffer_free(&tsv);
    free(places);
}

static void handle_create(int fd, const Request *request) {
    unsigned columns = query_number(request->query, "columns", 80, 1000);
    unsigned rows = query_number(request->query, "rows", 24, 1000);
    char place_id[320] = "", command_id[256] = "";
    query_value(request->query, "place", place_id, sizeof(place_id));
    query_value(request->query, "command", command_id, sizeof(command_id));
    const char *local[] = {g_shell, "-l", NULL};
    const char *container_shell[] = {"/bin/sh", "-c", "if command -v bash >/dev/null 2>&1; then exec bash -l; fi; exec sh -l", NULL};
    const char *arguments[96];
    const char *const *command = local;
    const char *place_name = "", *command_name = "";
    Place place;
    RegistryCommand registered;
    if (place_id[0]) {
        if (!places_find(place_id, &place)) {
            send_text(fd, "404 Not Found", "That container is not running.\n");
            return;
        }
        if (command_id[0] && !place_registered_command(&place, command_id, &registered)) {
            send_text(fd, "404 Not Found", "That container has no such command.\n");
            return;
        }
        bool has_command = command_id[0] != '\0';
        if (!place_exec_command(&place, has_command ? registered.user : "", has_command ? registered.directory : "",
                                has_command ? (const char *const *)registered.arguments : container_shell, arguments, 96)) {
            send_text(fd, "400 Bad Request", "That command is too long.\n");
            return;
        }
        command = arguments;
        place_name = place.name;
        command_name = has_command ? (registered.name[0] ? registered.name : command_id) : "";
    }
    char id[17];
    if (keeper_start(g_executable, command, g_home, place_name, command_name, (uint16_t)columns, (uint16_t)rows, id)) {
        send_response(fd, "201 Created", "text/plain; charset=utf-8", id, strlen(id), NULL);
    } else {
        send_text(fd, "500 Internal Server Error", "Could not start a terminal session.\n");
    }
}

static void handle_close(int fd, const char *id) {
    SessionEntry entry;
    if (keeper_find(id, &entry) && keeper_close(entry.endpoint)) send_response(fd, "204 No Content", NULL, NULL, 0, NULL);
    else send_text(fd, "404 Not Found", "No such terminal session.\n");
}

typedef struct {
    int client;
    int keeper;
    pthread_mutex_t write_lock;
} Bridge;

static bool send_websocket(Bridge *bridge, uint8_t opcode, uint8_t type, const void *payload, size_t length) {
    uint8_t header[11];
    size_t header_length = 2;
    header[0] = (uint8_t)(0x80 | opcode);
    if (opcode == 0x2) length += 1;
    if (length < 126) {
        header[1] = (uint8_t)length;
    } else if (length < 65536) {
        header[1] = 126;
        header[2] = (uint8_t)(length >> 8);
        header[3] = (uint8_t)length;
        header_length = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) header[2 + i] = (uint8_t)((uint64_t)length >> (56 - 8 * i));
        header_length = 10;
    }
    if (opcode == 0x2) {
        header[header_length++] = type;
        length -= 1;
    }
    pthread_mutex_lock(&bridge->write_lock);
    bool ok = write_all(bridge->client, header, header_length) && (length == 0 || write_all(bridge->client, payload, length));
    pthread_mutex_unlock(&bridge->write_lock);
    return ok;
}

// Forwards keeper output to the page until the session ends or either side
// goes away.
static void *pump_keeper_to_page(void *argument) {
    Bridge *bridge = argument;
    uint8_t header[5];
    char *payload = malloc(SESSION_MAX_FRAME);
    while (payload && read_exact(bridge->keeper, header, sizeof(header))) {
        uint32_t length = (uint32_t)header[1] | (uint32_t)header[2] << 8 | (uint32_t)header[3] << 16 | (uint32_t)header[4] << 24;
        if (length > SESSION_MAX_FRAME || !read_exact(bridge->keeper, payload, length)) break;
        bool ok = true;
        if (header[0] == SESSION_FRAME_OUTPUT || (header[0] == SESSION_FRAME_POSITION && length == 8)) {
            ok = send_websocket(bridge, 0x2, header[0], payload, length);
        } else if (header[0] == SESSION_FRAME_ENDED) {
            send_websocket(bridge, 0x2, SESSION_FRAME_ENDED, NULL, 0);
            break;
        }
        if (!ok) break;
    }
    free(payload);
    uint8_t close_frame[4] = {0x88, 0x02, 0x03, 0xe8};
    pthread_mutex_lock(&bridge->write_lock);
    write_all(bridge->client, close_frame, sizeof(close_frame));
    pthread_mutex_unlock(&bridge->write_lock);
    shutdown(bridge->client, SHUT_RDWR);
    return NULL;
}

// Reads page messages and forwards them to the keeper. Returns when the page
// closes the connection or the bridge fails.
static void pump_page_to_keeper(Bridge *bridge) {
    Buffer message = {0};
    uint8_t message_opcode = 0;
    for (;;) {
        uint8_t header[2];
        if (!read_exact(bridge->client, header, 2)) break;
        bool final = header[0] & 0x80;
        uint8_t opcode = header[0] & 0x0f;
        uint64_t length = header[1] & 0x7f;
        if (!(header[1] & 0x80)) break;
        if (length == 126) {
            uint8_t extended[2];
            if (!read_exact(bridge->client, extended, 2)) break;
            length = (uint64_t)extended[0] << 8 | extended[1];
        } else if (length == 127) {
            uint8_t extended[8];
            if (!read_exact(bridge->client, extended, 8)) break;
            length = 0;
            for (int i = 0; i < 8; i++) length = length << 8 | extended[i];
        }
        uint8_t mask[4];
        if (length > MAX_WEBSOCKET_MESSAGE || !read_exact(bridge->client, mask, 4)) break;
        char *payload = malloc(length ? (size_t)length : 1);
        if (!payload || !read_exact(bridge->client, payload, (size_t)length)) {
            free(payload);
            break;
        }
        for (uint64_t i = 0; i < length; i++) payload[i] ^= (char)mask[i % 4];
        if (opcode == 0x8) {
            free(payload);
            break;
        }
        if (opcode == 0x9) {
            send_websocket(bridge, 0xA, 0, payload, (size_t)length);
            free(payload);
            continue;
        }
        if (opcode == 0xA) {
            free(payload);
            continue;
        }
        if (opcode != 0x0) {
            message.size = 0;
            message_opcode = opcode;
        }
        bool fits = message.size + length <= MAX_WEBSOCKET_MESSAGE && buffer_append(&message, payload, (size_t)length);
        free(payload);
        if (!fits) break;
        if (!final) continue;
        bool ok = true;
        const uint8_t *bytes = (const uint8_t *)message.data;
        Bytes frame = {0};
        if (message_opcode == 0x2 && message.size > 1 && bytes[0] == SESSION_FRAME_INPUT) {
            session_append_frame(&frame, SESSION_FRAME_INPUT, message.data + 1, message.size - 1);
        } else if (message_opcode == 0x2 && message.size == 5 && bytes[0] == SESSION_FRAME_RESIZE) {
            unsigned columns = (unsigned)bytes[1] | (unsigned)bytes[2] << 8;
            unsigned rows = (unsigned)bytes[3] | (unsigned)bytes[4] << 8;
            if (columns > 0 && columns <= 1000 && rows > 0 && rows <= 1000) session_append_resize(&frame, (uint16_t)columns, (uint16_t)rows);
        }
        if (frame.size) ok = write_all(bridge->keeper, frame.data, frame.size);
        bytes_free(&frame);
        message.size = 0;
        if (!ok) break;
    }
    buffer_free(&message);
}

static void handle_attach(int fd, const Request *request, const char *id) {
    SessionEntry session;
    if (!keeper_find(id, &session)) {
        send_text(fd, "404 Not Found", "No such terminal session.\n");
        return;
    }
    int keeper = keeper_connect(session.endpoint);
    if (keeper < 0) {
        send_text(fd, "502 Bad Gateway", "Could not reach the terminal session.\n");
        return;
    }
    unsigned columns = query_number(request->query, "columns", 80, 1000);
    unsigned rows = query_number(request->query, "rows", 24, 1000);
    char from_text[32];
    bool resume = query_value(request->query, "from", from_text, sizeof(from_text)) && from_text[0];
    uint64_t from = resume ? strtoull(from_text, NULL, 10) : 0;
    Bytes hello = {0};
    bool sent = session_append_hello(&hello, resume ? SESSION_PURPOSE_RESUME : SESSION_PURPOSE_ATTACH, (uint16_t)columns, (uint16_t)rows, from) &&
        write_all(keeper, hello.data, hello.size);
    bytes_free(&hello);
    if (!sent) {
        close(keeper);
        send_text(fd, "502 Bad Gateway", "Could not reach the terminal session.\n");
        return;
    }
    char accept[32];
    session_websocket_accept(request->websocket_key, accept);
    Buffer head = {0};
    buffer_appendf(&head, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    bool ok = write_all(fd, head.data, head.size);
    buffer_free(&head);
    if (!ok) {
        close(keeper);
        return;
    }
    Bridge bridge = {.client = fd, .keeper = keeper};
    pthread_mutex_init(&bridge.write_lock, NULL);
    pthread_t pump;
    if (pthread_create(&pump, NULL, pump_keeper_to_page, &bridge) == 0) {
        pump_page_to_keeper(&bridge);
        shutdown(keeper, SHUT_RDWR);
        pthread_join(pump, NULL);
    }
    close(keeper);
    pthread_mutex_destroy(&bridge.write_lock);
}

static void handle_connection(int fd) {
    Buffer head = {0};
    char chunk[4096];
    char *end = NULL;
    while (!end) {
        ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || head.size + (size_t)count > MAX_REQUEST_HEAD) break;
        buffer_append(&head, chunk, (size_t)count);
        end = strstr(head.data, "\r\n\r\n");
    }
    Request request;
    if (!end || !parse_request(head.data, &request)) {
        buffer_free(&head);
        return;
    }
    size_t body_received = head.size - (size_t)(end + 4 - head.data);
    while (body_received < request.content_length && request.content_length < 65536) {
        ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
        if (count <= 0) break;
        body_received += (size_t)count;
    }
    buffer_free(&head);

    bool get = strcmp(request.method, "GET") == 0;
    const char *path = request.path;
    char token[128];
    if (g_token[0] && strcmp(path, "/") == 0 && query_value(request.query, "token", token, sizeof(token)) &&
        strcmp(token, g_token) == 0) {
        char headers[300];
        snprintf(headers, sizeof(headers), "Set-Cookie: terminal_token=%s; HttpOnly; SameSite=Strict; Path=/\r\nLocation: /\r\n", g_token);
        send_response(fd, "303 See Other", NULL, NULL, 0, headers);
        return;
    }
    if (!authorized(&request)) {
        send_text(fd, "401 Unauthorized", "Open the URL that TerminalBackend printed when it started.\n");
        return;
    }
    if (strncmp(path, "/api/", 5) == 0 && !same_origin(&request)) {
        send_text(fd, "403 Forbidden", "Cross-origin requests are not allowed.\n");
        return;
    }
    static const struct {
        const char *path;
        const char *file;
        const char *type;
    } files[] = {
        {"/", "index.html", "text/html; charset=utf-8"},
        {"/web/app.js", "app.js", "text/javascript; charset=utf-8"},
        {"/web/style.css", "style.css", "text/css; charset=utf-8"},
        {"/web/xterm.js", "xterm.js", "text/javascript; charset=utf-8"},
        {"/web/xterm.css", "xterm.css", "text/css; charset=utf-8"},
        {"/web/addon-fit.js", "addon-fit.js", "text/javascript; charset=utf-8"},
        {"/web/app-icon.png", "app-icon.png", "image/png"},
        {"/favicon.ico", "app-icon.png", "image/png"},
    };
    if (get) {
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            if (strcmp(path, files[i].path) == 0) {
                send_web_file(fd, files[i].file, files[i].type);
                return;
            }
        }
    }
    if (strcmp(path, "/api/places") == 0 && get) {
        handle_places(fd);
        return;
    }
    if (strcmp(path, "/api/sessions") == 0) {
        if (get) handle_list(fd);
        else if (strcmp(request.method, "POST") == 0) handle_create(fd, &request);
        else send_text(fd, "405 Method Not Allowed", "Method not allowed.\n");
        return;
    }
    if (strncmp(path, "/api/sessions/", 14) == 0) {
        char id[64];
        const char *rest = path + 14;
        const char *slash = strchr(rest, '/');
        size_t length = slash ? (size_t)(slash - rest) : strlen(rest);
        if (length >= sizeof(id)) length = sizeof(id) - 1;
        memcpy(id, rest, length);
        id[length] = '\0';
        if (!session_valid_id(id)) {
            send_text(fd, "404 Not Found", "No such terminal session.\n");
        } else if (slash && strcmp(slash, "/close") == 0 && strcmp(request.method, "POST") == 0) {
            handle_close(fd, id);
        } else if (slash && strcmp(slash, "/attach") == 0 && get && strcasecmp(request.upgrade, "websocket") == 0 &&
                   request.websocket_key[0]) {
            handle_attach(fd, &request, id);
        } else {
            send_text(fd, "404 Not Found", "Not found.\n");
        }
        return;
    }
    send_text(fd, "404 Not Found", "Not found.\n");
}

static void *connection_thread(void *argument) {
    int fd = (int)(intptr_t)argument;
    handle_connection(fd);
    close(fd);
    return NULL;
}

static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    g_shutdown_requested = 1;
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
}

static void fatal(const char *message) {
    fprintf(stderr, "TerminalBackend: %s: %s\n", message, strerror(errno));
    exit(1);
}

static void default_socket_path(char *out, size_t capacity) {
#ifdef __APPLE__
    char directory[PATH_MAX];
    size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof(directory));
    snprintf(out, capacity, "%sorg.outershell.Terminal", length > 0 && length <= sizeof(directory) ? directory : "/tmp/");
#else
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] == '/') snprintf(out, capacity, "%s/org.outershell.Terminal", runtime);
    else snprintf(out, capacity, "/tmp/org.outershell.Terminal-%u", (unsigned)getuid());
#endif
}

static int create_unix_listener(const char *path) {
    if (path && path[0]) snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", path);
    else default_socket_path(g_listen_socket_path, sizeof(g_listen_socket_path));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) fatal("socket");
    unlink(g_listen_socket_path);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(g_listen_socket_path) >= sizeof(address.sun_path)) fatal("socket path too long");
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", g_listen_socket_path);
    mode_t previous = umask(0177);
    int bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
    umask(previous);
    if (bound != 0) fatal("bind");
    if (listen(fd, 64) != 0) fatal("listen");
    g_listen_socket_is_owned = true;
    return fd;
}

static int create_tcp_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) fatal("socket");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) fatal("bind");
    if (listen(fd, 64) != 0) fatal("listen");
    return fd;
}

static int systemd_activated_listener(void) {
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    if (!listen_pid || !listen_fds || strtol(listen_pid, NULL, 10) != (long)getpid() || strtol(listen_fds, NULL, 10) < 1) {
        return -1;
    }
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    return 3;
}

// Services start with a minimal environment. Terminal sessions inherit this
// process's environment, so give them the user's identity and a UTF-8 locale.
static void prepare_environment(void) {
    struct passwd *user = getpwuid(getuid());
    if (user) {
        if (user->pw_dir && user->pw_dir[0]) snprintf(g_home, sizeof(g_home), "%s", user->pw_dir);
        if (user->pw_shell && user->pw_shell[0] == '/') snprintf(g_shell, sizeof(g_shell), "%s", user->pw_shell);
        if (!getenv("USER")) setenv("USER", user->pw_name, 1);
        if (!getenv("LOGNAME")) setenv("LOGNAME", user->pw_name, 1);
    }
    if (!getenv("HOME")) setenv("HOME", g_home, 1);
    setenv("SHELL", g_shell, 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    if (!getenv("LANG") && !getenv("LC_ALL") && !getenv("LC_CTYPE")) {
#ifdef __APPLE__
        setenv("LANG", "en_US.UTF-8", 1);
#else
        setenv("LANG", "C.UTF-8", 1);
#endif
    }
    if (!getenv("PATH")) setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
}

// Finds this program, which runs sessions' keepers, and on macOS the web
// resources of the app bundle it may be part of.
static void locate_executable(void) {
#ifdef __APPLE__
    char executable[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) != 0 || !realpath(executable, g_executable)) return;
    char bundle[PATH_MAX];
    snprintf(bundle, sizeof(bundle), "%s", g_executable);
    char *macos = strstr(bundle, "/Contents/MacOS/");
    if (!macos) return;
    *macos = '\0';
    snprintf(g_web_root, sizeof(g_web_root), "%s/Contents/Resources/web", bundle);
#else
    ssize_t length = readlink("/proc/self/exe", g_executable, sizeof(g_executable) - 1);
    g_executable[length > 0 ? length : 0] = '\0';
#endif
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [--socket-path PATH | --launchd-socket-name NAME | --port PORT] [--web-root DIR]\n"
            "          [--bundles-dir DIR] [--label LABEL]\n"
            "\n"
            "--port listens on 127.0.0.1 for development. It prints a URL with a one-time\n"
            "token; opening it stores the token in a cookie that later requests must carry.\n"
            "Set TERMINAL_TOKEN (at least 32 lowercase hex digits) to keep the token across restarts.\n",
            program);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--keeper") == 0) return keeper_main(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_shutdown_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    prepare_environment();
    locate_executable();
#ifdef __APPLE__
    char launchd_socket_name[128] = "";
#endif
    char socket_path[PATH_MAX] = "";
    int port = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            snprintf(socket_path, sizeof(socket_path), "%s", argv[++i]);
#ifdef __APPLE__
        } else if (strcmp(argv[i], "--launchd-socket-name") == 0 && i + 1 < argc) {
            snprintf(launchd_socket_name, sizeof(launchd_socket_name), "%s", argv[++i]);
#endif
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--web-root") == 0 && i + 1 < argc) {
            snprintf(g_web_root, sizeof(g_web_root), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            snprintf(g_web_root, sizeof(g_web_root), "%s/../web", argv[++i]);
        } else if ((strcmp(argv[i], "--label") == 0 || strcmp(argv[i], "--icon-file") == 0) && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }
    if (!g_executable[0]) {
        fprintf(stderr, "TerminalBackend: could not find its own executable\n");
        return 1;
    }
    if (!g_web_root[0]) {
        fprintf(stderr, "TerminalBackend: pass --web-root\n");
        return 2;
    }

    int listen_fd;
    if (port >= 0) {
        const char *token = getenv("TERMINAL_TOKEN");
        if (token && strlen(token) >= 32 && strlen(token) < sizeof(g_token) && strspn(token, "0123456789abcdef") == strlen(token)) {
            snprintf(g_token, sizeof(g_token), "%s", token);
        } else {
            uint8_t random[24];
            int random_fd = open("/dev/urandom", O_RDONLY);
            if (random_fd < 0 || !read_exact_file(random_fd, random, sizeof(random))) fatal("random");
            close(random_fd);
            for (size_t i = 0; i < sizeof(random); i++) snprintf(g_token + 2 * i, 3, "%02x", random[i]);
        }
        unsetenv("TERMINAL_TOKEN");
        listen_fd = create_tcp_listener(port);
        printf("TerminalBackend listening on http://127.0.0.1:%d/?token=%s\n", port, g_token);
    } else if ((listen_fd = systemd_activated_listener()) >= 0) {
        printf("TerminalBackend listening on an inherited systemd socket\n");
#ifdef __APPLE__
    } else if (launchd_socket_name[0]) {
        int *fds = NULL;
        size_t count = 0;
        int result = launch_activate_socket(launchd_socket_name, &fds, &count);
        if (result != 0 || !fds || count == 0) {
            errno = result;
            fatal("launch_activate_socket");
        }
        listen_fd = fds[0];
        for (size_t i = 1; i < count; i++) close(fds[i]);
        free(fds);
        printf("TerminalBackend listening on launchd socket %s\n", launchd_socket_name);
#endif
    } else {
        listen_fd = create_unix_listener(socket_path);
        printf("TerminalBackend listening on unix socket %s\n", g_listen_socket_path);
    }
    fflush(stdout);
    g_listen_fd = listen_fd;

    while (!g_shutdown_requested) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (g_shutdown_requested) break;
            fatal("accept");
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        pthread_t thread;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attributes, connection_thread, (void *)(intptr_t)fd) != 0) close(fd);
        pthread_attr_destroy(&attributes);
    }
    close(listen_fd);
    if (g_listen_socket_is_owned) unlink(g_listen_socket_path);
    return 0;
}
