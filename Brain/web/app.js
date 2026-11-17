"use strict";
// Giao diện trạm kiểm tra – thuần HTML/JS, không build. Mọi chữ lấy từ server đều đưa vào DOM bằng textContent (không innerHTML).
const $ = (id) => document.getElementById(id);
const STATE_LABEL = { CHO_XE: "Chờ xe", XE_VAO: "Xe vào trạm", DANG_QUET: "Đang quét", CO_KET_QUA: "Có kết quả" };
const CLASS_VI = { kinh_vo: "Kính vỡ / nứt", mop_than: "Móp thân xe", lop_hong: "Lốp hỏng / xẹp" };
const money = (v) => new Intl.NumberFormat("vi-VN").format(v) + " ₫";

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
const clear = (e) => { while (e.firstChild) e.removeChild(e.firstChild); return e; };
function toast(msg) { const t = h("div", { class: "toast" }, msg); document.body.append(t); setTimeout(() => t.remove(), 2800); }
async function api(path, body) {
  const opt = body === undefined ? {} : { method: "POST", headers: { "X-Tram": "1", "Content-Type": body instanceof Blob ? "application/octet-stream" : "application/json" }, body: body instanceof Blob ? body : JSON.stringify(body) };
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.err || r.statusText);
  return j;
}

const reduceMotion = () => matchMedia("(prefers-reduced-motion: reduce)").matches;
const loading = (box) => clear(box).append(h("span", { class: "skel w8" }), h("span", { class: "skel w6" }));
const restartAnim = (e, cls) => { e.classList.remove(cls); void e.offsetWidth; e.classList.add(cls); };
function countUp(el, value) {                               // đếm tăng tổng tiền khi có kết quả mới (chỉ là hiệu ứng trên 1 nút DOM)
  if (reduceMotion() || !value) { el.textContent = money(value); return; }
  const t0 = performance.now(), dur = 900;
  const step = (t) => { const k = Math.min(1, (t - t0) / dur); el.textContent = money(Math.round(value * (1 - Math.pow(1 - k, 3)))); if (k < 1 && el.isConnected) requestAnimationFrame(step); };
  requestAnimationFrame(step);
}

