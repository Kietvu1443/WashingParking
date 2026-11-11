// Chạy:  node demo/logic.test.mjs
import assert from 'node:assert/strict';
import * as L from './logic.js';

let n = 0;
const t = (name, fn) => { fn(); n++; console.log('ok  ' + name); };
const mk = () => L.createState({ instantArrive: true });
const car = (s, plate, o = {}) => L.spawnVehicle(s, { type: 'CAR', plate, ...o });
const toWashed = (s, id) => { assert.ok(L.startWash(s, id).ok); assert.ok(L.completeWash(s, id).ok); };

t('normalizePlate', () => {
  assert.equal(L.normalizePlate('51A - 123.45'), '51A12345');
  assert.equal(L.normalizePlate('29-B1 999.88'), '29B199988');
});

t('ô tô có ví đủ tiền: tự trừ ví, đi thẳng qua cổng ra', () => {
  const s = mk();
  const r = car(s, '51A-123.45', { wallet: 500000, dirtScore: 10 });
  assert.equal(r.order.status, 'IN_BAY');
  assert.ok(['B1', 'B2', 'B3'].includes(r.order.bayId));
  toWashed(s, r.order.id);
  assert.ok(L.moveToLot(s, r.order.id).ok);
  assert.equal(s.bays.B1.status, 'AVAILABLE');
  assert.equal(s.lot.length, 1);
  const x = L.goToExit(s, r.order.id);
  assert.equal(x.barrier, 'OPEN');
  assert.equal(r.order.status, 'COMPLETED');
  assert.equal(r.order.payMethod, 'WALLET');
  assert.equal(r.order.wallet, 420000);
  assert.equal(s.revenue, 80000);
});

t('chuyển trạng thái nghiêm ngặt', () => {
  const s = mk();
  const r = car(s, '30A-111.22');
  assert.equal(L.completeWash(s, r.order.id).reason, 'INVALID_STATE');
  assert.equal(L.moveToLot(s, r.order.id).reason, 'INVALID_STATE');
  assert.equal(L.goToExit(s, r.order.id).reason, 'INVALID_STATE');
  assert.equal(L.payCash(s, r.order.id).reason, 'NOT_BLOCKED');
});

t('hết ô: vào hàng chờ, rồi được đưa vào ô khi có chỗ', () => {
  const s = mk();
  const a = ['1', '2', '3'].map((i) => car(s, `51A-000.0${i}`).order);
  assert.deepEqual(a.map((v) => v.bayId), ['B1', 'B2', 'B3']);
  const w = car(s, '51A-000.04').order;
  assert.equal(w.status, 'WAITING_BAY');
  assert.equal(s.queue.length, 1);
  toWashed(s, a[0].id);
  assert.equal(w.status, 'WAITING_BAY'); // ô chưa trả cho tới khi xe rời ô
  L.moveToLot(s, a[0].id);
  assert.equal(w.status, 'IN_BAY');
  assert.equal(w.bayId, 'B1');
  assert.equal(s.queue.length, 0);
});

t('hàng chờ đầy: từ chối xe', () => {
  const s = mk();
  for (let i = 0; i < 3 + L.CFG.QUEUE_CAP; i++) assert.ok(car(s, `60A-100.0${i}`).ok);
  assert.equal(car(s, '60A-999.99').reason, 'QUEUE_FULL');
  // xe tải vẫn vào được vì khoang C còn trống
  assert.ok(L.spawnVehicle(s, { type: 'TRUCK', plate: '29C-111.11' }).ok);
});

t('biển số trùng với xe đang trong trạm', () => {
  const s = mk();
  car(s, '51A-123.45');
  assert.equal(car(s, '51A12345').reason, 'DUPLICATE_PLATE');
});

