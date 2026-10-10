// Live tab: gauges and vital tiles with 2-minute sparklines, fed by the
// dashboard's /api/live poll (app.js calls Vitals.render). Loaded before app.js.
(() => {
const $ = id => document.getElementById(id);
const vSpeed = Charts.gauge($("vSpeed"), { min: 0, max: 160, unit: "mph" });
const vRpm = Charts.gauge($("vRpm"), { min: 0, max: 7000, unit: "rpm", redline: 6500, fmt: v => Math.round(v / 10) * 10 });
const vBoost = Charts.gauge($("vBoost"), { min: -15, max: 25, unit: "psi", fmt: v => (v > 0 ? "+" : "") + v.toFixed(1) });

// ---- status rules: each returns [level, label] or null ----
const S = (level, label) => [level, label];
const rules = {
  battery: (v, l) => v == null ? null : l.rpm > 0
    ? v < 12.8 ? S("critical", "Not charging") : v < 13.2 ? S("warning", "Low charge") : v > 15 ? S("critical", "Overcharging") : S("good", "Charging")
    : v < 12.0 ? S("critical", "Low") : v < 12.4 ? S("warning", "Partial") : S("good", "Healthy"),
  coolant: v => v == null ? null : v > 245 ? S("critical", "Overheating") : v > 230 ? S("warning", "Hot")
    : v < 140 ? S("info", "Warming up") : S("good", "Normal"),
  iat: v => v == null ? null : v > 150 ? S("warning", "Heat soak") : null,
  fuel: v => v == null ? null : v < 12 ? S("critical", "Low fuel") : v < 25 ? S("warning", "Getting low") : null,
  trims: (_, l) => {
    const t = Math.abs((l.stft ?? 0) + (l.ltft ?? 0));
    return l.rpm > 0 ? t > 25 ? S("critical", "Way off") : t > 15 ? S("warning", "Drifting") : S("good", "Normal") : null;
  },
  cat: v => v == null ? null : v > 1650 ? S("critical", "Very hot") : null,
  mil: () => !health ? null : health.mil ? S("critical", "Light on") : health.count ? S("warning", "Codes stored") : S("good", "All clear"),
};
const ICON = { good: "✓", warning: "!", critical: "!", info: "i" };

// ---- tiles ----
let health = null;  // latest scan from /api/health
const tiles = [
  { id: "gear", label: "Gear", unit: "", get: l => l.gear, fmt: v => v,
    sub: l => l.gear ? "" : l.rpm > 0 && l.mph > 3 ? "clutch in / between gears" : "" },
  { id: "mil", label: "Check engine", unit: "", get: () => health ? (health.mil ? "ON" : "Off") : null, fmt: v => v,
    sub: () => health ? `${health.count} fault code${health.count === 1 ? "" : "s"} at last scan` : "no scan yet" },
  { id: "battery", label: "Battery", unit: "V", get: l => l.voltage || null, fmt: v => v.toFixed(1), spark: true,
    sub: l => l.rpm > 0 ? "alternator output" : "resting voltage" },
  { id: "coolant", label: "Coolant", unit: "°F", get: l => l.coolant_f, fmt: Math.round, spark: true },
  { id: "iat", label: "Intake air", unit: "°F", get: l => l.iat_f, fmt: Math.round, spark: true,
    sub: l => l.ambient_f != null ? `${Math.round(l.ambient_f)}°F outside` : "" },
  { id: "fuel", label: "Fuel level", unit: "%", get: l => l.fuel_level || null, fmt: Math.round,
    sub: l => l.fuel_level ? `≈ ${(l.fuel_level / 100 * l.tank_gal).toFixed(1)} gal${range(l)}` : "" },
  { id: "odo", label: "Odometer", unit: "mi", get: l => l.odometer_mi, fmt: v => Math.round(v).toLocaleString(),
    sub: l => l.trip_miles > 0 ? `${l.trip_miles.toFixed(1)} mi this trip` : "" },
  { id: "gph", label: "Fuel flow", unit: "gal/h", get: l => l.rpm > 0 ? l.gph : null, fmt: v => v.toFixed(2), spark: true,
    sub: l => l.instant_mpg != null ? `${Math.min(l.instant_mpg, 99).toFixed(1)} mpg right now` : l.trip_mpg ? `${l.trip_mpg.toFixed(1)} mpg this trip` : "" },
  { id: "trims", label: "Fuel trims", unit: "%", get: l => l.rpm > 0 ? l.ltft + l.stft : null, fmt: v => (v >= 0 ? "+" : "") + v.toFixed(0),
    sub: l => `short ${sign(l.stft)}% · long ${sign(l.ltft)}%`, spark: true },
  { id: "afr", label: "Air / fuel", unit: "AFR", get: l => l.rpm > 0 && l.lambda ? l.lambda * 14.7 : null, fmt: v => v.toFixed(1),
    sub: l => l.fuel_status || "", spark: true },
  { id: "timing", label: "Ignition timing", unit: "°", get: l => l.rpm > 0 ? l.timing : null, fmt: v => v.toFixed(1), spark: true,
    sub: () => "advance before TDC" },
  { id: "cat", label: "Catalyst", unit: "°F", get: l => l.cat_f, fmt: Math.round },
  { id: "alt", label: "Altitude", unit: "ft", get: l => l.baro_kpa ? 145366.45 * (1 - (l.baro_kpa / 101.325) ** 0.190284) : null,
    fmt: v => (Math.round(v / 50) * 50).toLocaleString(), sub: l => l.baro_kpa ? `from barometer · ${l.baro_kpa} kPa` : "" },
  { id: "inputs", label: "Driver & engine", bars: [["Pedal", "pedal"], ["Throttle", "throttle"], ["Load", "load"]] },
];
const sign = v => v == null ? "—" : (v >= 0 ? "+" : "") + v.toFixed(0);
let avgMpg = null;
const range = l => avgMpg ? ` · ~${Math.round(l.fuel_level / 100 * l.tank_gal * avgMpg)} mi range` : "";

const HIST = 240;  // 2 minutes at 2 Hz
const hist = Object.fromEntries(tiles.map(t => [t.id, []]));
$("vitals").innerHTML = tiles.map(t => t.bars
  ? `<div class="vital" id="t-${t.id}"><div class="eyebrow">${t.label}</div><div class="bars">${t.bars.map(([lab, k]) =>
      `<div class="bar-kv"><span>${lab}</span><div class="meter"><i data-k="${k}"></i></div><b data-v="${k}">—</b></div>`).join("")}</div></div>`
  : `<div class="vital" id="t-${t.id}"><div class="row"><div class="eyebrow">${t.label}</div><span class="status" data-status></span></div>
      <div><span class="v" data-v>—</span><span class="u">${t.unit}</span></div><div class="sub" data-sub></div>
      ${t.spark ? `<svg class="spark" viewBox="0 0 ${HIST} 34" preserveAspectRatio="none" aria-hidden="true"><path fill="none" stroke="var(--series-1)" stroke-width="2" vector-effect="non-scaling-stroke" stroke-linejoin="round"/></svg>` : ""}</div>`).join("");

function spark(svg, data) {
  const vals = data.filter(v => v != null);
  if (vals.length < 2) { svg.firstElementChild.setAttribute("d", ""); return; }
  let lo = Math.min(...vals), hi = Math.max(...vals);
  if (hi - lo < 1e-6) { lo -= 1; hi += 1; }
  const dx = HIST / Math.max(data.length - 1, 1);  // stretch until the window fills
  let d = "", pen = false;
  data.forEach((v, i) => {
    if (v == null) { pen = false; return; }
    const y = 31 - (v - lo) / (hi - lo) * 28;
    d += `${pen ? "L" : "M"}${(i * dx).toFixed(1)},${y.toFixed(1)}`;
    pen = true;
  });
  svg.firstElementChild.setAttribute("d", d);
}

function renderTiles(l) {
  for (const t of tiles) {
    const root = $(`t-${t.id}`);
    if (t.bars) {
      for (const [, k] of t.bars) {
        root.querySelector(`[data-k=${k}]`).style.width = (l[k] ?? 0) + "%";
        root.querySelector(`[data-v=${k}]`).textContent = l[k] == null ? "—" : Math.round(l[k]) + "%";
      }
      continue;
    }
    const v = t.get(l);
    root.querySelector("[data-v]").textContent = v == null || Number.isNaN(v) ? "—" : t.fmt(v);
    root.querySelector("[data-sub]").textContent = t.sub ? t.sub(l) : "";
    const st = rules[t.id] ? rules[t.id](v, l) : null;
    const chip = root.querySelector("[data-status]");
    chip.className = "status" + (st ? " " + st[0] : "");
    chip.innerHTML = st ? `<i aria-hidden="true">${ICON[st[0]]}</i>${st[1]}` : "";
    root.classList.toggle("critical", st?.[0] === "critical");
    root.classList.toggle("warning", st?.[0] === "warning");
    if (t.spark) {
      const h = hist[t.id];
      h.push(v);
      if (h.length > HIST) h.shift();
      spark(root.querySelector("svg.spark"), h);
    }
  }
}


let healthTimer = 0;
async function loadVitalsHealth() {
  try {
    const h = await fetch("/api/health").then(r => r.json());
    if (h.latest) health = { mil: h.latest.mil, count: h.latest.modules.reduce((a, m) => a + m.codes.length, 0) };
  } catch {}
}

window.Vitals = {
  render(l) {
    $("vOffline").hidden = l.online;
    $("vLiveArea").hidden = !l.online;
    if (!l.online) {
      $("vOfflineText").textContent = "Vitals appear when the ignition is on and the car is on home WiFi.";
      return;
    }
    if (Date.now() - healthTimer > 60000) { healthTimer = Date.now(); loadVitalsHealth(); }
    vSpeed(l.mph);
    vRpm(l.rpm);
    vBoost(l.rpm > 0 ? l.boost_psi : null);
    renderTiles(l);
  },
  setAvgMpg(v) { avgMpg = v; },
};
})();
