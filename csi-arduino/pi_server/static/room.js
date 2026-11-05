// 3D room view: the room, the boards, the radio paths, and the people the
// Pi thinks are there. Polls /api/state 4x a second. Runs fully offline
// (three.js is served by the Pi from /static/vendor/three).
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/OrbitControls.js';
import { CSS2DRenderer, CSS2DObject } from 'three/addons/CSS2DRenderer.js';

const PERSON_COLORS = ['#3987e5', '#d95926', '#199e70'];   // categorical slots 1-3 (dark mode)
const HEAT_RGB = [144, 133, 233];                            // violet: "someone likely here"
const LINK_IDLE = new THREE.Color('#4b5566');
const LINK_HOT = new THREE.Color('#f2f4f8');
const POLL_MS = 250;

const $ = (id) => document.getElementById(id);
let room = null;
let W = 6, D = 6, H = 2.3;
// Room coordinates (x east, y north, z up, metres from the SW corner) -> three.js
const P = (x, y, z = 0) => new THREE.Vector3(x - W / 2, z, -(y - D / 2));

// --------------------------------------------------------------------------
//  Renderer, camera, controls
// --------------------------------------------------------------------------
const host = $('scene');
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.setSize(window.innerWidth, window.innerHeight);
host.appendChild(renderer.domElement);

const labels = new CSS2DRenderer();
labels.setSize(window.innerWidth, window.innerHeight);
labels.domElement.style.position = 'fixed';
labels.domElement.style.inset = '0';
labels.domElement.style.pointerEvents = 'none';
host.appendChild(labels.domElement);

const scene = new THREE.Scene();
scene.background = new THREE.Color('#0e1015');
const camera = new THREE.PerspectiveCamera(40, window.innerWidth / window.innerHeight, 0.1, 200);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.autoRotate = true;
controls.autoRotateSpeed = 0.45;          // ~2 minutes per turn: a slow spin
controls.maxPolarAngle = 1.42;
controls.minDistance = 3;
controls.maxDistance = 30;
let resumeSpinAt = 0;
let spinWanted = true;
controls.addEventListener('start', () => { controls.autoRotate = false; resumeSpinAt = Infinity; });
controls.addEventListener('end', () => { resumeSpinAt = performance.now() + 8000; });

scene.add(new THREE.HemisphereLight(0xbcc8ff, 0x15171c, 0.9));
const sun = new THREE.DirectionalLight(0xffffff, 1.1);
sun.position.set(4, 9, 6);
scene.add(sun);

// Start far enough back that the whole room fits, on a wide screen or a phone
function fitCamera() {
  const radius = Math.hypot(W, D) / 2 + 0.6;
  const vfov = THREE.MathUtils.degToRad(camera.fov);
  const hfov = 2 * Math.atan(Math.tan(vfov / 2) * camera.aspect);
  const dist = radius / Math.sin(Math.min(vfov, hfov) / 2) * 0.92;
  const dir = new THREE.Vector3(0.55, 0.62, 0.75).normalize();
  controls.target.set(0, 0.6, 0);
  camera.position.copy(dir.multiplyScalar(dist)).add(controls.target);
  controls.update();
}

window.addEventListener('resize', () => {
  camera.aspect = window.innerWidth / window.innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
  labels.setSize(window.innerWidth, window.innerHeight);
});

function tag(text, cls, color) {
  const el = document.createElement('div');
  el.className = cls;
  el.innerHTML = text;
  if (color) el.style.setProperty('--c', color);
  return new CSS2DObject(el);
}

// --------------------------------------------------------------------------
//  Static room: floor, zones, walls, boards
// --------------------------------------------------------------------------
const walls = [];
const nodeHeads = { tx: {}, rx: {} };
let heatTex = null, heatMesh = null;
const linkGroup = new THREE.Group();
scene.add(linkGroup);

