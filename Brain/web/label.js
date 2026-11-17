"use strict";
// Trang gán nhãn hư hỏng. Toạ độ khung/đa giác lưu theo PIXEL ẢNH GỐC; server tự cắt xe và chiếu về ảnh cắt khi lưu.
// Mọi chữ từ server đưa vào DOM bằng textContent (không innerHTML).
const $ = (id) => document.getElementById(id);
const COLORS = ["#ff5d5d", "#ffb020", "#35c2ff", "#b36bff", "#2ee6a6", "#ff7ad9", "#c8f031", "#8f9bff", "#ff9a62"];
const CAR_COLOR = "#ffffff";
const HANDLE = 5, HIT = 8, MIN_PX = 6;

function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) if (c != null && c !== false) e.append(c.nodeType ? c : document.createTextNode(String(c)));
  return e;
}
function toast(msg) { const t = h("div", { class: "toast" }, msg); document.body.append(t); setTimeout(() => t.remove(), 2800); }
async function api(path, body) {
  const opt = body === undefined ? {} : { method: "POST", headers: { "X-Tram": "1", "Content-Type": body instanceof Blob ? "application/octet-stream" : "application/json" }, body: body instanceof Blob ? body : JSON.stringify(body) };
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.err || r.statusText);
  return j;
}
try { const t = localStorage.getItem("theme"); if (t) document.documentElement.dataset.theme = t; } catch (e) { /* bỏ qua */ }

const S = {
  items: [], classes: [], names: {}, idx: -1, item: null, img: null,
  tool: "box", cls: 0, sel: null,            // sel = {kind:"car"} | {kind:"shape", i}
  drag: null, poly: null, mouse: null, undo: [], dirty: false, scale: 1,
};
const cv = $("cv"), ctx = cv.getContext("2d");
const color = (c) => COLORS[c % COLORS.length];
const clsName = (c) => S.names[S.classes[c]] || S.classes[c] || "?";

// ---------- tải dữ liệu ----------
async function loadList(keepName) {
  const d = await api("/api/ds/list");
  S.items = d.items; S.classes = d.classes; S.names = d.names;
  if (S.cls >= S.classes.length) S.cls = 0;
  $("cnt").textContent = `${S.items.filter((i) => i.verified).length}/${S.items.length} đã duyệt`;
  renderList(); renderClasses();
  const want = keepName ? S.items.findIndex((i) => i.name === keepName) : S.idx;
  if (S.items.length) await open(Math.min(Math.max(want, 0), S.items.length - 1), true);
  else { S.item = null; S.img = null; $("nm").textContent = "Chưa có ảnh — bấm Chụp hoặc Tải ảnh"; $("st").textContent = ""; draw(); }
}
function renderList() {
  const box = $("list");
  while (box.firstChild) box.removeChild(box.firstChild);
  S.items.forEach((it, i) => box.append(h("button", { "aria-current": String(i === S.idx), onclick: () => open(i) },
    h("span", {}, it.verified ? "✅" : "⬜"), h("span", {}, it.name.replace(/\.jpg$/, "").replace(/^[a-z]+_20/, "")), h("span", { class: "n" }, it.boxes))));
}
function renderClasses() {
  const box = $("classes");
  while (box.firstChild) box.removeChild(box.firstChild);
  S.classes.forEach((c, i) => box.append(h("button", { class: "cls", "aria-pressed": String(i === S.cls), onclick: () => pickClass(i), ondblclick: () => renameClass(i) },
    h("span", { class: "sw", style: "background:" + color(i) }), h("span", {}, clsName(i)), i < 9 ? h("kbd", {}, i + 1) : null)));
}
async function open(i, force) {
  if (!force && S.dirty && !confirm("Bỏ thay đổi chưa lưu của ảnh này?")) return;
  const it = S.items[i];
  S.idx = i; S.sel = null; S.drag = null; S.poly = null; S.undo = []; S.dirty = false;
  const [item, img] = await Promise.all([api("/api/ds/get?name=" + encodeURIComponent(it.name)), new Promise((ok, no) => {
    const im = new Image(); im.onload = () => ok(im); im.onerror = () => no(new Error("Không tải được ảnh")); im.src = "/ds/img/" + encodeURIComponent(it.name) + "?t=" + Date.now();
  })]);
  S.item = item; S.img = img;
  if (item.legacy) S.tool = S.tool === "car" ? "box" : S.tool;
  $("nm").textContent = it.name;
  $("st").textContent = item.legacy ? "ảnh đã cắt" : (item.car ? "ảnh gốc" : "chưa có khung xe");
  syncTools(); renderList(); layout(); draw();
}

