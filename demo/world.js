// Dựng cảnh tĩnh của trạm: đường, khu rửa (6 khoang), hàng chờ, bãi chờ, cổng vào/ra.
import * as THREE from 'three';
import { mat, glow, box, cyl, sphere, textSprite } from './models.js';
import { BAY_DEFS, PARK_DEFS, TYPES } from './logic.js';

export const BAY_X = { B1: -6.5, B2: -2, B3: 2.5, C1: 8 };
export const BAY_W = { B1: 4, B2: 4, B3: 4, C1: 6 };
export const BAY_Z = -2;
export const ROAD_Z = 6; // đường vào (chạy theo trục X)
export const BACK_Z = -9; // đường sau khu rửa, dẫn ra cổng ra
export const QUEUE_Z = 13;
export const LOT_Z = -15;
export const GATE_IN_X = -24;
export const GATE_OUT_X = 26;
export const QUEUE_X = [-20, -15.5, -11, -6.5];
export const LOT_X = [-12.5, -7.5, -2.5, 2.5, 7.5, 12.5];
export const START = [-48, ROAD_Z];
export const EXIT_STOP = [23.5, BACK_Z];
export const END = [54, BACK_Z];
export const PARK_Z = 13; // hàng chỗ đỗ, quay ra đường vào (ROAD_Z)
export const CONN_X = 18; // đường nối đường vào <-> đường sau (xe từ khoang sang bãi đỗ, và từ bãi đỗ ra cổng)
const PARK_W = { CAR: 2.6, TRUCK: 3.8 };
const PARK_X0 = 2;
export const PARK_POS = {}; // id chỗ đỗ -> [x, z]
let PARK_X1 = PARK_X0;
for (const { id, type } of PARK_DEFS) {
  PARK_POS[id] = [PARK_X1 + PARK_W[type] / 2, PARK_Z];
  PARK_X1 += PARK_W[type];
}
export const CCTV = {
  in: { pos: [-27.6, 5, 9.4], target: [-31, 0.5, 6], label: 'CAM 01 · CỔNG VÀO' },
  out: { pos: [21, 5, -5.2], target: [24.5, 0.5, -9], label: 'CAM 02 · CỔNG RA' },
};

function makeRoller(g, x, y, z, len, vertical) {
  const pivot = new THREE.Group();
  pivot.position.set(x, y, z);
  const m = mat('#38b6d6', { rough: 0.5 });
  if (vertical) {
    cyl(pivot, 0.18, len, 0, 0, 0, '#2b8aa3');
    for (let k = 0; k < 3; k++) {
      const f = box(pivot, 0.95, len - 0.2, 0.05, 0, 0, 0, m, true);
      f.rotation.y = (k * Math.PI) / 3;
    }
  } else {
    cyl(pivot, 0.18, len, 0, 0, 0, '#2b8aa3', { rz: Math.PI / 2 });
    for (let k = 0; k < 3; k++) {
      const f = box(pivot, len - 0.2, 0.95, 0.05, 0, 0, 0, m, true);
      f.rotation.x = (k * Math.PI) / 3;
    }
  }
  g.add(pivot);
  return { pivot, vertical };
}

function tree(scene, x, z, s = 1) {
  const g = new THREE.Group();
  cyl(g, 0.18 * s, 1.2 * s, 0, 0.6 * s, 0, '#7a5a3a');
  sphere(g, 1.0 * s, 0, 1.9 * s, 0, '#4aa56a');
  sphere(g, 0.7 * s, 0.5 * s, 2.4 * s, 0.2 * s, '#5bb87a');
  g.position.set(x, 0, z);
  scene.add(g);
}

