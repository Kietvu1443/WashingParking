// ============================================================================
//  WASHINGPARKING · Giao diện 3D tương tác (Three.js) · phạm vi: ô tô và xe tải
//  MÔ PHỎNG chạy hoàn toàn trong trình duyệt (logic.js). TRỰC TIẾP đọc /api/bays.
// ============================================================================
import * as THREE from 'three';
import { OrbitControls } from './vendor/OrbitControls.js';
import * as L from './logic.js';
import { makeVehicle, textSprite, PALETTE } from './models.js';
import * as WD from './world.js';

const $ = (id) => document.getElementById(id);
const root = $('viewport');
const fmtMoney = (n) => n.toLocaleString('vi-VN') + 'đ';
const fmtT = (t) => `${String(Math.floor(t / 60)).padStart(2, '0')}:${String(Math.floor(t % 60)).padStart(2, '0')}`;
const REASONS = {
  INVALID_STATE: 'Xe chưa ở đúng bước để thực hiện thao tác này.',
  LOT_FULL: 'Bãi chờ đã đầy (6/6). Hãy cho một xe ra cổng trước.',
  EXIT_BUSY: 'Cổng ra đang xử lý một xe khác.',
  QUEUE_FULL: 'Hàng chờ rửa đã đầy (4/4): xe phải chờ ngoài cổng.',
  DUPLICATE_PLATE: 'Biển số này đang ở trong trạm.',
  INVALID_PLATE: 'Biển số không hợp lệ.',
  INVALID_TYPE: 'Loại xe không được hỗ trợ.',
  PARKING_FULL: 'Bãi đỗ xe đã đầy (10 ô tô / 2 xe tải).',
  INVALID_SERVICE: 'Dịch vụ không hợp lệ.',
  ALREADY_PAID: 'Đơn này đã thanh toán.',
  NOT_BLOCKED: 'Chỉ thu tiền mặt khi xe đang bị chặn tại cổng ra.',
  BAY_BUSY: 'Khoang đang có xe, không thể bảo trì.',
  NOT_FOUND: 'Không tìm thấy.',
};
const STATUS_PILL = { WAITING_BAY: 'amber', IN_BAY: 'amber', WASHING: 'blue', WASHED: 'amber', IN_WAITING_LOT: 'gray', PARKED: 'gray', AT_EXIT: 'red', COMPLETED: 'green' };

// ---------- trạng thái ----------
let S = L.createState();
let paused = false, speed = 1, mode = 'sim', showLabels = true;
let demo = { on: false, t0: 0, i: 0 };
let selected = { kind: 'station' };
let lastLogN = 0, prevExit = 'IDLE';
let liveBays = null, liveTimer = 0, liveWarned = false;

// ---------- renderer / scene ----------
const scene = new THREE.Scene();
scene.background = new THREE.Color('#e6edef');
scene.fog = new THREE.Fog('#e6edef', 110, 230);
const camera = new THREE.PerspectiveCamera(38, 1, 0.1, 400);
// Firefox đôi khi từ chối ngữ cảnh WebGL2 với một số tùy chọn: thử lần lượt các cấu hình nhẹ dần.
const GL_TRIES = [
  { antialias: true, preserveDrawingBuffer: true },
  { antialias: false, preserveDrawingBuffer: true },
  { antialias: false, preserveDrawingBuffer: false, powerPreference: 'default' },
  { antialias: false, preserveDrawingBuffer: false, failIfMajorPerformanceCaveat: false, precision: 'mediump' },
];
let renderer = null, glError = null;
for (const opts of GL_TRIES) {
  try {
    renderer = new THREE.WebGLRenderer(opts);
    break;
  } catch (e) {
    glError = e;
  }
}
if (!renderer) {
  const hasGL2 = !!document.createElement('canvas').getContext('webgl2');
  $('loading').innerHTML =
    `Không khởi tạo được WebGL2 (${hasGL2 ? 'trình duyệt có WebGL2 nhưng bị từ chối' : 'trình duyệt không cung cấp WebGL2'}).<br>` +
    `Chi tiết: ${glError?.message || 'không rõ'}<br>Firefox: vào <b>about:config</b>, đặt <b>webgl.disabled = false</b>, ` +
    `<b>webgl.enable-webgl2 = true</b>, <b>webgl.force-enabled = true</b>, rồi tải lại; hoặc cập nhật driver đồ họa.`;
  $('loading').style.cssText = 'padding:24px;text-align:center;line-height:1.7';
  throw glError;
}
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFShadowMap;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.1;
root.appendChild(renderer.domElement);
$('loading').remove();

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.minDistance = 8;
controls.maxDistance = 120;
controls.maxPolarAngle = Math.PI / 2.05;

const W = WD.buildWorld(scene);
const vehicleRoot = new THREE.Group();
scene.add(vehicleRoot);
const ring = new THREE.Mesh(new THREE.RingGeometry(0.9, 1.1, 40), new THREE.MeshBasicMaterial({ color: '#2a8fa8', side: THREE.DoubleSide, depthTest: false, transparent: true, opacity: 0.9 }));
ring.rotation.x = -Math.PI / 2;
ring.position.y = 0.2;
ring.renderOrder = 12;
ring.visible = false;
scene.add(ring);

function resize() {
  const w = root.clientWidth, h = root.clientHeight;
  renderer.setSize(w, h);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
}
new ResizeObserver(resize).observe(root);
resize();

// ---------- góc nhìn ----------
const VIEWS = {
  overview: { p: [-30, 26, 34], t: [0, 0, -3] },
  entrance: { p: [-46, 15, 24], t: [-24, 1, 7] },
  hall: { p: [3, 15, 22], t: [1.25, 1, -2] },
  lot: { p: [-26, 20, -28], t: [0, 0, -14] },
  park: { p: [19, 34, 42], t: [19, 0, 8] },
  exit: { p: [12, 11, 6], t: [27, 1, -8] },
};
let goal = null;
camera.position.set(...VIEWS.overview.p);
controls.target.set(...VIEWS.overview.t);
controls.update();
controls.addEventListener('start', () => (goal = null));
function setView(v) {
  const d = VIEWS[v] || VIEWS.overview;
  goal = { p: new THREE.Vector3(...d.p), t: new THREE.Vector3(...d.t) };
  document.querySelectorAll('[data-view]').forEach((b) => b.classList.toggle('selected', b.dataset.view === v));
}
document.querySelectorAll('[data-view]').forEach((b) => (b.onclick = () => setView(b.dataset.view)));
$('labels').onclick = () => {
  showLabels = !showLabels;
  $('labels').classList.toggle('on', showLabels);
};

// ---------- xe 3D ----------
const meshes = new Map();
const goalKey = (v) => {
  switch (v.status) {
    case 'WAITING_BAY': return 'Q';
    case 'IN_BAY': case 'WASHING': case 'WASHED': return 'B:' + v.bayId;
    case 'IN_WAITING_LOT': return 'L';
    case 'PARKED': return 'P:' + v.parkSpot;
    case 'AT_EXIT': return 'X';
    default: return 'O';
  }
};
const FACE = { Q: Math.PI, B: 0, L: 0, P: Math.PI, X: -Math.PI / 2, O: -Math.PI / 2 };

