// Small SVG chart helpers: line/dot charts with a crosshair tooltip (optionally
// synced across charts), bar charts with per-bar tooltips, and arc gauges.

const SVGNS = "http://www.w3.org/2000/svg";
const tooltipEl = () => document.getElementById("tooltip");

function el(tag, attrs = {}, parent) {
  const e = document.createElementNS(SVGNS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (parent) parent.appendChild(e);
  return e;
}

function niceTicks(min, max, count = 4) {
  if (!isFinite(min) || !isFinite(max)) return [0, 1];
  if (min === max) { min -= 1; max += 1; }
  const span = max - min;
  const step0 = span / count;
  const mag = 10 ** Math.floor(Math.log10(step0));
  const step = [1, 2, 2.5, 5, 10].map(m => m * mag).find(s => s >= step0);
  const lo = Math.floor(min / step) * step;
  const hi = Math.ceil(max / step) * step;
  const ticks = [];
  for (let v = lo; v <= hi + step / 2; v += step) ticks.push(+v.toFixed(10));
  return ticks;
}

function showTooltip(x, y, title, rows) {
  const t = tooltipEl();
  t.innerHTML = `<div class="tt-title">${title}</div>` + rows.map(r =>
    `<div class="tt-row"><span class="tt-key">${r.color ? `<i style="background:${r.color}"></i>` : ""}${r.label}</span><b>${r.value}</b></div>`).join("");
  t.hidden = false;
  const w = t.offsetWidth, h = t.offsetHeight;
  let left = x + 14, top = y - h - 10;
  if (left + w > window.innerWidth - 8) left = x - w - 14;
  if (top < 8) top = y + 14;
  t.style.left = left + "px";
  t.style.top = top + "px";
}
function hideTooltip() { tooltipEl().hidden = true; }

function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

// opts: { x: number[], series: [{name, values, color(var name), kind: "line"|"dot"}],
//         fmtX(x) -> string, fmtY(y) -> string, yMin?, xTicks?(min,max)->[], title?(i)->string, sync? }
function lineChart(container, opts) {
  const chart = { container, opts, showAt: null };
  const draw = () => {
    container.innerHTML = "";
    const W = container.clientWidth, H = container.clientHeight;
    const m = { l: 44, r: 12, t: 10, b: 24 };
    const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, role: "img", "aria-label": opts.label || "chart" }, container);
    const xs = opts.x;
    const all = opts.series.flatMap(s => s.values.filter(v => v != null));
    if (!xs.length || !all.length) {
      el("text", { x: W / 2, y: H / 2, "text-anchor": "middle", class: "empty-msg" }, svg).textContent = opts.empty || "No data yet";
      return;
    }
    const xmin = Math.min(...xs), xmax = Math.max(...xs);
    let ymin = opts.yMin ?? Math.min(...all), ymax = Math.max(...all);
    if (opts.yMin == null) ymin = Math.min(ymin, 0) < 0 ? ymin : Math.max(0, ymin - (ymax - ymin) * .1);
    const yt = niceTicks(ymin, ymax);
    ymin = yt[0]; ymax = yt[yt.length - 1];
    const X = v => m.l + (xmax === xmin ? (W - m.l - m.r) / 2 : (v - xmin) / (xmax - xmin) * (W - m.l - m.r));
    const Y = v => m.t + (1 - (v - ymin) / (ymax - ymin || 1)) * (H - m.t - m.b);

    for (const v of yt) {
      el("line", { x1: m.l, x2: W - m.r, y1: Y(v), y2: Y(v), class: "gridline" }, svg);
      el("text", { x: m.l - 8, y: Y(v) + 4, "text-anchor": "end" }, svg).textContent = opts.fmtTick ? opts.fmtTick(v) : v;
    }
    const xticks = opts.xTicks ? opts.xTicks(xmin, xmax, W - m.l - m.r) : niceTicks(xmin, xmax, Math.max(2, Math.floor(W / 110)));
    for (const v of xticks) {
      if (v < xmin || v > xmax) continue;
      el("text", { x: X(v), y: H - 6, "text-anchor": "middle" }, svg).textContent = opts.fmtX(v, true);
    }

    for (const s of opts.series) {
      const color = cssVar(s.color);
      if (s.kind === "dot") {
        s.values.forEach((v, i) => {
          if (v == null) return;
          el("circle", { cx: X(xs[i]), cy: Y(v), r: 4.5, fill: color, stroke: cssVar("--surface-1"), "stroke-width": 2 }, svg);
        });
      } else {
        let d = "", pen = false;
        s.values.forEach((v, i) => {
          if (v == null) { pen = false; return; }
          d += `${pen ? "L" : "M"}${X(xs[i]).toFixed(1)},${Y(v).toFixed(1)}`;
          pen = true;
        });
        el("path", { d, fill: "none", stroke: color, "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round" }, svg);
      }
    }

    const cross = el("line", { y1: m.t, y2: H - m.b, class: "crosshair", visibility: "hidden" }, svg);
    const markers = opts.series.map(s => el("circle", { r: 5, fill: cssVar(s.color), stroke: cssVar("--surface-1"), "stroke-width": 2, visibility: "hidden" }, svg));
    const hit = el("rect", { x: m.l, y: 0, width: W - m.l - m.r, height: H, fill: "transparent" }, svg);

    const nearest = xv => {
      let lo = 0, hi = xs.length - 1;
      while (hi - lo > 1) { const mid = (lo + hi) >> 1; xs[mid] < xv ? lo = mid : hi = mid; }
      return Math.abs(xs[lo] - xv) <= Math.abs(xs[hi] - xv) ? lo : hi;
    };
    chart.showAt = (i, clientX, clientY) => {
      if (i == null) {
        cross.setAttribute("visibility", "hidden");
        markers.forEach(mk => mk.setAttribute("visibility", "hidden"));
        return;
      }
      cross.setAttribute("x1", X(xs[i])); cross.setAttribute("x2", X(xs[i]));
      cross.setAttribute("visibility", "visible");
      opts.series.forEach((s, k) => {
        const v = s.values[i];
        markers[k].setAttribute("visibility", v == null ? "hidden" : "visible");
        if (v != null) { markers[k].setAttribute("cx", X(xs[i])); markers[k].setAttribute("cy", Y(v)); }
      });
      if (clientX != null) {
        const rows = opts.series.map(s => ({ label: s.name, value: s.values[i] == null ? "—" : opts.fmtY(s.values[i], s), color: cssVar(s.color) }));
        showTooltip(clientX, clientY, opts.title ? opts.title(i) : opts.fmtX(xs[i]), rows.concat(opts.extraRows ? opts.extraRows(i) : []));
      }
    };
    chart.indexFor = xv => nearest(xv);
    const move = ev => {
      const r = svg.getBoundingClientRect();
      const px = (ev.clientX - r.left) * (W / r.width);
      const xv = xmin + (px - m.l) / (W - m.l - m.r) * (xmax - xmin);
      const i = nearest(xv);
      chart.showAt(i, ev.clientX, ev.clientY);
      if (opts.sync) opts.sync.forEach(c => c !== chart && c.showAt && c.showAt(c.indexFor(xs[i])));
      if (opts.onHover) opts.onHover(i);
    };
    hit.addEventListener("pointermove", move);
    hit.addEventListener("pointerleave", () => {
      chart.showAt(null); hideTooltip();
      if (opts.sync) opts.sync.forEach(c => c.showAt && c.showAt(null));
    });
    if (opts.onClick) hit.addEventListener("click", ev => { move(ev); opts.onClick(nearest(xmin + ((ev.clientX - svg.getBoundingClientRect().left) * (W / svg.getBoundingClientRect().width) - m.l) / (W - m.l - m.r) * (xmax - xmin))); });
  };
  chart.redraw = draw;
  draw();
  new ResizeObserver(() => { if (container.clientWidth) draw(); }).observe(container);
  return chart;
}

