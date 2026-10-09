const $ = id => document.getElementById(id);
const api = (path, opts) => fetch(path, opts).then(r => { if (!r.ok) throw new Error(r.status); return r.json(); });

const fmt = {
  mpg: v => v == null ? "—" : v.toFixed(1),
  mi: v => v == null ? "—" : v >= 100 ? Math.round(v).toLocaleString() : v.toFixed(1),
  gal: v => v == null ? "—" : v.toFixed(2),
  money: v => v == null ? "—" : "$" + v.toFixed(2),
  dur: s => { const m = Math.round(s / 60); return m >= 60 ? `${Math.floor(m / 60)}h ${String(m % 60).padStart(2, "0")}m` : `${m} min`; },
  clock: s => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`,
  date: ts => new Date(ts * 1000).toLocaleDateString(undefined, { month: "short", day: "numeric" }),
  when: ts => new Date(ts * 1000).toLocaleString(undefined, { weekday: "short", month: "short", day: "numeric", hour: "numeric", minute: "2-digit" }),
};

let summary = null;

// ---------- theme ----------
function applyTheme(t) { if (t) document.documentElement.dataset.theme = t; }
try { applyTheme(localStorage.getItem("carlog-theme")); } catch {}
$("themeBtn").onclick = () => {
  const dark = document.documentElement.dataset.theme
    ? document.documentElement.dataset.theme === "dark"
    : matchMedia("(prefers-color-scheme: dark)").matches;
  const next = dark ? "light" : "dark";
  applyTheme(next);
  try { localStorage.setItem("carlog-theme", next); } catch {}
  if (summary) render(summary);
};

// ---------- summary ----------
async function loadSummary() {
  summary = await api("/api/summary");
  render(summary);
}

function render(s) {
  $("carName").textContent = s.settings.car_name;
  $("carSub").textContent = s.odometer_mi
    ? `${Math.round(s.odometer_mi).toLocaleString()} mi on the odometer` : "fuel economy & telemetry";
  for (const form of ["fillForm", "maintForm"]) {
    const odoField = $(form).odometer_mi;
    if (s.odometer_mi && !odoField.value) odoField.value = Math.round(s.odometer_mi);
  }
  document.title = `${s.settings.car_name} · carlog`;
  const r = s.last30, life = s.lifetime;

  const hero = r.mpg ?? life.mpg;
  $("heroMpg").textContent = fmt.mpg(hero);
  $("heroSub").textContent = r.trips
    ? `${fmt.mi(r.miles)} mi · ${fmt.gal(r.gallons)} gal · ${r.trips} trip${r.trips === 1 ? "" : "s"}`
    : life.trips ? "No trips in the last 30 days — showing lifetime" : "No trips yet";
  const cal = s.calibration;
  $("calLine").innerHTML = `<span class="badge">${cal.source}</span><span>${cal.detail}${cal.source !== "uncalibrated" ? ` · ×${cal.factor.toFixed(3)}` : ""}</span>`;

  $("tLife").textContent = fmt.mpg(life.mpg);
  $("tLifeSub").textContent = life.trips ? `mpg over ${life.trips} trips` : "mpg";
  $("tMiles").textContent = fmt.mi(life.miles);
  $("tMilesSub").textContent = life.trips ? `${life.hours.toFixed(1)} hours driving` : "";
  $("tCost").textContent = fmt.money(life.cost);
  $("tCostSub").textContent = life.gallons ? `${fmt.gal(life.gallons)} gal at ${fmt.money(s.settings.fuel_price)}/gal` : "";
  $("tRange").textContent = s.range_miles ? `${Math.round(s.range_miles)} mi` : "—";
  $("tFuelBar").style.width = (s.fuel_level ?? 0) + "%";
  $("tRangeSub").textContent = s.fuel_level != null ? `${Math.round(s.fuel_level)}% tank` : "fuel level unknown";

  renderTrend(s.trips);
  renderBands(s.bands);
  renderInsights(s);
  renderTrips(s.trips);
}

function renderTrend(trips) {
  const ts = trips.filter(t => t.mpg != null && t.miles >= 0.3).slice().reverse();
  const roll = ts.map((_, i) => {
    const w = ts.slice(Math.max(0, i - 9), i + 1);
    const mi = w.reduce((a, t) => a + t.miles, 0), gal = w.reduce((a, t) => a + t.gallons, 0);
    return gal > 0 ? mi / gal : null;
  });
  Charts.lineChart($("trendChart"), {
    label: "MPG for each trip with a rolling average",
    x: ts.map(t => t.start_ts),
    series: [
      { name: "Trip", values: ts.map(t => Math.min(t.mpg, 80)), color: "--series-1", kind: "dot" },
      { name: "Rolling avg", values: roll, color: "--series-2", kind: "line" },
    ],
    fmtX: v => fmt.date(v),
    xTicks: (a, b, w) => Charts.dayTicks(a, b, w),
    fmtY: (v, s) => s.kind === "dot" ? fmt.mpg(ts.find(t => Math.min(t.mpg, 80) === v)?.mpg ?? v) + " mpg" : fmt.mpg(v) + " mpg",
    title: i => fmt.when(ts[i].start_ts),
    extraRows: i => [{ label: "Distance", value: fmt.mi(ts[i].miles) + " mi" }],
    onClick: i => openTrip(ts[i].id),
    empty: "Your trips will plot here",
  });
}

function renderBands(bands) {
  Charts.barChart($("bandChart"), {
    label: "Average MPG in each 10 mph speed band",
    labels: bands.map(b => b.label),
    values: bands.map(b => b.mpg),
    color: "--series-1",
    fmt: v => v.toFixed(0),
    tipTitle: i => `${bands[i].label} mph`,
    tip: i => [{ label: "MPG", value: fmt.mpg(bands[i].mpg) }, { label: "Miles", value: fmt.mi(bands[i].miles) }],
    empty: "Drive a bit to see your sweet spot",
  });
}

function renderInsights(s) {
  const life = s.lifetime, trips = s.trips;
  const idlePct = life.gallons > 0 ? life.idle_gallons / life.gallons * 100 : null;
  const best = trips.filter(t => t.mpg != null && t.miles >= 2).sort((a, b) => b.mpg - a.mpg)[0];
  const hard = trips.reduce((a, t) => a + t.hard_accel + t.hard_brake, 0);
  const items = [
    ["Spent idling", idlePct == null ? "—" : `${idlePct.toFixed(0)}%`, life.idle_gallons ? `${fmt.gal(life.idle_gallons)} gal standing still` : "of fuel"],
    ["Average trip", life.trips ? `${fmt.mi(life.miles / life.trips)} mi` : "—", life.trips ? fmt.dur(life.hours * 3600 / life.trips) : ""],
    ["Best trip", best ? `${fmt.mpg(best.mpg)}` : "—", best ? `mpg · ${fmt.mi(best.miles)} mi on ${fmt.date(best.start_ts)}` : "2+ mile trips"],
    ["Hard accel / brake", life.miles > 1 ? (hard / life.miles * 100).toFixed(1) : "—", "per 100 miles"],
  ];
  $("insights").innerHTML = items.map(([k, v, sub]) =>
    `<div class="insight"><div class="eyebrow">${k}</div><div class="tile-num">${v}</div><div class="muted small">${sub}</div></div>`).join("");
}

function renderTrips(trips) {
  $("tripCount").textContent = trips.length ? `${trips.length} logged` : "";
  if (!trips.length) return;
  const max = Math.max(...trips.map(t => t.mpg ?? 0), 1);
  $("tripRows").innerHTML = trips.map(t => `
    <tr tabindex="0" data-id="${t.id}">
      <td>${fmt.when(t.start_ts)}${t.ts_approx ? ' <span class="approx" title="Recorded before the clock was set; time is approximate">≈</span>' : ""}${t.in_progress ? ' <span class="badge new" title="Still being recorded, or the car turned off before the end was uploaded; the rest arrives next time the logger is on WiFi">in progress</span>' : ""}</td>
      <td class="num">${fmt.mi(t.miles)} mi</td>
      <td class="num">${fmt.dur(t.duration_s)}</td>
      <td class="num">${Math.round(t.avg_mph)} mph</td>
      <td class="num"><span class="mpg-cell"><span class="mpg-bar"><i style="width:${(t.mpg ?? 0) / max * 100}%"></i></span><b>${fmt.mpg(t.mpg)}</b></span></td>
      <td class="num">${fmt.gal(t.gallons)} gal</td>
      <td class="num">${fmt.money(t.cost)}</td>
    </tr>`).join("");
  for (const tr of $("tripRows").querySelectorAll("tr[data-id]")) {
    tr.onclick = () => openTrip(+tr.dataset.id);
    tr.onkeydown = e => { if (e.key === "Enter") openTrip(+tr.dataset.id); };
  }
}

// ---------- trip detail ----------
async function openTrip(id) {
  const dlg = $("tripDlg");
  history.replaceState(null, "", `#trip=${id}`);
  dlg.onclose = () => history.replaceState(null, "", location.pathname);
  $("tdCharts").innerHTML = "";
  $("tdStats").innerHTML = "";
  dlg.showModal();
  const d = await api(`/api/trips/${id}`);
  const t = d.trip, s = d.series;
  $("tdTitle").textContent = fmt.when(t.start_ts);
  $("tdSub").textContent = `${t.ambient_f != null ? Math.round(t.ambient_f) + "°F outside · " : ""}top speed ${Math.round(t.max_mph)} mph · avg ${Math.round(t.avg_rpm)} rpm`;
  const stats = [
    ["MPG", fmt.mpg(t.mpg)], ["Distance", fmt.mi(t.miles) + " mi"], ["Time", fmt.dur(t.duration_s)],
    ["Fuel", fmt.gal(t.gallons) + " gal"], ["Avg speed", Math.round(t.avg_mph) + " mph"], ["Idle", fmt.dur(t.idle_s)],
  ];
  if (t.odo_end_mi) stats.push(["Odometer", Math.round(t.odo_end_mi).toLocaleString() + " mi"]);
  for (const r of d.perf || []) stats.push([r.kind, `${r.precise ? "" : "~"}${r.time_s.toFixed(2)} s`]);
  $("tdStats").innerHTML = stats.map(([k, v]) => `<div class="insight"><div class="eyebrow">${k}</div><div class="tile-num">${v}</div></div>`).join("");

  const panels = [
    ["Speed", "mph", s.mph, v => v.toFixed(0)],
    ["Instant MPG", "10 s average, capped at 99", s.mpg, v => v.toFixed(1)],
    ["Engine speed", "rpm", s.rpm, v => Math.round(v).toLocaleString()],
    ["Boost", "psi above atmosphere", s.boost_psi, v => v.toFixed(1)],
    ["Coolant", "°F", s.coolant_f, v => v.toFixed(0)],
    ["Gear", "detected from rpm and speed", s.gear, v => v.toFixed(0)],
  ];
  const sync = [];
  for (const [name, unit, values, f] of panels) {
    if (!values.some(v => v != null)) continue;
    const title = document.createElement("div");
    title.className = "td-chart-title";
    title.innerHTML = `${name} <span class="muted small">${unit}</span>`;
    const box = document.createElement("div");
    box.className = "chart";
    box.style.height = "130px";
    $("tdCharts").append(title, box);
    sync.push(Charts.lineChart(box, {
      label: `${name} over the trip`, x: s.t,
      series: [{ name, values, color: "--series-1", kind: "line" }],
      fmtX: v => fmt.clock(v), fmtY: v => f(v), fmtTick: v => v.toLocaleString(), sync,
      title: i => fmt.clock(s.t[i]),
    }));
  }
  $("tdDelete").onclick = async () => {
    if (!confirm("Delete this trip? This can't be undone.")) return;
    await api(`/api/trips/${id}`, { method: "DELETE" });
    dlg.close();
    loadSummary();
  };
}