// ---------- Thẻ kết quả (dùng ở Trạm, Báo cáo, Thử ảnh) ----------
function renderReport(box, rep, opts = {}) {
  clear(box).append(h("h2", {}, opts.title || "Kết quả kiểm tra"));
  box.classList.remove("reveal");
  if (!rep) {
    if (opts.busy) box.append(h("span", { class: "skel w8" }), h("span", { class: "skel w6" }), h("span", { class: "skel w8" }));
    else box.append(h("div", { class: "empty" }, h("span", { class: "ico i-car" }), h("p", {}, "Chưa có xe nào được kiểm tra. Đặt xe vào vị trí kiểm tra.")));
    return;
  }
  const imgBase = opts.imgBase || "/inspections/";
  box.append(h("dl", { class: "kv" },
    h("dt", {}, "Biển số"), h("dd", {}, rep.plate ? h("span", { class: "plate" }, rep.plate_display) : h("span", { class: "muted" }, "Chưa đọc được")),
    h("dt", {}, "Chủ xe"), h("dd", {}, rep.owner || (rep.plate ? "Xe lạ (chưa đăng ký)" : h("span", { class: "muted" }, "Không rõ"))),
    h("dt", {}, "Loại xe"), h("dd", {}, rep.vehicle_type || h("span", { class: "muted" }, "Không rõ")),
    h("dt", {}, "Thời gian"), h("dd", {}, rep.time + (rep.ms ? ` • ${rep.ms} ms` : ""))));
  if (rep.need_staff) box.append(h("div", { class: "banner warn" }, "Cần nhân viên kiểm tra", h("ul", {}, (rep.need_reasons || []).map((r) => h("li", {}, r)))));
  else box.append(h("div", { class: "banner ok" }, "Kết quả đủ tin cậy"));
  box.append(h("h2", { style: "margin-top:14px" }, `Hư hỏng phát hiện (${rep.findings.length})`));
  if (!rep.damage_model_loaded) box.append(h("p", { class: "muted" }, "Chưa có mô hình hư hỏng (hu_hong.onnx), chưa thể kết luận xe có hỏng hay không."));
  else if (!rep.findings.length) box.append(h("p", { class: "muted" }, "Không thấy hư hỏng."));
  rep.findings.forEach((f, i) => {
    const src = f.crop ? imgBase + f.crop : null, pct = Math.round(f.conf * 100);
    box.append(h("div", { class: "find" }, src ? h("img", { src, alt: f.class, loading: "lazy" }) : h("div"),
      h("div", {}, h("b", {}, CLASS_VI[f.class] || f.class), h("div", { class: "muted" }, `${f.frames} khung hình`),
        h("div", { class: "meter" }, h("i", { style: `width:${pct}%;--i:${i}` }))),
      h("span", { class: "conf" }, pct + "%")));
  });
  box.append(h("h2", { style: "margin-top:16px" }, "Dịch vụ đề xuất"));
  let totalEl = null, total = 0;
  if (!rep.recommendations.length) box.append(h("p", { class: "muted" }, "Không có dịch vụ nào được đề xuất."));
  else {
    total = rep.recommendations.reduce((s, r) => s + r.price, 0);
    totalEl = h("td", { class: "price" }, money(total));
    box.append(h("table", { class: "svc" }, h("tbody", {}, rep.recommendations.map((r) => h("tr", {}, h("td", {}, h("b", {}, r.service), h("div", { class: "muted" }, r.reason)), h("td", { class: "price" }, money(r.price)))),
      h("tr", { class: "total" }, h("td", {}, h("b", {}, "Tạm tính")), totalEl))));
  }
  if (opts.animate) {                                       // chỉ chạy hiệu ứng khi có báo cáo MỚI, không phải mỗi lần poll
    [...box.children].forEach((c, i) => c.style.setProperty("--i", i));
    restartAnim(box, "reveal");
    if (totalEl) countUp(totalEl, total);
  }
}