// opts: { labels: string[], values: (number|null)[], color, fmt(v), tip(i) -> rows, xLabel }
function barChart(container, opts) {
  const draw = () => {
    container.innerHTML = "";
    const W = container.clientWidth, H = container.clientHeight;
    const m = { l: 36, r: 8, t: 18, b: 24 };
    const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, role: "img", "aria-label": opts.label || "bar chart" }, container);
    const vals = opts.values.filter(v => v != null);
    if (!vals.length) {
      el("text", { x: W / 2, y: H / 2, "text-anchor": "middle", class: "empty-msg" }, svg).textContent = opts.empty || "No data yet";
      return;
    }
    const yt = niceTicks(0, Math.max(...vals));
    const ymax = yt[yt.length - 1];
    const Y = v => m.t + (1 - v / ymax) * (H - m.t - m.b);
    for (const v of yt) {
      el("line", { x1: m.l, x2: W - m.r, y1: Y(v), y2: Y(v), class: "gridline" }, svg);
      el("text", { x: m.l - 6, y: Y(v) + 4, "text-anchor": "end" }, svg).textContent = v;
    }
    const n = opts.labels.length;
    const slot = (W - m.l - m.r) / n;
    const bw = Math.min(44, slot - 2);
    const color = cssVar(opts.color);
    const best = Math.max(...vals);
    opts.labels.forEach((lab, i) => {
      const cx = m.l + slot * i + slot / 2;
      el("text", { x: cx, y: H - 6, "text-anchor": "middle" }, svg).textContent = slot < 46 ? lab.split("–")[0] : lab;
      const v = opts.values[i];
      if (v == null) return;
      const x = cx - bw / 2, y = Y(v), h = Y(0) - y, r = Math.min(4, h, bw / 2);
      el("path", { d: `M${x},${Y(0)}V${y + r}Q${x},${y} ${x + r},${y}H${x + bw - r}Q${x + bw},${y} ${x + bw},${y + r}V${Y(0)}Z`, fill: color }, svg);
      if (v === best) {
        el("text", { x: cx, y: y - 5, "text-anchor": "middle", style: "fill:var(--text-primary);font-weight:600" }, svg).textContent = opts.fmt(v);
      }
      const hit = el("rect", { x: m.l + slot * i, y: m.t, width: slot, height: H - m.t - m.b, fill: "transparent" }, svg);
      hit.addEventListener("pointermove", ev => showTooltip(ev.clientX, ev.clientY, opts.tipTitle(i), opts.tip(i)));
      hit.addEventListener("pointerleave", hideTooltip);
    });
  };
  draw();
  new ResizeObserver(() => { if (container.clientWidth) draw(); }).observe(container);
}