for (const b of document.querySelectorAll("[data-close]")) b.onclick = () => b.closest("dialog").close();

// ---------- live ----------
const gSpeed = Charts.gauge($("gSpeed"), { min: 0, max: 120, label: "Speed", unit: "mph" });
const gRpm = Charts.gauge($("gRpm"), { min: 0, max: 7000, label: "RPM", unit: "rpm", redline: 6500, fmt: v => (v / 1000).toFixed(1) + "k" });

async function pollLive() {
  try {
    const l = await api("/api/live");
    const pill = $("statusPill");
    const driving = l.online && l.state === "recording";
    pill.classList.toggle("on", l.online);
    $("live").hidden = !driving;
    if (!l.online) {
      $("statusText").textContent = l.age_s != null ? `parked · seen ${ago(l.age_s)}` : "car offline";
      return;
    }
    if (!driving) {
      $("statusText").textContent = l.state === "engine off" ? "connected · engine off" : `connected · ${l.state}`;
      return;
    }
    $("statusText").textContent = l.state === "recording" ? "driving · live" : `live · ${l.state}`;
    gSpeed(l.mph);
    gRpm(l.rpm);
    $("lInstant").textContent = l.instant_mpg == null ? (l.mph > 1 ? "∞" : "—") : Math.min(l.instant_mpg, 99).toFixed(1);
    $("lTripMpg").textContent = fmt.mpg(l.trip_mpg);
    $("lTripMi").textContent = l.trip_miles.toFixed(1);
    $("lBoost").textContent = l.boost_psi == null ? "—" : `${l.boost_psi.toFixed(1)} psi`;
    $("lCoolant").textContent = l.coolant_f == null ? "—" : `${Math.round(l.coolant_f)}°F`;
    $("lIat").textContent = l.iat_f == null ? "—" : `${Math.round(l.iat_f)}°F`;
    $("lVolt").textContent = `${l.voltage.toFixed(1)} V`;
    $("lGph").textContent = `${l.gph.toFixed(2)} gal/h`;
    $("lTrim").textContent = `${l.stft >= 0 ? "+" : ""}${l.stft.toFixed(0)}% / ${l.ltft >= 0 ? "+" : ""}${l.ltft.toFixed(0)}%`;
    $("lPedal").style.width = l.pedal + "%";
    $("lLoad").style.width = l.load + "%";
  } catch {
    $("statusText").textContent = "server unreachable";
  }
}
function ago(s) {
  if (s < 90) return "just now";
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  return `${Math.round(s / 86400)} d ago`;
}