// ---------- hình học ----------
const shapes = () => S.item.shapes;
const bboxOf = (pts) => { const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]); return [Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)]; };
const rectOf = (target) => target.kind === "car" ? S.item.car : bboxOf(shapes()[target.i].pts);
const clampX = (v) => Math.min(S.item.w, Math.max(0, v)), clampY = (v) => Math.min(S.item.h, Math.max(0, v));
function toImg(e) { const r = cv.getBoundingClientRect(); return [clampX((e.clientX - r.left) / S.scale), clampY((e.clientY - r.top) / S.scale)]; }
function handlesOf(r) {
  const [x1, y1, x2, y2] = r, mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
  return [["nw", x1, y1], ["n", mx, y1], ["ne", x2, y1], ["e", x2, my], ["se", x2, y2], ["s", mx, y2], ["sw", x1, y2], ["w", x1, my]];
}
function pushUndo() {
  S.undo.push(JSON.stringify({ car: S.item.car, shapes: S.item.shapes }));
  if (S.undo.length > 60) S.undo.shift();
  S.dirty = true;
}
function undo() {
  const s = S.undo.pop();
  if (!s) return;
  const o = JSON.parse(s); S.item.car = o.car; S.item.shapes = o.shapes; S.sel = null; S.poly = null; draw();
}
function hit(p) {                       // trả {kind, i, part, handle?} theo thứ tự ưu tiên: tay nắm của mục đang chọn → hình (nhỏ nhất) → khung xe
  const tol = HIT / S.scale;
  if (S.sel) {
    const t = S.sel;
    if (t.kind === "shape" && shapes()[t.i].type === "poly") {
      const k = shapes()[t.i].pts.findIndex((q) => Math.hypot(q[0] - p[0], q[1] - p[1]) <= tol);
      if (k >= 0) return { ...t, part: "vertex", k };
    } else if (t.kind === "car" ? S.item.car : true) {
      const r = rectOf(t);
      for (const [n, x, y] of handlesOf(r)) if (Math.hypot(x - p[0], y - p[1]) <= tol) return { ...t, part: "resize", handle: n };
    }
  }
  let best = null, area = Infinity;
  shapes().forEach((s, i) => {
    const b = bboxOf(s.pts);
    if (p[0] >= b[0] - tol && p[0] <= b[2] + tol && p[1] >= b[1] - tol && p[1] <= b[3] + tol) {
      const a = (b[2] - b[0]) * (b[3] - b[1]);
      if (a < area) { area = a; best = { kind: "shape", i, part: "move" }; }
    }
  });
  if (best) return best;
  if (S.item.car) {
    const [x1, y1, x2, y2] = S.item.car;
    const near = Math.min(Math.abs(p[0] - x1), Math.abs(p[0] - x2), Math.abs(p[1] - y1), Math.abs(p[1] - y2)) <= tol;
    const inside = p[0] >= x1 && p[0] <= x2 && p[1] >= y1 && p[1] <= y2;
    if (near && p[0] >= x1 - tol && p[0] <= x2 + tol && p[1] >= y1 - tol && p[1] <= y2 + tol) return { kind: "car", part: "move" };
    if (S.tool === "car" && inside) return { kind: "car", part: "move" };
  }
  return null;
}