export function buildWorld(scene) {
  const W = { labels: [], pickRoots: [], bays: {}, parking: {}, gateIn: null, gateOut: null };
  const label = (text, x, y, z, opt) => {
    const s = textSprite(text, opt);
    s.position.set(x, y, z);
    scene.add(s);
    W.labels.push(s);
    return s;
  };
  const pickGroup = (pick) => {
    const g = new THREE.Group();
    g.userData.pick = pick;
    scene.add(g);
    W.pickRoots.push(g);
    return g;
  };

  // ánh sáng
  scene.add(new THREE.HemisphereLight(0xffffff, 0x9fb0b5, 2.3));
  const sun = new THREE.DirectionalLight(0xfff4df, 3);
  sun.position.set(-20, 40, 28);
  sun.castShadow = true;
  sun.shadow.mapSize.set(2048, 2048);
  Object.assign(sun.shadow.camera, { left: -48, right: 48, top: 40, bottom: -40, near: 1, far: 120 });
  sun.shadow.normalBias = 0.05;
  scene.add(sun);

  // mặt đất + đường
  const ground = new THREE.Mesh(new THREE.PlaneGeometry(260, 260), mat('#dbe6e1', { rough: 1 }));
  ground.rotation.x = -Math.PI / 2;
  ground.receiveShadow = true;
  scene.add(ground);
  const road = '#4a5459';
  box(scene, 76, 0.1, 5, -12, 0.05, ROAD_Z, road, true);
  box(scene, 22, 0.1, 9, -13.5, 0.05, QUEUE_Z, '#566268', true);
  box(scene, 72, 0.1, 5.5, 17, 0.05, -8.75, road, true);
  box(scene, 34, 0.1, 7.4, 0, 0.05, LOT_Z, '#566268', true);
  // bãi đỗ xe: đường vào kéo dài, nền bãi, đường nối sang đường sau
  box(scene, PARK_X1 + 4 - 26, 0.1, 5, (26 + PARK_X1 + 4) / 2, 0.05, ROAD_Z, road, true);
  box(scene, PARK_X1 - PARK_X0 + 0.8, 0.1, 9, (PARK_X0 + PARK_X1) / 2, 0.05, PARK_Z, '#566268', true);
  box(scene, 3.4, 0.1, 9.5, CONN_X, 0.05, -1.25, road, true);
  box(scene, 22, 0.12, 9.6, 1.25, 0.06, -1.2, '#b9c7cb', true);
  // vạch kẻ đường
  for (let x = -46; x < 18; x += 4) box(scene, 1.8, 0.02, 0.12, x, 0.11, ROAD_Z, '#e9eef0', true);
  for (let x = -12; x < 52; x += 4) box(scene, 1.8, 0.02, 0.12, x, 0.11, -8.75, '#e9eef0', true);

  // vị trí hàng chờ (4) và bãi chờ (6)
  QUEUE_X.forEach((x, i) => {
    box(scene, 3.6, 0.02, 7.6, x, 0.11, QUEUE_Z, '#6b787e', true);
    label(`Chờ ${i + 1}`, x, 0.9, QUEUE_Z + 4.6, { w: 192, h: 64, scale: [1.8, 0.6], font: 'bold 28px "Segoe UI", Arial' });
  });
  LOT_X.forEach((x, i) => {
    box(scene, 4.2, 0.02, 7.0, x, 0.11, LOT_Z, '#6b787e', true);
    label(`L${i + 1}`, x, 0.9, LOT_Z - 4.2, { w: 128, h: 64, scale: [1.3, 0.65] });
  });
  label('KHU CHỜ RỬA (hết khoang)', -13.5, 4.2, 18, { w: 512, h: 72, scale: [6.5, 0.9], bg: '#27424d', fg: '#fff', border: null, font: 'bold 34px "Segoe UI", Arial' });
  label('BÃI CHỜ · Ô TÔ / XE TẢI', 0, 4.2, -20.5, { w: 512, h: 72, scale: [6.5, 0.9], bg: '#27424d', fg: '#fff', border: null, font: 'bold 34px "Segoe UI", Arial' });
  label('KHU RỬA XE', 1.25, 6.4, -2, { w: 384, h: 72, scale: [4.6, 0.9], bg: '#1d6a80', fg: '#fff', border: null, font: 'bold 40px "Segoe UI", Arial' });

  // bãi đỗ xe: 10 chỗ ô tô + 2 chỗ xe tải, đèn xanh = trống, đỏ = có xe
  {
    const g = pickGroup({ kind: 'parking' });
    PARK_DEFS.forEach(({ id, type }) => {
      const [x, z] = PARK_POS[id];
      const w = PARK_W[type];
      box(g, w - 0.2, 0.02, 7.6, x, 0.11, z, type === 'TRUCK' ? '#7a8a90' : '#6b787e', true);
      box(g, 0.08, 0.025, 7.6, x - w / 2 + 0.05, 0.12, z, '#e9eef0', true);
      const lightMat = glow('#2fb36a', 0.9);
      box(g, w - 0.6, 0.12, 0.22, x, 0.24, z - 3.4, lightMat, true);
      label(id, x, 0.9, z + 4.6, { w: 128, h: 64, scale: [1.3, 0.65] });
      W.parking[id] = { id, type, x, z, lightMat };
    });
    box(g, 0.08, 0.025, 7.6, PARK_X1 - 0.05, 0.12, PARK_Z, '#e9eef0', true);
    label('BÃI ĐỖ XE · 10 Ô TÔ / 2 XE TẢI', (PARK_X0 + PARK_X1) / 2, 4.2, 19.5, { w: 576, h: 72, scale: [7.4, 0.92], bg: '#27424d', fg: '#fff', border: null, font: 'bold 34px "Segoe UI", Arial' });
  }

  // vách ngăn khoang
  const ids = BAY_DEFS.map((b) => b.id);
  const edges = [BAY_X[ids[0]] - BAY_W[ids[0]] / 2 - 0.2];
  for (let i = 0; i < ids.length - 1; i++) {
    const a = ids[i], b = ids[i + 1];
    edges.push((BAY_X[a] + BAY_W[a] / 2 + BAY_X[b] - BAY_W[b] / 2) / 2);
  }
  const last = ids[ids.length - 1];
  edges.push(BAY_X[last] + BAY_W[last] / 2 + 0.2);
  edges.forEach((x) => box(scene, 0.16, 1.1, 7.6, x, 0.55, BAY_Z, '#8da2aa'));

  // 4 khoang rửa (3 ô tô + 1 xe tải)
  BAY_DEFS.forEach(({ id, type }) => {
    const x = BAY_X[id], w = BAY_W[id];
    const group = pickGroup({ kind: 'bay', id });
    box(group, w - 0.2, 0.14, 7.6, x, 0.13, BAY_Z, '#cdd8dc', true);
    const lightMat = glow('#2fb36a', 0.9);
    box(group, w - 0.5, 0.12, 0.22, x, 0.24, 2.4, lightMat, true);
    const gantry = new THREE.Group();
    group.add(gantry);
    const half = w / 2 - 0.12;
    box(gantry, 0.2, 4, 0.25, x - half, 2, BAY_Z, '#6e8791');
    box(gantry, 0.2, 4, 0.25, x + half, 2, BAY_Z, '#6e8791');
    box(gantry, w - 0.2, 0.3, 0.35, x, 4.05, BAY_Z, '#4f6872');
    box(gantry, w - 0.8, 0.08, 0.08, x, 3.82, BAY_Z, '#c9d6db', true);
    const rollers = [
      makeRoller(gantry, x, 3.35, BAY_Z, w - 0.8, false),
      makeRoller(gantry, x - (w / 2 - 0.6), 1.5, BAY_Z, 2.6, true),
      makeRoller(gantry, x + (w / 2 - 0.6), 1.5, BAY_Z, 2.6, true),
    ];

    // thanh tiến độ luôn hướng về camera
    const barGroup = new THREE.Group();
    barGroup.position.set(x, type === 'TRUCK' ? 6.2 : 5.4, BAY_Z);
    const bw = Math.max(2.2, w - 0.6);
    const bgM = new THREE.MeshBasicMaterial({ color: '#16232a', depthTest: false, transparent: true, opacity: 0.85 });
    const bg = new THREE.Mesh(new THREE.PlaneGeometry(bw + 0.16, 0.42), bgM);
    bg.renderOrder = 18;
    const fillGeo = new THREE.PlaneGeometry(1, 1);
    fillGeo.translate(0.5, 0, 0);
    const fillM = new THREE.MeshBasicMaterial({ color: '#3d8ff0', depthTest: false, transparent: true });
    const fill = new THREE.Mesh(fillGeo, fillM);
    fill.position.set(-bw / 2, 0, 0.01);
    fill.renderOrder = 19;
    barGroup.add(bg, fill);
    barGroup.visible = false;
    scene.add(barGroup);

    // tia nước
    const N = 70;
    const pos = new Float32Array(N * 3);
    const vel = new Float32Array(N);
    for (let i = 0; i < N; i++) pos[i * 3 + 1] = -5;
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    const points = new THREE.Points(geo, new THREE.PointsMaterial({ color: '#8fd3f2', size: 0.17, transparent: true, opacity: 0.85 }));
    points.frustumCulled = false;
    points.visible = false;
    scene.add(points);

    label(`${id} · ${TYPES[type].name}`, x, 1.0, 3.5, { w: 256, h: 64, scale: [2.4, 0.6], font: 'bold 30px "Segoe UI", Arial' });
    W.bays[id] = { id, type, x, w, group, lightMat, gantry, rollers, barGroup, bw, fill, fillM, spray: { pos, vel, geo, points, N }, gz: 0 };
  });

  // cổng vào
  {
    const g = pickGroup({ kind: 'gateIn' });
    box(g, 1.2, 2.6, 1.4, GATE_IN_X, 1.3, 10, '#e8f1f3');
    box(g, 0.9, 0.9, 0.06, GATE_IN_X, 1.7, 9.28, '#9ec9d8');
    box(g, 1.5, 0.2, 1.7, GATE_IN_X, 2.7, 10, '#1d6a80');
    box(g, 0.5, 1.05, 0.5, GATE_IN_X, 0.52, 8.9, '#4f6872');
    const pivot = new THREE.Group();
    pivot.position.set(GATE_IN_X, 1.1, 8.9);
    for (let i = 0; i < 5; i++) box(pivot, 0.2, 0.2, 1.06, 0, 0, -(0.53 + i * 1.06), i % 2 ? '#ffffff' : '#e03a3a');
    box(pivot, 0.3, 0.3, 0.8, 0, 0, 0.5, '#4f6872');
    g.add(pivot);
    box(g, 0.2, 3.4, 0.2, -28, 1.7, 9.4, '#6e8791');
    box(g, 0.7, 0.4, 0.55, -28, 3.5, 8.8, '#2b3a42');
    const camLed = glow('#ee5a4d', 1);
    sphere(g, 0.08, -28, 3.55, 8.5, camLed);
    box(scene, 2.4, 0.03, 4.8, -31, 0.12, ROAD_Z, '#2d363b', true);
    box(g, 0.15, 0.9, 0.15, -31, 0.45, 9.3, '#6e8791');
    const sensorMat = glow('#2fb36a', 1);
    sphere(g, 0.14, -31, 1.0, 9.3, sensorMat);
    label('CỔNG VÀO · AI', GATE_IN_X, 4.3, 10, { w: 320, h: 64, scale: [3.4, 0.7], bg: '#1d6a80', fg: '#fff', border: null });
    W.gateIn = { pivot, sensorMat, camLed, angle: 0 };
  }

  // cổng ra
  {
    const g = pickGroup({ kind: 'gateOut' });
    box(g, 0.5, 1.05, 0.5, GATE_OUT_X, 0.52, -5.4, '#4f6872');
    const pivot = new THREE.Group();
    pivot.position.set(GATE_OUT_X, 1.1, -5.4);
    for (let i = 0; i < 5; i++) box(pivot, 0.2, 0.2, 1.06, 0, 0, -(0.53 + i * 1.06), i % 2 ? '#ffffff' : '#e03a3a');
    box(pivot, 0.3, 0.3, 0.8, 0, 0, 0.5, '#4f6872');
    g.add(pivot);
    box(g, 0.25, 3.5, 0.25, GATE_OUT_X - 1.1, 1.75, -5.4, '#6e8791');
    const redMat = glow('#ee5a4d', 0.1);
    const greenMat = glow('#2fb36a', 1.2);
    sphere(g, 0.24, GATE_OUT_X - 1.1, 3.35, -5.1, redMat);
    sphere(g, 0.24, GATE_OUT_X - 1.1, 2.8, -5.1, greenMat);
    box(g, 2.6, 2.6, 2.2, GATE_OUT_X + 3.2, 1.3, -4.3, '#e8f1f3');
    box(g, 2.8, 0.2, 2.4, GATE_OUT_X + 3.2, 2.7, -4.3, '#1d6a80');
    box(g, 1.8, 0.9, 0.06, GATE_OUT_X + 3.2, 1.7, -5.42, '#9ec9d8');
    const monitorMat = glow('#3aa3bf', 0.9);
    box(g, 0.8, 0.5, 0.05, GATE_OUT_X + 3.2, 1.55, -5.3, monitorMat, true);
    box(g, 0.2, 3.4, 0.2, 21, 1.7, -5.4, '#6e8791');
    box(g, 0.7, 0.4, 0.55, 21, 3.5, -5.8, '#2b3a42');
    sphere(g, 0.08, 21, 3.55, -6.1, glow('#ee5a4d', 1));
    box(scene, 0.3, 0.03, 5.4, 24.9, 0.12, -8.75, '#ffffff', true);
    label('CỔNG RA · THU NGÂN', GATE_OUT_X + 1.6, 4.6, -4.4, { w: 384, h: 64, scale: [4, 0.7], bg: '#1d6a80', fg: '#fff', border: null });
    W.gateOut = { pivot, redMat, greenMat, monitorMat, angle: 1.35 };
  }

  [[-30, 15], [-26, 20], [40, 12], [14.5, 2.2], [-19, -17], [20, -18], [34, -14], [38, -3], [44, -16], [-36, -10], [-40, 14]].forEach(([x, z], i) => tree(scene, x, z, 0.9 + (i % 3) * 0.15));
  return W;
}

export function updateBayFx(bay, washing, t, dt) {
  const target = washing ? Math.sin(t * 1.1) * (bay.w > 5 ? 1.6 : 1.8) : 0;
  bay.gz += (target - bay.gz) * Math.min(1, dt * 4);
  bay.gantry.position.z = bay.gz;
  if (washing) {
    bay.rollers.forEach((r) => {
      if (r.vertical) r.pivot.rotation.y += dt * 9;
      else r.pivot.rotation.x += dt * 9;
    });
  }
  const sp = bay.spray;
  sp.points.visible = washing;
  if (!washing) return;
  for (let i = 0; i < sp.N; i++) {
    const k = i * 3;
    sp.pos[k + 1] += sp.vel[i] * dt;
    sp.vel[i] -= 9 * dt;
    if (sp.pos[k + 1] < 0.15) {
      sp.pos[k] = bay.x + (Math.random() - 0.5) * (bay.w - 0.8);
      sp.pos[k + 1] = 3.7;
      sp.pos[k + 2] = BAY_Z + bay.gz + (Math.random() - 0.5) * 0.7;
      sp.vel[i] = -(1 + Math.random() * 2);
    }
  }
  sp.geo.attributes.position.needsUpdate = true;
}