function buildRoom() {
  W = room.width; D = room.depth; H = room.height || 2.3;
  $('room-name').textContent = room.name || 'Room';
  fitCamera();

  const floor = new THREE.Mesh(new THREE.PlaneGeometry(W, D),
    new THREE.MeshStandardMaterial({ color: '#171a21', roughness: 0.95 }));
  floor.rotation.x = -Math.PI / 2;
  scene.add(floor);

  // Zone grid + names
  const cols = room.zones?.cols || 3, rows = room.zones?.rows || 3;
  const pts = [];
  for (let c = 1; c < cols; c++) { const x = c * W / cols; pts.push(P(x, 0, 0.005), P(x, D, 0.005)); }
  for (let r = 1; r < rows; r++) { const y = r * D / rows; pts.push(P(0, y, 0.005), P(W, y, 0.005)); }
  scene.add(new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(pts),
    new THREE.LineBasicMaterial({ color: '#2f3746' })));
  for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) {
    const t = tag(String.fromCharCode(65 + c) + (r + 1), 'zone-tag');
    t.position.copy(P((c + 0.5) * W / cols, (r + 0.5) * D / rows, 0.01));
    scene.add(t);
  }

  // Heat map (floor glow)
  const nx = 64, ny = 64;
  heatTex = new THREE.DataTexture(new Uint8Array(nx * ny * 4), nx, ny, THREE.RGBAFormat);
  heatTex.magFilter = THREE.LinearFilter; heatTex.minFilter = THREE.LinearFilter;
  heatTex.needsUpdate = true;
  heatMesh = new THREE.Mesh(new THREE.PlaneGeometry(W, D),
    new THREE.MeshBasicMaterial({ map: heatTex, transparent: true, depthWrite: false,
                                  blending: THREE.AdditiveBlending }));
  heatMesh.rotation.x = -Math.PI / 2;
  heatMesh.position.y = 0.012;
  scene.add(heatMesh);

  // Walls: faint glass, fading out on the side facing the camera (doll-house view)
  const wallMat = () => new THREE.MeshStandardMaterial({ color: '#8fa3bf', transparent: true, opacity: 0.07,
                                                        side: THREE.DoubleSide, depthWrite: false });
  const defs = [
    { pos: P(W / 2, 0, H / 2), rot: 0, len: W, normal: new THREE.Vector3(0, 0, 1) },        // south
    { pos: P(W / 2, D, H / 2), rot: 0, len: W, normal: new THREE.Vector3(0, 0, -1) },       // north
    { pos: P(0, D / 2, H / 2), rot: Math.PI / 2, len: D, normal: new THREE.Vector3(-1, 0, 0) }, // west
    { pos: P(W, D / 2, H / 2), rot: Math.PI / 2, len: D, normal: new THREE.Vector3(1, 0, 0) },  // east
  ];
  for (const d of defs) {
    const m = new THREE.Mesh(new THREE.PlaneGeometry(d.len, H), wallMat());
    m.position.copy(d.pos); m.rotation.y = d.rot;
    const edges = new THREE.LineSegments(new THREE.EdgesGeometry(m.geometry),
      new THREE.LineBasicMaterial({ color: '#5b6b84', transparent: true, opacity: 0.6 }));
    m.add(edges);
    scene.add(m);
    walls.push({ mesh: m, edges, normal: d.normal, center: d.pos.clone() });
  }

  for (const f of room.furniture || []) {
    const box = new THREE.Mesh(new THREE.BoxGeometry(f.w, f.h, f.d),
      new THREE.MeshStandardMaterial({ color: '#39404d', roughness: 0.8, transparent: true, opacity: 0.85 }));
    box.position.copy(P(f.x, f.y, f.h / 2));
    scene.add(box);
    if (f.name) { const t = tag(f.name, 'node-tag'); t.position.copy(P(f.x, f.y, f.h + 0.1)); scene.add(t); }
  }

  const poleMat = new THREE.MeshStandardMaterial({ color: '#5a6475' });
  const addBoard = (kind, id, n) => {
    const z = n.z ?? 1.0;
    const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.012, 0.012, z, 8), poleMat);
    pole.position.copy(P(n.x, n.y, z / 2));
    scene.add(pole);
    const size = kind === 'rx' ? [0.17, 0.07, 0.17] : [0.11, 0.05, 0.11];
    const head = new THREE.Mesh(new THREE.BoxGeometry(...size),
      new THREE.MeshStandardMaterial({ color: '#dfe5ee', emissive: '#9aa6b8', emissiveIntensity: 0.5 }));
    head.position.copy(P(n.x, n.y, z));
    scene.add(head);
    if (kind === 'rx') {
      const ant = new THREE.Mesh(new THREE.CylinderGeometry(0.006, 0.006, 0.28, 6), poleMat);
      ant.position.copy(P(n.x, n.y, z + 0.17));
      scene.add(ant);
    }
    const t = tag(n.label || `${kind.toUpperCase()} ${id}`, 'node-tag');
    t.position.copy(P(n.x, n.y, z + (kind === 'rx' ? 0.42 : 0.16)));
    scene.add(t);
    nodeHeads[kind][id] = head.position.clone();
  };
  for (const [id, n] of Object.entries(room.receivers || {})) addBoard('rx', id, n);
  for (const [id, n] of Object.entries(room.transmitters || {})) addBoard('tx', id, n);
  if (room.access_point) {
    const a = room.access_point;
    const ap = new THREE.Mesh(new THREE.CylinderGeometry(0.11, 0.11, 0.03, 24),
      new THREE.MeshStandardMaterial({ color: '#f0efec', emissive: '#777', emissiveIntensity: 0.3 }));
    ap.position.copy(P(a.x, a.y, a.z ?? 2.1));
    scene.add(ap);
    const t = tag(a.label || 'AP', 'node-tag'); t.position.copy(P(a.x, a.y, (a.z ?? 2.1) + 0.15)); scene.add(t);
  }
}

