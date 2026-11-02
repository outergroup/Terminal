// Terminal: a list of persistent terminal sessions, each shown in its own
// xterm.js terminal. A terminal keeps its screen while the page is open and
// tracks how much of its session's output it has shown, so after a lost
// connection it reconnects and receives only the output it missed.
(() => {
  "use strict";

  const list = document.querySelector(".session-list");
  const terminals = document.querySelector(".terminals");
  const status = document.querySelector(".status");
  const encoder = new TextEncoder();
  const views = new Map();
  let sessions = [];
  let shells = [];
  let home = "";
  // A link to #new=PLACE[&command=COMMAND] starts a session in a container,
  // running its login program or a command registered in its image; Outer
  // Shell links here from a container's apps.
  let pendingNew = null;
  let selected = "";
  readHash();
  let creating = null;
  let createError = "";
  let fontSize = 13;
  try { fontSize = clampFontSize(Number(localStorage.getItem("fontSize")) || fontSize); } catch {}

  const unescapeField = value => value.replace(/\\(.)/g, (_, c) => c === "t" ? "\t" : c === "n" ? "\n" : c === "r" ? "\r" : c);

  function readHash() {
    const hash = location.hash.slice(1);
    if (hash.startsWith("new=")) {
      const parameters = new URLSearchParams(hash);
      pendingNew = { place: parameters.get("new") || "", command: parameters.get("command") || "" };
      history.replaceState(null, "", location.pathname + location.search);
    } else {
      selected = decodeURIComponent(hash);
    }
  }

  function parseSessions(text) {
    const lines = text.split("\n").filter(Boolean);
    const columns = (lines.shift() || "").split("\t");
    return lines.map(line => {
      const fields = line.split("\t").map(unescapeField);
      const row = Object.fromEntries(columns.map((name, index) => [name, fields[index] ?? ""]));
      for (const name of ["created", "active", "clients"]) row[name] = Number(row[name]) || 0;
      return row;
    });
  }

  async function refresh() {
    try {
      const response = await fetch("/api/sessions", { cache: "no-store" });
      if (!response.ok) throw new Error(await response.text());
      home = unescapeField(response.headers.get("Terminal-Home") || "");
      const shellFields = (response.headers.get("Terminal-Shells") || "").split("\t").filter(Boolean);
      shells = [];
      for (let i = 0; i + 1 < shellFields.length; i += 2) shells.push({ kind: shellFields[i], name: shellFields[i + 1] });
      // Sessions are listed by recent activity when the page opens, then keep
      // their places so that rows don't move under a finger; new ones go first.
      const previous = sessions.map(session => session.id);
      sessions = parseSessions(await response.text());
      sessions.sort((a, b) => previous.indexOf(a.id) - previous.indexOf(b.id) || b.active - a.active || a.created - b.created);
      for (const [id, view] of views) {
        if (!view.ended && !sessions.some(session => session.id === id)) dropView(id);
      }
      if (pendingNew && !creating) {
        const request = pendingNew;
        pendingNew = null;
        await createSession(request);
        return;
      }
      if (!sessions.length && !creating) {
        await createSession();
        return;
      }
      if (!sessions.some(session => session.id === selected)) selected = sessions[0] ? sessions[0].id : "";
      if (selected && !views.has(selected)) select(selected);
      else render();
      status.textContent = createError;
    } catch (error) {
      status.textContent = "Could not reach Terminal. Retrying…";
    }
  }

  function describeLocation(cwd) {
    if (home && (cwd === home || cwd.startsWith(home + "/"))) return "~" + cwd.slice(home.length);
    return cwd;
  }

  function describeAge(active) {
    const seconds = Math.max(0, Date.now() / 1000 - active);
    if (seconds < 60) return "active now";
    if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)} hr ago`;
    if (seconds < 172800) return "yesterday";
    return `${Math.floor(seconds / 86400)} days ago`;
  }

  function sessionName(session) {
    const view = views.get(session.id);
    const title = (view && view.title) || session.title;
    return title && title !== session.command ? title : session.command || session.place || "Terminal";
  }

  // Rows are updated in place: on touch screens, a tap is dropped when the
  // content under it is replaced, and busy sessions retitle themselves often.
  const rows = new Map();

  function makeRow(id) {
    const item = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "session";
    const name = document.createElement("span");
    name.className = "session-name";
    name.append("");
    const detail = document.createElement("span");
    detail.className = "session-detail";
    detail.append("");
    button.append(name, detail);
    button.addEventListener("click", () => select(id));
    const close = document.createElement("button");
    close.type = "button";
    close.className = "close-session";
    close.textContent = "×";
    close.title = "Close terminal";
    close.addEventListener("click", () => {
      const session = sessions.find(candidate => candidate.id === id);
      if (session) closeSession(session);
    });
    item.append(button, close);
    return { item, button, name: name.firstChild, detail: detail.firstChild, close };
  }

  function setText(node, text) {
    if (node.data !== text) node.data = text;
  }

  function setAttribute(element, name, value) {
    if (element.getAttribute(name) !== value) element.setAttribute(name, value);
  }

  function render() {
    const items = sessions.map(session => {
      let row = rows.get(session.id);
      if (!row) {
        row = makeRow(session.id);
        rows.set(session.id, row);
      }
      const current = session.id === selected;
      setAttribute(row.item, "class", "session-item" + (current ? " current" : ""));
      setAttribute(row.button, "aria-current", String(current));
      setText(row.name, sessionName(session));
      const view = views.get(session.id);
      const elsewhere = session.clients - (view && view.attached ? 1 : 0);
      setText(row.detail, [session.place ? `in ${session.place}` : describeLocation(session.cwd), describeAge(session.active),
        elsewhere > 0 ? "open elsewhere" : ""]
        .filter(Boolean).join(" · "));
      setAttribute(row.close, "aria-label", `Close ${sessionName(session)}`);
      return row.item;
    });
    for (const id of rows.keys()) {
      if (!sessions.some(session => session.id === id)) rows.delete(id);
    }
    if (items.length !== list.children.length || items.some((item, index) => list.children[index] !== item)) {
      list.replaceChildren(...items);
    }
  }

  function select(id) {
    selected = id || "";
    history.replaceState(null, "", selected ? `#${selected}` : location.pathname);
    for (const [viewID, view] of views) view.element.hidden = viewID !== selected;
    if (selected) {
      const view = views.get(selected) || openView(selected);
      view.element.hidden = false;
      requestAnimationFrame(() => {
        view.fit.fit();
        view.terminal.focus();
      });
    }
    render();
  }

  // Starts a session on this computer, optionally with one of its shells, or
  // in a container (place), optionally running a command registered there.
  async function createSession({ shell = "", place = "", command = "" } = {}) {
    if (creating) return creating;
    creating = (async () => {
      const size = measureSize();
      const query = new URLSearchParams({ columns: size.columns, rows: size.rows });
      if (shell) query.set("shell", shell);
      if (place) query.set("place", place);
      if (command) query.set("command", command);
      const response = await fetch(`/api/sessions?${query}`, { method: "POST" });
      if (!response.ok) throw new Error((await response.text()).trim());
      const id = (await response.text()).trim();
      selected = id;
      await refresh();
      select(id);
    })();
    try { await creating; } catch (error) { createError = status.textContent = error.message || "Could not start a terminal."; }
    finally { creating = null; }
  }

  async function closeSession(session) {
    if (!confirm(`Close “${sessionName(session)}”? Anything running in it will stop.`)) return;
    await fetch(`/api/sessions/${encodeURIComponent(session.id)}/close`, { method: "POST" });
    dropView(session.id);
    await refresh();
  }

  function clampFontSize(size) {
    return Math.min(32, Math.max(8, Math.round(size)));
  }

  function setFontSize(size) {
    size = clampFontSize(size);
    if (size === fontSize) return;
    fontSize = size;
    for (const view of views.values()) view.terminal.options.fontSize = size;
    const view = views.get(selected);
    if (view) view.fit.fit();
    try { localStorage.setItem("fontSize", String(size)); } catch {}
  }

  function measureSize() {
    const current = views.get(selected);
    if (current) return { columns: current.terminal.cols, rows: current.terminal.rows };
    const columns = Math.max(20, Math.floor((terminals.clientWidth - 16) / (fontSize * 0.6)));
    const rows = Math.max(5, Math.floor((terminals.clientHeight - 8) / (fontSize * 1.3)));
    return { columns, rows };
  }

  function dropView(id) {
    const view = views.get(id);
    if (!view) return;
    view.ended = true;
    clearTimeout(view.timer);
    if (view.socket) view.socket.close();
    view.terminal.dispose();
    view.element.remove();
    views.delete(id);
  }

  function openView(id) {
    const element = document.createElement("div");
    element.className = "terminal-view";
    terminals.append(element);
    const terminal = new Terminal({
      cursorBlink: true,
      fontFamily: 'ui-monospace, "SF Mono", Menlo, Consolas, "DejaVu Sans Mono", monospace',
      fontSize,
      scrollback: 10000,
      allowProposedApi: true,
      theme: { background: "#161616" },
    });
    const fit = new FitAddon.FitAddon();
    terminal.loadAddon(fit);
    terminal.open(element);
    const view = {
      id, element, terminal, fit,
      socket: null, position: null, attached: false, ended: false,
      delay: 500, timer: null, notice: null, title: "",
    };
    views.set(id, view);
    const send = bytes => {
      if (view.socket && view.socket.readyState === WebSocket.OPEN) view.socket.send(bytes);
    };
    terminal.onData(data => {
      const text = encoder.encode(data);
      const message = new Uint8Array(text.length + 1);
      message[0] = 0x49;
      message.set(text, 1);
      send(message);
    });
    terminal.onBinary(data => {
      const message = new Uint8Array(data.length + 1);
      message[0] = 0x49;
      for (let i = 0; i < data.length; i += 1) message[i + 1] = data.charCodeAt(i) & 0xff;
      send(message);
    });
    terminal.onResize(({ cols, rows }) => send(sizeMessage(cols, rows)));
    terminal.onTitleChange(title => {
      view.title = title;
      render();
    });
    fit.fit();
    connect(view);
    return view;
  }

  function sizeMessage(columns, rows) {
    const message = new Uint8Array(5);
    message[0] = 0x52;
    new DataView(message.buffer).setUint16(1, columns, true);
    new DataView(message.buffer).setUint16(3, rows, true);
    return message;
  }

  function showNotice(view, text) {
    if (text && !view.notice) {
      view.notice = document.createElement("div");
      view.notice.className = "notice";
      view.element.append(view.notice);
    }
    if (view.notice) {
      if (text) view.notice.textContent = text;
      else {
        view.notice.remove();
        view.notice = null;
      }
    }
  }

  function connect(view) {
    clearTimeout(view.timer);
    view.timer = null;
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    const from = view.position === null ? "" : `&from=${view.position}`;
    const url = `${scheme}//${location.host}/api/sessions/${encodeURIComponent(view.id)}/attach` +
      `?columns=${view.terminal.cols}&rows=${view.terminal.rows}${from}`;
    const socket = new WebSocket(url);
    socket.binaryType = "arraybuffer";
    view.socket = socket;
    socket.onopen = () => {
      view.delay = 500;
      view.attached = true;
      showNotice(view, "");
    };
    socket.onmessage = event => {
      const bytes = new Uint8Array(event.data);
      if (bytes[0] === 0x4f) {
        const output = bytes.subarray(1);
        view.terminal.write(output);
        if (view.position !== null) view.position += output.length;
      } else if (bytes[0] === 0x50 && bytes.length === 9) {
        view.position = Number(new DataView(bytes.buffer, bytes.byteOffset + 1, 8).getBigUint64(0, true));
      } else if (bytes[0] === 0x58) {
        view.ended = true;
      }
    };
    socket.onclose = () => {
      if (socket !== view.socket) return;
      view.socket = null;
      view.attached = false;
      if (view.ended) {
        view.terminal.write("\r\n\x1b[2m[This terminal has ended.]\x1b[0m\r\n");
        sessions = sessions.filter(session => session.id !== view.id);
        setTimeout(() => {
          dropView(view.id);
          refresh();
        }, 800);
        return;
      }
      showNotice(view, "Reconnecting…");
      scheduleReconnect(view);
    };
  }

  function scheduleReconnect(view) {
    if (view.ended || view.socket || view.timer || document.visibilityState !== "visible") return;
    view.timer = setTimeout(() => connect(view), view.delay);
    view.delay = Math.min(view.delay * 2, 5000);
  }

  function reconnectAll() {
    for (const view of views.values()) {
      if (!view.socket && !view.ended) {
        view.delay = 500;
        clearTimeout(view.timer);
        view.timer = null;
        connect(view);
      }
    }
    refresh();
  }

  // "+" offers this computer, or each of its shells where there are several
  // (Windows), and each running container registered with Outer Shell.
  const newButton = document.querySelector(".new-session");
  let shellMenu = null;
  function closeShellMenu() {
    if (shellMenu) shellMenu.remove();
    shellMenu = null;
    newButton.setAttribute("aria-expanded", "false");
  }
  async function loadPlaces() {
    try {
      const response = await fetch("/api/places", { cache: "no-store" });
      return response.ok ? parseSessions(await response.text()) : [];
    } catch {
      return [];
    }
  }
  newButton.addEventListener("click", async event => {
    event.stopPropagation();
    createError = status.textContent = "";
    if (shellMenu) {
      closeShellMenu();
      return;
    }
    const places = await loadPlaces();
    if (shells.length < 2 && !places.length) {
      createSession();
      return;
    }
    shellMenu = document.createElement("div");
    shellMenu.className = "shell-menu";
    shellMenu.setAttribute("role", "menu");
    const addItem = (label, options) => {
      const item = document.createElement("button");
      item.type = "button";
      item.setAttribute("role", "menuitem");
      item.textContent = label;
      item.addEventListener("click", () => {
        closeShellMenu();
        createSession(options);
      });
      shellMenu.append(item);
    };
    const addHeading = label => {
      const heading = document.createElement("div");
      heading.className = "menu-heading";
      heading.textContent = label;
      shellMenu.append(heading);
    };
    if (places.length) addHeading("This computer");
    if (shells.length < 2) addItem(places.length ? "New terminal" : "Terminal", {});
    for (const shell of shells.length < 2 ? [] : shells) addItem(shell.name, { shell: shell.kind });
    if (places.length) addHeading("Containers");
    for (const place of places) addItem(place.name, { place: place.id });
    newButton.parentElement.append(shellMenu);
    newButton.setAttribute("aria-expanded", "true");
    shellMenu.querySelector("button").focus();
  });
  document.addEventListener("pointerover", event => {
    document.documentElement.classList.toggle("mouse", event.pointerType === "mouse");
  }, true);
  document.addEventListener("click", closeShellMenu);
  document.addEventListener("keydown", event => {
    if (event.key === "Escape") closeShellMenu();
  });
  // Pinching changes the font size rather than zooming the page.
  let pinchFontSize = null;
  document.addEventListener("gesturestart", event => {
    event.preventDefault();
    pinchFontSize = fontSize;
  });
  document.addEventListener("gesturechange", event => {
    event.preventDefault();
    if (pinchFontSize !== null) setFontSize(pinchFontSize * event.scale);
  });
  document.addEventListener("gestureend", event => {
    event.preventDefault();
    pinchFontSize = null;
  });
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible") reconnectAll();
  });
  window.addEventListener("online", reconnectAll);
  window.addEventListener("pageshow", reconnectAll);
  window.addEventListener("hashchange", () => {
    if (location.hash.startsWith("#new=")) {
      readHash();
      refresh();
      return;
    }
    const id = decodeURIComponent(location.hash.slice(1));
    if (id && id !== selected) select(id);
  });
  // An on-screen keyboard covers the page rather than resizing it. The page
  // keeps its size so the keyboard slides over it, and the terminal moves its
  // bottom edge above the keyboard.
  function fitToVisualViewport() {
    const viewport = window.visualViewport;
    const covered = Math.max(0, document.documentElement.clientHeight - viewport.height - viewport.offsetTop);
    document.documentElement.style.setProperty("--keyboard-inset", `${covered}px`);
    if (viewport.offsetTop || window.scrollY) window.scrollTo(0, 0);
  }
  if (window.visualViewport) {
    visualViewport.addEventListener("resize", fitToVisualViewport);
    visualViewport.addEventListener("scroll", fitToVisualViewport);
    fitToVisualViewport();
  }
  new ResizeObserver(() => {
    const view = views.get(selected);
    if (view) view.fit.fit();
  }).observe(terminals);
  setInterval(() => {
    if (document.visibilityState === "visible") refresh();
  }, 5000);
  refresh();
})();