// ---------- vẽ ----------
function layout() {
  if (!S.img) return;
  const w = $("stage").clientWidth, ratio = window.devicePixelRatio || 1;
  S.scale = w / S.item.w;
  cv.style.height = Math.round(S.item.h * S.scale) + "px";
  cv.width = Math.round(w * ratio); cv.height = Math.round(S.item.h * S.scale * ratio);
}
function rrect(x, y, w, h2, r) { ctx.beginPath(); ctx.roundRect ? ctx.roundRect(x, y, w, h2, r) : ctx.rect(x, y, w, h2); }
function tag(text, x, y, col) {
  ctx.font = "600 12px " + getComputedStyle(document.body).fontFamily;
  const w = ctx.measureText(text).width + 12, hh = 20, ty = y - hh >= 0 ? y - hh : y;
  ctx.fillStyle = col; rrect(x, ty, w, hh, 5); ctx.fill();
  ctx.fillStyle = "#101010"; ctx.textBaseline = "middle"; ctx.fillText(text, x + 6, ty + hh / 2 + 1);
}
function handle(x, y) { ctx.fillStyle = "#fff"; ctx.strokeStyle = "#111"; ctx.lineWidth = 1.5; ctx.beginPath(); ctx.arc(x, y, HANDLE, 0, 7); ctx.fill(); ctx.stroke(); }
function draw() {
  const ratio = window.devicePixelRatio || 1, k = S.scale;
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, cv.width, cv.height);
  if (!S.item || !S.img) return;
  ctx.drawImage(S.img, 0, 0, S.item.w * k, S.item.h * k);
  const car = S.item.car;
  if (!S.item.legacy) $("st").textContent = car ? "ảnh gốc" : "chưa có khung xe";
  if (car) {                                               // tối phần ngoài khung xe để thấy vùng sẽ được cắt
    const [x1, y1, x2, y2] = car.map((v) => v * k);
    ctx.fillStyle = "rgba(0,0,0,.38)";
    ctx.beginPath(); ctx.rect(0, 0, S.item.w * k, S.item.h * k); ctx.rect(x1, y1, x2 - x1, y2 - y1); ctx.fill("evenodd");
    ctx.setLineDash([8, 5]); ctx.strokeStyle = CAR_COLOR; ctx.lineWidth = S.sel && S.sel.kind === "car" ? 3 : 1.5;
    ctx.strokeRect(x1, y1, x2 - x1, y2 - y1); ctx.setLineDash([]);
    tag("Xe (vùng cắt)", x1, y1, CAR_COLOR);
  }
  shapes().forEach((s, i) => {
    const col = color(s.cls), on = S.sel && S.sel.kind === "shape" && S.sel.i === i;
    ctx.strokeStyle = col; ctx.fillStyle = col + "26"; ctx.lineWidth = on ? 3 : 2;
    let lx, ly;
    if (s.type === "poly") {
      ctx.beginPath(); s.pts.forEach(([x, y], j) => j ? ctx.lineTo(x * k, y * k) : ctx.moveTo(x * k, y * k)); ctx.closePath(); ctx.fill(); ctx.stroke();
      const b = bboxOf(s.pts); lx = b[0] * k; ly = b[1] * k;
    } else {
      const [x1, y1, x2, y2] = bboxOf(s.pts).map((v) => v * k);
      ctx.fillRect(x1, y1, x2 - x1, y2 - y1); ctx.strokeRect(x1, y1, x2 - x1, y2 - y1); lx = x1; ly = y1;
    }
    tag(clsName(s.cls), lx, ly, col);
  });
  if (S.sel) {                                              // tay nắm cho mục đang chọn
    const t = S.sel;
    if (t.kind === "shape" && shapes()[t.i] && shapes()[t.i].type === "poly") shapes()[t.i].pts.forEach(([x, y]) => handle(x * k, y * k));
    else if (t.kind === "car" ? S.item.car : shapes()[t.i]) handlesOf(rectOf(t)).forEach(([, x, y]) => handle(x * k, y * k));
  }
  if (S.poly) {                                             // đa giác đang vẽ
    const col = color(S.cls); ctx.strokeStyle = col; ctx.lineWidth = 2;
    ctx.beginPath(); S.poly.forEach(([x, y], j) => j ? ctx.lineTo(x * k, y * k) : ctx.moveTo(x * k, y * k));
    if (S.mouse) ctx.lineTo(S.mouse[0] * k, S.mouse[1] * k); ctx.stroke();
    S.poly.forEach(([x, y]) => handle(x * k, y * k));
  }
  const d = S.drag;
  if (d && d.mode === "draw") {                             // khung đang kéo
    const [x1, y1, x2, y2] = [Math.min(d.a[0], d.b[0]), Math.min(d.a[1], d.b[1]), Math.max(d.a[0], d.b[0]), Math.max(d.a[1], d.b[1])].map((v) => v * k);
    ctx.strokeStyle = S.tool === "car" ? CAR_COLOR : color(S.cls); ctx.lineWidth = 2; ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
  }
  if (S.mouse && !(d && d.mode !== "draw")) {   // đường ngắm nét đứt
    ctx.setLineDash([4, 4]); ctx.strokeStyle = "rgba(255,255,255,.55)"; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(S.mouse[0] * k, 0); ctx.lineTo(S.mouse[0] * k, S.item.h * k); ctx.moveTo(0, S.mouse[1] * k); ctx.lineTo(S.item.w * k, S.mouse[1] * k); ctx.stroke(); ctx.setLineDash([]);
  }
}