function createMesh(v) {
  const { group, h, setDirt } = makeVehicle(v.type, PALETTE[v.id % PALETTE.length], v.id);
  group.userData.pick = { kind: 'vehicle', id: v.id };
  const lab = textSprite(v.plateLabel, { scale: [2.7, 0.68] });
  lab.position.set(0, h + 0.9, 0);
  lab.visible = showLabels;
  group.add(lab);
  group.position.set(WD.START[0], 0.02, WD.START[1]);
  group.rotation.y = -Math.PI / 2;
  vehicleRoot.add(group);
  const m = { id: v.id, group, lab, key: null, route: [], where: 'ENTRY', setDirt, qSlot: null, lotSlot: null, pSpot: null, st: v.status, bx: 0, fromX: 0, scanned: false, scan: null, scanX: 0 };
  meshes.set(v.id, m);
  return m;
}
function disposeMesh(m) {
  vehicleRoot.remove(m.group);
  m.group.traverse((o) => {
    o.geometry?.dispose?.();
    if (o.material) { o.material.map?.dispose?.(); o.material.dispose?.(); }
  });
  meshes.delete(m.id);
}
const freeSlot = (field, count) => {
  const used = new Set([...meshes.values()].map((m) => m[field]).filter((x) => x !== null));
  for (let i = 0; i < count; i++) if (!used.has(i)) return i;
  return count - 1;
};
function targetPoint(m, kind) {
  if (kind === 'Q') return [WD.QUEUE_X[m.qSlot], WD.QUEUE_Z];
  if (kind === 'L') return [WD.LOT_X[m.lotSlot], WD.LOT_Z];
  if (kind === 'P') return WD.PARK_POS[m.pSpot];
  if (kind === 'X') return WD.EXIT_STOP;
  if (kind === 'O') return WD.END;
  return [m.bx, WD.BAY_Z];
}
// Mọi tuyến đường đều chạy trên đường (driveway z=6 hoặc đường sau z=-9) rồi mới rẽ vào ô: xe không cắt qua tường.
function buildRoute(m, kind) {
  const to = targetPoint(m, kind);
  const from = m.where;
  if (kind === 'O') return [[34, WD.BACK_Z], to];
  if (kind === 'Q') return from === 'ENTRY' ? [[to[0], WD.ROAD_Z], to] : [to];
  if (kind === 'P') {
    // từ khoang: đường sau -> đường nối -> đường vào -> chỗ đỗ; từ cổng vào: đường vào -> chỗ đỗ
    if (from === 'B') return [[m.fromX, WD.BACK_Z], [WD.CONN_X, WD.BACK_Z], [WD.CONN_X, WD.ROAD_Z], [to[0], WD.ROAD_Z], to];
    return [[to[0], WD.ROAD_Z], to];
  }
  // từ bãi đỗ ra cổng: đường vào -> đường nối -> đường sau -> điểm dừng cổng ra
  if (kind === 'X' && from === 'P') return [[m.fromX, WD.ROAD_Z], [WD.CONN_X, WD.ROAD_Z], [WD.CONN_X, WD.BACK_Z], to];
  if (kind === 'X') return [[m.fromX, WD.BACK_Z], to];
  if (kind === 'L') return [[m.fromX, WD.BACK_Z], [to[0], WD.BACK_Z], to];
  // đích là khoang rửa
  if (from === 'Q') return [[m.fromX, WD.ROAD_Z], [to[0], WD.ROAD_Z], to];
  return [[to[0], WD.ROAD_Z], to];
}

const SCAN_T = 1.8; // giây (mô phỏng) camera AI quét một xe
const SCAN_X = -31; // điểm dừng quét ở cổng vào
let camLast = null;
// Đi tiếp trên tuyến hiện tại; trả về true khi vừa đến điểm cuối.
function advance(m, dt) {
  const [tx, tz] = m.route[0];
  const p = m.group.position;
  const dx = tx - p.x, dz = tz - p.z, d = Math.hypot(dx, dz), step = dt * 5.5;
  if (d > 0.001) {
    let diff = Math.atan2(-dx, -dz) - m.group.rotation.y;
    diff = Math.atan2(Math.sin(diff), Math.cos(diff));
    m.group.rotation.y += diff * Math.min(1, dt * 8);
  }
  if (d <= step) {
    p.x = tx;
    p.z = tz;
    m.route.shift();
    if (m.route.length === 0) { if (m.key) m.where = m.key[0]; return true; }
    return false;
  }
  p.x += (dx / d) * step;
  p.z += (dz / d) * step;
  return false;
}

// Độ bẩn nhìn thấy trên mô hình 3D (0..1): xe tới đã bẩn theo điểm AI, sạch dần khi rửa, rửa xong thì sạch.
// Xe chỉ gửi (không rửa) giữ nguyên độ bẩn.
function dirtAmount(v) {
  const base = (v.dirtScore ?? 0) / 100;
  if (v.service === 'PARK') return base;
  if (v.tWashEnd !== null) return 0;
  if (v.status === 'WASHING') return base * (1 - v.progress);
  return base;
}

function syncVehicles(dt) {
  const alive = new Set();
  for (const v of S.vehicles) {
    alive.add(v.id);
    let m = meshes.get(v.id);
    if (!m && v.status === 'COMPLETED') continue; // đã ra khỏi trạm, mesh đã gỡ
    const dk = goalKey(v);
    const kind = dk[0];
    if (!m) {
      m = createMesh(v);
      if (v.arrived) {
        // xe khởi tạo sẵn ở trạng thái đã tới nơi: đặt thẳng vào vị trí
        if (kind === 'Q') m.qSlot = freeSlot('qSlot', 4);
        if (kind === 'L') m.lotSlot = freeSlot('lotSlot', 6);
        if (kind === 'B') m.bx = WD.BAY_X[v.bayId];
        if (kind === 'P') m.pSpot = v.parkSpot;
        const p = targetPoint(m, kind);
        m.group.position.set(p[0], 0.02, p[1]);
        m.group.rotation.y = FACE[kind];
        m.key = dk;
        m.where = kind;
        m.scanned = true;
      }
    }
    m.setDirt(dirtAmount(v));
    if (!m.scanned) {
      // cổng vào: xe dừng tại điểm quét, camera AI quét xong barie mới mở
      if (!m.scan) { m.scan = { t: -1 }; m.route = [[SCAN_X, WD.ROAD_Z]]; }
      if (m.scan.t < 0) { if (advance(m, dt)) m.scan.t = 0; }
      else if ((m.scan.t += dt) >= SCAN_T) {
        m.scanned = true;
        camLast = { where: 'in', id: v.id, until: simClock + 3 };
        // xe đã được gán khoang / chỗ đỗ ngay lúc vào: camera khoang / bãi đỗ hiện sau kết quả quét cổng vào
        if (v.status === 'IN_BAY') evt('bay', v.id, 3);
        else if (v.status === 'PARKED') evt('park', v.id, 3);
        m.st = v.status;
      }
      m.lab.visible = showLabels;
      continue;
    }
    if (kind !== 'Q') m.qSlot = null;
    if (kind !== 'L') m.lotSlot = null;
    if (m.st !== v.status) {
      // chuyển trạng thái sau khi đã qua cổng vào: camera AI khoang / bãi đỗ / độ bẩn
      if (v.status === 'IN_BAY') evt('bay', v.id);
      else if (v.status === 'PARKED') evt('park', v.id);
      else if (v.status === 'WASHING') evt('dirt', v.id);
      m.st = v.status;
    }
    // nhận lệnh đích mới chỉ khi xe đã đi hết đoạn đường hiện tại
    if (m.route.length === 0 && m.key !== dk) {
      m.fromX = m.group.position.x;
      if (kind === 'Q' && m.qSlot === null) m.qSlot = freeSlot('qSlot', 4);
      if (kind === 'L' && m.lotSlot === null) m.lotSlot = freeSlot('lotSlot', 6);
      if (kind === 'B') m.bx = WD.BAY_X[v.bayId];
      if (kind === 'P') m.pSpot = v.parkSpot;
      m.route = buildRoute(m, kind);
      m.key = dk;
      m.scanX = 0;
    }
    if (m.route.length) advance(m, dt);
    else if (m.key === dk && !v.arrived && kind !== 'O') {
      if (kind === 'X') {
        // cổng ra: camera quét biển số và kiểm tra thanh toán rồi mới quyết định barie
        m.scanX += dt;
        if (m.scanX >= SCAN_T) { L.arrive(S, v.id); camLast = { where: 'out', id: v.id, until: simClock + 3 }; }
      } else L.arrive(S, v.id);
    }
    if (m.route.length === 0 && m.key === 'O') {
      disposeMesh(m);
      continue;
    }
    if (m.route.length === 0 && m.key) {
      let diff = FACE[m.key[0]] - m.group.rotation.y;
      diff = Math.atan2(Math.sin(diff), Math.cos(diff));
      m.group.rotation.y += diff * Math.min(1, dt * 6);
    }
    m.lab.visible = showLabels;
  }
  for (const [id, m] of meshes) if (!alive.has(id)) disposeMesh(m);
}