// ---------- fill-ups ----------
function localNow() {
  const d = new Date();
  d.setMinutes(d.getMinutes() - d.getTimezoneOffset());
  return d.toISOString().slice(0, 16);
}
async function loadFills() {
  const rows = await api("/api/fillups");
  if (!rows.length) {
    $("fillRows").innerHTML = '<tr><td colspan="7" class="muted empty">No fill-ups logged.</td></tr>';
    return;
  }
  fillRowsById = new Map(rows.map(f => [f.id, f]));
  $("fillRows").innerHTML = rows.map(f => `
    <tr class="clickable" data-id="${f.id}" title="Click to edit"><td>${fmt.when(f.ts)}</td>
    <td class="num">${f.gallons_est ? "~" : ""}${f.gallons.toFixed(f.gallons_est ? 1 : 3)}</td>
    <td class="num">${f.price ? "$" + f.price.toFixed(3) : "—"}</td>
    <td class="num">${f.odometer_mi ? Math.round(f.odometer_mi).toLocaleString() : "—"}</td>
    <td class="num">${f.mpg ? (f.mpg_est ? "~" : "") + fmt.mpg(f.mpg) : "—"}</td>
    <td>${f.source === "auto" ? '<span class="badge auto">auto</span> ' : ""}<span class="badge">${f.full ? "full" : "partial"}</span></td>
    <td class="num"><button class="link-btn" data-del="${f.id}" aria-label="Delete fill-up">✕</button></td></tr>`).join("");
  for (const b of $("fillRows").querySelectorAll("[data-del]")) {
    b.onclick = e => { e.stopPropagation(); deleteFill(+b.dataset.del); };
  }
  for (const tr of $("fillRows").querySelectorAll("tr[data-id]")) tr.onclick = () => openFill(+tr.dataset.id);
}
let fillRowsById = new Map(), editingFill = null;
async function deleteFill(id) {
  if (!confirm("Delete this fill-up?")) return false;
  await api(`/api/fillups/${id}`, { method: "DELETE" });
  loadFills(); loadSummary();
  return true;
}
function openFill(id) {
  const f = fillRowsById.get(id), form = $("fdForm");
  editingFill = id;
  $("fdSub").textContent = fmt.when(f.ts) + (f.source === "auto" ? " · detected automatically" : "");
  form.gallons.value = f.gallons_est ? f.gallons.toFixed(1) : f.gallons;
  form.price.value = f.price || "";
  form.odometer_mi.value = f.odometer_mi ? Math.round(f.odometer_mi) : "";
  form.full.checked = !!f.full;
  $("fdHint").textContent = f.gallons_est
    ? `Gallons are estimated from the fuel gauge (${f.note.replace(/^Gauge /, "")}). Enter the number from the pump for exact MPG.`
    : f.note || "";
  $("fillDlg").showModal();
}
$("fdForm").onsubmit = async e => {
  e.preventDefault();
  const form = e.target, f = fillRowsById.get(editingFill);
  const body = { price: form.price.value, odometer_mi: form.odometer_mi.value, full: form.full.checked };
  // Only send gallons if changed, so an untouched estimate stays marked as one
  const shown = f.gallons_est ? f.gallons.toFixed(1) : String(f.gallons);
  if (form.gallons.value !== shown) body.gallons = form.gallons.value;
  await api(`/api/fillups/${editingFill}`, { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  $("fillDlg").close();
  loadFills(); loadSummary();
};
$("fdDelete").onclick = async () => { if (await deleteFill(editingFill)) $("fillDlg").close(); };
$("fillForm").ts.value = localNow();
$("fillForm").onsubmit = async e => {
  e.preventDefault();
  const f = e.target;
  await api("/api/fillups", {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      ts: new Date(f.ts.value).getTime() / 1000, gallons: f.gallons.value, price: f.price.value,
      odometer_mi: f.odometer_mi.value, full: f.full.checked,
    }),
  });
  f.reset();
  f.ts.value = localNow();
  f.full.checked = true;
  if (summary?.odometer_mi) f.odometer_mi.value = Math.round(summary.odometer_mi);
  loadFills(); loadSummary();
};

