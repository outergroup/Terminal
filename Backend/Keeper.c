#define _GNU_SOURCE
#include "Keeper.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

// Adds the folder holding Outer Shell's outerctl to PATH, so sessions and the
// agents run in them can use it. It goes last, so it never shadows anything.
static void add_outerctl_to_path(void) {
    const char *home = getenv("HOME");
    char directories[3][PATH_MAX] = {{0}};
    const char *override = getenv("OUTERSHELL_HOME");
    if (override && override[0]) snprintf(directories[0], PATH_MAX, "%s/bin", override);
#ifdef __APPLE__
    if (home) snprintf(directories[1], PATH_MAX, "%s/Library/Application Support/outershell/bin", home);
    snprintf(directories[2], PATH_MAX, "/Library/Application Support/outershell/bin");
#else
    const char *state = getenv("XDG_STATE_HOME");
    if (state && state[0]) snprintf(directories[1], PATH_MAX, "%s/outershell/bin", state);
    else if (home) snprintf(directories[1], PATH_MAX, "%s/.local/state/outershell/bin", home);
    snprintf(directories[2], PATH_MAX, "/opt/outershell/bin");
#endif
    for (size_t i = 0; i < 3; i++) {
        char outerctl[PATH_MAX + 16];
        if (!directories[i][0]) continue;
        snprintf(outerctl, sizeof(outerctl), "%s/outerctl", directories[i]);
        if (access(outerctl, X_OK)) continue;
        const char *path = getenv("PATH");
        char combined[8192];
        if (snprintf(combined, sizeof(combined), "%s%s%s", path && path[0] ? path : "/usr/bin:/bin",
                     ":", directories[i]) < (int)sizeof(combined))
            setenv("PATH", combined, 1);
        return;
    }
}

static bool private_directory(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return false;
    struct stat status;
    if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != getuid()) return false;
    return (status.st_mode & 0077) == 0 || chmod(path, 0700) == 0;
}

// Sessions live in a private per-user directory that is cleared on restart.
static bool session_directory(char *out, size_t capacity) {
#ifdef __APPLE__
    char temporary[PATH_MAX];
    size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, temporary, sizeof(temporary));
    if (length == 0 || length > sizeof(temporary)) return false;
    snprintf(out, capacity, "%sorg.outershell.Terminal.sessions", temporary);
#else
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    struct stat status;
    if (!runtime || runtime[0] != '/') {
        snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)getuid());
        runtime = stat(fallback, &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == getuid() ? fallback : NULL;
    }
    if (runtime) snprintf(out, capacity, "%s/org.outershell.Terminal.sessions", runtime);
    else snprintf(out, capacity, "/tmp/org.outershell.Terminal.sessions-%u", (unsigned)getuid());
#endif
    return private_directory(out);
}

static bool socket_path_for(const char *id, char *out, size_t capacity) {
    char directory[PATH_MAX];
    if (!session_directory(directory, sizeof(directory))) return false;
    int length = snprintf(out, capacity, "%s/%s.sock", directory, id);
    return length > 0 && (size_t)length < sizeof(((struct sockaddr_un *)0)->sun_path) && (size_t)length < capacity;
}

static bool random_hex(char *out, size_t bytes) {
    uint8_t random[32];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || bytes > sizeof(random)) {
        if (fd >= 0) close(fd);
        return false;
    }
    size_t got = 0;
    while (got < bytes) {
        ssize_t count = read(fd, random + got, bytes - got);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        got += (size_t)count;
    }
    close(fd);
    if (got < bytes) return false;
    for (size_t i = 0; i < bytes; i++) snprintf(out + 2 * i, 3, "%02x", random[i]);
    return true;
}

static bool write_all(int fd, const void *data, size_t size) {
    const char *bytes = data;
    while (size > 0) {
        ssize_t written = send(fd, bytes, size, SEND_FLAGS);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes += written;
        size -= (size_t)written;
    }
    return true;
}

int keeper_connect(const char *endpoint) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", endpoint);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) return fd;
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

