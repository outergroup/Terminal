// Places where Terminal can run a session besides this computer: the
// containers registered with Outer Shell. A session in a container is a keeper
// on this computer running the container runtime's `exec -it`, so nothing is
// installed in the container and the session survives this backend like any
// other. It ends when the container stops.
#ifndef TERMINAL_PLACES_H
#define TERMINAL_PLACES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#include "Registry.h"

// A place's ID is the container's ID in Outer Shell.
typedef struct {
    char id[320];
    char name[256];
    char container[256];
    char endpoint[512];
    char executable[PATH_MAX];
} Place;

// Lists the running containers registered with Outer Shell.
bool places_list(Place **places, size_t *count);
bool places_find(const char *id, Place *place);
// Builds the command that runs `program` in the place's container with a
// terminal. `user` and `directory` may be empty. Returns the argument count,
// or 0 when it does not fit.
size_t place_exec_command(const Place *place, const char *user, const char *directory,
                          const char *const *program, const char **arguments, size_t capacity);
bool place_registered_command(const Place *place, const char *command_id, RegistryCommand *command);

#endif
