// WashingParking: máy trạng thái đơn rửa xe / gửi xe (thuần logic, không phụ thuộc Three.js / DOM).
// Dùng chung làm tham chiếu cho backend (xem docs/superpowers/specs).
//
// Luồng rửa:        WAITING_BAY? -> IN_BAY -> WASHING -> WASHED -> IN_WAITING_LOT -> AT_EXIT -> COMPLETED
// Luồng gửi xe:     PARKED -> AT_EXIT -> COMPLETED           (dịch vụ PARK)
// Luồng rửa + gửi:  ... WASHED -> PARKED -> AT_EXIT -> COMPLETED (hết chỗ đỗ -> rơi về IN_WAITING_LOT)
// (Phạm vi hiện tại: ô tô và xe tải. Xe máy chưa nằm trong phạm vi.)
// Ô rửa được trả lại khi xe rời ô (moveToLot / moveToParking), không phải lúc rửa xong.

export const TYPES = {
  CAR: { name: 'Ô tô' },
  TRUCK: { name: 'Xe tải' },
};

export const BAY_DEFS = [
  { id: 'B1', type: 'CAR' },
  { id: 'B2', type: 'CAR' },
  { id: 'B3', type: 'CAR' },
  { id: 'C1', type: 'TRUCK' },
];

// Bãi đỗ xe riêng (tách khỏi bãi chờ): 10 chỗ ô tô + 2 chỗ xe tải
export const PARK_DEFS = [
  ...Array.from({ length: 10 }, (_, i) => ({ id: `P${i + 1}`, type: 'CAR' })),
  { id: 'T1', type: 'TRUCK' },
  { id: 'T2', type: 'TRUCK' },
];
export const PARK_RATE = { CAR: 20000, TRUCK: 40000 }; // đ / giờ

export const SERVICES = {
  WASH: 'Rửa xe',
  PARK: 'Gửi xe',
  WASH_PARK: 'Rửa + gửi xe',
};

// sec = thời gian rửa mô phỏng (giây ở tốc độ 1x)
export const PACKAGES = {
  CAR: [
    { name: 'Rửa cơ bản', price: 80000, sec: 6 },
    { name: 'Bọt tuyết + hút bụi nội thất', price: 150000, sec: 9 },
    { name: 'Gói toàn diện', price: 250000, sec: 12 },
  ],
  TRUCK: [
    { name: 'Rửa ngoài', price: 150000, sec: 8 },
    { name: 'Rửa ngoài + xịt gầm', price: 250000, sec: 12 },
  ],
};

// AI nhận diện độ bẩn (0–100) -> thông số rửa. max = cận trên của mức; timeMul kéo dài thời gian rửa.
export const DIRT_LEVELS = [
  { name: 'Sạch', max: 25, pressureBar: 80, tempC: 25, waterL: 60, timeMul: 0.8 },
  { name: 'Vừa', max: 50, pressureBar: 100, tempC: 35, waterL: 100, timeMul: 1.0 },
  { name: 'Bẩn', max: 75, pressureBar: 120, tempC: 45, waterL: 140, timeMul: 1.3 },
  { name: 'Rất bẩn', max: 100, pressureBar: 140, tempC: 55, waterL: 180, timeMul: 1.6 },
];
const TRUCK_WATER_MUL = 1.5;

export function dirtParams(type, score) {
  const sc = Math.max(0, Math.min(100, Math.round(Number(score) || 0)));
  const idx = DIRT_LEVELS.findIndex((l) => sc <= l.max);
  const l = DIRT_LEVELS[idx];
  return {
    score: sc,
    level: idx,
    levelName: l.name,
    pressureBar: l.pressureBar,
    tempC: l.tempC,
    waterL: Math.round(l.waterL * (type === 'TRUCK' ? TRUCK_WATER_MUL : 1)),
    timeMul: l.timeMul,
  };
}

export const CFG = {
  QUEUE_CAP: 4,
  LOT_CAP: 6,
  IN_BAY_DELAY: 2.0, // từ lúc xe vào ô tới lúc bắt đầu rửa (camera trong ô quét độ bẩn)
  STAFF_DELAY: 3.0, // nhân viên đưa xe ra sau khi rửa xong
  LOT_DWELL: 4.0, // khách lấy xe sau khi xe vào bãi chờ
  EXIT_PASS: 3.0, // barie cổng ra giữ mở cho xe đi qua
  CASHIER_DELAY: 4.0, // thu ngân tự thu tiền (chế độ demo tự động)
  SIM_MIN_PER_SEC: 10, // 1 giây mô phỏng = 10 phút thực (tính phí gửi xe)
  PARK_STAY_MIN: 20, // thời gian gửi ngẫu nhiên (giây mô phỏng)
  PARK_STAY_MAX: 40,
  DEFAULT_WALLET: 500000,
  PURGE_AFTER: 60,
};