// ---------- maintenance ----------
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => `&#${c.charCodeAt(0)};`);
const localDate = () => localNow().slice(0, 10);
function ago(ts) {
  const days = Math.floor((Date.now() / 1000 - ts) / 86400);
  if (days < 1) return "today";
  if (days < 60) return `${days} day${days === 1 ? "" : "s"} ago`;
  const months = Math.round(days / 30.4);
  return months < 24 ? `${months} months ago` : `${(days / 365).toFixed(1)} years ago`;
}
async function loadMaint() {
  const rows = await api("/api/maintenance");
  $("maintCount").textContent = rows.length ? `${rows.length} entr${rows.length === 1 ? "y" : "ies"}` : "";
  if (!rows.length) {
    $("maintRows").innerHTML = '<tr><td colspan="6" class="muted empty">No maintenance logged.</td></tr>';
    $("maintSince").innerHTML = '<p class="muted small">Nothing logged yet.</p>';
    return;
  }
  $("maintRows").innerHTML = rows.map(m => `
    <tr><td>${new Date(m.ts * 1000).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" })}</td>
    <td>${esc(m.kind)}</td>
    <td class="num">${m.odometer_mi ? Math.round(m.odometer_mi).toLocaleString() : "—"}</td>
    <td class="num">${m.cost != null ? fmt.money(m.cost) : "—"}</td>
    <td class="note">${esc(m.note)}</td>
    <td class="num"><button class="link-btn" data-del="${m.id}" aria-label="Delete entry">✕</button></td></tr>`).join("");
  for (const b of $("maintRows").querySelectorAll("[data-del]")) {
    b.onclick = async () => {
      if (!confirm("Delete this maintenance entry?")) return;
      await api(`/api/maintenance/${b.dataset.del}`, { method: "DELETE" });
      loadMaint();
    };
  }
  // Most recent entry per service type (case-insensitive)
  const latest = new Map();
  for (const m of rows) {
    const k = m.kind.trim().toLowerCase();
    if (!latest.has(k)) latest.set(k, m);
  }
  const odo = summary?.odometer_mi;
  $("maintSince").innerHTML = [...latest.values()].map(m => {
    const miles = odo && m.odometer_mi ? Math.max(0, odo - m.odometer_mi) : null;
    return `<div class="insight"><div class="eyebrow">${esc(m.kind)}</div>
      <div class="tile-num">${miles != null ? Math.round(miles).toLocaleString() + " mi" : "—"}</div>
      <div class="muted small">${ago(m.ts)}${m.odometer_mi ? ` · at ${Math.round(m.odometer_mi).toLocaleString()}` : ""}</div></div>`;
  }).join("");
}
$("maintForm").date.value = localDate();
$("maintForm").onsubmit = async e => {
  e.preventDefault();
  const f = e.target;
  const [y, mo, d] = f.date.value.split("-").map(Number);
  await api("/api/maintenance", {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      ts: new Date(y, mo - 1, d, 12).getTime() / 1000, odometer_mi: f.odometer_mi.value,
      kind: f.kind.value, cost: f.cost.value, note: f.note.value,
    }),
  });
  f.reset();
  f.date.value = localDate();
  if (summary?.odometer_mi) f.odometer_mi.value = Math.round(summary.odometer_mi);
  loadMaint();
};