// ---------- chuột ----------
function applyResize(r0, handleName, p) {
  let [x1, y1, x2, y2] = r0;
  if (handleName.includes("w")) x1 = p[0]; if (handleName.includes("e")) x2 = p[0];
  if (handleName.includes("n")) y1 = p[1]; if (handleName.includes("s")) y2 = p[1];
  return [Math.min(x1, x2), Math.min(y1, y2), Math.max(x1, x2), Math.max(y1, y2)];
}
function setRect(target, r) {
  if (target.kind === "car") { S.item.car = r.map(Math.round); return; }
  const s = shapes()[target.i], b = bboxOf(s.pts);
  const sx = (r[2] - r[0]) / Math.max(1e-6, b[2] - b[0]), sy = (r[3] - r[1]) / Math.max(1e-6, b[3] - b[1]);
  s.pts = s.pts.map(([x, y]) => [clampX(r[0] + (x - b[0]) * sx), clampY(r[1] + (y - b[1]) * sy)]);
}
cv.addEventListener("pointerdown", (e) => {
  if (!S.item || e.button === 2) return;
  const p = toImg(e);
  cv.setPointerCapture(e.pointerId);
  if (S.poly) {                                              // đang vẽ đa giác: thêm điểm / đóng
    if (S.poly.length >= 3 && Math.hypot(S.poly[0][0] - p[0], S.poly[0][1] - p[1]) <= HIT / S.scale) finishPoly(); else S.poly.push(p);
    draw(); return;
  }
  const t = hit(p);
  if (t) {
    S.sel = { kind: t.kind, i: t.i };
    S.drag = { mode: t.part, t, start: p, orig: JSON.parse(JSON.stringify(t.kind === "car" ? S.item.car : shapes()[t.i].pts)), r0: rectOf(t),
      snap: JSON.stringify({ car: S.item.car, shapes: S.item.shapes }), pushed: false };
    if (t.kind === "shape") { S.cls = shapes()[t.i].cls; renderClasses(); }
  } else if (S.tool === "poly") {
    S.sel = null; S.poly = [p];
  } else {
    S.sel = null; S.drag = { mode: "draw", a: p, b: p };
  }
  draw();
});
cv.addEventListener("pointermove", (e) => {
  if (!S.item) return;
  const p = toImg(e); S.mouse = p;
  const d = S.drag;
  if (d && d.mode !== "draw" && !d.pushed && Math.hypot(p[0] - d.start[0], p[1] - d.start[1]) * S.scale >= 2) {   // chỉ ghi hoàn tác khi thật sự kéo
    S.undo.push(d.snap); S.dirty = true; d.pushed = true;
  }
  if (d && d.mode !== "draw" && !d.pushed) { draw(); return; }
  if (d && d.mode === "draw") d.b = p;
  else if (d && d.mode === "move") {
    const dx = p[0] - d.start[0], dy = p[1] - d.start[1];
    if (d.t.kind === "car") { const [x1, y1, x2, y2] = d.orig; const ddx = Math.min(S.item.w - x2, Math.max(-x1, dx)), ddy = Math.min(S.item.h - y2, Math.max(-y1, dy)); S.item.car = [x1 + ddx, y1 + ddy, x2 + ddx, y2 + ddy].map(Math.round); }
    else { const b = bboxOf(d.orig); const ddx = Math.min(S.item.w - b[2], Math.max(-b[0], dx)), ddy = Math.min(S.item.h - b[3], Math.max(-b[1], dy)); shapes()[d.t.i].pts = d.orig.map(([x, y]) => [x + ddx, y + ddy]); }
  } else if (d && d.mode === "resize") {
    const r = applyResize(d.r0, d.t.handle, p);
    if (d.t.kind === "car") S.item.car = r.map(Math.round);
    else { shapes()[d.t.i].pts = d.orig; setRect(d.t, r); }
  } else if (d && d.mode === "vertex") shapes()[d.t.i].pts[d.t.k] = p;
  draw();
});
cv.addEventListener("pointerup", () => {
  const d = S.drag; S.drag = null;
  if (!d) return;
  if (d.mode === "draw") {
    const r = [Math.min(d.a[0], d.b[0]), Math.min(d.a[1], d.b[1]), Math.max(d.a[0], d.b[0]), Math.max(d.a[1], d.b[1])];
    if ((r[2] - r[0]) * S.scale >= MIN_PX && (r[3] - r[1]) * S.scale >= MIN_PX) {
      pushUndo();
      if (S.tool === "car") { S.item.car = r.map(Math.round); S.sel = { kind: "car" }; }
      else { shapes().push({ cls: S.cls, type: "box", pts: [[r[0], r[1]], [r[2], r[3]]] }); S.sel = { kind: "shape", i: shapes().length - 1 }; }
    }
  }
  draw();
});
cv.addEventListener("contextmenu", (e) => {
  e.preventDefault();
  if (!S.item) return;
  const t = hit(toImg(e));
  if (t) removeTarget(t);
});
cv.addEventListener("dblclick", () => { if (S.poly && S.poly.length >= 3) { S.poly.pop(); finishPoly(); } });
cv.addEventListener("pointerleave", () => { S.mouse = null; draw(); });

