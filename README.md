# Terminal

Terminal is Outer Shell's terminal app: a list of persistent terminal sessions
that you can open from any device. Sessions keep running when you close the
page, lose your connection, or the app restarts, and a page that comes back
resumes exactly where it left off. This repository holds the HTML version; an
outerframe version will follow.

## How it works

Each session runs in a keeper process, which is `TerminalBackend` started with
`--keeper`. The keeper owns the session's pseudoterminal (a pseudoconsole on
Windows), keeps recent output, and listens on a Unix socket or named pipe that
only the current user can open. Keepers start outside the app's service
(their own systemd scope on Linux, their own session on macOS, outside the
app's job object on Windows), so sessions outlive the backend. A session ends
when its program exits or someone closes it. `Backend/Session.h` describes the
protocol between the backend and keepers.

The backend serves the page and bridges each browser terminal to its keeper
over a WebSocket. The page keeps one xterm.js terminal per open session and
tracks its position in the session's output, so after a lost connection it
reconnects and the keeper sends only the output it missed. Reloading the page
reopens the same session with a replay of recent output. See the comment at the
top of `Backend/main.c` for the HTTP API and message formats, which use TSV and
binary frames rather than JSON.

On Windows, new sessions can run PowerShell (PowerShell 7 when installed) or
Command Prompt.

## Layout

```text
Backend/main.c         TerminalBackend for macOS and Linux
Backend/Keeper.c       session keepers for macOS and Linux
Backend/Session.c      the keeper protocol and output tracking, shared with Windows
Windows/               TerminalBackend for Windows, built by Outer Shell's Windows build
Resources/TerminalWeb/ the page, with xterm.js 5.5.0 and its fit addon (MIT)
Scripts/               icon rendering (app-icon.png is the apple.terminal symbol), release packaging and tests
app                    development tasks
```

## Develop

```bash
./app run
```

runs the backend on `127.0.0.1:7356` and prints a URL with a one-time token;
open that URL in a browser. Set `PORT` to use another port.

```bash
./app test
```

runs `Scripts/test_backend.py` against a fresh build. With `TERMINAL_SOCKET`
and `TERMINAL_RESTART` set, it tests an installed Terminal instead. On Windows,
`Scripts/test_windows_backend.ps1` tests a built `TerminalBackend.exe`.

## Deploy

```bash
./app deploy                      # this Mac
./app target "ssh you@server"     # or a Linux server
./app deploy
```

Deployment installs Terminal through Outer Shell like its other bundled apps
(`outerctl bundled-app install`). Linux deployments use a static musl build.

## Release

Outer Shell's app catalog installs Terminal from release archives:

1. Run `Scripts/build_linux_resources.sh` on each Linux architecture with
   glibc and with musl (`OUTER_APP_LINUX_LIBC=musl`).
2. Run `Scripts/package_release.sh` on a Mac. It builds the macOS backend and
   writes `build/release/Terminal/<variant>.tar.gz`.

The Windows backend is compiled into Outer Shell's Windows build, which expects
this repository next to Outer Shell's (`TERMINAL_ROOT`).
