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

// ---------- Thẻ kết quả (dùng ở Trạm, Báo cáo, Thử ảnh) ----------
function renderReport(box, rep, opts = {}) {
  clear(box).append(h("h2", {}, opts.title || "Kết quả kiểm tra"));
  if (!rep) { box.append(h("p", { class: "muted" }, "Chưa có xe nào được kiểm tra. Đặt xe vào vị trí kiểm tra.")); return; }
  const imgBase = opts.imgBase || "/inspections/";
  box.append(h("dl", { class: "kv" },
    h("dt", {}, "Biển số"), h("dd", {}, rep.plate ? h("span", { class: "plate" }, rep.plate_display) : h("span", { class: "muted" }, "Chưa đọc được")),
    h("dt", {}, "Chủ xe"), h("dd", {}, rep.owner || (rep.plate ? "Xe lạ (chưa đăng ký)" : "—")),
    h("dt", {}, "Loại xe"), h("dd", {}, rep.vehicle_type || "—"),
    h("dt", {}, "Thời gian"), h("dd", {}, rep.time + (rep.ms ? ` • ${rep.ms} ms` : ""))));
  if (rep.need_staff) box.append(h("div", { class: "banner warn" }, "Cần nhân viên kiểm tra", h("ul", {}, (rep.need_reasons || []).map((r) => h("li", {}, r)))));
  else box.append(h("div", { class: "banner ok" }, "Kết quả đủ tin cậy"));
  box.append(h("h2", { style: "margin-top:14px" }, `Hư hỏng phát hiện (${rep.findings.length})`));
  if (!rep.damage_model_loaded) box.append(h("p", { class: "muted" }, "Chưa có mô hình hư hỏng (hu_hong.onnx) – chưa thể kết luận xe có hỏng hay không."));
  else if (!rep.findings.length) box.append(h("p", { class: "muted" }, "Không thấy hư hỏng."));
  for (const f of rep.findings) {
    const src = f.crop ? imgBase + f.crop : null;
    box.append(h("div", { class: "find" }, src ? h("img", { src, alt: f.class, loading: "lazy" }) : h("div"),
      h("div", {}, h("b", {}, CLASS_VI[f.class] || f.class), h("span", { class: "muted" }, ` • ${Math.round(f.conf * 100)}% • ${f.frames} khung hình`),
        h("div", { class: "meter" }, h("i", { style: `width:${Math.round(f.conf * 100)}%` })))));
  }
  box.append(h("h2", { style: "margin-top:14px" }, "Dịch vụ đề xuất"));
  if (!rep.recommendations.length) box.append(h("p", { class: "muted" }, "Không có dịch vụ nào được đề xuất."));
  else {
    const total = rep.recommendations.reduce((s, r) => s + r.price, 0);
    box.append(h("table", {}, h("tbody", {}, rep.recommendations.map((r) => h("tr", {}, h("td", {}, h("b", {}, r.service), h("div", { class: "muted" }, r.reason)), h("td", { class: "price" }, money(r.price)))),
      h("tr", {}, h("td", {}, h("b", {}, "Tạm tính")), h("td", { class: "price" }, h("b", {}, money(total)))))));
  }
}

