// Helper dựng khối low-poly + mô hình xe + nhãn chữ (sprite).
import * as THREE from 'three';

export const mat = (color, o = {}) =>
  new THREE.MeshStandardMaterial({ color, metalness: o.metal ?? 0, roughness: o.rough ?? 0.7, ...(o.extra || {}) });

export const glow = (color, k = 0.8) => mat(color, { extra: { emissive: new THREE.Color(color), emissiveIntensity: k } });

export function box(g, w, h, d, x, y, z, m, noShadow = false) {
  const o = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), typeof m === 'string' ? mat(m) : m);
  o.position.set(x, y, z);
  o.castShadow = !noShadow;
  o.receiveShadow = true;
  g.add(o);
  return o;
}

export function cyl(g, r, h, x, y, z, m, { rx = 0, rz = 0, seg = 20 } = {}) {
  const o = new THREE.Mesh(new THREE.CylinderGeometry(r, r, h, seg), typeof m === 'string' ? mat(m) : m);
  o.position.set(x, y, z);
  o.rotation.set(rx, 0, rz);
  o.castShadow = true;
  o.receiveShadow = true;
  g.add(o);
  return o;
}

export function sphere(g, r, x, y, z, m) {
  const o = new THREE.Mesh(new THREE.SphereGeometry(r, 16, 12), typeof m === 'string' ? mat(m) : m);
  o.position.set(x, y, z);
  o.castShadow = true;
  g.add(o);
  return o;
}

// Nhãn chữ luôn hướng về camera.
export function textSprite(text, { w = 256, h = 64, scale = [2.6, 0.65], bg = '#ffffff', fg = '#16232a', border = '#16232a', font = 'bold 34px "Segoe UI", Arial' } = {}) {
  const c = document.createElement('canvas');
  c.width = w;
  c.height = h;
  const x = c.getContext('2d');
  x.fillStyle = bg;
  x.beginPath();
  x.roundRect(3, 3, w - 6, h - 6, 12);
  x.fill();
  if (border) {
    x.lineWidth = 4;
    x.strokeStyle = border;
    x.stroke();
  }
  x.fillStyle = fg;
  x.font = font;
  x.textAlign = 'center';
  x.textBaseline = 'middle';
  x.fillText(text, w / 2, h / 2 + 2);
  const tex = new THREE.CanvasTexture(c);
  tex.colorSpace = THREE.SRGBColorSpace;
  const s = new THREE.Sprite(new THREE.SpriteMaterial({ map: tex, depthTest: false, transparent: true }));
  s.scale.set(scale[0], scale[1], 1);
  s.renderOrder = 10;
  return s;
}

export const PALETTE = ['#e04b4b', '#3d7be0', '#f0b840', '#2fb36a', '#8a63d2', '#e8edf0', '#ff8a3d', '#2b3a42'];

// Bùn đất: lớp phủ nâu lên sơn + kính, và các mảng bùn nhỏ dán lên thân xe. amount 0 (sạch) -> 1 (rất bẩn).
const DIRT_PAINT = new THREE.Color('#6b5035');
const DIRT_GLASS = new THREE.Color('#7c7260');
const mulberry = (seed) => {
  let a = (seed * 2654435761) >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
};

