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
  let selected = decodeURIComponent(location.hash.slice(1));
  let creating = null;

  const unescapeField = value => value.replace(/\\(.)/g, (_, c) => c === "t" ? "\t" : c === "n" ? "\n" : c === "r" ? "\r" : c);

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
      sessions = parseSessions(await response.text());
      sessions.sort((a, b) => b.active - a.active || a.created - b.created);
      for (const [id, view] of views) {
        if (!view.ended && !sessions.some(session => session.id === id)) dropView(id);
      }
      if (!sessions.length && !creating) {
        await createSession();
        return;
      }
      if (!sessions.some(session => session.id === selected)) selected = sessions[0] ? sessions[0].id : "";
      if (selected && !views.has(selected)) select(selected);
      else render();
      status.textContent = "";
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
    return title && title !== session.command ? title : session.command || "Terminal";
  }

  function render() {
    list.replaceChildren(...sessions.map(session => {
      const item = document.createElement("li");
      item.className = "session-item" + (session.id === selected ? " current" : "");
      const button = document.createElement("button");
      button.type = "button";
      button.className = "session";
      button.setAttribute("aria-current", String(session.id === selected));
      const name = document.createElement("span");
      name.className = "session-name";
      name.textContent = sessionName(session);
      const detail = document.createElement("span");
      detail.className = "session-detail";
      const view = views.get(session.id);
      const elsewhere = session.clients - (view && view.attached ? 1 : 0);
      detail.textContent = [describeLocation(session.cwd), describeAge(session.active), elsewhere > 0 ? "open elsewhere" : ""]
        .filter(Boolean).join(" · ");
      button.append(name, detail);
      button.addEventListener("click", () => select(session.id));
      const close = document.createElement("button");
      close.type = "button";
      close.className = "close-session";
      close.textContent = "×";
      close.title = "Close terminal";
      close.setAttribute("aria-label", `Close ${sessionName(session)}`);
      close.addEventListener("click", () => closeSession(session));
      item.append(button, close);
      return item;
    }));
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

  async function createSession(shell) {
    if (creating) return creating;
    creating = (async () => {
      const size = measureSize();
      const kind = shell ? `&shell=${encodeURIComponent(shell)}` : "";
      const response = await fetch(`/api/sessions?columns=${size.columns}&rows=${size.rows}${kind}`, { method: "POST" });
      if (!response.ok) throw new Error(await response.text());
      const id = (await response.text()).trim();
      selected = id;
      await refresh();
      select(id);
    })();
    try { await creating; } catch { status.textContent = "Could not start a terminal."; }
    finally { creating = null; }
  }

  async function closeSession(session) {
    if (!confirm(`Close “${sessionName(session)}”? Anything running in it will stop.`)) return;
    await fetch(`/api/sessions/${encodeURIComponent(session.id)}/close`, { method: "POST" });
    dropView(session.id);
    await refresh();
  }

  function measureSize() {
    const current = views.get(selected);
    if (current) return { columns: current.terminal.cols, rows: current.terminal.rows };
    const columns = Math.max(20, Math.floor((terminals.clientWidth - 16) / 7.8));
    const rows = Math.max(5, Math.floor((terminals.clientHeight - 8) / 17));
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
      fontSize: 13,
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

  // Where more than one shell is available (Windows), "+" offers a choice.
  const newButton = document.querySelector(".new-session");
  let shellMenu = null;
  function closeShellMenu() {
    if (shellMenu) shellMenu.remove();
    shellMenu = null;
    newButton.setAttribute("aria-expanded", "false");
  }
  newButton.addEventListener("click", event => {
    event.stopPropagation();
    if (shellMenu) {
      closeShellMenu();
      return;
    }
    if (shells.length < 2) {
      createSession();
      return;
    }
    shellMenu = document.createElement("div");
    shellMenu.className = "shell-menu";
    shellMenu.setAttribute("role", "menu");
    for (const shell of shells) {
      const item = document.createElement("button");
      item.type = "button";
      item.setAttribute("role", "menuitem");
      item.textContent = shell.name;
      item.addEventListener("click", () => {
        closeShellMenu();
        createSession(shell.kind);
      });
      shellMenu.append(item);
    }
    newButton.parentElement.append(shellMenu);
    newButton.setAttribute("aria-expanded", "true");
    shellMenu.querySelector("button").focus();
  });
  document.addEventListener("click", closeShellMenu);
  document.addEventListener("keydown", event => {
    if (event.key === "Escape") closeShellMenu();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible") reconnectAll();
  });
  window.addEventListener("online", reconnectAll);
  window.addEventListener("pageshow", reconnectAll);
  window.addEventListener("hashchange", () => {
    const id = decodeURIComponent(location.hash.slice(1));
    if (id && id !== selected) select(id);
  });
  new ResizeObserver(() => {
    const view = views.get(selected);
    if (view) view.fit.fit();
  }).observe(terminals);
  setInterval(() => {
    if (document.visibilityState === "visible") refresh();
  }, 5000);
  refresh();
})();