// ---------- health ----------
const STATUS_ICON = { good: "✓", warning: "!", critical: "!", info: "–" };
const chip = (level, label) => `<span class="status ${level}"><i aria-hidden="true">${STATUS_ICON[level]}</i>${esc(label)}</span>`;
const lookup = (code, module) => `https://www.google.com/search?q=${encodeURIComponent(module ? `VW ${module} fault ${code}` : "VW " + code)}`;

function renderScanBtn(r) {
  const b = $("scanBtn");
  const waiting = !!r.requested_at;
  b.disabled = waiting || !r.car_online;
  b.textContent = waiting ? (r.sent_at ? "Scanning…" : "Waiting for car…") : "Scan now";
  b.title = r.car_online ? "Read fault codes from every module now (about 15 s)" : "The car needs to be on and on home WiFi";
  clearTimeout(renderScanBtn.timer);
  if (waiting) renderScanBtn.timer = setTimeout(loadHealth, 3000);  // pick up the result as soon as it lands
}
$("scanBtn").onclick = async () => renderScanBtn(await api("/api/health/scan", { method: "POST" }));

async function loadHealth() {
  const h = await api("/api/health");
  renderScanBtn(h.request);
  if (!h.latest) return;
  const L = h.latest;
  $("healthWhen").textContent = `scanned ${L.ts_approx ? "≈ " : ""}${fmt.when(L.ts)}${L.odometer_mi ? " · " + Math.round(L.odometer_mi).toLocaleString() + " mi" : ""}`;
  const codes = L.modules.flatMap(m => m.codes.map(c => ({ ...c, module: m.label })));
  const active = codes.filter(c => c.active || c.stored);
  const top = L.mil ? chip("critical", "Check engine light is on")
    : active.length ? chip("warning", `${codes.length} fault code${codes.length === 1 ? "" : "s"} stored`)
    : codes.length ? chip("info", `${codes.length} pending code${codes.length === 1 ? "" : "s"}`)
    : chip("good", "No fault codes in any module");
  const modules = L.modules.map(m => `<div class="module"><span class="name">${esc(m.label)}</span>${
    !m.answered ? chip("info", "No answer") : m.codes.length ? chip("warning", `${m.codes.length} code${m.codes.length === 1 ? "" : "s"}`) : chip("good", "OK")}</div>`).join("");
  const codeList = codes.map(c => `<div class="code"><b>${c.code}</b>
      <span>${c.text ? esc(c.text) : `<a href="${lookup(c.code, c.vw && c.module)}" target="_blank" rel="noopener">${c.vw ? "VW fault number — look it up" : "Look up " + c.code}</a>`}</span>
      <span class="flags">${c.new ? '<span class="badge new">new</span>' : ""}${c.active ? '<span class="badge">active now</span>' : ""}${c.stored ? '<span class="badge">stored</span>' : ""}${c.pending && !c.stored ? '<span class="badge">pending</span>' : ""}</span>
      <span class="meta">${esc(c.module)} · first seen ${fmt.date(c.first_ts)}${c.vw ? "" : " · fault type " + c.ftb}</span></div>`).join("");
  const mis = L.misfire.length ? `<div class="sub-head">Misfire counters <span class="muted small">(last drive cycle · 10-cycle average)</span></div>
    <div class="misfire">${L.misfire.map(m => `<div class="insight"><div class="eyebrow">${m.cyl === "all" ? "All cylinders" : "Cylinder " + m.cyl}</div>
      <div class="tile-num">${m.last ?? "—"}</div><div class="muted small">avg ${m.avg10 ?? "—"}</div></div>`).join("")}</div>` : "";
  const cleared = h.cleared.length ? `<details><summary>${h.cleared.length} code${h.cleared.length === 1 ? "" : "s"} seen before but gone now</summary><div class="codes">${
    h.cleared.map(c => `<div class="code"><b>${c.code}</b><span>${c.text ? esc(c.text) : `<a href="${lookup(c.code)}" target="_blank" rel="noopener">Look up</a>`}</span><span></span>
      <span class="meta">${esc(c.module)} · last seen ${fmt.date(c.last_ts)}</span></div>`).join("")}</div></details>` : "";
  $("health").innerHTML = `<div class="health-top">${top}<span class="muted small">${h.scans} scan${h.scans === 1 ? "" : "s"} so far · full scan every 20 starts or 300 mi, or when engine codes change</span></div>
    <div class="modules">${modules}</div>${codeList ? `<div class="codes">${codeList}</div>` : ""}${mis}${cleared}`;
}