// One tick per local midnight, thinned to fit; for x values in unix seconds.
function dayTicks(min, max, width) {
  const day = 86400, ticks = [];
  const d = new Date(min * 1000);
  d.setHours(0, 0, 0, 0);
  let t = d.getTime() / 1000 + day;
  const span = Math.max(1, Math.ceil((max - min) / day));
  const every = Math.max(1, Math.ceil(span / Math.max(2, Math.floor(width / 90))));
  for (let k = 0; t <= max; k++, t += day) if (k % every === 0) ticks.push(t);
  return ticks.length ? ticks : [min];
}

// 240-degree arc gauge; returns update(value)
function gauge(container, { min, max, label, unit, fmt = v => Math.round(v), redline }) {
  const size = 200, cx = 100, cy = 100, r = 80, sweep = 240, start = 150;
  const svg = el("svg", { viewBox: `0 0 ${size} ${size - 24}`, role: "img", "aria-label": label }, container);
  const pt = a => [cx + r * Math.cos(a * Math.PI / 180), cy + r * Math.sin(a * Math.PI / 180)];
  const arc = (a0, a1) => {
    const [x0, y0] = pt(a0), [x1, y1] = pt(a1);
    return `M${x0},${y0}A${r},${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1},${y1}`;
  };
  el("path", { d: arc(start, start + sweep), fill: "none", stroke: "var(--surface-2)", "stroke-width": 12, "stroke-linecap": "round" }, svg);
  if (redline) {
    const a = start + sweep * (redline - min) / (max - min);
    el("path", { d: arc(a, start + sweep), fill: "none", stroke: "var(--critical)", "stroke-width": 3, opacity: .7 }, svg);
  }
  const val = el("path", { fill: "none", stroke: "var(--series-1)", "stroke-width": 12, "stroke-linecap": "round" }, svg);
  const num = el("text", { x: cx, y: cy + 6, "text-anchor": "middle", style: "font-size:40px;font-weight:700;fill:var(--text-primary)" }, svg);
  const lab = el("text", { x: cx, y: cy + 30, "text-anchor": "middle", style: "font-size:13px;fill:var(--text-secondary)" }, svg);
  lab.textContent = unit;
  return v => {
    const f = Math.max(0, Math.min(1, ((v ?? min) - min) / (max - min)));
    val.setAttribute("d", f > 0.004 ? arc(start, start + sweep * f) : "");
    num.textContent = v == null ? "—" : fmt(v);
  };
}

window.Charts = { dayTicks, lineChart, barChart, gauge, showTooltip, hideTooltip };