function finishPoly() {
  if (S.poly && S.poly.length >= 3) {
    pushUndo();
    shapes().push({ cls: S.cls, type: "poly", pts: S.poly });
    S.sel = { kind: "shape", i: shapes().length - 1 };
  }
  S.poly = null; draw();
}
function removeTarget(t) {
  pushUndo();
  if (t.kind === "car") S.item.car = null; else shapes().splice(t.i, 1);
  S.sel = null; draw();
}

// ---------- thao tác ----------
function syncTools() {
  document.querySelectorAll("[data-tool]").forEach((b) => {
    b.setAttribute("aria-pressed", String(b.dataset.tool === S.tool));
    if (b.dataset.tool === "car") b.hidden = !!(S.item && S.item.legacy);
  });
}
function setTool(t) { if (t === "car" && S.item && S.item.legacy) return; S.tool = t; S.poly = null; syncTools(); draw(); }
function pickClass(i) {
  S.cls = i;
  if (S.sel && S.sel.kind === "shape" && shapes()[S.sel.i]) { pushUndo(); shapes()[S.sel.i].cls = i; }
  renderClasses(); draw();
}
async function renameClass(i) {
  const n = prompt("Tên hiển thị mới cho lớp này:", clsName(i));
  if (n == null) return;
  try { await api("/api/ds/class", { id: S.classes[i], name: n }); await refreshClasses(); } catch (e) { toast(e.message); }
}
async function refreshClasses() { const d = await api("/api/ds/list"); S.classes = d.classes; S.names = d.names; renderClasses(); draw(); }
async function save(next) {
  if (!S.item) return;
  try {
    const r = await api("/api/ds/save", { name: S.item.name, car: S.item.car, shapes: S.item.shapes });
    S.dirty = false; toast(`Đã lưu (${r.boxes} khung)`);
    const it = S.items[S.idx]; it.verified = true; it.boxes = r.boxes;
    if (next && S.idx < S.items.length - 1) await open(S.idx + 1, true);
    else { await loadList(S.item.name); }
  } catch (e) { toast(e.message); }
}
async function removeImage() {
  if (!S.item || !confirm("Chuyển ảnh này vào thùng rác (dataset_hu_hong/_deleted)?")) return;
  try { await api("/api/ds/delete", { name: S.item.name }); S.dirty = false; S.idx = Math.max(0, S.idx - 1); await loadList(); } catch (e) { toast(e.message); }
}