// --------------------------------------------------------------------------
//  Radio paths (one per transmitter -> receiver), with travelling "packets"
// --------------------------------------------------------------------------
const linkObjs = {};
const UP = new THREE.Vector3(0, 1, 0);

function ensureLink(l) {
  if (linkObjs[l.key]) return linkObjs[l.key];
  const a = nodeHeads.tx[String(l.tx_node_id)], b = nodeHeads.rx[String(l.receiver_id)];
  if (!a || !b) return null;
  const len = a.distanceTo(b);
  const mat = new THREE.MeshBasicMaterial({ color: LINK_IDLE.clone(), transparent: true, opacity: 0.5,
                                            depthWrite: false });
  const tube = new THREE.Mesh(new THREE.CylinderGeometry(1, 1, len, 8, 1, true), mat);
  tube.position.copy(a).add(b).multiplyScalar(0.5);
  tube.quaternion.setFromUnitVectors(UP, b.clone().sub(a).normalize());
  linkGroup.add(tube);
  const dots = [];
  for (let i = 0; i < 3; i++) {
    const dot = new THREE.Mesh(new THREE.SphereGeometry(0.022, 10, 8),
      new THREE.MeshBasicMaterial({ color: '#ffffff', transparent: true, opacity: 0.7 }));
    linkGroup.add(dot);
    dots.push({ mesh: dot, phase: i / 3 });
  }
  return (linkObjs[l.key] = { a, b, len, tube, mat, dots, s: 0, target: 0, online: true });
}

// --------------------------------------------------------------------------
//  People
// --------------------------------------------------------------------------
const people = {};          // id -> {group, mats, label, target, alive, fade}
const ghosts = [];

function makePerson(id) {
  const color = PERSON_COLORS[(id - 1) % PERSON_COLORS.length];
  const g = new THREE.Group();
  const mat = new THREE.MeshStandardMaterial({ color, emissive: color, emissiveIntensity: 0.55,
                                               roughness: 0.45, transparent: true, opacity: 0 });
  const body = new THREE.Mesh(new THREE.CapsuleGeometry(0.17, 0.92, 6, 18), mat);
  body.position.y = 0.17 + 0.46 + 0.04;
  const head = new THREE.Mesh(new THREE.SphereGeometry(0.12, 20, 14), mat);
  head.position.y = 1.5;
  const ringMat = new THREE.MeshBasicMaterial({ color, transparent: true, opacity: 0, side: THREE.DoubleSide,
                                                depthWrite: false, blending: THREE.AdditiveBlending });
  const ring = new THREE.Mesh(new THREE.RingGeometry(0.27, 0.34, 48), ringMat);
  ring.rotation.x = -Math.PI / 2; ring.position.y = 0.02;
  const figure = new THREE.Group();
  figure.add(body, head);
  g.add(figure, ring);
  const label = tag('', 'tag', color);
  label.position.y = 1.85;
  g.add(label);
  scene.add(g);
  return { id, group: g, figure, mat, ringMat, ring, label, target: null, alive: true, fade: 0,
           moving: false, color };
}