// ---------- hiệu ứng khoang / cổng ----------
const BAY_COLORS = { green: '#2fb36a', amber: '#f0b840', blue: '#3d8ff0', gray: '#8c969b' };
function bayVisual(id) {
  if (mode === 'live' && liveBays?.[id]) {
    const s = liveBays[id];
    return { color: s === 'OCCUPIED' ? 'amber' : s === 'MAINTENANCE' ? 'gray' : 'green', washing: false, v: null };
  }
  const b = S.bays[id];
  const v = b.orderId ? L.getVehicle(S, b.orderId) : null;
  if (b.status === 'MAINTENANCE') return { color: 'gray', washing: false, v };
  if (!v) return { color: 'green', washing: false, v: null };
  if (v.status === 'WASHING') return { color: 'blue', washing: true, v };
  return { color: 'amber', washing: false, v };
}
let simClock = 0;
function updateWorld(dt) {
  simClock += dt;
  for (const id of Object.keys(W.bays)) {
    const bay = W.bays[id];
    const vis = bayVisual(id);
    const col = BAY_COLORS[vis.color];
    bay.lightMat.color.set(col);
    bay.lightMat.emissive.set(col);
    WD.updateBayFx(bay, vis.washing, simClock, dt);
    const show = vis.v && (vis.v.status === 'WASHING' || vis.v.status === 'WASHED');
    bay.barGroup.visible = !!show && showLabels;
    if (show) {
      bay.barGroup.quaternion.copy(camera.quaternion);
      bay.fill.scale.set(Math.max(0.001, bay.bw * vis.v.progress), 0.3, 1);
      bay.fillM.color.set(vis.v.status === 'WASHED' ? '#2fb36a' : '#3d8ff0');
    }
  }
  for (const id of Object.keys(W.parking)) {
    const col = S.parking[id].status === 'OCCUPIED' ? '#e0513f' : '#2fb36a';
    W.parking[id].lightMat.color.set(col);
    W.parking[id].lightMat.emissive.set(col);
  }
  const near = (x0, x1) => [...meshes.values()].some((m) => m.group.position.x > x0 && m.group.position.x < x1 && Math.abs(m.group.position.z - WD.ROAD_Z) < 3);
  const gi = W.gateIn;
  const opening = [...meshes.values()].some((m) => m.scanned && m.group.position.x > -32 && m.group.position.x < -19 && Math.abs(m.group.position.z - WD.ROAD_Z) < 3);
  gi.angle += ((opening ? 1.35 : 0) - gi.angle) * Math.min(1, dt * 4 + 0.02);
  gi.pivot.rotation.x = gi.angle;
  const sensing = near(-33, -28.5);
  gi.sensorMat.color.set(sensing ? '#ee5a4d' : '#2fb36a');
  gi.sensorMat.emissive.set(sensing ? '#ee5a4d' : '#2fb36a');
  const go = W.gateOut;
  const blocked = S.exit.state === 'BLOCK';
  go.angle += ((blocked ? 0 : 1.35) - go.angle) * Math.min(1, dt * 4 + 0.02);
  go.pivot.rotation.x = go.angle;
  go.redMat.emissiveIntensity = blocked ? 1.6 : 0.08;
  go.greenMat.emissiveIntensity = blocked ? 0.08 : 1.2;
  const mon = blocked ? '#ee5a4d' : '#3aa3bf';
  go.monitorMat.color.set(mon);
  go.monitorMat.emissive.set(mon);
  W.labels.forEach((s) => (s.visible = showLabels));
}