export const STATUS_VI = {
  WAITING_BAY: 'Đang chờ ô rửa',
  IN_BAY: 'Đã vào ô rửa',
  WASHING: 'Đang rửa',
  WASHED: 'Đã rửa xong',
  IN_WAITING_LOT: 'Ở bãi chờ',
  PARKED: 'Đang gửi xe',
  AT_EXIT: 'Tại cổng ra',
  COMPLETED: 'Đã ra khỏi trạm',
};

export const normalizePlate = (p) => String(p ?? '').toUpperCase().replace(/[^A-Z0-9]/g, '');

export function createState(opts = {}) {
  return {
    time: 0,
    nextId: 1,
    logN: 0,
    instantArrive: !!opts.instantArrive, // test: xe tới nơi ngay lập tức
    autoCashier: !!opts.autoCashier,
    vehicles: [],
    bays: Object.fromEntries(BAY_DEFS.map((b) => [b.id, { id: b.id, type: b.type, status: 'AVAILABLE', orderId: null }])),
    parking: Object.fromEntries(PARK_DEFS.map((b) => [b.id, { id: b.id, type: b.type, status: 'AVAILABLE', orderId: null }])),
    queue: [], // id xe chờ ô rửa (trước khi rửa)
    lot: [], // id xe ở bãi chờ (sau khi rửa, ô tô/xe tải)
    exit: { state: 'IDLE', orderId: null, timer: 0 }, // IDLE | WAIT | OPEN | BLOCK
    log: [],
    revenue: 0,
    completed: 0,
  };
}

const ok = (extra = {}) => ({ ok: true, ...extra });
const fail = (reason) => ({ ok: false, reason });
const vnd = (n) => `${n.toLocaleString('vi-VN')}đ`;

export function log(s, msg, kind = 'info') {
  s.log.push({ n: ++s.logN, t: s.time, msg, kind });
  if (s.log.length > 300) s.log.shift();
}

export const getVehicle = (s, id) => s.vehicles.find((v) => v.id === id) || null;
const MOVING = new Set(['IN_BAY', 'WAITING_BAY', 'IN_WAITING_LOT', 'PARKED', 'AT_EXIT', 'COMPLETED']);

function setStatus(s, v, st) {
  v.status = st;
  v.dwell = 0;
  v.arrived = s.instantArrive || !MOVING.has(st);
  if (st === 'WASHING') v.tWashStart = s.time;
  if (st === 'WASHED') v.tWashEnd = s.time;
  if (st === 'IN_WAITING_LOT') v.tLot = s.time;
  if (st === 'PARKED') v.tParkIn = s.time;
  if (st === 'COMPLETED') v.tExit = s.time;
}

export function arrive(s, id) {
  const v = getVehicle(s, id);
  if (v) v.arrived = true;
}

const freeBay = (s, type) => Object.values(s.bays).find((b) => b.type === type && b.status === 'AVAILABLE') || null;
const freeSpot = (s, type) => Object.values(s.parking).find((p) => p.type === type && p.status === 'AVAILABLE') || null;

// Phí gửi xe: làm tròn lên theo giờ, tối thiểu 1 giờ. parkedSec = giây mô phỏng đã đỗ.
export const parkFeeFor = (type, parkedSec) => Math.ceil(Math.max(1, (parkedSec * CFG.SIM_MIN_PER_SEC) / 60)) * PARK_RATE[type];

function assignBay(s, v, bay) {
  bay.status = 'OCCUPIED';
  bay.orderId = v.id;
  v.bayId = bay.id;
  v.dirt = dirtParams(v.type, v.dirtScore);
  setStatus(s, v, 'IN_BAY');
  log(s, `${v.plate} → khoang ${bay.id} (${TYPES[v.type].name})`, 'ok');
}

function releaseBay(s, v) {
  const bay = s.bays[v.bayId];
  if (bay && bay.orderId === v.id) {
    bay.status = 'AVAILABLE';
    bay.orderId = null;
  }
  v.lastBay = v.bayId;
  v.bayId = null;
  promoteQueue(s);
}

function takeSpot(s, v, spot) {
  spot.status = 'OCCUPIED';
  spot.orderId = v.id;
  v.parkSpot = spot.id;
  setStatus(s, v, 'PARKED');
  log(s, `${v.plate} → chỗ đỗ ${spot.id} (${TYPES[v.type].name}) · ${vnd(PARK_RATE[v.type])}/giờ`, 'ok');
}