// Sends one request and waits for a frame of the expected type.
static bool request(const char *endpoint, uint8_t purpose, uint8_t expected, Bytes *payload, bool *stale) {
    *stale = false;
    int fd = keeper_connect(endpoint);
    if (fd < 0) {
        *stale = errno == ECONNREFUSED || errno == ENOENT;
        return false;
    }
    struct timeval timeout = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    Bytes hello = {0};
    bool ok = session_append_hello(&hello, purpose, 0, 0, 0) && write_all(fd, hello.data, hello.size);
    bytes_free(&hello);
    SessionFrameReader reader = {0};
    bool found = false;
    char buffer[16384];
    while (ok && !found) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || !bytes_append(&reader.pending, buffer, (size_t)count)) break;
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
    close(fd);
    bytes_free(&reader.pending);
    return ok && found;
}

static int compare_entries(const void *a, const void *b) {
    const SessionEntry *left = a;
    const SessionEntry *right = b;
    if (left->info.active != right->info.active) return left->info.active > right->info.active ? -1 : 1;
    return strcmp(left->info.id, right->info.id);
}

bool keeper_list(SessionEntry **entries, size_t *count) {
    *entries = NULL;
    *count = 0;
    char directory[PATH_MAX];
    if (!session_directory(directory, sizeof(directory))) return false;
    DIR *listing = opendir(directory);
    if (!listing) return false;
    struct dirent *item;
    while ((item = readdir(listing)) != NULL) {
        size_t length = strlen(item->d_name);
        if (length < 6 || strcmp(item->d_name + length - 5, ".sock") != 0) continue;
        char endpoint[1024];
        snprintf(endpoint, sizeof(endpoint), "%s/%s", directory, item->d_name);
        Bytes payload = {0};
        bool stale = false;
        if (request(endpoint, SESSION_PURPOSE_INFO, SESSION_FRAME_INFO, &payload, &stale)) {
            SessionEntry entry;
            memset(&entry, 0, sizeof(entry));
            session_parse_info(payload.data, payload.size, &entry.info);
            snprintf(entry.endpoint, sizeof(entry.endpoint), "%s", endpoint);
            SessionEntry *grown = realloc(*entries, (*count + 1) * sizeof(SessionEntry));
            if (grown && session_valid_id(entry.info.id)) {
                *entries = grown;
                (*entries)[(*count)++] = entry;
            } else if (grown) {
                *entries = grown;
            }
        } else if (stale) {
            unlink(endpoint);
        }
        bytes_free(&payload);
    }
    closedir(listing);
    if (*count > 1) qsort(*entries, *count, sizeof(SessionEntry), compare_entries);
    return true;
}

bool keeper_find(const char *id, SessionEntry *entry) {
    if (!session_valid_id(id)) return false;
    char endpoint[1024];
    if (!socket_path_for(id, endpoint, sizeof(endpoint))) return false;
    Bytes payload = {0};
    bool stale = false;
    bool ok = request(endpoint, SESSION_PURPOSE_INFO, SESSION_FRAME_INFO, &payload, &stale);
    if (ok) {
        memset(entry, 0, sizeof(*entry));
        session_parse_info(payload.data, payload.size, &entry->info);
        snprintf(entry->endpoint, sizeof(entry->endpoint), "%s", endpoint);
        ok = strcmp(entry->info.id, id) == 0;
    } else if (stale) {
        unlink(endpoint);
    }
    bytes_free(&payload);
    return ok;
}

bool keeper_close(const char *endpoint) {
    bool stale = false;
    return request(endpoint, SESSION_PURPOSE_CLOSE, SESSION_FRAME_ENDED, NULL, &stale);
}

#ifdef __linux__
static bool command_on_path(const char *name) {
    const char *path = getenv("PATH");
    char candidate[PATH_MAX];
    for (const char *start = path; start && *start;) {
        const char *end = strchr(start, ':');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length > 0 && length + strlen(name) + 2 < sizeof(candidate)) {
            snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, start, name);
            if (access(candidate, X_OK) == 0) return true;
        }
        start = end ? end + 1 : NULL;
    }
    return false;
}
#endif

