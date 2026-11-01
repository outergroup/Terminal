#include "Session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SESSION_SEQUENCE_CAPACITY 2048
#define SESSION_OFFSET_MARK_CAPACITY 8192

static const int kTrackedModes[] = {1, 1000, 1002, 1003, 1004, 1005, 1006, 1015, 2004, 9001};
#define TRACKED_MODE_COUNT (sizeof(kTrackedModes) / sizeof(kTrackedModes[0]))

bool bytes_append(Bytes *bytes, const void *data, size_t size) {
    if (bytes->size + size + 1 > bytes->capacity) {
        size_t capacity = bytes->capacity ? bytes->capacity : 256;
        while (capacity < bytes->size + size + 1) capacity *= 2;
        char *grown = realloc(bytes->data, capacity);
        if (!grown) return false;
        bytes->data = grown;
        bytes->capacity = capacity;
    }
    if (size) memcpy(bytes->data + bytes->size, data, size);
    bytes->size += size;
    bytes->data[bytes->size] = '\0';
    return true;
}

bool bytes_append_string(Bytes *bytes, const char *value) {
    return bytes_append(bytes, value, strlen(value));
}

void bytes_free(Bytes *bytes) {
    free(bytes->data);
    bytes->data = NULL;
    bytes->size = bytes->capacity = 0;
}

static void write_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static uint16_t read_u16(const char *data) {
    return (uint16_t)((uint8_t)data[0] | (uint16_t)(uint8_t)data[1] << 8);
}

uint64_t session_read_u64(const char *data) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) value = value << 8 | (uint8_t)data[i];
    return value;
}

bool session_append_frame(Bytes *out, uint8_t type, const void *payload, size_t size) {
    uint8_t header[5] = {type, (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24)};
    return bytes_append(out, header, sizeof(header)) && bytes_append(out, payload, size);
}

bool session_append_hello(Bytes *out, uint8_t purpose, uint16_t columns, uint16_t rows, uint64_t from) {
    uint8_t payload[13];
    size_t length = 1;
    payload[0] = purpose;
    if (purpose == SESSION_PURPOSE_ATTACH || purpose == SESSION_PURPOSE_RESUME) {
        write_u16(payload + 1, columns);
        write_u16(payload + 3, rows);
        length = 5;
    }
    if (purpose == SESSION_PURPOSE_RESUME) {
        for (int i = 0; i < 8; i++) payload[5 + i] = (uint8_t)(from >> (8 * i));
        length = 13;
    }
    return session_append_frame(out, SESSION_FRAME_HELLO, payload, length);
}

bool session_append_resize(Bytes *out, uint16_t columns, uint16_t rows) {
    uint8_t payload[4];
    write_u16(payload, columns);
    write_u16(payload + 2, rows);
    return session_append_frame(out, SESSION_FRAME_RESIZE, payload, sizeof(payload));
}

int session_next_frame(SessionFrameReader *reader, uint8_t *type, const char **data, uint32_t *size) {
    size_t available = reader->pending.size - reader->offset;
    if (available >= 5) {
        const uint8_t *bytes = (const uint8_t *)reader->pending.data + reader->offset;
        uint32_t length = (uint32_t)bytes[1] | (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3] << 16 | (uint32_t)bytes[4] << 24;
        if (length > SESSION_MAX_FRAME) return -1;
        if (available >= 5u + length) {
            *type = bytes[0];
            *data = (const char *)bytes + 5;
            *size = length;
            reader->offset += 5u + length;
            return 1;
        }
    }
    if (reader->offset > 0) {
        memmove(reader->pending.data, reader->pending.data + reader->offset, available);
        reader->pending.size = available;
        reader->offset = 0;
    }
    return 0;
}

bool session_parse_size(const char *data, uint32_t size, uint16_t *columns, uint16_t *rows) {
    if (size < 4) return false;
    *columns = read_u16(data);
    *rows = read_u16(data + 2);
    if (*columns == 0 || *columns > 1000) *columns = 80;
    if (*rows == 0 || *rows > 1000) *rows = 24;
    return true;
}

