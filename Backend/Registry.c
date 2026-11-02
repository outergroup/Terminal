#include "Registry.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const unsigned char registry_containers_request[6] = {2, 0, 0, 0, 34, 0};

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Reads the string at the UInt32 offset and length pair at `field`.
static bool record_string(const unsigned char *bytes, size_t size, size_t field, char *out, size_t capacity) {
    if (field + 8 > size) return false;
    uint32_t offset = get_u32(bytes + field), length = get_u32(bytes + field + 4);
    if (offset > size || length > size - offset || length >= capacity) return false;
    memcpy(out, bytes + offset, length);
    out[length] = '\0';
    return true;
}

bool registry_valid_identifier(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 255 || !isalnum((unsigned char)s[0])) return false;
    for (size_t i = 1; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr("_.-", s[i])) return false;
    return true;
}

// The reply is "OSED", version 1, its size, the record count, the record and
// header sizes (72 and 48), and records of nine string references: ID, name,
// runtime, container ID, state, image, endpoint, error, and one more.
bool registry_parse_containers(const unsigned char *bytes, size_t size, RegistryContainer **containers, size_t *count) {
    *containers = NULL;
    *count = 0;
    if (size < 48 || memcmp(bytes, "OSED", 4) || get_u32(bytes + 4) != 1 || get_u32(bytes + 8) != size ||
        get_u32(bytes + 16) != 72 || get_u32(bytes + 20) != 48)
        return false;
    size_t records = get_u32(bytes + 12);
    if (records > (size - 48) / 72) return false;
    if (!records) return true;
    *containers = calloc(records, sizeof(RegistryContainer));
    if (!*containers) return false;
    for (size_t i = 0; i < records; i++) {
        char fields[9][512];
        bool read = true;
        for (size_t j = 0; j < 9; j++)
            read = record_string(bytes, size, 48 + i * 72 + j * 8, fields[j], sizeof(fields[j])) && read;
        if (!read || !registry_valid_identifier(fields[0]) || !registry_valid_identifier(fields[3])) continue;
        RegistryContainer *container = &(*containers)[(*count)++];
        snprintf(container->id, sizeof(container->id), "%s", fields[0]);
        snprintf(container->name, sizeof(container->name), "%s", fields[1][0] ? fields[1] : fields[3]);
        snprintf(container->runtime, sizeof(container->runtime), "%s", fields[2]);
        snprintf(container->container, sizeof(container->container), "%s", fields[3]);
        snprintf(container->state, sizeof(container->state), "%s", fields[4]);
        snprintf(container->endpoint, sizeof(container->endpoint), "%s", fields[6]);
    }
    return true;
}

void registry_command_path(const char *command_id, char *path, size_t capacity) {
    snprintf(path, capacity, "/etc/outershell/commands.d/%s.outercommand", command_id);
}

bool registry_parse_command(const char *text, RegistryCommand *command) {
    memset(command, 0, sizeof(*command));
    if (strncmp(text, "[Command]\n", 10)) return false;
    size_t used = 0, count = 0;
    for (const char *line = text + 10; *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        const char *equals = memchr(line, '=', length);
        if (equals) {
            size_t key_length = (size_t)(equals - line), value_length = length - key_length - 1;
            const char *value = equals + 1;
            char *field = NULL;
            size_t capacity = 0;
            if (key_length == 4 && !memcmp(line, "Name", 4)) field = command->name, capacity = sizeof(command->name);
            else if (key_length == 4 && !memcmp(line, "User", 4)) field = command->user, capacity = sizeof(command->user);
            else if (key_length == 16 && !memcmp(line, "WorkingDirectory", 16))
                field = command->directory, capacity = sizeof(command->directory);
            bool executable = key_length == 10 && !memcmp(line, "Executable", 10);
            bool argument = key_length == 8 && !memcmp(line, "Argument", 8);
            if (field) {
                if (value_length >= capacity) return false;
                memcpy(field, value, value_length);
                field[value_length] = '\0';
            } else if (executable || argument) {
                if ((executable && count) || (argument && !count) ||
                    count + 1 >= sizeof(command->arguments) / sizeof(*command->arguments) ||
                    used + value_length + 1 > sizeof(command->storage))
                    return false;
                memcpy(command->storage + used, value, value_length);
                command->storage[used + value_length] = '\0';
                command->arguments[count++] = command->storage + used;
                used += value_length + 1;
            }
        }
        line = end ? end + 1 : line + length;
    }
    return count > 0 && command->arguments[0][0] == '/';
}
