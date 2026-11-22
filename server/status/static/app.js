// The status page (server/status/wwhd_status.py serves it behind a login). Every value from the server is put in as
// text (textContent / createTextNode), never as HTML: player names are the players' own text.
"use strict";
(function () {
  const SVGNS = "http://www.w3.org/2000/svg";
  const SQ = 100; // one sea square = 100 chart units = 100,000 game units
  const ORIGIN = -350000, SCALE = 1000;
  let last = null;

  const $ = (id) => document.getElementById(id);
  function el(tag, attrs, text) {
    const e = document.createElement(tag);
    for (const k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (text !== undefined) e.textContent = String(text);
    return e;
  }
  function svg(tag, attrs, text) {
    const e = document.createElementNS(SVGNS, tag);
    for (const k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (text !== undefined) e.textContent = String(text);
    return e;
  }
  function dur(s) {
    s = Math.max(0, Math.floor(s));
    const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
    if (d) return d + " d " + h + " h";
    if (h) return h + " h " + m + " min";
    if (m) return m + " min";
    return s + " s";
  }
  function bytes(n) {
    const u = ["B", "KB", "MB", "GB", "TB"];
    let i = 0;
    while (n >= 1000 && i < u.length - 1) { n /= 1000; i++; }
    return n.toFixed(i ? 1 : 0) + " " + u[i];
  }

  function tiles(d) {
    const t = $("tiles");
    t.replaceChildren();
    const lost = d.list.filter((p) => !p.online).length;
    const items = [
      ["Players online", d.players + " / " + d.max_players],
      ["At sea", d.list.filter((p) => p.at_sea && p.online).length],
      ["Lost (resuming)", lost],
      ["Parties", d.parties_active + (d.parties_kept ? " (+" + d.parties_kept + " kept)" : "")],
      ["Uptime", dur(d.time - d.started)],
      ["Traffic out / in", bytes(d.bytes_out) + " / " + bytes(d.bytes_in)],
      ["Refused / kicked", d.refused + " / " + d.kicked],
      ["Dropped (rate / bad)", d.dropped_rate + " / " + d.dropped_bad],
      ["Bans", d.bans],
      ["Server key", d.fingerprint, true],
    ];
    for (const [k, v, small] of items) {
      const b = el("div", { class: "tile" });
      b.append(el("div", { class: "k" }, k), el("div", { class: "v" + (small ? " small" : "") }, v));
      t.append(b);
    }
  }

  function chart(d) {
    const box = $("chart");
    const s = svg("svg", { viewBox: "0 0 700 700", role: "img", "aria-label": "Sea chart with players" });
    const perSq = {};
    for (const p of d.list) if (p.at_sea && p.square >= 0) perSq[p.square] = (perSq[p.square] || 0) + 1;
    for (let i = 0; i < 49; i++) {
      const x = (i % 7) * SQ, y = Math.floor(i / 7) * SQ;
      s.append(svg("rect", { class: "sq", x: x, y: y, width: SQ, height: SQ }));
      const name = (d.sea_names && d.sea_names[i]) || "";
      const words = name.split(" ");
      const half = Math.ceil(words.length / 2);
      const lines = words.length > 2 ? [words.slice(0, half).join(" "), words.slice(half).join(" ")] : [name];
      lines.forEach((ln, k) => s.append(svg("text", { class: "lbl", x: x + SQ / 2, y: y + SQ - 12 + (k - lines.length + 1) * 7.5 }, ln)));
      if (perSq[i]) s.append(svg("text", { class: "cnt", x: x + SQ - 5, y: y + 12 }, perSq[i]));
    }
    for (const p of d.list) {
      if (!p.at_sea || !p.has_pose) continue;
      const cx = (p.x - ORIGIN) / SCALE, cy = (p.z - ORIGIN) / SCALE;
      if (cx < 0 || cy < 0 || cx > 700 || cy > 700) continue;
      const c = svg("circle", { class: "p" + (p.online ? "" : " lost"), cx: cx.toFixed(1), cy: cy.toFixed(1), r: 4.5 });
      c.append(svg("title", {}, (p.name || p.code) + " — " + p.place + (p.flags & 2 ? " (sailing)" : "")));
      s.append(c);
    }
    box.replaceChildren(s);
  }

  function places(d) {
    const counts = {};
    for (const p of d.list) {
      const k = p.at_sea ? "The Great Sea" : p.place;
      counts[k] = (counts[k] || 0) + 1;
    }
    const ul = $("places");
    ul.replaceChildren();
    const rows = Object.entries(counts).sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]));
    if (!rows.length) ul.append(el("li", { class: "empty" }, "Nobody online"));
    for (const [k, n] of rows) {
      const li = el("li");
      li.append(el("span", {}, k), el("span", { class: "n" }, n));
      ul.append(li);
    }
  }

  function players(d) {
    const q = $("filter").value.trim().toLowerCase();
    const tb = $("players").tBodies[0];
    tb.replaceChildren();
    const list = d.list.slice().sort((a, b) => (b.online - a.online) || String(a.name).localeCompare(String(b.name)));
    let shown = 0;
    for (const p of list) {
      const hay = (p.name + " " + p.code + " " + p.place).toLowerCase();
      if (q && !hay.includes(q)) continue;
      shown++;
      const tr = el("tr", p.online ? {} : { class: "lost" });
      const where = p.place + (p.at_sea && p.flags & 2 ? " · sailing" : p.at_sea && p.flags & 1 ? " · boat" : "") +
        (p.copy ? " · copy " + p.copy : "");
      tr.append(
        el("td", {}, p.name || "—"),
        el("td", { class: "code" }, p.code),
        el("td", {}, where),
        el("td", {}, p.party || "—"),
        el("td", {}, p.online ? p.ping + " ms" + (p.tcp ? " (TCP)" : "") : "lost"),
        el("td", {}, p.device),
        el("td", {}, dur(p.online_s)),
        el("td", {}, p.pvp ? (p.safe ? "on · safe" : "on") : "off"));
      tb.append(tr);
    }
    if (!shown) {
      const tr = el("tr");
      tr.append(el("td", { colspan: 8 }, q ? "No match" : "Nobody online"));
      tb.append(tr);
    }
  }

  function parties(d) {
    const tb = $("parties").tBodies[0];
    tb.replaceChildren();
    for (const p of d.party_list) {
      const tr = el("tr");
      tr.append(el("td", {}, p.id), el("td", { class: "code" }, p.leader), el("td", {}, p.members),
        el("td", {}, p.names.join(", ")), el("td", {}, p.online ? "playing" : "nobody online (kept)"));
      tb.append(tr);
    }
    if (!d.party_list.length) {
      const tr = el("tr");
      tr.append(el("td", { colspan: 5 }, "No parties"));
      tb.append(tr);
    }
  }

  function render(d) {
    const st = $("state");
    if (!d.reachable) {
      st.textContent = "server down";
      st.className = "state down";
      $("foot").textContent = d.error + " · checked " + new Date(d.checked * 1000).toLocaleTimeString();
      return;
    }
    last = d;
    st.textContent = "running";
    st.className = "state up";
    $("srv-name").textContent = d.name;
    document.title = d.name + " · status";
    tiles(d);
    chart(d);
    places(d);
    players(d);
    parties(d);
    $("foot").textContent = "Updated " + new Date(d.time * 1000).toLocaleTimeString() + " · crypto " + d.crypto +
      " · scaling " + d.scale + (d.scale !== "off" ? " (this page shows the coordinator's own players only)" : "");
  }

  async function tick() {
    try {
      const r = await fetch("/api/status", { credentials: "same-origin", cache: "no-store" });
      if (r.status === 401) { location.href = "/login"; return; }
      render(await r.json());
    } catch (e) {
      const st = $("state");
      st.textContent = "page offline";
      st.className = "state down";
    }
  }
  $("filter").addEventListener("input", () => last && players(last));
  tick();
  setInterval(tick, 5000);
})();