// ---------- performance & gears ----------
async function loadPerf() {
  const p = await api("/api/performance");
  const kinds = [["0-60", "0–60 mph"], ["30-70", "30–70 mph"], ["1/4 mile", "Quarter mile"]];
  const t = r => `${r.precise ? "" : "~"}${r.time_s.toFixed(2)} s`;
  const best = kinds.map(([k, label]) => {
    const r = p.best[k];
    return `<div class="insight"><div class="eyebrow">${label}</div><div class="tile-num">${r ? t(r) : "—"}</div>
      <div class="muted small">${r ? (r.trap_mph ? `${r.trap_mph} mph trap · ` : "") + fmt.date(r.ts) : "no run yet"}</div></div>`;
  }).join("");
  const rows = p.runs.slice(0, 8).map(r => `<tr class="clickable" data-trip="${r.trip_id}"><td>${fmt.when(r.ts)}</td><td>${r.kind}</td>
    <td class="num">${t(r)}</td><td class="num">${r.trap_mph ? r.trap_mph + " mph" : ""}</td></tr>`).join("");
  $("perf").innerHTML = `<div class="perf-best">${best}</div>${rows
    ? `<div class="sub-head">Recent runs</div><div class="table-wrap"><table class="fills"><tbody>${rows}</tbody></table></div>`
    : '<p class="muted small">Runs are timed automatically when you accelerate hard: from a stop to 60 mph or a quarter mile, or 30–70 mph with your foot down. "~" means fewer samples than ideal.</p>'}`;
  for (const tr of $("perf").querySelectorAll("tr[data-trip]")) tr.onclick = () => openTrip(+tr.dataset.trip);
}