function promoteQueue(s) {
  for (const id of [...s.queue]) {
    const v = getVehicle(s, id);
    const bay = v && freeBay(s, v.type);
    if (bay) {
      s.queue.splice(s.queue.indexOf(id), 1);
      assignBay(s, v, bay);
      log(s, `${v.plate} rời hàng chờ, vào khoang ${bay.id}`, 'info');
    }
  }
}

export function spawnVehicle(s, o = {}) {
  const type = o.type;
  if (!TYPES[type]) return fail('INVALID_TYPE');
  const plate = normalizePlate(o.plate);
  if (!plate) return fail('INVALID_PLATE');
  if (s.vehicles.some((v) => v.plate === plate && v.status !== 'COMPLETED')) return fail('DUPLICATE_PLATE');
  const guest = !!o.guest;
  const service = guest ? 'WASH' : o.service ?? 'WASH'; // khách vãng lai chỉ rửa
  if (!SERVICES[service]) return fail('INVALID_SERVICE');
  const wash = service !== 'PARK';
  const bay = wash ? freeBay(s, type) : null;
  const spot = service === 'PARK' ? freeSpot(s, type) : null;
  if (service === 'PARK' && !spot) return fail('PARKING_FULL');
  if (wash && !bay && s.queue.length >= CFG.QUEUE_CAP) return fail('QUEUE_FULL');

  const pkgs = PACKAGES[type];
  // khách vãng lai không có đặt trước -> gói cơ bản; khách đã đăng ký dùng gói đặt trước (nếu hợp lệ)
  const pkgIdx = guest || !pkgs[o.packageIndex] ? 0 : o.packageIndex;
  const pkg = wash ? pkgs[pkgIdx] : null;
  const id = s.nextId++;
  const v = {
    id,
    code: guest ? `GT-${String(id).padStart(4, '0')}` : `WP-${String(id).padStart(4, '0')}`,
    plate,
    plateLabel: o.plate ? String(o.plate).toUpperCase().trim() : plate,
    type,
    guest,
    name: guest ? 'Khách vãng lai' : o.name || 'Nguyễn Văn A',
    phone: guest ? '' : o.phone || '0900000000',
    service,
    wallet: guest ? 0 : Number.isFinite(o.wallet) ? o.wallet : CFG.DEFAULT_WALLET,
    packageIndex: wash ? pkgIdx : null,
    packageName: wash ? pkg.name : 'Gửi xe',
    price: wash ? pkg.price : 0, // phí rửa
    baseSec: wash ? pkg.sec : 0,
    washSec: wash ? pkg.sec : 0, // chốt lại ở startWash theo độ bẩn
    dirtScore: Number.isFinite(o.dirtScore) ? o.dirtScore : Math.floor(Math.random() * 101),
    dirt: null,
    parkSpot: null,
    parkStay: CFG.PARK_STAY_MIN + Math.random() * (CFG.PARK_STAY_MAX - CFG.PARK_STAY_MIN),
    tParkIn: null,
    parkFee: 0,
    total: wash ? pkg.price : 0,
    paid: false,
    payMethod: null,
    blockReason: null,
    status: 'WAITING_BAY',
    bayId: null,
    lastBay: null,
    progress: 0,
    dwell: 0,
    arrived: false,
    tEntry: s.time,
    tWashStart: null,
    tWashEnd: null,
    tLot: null,
    tPaid: null,
    tExit: null,
    conf: Math.round(90 + Math.random() * 9),
  };
  s.vehicles.push(v);
  log(s, `Cảm biến + camera cổng vào: ${TYPES[type].name} · ${v.plateLabel} (${v.conf}%) · ${guest ? 'vé vãng lai ' + v.code : 'khớp tài khoản ' + v.name} · ${SERVICES[service]}`, 'info');
  if (service === 'PARK') takeSpot(s, v, spot);
  else if (bay) assignBay(s, v, bay);
  else {
    s.queue.push(id);
    setStatus(s, v, 'WAITING_BAY');
    log(s, `${v.plate}: hết khoang ${TYPES[type].name}, vào hàng chờ (${s.queue.length}/${CFG.QUEUE_CAP})`, 'warn');
  }
  return ok({ order: v });
}

export function startWash(s, id) {
  const v = getVehicle(s, id);
  if (!v) return fail('NOT_FOUND');
  if (v.status !== 'IN_BAY') return fail('INVALID_STATE');
  v.washSec = v.baseSec * v.dirt.timeMul;
  setStatus(s, v, 'WASHING');
  v.progress = 0;
  const d = v.dirt;
  log(s, `${v.plate} độ bẩn ${d.score}% (${d.levelName}) → ${d.pressureBar} bar · ${d.tempC}°C · ${d.waterL} L · ×${d.timeMul} thời gian`, 'info');
  log(s, `${v.plate} bắt đầu rửa · ${v.packageName}`, 'info');
  return ok({ order: v });
}