// ---------- cửa sổ Camera AI ----------
const camEl = $('cam');
const camShown = { sig: '' };
const TH = [0.25, 0.5, 0.8, 1];
let camEvt = null; // sự kiện camera trong khoang / bãi đỗ: { where, id, t0, until }
const evt = (where, id, delay = 0) => { camEvt = { where, id, t0: simClock + delay, until: simClock + delay + 4.5 }; };
function camState() {
  for (const m of meshes.values()) {
    const v = L.getVehicle(S, m.id);
    if (!v) continue;
    if (!m.scanned && m.scan && m.scan.t >= 0) return { where: 'in', v, p: Math.min(1, m.scan.t / SCAN_T) };
    if (m.key === 'X' && m.route.length === 0 && !v.arrived && m.scanX > 0) return { where: 'out', v, p: Math.min(1, m.scanX / SCAN_T) };
  }
  for (const v of S.vehicles) {
    // camera trong khoang quét độ bẩn trong lúc chờ bắt đầu rửa
    if (v.status === 'IN_BAY' && v.arrived && v.dirt) return { where: 'dirt', v, p: Math.min(1, v.dwell / L.CFG.IN_BAY_DELAY) };
  }
  if (camLast && simClock < camLast.until) {
    const v = L.getVehicle(S, camLast.id);
    if (v) return { where: camLast.where, v, p: 1 };
  }
  if (camEvt && simClock >= camEvt.t0 && simClock < camEvt.until) {
    const v = L.getVehicle(S, camEvt.id);
    if (v) return { where: camEvt.where, v, p: camEvt.where === 'dirt' ? 1 : Math.min(1, (simClock - camEvt.t0) / 1.6) };
  }
  return null;
}
function carSvg(type) {
  return type === 'TRUCK'
    ? '<svg viewBox="0 0 160 70"><rect x="6" y="10" width="92" height="36" rx="3" fill="#dfe6e9"/><path d="M98 20h30l16 14v12H98z" fill="#e8b93a"/><rect x="108" y="24" width="14" height="10" fill="#9ec9d8"/><circle cx="32" cy="52" r="10" fill="#23282b"/><circle cx="76" cy="52" r="10" fill="#23282b"/><circle cx="124" cy="52" r="10" fill="#23282b"/></svg>'
    : '<svg viewBox="0 0 160 70"><path d="M10 44l10-18q4-8 14-8h56q8 0 14 8l12 12h24v6z" fill="#3d7be0"/><path d="M30 24h26v14H22zM62 24h28l10 14H62z" fill="#cfe6f2"/><circle cx="42" cy="48" r="10" fill="#23282b"/><circle cx="120" cy="48" r="10" fill="#23282b"/></svg>';
}
const CAM_STEPS = {
  in: ['Phát hiện xe', 'Phân loại loại xe', 'Đọc biển số', 'Khớp tài khoản'],
  out: ['Phát hiện xe', 'Phân loại loại xe', 'Đọc biển số', 'Kiểm tra ví / thanh toán'],
  bay: ['Quét ảnh các khoang', 'Nhận diện khoang trống', 'Chọn khoang', 'Dẫn xe vào khoang'],
  dirt: ['Quét bề mặt xe', 'Đánh giá độ bẩn', 'Chọn thông số rửa', 'Điều chỉnh thời gian rửa'],
  park: ['Quét bãi đỗ', 'Nhận diện chỗ trống', 'Chọn chỗ đỗ', 'Dẫn xe vào chỗ'],
};
const CAM_WHERE = { in: 'Cổng vào', out: 'Cổng ra', bay: 'Khoang rửa', dirt: 'Trong khoang', park: 'Bãi đỗ xe' };
// độ tin cậy giả lập của từng ô nhận diện (ổn định theo xe + ô)
const cellConf = (id, v) => 90 + ((id.charCodeAt(0) * 7 + id.charCodeAt(1) * 3 + v.id * 5) % 9);
function camGridHtml(w, v, p) {
  if (w === 'dirt') {
    const d = v.dirt, shown = Math.round(d.score * Math.min(1, p / 0.8));
    return `<div class="dirtbox"><div class="dirtpct">${shown}%</div><div class="dirtbar"><i style="width:${shown}%"></i></div><small>${p >= 0.8 ? 'Độ bẩn: ' + d.levelName : 'Đang đo…'}</small></div>`;
  }
  const pick = w === 'bay' ? v.bayId || v.lastBay : v.parkSpot || v.lastSpot;
  const cells = Object.values(w === 'bay' ? S.bays : S.parking);
  return cells.map((c) => {
    let cls = c.status === 'MAINTENANCE' ? 'maint' : c.status === 'OCCUPIED' ? 'occ' : 'free';
    let txt = cls === 'maint' ? 'Bảo trì' : cls === 'occ' ? 'Có xe' : 'Trống';
    if (c.id === pick) { cls = p >= TH[2] ? 'pick' : 'free'; txt = p >= TH[2] ? '✔ Chọn' : 'Trống'; }
    return `<div class="gc ${cls}"><b>${c.id}</b><small>${txt}${w === 'bay' && cls !== 'maint' ? ' ' + cellConf(c.id, v) + '%' : ''}</small></div>`;
  }).join('');
}
function updateCam() {
  const c = camState();
  camEl.hidden = !c;
  if (!c) { camShown.sig = ''; return; }
  const v = c.v, p = c.p, w = c.where, out = w === 'out', gate = w === 'in' || out;
  const sig = v.id + w;
  if (camShown.sig !== sig) {
    camShown.sig = sig;
    camShown.grid = '';
    $('camCar').innerHTML = carSvg(v.type);
    $('camWhere').textContent = CAM_WHERE[w];
    $('camStage').classList.toggle('grid', !gate);
    $('camGrid').className = 'camgrid ' + w;
    CAM_STEPS[w].forEach((t, i) => ($('camSteps').children[i].textContent = t));
  }
  $('camPct').textContent = Math.round(p * 100) + '%';
  $('camBar').style.width = p * 100 + '%';
  $('camLine').style.cssText = p < 1 ? `top:${p * 100}%;opacity:1` : 'opacity:0';
  $('camBox').classList.toggle('on', p >= TH[0] * 0.6);
  const steps = $('camSteps').children;
  const act = TH.findIndex((t) => p < t);
  for (let i = 0; i < 4; i++) steps[i].className = p >= TH[i] ? 'done' : i === act ? 'act' : '';
  const res = $('camResult');
  if (!gate) {
    const gh = camGridHtml(w, v, p);
    if (gh !== camShown.grid) { camShown.grid = gh; $('camGrid').innerHTML = gh; }
    const pick = w === 'bay' ? v.bayId || v.lastBay : v.parkSpot || v.lastSpot;
    if (w === 'bay') steps[2].textContent = p >= TH[2] ? `Chọn khoang ${pick}` : 'Chọn khoang';
    if (w === 'park') steps[2].textContent = p >= TH[2] ? `Chọn chỗ ${pick}` : 'Chọn chỗ đỗ';
    if (w === 'dirt') {
      steps[1].textContent = p >= TH[1] ? `Độ bẩn: ${v.dirt.score}% (${v.dirt.levelName})` : 'Đánh giá độ bẩn';
      steps[3].textContent = p >= TH[3] ? `Thời gian rửa ×${v.dirt.timeMul}` : 'Điều chỉnh thời gian rửa';
    }
    if (p < 1) { res.className = 'camresult'; res.textContent = 'Đang quét…'; return; }
    res.className = 'camresult ok';
    if (w === 'bay') res.textContent = `✔ AI: khoang ${pick} trống (${cellConf(pick, v)}%) → mời xe vào`;
    else if (w === 'park') res.textContent = `✔ AI: chỗ ${pick} trống → mời xe vào chỗ đỗ`;
    else res.textContent = `✔ ${v.dirt.pressureBar} bar · ${v.dirt.tempC}°C · ${v.dirt.waterL} L · thời gian ×${v.dirt.timeMul}`;
    return;
  }
  steps[1].textContent = p >= TH[1] ? `Loại xe: ${L.TYPES[v.type].name}` : 'Phân loại loại xe';
  const len = v.plateLabel.length;
  const n = p < 0.5 ? 0 : p >= TH[2] ? len : Math.ceil(((p - 0.5) / 0.3) * len);
  $('camPlate').textContent = n ? v.plateLabel.slice(0, n) + (n < len ? '▌' : '') : '';
  steps[2].textContent = p >= TH[2] ? `Biển số: ${v.plateLabel}` : 'Đọc biển số';
  if (p < 1) { res.className = 'camresult'; res.textContent = 'Đang quét…'; return; }
  if (out) {
    const blocked = S.exit.state === 'BLOCK' && S.exit.orderId === v.id;
    if (v.paid) {
      res.className = 'camresult ok';
      res.textContent = v.payMethod === 'WALLET' ? `✔ Ví app tự trừ ${fmtMoney(v.total)} (còn ${fmtMoney(v.wallet)}) → barie mở` : '✔ Đã thanh toán → barie mở';
    } else if (blocked) {
      res.className = 'camresult bad';
      res.textContent = `✖ ${v.guest ? 'Khách vãng lai' : 'Số dư ví ' + fmtMoney(v.wallet) + ' không đủ'} → hạ barie`;
    } else { res.className = 'camresult'; res.textContent = 'Đang kiểm tra ví…'; }
  } else {
    res.className = 'camresult ok';
    res.textContent = `✔ ${L.TYPES[v.type].name} · ${v.plateLabel} · ${v.conf}% · ${v.guest ? 'Vé vãng lai' : 'Khớp tài khoản ' + v.name} · ${L.SERVICES[v.service]} → barie mở`;
  }
}