// Mô hình xe (ô tô, xe tải): đầu xe hướng về -Z.
// Trả về { group, h, setDirt }: h = chiều cao để đặt nhãn; setDirt(amount 0..1) đổi độ bẩn nhìn thấy được.
export function makeVehicle(type, color, seed = 1) {
  const g = new THREE.Group();
  const rnd = mulberry(seed + 7);
  const paints = []; // { mat, base, dirt, k }
  const paint = (m, dirtColor, k) => {
    paints.push({ mat: m, base: m.color.clone(), dirt: dirtColor, k });
    return m;
  };
  const body = paint(mat(color, { rough: 0.45, metal: 0.2 }), DIRT_PAINT, 0.85);
  const glass = paint(mat('#9ec9d8', { rough: 0.2, metal: 0.5 }), DIRT_GLASS, 0.75);
  const dark = mat('#23282b');
  const lamp = glow('#ffe9a8', 0.7);
  const tail = glow('#e03030', 0.6);
  const wheel = (x, y, z, r, w) => cyl(g, r, w, x, y, z, dark, { rz: Math.PI / 2 });

  // mảng bùn: hộp mỏng nằm sát bề mặt; hiện dần theo ngưỡng thr khi amount tăng
  const grime = mat('#3f2d1d', { rough: 1 });
  const patches = [];
  const patch = (x, y, z, sx, sy, sz) => {
    const o = new THREE.Mesh(new THREE.BoxGeometry(sx, sy, sz), grime);
    o.position.set(x, y, z);
    o.receiveShadow = true;
    o.visible = false;
    g.add(o);
    patches.push({ mesh: o, thr: 0.08 + rnd() * 0.7 });
  };
  const R = (a, b) => a + rnd() * (b - a);
  // mảng trên mặt bên (trục X cố định), lệch nhiều xuống thấp như bùn bắn lên từ bánh
  const sides = (x, n, y0, y1, z0, z1, maxDz) => {
    for (const sg of [1, -1]) {
      for (let i = 0; i < n; i++) patch(sg * x, y0 + Math.pow(rnd(), 1.4) * (y1 - y0), R(z0, z1), 0.04, R(0.12, 0.4), R(0.25, maxDz));
    }
  };
  // mảng trên mặt phẳng nằm ngang (y cố định)
  const tops = (y, n, x1, z0, z1) => {
    for (let i = 0; i < n; i++) patch(R(-x1, x1), y, R(z0, z1), R(0.3, 0.8), 0.04, R(0.25, 0.7));
  };

  let h = 1.8;
  if (type === 'CAR') {
    box(g, 1.9, 0.7, 4.2, 0, 0.65, 0, body);
    box(g, 1.7, 0.55, 2.2, 0, 1.25, 0.25, body);
    box(g, 1.74, 0.32, 2.0, 0, 1.3, 0.25, glass);
    box(g, 1.6, 0.1, 1.8, 0, 1.58, 0.25, body);
    box(g, 0.4, 0.18, 0.08, 0.6, 0.72, -2.12, lamp, true);
    box(g, 0.4, 0.18, 0.08, -0.6, 0.72, -2.12, lamp, true);
    box(g, 0.4, 0.16, 0.08, 0.6, 0.72, 2.12, tail, true);
    box(g, 0.4, 0.16, 0.08, -0.6, 0.72, 2.12, tail, true);
    [[0.98, -1.35], [-0.98, -1.35], [0.98, 1.35], [-0.98, 1.35]].forEach(([x, z]) => wheel(x, 0.36, z, 0.36, 0.28));
    h = 1.8;
    sides(0.96, 11, 0.32, 0.95, -2.0, 2.0, 0.9); // thân dưới
    sides(0.86, 4, 1.05, 1.5, -0.8, 1.3, 0.6); // cabin
    tops(1.02, 5, 0.8, -2.0, -0.9); // nắp capo
    tops(1.02, 3, 0.8, 1.4, 2.0); // cốp
    tops(1.64, 4, 0.7, -0.5, 1.0); // nóc
    for (const z of [-2.12, 2.12]) for (let i = 0; i < 3; i++) patch(R(-0.8, 0.8), R(0.35, 0.65), z, R(0.25, 0.55), R(0.12, 0.3), 0.04); // đầu / đuôi xe
  } else if (type === 'TRUCK') {
    const cargo = paint(mat('#e8edf0'), DIRT_PAINT, 0.85);
    box(g, 2.3, 0.4, 7, 0, 0.75, 0, dark);
    box(g, 2.4, 1.9, 2, 0, 1.75, -2.5, body);
    box(g, 2.2, 0.7, 0.1, 0, 2.1, -3.52, glass);
    box(g, 2.64, 2.5, 4.8, 0, 2.3, 1.5, cargo);
    box(g, 0.5, 0.2, 0.08, 0.8, 0.95, -3.52, lamp, true);
    box(g, 0.5, 0.2, 0.08, -0.8, 0.95, -3.52, lamp, true);
    [[-2.5], [1.0], [2.6]].forEach(([z]) => {
      wheel(1.2, 0.5, z, 0.5, 0.4);
      wheel(-1.2, 0.5, z, 0.5, 0.4);
    });
    h = 3.7;
    sides(1.21, 7, 0.85, 2.0, -3.4, -1.6, 1.0); // cabin
    sides(1.33, 12, 1.1, 2.3, -0.8, 3.8, 1.2); // thùng hàng
    tops(3.56, 6, 1.2, -0.7, 3.7); // nóc thùng
    for (let i = 0; i < 5; i++) patch(R(-1.05, 1.05), R(0.85, 1.7), -3.52, R(0.3, 0.7), R(0.15, 0.4), 0.04); // đầu cabin
    for (let i = 0; i < 4; i++) patch(R(-1.15, 1.15), R(1.2, 2.2), 3.91, R(0.3, 0.8), R(0.2, 0.5), 0.04); // đuôi thùng
  }

  let last = -1;
  function setDirt(amount) {
    const a = Math.max(0, Math.min(1, amount));
    if (Math.abs(a - last) < 0.004) return;
    last = a;
    for (const p of paints) p.mat.color.copy(p.base).lerp(p.dirt, a * p.k);
    for (const p of patches) {
      const s = Math.max(0, Math.min(1, (a - p.thr) / 0.15));
      p.mesh.visible = s > 0.02;
      p.mesh.scale.setScalar(Math.max(s, 0.001));
    }
  }
  return { group: g, h, setDirt };
}