export function completeWash(s, id) {
  const v = getVehicle(s, id);
  if (!v) return fail('NOT_FOUND');
  if (v.status !== 'WASHING') return fail('INVALID_STATE');
  v.progress = 1;
  setStatus(s, v, 'WASHED');
  log(s, `${v.plate} rửa xong · phí rửa ${vnd(v.price)}`, 'ok');
  return ok({ order: v });
}

export function moveToLot(s, id) {
  const v = getVehicle(s, id);
  if (!v) return fail('NOT_FOUND');
  if (v.status !== 'WASHED') return fail('INVALID_STATE');
  if (s.lot.length >= CFG.LOT_CAP) return fail('LOT_FULL');
  releaseBay(s, v);
  s.lot.push(v.id);
  setStatus(s, v, 'IN_WAITING_LOT');
  log(s, `${v.plate} ra bãi chờ (${s.lot.length}/${CFG.LOT_CAP})`, 'info');
  return ok({ order: v });
}

// Rửa xong -> sang bãi đỗ (dịch vụ WASH_PARK)
export function moveToParking(s, id) {
  const v = getVehicle(s, id);
  if (!v) return fail('NOT_FOUND');
  if (v.status !== 'WASHED') return fail('INVALID_STATE');
  const spot = freeSpot(s, v.type);
  if (!spot) return fail('PARKING_FULL');
  releaseBay(s, v);
  takeSpot(s, v, spot);
  return ok({ order: v });
}

export function goToExit(s, id) {
  const v = getVehicle(s, id);
  if (!v) return fail('NOT_FOUND');
  if (v.status !== 'IN_WAITING_LOT' && v.status !== 'PARKED') return fail('INVALID_STATE');
  if (s.exit.orderId !== null) return fail('EXIT_BUSY');
  if (v.status === 'PARKED') {
    const spot = s.parking[v.parkSpot];
    if (spot && spot.orderId === v.id) {
      spot.status = 'AVAILABLE';
      spot.orderId = null;
    }
    v.parkSec = v.dwell; // giây mô phỏng đã đỗ (chỉ đếm sau khi xe tới chỗ)
    v.parkFee = parkFeeFor(v.type, v.parkSec);
    v.lastSpot = v.parkSpot;
    v.parkSpot = null;
    log(s, `${v.plate} lấy xe khỏi ${v.lastSpot} · phí gửi ${vnd(v.parkFee)}`, 'info');
  } else {
    s.lot.splice(s.lot.indexOf(v.id), 1);
  }
  v.total = v.price + v.parkFee;
  setStatus(s, v, 'AT_EXIT');
  s.exit = { state: 'WAIT', orderId: v.id, timer: 0 };
  evaluateExit(s);
  return ok({ order: v, barrier: s.exit.state });
}

// Khách lấy xe thủ công khỏi bãi đỗ (nút "Lấy xe")
export function pickUp(s, id) {
  const v = getVehicle(s, id);
  if (!v) return fail('NOT_FOUND');
  if (v.status !== 'PARKED') return fail('INVALID_STATE');
  return goToExit(s, id);
}

// Camera cổng ra đọc biển số: ví đủ tiền -> tự trừ, barie không hạ; khách vãng lai / thiếu tiền -> hạ barie.
function evaluateExit(s) {
  const e = s.exit;
  if (e.state !== 'WAIT') return;
  const v = getVehicle(s, e.orderId);
  if (!v || !v.arrived) return;
  log(s, `Camera cổng ra: ${v.plateLabel}`, 'info');
  if (!v.paid && !v.guest && v.wallet >= v.total) {
    v.wallet -= v.total;
    v.paid = true;
    v.payMethod = 'WALLET';
    v.tPaid = s.time;
    s.revenue += v.total;
    log(s, `${v.plate} ví app tự trừ ${vnd(v.total)} (còn ${vnd(v.wallet)})`, 'ok');
  }
  if (v.paid) openExit(s, v);
  else {
    e.state = 'BLOCK';
    v.blockTimer = 0;
    v.blockReason = v.guest ? 'GUEST' : 'LOW_BALANCE';
    log(s, `${v.plate} ${v.guest ? 'khách vãng lai' : `số dư ví ${vnd(v.wallet)} không đủ`} → hạ barie, thu ngân thu ${vnd(v.total)}`, 'warn');
  }
}