$("b-save").addEventListener("click", () => save(true));
$("b-prev").addEventListener("click", () => S.idx > 0 && open(S.idx - 1));
$("b-next").addEventListener("click", () => S.idx < S.items.length - 1 && open(S.idx + 1));
$("b-del").addEventListener("click", removeImage);
document.querySelectorAll("[data-tool]").forEach((b) => b.addEventListener("click", () => setTool(b.dataset.tool)));
$("b-cap").addEventListener("click", async () => {
  try {
    const r = await api("/api/dataset/capture", {});
    toast(r.car ? "Đã chụp " + r.name : "Đã chụp – không thấy xe, hãy vẽ khung xe (phím C)");
    S.dirty = false; S.idx = 0; await loadList(r.name);
  } catch (e) { toast(e.message); }
});
// ---------- camera (dùng chung API với tab Trạm) ----------
async function loadCams() {
  try {
    const c = await api("/api/cameras"), sel = $("cam-pick");
    while (sel.firstChild) sel.removeChild(sel.firstChild);
    for (const o of c.options) sel.append(h("option", { value: o.id }, o.label));
    if (c.current === "custom") sel.append(h("option", { value: "custom", disabled: true }, "Nguồn khác (Cài đặt)"));
    sel.value = c.current;
  } catch (e) { toast(e.message); }
}
$("cam-refresh").addEventListener("click", loadCams);
$("cam-pick").addEventListener("change", async (e) => {
  try { await api("/api/camera", { id: e.target.value }); toast("Đang chuyển camera…"); } catch (err) { toast(err.message); loadCams(); }
});
function refreshPreview() {                                  // hình camera kiểm tra (chính là nguồn của nút Chụp), ~1 khung/giây
  if (document.hidden) return;
  const im = $("cam-prev"), n = new Image();
  n.onload = () => { im.src = n.src; };
  n.src = "/snapshot.jpg?cam=inspect&t=" + Date.now();
}
setInterval(refreshPreview, 1000); refreshPreview(); loadCams();
$("b-up").addEventListener("click", () => $("f-up").click());
$("f-up").addEventListener("change", async (e) => {
  let last = null, n = 0;
  for (const f of e.target.files) {
    try { last = (await api("/api/dataset/upload", f)).name; n++; } catch (err) { toast(f.name + ": " + err.message); }
  }
  e.target.value = "";
  if (n) { toast(`Đã tải ${n} ảnh`); S.dirty = false; await loadList(last); }
});
$("new-cls").addEventListener("keydown", async (e) => {
  if (e.key !== "Enter") return;
  e.stopPropagation();
  try { const r = await api("/api/ds/class", { name: e.target.value }); e.target.value = ""; await refreshClasses(); S.cls = S.classes.indexOf(r.id); renderClasses(); } catch (err) { toast(err.message); }
});
document.addEventListener("keydown", (e) => {
  if (e.target.tagName === "INPUT") return;
  const k = e.key.toLowerCase();
  if (e.ctrlKey && k === "z") { e.preventDefault(); undo(); return; }
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  if (/^[1-9]$/.test(k)) { if (+k <= S.classes.length) pickClass(+k - 1); return; }
  if (k === "b") setTool("box");
  else if (k === "p") setTool("poly");
  else if (k === "c") setTool("car");
  else if (k === "a") S.idx > 0 && open(S.idx - 1);
  else if (k === "d") S.idx < S.items.length - 1 && open(S.idx + 1);
  else if (k === "s") save(true);
  else if (k === "enter" && S.poly) finishPoly();
  else if (k === "escape") { S.poly = null; S.sel = null; S.drag = null; draw(); }
  else if ((k === "delete" || k === "backspace") && S.sel) { e.preventDefault(); removeTarget(S.sel); }
});
window.addEventListener("resize", () => { layout(); draw(); });
window.addEventListener("beforeunload", (e) => { if (S.dirty) { e.preventDefault(); e.returnValue = ""; } });
loadList().catch((e) => toast(e.message));