// Under systemd, stopping a service kills every process in its control group.
// Starting the keeper in a scope of its own lets sessions outlive the backend.
static bool use_systemd_scope(void) {
#ifdef __linux__
    if (!command_on_path("systemd-run")) return false;
    int fd = open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char text[4096];
    ssize_t count = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (count <= 0) return false;
    text[count] = '\0';
    return strstr(text, ".service") != NULL;
#else
    return false;
#endif
}

static bool spawn(const char *const *arguments, bool scoped, const char *id) {
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        // setsid also leaves the process group that launchd stops with the backend.
        setsid();
        pid_t keeper = fork();
        if (keeper == 0) {
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, STDIN_FILENO);
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
            }
            long limit = sysconf(_SC_OPEN_MAX);
            for (int fd = 3; fd < (limit > 0 && limit < 4096 ? limit : 4096); fd++) close(fd);
            const char *argv[64];
            size_t count = 0;
            char unit[96];
            if (scoped) {
                snprintf(unit, sizeof(unit), "--unit=org.outershell.Terminal.session-%s", id);
                argv[count++] = "systemd-run";
                if (getuid() != 0) argv[count++] = "--user";
                argv[count++] = "--scope";
                argv[count++] = "--quiet";
                argv[count++] = "--collect";
                argv[count++] = unit;
                argv[count++] = "--";
            }
            for (size_t i = 0; arguments[i] && count + 1 < sizeof(argv) / sizeof(argv[0]); i++) argv[count++] = arguments[i];
            argv[count] = NULL;
            if (scoped) execvp("systemd-run", (char *const *)argv);
            else execv(argv[0], (char *const *)argv);
            _exit(127);
        }
        _exit(keeper > 0 ? 0 : 1);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool keeper_start(const char *executable, const char *const *command, const char *cwd, const char *place,
                  const char *command_name, uint16_t columns, uint16_t rows, char id[17]) {
    char endpoint[1024];
    if (!random_hex(id, 8) || !socket_path_for(id, endpoint, sizeof(endpoint))) return false;
    char column_text[16];
    char row_text[16];
    snprintf(column_text, sizeof(column_text), "%u", columns);
    snprintf(row_text, sizeof(row_text), "%u", rows);
    const char *arguments[80] = {executable, "--keeper", "--id", id, "--columns", column_text, "--rows", row_text, "--cwd", cwd,
                                 "--place", place, "--command-name", command_name, "--"};
    size_t count = 15;
    for (size_t i = 0; command[i] && count + 1 < sizeof(arguments) / sizeof(arguments[0]); i++) arguments[count++] = command[i];
    arguments[count] = NULL;
    bool scoped = use_systemd_scope();
    for (int round = 0; round < 2; round++) {
        if (!spawn(arguments, scoped, id)) return false;
        for (int attempt = 0; attempt < (scoped ? 40 : 100); attempt++) {
            int fd = keeper_connect(endpoint);
            if (fd >= 0) {
                close(fd);
                return true;
            }
            usleep(50000);
        }
        if (!scoped) break;
        scoped = false;
    }
    return false;
}

typedef struct {
    int fd;
    SessionFrameReader reader;
    Bytes outgoing;
    size_t sent;
    bool attached;
    bool close_after_flush;
} Client;

static void flush_client(Client *client) {
    while (client->sent < client->outgoing.size) {
        ssize_t written = send(client->fd, client->outgoing.data + client->sent, client->outgoing.size - client->sent, SEND_FLAGS);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            client->close_after_flush = true;
            break;
        }
        client->sent += (size_t)written;
    }
    client->outgoing.size = 0;
    client->sent = 0;
}

static void queue_client(Client *client, const Bytes *frames) {
    if (client->outgoing.size - client->sent > 8u * 1024u * 1024u || !bytes_append(&client->outgoing, frames->data, frames->size)) {
        client->close_after_flush = true;
        client->outgoing.size = client->sent = 0;
    }
}