// ---------- chọn đối tượng trong cảnh ----------
const ray = new THREE.Raycaster();
const ptr = new THREE.Vector2();
function pickAt(ev) {
  const r = renderer.domElement.getBoundingClientRect();
  ptr.set(((ev.clientX - r.left) / r.width) * 2 - 1, -((ev.clientY - r.top) / r.height) * 2 + 1);
  ray.setFromCamera(ptr, camera);
  const hits = ray.intersectObjects([...W.pickRoots, vehicleRoot], true);
  for (const h of hits) {
    let o = h.object;
    while (o) {
      if (o.userData?.pick) return o.userData.pick;
      o = o.parent;
    }
  }
  return null;
}
let down = null, lastMove = 0;
renderer.domElement.addEventListener('pointerdown', (e) => (down = [e.clientX, e.clientY]));
renderer.domElement.addEventListener('pointerup', (e) => {
  if (!down || Math.hypot(e.clientX - down[0], e.clientY - down[1]) > 5) return;
  const p = pickAt(e);
  if (p) select(p);
  else select({ kind: 'station' });
});
renderer.domElement.addEventListener('pointermove', (e) => {
  if (e.timeStamp - lastMove < 80) return;
  lastMove = e.timeStamp;
  renderer.domElement.style.cursor = pickAt(e) ? 'pointer' : 'grab';
});
function select(sel) {
  selected = sel;
  updateUI();
}

// ---------- bảng chi tiết ----------
const pill = (txt, c) => `<span class="pill ${c}">${txt}</span>`;
const back = `<button class="back" data-act="back">← Tất cả xe</button>`;

const parkFeeNow = (v) => (v.status === 'PARKED' ? L.parkFeeFor(v.type, v.dwell) : v.parkFee);
const fmtPark = (v) => {
  const min = Math.floor(v.dwell * L.CFG.SIM_MIN_PER_SEC);
  return `${Math.floor(min / 60)}g${String(min % 60).padStart(2, '0')}`;
};
function dirtBill(v) {
  const d = v.dirt;
  return `<div class="bill"><div><span>Độ bẩn (AI)</span><span>${d.score}% · ${d.levelName}</span></div><div><span>Áp suất</span><span>${d.pressureBar} bar</span></div><div><span>Nhiệt độ</span><span>${d.tempC}°C</span></div><div><span>Lượng nước</span><span>${d.waterL} L</span></div><div><span>Thời gian rửa</span><span>×${d.timeMul}${v.status === 'WASHING' ? ` (${Math.round(v.washSec)} giây)` : ''}</span></div></div>`;
}

function phoneHtml(v) {
  const idx = L.timelineIndex(v);
  const tl = L.TIMELINE.map((t, i) => `<li class="${i < idx ? 'done' : i === idx ? 'cur' : ''}">${t}</li>`).join('');
  let banner;
  switch (v.status) {
    case 'WAITING_BAY': banner = [`Đang chờ khoang trống (vị trí ${S.queue.indexOf(v.id) + 1}/${L.CFG.QUEUE_CAP})`, '']; break;
    case 'IN_BAY': case 'WASHING': banner = [`Xe của bạn đang ở Khoang ${v.bayId} - ${L.TYPES[v.type].name}`, '']; break;
    case 'WASHED': banner = ['Xe của bạn đã rửa xong!', 'green']; break;
    case 'IN_WAITING_LOT': banner = ['Xe đang ở bãi chờ, mời bạn lấy xe', 'green']; break;
    case 'PARKED': banner = [`Xe đang gửi tại chỗ ${v.parkSpot} · ${fmtMoney(L.PARK_RATE[v.type])}/giờ`, 'green']; break;
    case 'AT_EXIT':
      banner = v.paid ? [v.payMethod === 'WALLET' ? 'Ví app đã tự trừ tiền · barie mở' : 'Đã thanh toán · barie mở', 'green']
        : [v.guest ? 'Chưa thanh toán · barie hạ, trả tiền mặt tại cổng' : 'Số dư ví không đủ · barie hạ, trả tiền mặt tại cổng', 'red'];
      break;
    default: banner = ['Hoàn tất · hẹn gặp lại!', 'green'];
  }
  const done = ['WASHED', 'IN_WAITING_LOT', 'PARKED', 'AT_EXIT', 'COMPLETED'].includes(v.status);
  const wash = v.service !== 'PARK';
  let rows = `<div><span>Dịch vụ</span><span>${L.SERVICES[v.service]}</span></div>`;
  if (wash) rows += `<div><span>Gói</span><span>${v.packageName}</span></div>`;
  if (v.dirt && v.status !== 'WAITING_BAY') rows += `<div><span>Độ bẩn (AI)</span><span>${v.dirt.score}% · ${v.dirt.levelName}</span></div>`;
  if (wash && done && v.tWashEnd !== null) rows += `<div><span>Vào / xong</span><span>${fmtT(v.tEntry)} / ${fmtT(v.tWashEnd)}</span></div>`;
  if (wash) rows += `<div><span>Phí rửa</span><span>${fmtMoney(v.price)}</span></div>`;
  if (v.status === 'PARKED') rows += `<div><span>Gửi xe (<span data-live="ptime"></span>)</span><span data-live="pfee"></span></div>`;
  else if (v.parkFee) rows += `<div><span>Phí gửi xe</span><span>${fmtMoney(v.parkFee)}</span></div>`;
  const totalTxt = v.status === 'PARKED' ? '<span data-live="ptotal"></span>' : `<span>${fmtMoney(v.total)}</span>`;
  const bill = `<div class="bill">${rows}<div class="tot"><span>${done ? 'Tổng' : 'Tạm tính'}</span>${totalTxt}</div></div>`;
  const due = v.price + parkFeeNow(v);
  let pay = '';
  if (v.guest) pay = `<p class="note" style="margin:0">Vé vãng lai <b>${v.code}</b>. Trả tiền mặt tại cổng ra.</p>`;
  else if (v.paid) pay = `<div class="okbox" style="margin:0">✔ Đã thanh toán (${v.payMethod === 'CASH' ? 'tiền mặt' : 'ví app'}) · ví còn ${fmtMoney(v.wallet)}</div>`;
  else pay = `<div class="wallet"><span>Ví app</span><b>${fmtMoney(v.wallet)}</b></div><p class="note" style="margin:6px 0 0">Tự trừ khi camera cổng ra đọc biển số${v.wallet < due ? '. <b style="color:#b3261e">Số dư chưa đủ, sẽ phải trả tiền mặt.</b>' : '.'}</p>`;
  return `<div class="phone"><div class="pbody"><h4>${v.guest ? 'Khách vãng lai' : 'Xin chào, ' + v.name}</h4><div class="plate">${v.plateLabel}</div>
    <div class="banner ${banner[1]}">${banner[0]}</div><ul class="tl">${tl}</ul>${bill}${pay}</div></div>`;
}