function updatePeople(list) {
  const seen = new Set();
  for (const p of list) {
    seen.add(p.id);
    let o = people[p.id];
    if (!o) {
      o = people[p.id] = makePerson(p.id);
      o.group.position.copy(P(p.x, p.y));
    }
    o.alive = true;
    o.target = P(p.x, p.y);
    o.moving = p.moving;
    o.label.element.innerHTML = `Person ${p.id} <small>&middot; ${p.zone}${p.moving ? ' &middot; moving' : ''}</small>`;
  }
  for (const o of Object.values(people)) if (!seen.has(o.id)) o.alive = false;

  const rows = list.map((p) => {
    const c = PERSON_COLORS[(p.id - 1) % PERSON_COLORS.length];
    return `<div class="person-row"><span class="swatch" style="background:${c}"></span>
      Person ${p.id}<span class="meta">zone ${p.zone} &middot; ${p.moving ? 'moving' : 'still'}</span></div>`;
  });
  $('people').innerHTML = rows.join('');
}

function updateGhosts(truth, show) {
  while (ghosts.length < truth.length) {
    const m = new THREE.Mesh(new THREE.CylinderGeometry(0.2, 0.2, 1.7, 24, 1, true),
      new THREE.MeshBasicMaterial({ color: '#ffffff', transparent: true, opacity: 0.07,
                                    side: THREE.DoubleSide, depthWrite: false }));
    const rim = new THREE.LineLoop(new THREE.BufferGeometry().setFromPoints(
      Array.from({ length: 40 }, (_, i) => new THREE.Vector3(0.2 * Math.cos(i / 40 * 6.283), -0.84,
                                                              0.2 * Math.sin(i / 40 * 6.283)))),
      new THREE.LineBasicMaterial({ color: '#ffffff', transparent: true, opacity: 0.6 }));
    m.add(rim);
    const t = tag('true position', 'ghost-tag'); t.position.y = 1.0; m.add(t);
    scene.add(m); ghosts.push(m);
  }
  ghosts.forEach((g, i) => {
    g.visible = show && i < truth.length;
    if (i < truth.length) g.position.copy(P(truth[i].x, truth[i].y, 0.85));
  });
}

// --------------------------------------------------------------------------
//  Heat map
// --------------------------------------------------------------------------
function updateHeat(h) {
  if (!h || !h.data) return;
  const raw = atob(h.data);
  if (heatTex.image.width !== h.nx || heatTex.image.height !== h.ny) {
    heatTex.dispose();
    heatTex = new THREE.DataTexture(new Uint8Array(h.nx * h.ny * 4), h.nx, h.ny, THREE.RGBAFormat);
    heatTex.magFilter = THREE.LinearFilter; heatTex.minFilter = THREE.LinearFilter;
    heatMesh.material.map = heatTex;
    heatMesh.material.needsUpdate = true;
  }
  // Stretch to the strongest cell (but a weak, noisy map stays faint), then
  // let alpha carry the intensity: the colour itself stays full strength, so
  // the additive glow is clearly visible on the dark floor.
  const n = h.nx * h.ny;
  let peak = 0;
  for (let i = 0; i < n; i++) peak = Math.max(peak, raw.charCodeAt(i));
  const scale = 1 / Math.max(peak, 90);
  const px = heatTex.image.data;
  for (let i = 0; i < n; i++) {
    const a = Math.pow(raw.charCodeAt(i) * scale, 1.2);
    px[i * 4] = HEAT_RGB[0]; px[i * 4 + 1] = HEAT_RGB[1]; px[i * 4 + 2] = HEAT_RGB[2];
    px[i * 4 + 3] = Math.round(255 * 0.85 * a);
  }
  heatTex.needsUpdate = true;
}

// --------------------------------------------------------------------------
//  HUD
// --------------------------------------------------------------------------
function chip(id, cls, text) {
  const el = $(id);
  el.className = 'chip ' + cls;
  el.lastElementChild.textContent = text;
}

let bannerDismissed = false;
function txLabel(id) { return room?.transmitters?.[id]?.label || `TX ${id}`; }