static void foreground(int master, pid_t child, char *command, size_t command_capacity, char *cwd, size_t cwd_capacity) {
    pid_t process = tcgetpgrp(master);
    if (process <= 0) process = child;
#ifdef __APPLE__
    if (proc_name(process, command, (uint32_t)command_capacity) <= 0 && process != child) {
        process = child;
        proc_name(process, command, (uint32_t)command_capacity);
    }
    struct proc_vnodepathinfo paths;
    memset(&paths, 0, sizeof(paths));
    if (proc_pidinfo(process, PROC_PIDVNODEPATHINFO, 0, &paths, sizeof(paths)) == (int)sizeof(paths)) {
        snprintf(cwd, cwd_capacity, "%s", paths.pvi_cdir.vip_path);
    }
#else
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)process);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t count = read(fd, command, command_capacity - 1);
        close(fd);
        if (count > 0) {
            command[count] = '\0';
            command[strcspn(command, "\n")] = '\0';
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/cwd", (int)process);
    ssize_t length = readlink(path, cwd, cwd_capacity - 1);
    if (length > 0) cwd[length] = '\0';
#endif
}

static int64_t now_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int keeper_main(int argc, char **argv) {
    const char *id = "";
    const char *cwd = "";
    const char *place = "";
    const char *command_name = "";
    unsigned columns = 80;
    unsigned rows = 24;
    char **command = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            command = argv + i + 1;
            break;
        }
        if (i + 1 >= argc) return 2;
        if (!strcmp(argv[i], "--id")) id = argv[++i];
        else if (!strcmp(argv[i], "--cwd")) cwd = argv[++i];
        else if (!strcmp(argv[i], "--place")) place = argv[++i];
        else if (!strcmp(argv[i], "--command-name")) command_name = argv[++i];
        else if (!strcmp(argv[i], "--columns")) columns = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--rows")) rows = (unsigned)strtoul(argv[++i], NULL, 10);
        else return 2;
    }
    if (!session_valid_id(id) || !command || !command[0] || columns == 0 || columns > 1000 || rows == 0 || rows > 1000) return 2;
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    char socket_path[1024];
    if (!socket_path_for(id, socket_path, sizeof(socket_path))) return 1;
    int existing = keeper_connect(socket_path);
    if (existing >= 0) {
        close(existing);
        return 1;
    }
    unlink(socket_path);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    mode_t previous_mask = umask(0177);
    bool bound = bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(listener, 16) == 0;
    umask(previous_mask);
    if (!bound) return 1;
    fcntl(listener, F_SETFL, fcntl(listener, F_GETFL) | O_NONBLOCK);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    const char *slave_name = master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0 ? ptsname(master) : NULL;
    if (!slave_name) {
        unlink(socket_path);
        return 1;
    }
    char slave_path[PATH_MAX];
    snprintf(slave_path, sizeof(slave_path), "%s", slave_name);
    struct winsize size;
    memset(&size, 0, sizeof(size));
    size.ws_col = (unsigned short)columns;
    size.ws_row = (unsigned short)rows;
    pid_t child = fork();
    if (child < 0) {
        unlink(socket_path);
        return 1;
    }
    if (child == 0) {
        setsid();
        int slave = open(slave_path, O_RDWR);
        if (slave < 0) _exit(127);
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        ioctl(slave, TIOCSWINSZ, &size);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);
        close(master);
        close(listener);
        signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        setenv("OUTERSHELL_TERMINAL_SESSION", id, 1);
        add_outerctl_to_path();
        if (cwd[0] && chdir(cwd) != 0 && getenv("HOME") && chdir(getenv("HOME")) != 0) {
        }
        execvp(command[0], command);
        fprintf(stderr, "Could not run %s: %s\n", command[0], strerror(errno));
        _exit(127);
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    ioctl(master, TIOCSWINSZ, &size);

    SessionTerminal *terminal = session_terminal_create();
    if (!terminal) return 1;
    Client clients[SESSION_MAX_CLIENTS];
    size_t client_count = 0;
    Bytes input = {0};
    size_t input_sent = 0;
    int64_t created = (int64_t)time(NULL);
    int64_t active = created;
    bool master_open = true;
    bool child_running = true;
    int64_t restore_size_at = 0;
    int64_t kill_at = 0;
    char buffer[65536];
    while (child_running && master_open) {
        struct pollfd fds[SESSION_MAX_CLIENTS + 2];
        fds[0] = (struct pollfd){listener, POLLIN, 0};
        fds[1] = (struct pollfd){master, (short)(POLLIN | (input.size > input_sent ? POLLOUT : 0)), 0};
        size_t polled = client_count;
        for (size_t i = 0; i < polled; i++) {
            fds[i + 2] = (struct pollfd){clients[i].fd, (short)(POLLIN | (clients[i].outgoing.size > clients[i].sent ? POLLOUT : 0)), 0};
        }
        int64_t now = now_milliseconds();
        int timeout = 1000;
        if (restore_size_at) timeout = restore_size_at > now ? (int)(restore_size_at - now) : 0;
        if (poll(fds, (nfds_t)(polled + 2), timeout) < 0 && errno != EINTR) break;
        now = now_milliseconds();
        if (restore_size_at && now >= restore_size_at) {
            ioctl(master, TIOCSWINSZ, &size);
            restore_size_at = 0;
        }
        if (kill_at && now >= kill_at) {
            kill(-child, SIGKILL);
            kill_at = 0;
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t count = read(master, buffer, sizeof(buffer));
            if (count > 0) {
                session_terminal_record(terminal, buffer, (size_t)count);
                active = (int64_t)time(NULL);
                Bytes frame = {0};
                session_append_frame(&frame, SESSION_FRAME_OUTPUT, buffer, (size_t)count);
                for (size_t i = 0; i < client_count; i++) {
                    if (clients[i].attached) queue_client(&clients[i], &frame);
                }
                bytes_free(&frame);
            } else if (count == 0 || (errno != EAGAIN && errno != EINTR)) {
                master_open = false;
            }
        }
        if ((fds[1].revents & POLLOUT) && input.size > input_sent) {
            ssize_t written = write(master, input.data + input_sent, input.size - input_sent);
            if (written > 0) input_sent += (size_t)written;
            if (input_sent == input.size) input.size = input_sent = 0;
        }
        if (fds[0].revents & POLLIN) {
            int fd = accept(listener, NULL, NULL);
            if (fd >= 0 && client_count < SESSION_MAX_CLIENTS) {
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
                memset(&clients[client_count], 0, sizeof(Client));
                clients[client_count].fd = fd;
                client_count++;
            } else if (fd >= 0) {
                close(fd);
            }
        }
        for (size_t i = 0; i < client_count; i++) {
            Client *client = &clients[i];
            short events = i < polled ? fds[i + 2].revents : 0;
            if (events & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t count = read(client->fd, buffer, sizeof(buffer));
                if (count > 0 && bytes_append(&client->reader.pending, buffer, (size_t)count)) {
                    uint8_t type;
                    const char *data;
                    uint32_t length;
                    int next;
                    while ((next = session_next_frame(&client->reader, &type, &data, &length)) == 1) {
                        Bytes frame = {0};
                        uint8_t purpose = 0;
                        SessionAttachRequest attach;
                        uint16_t new_columns, new_rows;
                        if (type == SESSION_FRAME_HELLO && session_parse_hello(data, length, &purpose, &attach)) {
                            if (purpose == SESSION_PURPOSE_ATTACH || purpose == SESSION_PURPOSE_RESUME) {
                                bool resumed = session_terminal_attach(terminal, &attach, &frame);
                                queue_client(client, &frame);
                                client->attached = true;
                                bool same_size = attach.columns == size.ws_col && attach.rows == size.ws_row;
                                size.ws_col = attach.columns;
                                size.ws_row = attach.rows;
                                if (!resumed && same_size && attach.columns > 2) {
                                    // Programs redraw on a size change; a full
                                    // replay needs that to repaint cleanly.
                                    struct winsize nudge = size;
                                    nudge.ws_col = (unsigned short)(attach.columns - 1);
                                    ioctl(master, TIOCSWINSZ, &nudge);
                                    restore_size_at = now + 150;
                                } else if (!same_size) {
                                    ioctl(master, TIOCSWINSZ, &size);
                                }
                            } else if (purpose == SESSION_PURPOSE_INFO) {
                                SessionInfo info;
                                memset(&info, 0, sizeof(info));
                                snprintf(info.id, sizeof(info.id), "%s", id);
                                snprintf(info.title, sizeof(info.title), "%s", session_terminal_title(terminal));
                                if (place[0]) {
                                    snprintf(info.place, sizeof(info.place), "%s", place);
                                    snprintf(info.command, sizeof(info.command), "%s", command_name);
                                } else {
                                    foreground(master, child, info.command, sizeof(info.command), info.cwd, sizeof(info.cwd));
                                    if (!info.command[0]) snprintf(info.command, sizeof(info.command), "%s", base_name(command[0]));
                                    if (!info.cwd[0]) snprintf(info.cwd, sizeof(info.cwd), "%s", cwd);
                                }
                                info.created = created;
                                info.active = active;
                                for (size_t j = 0; j < client_count; j++) info.clients += clients[j].attached ? 1 : 0;
                                session_append_info(&frame, &info);
                                queue_client(client, &frame);
                                client->close_after_flush = true;
                            } else if (purpose == SESSION_PURPOSE_CLOSE) {
                                client->attached = true;
                                kill(-child, SIGHUP);
                                kill(child, SIGHUP);
                                kill_at = now + 3000;
                            }
                        } else if (type == SESSION_FRAME_INPUT && client->attached) {
                            if (input.size - input_sent + length <= 1024u * 1024u) bytes_append(&input, data, length);
                            active = (int64_t)time(NULL);
                        } else if (type == SESSION_FRAME_RESIZE && client->attached && session_parse_size(data, length, &new_columns, &new_rows)) {
                            size.ws_col = new_columns;
                            size.ws_row = new_rows;
                            if (!restore_size_at) ioctl(master, TIOCSWINSZ, &size);
                        }
                        bytes_free(&frame);
                    }
                    if (next < 0) client->close_after_flush = true;
                } else if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) {
                    client->attached = false;
                    client->close_after_flush = true;
                    client->outgoing.size = client->sent = 0;
                }
            }
            flush_client(client);
        }
        for (size_t i = 0; i < client_count;) {
            if (clients[i].close_after_flush && clients[i].outgoing.size == clients[i].sent) {
                close(clients[i].fd);
                bytes_free(&clients[i].reader.pending);
                bytes_free(&clients[i].outgoing);
                clients[i] = clients[--client_count];
            } else {
                i++;
            }
        }
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child) child_running = false;
    }
    unlink(socket_path);
    close(listener);
    ssize_t count;
    while (master_open && (count = read(master, buffer, sizeof(buffer))) > 0) {
        Bytes frame = {0};
        session_append_frame(&frame, SESSION_FRAME_OUTPUT, buffer, (size_t)count);
        for (size_t i = 0; i < client_count; i++) {
            if (clients[i].attached) queue_client(&clients[i], &frame);
        }
        bytes_free(&frame);
    }
    kill(-child, SIGHUP);
    Bytes ended = {0};
    session_append_frame(&ended, SESSION_FRAME_ENDED, NULL, 0);
    for (size_t i = 0; i < client_count; i++) {
        fcntl(clients[i].fd, F_SETFL, fcntl(clients[i].fd, F_GETFL) & ~O_NONBLOCK);
        if (clients[i].attached) {
            write_all(clients[i].fd, clients[i].outgoing.data + clients[i].sent, clients[i].outgoing.size - clients[i].sent);
            write_all(clients[i].fd, ended.data, ended.size);
        }
        close(clients[i].fd);
    }
    bytes_free(&ended);
    close(master);
    return 0;
}
