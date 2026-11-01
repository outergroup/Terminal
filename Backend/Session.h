// Terminal sessions: the parts shared by the macOS/Linux and Windows backends.
//
// Each session is a keeper process that owns a pseudoterminal and the program
// running in it. It listens on a Unix socket (macOS, Linux) or a named pipe
// (Windows) that only the current user can open. The backend talks to keepers
// with frames: a type byte, a little-endian UInt32 payload length, and the
// payload.
//
//   'H' hello, client to keeper: UInt8 purpose, then for attach and resume
//       UInt16 columns and UInt16 rows, then for resume the UInt64 output
//       position the client has already shown.
//   'I' input, client to keeper: bytes typed into the session.
//   'R' resize, client to keeper: UInt16 columns, UInt16 rows.
//   'O' output, keeper to client: bytes the program wrote.
//   'P' position, keeper to client: the UInt64 output position after the
//       output sent so far.
//   'N' info, keeper to client: "key\tvalue\n" lines describing the session.
//   'X' ended, keeper to client: the session's program exited.
//
// A newly attached client receives a replay of recent output, minus requests
// that would make the client's terminal reply, followed by the terminal modes
// programs expect to still be in effect. A resuming client that has kept its
// screen receives only the output after its position, or a terminal reset and
// a replay when that output is no longer buffered.
#ifndef TERMINAL_SESSION_H
#define TERMINAL_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SESSION_REPLAY_CAPACITY (256u * 1024u)
#define SESSION_MAX_FRAME (1024u * 1024u)
#define SESSION_MAX_CLIENTS 16
#define SESSION_NO_POSITION UINT64_MAX

enum {
    SESSION_FRAME_HELLO = 'H',
    SESSION_FRAME_INPUT = 'I',
    SESSION_FRAME_RESIZE = 'R',
    SESSION_FRAME_OUTPUT = 'O',
    SESSION_FRAME_POSITION = 'P',
    SESSION_FRAME_INFO = 'N',
    SESSION_FRAME_ENDED = 'X',
};

enum {
    SESSION_PURPOSE_ATTACH = 1,
    SESSION_PURPOSE_INFO = 2,
    SESSION_PURPOSE_CLOSE = 3,
    SESSION_PURPOSE_RESUME = 4,
};

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} Bytes;

bool bytes_append(Bytes *bytes, const void *data, size_t size);
bool bytes_append_string(Bytes *bytes, const char *value);
void bytes_free(Bytes *bytes);

bool session_append_frame(Bytes *out, uint8_t type, const void *payload, size_t size);
bool session_append_hello(Bytes *out, uint8_t purpose, uint16_t columns, uint16_t rows, uint64_t from);
bool session_append_resize(Bytes *out, uint16_t columns, uint16_t rows);

typedef struct {
    Bytes pending;
    size_t offset;
} SessionFrameReader;

// Returns 1 with a frame, 0 when more bytes are needed, -1 for a malformed
// frame. The frame stays valid until the reader is next used.
int session_next_frame(SessionFrameReader *reader, uint8_t *type, const char **data, uint32_t *size);

typedef struct {
    uint16_t columns;
    uint16_t rows;
    uint64_t from;
} SessionAttachRequest;

bool session_parse_hello(const char *data, uint32_t size, uint8_t *purpose, SessionAttachRequest *request);
bool session_parse_size(const char *data, uint32_t size, uint16_t *columns, uint16_t *rows);
uint64_t session_read_u64(const char *data);

typedef struct SessionTerminal SessionTerminal;

SessionTerminal *session_terminal_create(void);
void session_terminal_record(SessionTerminal *terminal, const char *data, size_t size);
const char *session_terminal_title(const SessionTerminal *terminal);
// Appends the frames that bring a newly attached client up to date. Returns
// true when the client resumed, so its screen needs no redraw.
bool session_terminal_attach(const SessionTerminal *terminal, const SessionAttachRequest *request, Bytes *out);

typedef struct {
    char id[40];
    char title[256];
    char command[256];
    char cwd[1024];
    int64_t created;
    int64_t active;
    unsigned clients;
} SessionInfo;

bool session_append_info(Bytes *out, const SessionInfo *info);
void session_parse_info(const char *data, size_t size, SessionInfo *info);
bool session_valid_id(const char *id);
void session_clean_text(char *destination, size_t capacity, const char *source, size_t length);

// The Sec-WebSocket-Accept value for a WebSocket handshake's key.
void session_websocket_accept(const char *key, char accept[32]);
// Writes an unmasked server frame header and returns its length.
size_t session_websocket_header(uint8_t header[10], uint8_t opcode, size_t length);

#endif