function updateHud(s) {
  const n = s.people.length;
  $('count').textContent = n;
  $('count-word').textContent = n === 1 ? 'person' : 'people';
  chip('chip-mode', s.mode === 'model' ? 'good' : '', s.mode === 'model' ? 'AI model' : 'Path geometry');
  const cal = s.calibration || {};
  if (cal.state === 'running') chip('chip-cal', 'warn', `Calibrating ${Math.round((cal.progress || 0) * 100)}%`);
  else if (cal.state === 'ok' && cal.busy_links && cal.busy_links.length) chip('chip-cal', 'warn', 'Calibration looked busy');
  else if (cal.state === 'ok') chip('chip-cal', 'good', 'Calibrated');
  else if (cal.state === 'failed') chip('chip-cal', 'bad', 'Calibration failed');
  else chip('chip-cal', 'warn', 'Not calibrated');
  const total = s.links.length;
  chip('chip-data', s.links_online ? (s.links_online === total ? 'good' : 'warn') : 'bad',
       `${s.links_online}/${total || '?'} paths live`);
  $('activity').style.width = Math.round(100 * (s.activity || 0)) + '%';

  $('link-rows').innerHTML = s.links.map((l) => `
    <div class="link-row ${l.online ? '' : 'offline'}">
      <span class="name">${txLabel(String(l.tx_node_id))}</span>
      <div class="bar"><i style="width:${Math.round(100 * (l.disturbance || 0))}%"></i></div>
      <span class="val">${l.online ? l.rate + '/s' : 'offline'}</span>
    </div>`).join('') || '<div class="label">No paths yet</div>';
  $('model-note').textContent = s.mode === 'model' ? '' : 'AI model: ' + (s.model_status || '');

  // Banner: the one thing to fix first
  let title = '', text = '', action = '';
  if (!total || !s.links_online) {
    title = 'Waiting for WiFi data';
    text = 'No CSI is arriving. Is the ESP32-S3 powered and connected to linksys222, and are the transmitters on?';
  } else if (s.layout_warning && s.layout_warning.length) {
    title = 'CSI arrives in an unexpected format';
    text = `On ${s.layout_warning.join(', ')} the empty "guard" subcarriers aren't empty, so the bytes ` +
           'are not in the order this software assumes. Positions and the AI model will be wrong. ' +
           'Check the S3 runs csi_receiver_s3 from this folder (unchanged CSI settings), and see the server log.';
  } else if (cal.state === 'none' || cal.state === 'failed') {
    title = 'Calibrate the empty room';
    text = 'Everyone leaves the room for 20 seconds so the Pi learns what "empty" looks like. ' +
           'Until then only movement is detected, not people standing still.';
    action = '<button class="primary" id="banner-cal">Everyone is out - calibrate</button>';
  } else if (cal.state === 'ok' && cal.busy_links && cal.busy_links.length && !bannerDismissed) {
    title = 'Was the room empty during calibration?';
    text = 'Some paths kept changing while the empty room was recorded. If anyone was inside, ' +
           'clear the room and calibrate again - everything is measured against this reference.';
    action = '<button class="primary" id="banner-cal">Everyone is out - calibrate again</button> ' +
             '<button id="banner-ok">It was empty</button>';
  } else if (cal.state === 'running' && cal.message && !cal.message.includes('guided')) {
    title = 'Recording the empty room...';
    text = `Stay out of the room. ${Math.round((cal.progress || 0) * 100)}%`;
  }
  $('banner').style.display = title ? 'block' : 'none';
  if (title && $('banner-title').textContent !== title) {
    $('banner-title').textContent = title;
    $('banner-text').textContent = text;
    $('banner-action').innerHTML = action;
    const b = $('banner-cal'); if (b) b.onclick = () => { bannerDismissed = false; calibrate(false); };
    const ok = $('banner-ok'); if (ok) ok.onclick = () => { bannerDismissed = true; $('banner').style.display = 'none'; };
  } else if (title) {
    $('banner-text').textContent = text;
  }

  const sim = s.sim;
  $('t-ghost').style.display = sim ? '' : 'none';
  $('simbox').style.display = sim ? 'block' : 'none';
  if (sim) {
    const pct = (v) => (v == null ? '-' : Math.round(v * 100) + '%');
    $('sim-text').innerHTML = `Average position error <b>${sim.error_m ?? '-'} m</b><br>` +
      `People count right ${pct(sim.count_accuracy)} &middot; zone right ${pct(sim.zone_accuracy)}<br>` +
      `<span style="color:var(--text-muted)">(last minute)</span>`;
  }
}

async function calibrate(ask = true) {
  if (ask && !confirm('Everyone must be OUT of the room for the next 20 seconds. Start?')) return;
  await fetch('/api/calibrate', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                  body: JSON.stringify({ seconds: 20 }) });
}
$('btn-cal').onclick = () => calibrate(true);

