// Terminal session keepers on macOS and Linux.
#ifndef TERMINAL_KEEPER_H
#define TERMINAL_KEEPER_H

#include "Session.h"

typedef struct {
    SessionInfo info;
    char endpoint[1024];
} SessionEntry;

// Starts a keeper running `command` (argv style, NULL-terminated) as a new
// session and writes its ID. `executable` is this program's path.
bool keeper_start(const char *executable, const char *const *command, const char *cwd, uint16_t columns,
                  uint16_t rows, char id[17]);
// Lists the running sessions, most recently active first.
bool keeper_list(SessionEntry **entries, size_t *count);
bool keeper_find(const char *id, SessionEntry *entry);
// Connects to a session's keeper. Returns a socket, or -1.
int keeper_connect(const char *endpoint);
bool keeper_close(const char *endpoint);
// Runs a keeper: TerminalBackend --keeper --id ID --columns C --rows R --cwd DIR -- COMMAND...
int keeper_main(int argc, char **argv);

#endif