function openExit(s, v) {
  s.exit.state = 'OPEN';
  s.exit.timer = 0;
  setStatus(s, v, 'COMPLETED');
  s.completed++;
  log(s, `${v.plate} đã thanh toán (${v.payMethod === 'CASH' ? 'tiền mặt' : 'ví app'}) → barie mở, xe ra khỏi trạm`, 'ok');
}

export function payCash(s, id) {
  const v = getVehicle(s, id);
  if (!v) return fail('NOT_FOUND');
  if (v.paid) return fail('ALREADY_PAID');
  if (v.status !== 'AT_EXIT' || s.exit.orderId !== v.id || s.exit.state !== 'BLOCK') return fail('NOT_BLOCKED');
  v.paid = true;
  v.payMethod = 'CASH';
  v.tPaid = s.time;
  s.revenue += v.total;
  openExit(s, v);
  return ok({ order: v });
}

export function setMaintenance(s, bayId, on) {
  const b = s.bays[bayId];
  if (!b) return fail('NOT_FOUND');
  if (on && b.status !== 'AVAILABLE') return fail('BAY_BUSY');
  if (!on && b.status !== 'MAINTENANCE') return fail('INVALID_STATE');
  b.status = on ? 'MAINTENANCE' : 'AVAILABLE';
  log(s, `Khoang ${bayId} ${on ? 'bảo trì' : 'hoạt động trở lại'}`, 'warn');
  if (!on) promoteQueue(s);
  return ok();
}

export function tick(s, dt) {
  s.time += dt;
  for (const v of s.vehicles) {
    switch (v.status) {
      case 'IN_BAY':
        if (v.arrived && (v.dwell += dt) >= CFG.IN_BAY_DELAY) startWash(s, v.id);
        break;
      case 'WASHING':
        v.progress = Math.min(1, v.progress + dt / v.washSec);
        if (v.progress >= 1) completeWash(s, v.id);
        break;
      case 'WASHED':
        v.dwell += dt;
        if (v.dwell >= CFG.STAFF_DELAY) {
          if (!(v.service === 'WASH_PARK' && moveToParking(s, v.id).ok)) moveToLot(s, v.id);
        }
        break;
      case 'IN_WAITING_LOT':
        if (v.arrived && (v.dwell += dt) >= CFG.LOT_DWELL) goToExit(s, v.id);
        break;
      case 'PARKED':
        if (v.arrived) {
          v.dwell = Math.min(v.dwell + dt, v.parkStay); // chờ cổng ra bận thì không tính thêm phí
          if (v.dwell >= v.parkStay) goToExit(s, v.id);
        }
        break;
      default:
    }
  }
  evaluateExit(s);
  const e = s.exit;
  if (e.state === 'BLOCK' && s.autoCashier) {
    const v = getVehicle(s, e.orderId);
    if (v && (v.blockTimer += dt) >= CFG.CASHIER_DELAY) payCash(s, v.id);
  } else if (e.state === 'OPEN' && (e.timer += dt) >= CFG.EXIT_PASS) {
    s.exit = { state: 'IDLE', orderId: null, timer: 0 };
  }
  s.vehicles = s.vehicles.filter((v) => !(v.status === 'COMPLETED' && s.time - v.tExit > CFG.PURGE_AFTER));
}

export function stats(s) {
  const active = s.vehicles.filter((v) => v.status !== 'COMPLETED');
  const inType = (t) => Object.values(s.bays).filter((b) => b.type === t && b.status === 'OCCUPIED').length;
  const total = (t) => Object.values(s.bays).filter((b) => b.type === t).length;
  const spots = Object.values(s.parking);
  return {
    queue: s.queue.length,
    lot: s.lot.length,
    inStation: active.length,
    washing: active.filter((v) => v.status === 'WASHING').length,
    bayUse: Object.fromEntries(Object.keys(TYPES).map((t) => [t, { used: inType(t), total: total(t) }])),
    parkUse: { used: spots.filter((p) => p.status === 'OCCUPIED').length, total: spots.length },
  };
}

// Chỉ số bước trên timeline của app khách hàng
export const TIMELINE = ['Vào trạm', 'Vào khoang', 'Đang rửa', 'Rửa xong', 'Bãi chờ / đỗ', 'Thanh toán', 'Ra cổng'];
export function timelineIndex(v) {
  switch (v.status) {
    case 'WAITING_BAY': return 0;
    case 'IN_BAY': return 1;
    case 'WASHING': return 2;
    case 'WASHED': return 3;
    case 'IN_WAITING_LOT':
    case 'PARKED': return 4;
    case 'AT_EXIT': return 5;
    default: return 6;
  }
}