bool session_parse_hello(const char *data, uint32_t size, uint8_t *purpose, SessionAttachRequest *request) {
    if (size < 1) return false;
    *purpose = (uint8_t)data[0];
    request->columns = 80;
    request->rows = 24;
    request->from = SESSION_NO_POSITION;
    if (*purpose == SESSION_PURPOSE_ATTACH || *purpose == SESSION_PURPOSE_RESUME) {
        if (!session_parse_size(data + 1, size - 1, &request->columns, &request->rows)) return false;
    }
    if (*purpose == SESSION_PURPOSE_RESUME) {
        if (size < 13) return false;
        request->from = session_read_u64(data + 5);
    }
    return true;
}

bool session_valid_id(const char *id) {
    size_t length = strlen(id);
    if (length == 0 || length > 32) return false;
    for (size_t i = 0; i < length; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

static size_t utf8_sequence_length(unsigned char lead) {
    if (lead >= 0xf0) return 4;
    if (lead >= 0xe0) return 3;
    if (lead >= 0xc0) return 2;
    return 1;
}

// Copies text, replacing control characters with spaces and never cutting a
// UTF-8 sequence in half.
void session_clean_text(char *destination, size_t capacity, const char *source, size_t length) {
    size_t out = 0;
    size_t i = 0;
    for (; i < length && out + 1 < capacity; i++) {
        unsigned char c = (unsigned char)source[i];
        destination[out++] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
    if (i < length) {
        size_t lead = out;
        while (lead > 0 && ((unsigned char)destination[lead - 1] & 0xc0) == 0x80) lead--;
        if (lead > 0 && ((unsigned char)destination[lead - 1] & 0x80) &&
            out - (lead - 1) < utf8_sequence_length((unsigned char)destination[lead - 1])) {
            out = lead - 1;
        }
    }
    destination[out] = '\0';
}

typedef enum { PARSE_GROUND, PARSE_ESCAPE, PARSE_CSI, PARSE_STRING, PARSE_STRING_ESCAPE } ParseState;

typedef struct {
    uint64_t raw;
    uint64_t replay;
} OffsetMark;

struct SessionTerminal {
    uint8_t ring[SESSION_REPLAY_CAPACITY];
    uint64_t total;
    uint64_t raw_total;
    OffsetMark marks[SESSION_OFFSET_MARK_CAPACITY];
    size_t mark_start;
    size_t mark_count;
    ParseState state;
    char sequence[SESSION_SEQUENCE_CAPACITY];
    size_t sequence_length;
    bool passthrough;
    bool discard;
    char string_kind;
    uint32_t modes;
    int alternate_screen_mode;
    uint64_t alternate_screen_start;
    bool cursor_hidden;
    char title[256];
};

SessionTerminal *session_terminal_create(void) {
    return calloc(1, sizeof(SessionTerminal));
}

const char *session_terminal_title(const SessionTerminal *terminal) {
    return terminal->title;
}

static void append_ring(SessionTerminal *terminal, const char *data, size_t size) {
    if (size >= SESSION_REPLAY_CAPACITY) {
        data += size - SESSION_REPLAY_CAPACITY;
        terminal->total += size - SESSION_REPLAY_CAPACITY;
        size = SESSION_REPLAY_CAPACITY;
    }
    size_t start = (size_t)(terminal->total % SESSION_REPLAY_CAPACITY);
    size_t first = size < SESSION_REPLAY_CAPACITY - start ? size : SESSION_REPLAY_CAPACITY - start;
    memcpy(terminal->ring + start, data, first);
    memcpy(terminal->ring, data + first, size - first);
    terminal->total += size;
}

static void apply_mode(SessionTerminal *terminal, int mode, bool set) {
    if (mode == 25) {
        terminal->cursor_hidden = !set;
    } else if (mode == 47 || mode == 1047 || mode == 1049) {
        if (set && terminal->alternate_screen_mode == 0) {
            terminal->alternate_screen_mode = mode;
            terminal->alternate_screen_start = terminal->total;
        } else if (!set) {
            terminal->alternate_screen_mode = 0;
        }
    } else {
        for (size_t i = 0; i < TRACKED_MODE_COUNT; i++) {
            if (kTrackedModes[i] != mode) continue;
            if (set) terminal->modes |= 1u << i;
            else terminal->modes &= ~(1u << i);
        }
    }
}

// Keeps a control sequence in the replay unless it asks the terminal for a
// reply, and tracks the private modes it sets.
static bool keep_csi(SessionTerminal *terminal) {
    const char *sequence = terminal->sequence;
    size_t length = terminal->sequence_length;
    char final = sequence[length - 1];
    char prefix = length > 3 && strchr("?<=>", sequence[2]) ? sequence[2] : 0;
    bool dollar = false;
    for (size_t i = 2; i + 1 < length; i++) {
        if (sequence[i] == '$') dollar = true;
    }
    if (prefix == '?' && (final == 'h' || final == 'l')) {
        int value = 0;
        bool digits = false;
        for (size_t i = 3; i < length; i++) {
            char c = sequence[i];
            if (c >= '0' && c <= '9') {
                if (value < 100000) value = value * 10 + (c - '0');
                digits = true;
            } else {
                if (digits) apply_mode(terminal, value, final == 'h');
                value = 0;
                digits = false;
            }
        }
        return true;
    }
    if (final == 'c' || final == 'n' || final == 't' || final == 'x') return false;
    if (final == 'p' && dollar) return false;
    if ((final == 'u' && prefix == '?') || (final == 'q' && prefix == '>')) return false;
    return true;
}

static bool keep_string(SessionTerminal *terminal) {
    if (terminal->string_kind != ']') return false;
    const char *body = terminal->sequence + 2;
    size_t length = terminal->sequence_length - 2;
    if (length > 0 && body[length - 1] == '\a') length--;
    else if (length > 1 && body[length - 2] == '\x1b' && body[length - 1] == '\\') length -= 2;
    for (size_t i = 0; i < length; i++) {
        if (body[i] == '?' && (i == 0 || body[i - 1] == ';') && (i + 1 == length || body[i + 1] == ';')) return false;
    }
    if (length >= 2 && (body[0] == '0' || body[0] == '2') && body[1] == ';') {
        session_clean_text(terminal->title, sizeof(terminal->title), body + 2, length - 2);
    }
    return true;
}

static void finish_sequence(SessionTerminal *terminal, bool keep) {
    if (!terminal->discard && (keep || terminal->passthrough)) {
        append_ring(terminal, terminal->sequence, terminal->sequence_length);
    }
    terminal->sequence_length = 0;
    terminal->passthrough = false;
    terminal->discard = false;
    terminal->state = PARSE_GROUND;
}

static void push_sequence(SessionTerminal *terminal, char c) {
    if (terminal->discard) return;
    if (terminal->sequence_length == sizeof(terminal->sequence)) {
        if ((terminal->state == PARSE_STRING || terminal->state == PARSE_STRING_ESCAPE) && terminal->string_kind != ']') {
            terminal->discard = true;
            terminal->sequence_length = 0;
            return;
        }
        append_ring(terminal, terminal->sequence, terminal->sequence_length);
        terminal->sequence_length = 0;
        terminal->passthrough = true;
    }
    terminal->sequence[terminal->sequence_length++] = c;
}

static void feed(SessionTerminal *terminal, const char *data, size_t size) {
    size_t ground_start = 0;
    for (size_t i = 0; i < size; i++) {
        char c = data[i];
        switch (terminal->state) {
        case PARSE_GROUND:
            if (c == '\x1b') {
                append_ring(terminal, data + ground_start, i - ground_start);
                terminal->sequence_length = 0;
                push_sequence(terminal, c);
                terminal->state = PARSE_ESCAPE;
            }
            break;
        case PARSE_ESCAPE:
            push_sequence(terminal, c);
            if (c == '[') {
                terminal->state = PARSE_CSI;
            } else if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') {
                terminal->string_kind = c;
                terminal->state = PARSE_STRING;
            } else if (c < 0x20 || c > 0x2f) {
                if (c == 'c') {
                    terminal->modes = 0;
                    terminal->alternate_screen_mode = 0;
                    terminal->cursor_hidden = false;
                }
                finish_sequence(terminal, c != 'Z');
                ground_start = i + 1;
            }
            break;
        case PARSE_CSI:
            push_sequence(terminal, c);
            if (c >= 0x40 && c <= 0x7e) {
                finish_sequence(terminal, terminal->passthrough || keep_csi(terminal));
                ground_start = i + 1;
            } else if (c == 0x18 || c == 0x1a) {
                finish_sequence(terminal, false);
                ground_start = i + 1;
            }
            break;
        case PARSE_STRING:
            push_sequence(terminal, c);
            if (c == '\a') {
                finish_sequence(terminal, !terminal->passthrough && keep_string(terminal));
                ground_start = i + 1;
            } else if (c == '\x1b') {
                terminal->state = PARSE_STRING_ESCAPE;
            }
            break;
        case PARSE_STRING_ESCAPE:
            if (c == '\\') {
                push_sequence(terminal, c);
                finish_sequence(terminal, !terminal->passthrough && keep_string(terminal));
                ground_start = i + 1;
            } else {
                terminal->sequence_length = terminal->sequence_length > 0 ? terminal->sequence_length - 1 : 0;
                finish_sequence(terminal, !terminal->passthrough && keep_string(terminal));
                push_sequence(terminal, '\x1b');
                terminal->state = PARSE_ESCAPE;
                i--;
            }
            break;
        }
    }
    if (terminal->state == PARSE_GROUND) append_ring(terminal, data + ground_start, size - ground_start);
}

// Every chunk boundary is a position a client can resume from; the mark maps
// it to the matching offset in the replay buffer.
void session_terminal_record(SessionTerminal *terminal, const char *data, size_t size) {
    feed(terminal, data, size);
    terminal->raw_total += size;
    size_t slot = (terminal->mark_start + terminal->mark_count) % SESSION_OFFSET_MARK_CAPACITY;
    terminal->marks[slot].raw = terminal->raw_total;
    terminal->marks[slot].replay = terminal->total;
    if (terminal->mark_count < SESSION_OFFSET_MARK_CAPACITY) terminal->mark_count++;
    else terminal->mark_start = (terminal->mark_start + 1) % SESSION_OFFSET_MARK_CAPACITY;
}

static bool append_ring_frames(const SessionTerminal *terminal, uint64_t start, uint64_t end, Bytes *out) {
    Bytes bytes = {0};
    bool ok = true;
    for (uint64_t offset = start; ok && offset < end;) {
        size_t position = (size_t)(offset % SESSION_REPLAY_CAPACITY);
        size_t chunk = SESSION_REPLAY_CAPACITY - position;
        if (chunk > end - offset) chunk = (size_t)(end - offset);
        ok = bytes_append(&bytes, terminal->ring + position, chunk);
        offset += chunk;
    }
    for (size_t offset = 0; ok && offset < bytes.size; offset += SESSION_MAX_FRAME) {
        size_t chunk = bytes.size - offset < SESSION_MAX_FRAME ? bytes.size - offset : SESSION_MAX_FRAME;
        ok = session_append_frame(out, SESSION_FRAME_OUTPUT, bytes.data + offset, chunk);
    }
    bytes_free(&bytes);
    return ok;
}

static bool append_missed(const SessionTerminal *terminal, uint64_t from, Bytes *out) {
    if (from == terminal->raw_total) return true;
    if (from > terminal->raw_total || terminal->mark_count == 0) return false;
    size_t low = 0;
    size_t high = terminal->mark_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (terminal->marks[(terminal->mark_start + middle) % SESSION_OFFSET_MARK_CAPACITY].raw < from) low = middle + 1;
        else high = middle;
    }
    if (low == terminal->mark_count) return false;
    const OffsetMark *mark = &terminal->marks[(terminal->mark_start + low) % SESSION_OFFSET_MARK_CAPACITY];
    uint64_t oldest = terminal->total > SESSION_REPLAY_CAPACITY ? terminal->total - SESSION_REPLAY_CAPACITY : 0;
    if (mark->raw != from || mark->replay < oldest) return false;
    return append_ring_frames(terminal, mark->replay, terminal->total, out);
}

static bool append_replay(const SessionTerminal *terminal, Bytes *out) {
    uint64_t oldest = terminal->total > SESSION_REPLAY_CAPACITY ? terminal->total - SESSION_REPLAY_CAPACITY : 0;
    uint64_t end = terminal->total;
    if (terminal->alternate_screen_mode != 0) {
        end = terminal->alternate_screen_start > oldest ? terminal->alternate_screen_start : oldest;
    }
    uint64_t start = oldest;
    if (start > 0) {
        while (start < end && terminal->ring[start % SESSION_REPLAY_CAPACITY] != '\n') start++;
        if (start < end) start++;
    }
    bool ok = append_ring_frames(terminal, start, end, out);
    Bytes modes = {0};
    char mode[32];
    if (terminal->alternate_screen_mode != 0) {
        snprintf(mode, sizeof(mode), "\x1b[?%dh", terminal->alternate_screen_mode);
        bytes_append_string(&modes, mode);
    }
    for (size_t i = 0; i < TRACKED_MODE_COUNT; i++) {
        if (!(terminal->modes & (1u << i))) continue;
        snprintf(mode, sizeof(mode), "\x1b[?%dh", kTrackedModes[i]);
        bytes_append_string(&modes, mode);
    }
    if (terminal->cursor_hidden) bytes_append_string(&modes, "\x1b[?25l");
    if (ok && modes.size) ok = session_append_frame(out, SESSION_FRAME_OUTPUT, modes.data, modes.size);
    bytes_free(&modes);
    return ok;
}

static bool append_position(Bytes *out, uint64_t position) {
    uint8_t payload[8];
    for (int i = 0; i < 8; i++) payload[i] = (uint8_t)(position >> (8 * i));
    return session_append_frame(out, SESSION_FRAME_POSITION, payload, sizeof(payload));
}

bool session_terminal_attach(const SessionTerminal *terminal, const SessionAttachRequest *request, Bytes *out) {
    bool resumed = false;
    if (request->from != SESSION_NO_POSITION) {
        Bytes missed = {0};
        resumed = append_missed(terminal, request->from, &missed) && bytes_append(out, missed.data, missed.size);
        bytes_free(&missed);
        if (!resumed) session_append_frame(out, SESSION_FRAME_OUTPUT, "\x1b" "c", 2);
    }
    if (!resumed) append_replay(terminal, out);
    append_position(out, terminal->raw_total);
    return resumed;
}

static bool append_info_field(Bytes *out, const char *key, const char *value) {
    char clean[1024];
    session_clean_text(clean, sizeof(clean), value, strlen(value));
    return bytes_append_string(out, key) && bytes_append_string(out, "\t") && bytes_append_string(out, clean) &&
        bytes_append_string(out, "\n");
}

bool session_append_info(Bytes *out, const SessionInfo *info) {
    Bytes text = {0};
    char created[32];
    char active[32];
    char clients[32];
    snprintf(created, sizeof(created), "%lld", (long long)info->created);
    snprintf(active, sizeof(active), "%lld", (long long)info->active);
    snprintf(clients, sizeof(clients), "%u", info->clients);
    bool ok = append_info_field(&text, "id", info->id) && append_info_field(&text, "title", info->title) &&
        append_info_field(&text, "command", info->command) && append_info_field(&text, "cwd", info->cwd) &&
        append_info_field(&text, "created", created) && append_info_field(&text, "active", active) &&
        append_info_field(&text, "clients", clients) && session_append_frame(out, SESSION_FRAME_INFO, text.data, text.size);
    bytes_free(&text);
    return ok;
}

void session_parse_info(const char *data, size_t size, SessionInfo *info) {
    memset(info, 0, sizeof(*info));
    const char *end = data + size;
    while (data < end) {
        const char *line_end = memchr(data, '\n', (size_t)(end - data));
        if (!line_end) line_end = end;
        const char *tab = memchr(data, '\t', (size_t)(line_end - data));
        if (tab) {
            size_t key_length = (size_t)(tab - data);
            const char *value = tab + 1;
            size_t value_length = (size_t)(line_end - value);
            char number[32];
            session_clean_text(number, sizeof(number), value, value_length);
            if (key_length == 2 && !memcmp(data, "id", 2)) session_clean_text(info->id, sizeof(info->id), value, value_length);
            else if (key_length == 5 && !memcmp(data, "title", 5)) session_clean_text(info->title, sizeof(info->title), value, value_length);
            else if (key_length == 7 && !memcmp(data, "command", 7)) session_clean_text(info->command, sizeof(info->command), value, value_length);
            else if (key_length == 3 && !memcmp(data, "cwd", 3)) session_clean_text(info->cwd, sizeof(info->cwd), value, value_length);
            else if (key_length == 7 && !memcmp(data, "created", 7)) info->created = strtoll(number, NULL, 10);
            else if (key_length == 6 && !memcmp(data, "active", 6)) info->active = strtoll(number, NULL, 10);
            else if (key_length == 7 && !memcmp(data, "clients", 7)) info->clients = (unsigned)strtoul(number, NULL, 10);
        }
        data = line_end + 1;
    }
}

// SHA-1, used only for the WebSocket handshake.
static void sha1(const uint8_t *message, size_t length, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t padded = ((length + 8) / 64 + 1) * 64;
    uint8_t *data = calloc(padded, 1);
    if (!data) {
        memset(digest, 0, 20);
        return;
    }
    memcpy(data, message, length);
    data[length] = 0x80;
    uint64_t bits = (uint64_t)length * 8;
    for (int i = 0; i < 8; i++) data[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)data[chunk + 4 * i] << 24 | (uint32_t)data[chunk + 4 * i + 1] << 16 |
                (uint32_t)data[chunk + 4 * i + 2] << 8 | data[chunk + 4 * i + 3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t value = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = value << 1 | value >> 31;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d;
            d = c;
            c = b << 30 | b >> 2;
            b = a;
            a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(data);
    for (int i = 0; i < 5; i++) {
        digest[4 * i] = (uint8_t)(h[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(h[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(h[i] >> 8);
        digest[4 * i + 3] = (uint8_t)h[i];
    }
}

static void base64_encode(const uint8_t *data, size_t length, char *out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t value = (uint32_t)data[i] << 16 | (i + 1 < length ? (uint32_t)data[i + 1] << 8 : 0) |
            (i + 2 < length ? data[i + 2] : 0);
        out[o++] = alphabet[value >> 18 & 63];
        out[o++] = alphabet[value >> 12 & 63];
        out[o++] = i + 1 < length ? alphabet[value >> 6 & 63] : '=';
        out[o++] = i + 2 < length ? alphabet[value & 63] : '=';
    }
    out[o] = '\0';
}

void session_websocket_accept(const char *key, char accept[32]) {
    char input[256];
    snprintf(input, sizeof(input), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    uint8_t digest[20];
    sha1((const uint8_t *)input, strlen(input), digest);
    base64_encode(digest, sizeof(digest), accept);
}

size_t session_websocket_header(uint8_t header[10], uint8_t opcode, size_t length) {
    header[0] = (uint8_t)(0x80 | opcode);
    if (length < 126) {
        header[1] = (uint8_t)length;
        return 2;
    }
    if (length < 65536) {
        header[1] = 126;
        header[2] = (uint8_t)(length >> 8);
        header[3] = (uint8_t)length;
        return 4;
    }
    header[1] = 127;
    for (int i = 0; i < 8; i++) header[2 + i] = (uint8_t)((uint64_t)length >> (56 - 8 * i));
    return 10;
}