t('khách vãng lai: gói cơ bản, không có app, chưa trả -> hạ barie -> thu tiền mặt', () => {
  const s = mk();
  const v = L.spawnVehicle(s, { type: 'CAR', plate: '51B-777.88', guest: true, packageIndex: 2, service: 'WASH_PARK', wallet: 999999 }).order;
  assert.equal(v.packageIndex, 0);
  assert.equal(v.price, 80000);
  assert.equal(v.service, 'WASH'); // khách vãng lai chỉ rửa
  assert.equal(v.wallet, 0);
  assert.ok(v.code.startsWith('GT-'));
  toWashed(s, v.id);
  L.moveToLot(s, v.id);
  const x = L.goToExit(s, v.id);
  assert.equal(x.barrier, 'BLOCK');
  assert.equal(v.blockReason, 'GUEST');
  assert.equal(v.status, 'AT_EXIT');
  assert.ok(L.payCash(s, v.id).ok);
  assert.equal(s.exit.state, 'OPEN');
  assert.equal(v.status, 'COMPLETED');
  assert.equal(v.payMethod, 'CASH');
});

t('số dư ví không đủ: hạ barie, ví không bị trừ, thu ngân thu tiền mặt', () => {
  const s = mk();
  const v = car(s, '51C-444.55', { wallet: 20000 }).order;
  toWashed(s, v.id);
  L.moveToLot(s, v.id);
  assert.equal(L.goToExit(s, v.id).barrier, 'BLOCK');
  assert.equal(v.blockReason, 'LOW_BALANCE');
  assert.equal(v.wallet, 20000);
  assert.ok(L.payCash(s, v.id).ok);
  assert.equal(s.exit.state, 'OPEN');
  assert.equal(v.status, 'COMPLETED');
  assert.equal(v.payMethod, 'CASH');
  assert.equal(s.revenue, 80000);
});

t('độ bẩn → thông số rửa và kéo dài thời gian rửa', () => {
  assert.equal(L.dirtParams('CAR', 10).levelName, 'Sạch');
  assert.equal(L.dirtParams('CAR', 40).levelName, 'Vừa');
  assert.equal(L.dirtParams('CAR', 60).levelName, 'Bẩn');
  const hi = L.dirtParams('CAR', 95);
  assert.deepEqual([hi.levelName, hi.pressureBar, hi.tempC, hi.waterL, hi.timeMul], ['Rất bẩn', 140, 55, 180, 1.6]);
  assert.equal(L.dirtParams('TRUCK', 95).waterL, 270); // xe tải ×1.5 nước
  const s = mk();
  const clean = car(s, '51A-000.11', { dirtScore: 5 }).order;
  const dirty = car(s, '51A-000.12', { dirtScore: 95 }).order;
  assert.equal(clean.dirt.levelName, 'Sạch');
  assert.equal(dirty.dirt.pressureBar, 140);
  L.startWash(s, clean.id);
  L.startWash(s, dirty.id);
  assert.ok(Math.abs(clean.washSec - 6 * 0.8) < 1e-9);
  assert.ok(Math.abs(dirty.washSec - 6 * 1.6) < 1e-9);
  assert.equal(clean.price, dirty.price); // giá không đổi theo độ bẩn
});

const parkOnly = (s, plate, o = {}) => L.spawnVehicle(s, { type: 'CAR', plate, service: 'PARK', ...o });
const park = (s, plate, o = {}) => parkOnly(s, plate, o);

t('chỉ gửi xe: nhận chỗ đỗ, tính phí theo giờ, ví tự trừ khi ra', () => {
  const s = mk();
  const v = park(s, '51G-100.01').order;
  assert.equal(v.status, 'PARKED');
  assert.equal(v.parkSpot, 'P1');
  assert.equal(v.price, 0);
  assert.equal(v.bayId, null);
  assert.equal(L.stats(s).parkUse.used, 1);
  assert.equal(L.pickUp(s, v.id).barrier, 'OPEN');
  assert.equal(s.parking.P1.status, 'AVAILABLE');
  assert.equal(v.parkFee, 20000); // dưới 1 giờ -> tối thiểu 1 giờ
  assert.equal(v.status, 'COMPLETED');
  assert.equal(v.payMethod, 'WALLET');
  assert.equal(s.revenue, 20000);
});