function stationHtml() {
  const list = S.vehicles.filter((v) => v.status !== 'COMPLETED');
  let h = `<div class="head"><h3>Trạm rửa xe</h3>${pill(mode === 'live' ? 'TRỰC TIẾP' : 'MÔ PHỎNG', mode === 'live' ? 'green' : 'blue')}</div>`;
  h += `<div class="sum"><span>Doanh thu <b>${fmtMoney(S.revenue)}</b></span><span>Đã ra <b>${S.completed}</b> xe</span></div>`;
  h += list.length
    ? `<div class="vlist">${list.map((v) => `<button class="vitem" data-act="select" data-id="${v.id}"><span><b>${v.plateLabel}</b><small>${L.TYPES[v.type].name} · ${L.SERVICES[v.service]} · ${v.paid ? 'đã trả' : 'chưa trả'}</small></span>${pill(L.STATUS_VI[v.status], STATUS_PILL[v.status])}</button>`).join('')}</div>`
    : `<p class="note">Chưa có xe. Bấm <b>＋ Thêm xe</b> hoặc <b>▶ Chạy minh họa</b>.</p>`;
  h += `<p class="note">Bấm vào xe, khoang, bãi đỗ hoặc cổng trong cảnh 3D để xem và điều khiển.</p>`;
  return h;
}
function vehicleHtml(v) {
  const act = (a, t, dis) => `<button class="btn primary action" data-act="${a}" data-id="${v.id}" ${dis ? 'disabled' : ''}>${t}</button>`;
  let a = '';
  if (v.status === 'IN_BAY') a = act('start', 'Bắt đầu rửa', !v.arrived);
  if (v.status === 'WASHING') a = act('complete', 'Rửa xong ngay', false);
  if (v.status === 'WASHED') a = act('lot', 'Đưa ra bãi chờ', false);
  if (v.status === 'IN_WAITING_LOT') a = act('exit', 'Khách lấy xe → cổng ra', !v.arrived);
  if (v.status === 'PARKED') a = act('pickup', 'Lấy xe → cổng ra', !v.arrived);
  if (v.status === 'AT_EXIT' && S.exit.state === 'BLOCK') a = act('cash', 'Đã thu tiền mặt', false);
  const prog = v.status === 'WASHING' ? ` · <span data-live="prog"></span>` : '';
  return `${back}<div class="head"><h3>${v.plateLabel} · ${L.TYPES[v.type].name}</h3>${pill(L.STATUS_VI[v.status], STATUS_PILL[v.status])}</div><p class="note" style="margin:0 0 4px">${v.packageName}${prog}</p>${a}${phoneHtml(v)}`;
}
function bayHtml(id) {
  const b = S.bays[id];
  const v = bayVisual(id).v;
  const st = b.status === 'MAINTENANCE' ? ['BẢO TRÌ', 'gray'] : v ? (v.status === 'WASHING' ? ['ĐANG RỬA', 'blue'] : ['CÓ XE', 'amber']) : ['TRỐNG', 'green'];
  let h = `${back}<div class="head"><h3>Khoang ${id} · ${L.TYPES[b.type].name}</h3>${pill(st[0], st[1])}</div>`;
  if (v) h += `<button class="vitem" style="width:100%" data-act="select" data-id="${v.id}"><span><b>${v.plateLabel}</b><small>${v.packageName}${v.status === 'WASHING' ? ' · <span data-live="prog"></span>' : ''}</small></span><span>›</span></button>`;
  if (v?.dirt && v.arrived) h += dirtBill(v);
  if (b.status === 'AVAILABLE') h += `<button class="btn action" data-act="maint" data-id="${id}">Đặt bảo trì</button>`;
  if (b.status === 'MAINTENANCE') h += `<button class="btn primary action" data-act="unmaint" data-id="${id}">Mở lại khoang</button>`;
  h += `<p class="note">Khoang được trả lại khi xe rời khoang. Hết khoang cùng loại thì xe vào hàng chờ. Camera AI quét độ bẩn rồi đặt áp suất, nhiệt độ, lượng nước và kéo dài thời gian rửa (giá không đổi).</p>`;
  return h;
}
function parkingHtml() {
  const u = L.stats(S).parkUse;
  const list = S.vehicles.filter((v) => v.status === 'PARKED');
  let h = `${back}<div class="head"><h3>Bãi đỗ xe</h3>${pill(`${u.used}/${u.total}`, u.used >= u.total ? 'red' : 'green')}</div>`;
  h += `<p class="note" style="margin:0 0 6px">10 chỗ ô tô, 2 chỗ xe tải. Phí ${fmtMoney(L.PARK_RATE.CAR)}/giờ (ô tô), ${fmtMoney(L.PARK_RATE.TRUCK)}/giờ (xe tải), tối thiểu 1 giờ. 1 giây mô phỏng = ${L.CFG.SIM_MIN_PER_SEC} phút thực.</p>`;
  h += list.length
    ? `<div class="vlist">${list.map((v) => `<button class="vitem" data-act="select" data-id="${v.id}"><span><b>${v.plateLabel}</b><small>${v.parkSpot} · <span data-live="pfee:${v.id}"></span></small></span><span>›</span></button>`).join('')}</div>`
    : `<p class="note">Chưa có xe nào đang gửi.</p>`;
  h += `<button class="btn primary action" data-act="add">＋ Cho xe vào cổng</button>`;
  return h;
}
function gateInHtml() {
  return `${back}<div class="head"><h3>Cổng vào</h3>${pill('AI', 'blue')}</div>
    <p class="note">Cảm biến phát hiện xe, camera đọc biển số và loại xe. Biển số khớp tài khoản thì gắn đơn vào app (chọn Rửa / Gửi xe / Rửa + gửi), không khớp thì cấp vé vãng lai (chỉ rửa). Hết khoang, xe vào hàng chờ (tối đa 4); hết chỗ đỗ, xe chỉ gửi bị từ chối.</p>
    <button class="btn primary action" data-act="add">＋ Cho xe vào cổng</button>`;
}
function gateOutHtml() {
  const e = S.exit;
  const v = e.orderId ? L.getVehicle(S, e.orderId) : null;
  const st = { IDLE: ['Sẵn sàng', 'green'], WAIT: ['Đang đọc biển số', 'blue'], OPEN: ['Barie mở', 'green'], BLOCK: ['Barie hạ', 'red'] }[e.state];
  let h = `${back}<div class="head"><h3>Cổng ra · thu ngân</h3>${pill(st[0], st[1])}</div>`;
  if (e.state === 'BLOCK' && v) {
    h += `<div class="alertbox"><b>${v.plateLabel} chưa thanh toán</b>Cần thu ${fmtMoney(v.total)} · ${v.guest ? 'Khách vãng lai' : `${v.name} · số dư ví ${fmtMoney(v.wallet)} không đủ`}</div>`;
    h += `<button class="btn primary action" data-act="cash" data-id="${v.id}">Đã thu tiền mặt → mở barie</button>`;
  } else if (e.state === 'OPEN') h += `<div class="okbox">Đã thanh toán: barie không hạ, xe đi thẳng.</div>`;
  else h += `<p class="note">Camera đọc biển số: ví app đủ tiền thì tự trừ, barie không hạ. Khách vãng lai hoặc thiếu số dư: hạ barie, thu ngân thu tiền mặt rồi mới mở. Mỗi lần chỉ xử lý một xe.</p>`;
  return h;
}