// Toggles
const toggles = { spin: true, heat: true, links: true, labels: true, ghost: true };
for (const name of Object.keys(toggles)) {
  $('t-' + name).onclick = (e) => {
    toggles[name] = !toggles[name];
    e.currentTarget.classList.toggle('on', toggles[name]);
    if (name === 'spin') { spinWanted = toggles.spin; controls.autoRotate = spinWanted; }
    if (name === 'heat') heatMesh.visible = toggles.heat;
    if (name === 'links') linkGroup.visible = toggles.links;
    if (name === 'labels') document.body.classList.toggle('hide-labels', !toggles.labels);
  };
}

// --------------------------------------------------------------------------
//  Polling + animation
// --------------------------------------------------------------------------
let lastState = null;
async function poll() {
  try {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), 2000);
    const s = await (await fetch('/api/state', { signal: ctl.signal })).json();
    clearTimeout(timer);
    lastState = s;
    for (const l of s.links) {
      const o = ensureLink(l);
      if (o) { o.target = l.online ? l.disturbance : 0; o.online = l.online; }
    }
    updatePeople(s.people);
    updateHeat(s.heat);
    updateGhosts(s.sim ? s.sim.truth : [], toggles.ghost);
    updateHud(s);
  } catch (e) {
    $('banner').style.display = 'block';
    $('banner-title').textContent = 'Lost contact with the Pi';
    $('banner-text').textContent = 'Retrying... (is server.py running?)';
    $('banner-action').innerHTML = '';
  }
  setTimeout(poll, POLL_MS);
}

let lastFrame = performance.now();
function animate() {
  const nowMs = performance.now();
  const dt = Math.min((nowMs - lastFrame) / 1000, 0.1);
  lastFrame = nowMs;
  const t = nowMs / 1000;
  if (spinWanted && !controls.autoRotate && performance.now() > resumeSpinAt) controls.autoRotate = true;
  controls.update();

  // Doll-house walls: fade the ones between the camera and the room
  for (const w of walls) {
    const toCam = camera.position.clone().sub(w.center);
    const front = toCam.dot(w.normal) > 0;     // normals point out of the room
    w.mesh.material.opacity += ((front ? 0.015 : 0.08) - w.mesh.material.opacity) * 0.1;
    w.edges.material.opacity += ((front ? 0.15 : 0.6) - w.edges.material.opacity) * 0.1;
  }

  for (const o of Object.values(linkObjs)) {
    o.s += (o.target - o.s) * Math.min(1, dt * 6);
    const r = 0.006 + 0.022 * o.s;
    o.tube.scale.set(r, 1, r);
    o.mat.color.copy(LINK_IDLE).lerp(LINK_HOT, o.s);
    o.mat.opacity = o.online ? 0.35 + 0.6 * o.s : 0.12;
    for (const d of o.dots) {
      d.phase = (d.phase + dt * (0.35 + 0.5 * o.s)) % 1;
      d.mesh.position.copy(o.a).lerp(o.b, d.phase);
      d.mesh.visible = o.online;
      d.mesh.material.opacity = 0.35 + 0.6 * o.s;
    }
  }

  for (const o of Object.values(people)) {
    o.fade += ((o.alive ? 1 : 0) - o.fade) * Math.min(1, dt * (o.alive ? 3 : 6));
    if (o.target) o.group.position.lerp(o.target, 1 - Math.exp(-dt * 3.5));
    o.mat.opacity = 0.92 * o.fade;
    o.ringMat.opacity = 0.75 * o.fade;
    const pulse = 1 + 0.12 * Math.sin(t * 3 + o.id);
    o.ring.scale.set(pulse, pulse, pulse);
    o.figure.position.y = o.moving ? 0.035 * Math.abs(Math.sin(t * 7 + o.id)) : 0;
    o.figure.rotation.z = o.moving ? 0.05 * Math.sin(t * 7 + o.id) : 0;
    o.label.element.style.opacity = o.fade;
    if (!o.alive && o.fade < 0.02) {
      scene.remove(o.group);
      o.label.element.remove();
      delete people[o.id];
    }
  }

  renderer.render(scene, camera);
  labels.render(scene, camera);
  requestAnimationFrame(animate);
}

async function start() {
  for (;;) {
    try { room = await (await fetch('/api/room')).json(); break; }
    catch (e) { await new Promise((r) => setTimeout(r, 2000)); }
  }
  buildRoom();
  animate();
  poll();
}
start();