t('phí gửi xe làm tròn lên theo giờ (1 giây mô phỏng = 10 phút)', () => {
  assert.equal(L.parkFeeFor('CAR', 0), 20000);
  assert.equal(L.parkFeeFor('CAR', 6), 20000); // đúng 60 phút
  assert.equal(L.parkFeeFor('CAR', 6.1), 40000);
  assert.equal(L.parkFeeFor('TRUCK', 30), 5 * 40000); // 300 phút = 5 giờ
  const s = mk();
  const v = park(s, '51G-100.02').order;
  for (let i = 0; i < 100; i++) L.tick(s, 0.5); // 50 s > parkStay tối đa 40 s
  assert.equal(v.status, 'COMPLETED');
  assert.equal(v.parkFee, L.parkFeeFor('CAR', v.parkSec));
  assert.ok(v.parkSec >= 20 && v.parkSec <= 40.5);
});

t('bãi đỗ đầy: chỉ gửi xe bị từ chối; xe tải dùng chỗ T', () => {
  const s = mk();
  for (let i = 0; i < 10; i++) assert.ok(park(s, `52G-000.0${i}`).ok);
  assert.equal(park(s, '52G-999.99').reason, 'PARKING_FULL');
  const t1 = L.spawnVehicle(s, { type: 'TRUCK', plate: '29C-100.01', service: 'PARK' }).order;
  assert.equal(t1.parkSpot, 'T1');
  assert.equal(L.spawnVehicle(s, { type: 'TRUCK', plate: '29C-100.02', service: 'PARK' }).order.parkSpot, 'T2');
  assert.equal(L.spawnVehicle(s, { type: 'TRUCK', plate: '29C-100.03', service: 'PARK' }).reason, 'PARKING_FULL');
  assert.equal(park(s, '52G-111.11', { service: 'XYZ' }).reason, 'INVALID_SERVICE');
});

t('rửa + gửi: rửa xong sang bãi đỗ, phí = rửa + gửi; hết chỗ thì về bãi chờ', () => {
  const s = L.createState({ instantArrive: true });
  const v = car(s, '51H-100.01', { service: 'WASH_PARK', dirtScore: 10 }).order;
  for (let i = 0; i < 60 && v.status !== 'PARKED'; i++) L.tick(s, 0.25);
  assert.equal(v.status, 'PARKED');
  assert.equal(s.bays.B1.status, 'AVAILABLE');
  assert.equal(L.pickUp(s, v.id).barrier, 'OPEN');
  assert.equal(v.total, 80000 + 20000);
  assert.equal(s.revenue, 100000);
  // bãi đỗ ô tô đầy -> rơi về bãi chờ
  const s2 = L.createState({ instantArrive: true });
  for (let i = 0; i < 10; i++) park(s2, `53G-000.0${i}`);
  const w = car(s2, '51H-200.02', { service: 'WASH_PARK' }).order;
  for (let i = 0; i < 80 && w.status !== 'IN_WAITING_LOT'; i++) L.tick(s2, 0.25);
  assert.equal(w.status, 'IN_WAITING_LOT');
  assert.equal(w.parkFee, 0);
});

t('lấy xe thủ công chỉ khi đang gửi; cổng ra bận thì từ chối', () => {
  const s = mk();
  const a = park(s, '51G-300.01').order;
  const b = park(s, '51G-300.02', { wallet: 0 }).order;
  const w = car(s, '51G-300.03').order;
  assert.equal(L.pickUp(s, w.id).reason, 'INVALID_STATE');
  assert.equal(L.pickUp(s, b.id).barrier, 'BLOCK'); // ví 0đ -> hạ barie
  assert.equal(L.pickUp(s, a.id).reason, 'EXIT_BUSY');
  assert.ok(L.payCash(s, b.id).ok);
  assert.equal(b.total, 20000);
});

