#include "Places.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "OuterShellPaths.h"

extern char **environ;

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool read_all(int fd, void *data, size_t size, int timeout_ms) {
    unsigned char *p = data;
    while (size) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= (size_t)n;
    }
    return true;
}

static int connect_unix(const char *path) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(address.sun_path)) return -1;
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (!connect(fd, (struct sockaddr *)&address, sizeof(address))) return fd;
    close(fd);
    return -1;
}

static unsigned char *registry_environments(size_t *size) {
    char path[PATH_MAX];
    outer_shell_default_api_socket_path(path, sizeof(path));
    int fd = connect_unix(path);
#ifdef __APPLE__
    // Outer Shell's socket moves when it restarts, so the path this process
    // inherited can be stale. launchd knows the current one.
    char error[512];
    if (fd < 0 && outer_launchd_resolve_with_key_prefix("/launchd/user/org.outershell.OuterShell",
            "OUTERSHELL_API_SOCKET_", path, sizeof(path), error, sizeof(error)))
        fd = connect_unix(path);
#endif
    if (fd < 0) return NULL;
    unsigned char *response = NULL;
    unsigned char header[4];
    if (write(fd, registry_containers_request, sizeof(registry_containers_request)) == (ssize_t)sizeof(registry_containers_request) &&
        read_all(fd, header, 4, 5000)) {
        *size = get_u32(header);
        response = *size >= 48 && *size <= 16u * 1024 * 1024 ? malloc(*size) : NULL;
        if (response && !read_all(fd, response, *size, 5000)) {
            free(response);
            response = NULL;
        }
    }
    close(fd);
    return response;
}

static bool find_executable(const char *name, char *out, size_t capacity) {
    const char *path = getenv("PATH");
    char directories[8192];
    snprintf(directories, sizeof(directories), "%s:/opt/homebrew/bin:/usr/local/bin:/usr/bin:%s/.local/bin:"
             "/Applications/Docker.app/Contents/Resources/bin", path ? path : "", getenv("HOME") ? getenv("HOME") : "");
    char *save = NULL;
    for (char *directory = strtok_r(directories, ":", &save); directory; directory = strtok_r(NULL, ":", &save)) {
        if (!*directory) continue;
        snprintf(out, capacity, "%s/%s", directory, name);
        struct stat st;
        if (!stat(out, &st) && S_ISREG(st.st_mode) && !access(out, X_OK)) return true;
    }
    return false;
}

bool places_list(Place **places, size_t *count) {
    *places = NULL;
    *count = 0;
    size_t size = 0;
    unsigned char *bytes = registry_environments(&size);
    RegistryContainer *containers = NULL;
    size_t records = 0;
    bool ok = bytes && registry_parse_containers(bytes, size, &containers, &records);
    free(bytes);
    *places = ok && records ? calloc(records, sizeof(Place)) : NULL;
    for (size_t i = 0; *places && i < records; i++) {
        const RegistryContainer *container = &containers[i];
        bool docker = !strcmp(container->runtime, "docker"), apple = !strcmp(container->runtime, "apple.container");
        Place *place = &(*places)[*count];
        if ((!docker && !apple) || strcmp(container->state, "running") ||
            !find_executable(docker ? "docker" : "container", place->executable, sizeof(place->executable)))
            continue;
        snprintf(place->id, sizeof(place->id), "%s", container->id);
        snprintf(place->name, sizeof(place->name), "%s", container->name);
        snprintf(place->container, sizeof(place->container), "%s", container->container);
        snprintf(place->endpoint, sizeof(place->endpoint), "%s", docker ? container->endpoint : "");
        (*count)++;
    }
    free(containers);
    return ok;
}

bool places_find(const char *id, Place *place) {
    Place *places = NULL;
    size_t count = 0;
    bool found = false;
    if (places_list(&places, &count))
        for (size_t i = 0; i < count && !found; i++)
            if (!strcasecmp(places[i].id, id)) {
                *place = places[i];
                found = true;
            }
    free(places);
    return found;
}

static size_t exec_prefix(const Place *place, bool terminal, const char **arguments, size_t capacity) {
    if (capacity < 8) return 0;
    size_t n = 0;
    arguments[n++] = place->executable;
    if (place->endpoint[0]) {
        arguments[n++] = "--host";
        arguments[n++] = place->endpoint;
    }
    arguments[n++] = "exec";
    arguments[n++] = terminal ? "-it" : "-i";
    return n;
}

size_t place_exec_command(const Place *place, const char *user, const char *directory,
                          const char *const *program, const char **arguments, size_t capacity) {
    size_t n = exec_prefix(place, true, arguments, capacity);
    const char *options[] = {"-e", "TERM=xterm-256color", "-e", "COLORTERM=truecolor",
                             user[0] ? "-u" : NULL, user, directory[0] ? "-w" : NULL, directory};
    for (size_t i = 0; n && i < sizeof(options) / sizeof(*options); i += 2) {
        if (!options[i]) continue;
        if (n + 2 >= capacity) return 0;
        arguments[n++] = options[i];
        arguments[n++] = options[i + 1];
    }
    if (!n || n + 1 >= capacity) return 0;
    arguments[n++] = place->container;
    for (size_t i = 0; program[i]; i++) {
        if (n + 1 >= capacity) return 0;
        arguments[n++] = program[i];
    }
    arguments[n] = NULL;
    return n;
}

// Runs a command and returns what it writes, up to `capacity` - 1 bytes.
static bool capture(const char *const *arguments, char *out, size_t capacity) {
    int output[2];
    if (pipe(output)) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    pid_t pid = 0;
    bool spawned = !posix_spawn(&pid, arguments[0], &actions, NULL, (char *const *)arguments, environ);
    bool ok = spawned;
    posix_spawn_file_actions_destroy(&actions);
    close(output[1]);
    size_t used = 0;
    while (ok) {
        struct pollfd pfd = {output[0], POLLIN, 0};
        if (poll(&pfd, 1, 15000) <= 0) {
            kill(pid, SIGKILL);
            ok = false;
            break;
        }
        ssize_t n = read(output[0], out + used, capacity - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
        if (used == capacity - 1) break;
    }
    close(output[0]);
    int status = 0;
    if (spawned) waitpid(pid, &status, 0);
    out[used] = '\0';
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool place_registered_command(const Place *place, const char *command_id, RegistryCommand *command) {
    if (!registry_valid_identifier(command_id)) return false;
    char path[512];
    registry_command_path(command_id, path, sizeof(path));
    const char *arguments[16];
    size_t n = exec_prefix(place, false, arguments, 16);
    arguments[n++] = place->container;
    arguments[n++] = "cat";
    arguments[n++] = path;
    arguments[n] = NULL;
    char text[8192];
    return capture(arguments, text, sizeof(text)) && registry_parse_command(text, command);
}