function detailsHtml() {
  if (selected.kind === 'vehicle') {
    const v = L.getVehicle(S, selected.id);
    if (v) return vehicleHtml(v);
    selected = { kind: 'station' };
  }
  if (selected.kind === 'bay') return bayHtml(selected.id);
  if (selected.kind === 'parking') return parkingHtml();
  if (selected.kind === 'gateIn') return gateInHtml();
  if (selected.kind === 'gateOut') return gateOutHtml();
  return stationHtml();
}
function liveText(key) {
  const [k, id] = key.split(':');
  const v = id ? L.getVehicle(S, Number(id)) : selected.kind === 'vehicle' ? L.getVehicle(S, selected.id) : selected.kind === 'bay' ? bayVisual(selected.id).v : null;
  if (!v) return '';
  switch (k) {
    case 'prog': return Math.round(v.progress * 100) + '%';
    case 'pfee': return fmtMoney(parkFeeNow(v));
    case 'ptime': return fmtPark(v);
    case 'ptotal': return fmtMoney(v.price + parkFeeNow(v));
    default: return '';
  }
}

let lastDetails = '';
function renderDetails() {
  const h = detailsHtml();
  if (h !== lastDetails) {
    lastDetails = h;
    $('details').innerHTML = h;
  }
  $('details').querySelectorAll('[data-live]').forEach((n) => (n.textContent = liveText(n.dataset.live)));
}

// ---------- thao tác ----------
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toast.t);
  toast.t = setTimeout(() => t.classList.remove('show'), 3200);
}
function act(a, id) {
  if (mode === 'live' && a !== 'select' && a !== 'back') return toast('Chế độ TRỰC TIẾP chỉ hiển thị dữ liệu. Về MÔ PHỎNG để điều khiển.');
  let r = { ok: true };
  const vid = Number(id);
  switch (a) {
    case 'start': r = L.startWash(S, vid); break;
    case 'complete': r = L.completeWash(S, vid); break;
    case 'lot': r = L.moveToLot(S, vid); break;
    case 'exit': r = L.goToExit(S, vid); break;
    case 'pickup': r = L.pickUp(S, vid); break;
    case 'cash': r = L.payCash(S, vid || S.exit.orderId); break;
    case 'maint': r = L.setMaintenance(S, id, true); break;
    case 'unmaint': r = L.setMaintenance(S, id, false); break;
    case 'select': select({ kind: 'vehicle', id: vid }); return;
    case 'back': select({ kind: 'station' }); return;
    case 'add': openSpawn(); return;
    default:
  }
  if (!r.ok) toast(REASONS[r.reason] || r.reason);
  updateUI();
}
$('details').addEventListener('click', (e) => {
  const b = e.target.closest('[data-act]');
  if (b && !b.disabled) act(b.dataset.act, b.dataset.id);
});

// ---------- hộp thoại thêm xe ----------
const dlg = $('spawnDlg'), form = $('spawnForm');
const rnd = (a) => a[Math.floor(Math.random() * a.length)];
const digits = (n) => Array.from({ length: n }, () => Math.floor(Math.random() * 10)).join('');
const NAMES = ['Nguyễn Văn An', 'Trần Thị Bình', 'Lê Hoàng Cường', 'Phạm Minh Đức', 'Võ Thu Hà'];
function randomPlate(type) {
  if (type === 'TRUCK') return `${rnd(['29', '51', '43'])}C-${digits(3)}.${digits(2)}`;
  return `${rnd(['51', '30', '43', '92', '75'])}${rnd(['A', 'B', 'G', 'H', 'K'])}-${digits(3)}.${digits(2)}`;
}
function fillPkg() {
  form.pkg.innerHTML = L.PACKAGES[form.type.value].map((p, i) => `<option value="${i}">${p.name} · ${fmtMoney(p.price)}</option>`).join('');
  const guest = form.guest.value === '1';
  form.service.disabled = form.wallet.disabled = guest;
  form.pkg.disabled = guest || form.service.value === 'PARK';
}
function openSpawn() {
  form.plate.value = randomPlate(form.type.value);
  fillPkg();
  $('spawnErr')?.remove();
  dlg.showModal();
}
form.type.onchange = () => { form.plate.value = randomPlate(form.type.value); fillPkg(); };
form.guest.onchange = fillPkg;
form.service.onchange = fillPkg;
form.addEventListener('submit', (e) => {
  if (e.submitter?.value !== 'ok') return;
  const guest = form.guest.value === '1';
  const r = L.spawnVehicle(S, {
    type: form.type.value, plate: form.plate.value, guest,
    packageIndex: Number(form.pkg.value), service: guest ? 'WASH' : form.service.value, wallet: Number(form.wallet.value),
    name: rnd(NAMES), phone: '09' + digits(8),
  });
  if (!r.ok) {
    e.preventDefault();
    $('spawnErr')?.remove();
    const p = document.createElement('p');
    p.id = 'spawnErr';
    p.className = 'note';
    p.style.color = '#b3261e';
    p.textContent = REASONS[r.reason] || r.reason;
    form.querySelector('.dlgbtns').before(p);
    return;
  }
  toast(`${L.TYPES[r.order.type].name} ${r.order.plateLabel} vào cổng`);
  select({ kind: 'vehicle', id: r.order.id });
});
$('addcar').onclick = () => (mode === 'live' ? toast('Về chế độ MÔ PHỎNG để thêm xe.') : openSpawn());

// ---------- kịch bản minh họa ----------
const SCRIPT = [
  { t: 1, o: { type: 'CAR', plate: '51A-123.45', packageIndex: 1, wallet: 500000, name: 'Trần Văn B', phone: '0912345678' } },
  { t: 4, o: { type: 'TRUCK', plate: '29C-888.88', packageIndex: 1, wallet: 20000, name: 'Lê Hoàng C', phone: '0987654321' } },
  { t: 7, o: { type: 'CAR', plate: '30G-678.90', guest: true } },
  { t: 10, o: { type: 'CAR', plate: '30G-555.66', service: 'PARK', wallet: 500000, name: 'Phạm Minh D', phone: '0903111222' } },
  { t: 12, o: { type: 'CAR', plate: '51H-222.33', service: 'WASH_PARK', packageIndex: 2, wallet: 500000, name: 'Võ Thu E', phone: '0933444555' } },
  { t: 14, o: { type: 'CAR', plate: '43A-444.55', wallet: 20000, name: 'Đặng Văn F', phone: '0944555666' } },
  { t: 16, o: { type: 'CAR', plate: '92A-111.22', guest: true } },
  { t: 18, o: { type: 'CAR', plate: '75A-666.77', service: 'WASH_PARK', wallet: 500000, name: 'Ngô Thị G', phone: '0977888999' } },
];
function runScript() {
  while (demo.on && demo.i < SCRIPT.length && S.time - demo.t0 >= SCRIPT[demo.i].t) {
    const r = L.spawnVehicle(S, SCRIPT[demo.i].o);
    if (!r.ok) toast(REASONS[r.reason] || r.reason);
    demo.i++;
  }
}
function setDemo(on) {
  demo.on = on;
  S.autoCashier = on;
  $('demo').textContent = on ? '■ Dừng minh họa' : '▶ Chạy minh họa';
  $('demo').classList.toggle('primary', !on);
  if (on) { demo.t0 = S.time; demo.i = 0; toast('Minh họa: ví tự trừ, xe thiếu số dư bị chặn rồi thu tiền mặt, gửi xe, rửa + gửi, hàng chờ đầy.'); }
}
$('demo').onclick = () => {
  if (mode === 'live') return toast('Về chế độ MÔ PHỎNG để chạy minh họa.');
  if (demo.on) return setDemo(false);
  if (S.vehicles.length > 3 || S.time > 0) resetAll(false);
  setDemo(true);
};
$('pause').onclick = () => {
  paused = !paused;
  $('pause').textContent = paused ? '▶' : 'Ⅱ';
  $('pause').title = paused ? 'Tiếp tục' : 'Tạm dừng';
};
$('speed').onchange = (e) => (speed = Number(e.target.value));
$('reset').onclick = () => resetAll(true);