t('cổng ra chỉ xử lý một xe; bãi chờ có giới hạn', () => {
  const s = mk();
  const a = car(s, '70A-100.01', { wallet: 0 }).order;
  const b = car(s, '70A-100.02').order;
  [a, b].forEach((v) => { toWashed(s, v.id); L.moveToLot(s, v.id); });
  assert.equal(L.goToExit(s, a.id).barrier, 'BLOCK');
  assert.equal(L.goToExit(s, b.id).reason, 'EXIT_BUSY');
  // bãi chờ đầy
  const s2 = mk();
  for (let i = 0; i < L.CFG.LOT_CAP; i++) {
    const v = car(s2, `71A-200.0${i}`).order;
    toWashed(s2, v.id);
    assert.ok(L.moveToLot(s2, v.id).ok);
  }
  const x = car(s2, '71A-299.99').order;
  toWashed(s2, x.id);
  assert.equal(L.moveToLot(s2, x.id).reason, 'LOT_FULL');
  assert.equal(x.status, 'WASHED');
});

t('không nhận xe máy (ngoài phạm vi)', () => {
  assert.equal(L.spawnVehicle(mk(), { type: 'MOTORBIKE', plate: '59X1-678.90' }).reason, 'INVALID_TYPE');
});

t('xe tải dùng khoang C1, bảo trì khoang', () => {
  const s = mk();
  assert.equal(L.spawnVehicle(s, { type: 'TRUCK', plate: '29C-888.88' }).order.bayId, 'C1');
  assert.equal(L.setMaintenance(s, 'C1', true).reason, 'BAY_BUSY');
  assert.ok(L.setMaintenance(s, 'B1', true).ok);
  assert.equal(car(s, '51A-000.01').order.bayId, 'B2');
});

t('tick tự động: kịch bản ví đủ tiền chạy tới COMPLETED', () => {
  const s = L.createState({ instantArrive: true });
  const v = car(s, '51F-888.99', { packageIndex: 1 }).order;
  for (let i = 0; i < 400 && v.status !== 'COMPLETED'; i++) L.tick(s, 0.25);
  assert.equal(v.status, 'COMPLETED');
  assert.equal(v.payMethod, 'WALLET');
  assert.equal(s.completed, 1);
});

t('tick tự động: xe tải chưa trả bị chặn tới khi thu ngân thu tiền', () => {
  const s = L.createState({ instantArrive: true });
  const v = L.spawnVehicle(s, { type: 'TRUCK', plate: '29C-888.88', guest: true }).order;
  let blocked = false;
  for (let i = 0; i < 800 && v.status !== 'COMPLETED'; i++) {
    L.tick(s, 0.25);
    if (s.exit.state === 'BLOCK') blocked = true;
  }
  assert.equal(blocked, true);
  assert.equal(v.status, 'AT_EXIT'); // không có thu ngân tự động
  assert.ok(L.payCash(s, v.id).ok);
  assert.equal(v.status, 'COMPLETED');
});

t('không instantArrive: xe phải tới nơi (arrive) mới tự rửa / ra cổng', () => {
  const s = L.createState();
  const v = car(s, '51A-555.55').order;
  for (let i = 0; i < 40; i++) L.tick(s, 0.25);
  assert.equal(v.status, 'IN_BAY'); // chưa tới khoang
  L.arrive(s, v.id);
  for (let i = 0; i < 400 && v.status !== 'IN_WAITING_LOT'; i++) L.tick(s, 0.25);
  assert.equal(v.status, 'IN_WAITING_LOT');
  for (let i = 0; i < 40; i++) L.tick(s, 0.25);
  assert.equal(v.status, 'IN_WAITING_LOT'); // chưa arrive tại bãi chờ
  L.arrive(s, v.id);
  for (let i = 0; i < 40 && v.status !== 'AT_EXIT'; i++) L.tick(s, 0.25);
  assert.equal(v.status, 'AT_EXIT');
  assert.equal(s.exit.state, 'WAIT'); // chờ camera cổng ra
  L.arrive(s, v.id);
  L.tick(s, 0.25);
  assert.equal(v.status, 'COMPLETED');
});

console.log(`\n${n} nhóm kiểm thử đạt`);