async function loadGears() {
  const g = await api("/api/gears");
  if (!g.stats) {
    $("gearSub").textContent = "";
    $("gears").innerHTML = `<p class="muted small">Learning your gearbox from your driving: found ${g.learned} of ${g.needed} gears so far.
      Once all six show up, you'll get the current gear on the vitals page, time in each gear, shift points and clutch-slip warnings.</p>`;
    return;
  }
  const s = g.stats;
  $("gearSub").textContent = "time in each gear";
  $("gears").innerHTML = `<div class="chart" id="gearChart" style="height:180px"></div>
    <div class="sub-head">Typical upshift</div><div class="muted small">${Object.entries(s.upshift_rpm).map(([k, v]) => `${k}: <b>${v.toLocaleString()}</b> rpm`).join(" · ") || "not enough shifts yet"}</div>
    <div class="sub-head">Clutch</div><div>${s.slips.length ? chip("warning", `${s.slips.length} possible slip${s.slips.length === 1 ? "" : "s"}`) + `<div class="muted small">${s.slips.slice(-5).map(x => `gear ${x.gear} at ${x.rpm} rpm (+${x.over_pct}%)`).join(" · ")}</div>` : chip("good", "No slipping detected")}</div>`;
  Charts.barChart($("gearChart"), {
    label: "Time in each gear", labels: s.time_s.map((_, i) => `${i + 1}`), values: s.time_s.map(v => v / 60),
    color: "--series-1", fmt: v => `${v.toFixed(0)} min`,
    tipTitle: i => `Gear ${i + 1}`, tip: i => [{ label: "Time", value: fmt.dur(s.time_s[i]) }, { label: "rpm per mph", value: g.rpm_per_mph[i] }],
  });
}

// ---------- settings ----------
$("settingsBtn").onclick = () => {
  const f = $("setForm");
  for (const [k, v] of Object.entries(summary?.settings ?? {})) if (f[k]) f[k].value = v;
  $("setDlg").showModal();
};
$("setForm").onsubmit = async e => {
  e.preventDefault();
  const body = Object.fromEntries(new FormData(e.target));
  await api("/api/settings", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  $("setDlg").close();
  loadSummary();
};

loadSummary().then(loadMaint);
const loadExtras = () => Promise.allSettled([loadHealth(), loadPerf(), loadGears()]);
loadExtras();
setInterval(loadExtras, 60000);
loadFills();
pollLive();
const deep = location.hash.match(/trip=(\d+)/);
if (deep) openTrip(+deep[1]);
setInterval(pollLive, 1000);
setInterval(loadSummary, 60000);