// ---------- Tab ----------
let curTab = "tram";
function showTab(name) {
  curTab = name;
  document.querySelectorAll("#tabs button").forEach((b) => b.setAttribute("aria-selected", String(b.dataset.tab === name)));
  moveTabInd();
  document.querySelectorAll("main > section").forEach((s) => { s.hidden = s.id !== "tab-" + name; });
  if (name === "baocao") loadReports().catch(loadFail($("rep-list")));
  if (name === "data") loadData().catch(loadFail($("ds-stat")));
  if (name === "caidat") loadSettings().catch(loadFail($("cfg-form")));
  if (name === "tram" && !camsLoaded) { camsLoaded = true; loadCams(); }
  const streaming = name === "tram";                      // chỉ mở luồng MJPEG khi đang xem tab Trạm
  for (const r of ["overview", "inspect"]) { const im = $("vid-" + r); const want = streaming ? `/video?cam=${r}` : ""; if (want && im.getAttribute("src") !== want) im.src = want; if (!want) im.removeAttribute("src"); }
}
function moveTabInd() {                                    // thanh trượt dưới tab đang chọn
  const b = document.querySelector('#tabs button[aria-selected="true"]'), ind = $("tab-ind");
  if (!b) return;
  ind.style.width = b.offsetWidth + "px"; ind.style.transform = `translateX(${b.offsetLeft}px)`;
}
addEventListener("resize", moveTabInd);
if (document.fonts) document.fonts.ready.then(moveTabInd);
document.querySelectorAll("#tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
$("theme").addEventListener("click", () => {
  const cur = document.documentElement.dataset.theme || (matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
  const next = cur === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = next;
  try { localStorage.setItem("theme", next); } catch (e) { /* bỏ qua */ }
});
try { const t = localStorage.getItem("theme"); if (t) document.documentElement.dataset.theme = t; } catch (e) { /* bỏ qua */ }

// ---------- Trạm (live) ----------
let lastReportId, camsLoaded = false, prevState = null, prevLog = "", prevLogHead = null;
const STEPS = ["CHO_XE", "XE_VAO", "DANG_QUET", "CO_KET_QUA"];
async function loadCams() {
  try {
    const c = await api("/api/cameras"), sel = clear($("cam-pick"));
    for (const o of c.options) sel.append(h("option", { value: o.id }, o.label));
    if (c.current === "custom") sel.append(h("option", { value: "custom", disabled: true }, "Nguồn khác (Cài đặt)"));
    sel.value = c.current;
  } catch (e) { toast(e.message); }
}
$("cam-refresh").addEventListener("click", loadCams);
$("cam-pick").addEventListener("change", async (e) => {
  try { await api("/api/camera", { id: e.target.value }); toast("Đang chuyển camera…"); } catch (err) { toast(err.message); loadCams(); }
});
async function poll() {
  try {
    const s = await api("/api/state");
    const chip = $("chip");
    if (s.state !== prevState) {                           // đổi trạng thái: cập nhật khung ngắm, đường ray, chạy hiệu ứng 1 lần
      chip.className = "chip " + s.state; chip.textContent = STATE_LABEL[s.state] || s.state;
      if (prevState !== null) restartAnim(chip, "flip");
      document.querySelectorAll(".video-wrap").forEach((w) => { w.dataset.state = s.state; });
      const k = STEPS.indexOf(s.state);
      document.querySelectorAll("#rail li").forEach((li, i) => { li.className = i < k ? "done" : i === k ? "cur" : ""; });
      prevState = s.state;
    }
    const pct = Math.round(s.progress * 100), ring = $("prog");
    ring.style.setProperty("--p", pct); ring.setAttribute("aria-valuenow", pct); $("prog-pct").textContent = pct + "%";
    const e = $("hdr-esp"); e.className = "pill " + (s.esp.connected ? "ok" : "bad");
    e.lastChild.textContent = s.esp.connected ? `ESP32 ${s.esp.port}` : "ESP32 chưa nối";
    const cam = s.cams.overview, msg = $("vid-msg");
    msg.hidden = cam.ok; msg.textContent = cam.ok ? "" : (cam.err || "Chưa có camera");
    $("cam-active").textContent = cam.active == null ? "" : "Đang dùng: " + cam.active;
    $("wrap-inspect").hidden = !(s.cams.inspect && s.cams.inspect.src !== cam.src) || curTab !== "tram";
    const strip = clear($("strip"));
    const pill = (ok, text) => h("span", { class: "pill " + (ok ? "ok" : "warn") }, h("span", { class: "dot" }), text);
    strip.append(pill(!!s.models.car, "Xe: " + s.models.car), pill(!!s.models.plate, "Biển: " + (s.models.plate || "không có YOLO biển")),
      pill(s.models.damage_loaded, s.models.damage_loaded ? "Hư hỏng: " + s.models.damage : "Chưa có mô hình hư hỏng"));
    const lines = s.events.map((x) => `${x.ts}  ${x.msg}`).concat(s.logs), joined = lines.join("\n");
    if (joined !== prevLog) {                              // chỉ dựng lại nhật ký khi có dòng mới; dòng đầu mới thì nháy sáng
      const fresh = prevLogHead !== null && lines[0] !== prevLogHead;
      clear($("log")).append(...lines.map((l, i) => h("div", { class: fresh && i === 0 ? "new" : "" }, l)));
      prevLog = joined; prevLogHead = lines[0];
    }
    const id = s.report ? s.report.id : null;
    if (id !== lastReportId) { renderReport($("result"), s.report, { animate: lastReportId !== undefined && !!s.report }); lastReportId = id; }
  } catch (err) { $("hdr-esp").className = "pill bad"; }
}
setInterval(() => { if (!document.hidden) poll(); }, 1000);

// ---------- Báo cáo ----------
const loadFail = (box) => (e) => { clear(box).append(h("p", { class: "err" }, "Không tải được: " + e.message)); };
async function loadReports() {
  const tb = $("rep-list");
  clear(tb).append(h("tr", {}, h("td", { colspan: 5 }, h("span", { class: "skel w8" }), h("span", { class: "skel w6" }))));
  const rows = await api("/api/inspections?limit=100");
  clear(tb); $("rep-empty").hidden = rows.length > 0;
  for (const r of rows) {
    const tr = h("tr", { class: "click", onclick: () => { tb.querySelectorAll("tr.sel").forEach((x) => x.classList.remove("sel")); tr.classList.add("sel"); showReport(r.id); } },
      h("td", { class: "num" }, r.id), h("td", { class: "num" }, r.ts), h("td", {}, r.display ? h("b", {}, r.display) : h("span", { class: "muted" }, "Chưa đọc")),
      h("td", {}, r.owner || h("span", { class: "muted" }, "Không rõ")), h("td", {}, h("span", { class: "tag" + (r.need_staff ? " warn" : "") }, r.need_staff ? "Có" : "Không")));
    tb.append(tr);
  }
}
async function showReport(id) {
  const box = $("rep-detail"); renderReport(box, null, { title: "Báo cáo #" + id, busy: true });
  try { renderReport(box, await api("/api/inspection?id=" + id), { title: "Báo cáo #" + id, animate: true }); } catch (e) { loadFail(box)(e); }
}

// ---------- Dữ liệu ----------
async function loadData() {
  loading($("ds-stat"));
  const d = await api("/api/dataset"), s = await api("/api/state");
  const st = clear($("ds-stat"));
  st.append(h("div", {}, h("b", {}, d.images), "ảnh xe"), h("div", {}, h("b", {}, d.labelled), "ảnh đã gán nhãn"), h("div", {}, h("b", {}, d.pending || 0), "ảnh chờ gán nhãn"));
  for (const c of d.classes) st.append(h("div", {}, h("b", {}, d.boxes[c]), (d.names && d.names[c]) || CLASS_VI[c] || c));
  $("ds-note").textContent = s.models.damage_loaded ? "" : "Chưa có mô hình hư hỏng. Cần ~150 khung mỗi lớp + ~100 ảnh xe lành rồi huấn luyện bằng train_hu_hong.ipynb trên Google Colab, chép hu_hong.onnx vào SmartPK_PC/ và chạy lại tram_ai.";
  const m = s.models, tb = clear($("models"));
  const row = (a, b) => tb.append(h("tr", {}, h("td", { class: "muted" }, a), h("td", {}, b)));
  row("Xe", `${m.car} (lớp: ${Object.values(m.car_names).join(", ")})`);
  row("Biển số", `${m.plate || "chưa có"} • OCR ${m.plate_reader ? "sẵn sàng" : "KHÔNG chạy"}`);
  row("Hư hỏng", m.damage_loaded ? m.damage : (m.damage_err || "chưa có hu_hong.onnx"));
}
$("ds-capture").addEventListener("click", async () => {
  try { const r = await api("/api/dataset/capture", {}); toast("Đã lưu " + r.name); loadData(); } catch (e) { toast(e.message); }
});

// ---------- Cài đặt ----------
let loadedSrc = {};
async function loadSettings() {
  loading($("cfg-form"));
  const c = await api("/api/config"), f = clear($("cfg-form"));
  loadedSrc = { overview: String(c.cameras.overview.src), inspect: String(c.cameras.inspect.src) };
  const field = (label, id, val, attrs = {}) => f.append(h("label", { class: "f" }, label, h("input", { id, value: val, ...attrs })));
  field("Camera tổng quan", "c-ov", c.cameras.overview.src);
  field("Camera kiểm tra", "c-in", c.cameras.inspect.src);
  field("Ngưỡng nhận xe", "c-car", c.car_conf, { type: "number", step: "0.05", min: "0.15", max: "0.95" });
  field("Ngưỡng hư hỏng", "c-dmg", c.damage_conf, { type: "number", step: "0.05", min: "0.05", max: "0.95" });
  field("Số khung quét", "c-k", c.inspection.scan_frames, { type: "number", min: "1", max: "60" });
  field("Tỉ lệ khung có lỗi", "c-ratio", c.inspection.damage_ratio, { type: "number", step: "0.05", min: "0.05", max: "1" });
  await loadPorts(); await loadOwners();
}
$("cfg-save").addEventListener("click", async () => {
  try {
    const cameras = {};                                    // chỉ gửi nguồn camera khi ô bị sửa: không ghi đè lựa chọn ở tab Trạm
    if ($("c-ov").value !== loadedSrc.overview) cameras.overview = { src: $("c-ov").value };
    if ($("c-in").value !== loadedSrc.inspect) cameras.inspect = { src: $("c-in").value };
    await api("/api/config", { cameras, car_conf: +$("c-car").value, damage_conf: +$("c-dmg").value, inspection: { scan_frames: +$("c-k").value, damage_ratio: +$("c-ratio").value } });
    toast("Đã lưu");
  } catch (e) { toast(e.message); }
});
async function loadPorts() {
  const ports = await api("/api/ports"), sel = clear($("port"));
  if (!ports.length) sel.append(h("option", { value: "" }, "Không thấy cổng COM"));
  for (const p of ports) sel.append(h("option", { value: p.port }, `${p.port}: ${p.kind}${p.esp ? " (ESP32?)" : ""}`));
}
$("port-refresh").addEventListener("click", loadPorts);
$("port-connect").addEventListener("click", async () => {
  try { const r = await api("/api/esp/connect", { port: $("port").value }); toast(r.ok ? "Đã kết nối" : "Không mở được cổng (đóng Serial Monitor?)"); } catch (e) { toast(e.message); }
});
for (const [id, cmd] of [["servo-open", "open"], ["servo-sweep", "sweep"]]) {
  $(id).addEventListener("click", async () => {
    try { const r = await api("/api/esp/test", { cmd }); toast(r.ok ? "Đã gửi lệnh tới ESP32" : "Chưa nối ESP32"); } catch (e) { toast(e.message); }
  });
}
async function loadOwners() {
  const rows = await api("/api/owners"), tb = clear($("owners"));
  for (const o of rows) tb.append(h("tr", {}, h("td", {}, o.display), h("td", {}, o.name), h("td", {}, o.phone || ""), h("td", {}, h("button", { class: "btn ghost", onclick: async () => { await api("/api/owner/delete", { plate: o.plate }); loadOwners(); } }, "Xoá"))));
}
$("o-save").addEventListener("click", async () => {
  try { await api("/api/owner/save", { plate: $("o-plate").value, name: $("o-name").value, phone: $("o-phone").value }); toast("Đã lưu chủ xe"); loadOwners(); } catch (e) { toast(e.message); }
});

// ---------- Thử ảnh ----------
async function testFile(file) {
  if (!file) return;
  $("test-out").hidden = false; renderReport($("test-result"), null, { title: "Đang xử lý…", busy: true });
  try {
    const rep = await api("/api/test_image", file);
    $("test-img").src = rep.img; renderReport($("test-result"), rep, { title: `Kết quả (${rep.n_cars} xe)`, imgBase: "/inspections/", animate: true });
  } catch (e) { toast(e.message); loadFail($("test-result"))(e); }
}
const drop = $("drop");
drop.addEventListener("click", () => $("file").click());
drop.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); $("file").click(); } });
$("file").addEventListener("change", (e) => testFile(e.target.files[0]));
["dragover", "dragenter"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); }));
["dragleave", "drop"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); }));
drop.addEventListener("drop", (e) => testFile(e.dataTransfer.files[0]));

showTab("tram");
poll();
