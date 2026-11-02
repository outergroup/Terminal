// Reading the containers registered with Outer Shell, and the commands
// registered in their images. Shared by the macOS/Linux and Windows backends.
#ifndef TERMINAL_REGISTRY_H
#define TERMINAL_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

// The request that asks Outer Shell's registry for its containers. The reply
// is a little-endian UInt32 length followed by that many bytes.
extern const unsigned char registry_containers_request[6];

typedef struct {
    char id[256];
    char name[256];
    char runtime[32];
    char container[256];
    char state[32];
    char endpoint[512];
} RegistryContainer;

bool registry_parse_containers(const unsigned char *bytes, size_t size, RegistryContainer **containers, size_t *count);

// A command registered in a container's image with `outerctl image add-command`,
// found at /etc/outershell/commands.d/ID.outercommand.
typedef struct {
    char name[256];
    char user[128];
    char directory[1024];
    char *arguments[64];
    char storage[8192];
} RegistryCommand;

bool registry_parse_command(const char *text, RegistryCommand *command);
// Container and command IDs: a letter or digit, then letters, digits, _ . -
bool registry_valid_identifier(const char *s);
void registry_command_path(const char *command_id, char *path, size_t capacity);

#endif