// ---------- Tab ----------
let curTab = "tram";
function showTab(name) {
  curTab = name;
  document.querySelectorAll("#tabs button").forEach((b) => b.setAttribute("aria-selected", String(b.dataset.tab === name)));
  document.querySelectorAll("main > section").forEach((s) => { s.hidden = s.id !== "tab-" + name; });
  if (name === "baocao") loadReports();
  if (name === "data") loadData();
  if (name === "caidat") loadSettings();
  if (name === "tram" && !camsLoaded) { camsLoaded = true; loadCams(); }
  const streaming = name === "tram";                      // chỉ mở luồng MJPEG khi đang xem tab Trạm
  for (const r of ["overview", "inspect"]) { const im = $("vid-" + r); const want = streaming ? `/video?cam=${r}` : ""; if (want && im.getAttribute("src") !== want) im.src = want; if (!want) im.removeAttribute("src"); }
}
document.querySelectorAll("#tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
$("theme").addEventListener("click", () => {
  const cur = document.documentElement.dataset.theme || (matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
  const next = cur === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = next;
  try { localStorage.setItem("theme", next); } catch (e) { /* bỏ qua */ }
});
try { const t = localStorage.getItem("theme"); if (t) document.documentElement.dataset.theme = t; } catch (e) { /* bỏ qua */ }

// ---------- Trạm (live) ----------
let lastReportId = null, camsLoaded = false;
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
    const chip = $("chip"); chip.className = "chip " + s.state; chip.textContent = STATE_LABEL[s.state] || s.state;
    $("prog").style.width = Math.round(s.progress * 100) + "%";
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
    $("log").textContent = (s.events.map((x) => `${x.ts}  ${x.msg}`).concat(s.logs)).join("\n");
    const id = s.report ? s.report.id : null;
    if (id !== lastReportId) { lastReportId = id; renderReport($("result"), s.report); }
  } catch (err) { $("hdr-esp").className = "pill bad"; }
}
setInterval(() => { if (!document.hidden) poll(); }, 1000);

// ---------- Báo cáo ----------
async function loadReports() {
  const rows = await api("/api/inspections?limit=100");
  const tb = clear($("rep-list")); $("rep-empty").hidden = rows.length > 0;
  for (const r of rows) tb.append(h("tr", { class: "click", onclick: () => showReport(r.id) }, h("td", {}, r.id), h("td", {}, r.ts), h("td", {}, r.display || "—"), h("td", {}, r.owner || "—"), h("td", {}, r.need_staff ? "Có" : "Không")));
}
async function showReport(id) { renderReport($("rep-detail"), await api("/api/inspection?id=" + id), { title: "Báo cáo #" + id }); }

// ---------- Dữ liệu ----------
async function loadData() {
  const d = await api("/api/dataset"), s = await api("/api/state");
  const st = clear($("ds-stat"));
  st.append(h("div", {}, h("b", {}, d.images), "ảnh xe"), h("div", {}, h("b", {}, d.labelled), "ảnh đã gán nhãn"));
  for (const c of d.classes) st.append(h("div", {}, h("b", {}, d.boxes[c]), CLASS_VI[c] || c));
  $("ds-note").textContent = s.models.damage_loaded ? "" : "Chưa có mô hình hư hỏng. Cần ~150 khung mỗi lớp + ~100 ảnh xe lành rồi chạy train_hu_hong.py (Giai đoạn 3).";
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
  for (const p of ports) sel.append(h("option", { value: p.port }, `${p.port} – ${p.kind}${p.esp ? " (ESP32?)" : ""}`));
}
$("port-refresh").addEventListener("click", loadPorts);
$("port-connect").addEventListener("click", async () => {
  try { const r = await api("/api/esp/connect", { port: $("port").value }); toast(r.ok ? "Đã kết nối" : "Không mở được cổng (đóng Serial Monitor?)"); } catch (e) { toast(e.message); }
});
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
  $("test-out").hidden = false; renderReport($("test-result"), null, { title: "Đang xử lý…" });
  try {
    const rep = await api("/api/test_image", file);
    $("test-img").src = rep.img; renderReport($("test-result"), rep, { title: `Kết quả (${rep.n_cars} xe)`, imgBase: "/inspections/" });
  } catch (e) { toast(e.message); }
}
const drop = $("drop");
drop.addEventListener("click", () => $("file").click());
$("file").addEventListener("change", (e) => testFile(e.target.files[0]));
["dragover", "dragenter"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); }));
["dragleave", "drop"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); }));
drop.addEventListener("drop", (e) => testFile(e.dataTransfer.files[0]));

showTab("tram");
poll();