function seedInitial() {
  const car = L.spawnVehicle(S, { type: 'CAR', plate: '51A-888.88', packageIndex: 1, wallet: 500000, dirtScore: 80, name: 'Nguyễn Văn An', phone: '0901234567' }).order;
  L.arrive(S, car.id); L.startWash(S, car.id); car.progress = 0.45;
  const guest = L.spawnVehicle(S, { type: 'CAR', plate: '30G-123.45', guest: true }).order;
  L.arrive(S, guest.id); L.startWash(S, guest.id); guest.progress = 0.2;
  const truck = L.spawnVehicle(S, { type: 'TRUCK', plate: '29C-777.77', packageIndex: 0, wallet: 20000, name: 'Lê Hoàng Cường', phone: '0988111222' }).order;
  L.arrive(S, truck.id); L.startWash(S, truck.id); L.completeWash(S, truck.id); L.moveToLot(S, truck.id); L.arrive(S, truck.id);
  const parked = L.spawnVehicle(S, { type: 'CAR', plate: '43K-246.80', service: 'PARK', wallet: 500000, name: 'Phạm Minh Đức', phone: '0905246802' }).order;
  L.arrive(S, parked.id);
  parked.dwell = 6; // đã gửi được một lúc
  truck.dwell = -6; // ở bãi chờ thêm một lúc rồi mới ra cổng
}
function resetAll(seed) {
  for (const m of [...meshes.values()]) disposeMesh(m);
  S = L.createState();
  demo = { on: false, t0: 0, i: 0 };
  $('demo').textContent = '▶ Chạy minh họa';
  $('demo').classList.add('primary');
  $('log').innerHTML = '';
  camLast = null; lastLogN = 0; prevExit = 'IDLE'; selected = { kind: 'station' }; camEvt = null; lastDetails = '';
  if (seed) seedInitial();
  L.log(S, seed ? 'MÔ PHỎNG: hai ô tô đang rửa, xe tải ở bãi chờ, một ô tô đang gửi xe.' : 'Bắt đầu kịch bản minh họa.', 'info');
  updateUI();
}

// ---------- chế độ TRỰC TIẾP (backend FastAPI) ----------
function setMode(m) {
  mode = m;
  liveBays = null; liveWarned = false; liveTimer = 0;
  $('modeBtn').textContent = m === 'live' ? 'Chế độ: Trực tiếp' : 'Chế độ: Mô phỏng';
  if (m === 'live') toast('Đang đọc /api/bays từ backend…');
  updateUI();
}
async function pollLive() {
  try {
    const r = await fetch('/api/bays', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    liveBays = Object.fromEntries((await r.json()).map((b) => [b.id, b.status]));
  } catch (e) {
    liveBays = null;
    if (!liveWarned) { liveWarned = true; toast('Chưa kết nối được backend (/api/bays). Backend sẽ được xây ở bước sau.'); }
  }
}
$('modeBtn').onclick = () => setMode(mode === 'live' ? 'sim' : 'live');

// ---------- cập nhật giao diện (5 lần/giây) ----------
function updateUI() {
  const st = L.stats(S);
  $('sQueue').textContent = st.queue;
  $('sLot').textContent = st.lot;
  $('sPark').textContent = st.parkUse.used;
  const used = Object.values(st.bayUse).reduce((a, b) => a + b.used, 0);
  const total = Object.values(st.bayUse).reduce((a, b) => a + b.total, 0);
  $('sBays').textContent = `${used}/${total}`;
  $('sExit').textContent = { IDLE: 'Sẵn sàng', WAIT: 'Đang đọc…', OPEN: 'Barie mở', BLOCK: 'CHẶN · thu tiền' }[S.exit.state];
  $('chipExit').classList.toggle('alert', S.exit.state === 'BLOCK');
  renderDetails();

  for (const e of S.log) {
    if (e.n <= lastLogN) continue;
    lastLogN = e.n;
    const r = document.createElement('div');
    r.className = 'logline ' + (e.kind === 'ok' ? 'ok' : e.kind === 'warn' ? 'warn' : '');
    const tm = document.createElement('time');
    tm.textContent = fmtT(e.t);
    const sp = document.createElement('span');
    sp.textContent = e.msg;
    r.append(tm, sp);
    $('log').prepend(r);
    while ($('log').children.length > 12) $('log').lastChild.remove();
  }
  const was = prevExit;
  prevExit = S.exit.state;
  if (S.exit.state === 'BLOCK' && was !== 'BLOCK' && !S.autoCashier) {
    const v = L.getVehicle(S, S.exit.orderId);
    if (v) { toast(`${v.plateLabel} chưa thanh toán: barie hạ. Thu ngân bấm “Đã thu tiền mặt”.`); select({ kind: 'gateOut' }); }
  }
}

// ---------- vòng lặp ----------
let prev = performance.now(), uiAcc = 0;
function frame(now) {
  requestAnimationFrame(frame);
  const raw = Math.min((now - prev) / 1000, 0.06);
  prev = now;
  const live = mode === 'live';
  const dt = paused || live ? 0 : raw * speed;
  if (dt > 0) { runScript(); L.tick(S, dt); }
  syncVehicles(dt);
  updateWorld(paused ? 0 : raw);
  if (live) { liveTimer -= raw; if (liveTimer <= 0) { liveTimer = 2; pollLive(); } }

  if (goal) {
    const k = 1 - Math.exp(-5 * raw);
    camera.position.lerp(goal.p, k);
    controls.target.lerp(goal.t, k);
    if (camera.position.distanceTo(goal.p) < 0.05) goal = null;
  }
  controls.update();

  let rp = null;
  if (selected.kind === 'vehicle') { const m = meshes.get(selected.id); if (m) rp = [m.group.position.x, m.group.position.z, 2.4]; }
  else if (selected.kind === 'bay') rp = [W.bays[selected.id].x, WD.BAY_Z, W.bays[selected.id].w * 0.62];
  else if (selected.kind === 'gateIn') rp = [WD.GATE_IN_X, 8, 3.2];
  else if (selected.kind === 'gateOut') rp = [WD.GATE_OUT_X, -8.5, 3.4];
  ring.visible = !!rp;
  if (rp) { ring.position.x = rp[0]; ring.position.z = rp[1]; ring.scale.setScalar(rp[2]); }

  renderer.render(scene, camera);
  updateCam();
  uiAcc += raw;
  if (uiAcc > 0.2) { uiAcc = 0; updateUI(); }
}

resetAll(true);
requestAnimationFrame(frame);
window.__wp = { get state() { return S; }, L, select, setView, act, setDemo };
