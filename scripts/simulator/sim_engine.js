
/* ================================================================
   GalvOS Simulator Layer
   Intercepts /api/* fetch calls and drives the sim canvas.

   Wrapped in an IIFE: the WebUI bundle below is data/index.html
   verbatim and declares its own globals (OPT_PROFILES, PRESETS,
   state, ...). Sharing the global scope with it means one added
   WebUI variable can collide with a sim variable of the same name
   and take the whole page down with a redeclaration SyntaxError --
   which is exactly what happened when the sim was last re-synced.
   Everything the outside world needs is published on
   window.GALVOS_SIM / window.DEMO_LOG_ENTRIES instead.
   ================================================================ */
(function() {
'use strict';


"use strict";

/* ══════════════════════════════════════════════════════════════════════
   Preset engine — JS port of src/patterns/preset_patterns.cpp
   ══════════════════════════════════════════════════════════════════════ */

// Simulator build identity. FW_VERSION / FW_COMMIT track the firmware
// revision this port was generated from -- bump both together whenever the
// preset generators are re-synced with src/patterns/preset_patterns.cpp.
// SIM_UI_VERSION mirrors data/index.html's own UI_VERSION constant, which the
// WebUI bundle below is copied from verbatim.
const SIM_VERSION    = '1.2.0';
const FW_VERSION     = '6.80.0';
const FW_COMMIT      = '5d79b9a';
const SIM_UI_VERSION = '1.25.0';

// Reference animation frame period (pattern_engine.cpp's kAnimPhaseFrameMs).
// The firmware advances the integer preset phase by wall-clock time / 40ms,
// so a browser running the sim loop at 60fps must do the same instead of
// advancing once per rAF -- otherwise every preset animates ~2.4x too fast.
const ANIM_FRAME_MS = 40.0;

const TAU = Math.PI * 2;
const PI = Math.PI;
const SC = 18000;

const ssc = s => 0.25 + (s / 255) * 1.1765;
const aang = (ph, sp, m = 1) => (sp ? mod(ph * (sp / 5000) * m, TAU) : 0);
const contDelta = sp => (sp / 255) * 0.05;
const L = (a, b, t) => a + (b - a) * t;

function mod(a, b) { return a - Math.floor(a / b) * b; }
function csweep(ph, sp, i, N) {
  const span = TAU + contDelta(sp);
  const base = mod(ph * span, TAU);
  return base + span * i / N;
}
function adaptN(sz, base, min = 8, max = 512) {
  let n = Math.round(base * (0.4 + (sz / 255) * 1.2));
  return Math.max(min, Math.min(max, n));
}
function sHash(s) {
  const x = Math.sin(s * 127.1 + 1) * 43758.5453;
  return x - Math.floor(x);
}
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const b255 = v => clamp(Math.round(v), 0, 255);

/* ── Frame builder ─────────────────────────────────────────────────── */

function V(x, y, r = 255, g = 255, b = 255) { return { x, y, r, g, b }; }

class Frame {
  constructor() { this.segs = []; this.dots = []; }
  seg(verts, closed = false) { if (verts && verts.length > 1) this.segs.push({ v: verts, closed }); }
  line(x0, y0, x1, y1, r, g, b) { this.seg([V(x0, y0, r, g, b), V(x1, y1, r, g, b)], false); }
  dot(x, y, r, g, b) { this.dots.push({ x, y, r, g, b }); }
}

/* ── Geometry helpers ──────────────────────────────────────────────── */

function curve(N, fn) {
  const v = new Array(N);
  for (let i = 0; i < N; i++) v[i] = fn(i, N);
  return v;
}
function ngonVerts(sides, sc, off, r, g, b) {
  return curve(sides, i => {
    const a = TAU * i / sides + off;
    return V(Math.cos(a) * sc, Math.sin(a) * sc, r, g, b);
  });
}
function starVerts(pts, outer, inner, off, r, g, b) {
  const n = pts * 2;
  return curve(n, i => {
    const a = TAU * i / n + off - PI / 2;
    const rad = (i % 2 === 0) ? outer : inner;
    return V(Math.cos(a) * rad, Math.sin(a) * rad, r, g, b);
  });
}
function prj(v, ry, rx, sc) {
  const x1 = v.x * Math.cos(ry) + v.z * Math.sin(ry);
  const z1 = -v.x * Math.sin(ry) + v.z * Math.cos(ry);
  const y2 = v.y * Math.cos(rx) - z1 * Math.sin(rx);
  return { x: x1 * sc, y: y2 * sc };
}
// Greedy edge-chain walker — mirrors buildWfChains() in the firmware.
function buildChains(nv, E) {
  const used = new Array(E.length).fill(false);
  const adj = Array.from({ length: nv }, () => []);
  E.forEach(([a, b], e) => { adj[a].push([e, b]); adj[b].push([e, a]); });
  const chains = [];
  for (let e0 = 0; e0 < E.length; e0++) {
    if (used[e0]) continue;
    used[e0] = true;
    const start = E[e0][0];
    let cur = E[e0][1];
    const chain = [start, cur];
    let closed = false;
    while (chain.length <= nv + 1) {
      let next = null;
      for (const [e, other] of adj[cur]) if (!used[e]) { next = [e, other]; break; }
      if (!next) break;
      if (next[1] === start) { used[next[0]] = true; closed = true; break; }
      used[next[0]] = true;
      chain.push(next[1]);
      cur = next[1];
    }
    chains.push({ chain, closed });
  }
  return chains;
}
function wf(f, verts3d, edges, ry, rx, sc, r, g, b) {
  for (const { chain, closed } of buildChains(verts3d.length, edges)) {
    const vs = chain.map(idx => {
      const p = prj(verts3d[idx], ry, rx, sc);
      return V(p.x, p.y, r, g, b);
    });
    f.seg(vs, closed);
  }
}
function sinewave(f, A, freq, phOff, sc, r, g, b, N = 120) {
  const effA = A * live.waveAmp, effF = freq * live.waveFreq;
  f.seg(curve(N + 1, i => {
    const x = L(-1, 1, i / N);
    return V(x * sc, effA * Math.sin(effF * x * TAU + phOff) * sc, r, g, b);
  }), false);
}
const CV = [{ x: -1, y: -1, z: -1 }, { x: 1, y: -1, z: -1 }, { x: 1, y: 1, z: -1 }, { x: -1, y: 1, z: -1 },
            { x: -1, y: -1, z: 1 }, { x: 1, y: -1, z: 1 }, { x: 1, y: 1, z: 1 }, { x: -1, y: 1, z: 1 }];
const CE = [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]];

/* ── Live preset state (mirrors gLivePreset) ───────────────────────── */

const live = {
  waveAmp: 1.0, waveFreq: 1.0,
  spiralArms: 2, tunnelRings: 6, tunnelSides: 4,
  explosionRays: 16, fwShells: 2, fwGlitter: true,
  mwDots: 60, mwTilt: 60,
  trail: 6, randomHoldMs: 500,
  countdownSec: 90, starfieldCount: 0,
  hlineBounce: false,
  // Per-side/spoke colors for the Geometry polygons & stars (fw v6.59.0,
  // gLivePreset.seg_colors_enabled / seg_col_r|g|b[10]).
  segColors: false,
  segR: [255, 0, 0, 255, 0, 255, 255, 255, 128, 0],
  segG: [0, 255, 0, 255, 255, 0, 128, 255, 0, 128],
  segB: [0, 0, 255, 0, 255, 255, 0, 128, 255, 255]
};

// Per-side color for edge s, cycling the 10 slots like ngonSegmented() /
// starSegmented() do. Returns the flat preset color when disabled.
function segCol(s, r, g, b) {
  if (!live.segColors) return [r, g, b];
  const i = ((s % 10) + 10) % 10;
  return [live.segR[i], live.segG[i], live.segB[i]];
}

/* ── Preset table (order == firmware PRESETS[]) ────────────────────── */

const PRESETS = [
  ["Circle", "Geometry"], ["Square", "Geometry"], ["Triangle", "Geometry"], ["Pentagon", "Geometry"],
  ["Hexagon", "Geometry"], ["Octagon", "Geometry"], ["Star 4", "Geometry"], ["Star 5", "Geometry"],
  ["Star 6", "Geometry"], ["Star 8", "Geometry"],
  ["Cross +", "Lines"], ["X Shape", "Lines"], ["Grid 3x3", "Lines"], ["H Line", "Lines"], ["Diagonal", "Lines"],
  ["Archimedean Spiral", "Spirals"], ["Lissajous 1:2", "Spirals"], ["Lissajous 2:3", "Spirals"],
  ["Lissajous 3:4", "Spirals"], ["Lissajous 3:5", "Spirals"], ["Lissajous 5:6", "Spirals"],
  ["Double Spiral", "Spirals"], ["Rose 3", "Spirals"],
  ["Rose 4", "Curves"], ["Heart", "Curves"], ["Infinity", "Curves"], ["Astroid", "Curves"], ["Epitrochoid", "Curves"],
  ["Rotating Cube", "3D"], ["Static Cube", "3D"], ["Pyramid", "3D"], ["Octahedron", "3D"], ["Tetrahedron", "3D"],
  ["Sine Wave", "Waves"], ["Standing Wave", "Waves"], ["Multi Wave", "Waves"], ["Ocean Wave", "Waves"],
  ["Wave Interference", "Waves"], ["Sawtooth", "Waves"], ["Square Wave", "Waves"], ["Wave Packet", "Waves"],
  ["Beat Wave", "Waves"], ["Radial Waves", "Waves"], ["FM Wave", "Waves"], ["Vortex", "Waves"],
  ["Sine Helix", "Waves"], ["Wave Field", "Waves"], ["Fourier Square", "Waves"], ["Gravity Waves", "Waves"],
  ["Tsunami", "Waves"], ["Wave Spectrum", "Waves"],
  ["Hypotrochoid", "Complex"], ["Butterfly", "Complex"], ["Spirograph 5/3", "Complex"],
  ["Concentric Rings", "Complex"], ["Nested Squares", "Complex"], ["Pulsing Circle", "Complex"],
  ["Starburst", "Combo"], ["Chaos Bouncer", "Combo"], ["Laser Diamond", "Combo"],
  ["Confetti Burst", "Combo"], ["Disco Ball", "Combo"],
  ["Hibiscus", "Party"], ["Starburst Party", "Party"],
  ["Starfield", "Scenes"],
  ["Countdown Timer", "Timers"],
  ["Pentagram", "Geometry"], ["DNA Helix", "Complex"], ["Yin Yang", "Symbols"],
  ["Random Points", "Scenes"],
  ["Three Circles", "Geometry"], ["Point Spread", "Scenes"],
  ["Solar System", "Scenes"],
  ["Bouncing Points", "Scenes"],
  ["Shooting Stars", "Scenes"],
  ["Pythagoras Tree", "Scenes"],
  ["Endless Spiral", "Scenes"], ["Endless Tunnel", "Scenes"], ["Explosion Spread", "Scenes"],
  ["Fireworks", "Scenes"], ["Milky Way", "Scenes"]
];


/* ══════════════════════════════════════════════════════════════════════
   Generators — one function per preset index
   ══════════════════════════════════════════════════════════════════════ */

const state = {};                       // persistent per-preset state
function resetState() { for (const k in state) delete state[k]; }
const nowMs = () => performance.now();

// ngonSegmented()/starSegmented() equivalent: with Per-Side Colors on, every
// edge becomes its own open segment so each one can carry a different color
// (fw v6.59.0). Off, it stays a single closed path, exactly as before.
function genNgon(sides, colR, colG, colB) {
  return (f, ph, sp, sz) => {
    const sc = SC * ssc(sz) * 0.9, off = aang(ph, sp);
    if (!live.segColors) { f.seg(ngonVerts(sides, sc, off, colR, colG, colB), true); return; }
    for (let s = 0; s < sides; s++) {
      const a0 = TAU * s / sides + off, a1 = TAU * ((s + 1) % sides) / sides + off;
      const c = segCol(s, colR, colG, colB);
      f.line(Math.cos(a0) * sc, Math.sin(a0) * sc, Math.cos(a1) * sc, Math.sin(a1) * sc, c[0], c[1], c[2]);
    }
  };
}
function genStar(pts, colR, colG, colB) {
  return (f, ph, sp, sz) => {
    const s = SC * ssc(sz) * 0.9, inner = s * 0.36, off = aang(ph, sp);
    if (!live.segColors) { f.seg(starVerts(pts, s, inner, off, colR, colG, colB), true); return; }
    const nv = pts * 2;
    for (let k = 0; k < nv; k++) {
      const nk = (k + 1) % nv;
      const a0 = TAU * k / nv + off - PI / 2, a1 = TAU * nk / nv + off - PI / 2;
      const r0 = (k % 2 === 0) ? s : inner, r1 = (nk % 2 === 0) ? s : inner;
      const c = segCol(k, colR, colG, colB);
      f.line(Math.cos(a0) * r0, Math.sin(a0) * r0, Math.cos(a1) * r1, Math.sin(a1) * r1, c[0], c[1], c[2]);
    }
  };
}
function genLissajous(N, fx, fy, phy, colR, colG, colB) {
  return (f, ph, sp, sz) => {
    const sc = SC * ssc(sz) * 0.9, off = aang(ph, sp);
    f.seg(curve(N, (i, n) => {
      const t = TAU * i / n;
      return V(Math.cos(fx * t + off) * sc, Math.sin(fy * t + phy) * sc, colR, colG, colB);
    }), true);
  };
}
// pa(m): phase for a harmonic running at m times the base rate. Must come
// from its own aang() call (multiplier applied BEFORE the internal wrap) --
// scaling the already-wrapped t afterwards by a non-integer factor jumps the
// fed phase by ~PI at every wrap, i.e. a visible snap once per loop
// (fw v6.67.0, bugs01.md P2 #6). yFn receives it as its 5th argument.
function genWave(colR, colG, colB, yFn, N = 120) {
  return (f, ph, sp, sz) => {
    const sc = SC * ssc(sz) * 0.9, t = aang(ph, sp);
    const pa = m => aang(ph, sp, m);
    const wa = live.waveAmp, wf_ = live.waveFreq;
    f.seg(curve(N + 1, (i, n) => {
      const xx = L(-1, 1, i / (n - 1));
      return V(xx * sc, yFn(xx, t, wa, wf_, pa) * sc, colR, colG, colB);
    }), false);
  };
}

const GEN = [];

/* ── Geometry 0-9 ──────────────────────────────────────────────────── */
GEN[0] = (f, ph, sp, sz) => {                                   // Circle
  const sc = SC * ssc(sz) * 0.9, N = adaptN(sz, 360, 60, 900);
  f.seg(curve(N, (i, n) => {
    const t = csweep(ph, sp, i, n);
    return V(Math.cos(t) * sc, Math.sin(t) * sc, 255, 255, 255);
  }), false);
};
GEN[1] = genNgon(4, 255, 255, 0);
GEN[2] = genNgon(3, 0, 255, 255);
GEN[3] = genNgon(5, 255, 255, 0);
GEN[4] = genNgon(6, 0, 255, 0);
GEN[5] = genNgon(8, 0, 0, 255);
GEN[6] = genStar(4, 255, 0, 0);
GEN[7] = genStar(5, 255, 255, 0);
GEN[8] = genStar(6, 0, 255, 0);
GEN[9] = genStar(8, 0, 255, 255);

/* ── Lines 10-14 ───────────────────────────────────────────────────── */
GEN[10] = (f, ph, sp, sz) => {                                  // Cross +
  const s = SC * ssc(sz) * 0.9;
  f.line(-s, 0, s, 0, 255, 0, 0);
  f.line(0, -s, 0, s, 0, 255, 0);
};
GEN[11] = (f, ph, sp, sz) => {                                  // X Shape
  const s = SC * ssc(sz) * 0.65;
  f.line(-s, -s, s, s, 0, 255, 255);
  f.line(s, -s, -s, s, 255, 0, 255);
};
GEN[12] = (f, ph, sp, sz) => {                                  // Grid 3x3
  const s = SC * ssc(sz) * 0.9, st = s * 2 / 3;
  for (let i = 0; i <= 3; i++) { const x = -s + i * st; f.line(x, -s, x, s, 0, 255, 255); }
  for (let i = 0; i <= 3; i++) { const y = -s + i * st; f.line(-s, y, s, y, 0, 255, 255); }
};
GEN[13] = (f, ph, sp, sz) => {                                  // H Line
  // Default: sawtooth ramp bottom->top, snapping back every wrap. Bounce
  // (gLivePreset.hline_bounce, fw v6.67.0): fold the ramp into a 0..1..0
  // triangle so the line eases back down instead of snapping.
  const s = SC * ssc(sz) * 0.9, r = aang(ph, sp) / TAU;
  const tri = r < 0.5 ? r * 2 : 2 - r * 2;
  const off = ((live.hlineBounce ? tri : r) * 2 - 1) * s;
  f.line(-s, off, s, off, 255, 255, 0);
};
GEN[14] = (f, ph, sp, sz) => {                                  // Diagonal
  const s = SC * ssc(sz) * 0.9, a = aang(ph, sp);
  const ca = Math.cos(a), sa = Math.sin(a);
  f.line(-s * ca, -s * sa, s * ca, s * sa, 255, 0, 255);
};

/* ── Spirals 15-22 ─────────────────────────────────────────────────── */
GEN[15] = (f, ph, sp, sz) => {                                  // Archimedean Spiral
  const sc = SC * ssc(sz) * 0.9, off = aang(ph, sp), N = adaptN(sz, 200, 30, 400);
  f.seg(curve(N, (i, n) => {
    const t = i / n, a = t * TAU * 3.5 + off, rr = t * sc;
    return V(Math.cos(a) * rr, Math.sin(a) * rr, b255(t * 255), b255((1 - t) * 255), 128);
  }), false);
};
GEN[16] = genLissajous(112, 1, 2, PI / 4, 0, 255, 255);
GEN[17] = genLissajous(176, 2, 3, PI / 4, 0, 255, 255);
GEN[18] = genLissajous(240, 3, 4, PI / 3, 0, 255, 255);
GEN[19] = genLissajous(288, 3, 5, PI / 6, 0, 0, 255);
GEN[20] = genLissajous(352, 5, 6, TAU / 5, 255, 255, 0);
GEN[21] = (f, ph, sp, sz) => {                                  // Double Spiral
  const sc = SC * ssc(sz) * 0.9, off = aang(ph, sp), N = 150;
  for (const [d, col] of [[0, [255, 80, 0]], [PI, [0, 80, 255]]]) {
    f.seg(curve(N, (i, n) => {
      const t = i / n, a = t * TAU * 3 + off + d, r = t * sc;
      return V(Math.cos(a) * r, Math.sin(a) * r, col[0], col[1], col[2]);
    }), false);
  }
};
GEN[22] = (f, ph, sp, sz) => {                                  // Rose 3
  const sc = SC * ssc(sz) * 0.9, N = 200;
  f.seg(curve(N, (i, n) => {
    const t = csweep(ph, sp, i, n), rad = sc * Math.cos(3 * t);
    return V(rad * Math.cos(t), rad * Math.sin(t), 255, 100, 0);
  }), false);
};

/* ── Curves 23-27 ──────────────────────────────────────────────────── */
GEN[23] = (f, ph, sp, sz) => {                                  // Rose 4
  const sc = SC * ssc(sz) * 0.9, N = 200;
  f.seg(curve(N, (i, n) => {
    const t = csweep(ph, sp, i, n), rad = sc * Math.cos(4 * t);
    return V(rad * Math.cos(t), rad * Math.sin(t), 255, 50, 150);
  }), false);
};
GEN[24] = (f, ph, sp, sz) => {                                  // Heart
  const sc = SC * ssc(sz) * 0.045, a = aang(ph, sp), N = adaptN(sz, 200, 20, 300);
  const ca = Math.cos(a), sa = Math.sin(a);
  f.seg(curve(N, (i, n) => {
    const t = TAU * i / n;
    const hx = sc * 16 * Math.pow(Math.sin(t), 3);
    const hy = sc * (13 * Math.cos(t) - 5 * Math.cos(2 * t) - 2 * Math.cos(3 * t) - Math.cos(4 * t));
    return V(hx * ca - hy * sa, hx * sa + hy * ca, 255, 0, 80);
  }), true);
};
GEN[25] = (f, ph, sp, sz) => {                                  // Infinity
  const sc = SC * ssc(sz) * 0.9, N = adaptN(sz, 500, 60, 800);
  f.seg(curve(N, (i, n) => {
    const t = csweep(ph, sp, i, n), d = 1 + Math.sin(t) * Math.sin(t);
    return V(sc * Math.cos(t) / d, sc * Math.sin(t) * Math.cos(t) / d, 0, 200, 255);
  }), false);
};
GEN[26] = (f, ph, sp, sz) => {                                  // Astroid
  const sc = SC * ssc(sz) * 0.9, N = 200;
  f.seg(curve(N, (i, n) => {
    const t = csweep(ph, sp, i, n);
    return V(sc * Math.pow(Math.cos(t), 3), sc * Math.pow(Math.sin(t), 3), 200, 255, 50);
  }), false);
};
GEN[27] = (f, ph, sp, sz) => {                                  // Epitrochoid
  const R = 3, r = 1, d = 2.5, peak = 1 / (R + r + d);
  const sc = SC * ssc(sz) * 0.9 * peak, off = aang(ph, sp), N = 384;
  f.seg(curve(N, (i, n) => {
    const t = TAU * i / n + off;
    return V(sc * ((R + r) * Math.cos(t) - d * Math.cos((R + r) * t / r)),
             sc * ((R + r) * Math.sin(t) - d * Math.sin((R + r) * t / r)), 0, 255, 100);
  }), true);
};

/* ── 3D 28-32 ──────────────────────────────────────────────────────── */
GEN[28] = (f, ph, sp, sz) => wf(f, CV, CE, aang(ph, sp, 1), aang(ph, sp, 0.4), SC * ssc(sz) * 0.65, 0, 255, 255);
GEN[29] = (f, ph, sp, sz) => wf(f, CV, CE, 0.6, 0.4, SC * ssc(sz) * 0.65, 255, 255, 0);
GEN[30] = (f, ph, sp, sz) => {                                  // Pyramid
  const Vp = [{ x: -1, y: -1, z: -1 }, { x: 1, y: -1, z: -1 }, { x: 1, y: -1, z: 1 }, { x: -1, y: -1, z: 1 }, { x: 0, y: 1, z: 0 }];
  const Ep = [[0, 1], [1, 2], [2, 3], [3, 0], [0, 4], [1, 4], [2, 4], [3, 4]];
  wf(f, Vp, Ep, aang(ph, sp), 0.3, SC * ssc(sz) * 0.65, 255, 255, 0);
};
GEN[31] = (f, ph, sp, sz) => {                                  // Octahedron
  const Vo = [{ x: 1, y: 0, z: 0 }, { x: -1, y: 0, z: 0 }, { x: 0, y: 1, z: 0 }, { x: 0, y: -1, z: 0 }, { x: 0, y: 0, z: 1 }, { x: 0, y: 0, z: -1 }];
  const Eo = [[0, 2], [0, 3], [1, 2], [1, 3], [0, 4], [0, 5], [1, 4], [1, 5], [2, 4], [2, 5], [3, 4], [3, 5]];
  wf(f, Vo, Eo, aang(ph, sp), 0.35, SC * ssc(sz) * 0.7, 0, 255, 0);
};
GEN[32] = (f, ph, sp, sz) => {                                  // Tetrahedron
  const Vt = [{ x: 0, y: 1, z: 0 }, { x: 0.943, y: -0.333, z: 0 }, { x: -0.471, y: -0.333, z: 0.816 }, { x: -0.471, y: -0.333, z: -0.816 }];
  const Et = [[0, 1], [0, 2], [0, 3], [1, 2], [1, 3], [2, 3]];
  wf(f, Vt, Et, aang(ph, sp, 1.2), 0.4, SC * ssc(sz) * 0.75, 0, 0, 255);
};

/* ── Waves 33-50 ───────────────────────────────────────────────────── */
GEN[33] = (f, ph, sp, sz) => sinewave(f, 0.55, 1, aang(ph, sp), SC * ssc(sz) * 0.9, 0, 255, 255);
GEN[34] = (f, ph, sp, sz) => {
  const A = Math.abs(Math.sin(aang(ph, sp))) * 0.8 + 0.1;
  sinewave(f, A, 2, 0, SC * ssc(sz) * 0.9, 0, 255, 0);
};
GEN[35] = (f, ph, sp, sz) => {                                  // Multi Wave
  // Each harmonic's phase from its own aang() call -- see genWave()'s pa().
  const sc = SC * ssc(sz) * 0.9;
  sinewave(f, 0.3, 1, aang(ph, sp), sc, 255, 0, 0);
  sinewave(f, 0.2, 2, aang(ph, sp, 1.5), sc, 0, 255, 0);
  sinewave(f, 0.15, 3, aang(ph, sp, 2), sc, 0, 0, 255);
};
GEN[36] = genWave(0, 0, 255, (x, t, wa, wf_, pa) =>
  wa * (0.3 * Math.sin(4 * wf_ * x * PI + t) + 0.15 * Math.sin(8 * wf_ * x * PI + pa(1.7)) + 0.08 * Math.sin(16 * wf_ * x * PI + pa(2.3))));
GEN[37] = genWave(255, 255, 0, (x, t, wa, wf_) =>
  wa * 0.45 * (Math.sin(5 * wf_ * x * PI + t) + Math.sin(7 * wf_ * x * PI - t)) * 0.5);
GEN[38] = genWave(255, 0, 0, (x, t, wa, wf_) => {
  const p = mod(x * 2 * wf_ + t / PI, 2);
  return wa * 0.6 * (p < 1 ? p : p - 2);
});
GEN[39] = (f, ph, sp, sz) => {                                  // Square Wave
  const sc = SC * ssc(sz) * 0.9, t = aang(ph, sp);
  const wa = live.waveAmp, wf_ = live.waveFreq;
  const cycles = Math.max(1, Math.round(3 * wf_)), steps = cycles * 2;
  const amp = wa * 0.55 * sc, phi = mod(t, TAU) / TAU;
  const verts = [];
  let px = -sc, py = (Math.sin(phi * TAU) > 0 ? 1 : -1) * amp;
  verts.push(V(px, py, 0, 0, 255));
  for (let s = 0; s <= steps; s++) {
    const x = L(-1, 1, s / steps) * sc;
    const lvl = (Math.sin((s / steps + phi) * TAU * cycles) > 0 ? 1 : -1) * amp;
    if (lvl !== py) { verts.push(V(px, lvl, 0, 0, 255)); py = lvl; }
    verts.push(V(x, py, 0, 0, 255));
    px = x;
  }
  f.seg(verts, false);
};
GEN[40] = genWave(0, 255, 0, (x, t, wa, wf_) => Math.exp(-12 * x * x) * wa * Math.sin(8 * wf_ * x * PI + t) * 0.8);
GEN[41] = genWave(255, 255, 0, (x, t, wa, wf_) => 0.5 * wa * (Math.sin(10 * wf_ * x * PI + t) + Math.sin(11 * wf_ * x * PI + t)));
GEN[42] = (f, ph, sp, sz) => {                                  // Radial Waves
  const sc = SC * ssc(sz) * 0.9, t = aang(ph, sp);
  const wa = live.waveAmp, wf_ = live.waveFreq, NR = 4, NA = 80;
  for (let ring = 1; ring <= NR; ring++) {
    const r = ring / NR, R = r * sc;
    f.seg(curve(NA, (i, n) => {
      const a = TAU * i / n, rad = R * (1 + 0.12 * wa * Math.sin(8 * wf_ * a + r * 8 * wf_ - t));
      return V(Math.cos(a) * rad, Math.sin(a) * rad, 0, b255(100 + 155 * r), 255);
    }), true);
  }
};
GEN[43] = genWave(0, 255, 255, (x, t, wa, wf_) => 0.5 * wa * Math.sin(6 * wf_ * x * TAU + 4 * Math.sin(wf_ * x * TAU * 2 + t)));
GEN[44] = (f, ph, sp, sz) => {                                  // Vortex
  const sc = SC * ssc(sz) * 0.9, off = aang(ph, sp);
  const wa = live.waveAmp, wf_ = live.waveFreq, N = 200;
  f.seg(curve(N, (i, n) => {
    const t = i / n, a = t * TAU * 4 * wf_ + off, r = sc * (1 - t * 0.8), w = 0.08 * wa * Math.sin(a * 8);
    return V(Math.cos(a) * (r + w * sc), Math.sin(a) * (r + w * sc), b255(t * 255), b255((1 - t) * 200), 200);
  }), false);
};
GEN[45] = (f, ph, sp, sz) => {                                  // Sine Helix
  const sc = SC * ssc(sz) * 0.9, t = aang(ph, sp);
  const wa = live.waveAmp, wf_ = live.waveFreq, N = 200;
  f.seg(curve(N + 1, (i, n) => {
    const u = L(-1, 1, i / (n - 1)), a = u * TAU * 3 * wf_ + t;
    return V(u * sc, wa * Math.sin(a) * sc * 0.5, 0, b255(128 + 127 * Math.cos(a)), b255(128 + 127 * Math.sin(a)));
  }), false);
};
GEN[46] = (f, ph, sp, sz) => {                                  // Wave Field
  const sc = SC * ssc(sz) * 0.9, t = aang(ph, sp);
  const wa = live.waveAmp, wf_ = live.waveFreq, NROW = 5, NX = 61;
  for (let r0 = 0; r0 < NROW; r0++) {
    const row = r0 - 2, y0 = row * 0.36;
    f.seg(curve(NX, (i, n) => {
      const x = L(-1, 1, i / (n - 1));
      const y = y0 + 0.12 * wa * Math.sin(x * TAU * 3 * wf_ + t + row * 0.7);
      return V(x * sc, y * sc, 0, b255(128 + 127 * Math.sin(row + t)), 200);
    }), false);
  }
};
GEN[47] = genWave(0, 255, 0, (x, t, wa, wf_) => {
  let yy = 0;
  for (let k = 1; k <= 5; k += 2) yy += Math.sin(k * x * TAU * 1.5 * wf_ + t) / k;
  return yy * wa * 0.5;
});
GEN[48] = genWave(255, 0, 255, (x, t, wa, wf_) =>
  Math.exp(-2 * Math.abs(x)) * wa * Math.sin(10 * wf_ * x * PI + t) * (0.4 + 0.4 * Math.abs(x)));
GEN[49] = genWave(0, 255, 255, (x, t, wa, wf_, pa) =>
  (0.5 + 0.5 * x) * 0.5 * wa * Math.sin(TAU * (x * 2 * wf_ - pa(0.3))) * 0.7);
GEN[50] = genWave(255, 200, 50, (x, t, wa, wf_, pa) => {
  let yy = 0;
  for (let j = 0; j < 5; j++) { const k = j + 1; yy += Math.sin(k * wf_ * x * TAU + pa(j * 0.5 + 0.5)) * 0.2 / k; }
  return yy * wa;
});

/* ── Complex 51-56 ─────────────────────────────────────────────────── */
GEN[51] = (f, ph, sp, sz) => {                                  // Hypotrochoid
  const R = 6, r = 1, d = 3, peak = 1 / ((R - r) + d);
  const sc = SC * ssc(sz) * 0.9 * peak, off = aang(ph, sp), N = 384;
  f.seg(curve(N, (i, n) => {
    const t = TAU * i / n + off;
    return V(sc * ((R - r) * Math.cos(t) + d * Math.cos((R - r) * t / r)),
             sc * ((R - r) * Math.sin(t) - d * Math.sin((R - r) * t / r)), 255, 0, 255);
  }), true);
};
GEN[52] = (f, ph, sp, sz) => {                                  // Butterfly
  const sc = SC * ssc(sz) * 0.38, off = aang(ph, sp), N = 200;
  f.seg(curve(N, (i, n) => {
    const t = TAU * i / n;
    const e = Math.exp(Math.cos(t)) - 2 * Math.cos(4 * t) - Math.pow(Math.sin(t / 12), 5);
    return V(sc * e * Math.sin(t + off), sc * e * Math.cos(t + off), 255, 255, 0);
  }), true);
};
GEN[53] = (f, ph, sp, sz) => {                                  // Spirograph 5/3
  const off = aang(ph, sp), R = 5, r = 3, d = 1.5, peak = 1 / ((R - r) + d);
  const sc = SC * ssc(sz) * 0.9 * peak, N = 288, co = Math.cos(off), so = Math.sin(off);
  f.seg(curve(N, (i, n) => {
    const t = 6 * PI * i / n;
    const x = (R - r) * Math.cos(t) + d * Math.cos((R - r) / r * t);
    const y = (R - r) * Math.sin(t) - d * Math.sin((R - r) / r * t);
    return V(sc * (x * co - y * so), sc * (x * so + y * co), 0, 255, 255);
  }), false);
};
GEN[54] = (f, ph, sp, sz) => {                                  // Concentric Rings
  const sc = SC * ssc(sz) * 0.9, pulse = 0.8 + 0.2 * Math.abs(Math.sin(aang(ph, sp, 2)));
  for (let ring = 1; ring <= 5; ring++) {
    const r = sc * ring / 5 * pulse, h = ring / 5;
    const cr = b255(Math.abs(Math.sin(h * PI)) * 255);
    const cg = b255(Math.abs(Math.sin(h * PI + 2.094)) * 255);
    const cb = b255(Math.abs(Math.sin(h * PI + 4.189)) * 255);
    f.seg(ngonVerts(32, r, 0, cr, cg, cb), true);
  }
};
GEN[55] = (f, ph, sp, sz) => {                                  // Nested Squares
  const sc = SC * ssc(sz) * 0.9, br = aang(ph, sp);
  for (let l = 0; l < 6; l++) {
    const s = sc * (6 - l) / 6, rot = br + l * (PI / 24), h = l / 6;
    const cr = b255(Math.abs(Math.sin(h * PI)) * 255);
    const cg = b255(Math.abs(Math.sin(h * PI + 2.094)) * 255);
    const cb = b255(Math.abs(Math.sin(h * PI + 4.189)) * 255);
    f.seg(ngonVerts(4, s, rot, cr, cg, cb), true);
  }
};
GEN[56] = (f, ph, sp, sz) => {                                  // Pulsing Circle
  const sc = SC * ssc(sz) * 0.9;
  const pulse = sc * (0.5 + 0.5 * Math.abs(Math.sin(aang(ph, sp, 3))));
  const rot = aang(ph, sp, 0.2), N = 256;
  f.seg(curve(N, (i, n) => {
    const a = TAU * i / n + rot, wave = 1 + 0.15 * Math.sin(8 * a), r2 = pulse * wave;
    return V(Math.cos(a) * r2, Math.sin(a) * r2, b255(200 + 55 * Math.sin(a)), 0, b255(200 + 55 * Math.cos(a)));
  }), true);
};

/* ── Combo 57-61 ───────────────────────────────────────────────────── */
GEN[57] = (f, ph, sp, sz) => {                                  // Starburst
  const sc = SC * ssc(sz) * 0.9, off = aang(ph, sp), n = 24;
  for (let i = 0; i < n; i++) {
    const a = TAU * i / n + off, inner = sc * 0.3, outer = sc * (0.7 + 0.3 * Math.sin(i * 0.8));
    const cr = b255(128 + 127 * Math.sin(a)), cg = b255(128 + 127 * Math.cos(a));
    f.line(Math.cos(a) * inner, Math.sin(a) * inner, Math.cos(a) * outer, Math.sin(a) * outer, cr, cg, 255);
  }
};
GEN[58] = (f, ph, sp, sz) => {                                  // Chaos Bouncer
  const sc = SC * ssc(sz) * 0.9, dph = aang(ph, sp, 0.3);
  const fx = 4.3, fy = 2.9, cycles = 4, N = 320;
  const tri = u => (2 / PI) * Math.asin(Math.sin(u));
  f.seg(curve(N, (i, n) => {
    const t = TAU * cycles * i / (n - 1);
    return V(sc * tri(fx * t + dph), sc * tri(fy * t),
             b255(128 + 127 * Math.sin(t * 2 + dph)),
             b255(128 + 127 * Math.sin(t * 3 + 1)),
             b255(128 + 127 * Math.cos(t * 1.5)));
  }), false);
};
GEN[59] = (f, ph, sp, sz) => {                                  // Laser Diamond
  const sc = SC * ssc(sz) * 0.9, rot = aang(ph, sp);
  const rotate = (v, a) => V(v[0] * sc * Math.cos(a) - v[1] * sc * Math.sin(a), v[0] * sc * Math.sin(a) + v[1] * sc * Math.cos(a));
  const outer = [[0, 0.9], [0.9, 0], [0, -0.9], [-0.9, 0]].map(v => { const p = rotate(v, rot); return V(p.x, p.y, 0, 255, 255); });
  const inner = [[0, 0.55], [0.55, 0], [0, -0.55], [-0.55, 0]].map(v => { const p = rotate(v, -rot); return V(p.x, p.y, 255, 0, 255); });
  f.seg(outer, true);
  f.seg(inner, true);
  f.seg(ngonVerts(32, 0.72 * sc, rot, 0, 255, 255), true);
};
GEN[60] = (f, ph, sp, sz) => {                                  // Confetti Burst
  const sc = SC * ssc(sz) * 0.9, burst = mod(aang(ph, sp, 0.5) / TAU, 1);
  const NP = 18, NV = 4;
  for (let p = 0; p < NP; p++) {
    const ang = TAU * p / NP + p * 0.37;
    const r = burst * (0.35 + 0.65 * mod(p * 0.191, 1)) * sc;
    const cx = Math.cos(ang) * r, cy = Math.sin(ang) * r;
    const spin = ang * 3 + burst * TAU * 2, pr = 0.05 * sc;
    const cr = b255(128 + 127 * Math.sin(ang));
    const cg = b255(128 + 127 * Math.sin(ang + 2.1));
    const cb = b255(128 + 127 * Math.sin(ang + 4.2));
    f.seg(curve(NV, (i, n) => {
      const a = spin + TAU * i / n;
      return V(cx + Math.cos(a) * pr, cy + Math.sin(a) * pr * 0.6, cr, cg, cb);
    }), true);
  }
};
GEN[61] = (f, ph, sp, sz) => {                                  // Disco Ball
  const sc = SC * ssc(sz) * 0.9, rot = aang(ph, sp), yFore = 0.5;
  f.seg(ngonVerts(48, sc, 0, 255, 255, 255), true);
  for (let row = -2; row <= 2; row++) {
    const lat = row * 30 * PI / 180, ry = Math.sin(lat) * sc, rx = Math.cos(lat) * sc;
    const cw = row === 0 ? 255 : 200;
    f.seg(curve(28, (i, n) => {
      const a = TAU * i / n;
      return V(Math.cos(a) * rx, ry + Math.sin(a) * rx * 0.12,
               b255(cw * (0.5 + 0.5 * Math.sin(a + rot))),
               b255(cw * (0.5 + 0.5 * Math.sin(a + rot + 2.094))),
               b255(cw * (0.5 + 0.5 * Math.sin(a + rot + 4.189))));
    }), true);
  }
  const NM = 6;
  for (let mI = 0; mI < NM; mI++) {
    const lon = PI * mI / NM + rot;
    f.seg(curve(20, i => {
      const v = -PI / 2 + PI * i / 19;
      return V(Math.sin(lon) * Math.cos(v) * sc, Math.sin(v) * sc * yFore * 2,
               b255(128 + 127 * Math.cos(lon * 2 + rot)), b255(128 + 127 * Math.sin(lon * 3 - rot)), 255);
    }), false);
  }
};

/* ── Party 62-63 ───────────────────────────────────────────────────── */
GEN[62] = (f, ph, sp, sz) => {                                  // Hibiscus
  const sc = SC * ssc(sz) * 0.9, rot = aang(ph, sp, 0.2);
  const NP = 5, NT = 31, NC = 12;
  for (let p = 0; p < NP; p++) {
    const base = TAU * p / NP + rot, verts = [];
    for (let i = 0; i < NT; i++) {
      const t = i / (NT - 1), spread = Math.sin(t * PI), a = base + spread * 0.4, r = L(0.15, 0.65, t);
      verts.push(V(Math.cos(a) * r * sc, Math.sin(a) * r * sc, 255, b255(50 + t * 100), b255(100 - t * 100)));
    }
    for (let i = 0; i < NT; i++) {
      const t = (NT - 1 - i) / (NT - 1), spread = Math.sin(t * PI), a = base - spread * 0.4, r = L(0.15, 0.65, t);
      verts.push(V(Math.cos(a) * r * sc, Math.sin(a) * r * sc, 255, b255(50 + t * 100), 0));
    }
    f.seg(verts, true);
  }
  f.seg(curve(NC, (i, n) => {
    const a = TAU * i / n + rot * 2;
    return V(Math.cos(a) * 0.12 * sc, Math.sin(a) * 0.12 * sc, 255, 255, 0);
  }), true);
};
GEN[63] = GEN[57];                                              // Starburst Party

/* ── Scenes / specials ─────────────────────────────────────────────── */
GEN[64] = (f, ph, sp, sz) => {                                  // Starfield
  let nStars = 1 + Math.round(sz / 255 * 149);
  const baseSpd = (sp / 255) * 40;
  const perStar = optCfg.blank + 6;
  const budget = optCfg.maxPts;
  if (nStars * perStar > budget) nStars = Math.max(1, Math.floor(budget / perStar));
  const stars = [];
  for (let i = 0; i < nStars; i++) {
    const xPos = (sHash(i * 7) * 2 - 1) * SC * 0.95;
    const iSpd = baseSpd * (0.3 + sHash(i * 3) * 1.4);
    const off = sHash(i * 5) * 2.2;
    const yNorm = 1.1 - mod(ph * iSpd * 0.0004 + off, 2.2);
    if (yNorm < -1.1 || yNorm > 1.1) continue;
    const period = Math.floor(ph * iSpd * 0.0004);
    const bright = 20 + Math.floor(sHash(i * 2 + period) * 235);
    stars.push({
      x: xPos, y: yNorm * SC * 0.95,
      r: bright, g: b255(bright * (0.85 + sHash(i * 11) * 0.15)),
      b: bright > 160 ? Math.min(255, bright + 40) : b255(bright * 0.6)
    });
  }
  live.starfieldCount = stars.length;
  // greedy nearest-neighbour emission order (as in firmware)
  const used = new Array(stars.length).fill(false);
  let cx = 0, cy = 0;
  for (let s = 0; s < stars.length; s++) {
    let best = -1, bd = Infinity;
    for (let k = 0; k < stars.length; k++) {
      if (used[k]) continue;
      const dx = stars[k].x - cx, dy = stars[k].y - cy, d2 = dx * dx + dy * dy;
      if (d2 < bd) { bd = d2; best = k; }
    }
    if (best < 0) break;
    used[best] = true;
    cx = stars[best].x; cy = stars[best].y;
    f.dot(stars[best].x, stars[best].y, stars[best].r, stars[best].g, stars[best].b);
  }
};

GEN[65] = (f, ph, sp, sz) => {                                  // Countdown Timer
  if (state.cdEnd === undefined) state.cdEnd = nowMs() + live.countdownSec * 1000;
  let rem = Math.max(0, Math.ceil((state.cdEnd - nowMs()) / 1000));
  const expired = rem === 0;
  let cr, cg, cb;
  if (expired) { const blink = Math.floor(ph) % 60 < 30; cr = blink ? 255 : 0; cg = 0; cb = 0; }
  else if (rem <= 10) { cr = 255; cg = 0; cb = 0; }
  else if (rem <= 30) { cr = 255; cg = 255; cb = 0; }
  else { cr = 0; cg = 255; cb = 0; }

  const hh = Math.floor(rem / 3600), mm = Math.floor((rem % 3600) / 60), ss = rem % 60;
  const sc2 = ssc(sz) * 0.85;
  const dw = SC * 0.18 * sc2, dh = SC * 0.38 * sc2, gap = SC * 0.06 * sc2, cdot = SC * 0.04 * sc2;
  const showH = hh > 0;
  const totalW = showH ? (dw * 2 + gap) * 3 + gap * 2 : (dw * 2 + gap) * 2 + gap;
  let ox = -totalW * 0.5 + dw + gap * 0.5;

  const SEG = [
    [1,1,1,1,1,1,0],[0,1,1,0,0,0,0],[1,1,0,1,1,0,1],[1,1,1,1,0,0,1],[0,1,1,0,0,1,1],
    [1,0,1,1,0,1,1],[1,0,1,1,1,1,1],[1,1,1,0,0,0,0],[1,1,1,1,1,1,1],[1,1,1,1,0,1,1]
  ];
  const digit = (xc, d) => {
    const hw = dw * 0.5, hh2 = dh * 0.5, mh = dh * 0.02;
    const segs = [
      [xc - hw + mh, hh2, xc + hw - mh, hh2],
      [xc + hw, mh, xc + hw, hh2 - mh],
      [xc + hw, -hh2 + mh, xc + hw, -mh],
      [xc - hw + mh, -hh2, xc + hw - mh, -hh2],
      [xc - hw, -hh2 + mh, xc - hw, -mh],
      [xc - hw, mh, xc - hw, hh2 - mh],
      [xc - hw + mh, 0, xc + hw - mh, 0]
    ];
    for (let i = 0; i < 7; i++) if (SEG[d][i]) f.line(segs[i][0], segs[i][1], segs[i][2], segs[i][3], cr, cg, cb);
  };
  const colon = cx => {
    f.line(cx - cdot * 0.5, dh * 0.28, cx + cdot * 0.5, dh * 0.28, cr, cg, cb);
    f.line(cx - cdot * 0.5, -dh * 0.28, cx + cdot * 0.5, -dh * 0.28, cr, cg, cb);
  };
  if (showH) {
    digit(ox, Math.floor(hh / 10) % 10); digit(ox + dw + gap, hh % 10);
    colon(ox + dw * 2 + gap * 1.5);
    ox += (dw * 2 + gap) + gap * 2;
  }
  digit(ox, Math.floor(mm / 10)); digit(ox + dw + gap, mm % 10);
  colon(ox + dw * 2 + gap * 1.5);
  ox += (dw * 2 + gap) + gap * 2;
  digit(ox, Math.floor(ss / 10)); digit(ox + dw + gap, ss % 10);
};

GEN[66] = (f, ph, sp, sz) => {                                  // Pentagram
  const sc = SC * ssc(sz) * 0.9, off = aang(ph, sp);
  f.seg(curve(5, (k) => {
    const a = off + k * 4 * PI / 5 - PI / 2;
    return V(Math.cos(a) * sc, Math.sin(a) * sc, 255, 0, 255);
  }), true);
};
GEN[67] = (f, ph, sp, sz) => {                                  // DNA Helix
  const sc = SC * ssc(sz) * 0.9, amp = sc * 0.32, off = aang(ph, sp);
  const NS = 70, TURNS = 3;
  for (const [dphase, col] of [[0, [0, 255, 255]], [PI, [255, 0, 255]]]) {
    f.seg(curve(NS + 1, i => {
      const t = i / NS, x = L(-sc, sc, t), a = t * TAU * TURNS + off + dphase;
      return V(x, amp * Math.sin(a), col[0], col[1], col[2]);
    }), false);
  }
  const RUNGS = 9;
  for (let r = 0; r < RUNGS; r++) {
    const t = r / (RUNGS - 1), x = L(-sc, sc, t), a = t * TAU * TURNS + off;
    f.line(x, amp * Math.sin(a), x, amp * Math.sin(a + PI), 255, 255, 255);
  }
};
GEN[68] = (f, ph, sp, sz) => {                                  // Yin Yang
  const R = SC * ssc(sz) * 0.85, off = aang(ph, sp);
  const c = Math.cos(off), s = Math.sin(off);
  const rot = (x, y, r, g, b) => V(x * c - y * s, x * s + y * c, r, g, b);
  const NC = 64, NS = 32, ND = 20;
  f.seg(curve(NC, (i, n) => { const a = TAU * i / n; return rot(R * Math.cos(a), R * Math.sin(a), 255, 255, 255); }), true);
  const sChain = [];
  for (let i = 0; i <= NS; i++) { const a = -PI / 2 + PI * i / NS; sChain.push(rot(R * 0.5 * Math.cos(a), R * 0.5 + R * 0.5 * Math.sin(a), 255, 255, 255)); }
  for (let i = 1; i <= NS; i++) { const a = PI / 2 + PI * i / NS; sChain.push(rot(R * 0.5 * Math.cos(a), -R * 0.5 + R * 0.5 * Math.sin(a), 255, 255, 255)); }
  f.seg(sChain, false);
  const rd = R * 0.14;
  f.seg(curve(ND, (i, n) => { const a = TAU * i / n; return rot(rd * Math.cos(a), R * 0.5 + rd * Math.sin(a), 255, 0, 0); }), true);
  f.seg(curve(ND, (i, n) => { const a = TAU * i / n; return rot(rd * Math.cos(a), -R * 0.5 + rd * Math.sin(a), 0, 255, 255); }), true);
};
GEN[69] = (f, ph, sp, sz) => {                                  // Random Points
  const RANDOM_PTS_MAX_COUNT = 14;
  const nSlots = 1 + Math.round(sz / 255 * (RANDOM_PTS_MAX_COUNT - 1));
  const fadeMs = 750 - (sp / 255) * 650;
  const holdMs = Math.max(50, live.randomHoldMs);
  const onMs = fadeMs * 2 + holdMs, periodMs = onMs * 2, t0 = nowMs();
  for (let k = 0; k < nSlots; k++) {
    const slotOff = sHash(k * 97 + 11) * periodMs;
    const t = mod(t0 + slotOff, periodMs);
    if (t >= onMs) continue;
    const cycleIdx = Math.floor((t0 + slotOff) / periodMs);
    const seed = k * 10007 + cycleIdx * 997;
    const px = (sHash(seed * 3 + 1) * 2 - 1) * SC * 0.9;
    const py = (sHash(seed * 7 + 2) * 2 - 1) * SC * 0.9;
    const hue = sHash(seed * 5 + 3) * TAU;
    let v;
    if (t < fadeMs) v = t / fadeMs;
    else if (t < fadeMs + holdMs) v = 1;
    else v = 1 - (t - fadeMs - holdMs) / fadeMs;
    f.dot(px, py, b255((128 + 127 * Math.sin(hue)) * v),
                  b255((128 + 127 * Math.sin(hue + 2.094)) * v),
                  b255((128 + 127 * Math.sin(hue + 4.189)) * v));
  }
};
GEN[70] = (f, ph, sp, sz) => {                                  // Three Circles
  const sc = SC * ssc(sz) * 0.9, cx = 0.62 * sc, rad = 0.28 * sc;
  const xs = [-cx, 0, cx], cols = [[255, 0, 0], [0, 255, 0], [0, 0, 255]];
  for (let c = 0; c < 3; c++) {
    f.seg(curve(32, (i, n) => {
      const a = TAU * i / n;
      return V(xs[c] + Math.cos(a) * rad, Math.sin(a) * rad, cols[c][0], cols[c][1], cols[c][2]);
    }), true);
  }
};
GEN[71] = (f, ph, sp, sz) => {                                  // Point Spread
  const sc = SC * 0.85;
  const N = clamp(1 + Math.floor(sz * 11 / 255), 1, 12);
  for (let i = 0; i < N; i++) {
    let x, y;
    if (N === 1) { x = 0; y = 0; }
    else { const a = TAU * i / N - PI / 2; x = Math.cos(a) * sc; y = Math.sin(a) * sc; }
    const h = i / (N > 1 ? N : 1);
    f.dot(x, y, b255(Math.abs(Math.sin(h * PI)) * 255),
                b255(Math.abs(Math.sin(h * PI + 2.094)) * 255),
                b255(Math.abs(Math.sin(h * PI + 4.189)) * 255));
  }
};
GEN[72] = (f, ph, sp, sz) => {                                  // Solar System
  const sc = SC * ssc(sz) * 0.9;
  const ap_ = aang(ph, sp), am = aang(ph, sp, 4);
  const rSun = sc * 0.16, rPla = sc * 0.08, rMoo = sc * 0.04;
  const oPla = sc * 0.48, oMoo = sc * 0.20;
  const px = Math.cos(ap_) * oPla, py = Math.sin(ap_) * oPla;
  const mx = px + Math.cos(am) * oMoo, my = py + Math.sin(am) * oMoo;
  f.seg(curve(48, (i, n) => { const a = TAU * i / n; return V(Math.cos(a) * rSun, Math.sin(a) * rSun, 255, 255, 0); }), true);
  f.seg(curve(36, (i, n) => { const a = TAU * i / n; return V(px + Math.cos(a) * rPla, py + Math.sin(a) * rPla, 0, 0, 255); }), true);
  f.seg(curve(24, (i, n) => { const a = TAU * i / n; return V(mx + Math.cos(a) * rMoo, my + Math.sin(a) * rMoo, 255, 0, 255); }), true);
};

GEN[73] = (f, ph, sp, sz) => {                                  // Bouncing Points
  const MAXB = 8, TRAILMAX = 12;
  if (!state.balls) {
    state.balls = [];
    for (let k = 0; k < MAXB; k++) {
      const ang = sHash(k * 7919 + 1) * TAU;
      const b = {
        x: (sHash(k * 3 + 1) * 2 - 1) * SC * 0.5,
        y: (sHash(k * 5 + 2) * 2 - 1) * SC * 0.5,
        vx: Math.cos(ang), vy: Math.sin(ang), tx: [], ty: []
      };
      for (let t = 0; t < TRAILMAX; t++) { b.tx.push(b.x); b.ty.push(b.y); }
      state.balls.push(b);
    }
    state.lastMs = nowMs();
  }
  const now = nowMs();
  let dt = Math.min(50, now - state.lastMs);
  state.lastMs = now;
  const nBalls = clamp(1 + Math.round((sz / 255) * (MAXB - 1)), 1, MAXB);
  const spd = 4 + (sp / 255) * 32;                 // DAC units per millisecond
  const boundary = SC * 0.92;
  const trailLen = Math.min(live.trail, TRAILMAX);

  for (let k = 0; k < nBalls; k++) {
    const b = state.balls[k];
    b.x += b.vx * spd * dt;
    b.y += b.vy * spd * dt;
    const tdx = b.x - b.tx[0], tdy = b.y - b.ty[0];
    if (tdx * tdx + tdy * tdy >= 400) {
      b.tx.unshift(b.x); b.ty.unshift(b.y);
      b.tx.length = TRAILMAX; b.ty.length = TRAILMAX;
    }
    if (b.x > boundary) { b.x = 2 * boundary - b.x; b.vx = -Math.abs(b.vx); }
    if (b.x < -boundary) { b.x = -2 * boundary - b.x; b.vx = Math.abs(b.vx); }
    if (b.y > boundary) { b.y = 2 * boundary - b.y; b.vy = -Math.abs(b.vy); }
    if (b.y < -boundary) { b.y = -2 * boundary - b.y; b.vy = Math.abs(b.vy); }
  }
  for (let k = 0; k < nBalls; k++) {
    const b = state.balls[k], hue = TAU * k / nBalls;
    const br = 128 + 127 * Math.sin(hue), bg = 128 + 127 * Math.sin(hue + 2.094), bb = 128 + 127 * Math.sin(hue + 4.189);
    for (let t = trailLen - 1; t >= 0; t--) {
      const fade = 1 - (t + 1) / (trailLen + 1);
      f.dot(b.tx[t], b.ty[t], b255(br * fade), b255(bg * fade), b255(bb * fade));
    }
    f.dot(b.x, b.y, b255(br), b255(bg), b255(bb));
  }
};

GEN[74] = (f, ph, sp, sz) => {                                  // Shooting Stars
  const SSMAX = 8, TRAIL = 14;
  const baseSpd = 8 + (sp / 255) * 72;
  const spawn = (met, idx, seed) => {
    const side = sHash(seed);
    const ang = (0.35 + sHash(seed + 7) * 0.35) * PI;
    met.vx = Math.cos(ang) * baseSpd;
    met.vy = Math.sin(ang) * baseSpd;
    if (side < 0.5) {
      met.x = (sHash(seed + 1) * 2 - 1) * SC * 0.9;
      met.y = SC * 0.95;
      met.vy = -Math.abs(met.vy);
    } else {
      met.x = -SC * 0.95;
      met.y = (sHash(seed + 2) * 2 - 1) * SC * 0.9;
      met.vx = Math.abs(met.vx);
    }
    met.tx = new Array(TRAIL).fill(met.x);
    met.ty = new Array(TRAIL).fill(met.y);
    met.plusX = met.x; met.plusY = met.y; met.plusStart = 0; met.hue = idx;
  };
  if (!state.meteors) {
    state.meteors = [];
    for (let k = 0; k < SSMAX; k++) {
      const met = {}; spawn(met, k, k * 6271 + 1);
      const offv = sHash(k * 6271 + 4) * 2;
      met.x += met.vx * offv * 500; met.y += met.vy * offv * 500;
      met.tx.fill(met.x); met.ty.fill(met.y);
      state.meteors.push(met);
    }
    state.lastMs = nowMs();
  }
  const now = nowMs();
  const dt = Math.min(100, now - state.lastMs) || 16;
  state.lastMs = now;
  const nM = 1 + Math.round((sz / 255) * (SSMAX - 1));
  const boundary = SC * 0.98, plusDur = 600;

  for (let k = 0; k < nM; k++) {
    const met = state.meteors[k];
    const len = Math.hypot(met.vx, met.vy);
    if (len > 0.1) { met.vx = met.vx / len * baseSpd; met.vy = met.vy / len * baseSpd; }
    met.x += met.vx * dt; met.y += met.vy * dt;
    const dx = met.x - met.tx[0], dy = met.y - met.ty[0];
    if (dx * dx + dy * dy >= 400) {
      met.tx.unshift(met.x); met.ty.unshift(met.y);
      met.tx.length = TRAIL; met.ty.length = TRAIL;
    }
    if (Math.abs(met.x) > boundary || Math.abs(met.y) > boundary) {
      const px = clamp(met.x, -boundary, boundary), py = clamp(met.y, -boundary, boundary);
      spawn(met, k, Math.floor(now) ^ (k * 6271));
      met.plusX = px; met.plusY = py; met.plusStart = now;
    }
  }
  for (let k = 0; k < nM; k++) {
    const met = state.meteors[k], hue = TAU * met.hue / SSMAX;
    const cr = 180 + 75 * Math.sin(hue), cg = 180 + 75 * Math.sin(hue + 2.094);
    for (let t = TRAIL - 1; t >= 0; t--) {
      const fade = 1 - (t + 1) / (TRAIL + 1);
      f.dot(met.tx[t], met.ty[t], b255(cr * fade), b255(cg * fade), b255(255 * fade));
    }
    f.dot(met.x, met.y, b255(cr), b255(cg), 255);
    if (met.plusStart > 0 && now - met.plusStart < plusDur) {
      const tn = 1 - (now - met.plusStart) / plusDur, fade = tn * tn, pv = b255(255 * fade);
      const arm = SC * 0.06;
      f.dot(met.plusX - arm, met.plusY, pv, pv, pv);
      f.dot(met.plusX, met.plusY, pv, pv, pv);
      f.dot(met.plusX + arm, met.plusY, pv, pv, pv);
      f.dot(met.plusX, met.plusY - arm, pv, pv, pv);
      f.dot(met.plusX, met.plusY + arm, pv, pv, pv);
    }
  }
};

GEN[75] = (f, ph, sp, sz) => {                                  // Pythagoras Tree
  const PT_DEPTH = 7;
  const spd = 0.003 + (sp / 255) * 0.027;
  const frac = mod(ph * spd, 1);
  const zoom = Math.pow(0.5, frac);
  const cosA = 0.70711, sinA = 0.70711;
  const baseW = SC * (0.55 + (sz / 255) * 0.35) * zoom;
  const stack = [{ x0: -baseW * 0.5, y0: SC * 0.72, x1: baseW * 0.5, y1: SC * 0.72, depth: 0 }];
  const drawDepth = PT_DEPTH + 1;
  let guard = 0;
  while (stack.length && guard++ < 600) {
    const fr = stack.pop();
    const ex = fr.x1 - fr.x0, ey = fr.y1 - fr.y0, w = Math.hypot(ex, ey);
    if (w < 2) continue;
    const upx = ey, upy = -ex;
    const blx = fr.x0, bly = fr.y0, brx = fr.x1, bry = fr.y1;
    const trx = brx + upx, try_ = bry + upy, tlx = blx + upx, tly = bly + upy;
    let brightness = 1;
    if (fr.depth === 0) brightness = 1 - frac;
    else if (fr.depth === drawDepth) brightness = frac;
    const cd = Math.min(fr.depth, PT_DEPTH), tc = cd / PT_DEPTH;
    const cr = b255(255 * (1 - tc) * brightness), cg = b255(255 * tc * brightness);
    f.line(blx, -bly, tlx, -tly, cr, cg, 0);
    f.line(brx, -bry, trx, -try_, cr, cg, 0);
    if (fr.depth < drawDepth) {
      const rx = ex * cosA + ey * sinA, ry = -ex * sinA + ey * cosA;
      const len = Math.hypot(rx, ry), wL = w * cosA;
      const apLx = tlx + rx / len * wL, apLy = tly + ry / len * wL;
      stack.push({ x0: tlx, y0: tly, x1: apLx, y1: apLy, depth: fr.depth + 1 });
      stack.push({ x0: apLx, y0: apLy, x1: trx, y1: try_, depth: fr.depth + 1 });
    }
  }
};

GEN[76] = (f, ph, sp, sz) => {                                  // Endless Spiral
  const sc = SC * ssc(sz) * 0.95, flow = aang(ph, sp);
  const arms = clamp(live.spiralArms, 1, 6), perArm = adaptN(sz, 150, 40, 300), turns = 4;
  for (let a = 0; a < arms; a++) {
    const armPhase = TAU * a / arms;
    f.seg(curve(perArm, i => {
      const t = i / (perArm - 1);
      const ang = armPhase + flow + t * turns * TAU, rad = t * sc;
      const v = b255(40 + 215 * t);
      return V(Math.cos(ang) * rad, Math.sin(ang) * rad, v, 0, b255(255 - v));
    }), false);
  }
};
GEN[77] = (f, ph, sp, sz) => {                                  // Endless Tunnel
  const sc = SC * ssc(sz) * 0.98;
  const rings = clamp(live.tunnelRings, 3, 12), sides = clamp(live.tunnelSides, 3, 10);
  const vpx = sc * 0.12, vpy = -sc * 0.10;
  const travel = sp === 0 ? 0 : mod(ph * (sp / 9000), 1);
  for (let rIdx = 0; rIdx < rings; rIdx++) {
    let depth = rIdx / rings + travel / rings;
    depth -= Math.floor(depth);
    const scale = 0.04 + depth * depth, rad = scale * sc;
    const v = b255(30 + 225 * depth);
    const cx = vpx * (1 - depth), cy = vpy * (1 - depth);
    f.seg(curve(sides, (s, n) => {
      const aa = TAU * s / n - PI / 2;
      return V(cx + Math.cos(aa) * rad, cy + Math.sin(aa) * rad, v, b255(v / 2), 255);
    }), true);
  }
};
GEN[78] = (f, ph, sp, sz) => {                                  // Explosion Spread
  const sc = SC * ssc(sz) * 0.95;
  const rays = clamp(live.explosionRays, 4, 40);
  const cycleMs = 1600 - (sp / 255) * 1400;
  const now = nowMs();
  const cycleId = Math.floor(now / cycleMs);
  const prog = (now % cycleMs) / cycleMs;
  const ease = 1 - (1 - prog) * (1 - prog);
  const fade = prog < 0.7 ? 1 : (1 - (prog - 0.7) / 0.3);
  for (let i = 0; i < rays; i++) {
    const baseA = TAU * i / rays;
    const jit = (sHash(cycleId * 131 + i * 17) - 0.5) * (TAU / rays);
    const ang = baseA + jit;
    const rOuter = ease * sc, rInner = rOuter * 0.55;
    const hueI = i * 255 / rays;
    const r = b255(Math.abs(Math.sin(hueI / 40)) * 255 * fade);
    const g = b255(Math.abs(Math.sin(hueI / 40 + 2.094)) * 255 * fade);
    const b = b255(Math.abs(Math.sin(hueI / 40 + 4.189)) * 255 * fade);
    f.line(Math.cos(ang) * rInner, Math.sin(ang) * rInner, Math.cos(ang) * rOuter, Math.sin(ang) * rOuter, r, g, b);
  }
};
GEN[79] = (f, ph, sp, sz) => {                                  // Fireworks
  const sc = SC * ssc(sz) * 0.9;
  const shells = clamp(live.fwShells, 1, 3), glitter = live.fwGlitter;
  const riseMs = 1800 - (sp / 255) * 1500, burstMs = 1100;
  const lifeMs = riseMs + burstMs, periodMs = lifeMs + 400;
  const now = nowMs();
  for (let s = 0; s < shells; s++) {
    const off = sHash(s * 733 + 3) * periodMs;
    const t = mod(now + off, periodMs);
    const cycle = Math.floor((now + off) / periodMs);
    if (t >= lifeMs) continue;
    const seed = s * 10007 + cycle * 997;
    const launchX = (sHash(seed + 1) * 1.6 - 0.8) * sc;
    const burstY = (0.15 + sHash(seed + 2) * 0.55) * sc;
    const hue = sHash(seed + 3) * TAU;
    const startY = -sc * 0.95;
    const driftX = (sHash(seed + 5) - 0.5) * 0.35 * sc;
    const curveX = (sHash(seed + 6) - 0.5) * 0.18 * sc;
    const finalX = launchX + driftX;
    if (t < riseMs) {
      const rp = t / riseMs;
      const ry = L(startY, burstY, rp);
      const rx = launchX + driftX * rp + curveX * Math.sin(rp * PI);
      const rv = b255(180 + 75 * Math.sin(rp * PI));
      f.dot(rx, ry, rv, rv, 255);
    } else {
      const bp = (t - riseMs) / burstMs;
      const ease = 1 - (1 - bp) * (1 - bp);
      const fade = bp < 0.6 ? 1 : (1 - (bp - 0.6) / 0.4);
      if (bp < 0.12) { const fv = b255(255 * (1 - bp / 0.12)); f.dot(finalX, burstY, fv, fv, fv); }
      const variant = Math.floor(sHash(seed + 60) * 4) & 3;
      const sparks = variant === 3 ? 26 : (variant === 1 ? 24 : 18);
      const radBase = ease * sc * 0.42;
      const h = hue + bp * PI;
      for (let i = 0; i < sparks; i++) {
        const a = TAU * i / sparks + (sHash(seed + 40 + i) - 0.5) * 0.3;
        let gl = 1;
        if (glitter) {
          const tw = sHash(Math.floor(now / 60) * 31 + i * 7 + seed);
          gl = 0.35 + 0.65 * (tw > 0.5 ? 1 : 0.25);
        }
        let px, py;
        if (variant === 0) { const rad = radBase * 0.55; px = finalX + Math.cos(a) * rad; py = burstY + Math.sin(a) * rad; }
        else if (variant === 1) { const rad = radBase * 1.35; px = finalX + Math.cos(a) * rad; py = burstY + Math.sin(a) * rad; }
        else if (variant === 2) {
          const radX = radBase * (0.9 + sHash(seed + 61) * 0.5);
          const radY = radX * (0.35 + sHash(seed + 62) * 0.35);
          const rot = sHash(seed + 63) * TAU;
          const lx = Math.cos(a) * radX, ly = Math.sin(a) * radY;
          px = finalX + lx * Math.cos(rot) - ly * Math.sin(rot);
          py = burstY + lx * Math.sin(rot) + ly * Math.cos(rot);
        } else {
          const outRad = radBase * 0.5, fall = bp * bp * sc * 0.5;
          px = finalX + Math.cos(a) * outRad;
          py = burstY + Math.sin(a) * outRad * 0.4 - fall;
        }
        let r, g, b;
        if (variant === 3) { r = b255(255 * fade * gl); g = b255(190 * fade * gl); b = b255(40 * fade * gl); }
        else {
          r = b255((128 + 127 * Math.sin(h)) * fade * gl);
          g = b255((128 + 127 * Math.sin(h + 2.094)) * fade * gl);
          b = b255((128 + 127 * Math.sin(h + 4.189)) * fade * gl);
        }
        f.dot(px, py, r, g, b);
      }
    }
  }
};
GEN[80] = (f, ph, sp, sz) => {                                  // Milky Way
  const sc = SC * ssc(sz) * 0.92;
  const dots = clamp(live.mwDots, 10, 60), tiltPct = clamp(live.mwTilt, 20, 80);
  const yScale = tiltPct / 100, rot = aang(ph, sp), armTurns = 2.4;
  for (let i = 0; i < dots; i++) {
    const t = i / (dots - 1), arm = i & 1, armA = arm * PI;
    const rad = Math.pow(t, 0.7) * sc;
    const jit = (sHash(i * 53 + 7) - 0.5) * 0.35;
    const ang = armA + rot + t * armTurns * TAU + jit;
    const x = Math.cos(ang) * rad, y = Math.sin(ang) * rad * yScale;
    const coreB = 1 - 0.6 * t;
    const tw = 0.8 + 0.2 * sHash(Math.floor(ph / 4) * 17 + i * 3);
    const v = clamp(255 * coreB * tw, 20, 255);
    f.dot(x, y, b255(v * (0.85 - 0.35 * t)), b255(v * (0.9 - 0.2 * t)), b255(v));
  }
};


/* ══════════════════════════════════════════════════════════════════════
   Simplified point optimizer + post-processing + renderer
   ══════════════════════════════════════════════════════════════════════ */

// Per-profile optimizer parameters, mirroring OPT_PROFILE_DEFAULTS[] in
// include/config.h (order == OPT_PROFILE_* indices). The sim's optimizer is a
// simplified port -- corner dwell, length-proportional interior density and a
// budget squeeze -- so only the parameters those three stages actually read
// are carried here; the rest live in the /api/config mock below so the
// Optimizer tab shows the real numbers.
const OPT_PROFILES = [
  { name: 'Vector',      cornerAngle: 30, cornerMin: 2, cornerMax: 8, pts: 9.0,  blank: 16, minBlank: 6,  maxPts: 1300 },
  { name: 'Smooth',      cornerAngle: 60, cornerMin: 2, cornerMax: 3, pts: 11.0, blank: 16, minBlank: 6,  maxPts: 1300 },
  { name: 'Waves',       cornerAngle: 35, cornerMin: 2, cornerMax: 6, pts: 8.0,  blank: 16, minBlank: 6,  maxPts: 1300 },
  { name: 'Wireframe',   cornerAngle: 25, cornerMin: 2, cornerMax: 8, pts: 6.0,  blank: 20, minBlank: 4,  maxPts: 1300 },
  { name: 'MultiObject', cornerAngle: 25, cornerMin: 2, cornerMax: 6, pts: 5.0,  blank: 18, minBlank: 4,  maxPts: 1300 },
  { name: 'Particles',   cornerAngle: 25, cornerMin: 2, cornerMax: 4, pts: 6.0,  blank: 40, minBlank: 32, maxPts: 1300 },
  { name: 'Trails',      cornerAngle: 60, cornerMin: 3, cornerMax: 3, pts: 11.0, blank: 16, minBlank: 6,  maxPts: 880  },
  { name: 'Text',        cornerAngle: 28, cornerMin: 2, cornerMax: 5, pts: 6.0,  blank: 16, minBlank: 4,  maxPts: 1300 }
];
const OPT_PROFILE_PARTICLES = 5;   // OPT_PROFILE_* index, see include/config.h

// presetClassOf() (preset_patterns.cpp) as a name -> profile map. Keyed by
// PRESETS[] name rather than by index so a future insertion into PRESETS[]
// cannot silently shift a whole class onto the wrong profile. Anything
// unlisted falls back to Vector, exactly like the firmware's `default:`.
const PROFILE_MEMBER_NAMES = [
  ['Circle', 'Square', 'Triangle', 'Pentagon', 'Hexagon', 'Octagon',
   'Star 4', 'Star 5', 'Star 6', 'Star 8', 'Pentagram',
   'Cross +', 'X Shape', 'Grid 3x3', 'H Line', 'Diagonal', 'Three Circles'],
  ['Endless Spiral', 'Archimedean Spiral', 'Double Spiral',
   'Lissajous 1:2', 'Lissajous 2:3', 'Lissajous 3:4', 'Lissajous 3:5', 'Lissajous 5:6',
   'Rose 3', 'Rose 4', 'Heart', 'Infinity', 'Astroid', 'Epitrochoid',
   'Hypotrochoid', 'Butterfly', 'Spirograph 5/3', 'Pulsing Circle',
   'DNA Helix', 'Yin Yang'],
  ['Sine Wave', 'Standing Wave', 'Multi Wave', 'Ocean Wave', 'Wave Interference',
   'Sawtooth', 'Square Wave', 'Wave Packet', 'Beat Wave', 'Radial Waves',
   'FM Wave', 'Vortex', 'Sine Helix', 'Wave Field', 'Fourier Square',
   'Gravity Waves', 'Tsunami', 'Wave Spectrum'],
  ['Rotating Cube', 'Static Cube', 'Pyramid', 'Octahedron', 'Tetrahedron'],
  ['Solar System', 'Concentric Rings', 'Nested Squares', 'Disco Ball',
   'Laser Diamond', 'Starburst', 'Starburst Party', 'Hibiscus',
   'Chaos Bouncer', 'Countdown Timer', 'Endless Tunnel', 'Pythagoras Tree'],
  ['Starfield', 'Random Points', 'Point Spread', 'Confetti Burst',
   'Bouncing Points', 'Explosion Spread', 'Fireworks', 'Milky Way'],
  ['Shooting Stars'],
  []   // Text -- selected by the text renderer, not by presetClassOf()
];

const PRESET_PROFILE = (function() {
  const m = new Array(PRESETS.length).fill(0);
  PROFILE_MEMBER_NAMES.forEach(function(names, prof) {
    names.forEach(function(n) {
      const i = PRESETS.findIndex(p => p[0] === n);
      if (i >= 0) m[i] = prof;
    });
  });
  return m;
})();

function profileForPreset(idx) {
  const p = PRESET_PROFILE[idx];
  return OPT_PROFILES[p === undefined ? 0 : p];
}

// Active config for the frame currently being rendered -- set by simTick().
let optCfg = OPT_PROFILES[0];

function blankTo(out, from, x, y, cfg) {
  const n = cfg.blank;
  for (let d = 1; d <= n; d++) {
    const t = d / n, e = t * t * (3 - 2 * t);   // smoothstep easing
    out.push({ x: from.x + (x - from.x) * e, y: from.y + (y - from.y) * e, r: 0, g: 0, b: 0, blank: 1 });
  }
}

function exteriorAngleDeg(a, b, c) {
  const v1x = b.x - a.x, v1y = b.y - a.y, v2x = c.x - b.x, v2y = c.y - b.y;
  const l1 = Math.hypot(v1x, v1y), l2 = Math.hypot(v2x, v2y);
  if (l1 < 1e-6 || l2 < 1e-6) return 0;
  const cosv = clamp((v1x * v2x + v1y * v2y) / (l1 * l2), -1, 1);
  return Math.acos(cosv) * 180 / PI;
}

// Interior-density scale the last optimize() call ended up with (1.0 = the
// profile's requested density, below that = Stage 2 squeezed it to fit the
// frame budget). Published as stage2_scale on /api/optimizer-stats.
let simLastDensityScale = 1.0;

function optimize(frame, cfg) {
  let density = cfg.pts;
  let out = [];
  for (let pass = 0; pass < 4; pass++) {
    out = [];
    let last = null;
    for (const s of frame.segs) {
      const v = s.v;
      if (last) blankTo(out, last, v[0].x, v[0].y, cfg);
      out.push({ x: v[0].x, y: v[0].y, r: v[0].r, g: v[0].g, b: v[0].b, blank: 0 });
      const edges = s.closed ? v.length : v.length - 1;
      for (let i = 0; i < edges; i++) {
        const a = v[i], bb = v[(i + 1) % v.length];
        const len = Math.hypot(bb.x - a.x, bb.y - a.y);
        const k = Math.max(1, Math.round(len / 1000 * density));
        for (let j = 1; j <= k; j++) {
          const t = j / k;
          out.push({ x: a.x + (bb.x - a.x) * t, y: a.y + (bb.y - a.y) * t, r: bb.r, g: bb.g, b: bb.b, blank: 0 });
        }
        // corner dwell at the arrival vertex
        const cNext = v[(i + 2) % v.length];
        if (i < edges - 1 || s.closed) {
          const ang = exteriorAngleDeg(a, bb, cNext);
          const dwell = ang > cfg.cornerAngle ? cfg.cornerMax : cfg.cornerMin;
          for (let d = 0; d < dwell; d++) out.push({ x: bb.x, y: bb.y, r: bb.r, g: bb.g, b: bb.b, blank: 0 });
        }
      }
      last = out[out.length - 1];
    }
    for (const d of frame.dots) {
      if (last) blankTo(out, last, d.x, d.y, cfg);
      for (let i = 0; i < 5; i++) out.push({ x: d.x, y: d.y, r: d.r, g: d.g, b: d.b, blank: 0, dot: 1 });
      last = out[out.length - 1];
    }
    if (out.length <= cfg.maxPts || density <= 0.25) break;
    density = Math.max(0.25, density * (cfg.maxPts / out.length) * 0.96);
  }
  simLastDensityScale = cfg.pts > 0 ? density / cfg.pts : 1.0;
  if (out.length > cfg.maxPts) out.length = cfg.maxPts;
  return out;
}

/* ── Post-processing (mirrors pattern_engine.cpp) ───────────────────── */

function applyRotations(pts, ui) {
  const rad = ui.rotation * PI / 180;
  const zAng = (ui.rotZ ? ui.rotAngleZ : 0) + rad;
  if (zAng !== 0) {
    const c = Math.cos(zAng), s = Math.sin(zAng);
    for (const p of pts) { const nx = p.x * c - p.y * s, ny = p.x * s + p.y * c; p.x = nx; p.y = ny; }
  }
  if (ui.rotY) {
    const cy = Math.cos(ui.rotAngleY), sy = Math.sin(ui.rotAngleY);
    for (const p of pts) {
      const z3 = p.x * sy, nx = p.x * cy;
      let d = 1 + z3 * 0.35 / 32767; if (d < 0.1) d = 0.1;
      p.x = nx / d; p.y = p.y / d;
    }
  }
  if (ui.rotX) {
    const cx = Math.cos(ui.rotAngleX), sx = Math.sin(ui.rotAngleX);
    for (const p of pts) {
      const z3 = p.y * sx, ny = p.y * cx;
      let d = 1 + z3 * 0.35 / 32767; if (d < 0.1) d = 0.1;
      p.y = ny / d; p.x = p.x / d;
    }
  }
}

function decimate(pts, maxPerCopy) {
  if (pts.length <= maxPerCopy) return pts;
  const stride = pts.length / maxPerCopy;
  let nextPick = 0;
  const outp = [];
  for (let i = 0; i < pts.length; i++) {
    if (pts[i].blank || i >= nextPick) { outp.push(pts[i]); nextPick += stride; }
  }
  return outp;
}

function radialCopy(pts, segments, mirrorH, mirrorV, cfg) {
  const segs = clamp(segments, 2, 8);
  const jumpCost = (segs - 1) * cfg.minBlank;
  if (cfg.maxPts <= jumpCost) return pts;
  const maxPerCopy = Math.floor((cfg.maxPts - jumpCost) / segs);
  if (maxPerCopy < 2) return pts;
  const src = decimate(pts, maxPerCopy);
  const out = [];
  for (let k = 0; k < segs; k++) {
    const angle = k * TAU / segs, ca = Math.cos(angle), sa = Math.sin(angle);
    const flip = k % 2 === 1;
    const fx = flip && mirrorH ? -1 : 1, fy = flip && mirrorV ? -1 : 1;
    if (k > 0) {
      const first = src[0];
      const fsx = first.x * fx, fsy = first.y * fy;
      const dstX = fsx * ca - fsy * sa, dstY = fsx * sa + fsy * ca;
      const prev = out[out.length - 1];
      for (let d = 0; d < cfg.minBlank; d++) {
        const t = (d + 1) / cfg.minBlank;
        out.push({ x: prev.x + (dstX - prev.x) * t, y: prev.y + (dstY - prev.y) * t, r: 0, g: 0, b: 0, blank: 1 });
      }
    }
    for (const p of src) {
      const sx = p.x * fx, sy = p.y * fy;
      out.push({ x: sx * ca - sy * sa, y: sx * sa + sy * ca, r: p.r, g: p.g, b: p.b, blank: p.blank, dot: p.dot });
    }
  }
  return out;
}

// applyMirrorKaleido() -- true dihedral fold (group D_S), the default
// Kaleidoscope mode since fw v6.67.0. radialCopy() above is the plain
// rotational copy (C_S) the "Radial Repeat" mode still uses; this one first
// mirror-folds every point into a single [0, alpha] wedge and then stamps
// that wedge around, alternating rotation with a reflection so adjacent
// copies mirror each other across their shared edge. Segment count is
// even-only so the fold always closes.
function mirrorKaleido(pts, segments, cfg) {
  let segs = clamp(segments, 2, 8) & ~1;
  if (segs < 2) segs = 2;
  const alpha = TAU / segs, nFold = segs / 2;
  const n0x = 0, n0y = 1, n1x = Math.sin(alpha), n1y = -Math.cos(alpha);
  const jumpCost = (segs - 1) * cfg.minBlank;
  if (cfg.maxPts <= jumpCost) return pts;
  const maxPerCopy = Math.floor((cfg.maxPts - jumpCost) / segs);
  if (maxPerCopy < 2) return pts;

  // Fold AFTER decimation so mirror-axis crossings land on existing points.
  const src = decimate(pts, maxPerCopy).map(function(p) {
    let px = p.x, py = p.y;
    for (let fi = 0; fi < nFold; fi++) {
      const nx = (fi & 1) ? n1x : n0x, ny = (fi & 1) ? n1y : n0y;
      const d = px * nx + py * ny;
      if (d < 0) { px -= 2 * d * nx; py -= 2 * d * ny; }
    }
    return { x: px, y: py, r: p.r, g: p.g, b: p.b, blank: p.blank, dot: p.dot };
  });

  const out = [];
  for (let k = 0; k < segs; k++) {
    // Per-copy 2x2 matrix: even k = rotation by k*alpha; odd k = rotation by
    // (k+1)*alpha composed with a reflection across the x-axis.
    let m00, m01, m10, m11;
    const theta = ((k & 1) === 0 ? k : k + 1) * alpha;
    const ca = Math.cos(theta), sa = Math.sin(theta);
    if ((k & 1) === 0) { m00 = ca; m01 = -sa; m10 = sa; m11 = ca; }
    else               { m00 = ca; m01 = sa;  m10 = sa; m11 = -ca; }
    if (k > 0) {
      const first = src[0], prev = out[out.length - 1];
      const dstX = m00 * first.x + m01 * first.y, dstY = m10 * first.x + m11 * first.y;
      for (let d = 0; d < cfg.minBlank; d++) {
        const t = (d + 1) / cfg.minBlank;
        out.push({ x: prev.x + (dstX - prev.x) * t, y: prev.y + (dstY - prev.y) * t, r: 0, g: 0, b: 0, blank: 1 });
      }
    }
    for (const p of src) {
      out.push({ x: m00 * p.x + m01 * p.y, y: m10 * p.x + m11 * p.y,
                 r: p.r, g: p.g, b: p.b, blank: p.blank, dot: p.dot });
    }
  }
  return out;
}

function mirrorCopy(pts, flipX, flipY, cfg) {
  const maxPerCopy = Math.floor((cfg.maxPts - cfg.minBlank) / 2);
  if (maxPerCopy < 2) return pts;
  const src = decimate(pts, maxPerCopy);
  const out = src.slice();
  const first = src[0], prev = out[out.length - 1];
  const dstX = flipX ? -first.x : first.x, dstY = flipY ? -first.y : first.y;
  for (let d = 0; d < cfg.minBlank; d++) {
    const t = (d + 1) / cfg.minBlank;
    out.push({ x: prev.x + (dstX - prev.x) * t, y: prev.y + (dstY - prev.y) * t, r: 0, g: 0, b: 0, blank: 1 });
  }
  for (const p of src) out.push({ x: flipX ? -p.x : p.x, y: flipY ? -p.y : p.y, r: p.r, g: p.g, b: p.b, blank: p.blank, dot: p.dot });
  return out;
}

function radial4(pts, cfg) {
  const jumpCost = 3 * cfg.minBlank;
  const maxPerCopy = Math.floor((cfg.maxPts - jumpCost) / 4);
  if (maxPerCopy < 2) return pts;
  const src = decimate(pts, maxPerCopy);
  const flips = [[false, false], [true, false], [false, true], [true, true]];
  const out = [];
  for (let k = 0; k < 4; k++) {
    const [fx, fy] = flips[k];
    if (k > 0) {
      const first = src[0], prev = out[out.length - 1];
      const dstX = fx ? -first.x : first.x, dstY = fy ? -first.y : first.y;
      for (let d = 0; d < cfg.minBlank; d++) {
        const t = (d + 1) / cfg.minBlank;
        out.push({ x: prev.x + (dstX - prev.x) * t, y: prev.y + (dstY - prev.y) * t, r: 0, g: 0, b: 0, blank: 1 });
      }
    }
    for (const p of src) out.push({ x: fx ? -p.x : p.x, y: fy ? -p.y : p.y, r: p.r, g: p.g, b: p.b, blank: p.blank, dot: p.dot });
  }
  return out;
}

function applyColor(pts, ui) {
  const dim = ui.dimmer / 255;
  for (const p of pts) {
    if (ui.colOverride) { p.r = ui.col[0]; p.g = ui.col[1]; p.b = ui.col[2]; }
    p.r = b255(p.r * dim); p.g = b255(p.g * dim); p.b = b255(p.b * dim);
  }
}

// applyPointsOnlyMode() -- subsamples the already-transformed frame down to a
// handful of dwelling dots with a fade in/out cycle (pattern_engine.cpp).
// Operates purely on the final point array, so it works for every preset.
// The firmware's Dotter scatter and BPM-sync phase pinning are left out here
// (both need modulator/BPM state the sim does not model); everything else --
// dwell derived from the frame budget, the six wipe directions, the
// "no fade configured means always visible" rule -- is ported as-is.
const FADE_DIR = { IN_OUT: 0, OUT_IN: 1, LEFT_RIGHT: 2, RIGHT_LEFT: 3, TOP_BOTTOM: 4, BOTTOM_TOP: 5 };

function fadeWipePosition(dir, x, y, cx, cy, minX, maxX, minY, maxY, halfDiag) {
  const c01 = v => clamp(v, 0, 1);
  const d = Math.hypot(x - cx, y - cy);
  switch (dir) {
    case FADE_DIR.OUT_IN:      return 1 - c01(d / halfDiag);
    case FADE_DIR.LEFT_RIGHT:  return maxX > minX ? c01((x - minX) / (maxX - minX)) : 0;
    case FADE_DIR.RIGHT_LEFT:  return maxX > minX ? 1 - c01((x - minX) / (maxX - minX)) : 0;
    case FADE_DIR.TOP_BOTTOM:  return maxY > minY ? c01((y - minY) / (maxY - minY)) : 0;
    case FADE_DIR.BOTTOM_TOP:  return maxY > minY ? 1 - c01((y - minY) / (maxY - minY)) : 0;
    default:                   return c01(d / halfDiag);
  }
}

let pmAccMs = 0;

function pointsOnlyMode(pts, ui, cfg, dtMs) {
  const count = clamp(Math.round(ui.pointsCount), 2, 50);
  const lit = pts.filter(p => !p.blank);
  if (!lit.length) return [];

  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  for (const p of lit) {
    if (p.x < minX) minX = p.x;
    if (p.x > maxX) maxX = p.x;
    if (p.y < minY) minY = p.y;
    if (p.y > maxY) maxY = p.y;
  }
  const cx = (minX + maxX) * 0.5, cy = (minY + maxY) * 0.5;
  const halfDiag = Math.max(1, Math.max(maxX - minX, maxY - minY) * 0.5);

  const dwell = clamp(Math.floor(cfg.maxPts / count / 2), 3, 30);
  const cycleMs = Math.max(1, ui.fadeInMs + ui.fadeOutMs);
  pmAccMs = (pmAccMs + dtMs) % cycleMs;
  const noFade = ui.fadeInMs === 0 && ui.fadeOutMs === 0;

  const out = [];
  let prev = null;
  for (let k = 0; k < count; k++) {
    const src = lit[Math.floor(k * lit.length / count)];
    let v = 1;
    if (!ui.pointsStatic && !noFade) {
      const wipeT = fadeWipePosition(ui.fadeDir, src.x, src.y, cx, cy, minX, maxX, minY, maxY, halfDiag);
      const dotPhaseMs = (pmAccMs + wipeT * cycleMs) % cycleMs;
      if (dotPhaseMs < ui.fadeInMs) {
        const t = ui.fadeInMs ? dotPhaseMs / ui.fadeInMs : 1;
        v = ui.fadeInOn ? t * t * (3 - 2 * t) : 1;
      } else {
        const t = ui.fadeOutMs ? (dotPhaseMs - ui.fadeInMs) / ui.fadeOutMs : 1;
        v = ui.fadeOutOn ? 1 - t * t * (3 - 2 * t) : 0;
      }
    }
    if (prev) blankTo(out, prev, src.x, src.y, cfg);
    for (let d = 0; d < dwell; d++) {
      out.push({ x: src.x, y: src.y, r: b255(src.r * v), g: b255(src.g * v), b: b255(src.b * v), blank: 0, dot: 1 });
    }
    prev = out[out.length - 1];
  }
  return out;
}

/* ── Renderer ──────────────────────────────────────────────────────── */

const canvas = document.getElementById('sim-scan');
const ctx = canvas.getContext('2d');

function render(pts, ui) {
  const W = canvas.width, H = canvas.height;
  if (ui.afterglow) {
    ctx.globalCompositeOperation = 'source-over';
    ctx.fillStyle = 'rgba(0,0,0,0.42)';
    ctx.fillRect(0, 0, W, H);
  } else {
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, W, H);
  }
  const scale = (W * 0.5) / 34000;
  const cx = W / 2, cy = H / 2;
  const px = p => cx + p.x * scale;
  const py = p => cy - p.y * scale;

  ctx.globalCompositeOperation = 'lighter';
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';

  if (ui.showBlank) {
    ctx.strokeStyle = 'rgba(120,120,160,0.45)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let i = 1; i < pts.length; i++) {
      if (!pts[i].blank) continue;
      ctx.moveTo(px(pts[i - 1]), py(pts[i - 1]));
      ctx.lineTo(px(pts[i]), py(pts[i]));
    }
    ctx.stroke();
  }

  // Two passes: wide soft halo, then thin hot core.
  for (const layer of [{ w: 5.5, a: 0.16 }, { w: 1.6, a: 1.0 }]) {
    ctx.lineWidth = layer.w;
    for (let i = 1; i < pts.length; i++) {
      const a = pts[i - 1], b = pts[i];
      if (a.blank || b.blank) continue;
      const dx = px(b) - px(a), dy = py(b) - py(a);
      if (dx * dx + dy * dy < 0.02) continue;
      ctx.strokeStyle = `rgba(${b.r},${b.g},${b.b},${layer.a})`;
      ctx.beginPath();
      ctx.moveTo(px(a), py(a));
      ctx.lineTo(px(b), py(b));
      ctx.stroke();
    }
    // dwelling dots
    ctx.globalAlpha = layer.a;
    for (let i = 0; i < pts.length; i++) {
      const p = pts[i];
      if (!p.dot || p.blank) continue;
      if (i > 0 && pts[i - 1].dot && !pts[i - 1].blank && pts[i - 1].x === p.x && pts[i - 1].y === p.y) continue;
      ctx.fillStyle = `rgb(${p.r},${p.g},${p.b})`;
      ctx.beginPath();
      ctx.arc(px(p), py(p), layer.w * 0.65, 0, TAU);
      ctx.fill();
    }
    ctx.globalAlpha = 1;
  }

  if (ui.showPoints) {
    ctx.fillStyle = 'rgba(255,255,255,0.75)';
    for (const p of pts) {
      if (p.blank) continue;
      ctx.fillRect(px(p) - 0.75, py(p) - 0.75, 1.5, 1.5);
    }
  }
  ctx.globalCompositeOperation = 'source-over';
}


/* ── Sim UI state (mirrors gLivePreset + gState) ------------------ */
const simUI = {
  preset: 0, speed: 60, size: 180, rotation: 0, dimmer: 255,
  rotX: false, rotY: false, rotZ: false, rotSpeed: 0.03,
  rotAngleX: 0, rotAngleY: 0, rotAngleZ: 0,
  // kaleidoMode: 1 = Kaleidoscope (mirror fold, KALEIDO_MODE_MIRROR),
  // 0 = Radial Repeat. Mirror H/V only affect Radial Repeat, same as fw.
  kaleido: false, kaleidoMode: 1, kalSeg: 4, kalMH: false, kalMV: false,
  mirrorMode: 0,
  colOverride: false, col: [0, 255, 136],
  autoscaleSpeed: 0, autoscaleMode: 0,
  pointsOnly: false, pointsCount: 12, pointsStatic: false,
  fadeInOn: false, fadeOutOn: false, fadeInMs: 0, fadeOutMs: 0, fadeDir: 0,
  afterglow: true, showBlank: false, showPoints: false
};

/* ── Simulated hardware/config state ----------------------------- */
const SIM_START_MS = performance.now();

// Field names match buildStateJson()/buildCoreStatusJson() in
// src/net/web_ui.cpp exactly -- the WebUI bundle below is the real one, so a
// renamed key here shows up as an empty Dashboard field, not as an error.
const SIM_MOCK_STATE = {
  fw_version: FW_VERSION,
  hostname: 'galvos-sim', ip: 'simulator', rssi: -42,
  uptime_s: 0, free_heap: 285000, free_psram: 7800000, heap: 285000, psram: 7800000,
  laser_armed: false, arm_requested: false, estop_ok: true, scanfail_ok: true,
  safety_override: false, watchdog_ok: true, subsystems_ok: true, last_failsafe: '',
  no_hw_mode: true, dac_ok: true, source: 0,
  preset_idx: 0, points_per_sec: 28500, fps: 25,
  frame_n: 0, frame_lit: 0, frame_blank: 0, text_truncated: false,
  starfield_stars: 0,
  master_dimmer: 0, ui_override: true, ui_master_dimmer: 255,
  cpu0: 42, cpu1: 8, ota_pass: 'S1MU1AT0', auth_token: '',
  bpm: 120.0, bpm_source: 0, bpm_phase: 0,
  seq_running: false, seq_current: 0, seq_stepcount: 0, seq_loop: false,
  last_dmx_age_ms: -1, dmx_frame_count: 0,
  calib_active: false, ilda_active: false, playlist_active: false,
  etherdream_connected: false, etherdream_playing: false,
  helios_net_connected: false, helios_net_playing: false,
  osc_active: false, sacn_active: false,
  sd_ready: false, sd_fs_type: '', sd_free_kb: 0, sd_total_kb: 0, sd_file_count: 0,
  sd_error: 'No SD card (simulator)',
  ntp_server: '', ntp_tz: '', ntp_synced: false,
  dacClipX: 0, dacClipY: 0, dacClipPct: 0,
  fan1_duty: 0, fan2_duty: 0, temp_alert: false, temp_crit: false,
  temps: [42.3, 38.1, 51.7, 35.4, 44.9], temp_raw: [42.3, 38.1, 51.7, 35.4, 44.9],
  temp_unit: 'C', buffer_fill: 85,
  ok: [true, true, true, true, true], found: 5,
  names: ['ESP32 Core', 'PSU', 'Galvo X Driver', 'Galvo Y Driver', 'Laser Module'],
  temp_offsets: [0, 0, 0, 0, 0]
};

// Everything the sim persists for a WebUI round trip. Written by the POST
// branches of the mock, read back by the matching GET so the tabs behave like
// the real device does across a tab switch / page reload.
const SIM_STORE = {
  weld: { enabled: false, direction: 0, speed: 6000, glow: 4000, sparks: 6, spark_life: 260 },
  warp: { enabled: false, gridSize: 2, points: null, test: false },
  brightness: { enabled: false, gridSize: 2, gain: null },
  zone: { enabled: false, x: [], y: [] },
  text: { text: 'GALVOS', font: 0, anim: 0, speed: 128, size: 128,
          col_r: 255, col_g: 255, col_b: 255, rainbow: false,
          flip_x: false, flip_y: false, orbit_reverse: false, active: false },
  paint: { active: false, strokes: [] },
  timer: { remaining: 90, running: false, expired: false },
  activeProfile: 0
};

// WarpConfig::resetIdentity() (include/config.h) -- same row/column order and
// the same -1..+1 mapping, so a grid saved in the sim would be byte-compatible
// with one saved on the device.
function identityWarp(size) {
  const n = clamp(size, 2, 5);
  const pts = [];
  for (let r = 0; r < n; r++) {
    const row = [];
    for (let c = 0; c < n; c++) row.push([-1 + (2 * c) / (n - 1), -1 + (2 * r) / (n - 1)]);
    pts.push(row);
  }
  return pts;
}
function identityGain(size) {
  const g = [];
  for (let r = 0; r < size; r++) g.push(new Array(size).fill(255));
  return g;
}
SIM_STORE.warp.points = identityWarp(2);
SIM_STORE.brightness.gain = identityGain(2);

/* ══════════════════════════════════════════════════════════════════════
   Paint by Finger / Text Mode / Laser Welding — beam-preview renderers
   ══════════════════════════════════════════════════════════════════════
   Ports src/patterns/paint_patterns.cpp, text_renderer.cpp and the
   weld_path.h/weld_patterns.cpp Welding effect so the #sim-scan preview
   actually shows what these three modes project, matching
   pattern_engine.cpp's source priority (Text > Paint > Preset) -- until
   now the preview only ever rendered GEN[simUI.preset], so switching to
   Paint/Text (with or without Welding) left the canvas showing whatever
   preset was selected before. */

/* ── Paint canvas -> Frame ───────────────────────────────────────────
   paint::generate() (paint_patterns.cpp): each stroke is already a plain
   list of galvo-unit vertices with one uniform color, exactly what
   Frame.seg() wants. */
function buildPaintFrame(strokes) {
  const f = new Frame();
  (strokes || []).forEach(function(s) {
    if (!s || !s.x || !s.y) return;
    const n = Math.min(s.x.length, s.y.length);
    if (n < 2) return;
    const verts = [];
    for (let i = 0; i < n; i++) verts.push(V(s.x[i], s.y[i], s.r, s.g, s.b));
    f.seg(verts, !!s.closed);
  });
  return f;
}

/* ── Vector font -- transcribed from text_renderer.cpp's FONT_* tables ──
   Same (x,y) stroke data, PU=pen-up, EN=terminator, coordinate space
   x∈[-5,5] y∈[-7,7] (+y=up). Kept as flat arrays + a shared parser
   (mirrors renderGlyph()'s own loop) instead of hand-splitting each
   glyph, so the two stay comparable stroke-for-stroke against the C++
   source during a future re-sync. */
const GLYPH_PU = 126, GLYPH_EN = 127;
const FONT_RAW = {
  ' ': [GLYPH_EN, GLYPH_EN],
  'A': [-4,-7, 0,7, 4,-7, GLYPH_PU,0, -3,-2, 3,-2, GLYPH_EN,GLYPH_EN],
  'B': [-4,-7,-4,7, GLYPH_PU,0,-4,7,1,7,3,5,3,2,1,0,-4,0, GLYPH_PU,0,-4,0,1,0,3,-2,3,-5,1,-7,-4,-7, GLYPH_EN,GLYPH_EN],
  'C': [4,-5,2,-7,-2,-7,-4,-5,-4,5,-2,7,2,7,4,5, GLYPH_EN,GLYPH_EN],
  'D': [-4,-7,-4,7, GLYPH_PU,0,-4,7,1,7,4,4,4,-4,1,-7,-4,-7, GLYPH_EN,GLYPH_EN],
  'E': [-4,7,4,7, GLYPH_PU,0,-4,7,-4,-7, GLYPH_PU,0,-4,-7,4,-7, GLYPH_PU,0,-4,0,2,0, GLYPH_EN,GLYPH_EN],
  'F': [-4,7,4,7, GLYPH_PU,0,-4,7,-4,-7, GLYPH_PU,0,-4,0,2,0, GLYPH_EN,GLYPH_EN],
  'G': [4,5,2,7,-2,7,-4,5,-4,-5,-2,-7,2,-7,4,-5,4,0,1,0, GLYPH_EN,GLYPH_EN],
  'H': [-4,-7,-4,7, GLYPH_PU,0,4,-7,4,7, GLYPH_PU,0,-4,0,4,0, GLYPH_EN,GLYPH_EN],
  'I': [-2,-7,2,-7, GLYPH_PU,0,0,-7,0,7, GLYPH_PU,0,-2,7,2,7, GLYPH_EN,GLYPH_EN],
  'J': [4,7,4,-5,2,-7,-1,-7,-3,-5,-3,-3, GLYPH_EN,GLYPH_EN],
  'K': [-4,-7,-4,7, GLYPH_PU,0,4,-7,-4,0, GLYPH_PU,0,-4,0,4,7, GLYPH_EN,GLYPH_EN],
  'L': [-4,7,-4,-7, GLYPH_PU,0,-4,-7,4,-7, GLYPH_EN,GLYPH_EN],
  'M': [-4,-7,-4,7,0,1,4,7,4,-7, GLYPH_EN,GLYPH_EN],
  'N': [-4,-7,-4,7,4,-7,4,7, GLYPH_EN,GLYPH_EN],
  'O': [-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, GLYPH_EN,GLYPH_EN],
  'P': [-4,-7,-4,7, GLYPH_PU,0,-4,7,2,7,4,5,4,2,2,0,-4,0, GLYPH_EN,GLYPH_EN],
  'Q': [-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, GLYPH_PU,0,1,-4,4,-7, GLYPH_EN,GLYPH_EN],
  'R': [-4,-7,-4,7, GLYPH_PU,0,-4,7,2,7,4,5,4,2,2,0,-4,0, GLYPH_PU,0,-1,0,4,-7, GLYPH_EN,GLYPH_EN],
  'S': [4,6,2,7,-2,7,-4,5,-4,2,-2,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-6, GLYPH_EN,GLYPH_EN],
  'T': [-4,7,4,7, GLYPH_PU,0,0,7,0,-7, GLYPH_EN,GLYPH_EN],
  'U': [-4,7,-4,-5,-2,-7,2,-7,4,-5,4,7, GLYPH_EN,GLYPH_EN],
  'V': [-4,7,0,-7,4,7, GLYPH_EN,GLYPH_EN],
  'W': [-4,7,-2,-7,0,1,2,-7,4,7, GLYPH_EN,GLYPH_EN],
  'X': [-4,-7,4,7, GLYPH_PU,0,4,-7,-4,7, GLYPH_EN,GLYPH_EN],
  'Y': [-4,7,0,0, GLYPH_PU,0,4,7,0,0,0,-7, GLYPH_EN,GLYPH_EN],
  'Z': [-4,7,4,7,4,5,-4,-5,-4,-7,4,-7, GLYPH_EN,GLYPH_EN],
  '0': [-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, GLYPH_PU,0,-3,-5,3,5, GLYPH_EN,GLYPH_EN],
  '1': [-2,5,0,7,0,-7, GLYPH_PU,0,-3,-7,3,-7, GLYPH_EN,GLYPH_EN],
  '2': [-4,5,-2,7,2,7,4,5,4,2,-4,-4,-4,-7,4,-7, GLYPH_EN,GLYPH_EN],
  '3': [-4,6,-2,7,2,7,4,5,4,2,2,0, GLYPH_PU,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-6, GLYPH_EN,GLYPH_EN],
  '4': [4,-2,-4,-2,-1,7, GLYPH_PU,0,4,7,4,-7, GLYPH_EN,GLYPH_EN],
  '5': [4,7,-4,7,-4,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-5, GLYPH_EN,GLYPH_EN],
  '6': [3,7,0,7,-4,4,-4,-5,-2,-7,2,-7,4,-5,4,-2,2,0,-4,0, GLYPH_EN,GLYPH_EN],
  '7': [-4,7,4,7,4,5,-1,-7, GLYPH_EN,GLYPH_EN],
  '8': [0,0,-4,2,-4,5,-2,7,2,7,4,5,4,2,0,0,-4,-2,-4,-5,-2,-7,2,-7,4,-5,4,-2,0,0, GLYPH_EN,GLYPH_EN],
  '9': [4,2,4,5,2,7,-2,7,-4,5,-4,2,-2,0,4,0,4,-6,2,-7,-1,-7, GLYPH_EN,GLYPH_EN],
  '.': [0,-7,0,-6, GLYPH_EN,GLYPH_EN],
  ',': [0,-7,0,-6, GLYPH_PU,0,0,-7,-1,-9, GLYPH_EN,GLYPH_EN],
  '!': [0,7,0,-2, GLYPH_PU,0,0,-5,0,-7, GLYPH_EN,GLYPH_EN],
  '?': [-3,5,-2,7,2,7,4,5,4,3,0,0,0,-3, GLYPH_PU,0,0,-6,0,-7, GLYPH_EN,GLYPH_EN],
  '-': [-3,0,3,0, GLYPH_EN,GLYPH_EN],
  '+': [0,4,0,-4, GLYPH_PU,0,-4,0,4,0, GLYPH_EN,GLYPH_EN],
  ':': [0,3,0,2, GLYPH_PU,0,0,-2,0,-3, GLYPH_EN,GLYPH_EN],
  '#': [-2,5,-2,-5, GLYPH_PU,0,2,5,2,-5, GLYPH_PU,0,-4,2,4,2, GLYPH_PU,0,-4,-2,4,-2, GLYPH_EN,GLYPH_EN],
  '@': [2,0,-1,0,-3,2,-3,5,-1,7,2,7,4,5,4,-5,2,-7,-2,-7,-4,-5,-4,5,-2,7, GLYPH_EN,GLYPH_EN],
  '*': [0,5,0,-5, GLYPH_PU,0,-4,3,4,-3, GLYPH_PU,0,-4,-3,4,3, GLYPH_EN,GLYPH_EN]
};
const FONT_ADVANCE = {
  ' ':10, 'A':10,'B':10,'C':10,'D':10,'E':10,'F':10,'G':10,'H':10,'I':6,'J':8,'K':10,'L':9,
  'M':12,'N':10,'O':10,'P':10,'Q':11,'R':10,'S':10,'T':10,'U':10,'V':10,'W':12,'X':10,'Y':10,'Z':10,
  '0':10,'1':7,'2':10,'3':10,'4':10,'5':10,'6':10,'7':10,'8':10,'9':10,
  '.':6, ',':6, '!':6, '?':10, '-':8, '+':8, ':':6, '#':10, '@':12, '*':8
};
// parseGlyphSubpaths() -- mirrors renderGlyph()'s stroke-array walk: PU
// flushes the current pen-down run and starts a new one, EN ends the glyph.
function parseGlyphSubpaths(flat) {
  const subs = []; let cur = [];
  for (let i = 0; i < flat.length; i += 2) {
    const sx = flat[i];
    if (sx === GLYPH_EN) { if (cur.length) subs.push(cur); break; }
    const sy = flat[i + 1];
    if (sx === GLYPH_PU) { if (cur.length) subs.push(cur); cur = []; continue; }
    cur.push([sx, sy]);
  }
  return subs;
}
const FONT_GLYPHS = {};
Object.keys(FONT_RAW).forEach(function(ch) {
  FONT_GLYPHS[ch] = { subpaths: parseGlyphSubpaths(FONT_RAW[ch]), advance: FONT_ADVANCE[ch] };
});

// sizeToScale() (text_renderer.cpp) -- glyph-unit -> world-unit scale.
function sizeToScaleJs(sizeVal) {
  const BASE_SCALE = 32767 * 0.85 / 14;
  return BASE_SCALE * (0.1 + sizeVal / 255 * 0.9);
}
// textWidth() -- bug-compatible with the firmware: an unsupported char is
// skipped (cursor still advances at render time, but the WIDTH sum does
// not include it) -- a pre-existing quirk, not something to fix here.
function textWidthJs(text) {
  let w = 0;
  for (let i = 0; i < text.length; i++) {
    const g = FONT_GLYPHS[text[i].toUpperCase()];
    if (g) w += g.advance;
  }
  return w;
}
function renderGlyphInto(f, glyph, ox, oy, sc, r, g, b, dxOff, dyOff) {
  dxOff = dxOff || 0; dyOff = dyOff || 0;
  glyph.subpaths.forEach(function(sub) {
    if (sub.length < 2) return;
    f.seg(sub.map(function(p) { return V(ox + p[0] * sc + dxOff, oy + p[1] * sc + dyOff, r, g, b); }), false);
  });
}
// rainbow hue ramp -- same 6-sector formula as renderTextString()'s inline switch.
function rainbowColor(hue) {
  hue = hue - Math.floor(hue);
  const h6 = hue * 6, hi = Math.floor(h6) % 6, fr = h6 - Math.floor(h6);
  switch (hi) {
    case 0: return [255, b255(255 * fr), 0];
    case 1: return [b255(255 * (1 - fr)), 255, 0];
    case 2: return [0, 255, b255(255 * fr)];
    case 3: return [0, b255(255 * (1 - fr)), 255];
    case 4: return [b255(255 * fr), 0, 255];
    default: return [255, 0, b255(255 * (1 - fr))];
  }
}
// centroidTransform() -- applies fn(dx,dy)->[dx,dy] to every vertex added to
// `f` since `fromSeg`, around their shared centroid. Backs TANIM_ROTATE and
// the Flip X/Y post-pass, both of which firmware applies to the whole
// rendered string at once (renderTextString()'s tail).
function centroidTransform(f, fromSeg, fn) {
  let sx = 0, sy = 0, n = 0;
  for (let i = fromSeg; i < f.segs.length; i++) for (const v of f.segs[i].v) { sx += v.x; sy += v.y; n++; }
  if (!n) return;
  const cx = sx / n, cy = sy / n;
  for (let i = fromSeg; i < f.segs.length; i++)
    for (const v of f.segs[i].v) { const p = fn(v.x - cx, v.y - cy); v.x = cx + p[0]; v.y = cy + p[1]; }
}
// renderTextStringFrame() -- port of renderTextString(): lays out one line
// of `text` into Frame segments (one per glyph sub-path) starting at
// (tx,ty), font size `sc`, then applies whole-string rotation/flip.
function renderTextStringFrame(f, text, cfg, tx, ty, sc, rot, waveOn, waveT) {
  rot = rot || 0; waveOn = !!waveOn; waveT = waveT || 0;
  const fromSeg = f.segs.length;
  let cx = tx;
  for (let ci = 0; ci < text.length; ci++) {
    const glyph = FONT_GLYPHS[text[ci].toUpperCase()];
    if (!glyph) { cx += 10 * sc; continue; }
    let charTy = ty;
    if (waveOn) charTy += Math.sin(waveT + ci * 0.6) * sc * 3;
    let r = cfg.col_r, g = cfg.col_g, b = cfg.col_b;
    if (cfg.rainbow) { const c = rainbowColor(waveT * 0.5 + ci * 0.3); r = c[0]; g = c[1]; b = c[2]; }
    const gx = cx + glyph.advance * 0.5 * sc;
    if (cfg.font === 1) {                                        // FONT_BOLD
      const off = sc * 0.25;
      renderGlyphInto(f, glyph, gx, charTy, sc, r, g, b, -off * 0.5, 0);
      renderGlyphInto(f, glyph, gx, charTy, sc, r, g, b,  off * 0.5, 0);
    } else if (cfg.font === 2) {                                  // FONT_OUTLINE
      const off = sc * 0.055, dr = b255(r / 3), dg = b255(g / 3), db = b255(b / 3);
      renderGlyphInto(f, glyph, gx, charTy, sc, dr, dg, db,  off,  0);
      renderGlyphInto(f, glyph, gx, charTy, sc, dr, dg, db, -off,  0);
      renderGlyphInto(f, glyph, gx, charTy, sc, dr, dg, db,  0,   off);
      renderGlyphInto(f, glyph, gx, charTy, sc, dr, dg, db,  0,  -off);
      renderGlyphInto(f, glyph, gx, charTy, sc, r, g, b, 0, 0);
    } else {                                                      // FONT_SIMPLE
      renderGlyphInto(f, glyph, gx, charTy, sc, r, g, b, 0, 0);
    }
    cx += glyph.advance * sc;
  }
  if (Math.abs(rot) > 0.001) {
    const cr = Math.cos(rot), sr = Math.sin(rot);
    centroidTransform(f, fromSeg, function(dx, dy) { return [dx * cr - dy * sr, dx * sr + dy * cr]; });
  }
  if (cfg.flip_x || cfg.flip_y) {
    centroidTransform(f, fromSeg, function(dx, dy) { return [cfg.flip_x ? -dx : dx, cfg.flip_y ? -dy : dy]; });
  }
}
// TANIM_ORBIT -- wraps a flat rendered string onto a spinning sphere and
// perspective-projects it. Firmware blanks (but still draws through) the
// back hemisphere on the dense post-optimizer point buffer; the sim only
// has the sparse pre-optimizer vertex list, so the back hemisphere is
// dropped by splitting each glyph sub-path into its front-facing runs
// instead -- visually equivalent (nothing lit on the far side either way).
function buildOrbitFrame(f, text, cfg, displaySc, t) {
  const orbitSc = displaySc * 0.55;
  const otw = textWidthJs(text) * orbitSc;
  const tmp = new Frame();
  renderTextStringFrame(tmp, text, cfg, -otw / 2, 0, orbitSc);
  if (!tmp.segs.length) return;
  const R = 20000, focal = 42000, camZ = R + focal;
  const spin = mod(t * 1.2 * (cfg.orbit_reverse ? -1 : 1), TAU);
  const ARC = 1.4, fullW = Math.max(1, otw), kLon = ARC / fullW;
  const LAT_BAND = 0.20, glyphHalf = Math.max(1, 7 * orbitSc), kLat = LAT_BAND / glyphHalf;
  tmp.segs.forEach(function(seg) {
    let run = [];
    seg.v.forEach(function(v) {
      const phi = v.x * kLon + spin, lat = v.y * kLat, clat = Math.cos(lat);
      const X = R * Math.sin(phi) * clat, Y = R * Math.sin(lat), Z = R * Math.cos(phi) * clat;
      const proj = focal / Math.max(camZ - Z, 1000);
      if (Z >= 0) run.push(V(clamp(X * proj, -32767, 32767), clamp(Y * proj, -32767, 32767), v.r, v.g, v.b));
      else { if (run.length > 1) f.seg(run, false); run = []; }
    });
    if (run.length > 1) f.seg(run, false);
  });
}
// TANIM_STARWARS -- perspective crawl (own layout loop in generateImpl(),
// not renderTextString() -- always FONT_SIMPLE/no rainbow, matching fw).
function buildStarWarsFrame(f, text, cfg, displaySc, t) {
  const SE = 32767, SPAN = 2 * SE;
  const yBase = -SE + mod(t * 8000, SPAN);
  const persp = clamp((SE - yBase) / SPAN, 0.05, 1.0);
  const scaleP = displaySc * (0.2 + persp * 0.8);
  const twP = textWidthJs(text) * scaleP;
  const squeeze = 0.55 + 0.45 * persp;
  let cx = -twP / 2;
  for (let ci = 0; ci < text.length; ci++) {
    const glyph = FONT_GLYPHS[text[ci].toUpperCase()];
    if (!glyph) { cx += 10 * scaleP; continue; }
    const gx = cx + glyph.advance * 0.5 * scaleP;
    const fromSeg = f.segs.length;
    renderGlyphInto(f, glyph, gx, yBase, scaleP, cfg.col_r, cfg.col_g, cfg.col_b, 0, 0);
    for (let i = fromSeg; i < f.segs.length; i++)
      for (const v of f.segs[i].v) v.y = yBase + (v.y - yBase) * squeeze;
    cx += glyph.advance * scaleP;
  }
}
// buildTextFrame() -- port of text_renderer.cpp's generateImpl(): picks the
// display scale/animation and lays the string out into a Frame. `phase` is
// the same wall-clock-paced integer simTick() already advances for presets
// (kAnimPhaseFrameMs), matching how pattern_engine.cpp feeds its own phase
// counter into textrender::generate() unmodified.
function buildTextFrame(cfg, phase) {
  const f = new Frame();
  const text = (cfg.text || '').slice(0, 127);
  if (!text) return f;
  const sc = sizeToScaleJs(clamp(cfg.size, 0, 255));
  const spd = clamp(cfg.speed, 0, 255) / 255;
  const t = phase * spd * 0.08;
  const fullLen = text.length;
  let tw = textWidthJs(text) * sc;
  const maxHalf = 30000, GLYPH_HALF_H = 7;
  let anim = cfg.anim;
  if (anim === 0 && fullLen > 16) anim = 1;             // TEXT_MAX_STATIC_CHARS auto-scroll
  const scrolls = (anim === 1 || anim === 2);
  const scH = (GLYPH_HALF_H * sc > maxHalf) ? maxHalf / GLYPH_HALF_H : sc;
  const scW = (tw * 0.5 > maxHalf) ? sc * maxHalf / (tw * 0.5) : sc;
  const displaySc = scrolls ? scH : Math.min(scH, scW);
  if (displaySc !== sc) tw = textWidthJs(text) * displaySc;
  const startX = -tw / 2, baseY = 0;

  switch (anim) {
    case 0: renderTextStringFrame(f, text, cfg, startX, baseY, displaySc); break;
    case 1: { const SE = 32767, period = tw + 2 * SE, ox = SE - mod(t * 8000, period);
              renderTextStringFrame(f, text, cfg, ox, baseY, displaySc); break; }
    case 2: { const SE = 32767, period = tw + 2 * SE, ox = -tw - SE + mod(t * 8000, period);
              renderTextStringFrame(f, text, cfg, ox, baseY, displaySc); break; }
    case 3: { const SE = 32767, range = Math.max(1000, SE - tw * 0.5), bx = Math.sin(t * 2) * range;
              renderTextStringFrame(f, text, cfg, startX + bx, baseY, displaySc); break; }
    case 4: {                                                     // Typewriter
      const fpc = Math.max(4, Math.round(255 * 12 / Math.max(1, cfg.speed)));
      const cyc = fullLen + 3;
      let visible = Math.floor(phase / fpc) % cyc;
      if (visible > fullLen) visible = fullLen;
      if (visible === 0) break;
      const temp = text.slice(0, visible);
      const vw = textWidthJs(temp) * displaySc;
      renderTextStringFrame(f, temp, cfg, -vw / 2, baseY, displaySc);
      break;
    }
    case 5: renderTextStringFrame(f, text, cfg, startX, baseY, displaySc, 0, true, t); break;
    case 6: { const pulseSc = displaySc * (0.7 + 0.3 * Math.abs(Math.sin(t * 3))), pw = textWidthJs(text) * pulseSc;
              renderTextStringFrame(f, text, cfg, -pw / 2, baseY, pulseSc); break; }
    case 7: { const rot = mod(t * 1.5, TAU);
              renderTextStringFrame(f, text, cfg, startX, baseY, displaySc, rot); break; }
    case 8: { const zoom = 0.3 + 0.7 * (0.5 + 0.5 * Math.sin(t * 2)), zoomSc = displaySc * zoom,
                    zw = textWidthJs(text) * zoomSc;
              renderTextStringFrame(f, text, cfg, -zw / 2, baseY, zoomSc); break; }
    case 10: buildOrbitFrame(f, text, cfg, displaySc, t); break;
    case 11: buildStarWarsFrame(f, text, cfg, displaySc, t); break;
    default: renderTextStringFrame(f, text, cfg, startX, baseY, displaySc);
  }
  return f;
}
// glyphOutlinePathsJs() -- port of glyphOutlinePaths(): raw (un-optimized)
// per-glyph sub-paths, world-scaled. Backs /api/text/vertices (Paint tab's
// "Insert as strokes") and the Text-Weld path source below.
function glyphOutlinePathsJs(text, scale) {
  if (!text || scale <= 0) return [];
  text = text.slice(0, 127);
  let tw = textWidthJs(text) * scale;
  const maxHalf = 30000;
  if (tw * 0.5 > maxHalf) { scale *= maxHalf / (tw * 0.5); tw = textWidthJs(text) * scale; }
  let cx = -tw / 2;
  const paths = [];
  for (let ci = 0; ci < text.length; ci++) {
    const glyph = FONT_GLYPHS[text[ci].toUpperCase()];
    if (!glyph) { cx += 10 * scale; continue; }
    const gx = cx + glyph.advance * 0.5 * scale;
    glyph.subpaths.forEach(function(sub) {
      if (sub.length < 2) return;
      paths.push({ x: sub.map(function(p) { return gx + p[0] * scale; }),
                   y: sub.map(function(p) { return p[1] * scale; }) });
    });
    cx += glyph.advance * scale;
  }
  return paths;
}

/* ── Laser Welding -- port of weld_path.h/weld_patterns.cpp ─────────
   A torch head travels an arc-length-parameterized path built from
   whichever source (Paint canvas / Text glyph outlines) is active,
   trailing a fading afterglow and throwing ballistic sparks. */
function buildArcLengthPathJs(strokes) {
  const nodes = [], liftS = []; let s = 0;
  strokes.forEach(function(st) {
    const n = Math.min(st.x.length, st.y.length);
    if (n < 2) return;
    for (let i = 0; i < n; i++) {
      if (i > 0) s += Math.hypot(st.x[i] - st.x[i - 1], st.y[i] - st.y[i - 1]);
      nodes.push({ x: st.x[i], y: st.y[i], s: s });
      if (i === 0) liftS.push(s);
    }
    if (st.closed) {
      s += Math.hypot(st.x[0] - st.x[n - 1], st.y[0] - st.y[n - 1]);
      nodes.push({ x: st.x[0], y: st.y[0], s: s });
    }
  });
  if (nodes.length < 2) return null;
  return { nodes: nodes, liftS: liftS, pathLen: nodes[nodes.length - 1].s };
}
function sampleAtJs(nodes, pathLen, s) {
  if (nodes.length < 2 || s < 0 || s > pathLen) return null;
  let lo = 0, hi = nodes.length - 1, idx = 0;
  while (lo <= hi) { const mid = (lo + hi) >> 1; if (nodes[mid].s <= s) { idx = mid; lo = mid + 1; } else hi = mid - 1; }
  if (idx >= nodes.length - 1) { const last = nodes[nodes.length - 1]; return { x: last.x, y: last.y }; }
  const a = nodes[idx], b = nodes[idx + 1], ds = b.s - a.s, t = ds > 1e-6 ? (s - a.s) / ds : 0;
  return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t };
}
function crossesLiftJs(liftS, sa, sb) {
  const lo = Math.min(sa, sb), hi = Math.max(sa, sb);
  for (let i = 0; i < liftS.length; i++) if (liftS[i] > lo && liftS[i] < hi) return true;
  return false;
}
function weldPathFromPaint(strokes) {
  const src = (strokes || []).filter(function(s) { return s && s.x && s.y && Math.min(s.x.length, s.y.length) >= 2; })
    .map(function(s) { return { x: s.x, y: s.y, closed: !!s.closed }; });
  return src.length ? buildArcLengthPathJs(src) : null;
}
function weldPathFromText(cfg) {
  const scale = sizeToScaleJs(clamp(cfg.size, 0, 255));
  const paths = glyphOutlinePathsJs(cfg.text || '', scale);
  if (!paths.length) return null;
  return buildArcLengthPathJs(paths.map(function(p) { return { x: p.x, y: p.y, closed: false }; }));
}

// Persistent effect state (weld_patterns.cpp's file-scope statics) -- one
// travelling head regardless of which source is active, same as firmware
// (only one of Paint/Text is ever the active source at a time).
const weldState = {
  headPos: 0, pingDir: 1, seekEnd: false, hasTime: false,
  sparks: [], rng: 0x1234567
};
for (let i = 0; i < 10; i++) weldState.sparks.push({ x: 0, y: 0, vx: 0, vy: 0, life: 0, lifeMax: 1 });
function weldXorshift32() {
  let x = weldState.rng >>> 0;
  x ^= (x << 13); x >>>= 0; x ^= (x >>> 17); x ^= (x << 5); x >>>= 0;
  weldState.rng = x;
  return x;
}
const weldRandf = () => (weldXorshift32() >>> 8) * (1.0 / 16777216.0);
function weldReset() {
  weldState.headPos = 0; weldState.pingDir = 1; weldState.seekEnd = false; weldState.hasTime = false;
  weldState.sparks.forEach(function(sp) { sp.life = 0; });
}
function weldSpawnSpark(sp, hx, hy, tx, ty) {
  let bx = -tx, by = -ty;
  if (bx === 0 && by === 0) { bx = 0; by = 1; }
  const ang = (weldRandf() * 2 - 1) * 2.2, ca = Math.cos(ang), sa = Math.sin(ang);
  const dx = bx * ca - by * sa, dy = bx * sa + by * ca;
  const speed = 8000 + weldRandf() * 16000;
  sp.x = hx; sp.y = hy; sp.vx = dx * speed; sp.vy = dy * speed;
  sp.lifeMax = Math.max(0.001, (SIM_STORE.weld.spark_life * 0.001) * (0.6 + weldRandf() * 0.7));
  sp.life = sp.lifeMax;
}
const WELD_HEAD = { r: 255, g: 255, b: 255 }, WELD_GLOW = { r: 255, g: 0, b: 0 }, WELD_SPARK_COL = { r: 255, g: 255, b: 0 };
const WELD_TRAIL_VERTS = 24, WELD_HEAD_DWELL = 3;
// renderTorch() -- shared torch/afterglow/spark renderer, ported verbatim
// (same constants) from weld_patterns.cpp regardless of path source.
function renderWeldTorch(path, dtMs) {
  const f = new Frame();
  if (!path || path.nodes.length < 2 || path.pathLen <= 1) return f;
  const nodes = path.nodes, liftS = path.liftS, pathLen = path.pathLen;
  if (weldState.seekEnd) { weldState.headPos = pathLen; weldState.seekEnd = false; }

  const dt = weldState.hasTime ? clamp(dtMs / 1000, 0, 0.1) : 0;
  weldState.hasTime = true;

  const wcfg = SIM_STORE.weld;
  let dirSign = wcfg.direction === 0 ? 1 : wcfg.direction === 1 ? -1 : (weldState.pingDir >= 0 ? 1 : -1);
  const speed = wcfg.speed, glow = Math.max(1, wcfg.glow);
  weldState.headPos += speed * dirSign * dt;

  if (wcfg.direction === 2) {
    if (weldState.headPos > pathLen) { weldState.headPos = pathLen; weldState.pingDir = -1; dirSign = -1; }
    else if (weldState.headPos < 0) { weldState.headPos = 0; weldState.pingDir = 1; dirSign = 1; }
  } else if (dirSign > 0 && weldState.headPos > pathLen + glow) {
    weldState.headPos -= pathLen + glow;
  } else if (dirSign < 0 && weldState.headPos < -glow) {
    weldState.headPos += pathLen + glow;
  }

  const onPath = weldState.headPos >= 0 && weldState.headPos <= pathLen;
  const sh = sampleAtJs(nodes, pathLen, weldState.headPos);
  const sb = sampleAtJs(nodes, pathLen, weldState.headPos - dirSign * 200);
  let tx = 0, ty = 0;
  if (sh && sb) {
    const dx = sh.x - sb.x, dy = sh.y - sb.y, m = Math.hypot(dx, dy);
    if (m > 1e-3) { tx = dx / m; ty = dy / m; }
  }

  const decay = Math.max(0, 1 - 1.8 * dt);
  const sparkCount = clamp(wcfg.sparks, 0, 10);
  for (let i = 0; i < 10; i++) {
    const sp = weldState.sparks[i];
    if (i >= sparkCount) { sp.life = 0; continue; }
    if (sp.life > 0) {
      sp.vx *= decay; sp.vy = sp.vy * decay - 55000 * dt;
      sp.x += sp.vx * dt; sp.y += sp.vy * dt; sp.life -= dt;
      if (Math.abs(sp.x) > 32000 || Math.abs(sp.y) > 32000) sp.life = 0;
    }
    if (sp.life <= 0 && onPath && sh) weldSpawnSpark(sp, sh.x, sh.y, tx, ty);
  }

  // Trail: sample head-first, emit tail-first (draw order ends on the head).
  const sampS = new Array(WELD_TRAIL_VERTS), samp = new Array(WELD_TRAIL_VERTS);
  for (let k = 0; k < WELD_TRAIL_VERTS; k++) {
    const u = k / (WELD_TRAIL_VERTS - 1), spos = weldState.headPos - dirSign * u * glow;
    sampS[k] = spos; samp[k] = sampleAtJs(nodes, pathLen, spos);
  }
  let run = [], prevValidK = -1, headWritten = false;
  for (let k = WELD_TRAIL_VERTS - 1; k >= 0; k--) {
    const valid = !!samp[k];
    const cross = valid && prevValidK >= 0 && crossesLiftJs(liftS, sampS[prevValidK], sampS[k]);
    if (!valid || cross) { if (run.length >= 2) f.seg(run, false); run = []; }
    if (!valid) { prevValidK = -1; continue; }
    const u = k / (WELD_TRAIL_VERTS - 1), inv = 1 - u;
    run.push(V(samp[k].x, samp[k].y,
                b255((WELD_HEAD.r * inv + WELD_GLOW.r * u) * inv),
                b255((WELD_HEAD.g * inv + WELD_GLOW.g * u) * inv),
                b255((WELD_HEAD.b * inv + WELD_GLOW.b * u) * inv)));
    prevValidK = k;
    if (k === 0) headWritten = true;
  }
  if (headWritten && run.length >= 1) {
    const last = run[run.length - 1];
    for (let d = 0; d < WELD_HEAD_DWELL; d++) run.push(V(last.x, last.y, last.r, last.g, last.b));
  }
  if (run.length >= 2) f.seg(run, false);

  // Spark streaks: 2-vertex lines, age-faded toward the glow color.
  for (let i = 0; i < sparkCount; i++) {
    const sp = weldState.sparks[i];
    if (sp.life <= 0) continue;
    const frac = clamp(sp.life / sp.lifeMax, 0, 1), ageT = 1 - frac;
    const r = b255((WELD_SPARK_COL.r * (1 - ageT) + WELD_GLOW.r * ageT) * frac);
    const g = b255((WELD_SPARK_COL.g * (1 - ageT) + WELD_GLOW.g * ageT) * frac);
    const b = b255((WELD_SPARK_COL.b * (1 - ageT) + WELD_GLOW.b * ageT) * frac);
    f.seg([V(sp.x - sp.vx * 0.035, sp.y - sp.vy * 0.035, r, g, b), V(sp.x, sp.y, r, g, b)], false);
  }
  return f;
}

/* ── /api/config mock -------------------------------------------- */

// The 8 optimizer profiles as the firmware serializes them: the tuned
// per-profile defaults (OPT_PROFILE_DEFAULTS in include/config.h) plus the
// generic OPT_DEFAULT_* values for every field that table does not cover.
const OPT_PROFILE_EXTRA = {
  resample_enabled: false, resample_spacing_units: 160,
  curvature_resample_enabled: false, curvature_gain: 2.0,
  min_spacing_units: 40, max_spacing_units: 400,
  ringing_comp_enabled: false, ring_freq_hz: 200, ring_damping_ratio: 0.15,
  jitter_enabled: false, jitter_amount_units: 80,
  vel_clamp_enabled: false, max_step_units: 200,
  accel_clamp_enabled: false, max_accel_units: 800,
  reorder_segments: false, reorder_2opt: false
};
const OPT_PROFILE_STAGE1 = [12, 12, 12, 10, 10, 8, 12, 7];
const OPT_PROFILE_BLPPU  = [8.0, 8.0, 8.0, 0.8, 1.5, 0.9, 8.0, 1.0];
const OPT_PROFILE_MINIP  = [8, 8, 8, 6, 6, 4, 8, 1];

const SIM_PROJECTION = {
  rated_kpps: 15, kpps: 30, scan_angle_mech: 30, exit_angle: 45,
  ilda_test_angle: 20, power_r_mw: 500, power_g_mw: 100, power_b_mw: 300,
  active_preset: -1, scan_warp_enabled: false, warp_grid: []
};

// optimizer::ppsRatio() / applyPpsScaling() -- the effective values the
// Optimizer tab shows next to each requested one.
function ppsRatio(rated, out) {
  if (!rated || !out) return 1.0;
  return clamp(rated / out, 0.1, 10.0);
}

// optimizer::ringingStatus(): whether ZV shaping actually engages at these
// settings. shift_pts is the impulse delay in points; the shaped tail must
// fit inside kMaxBlankPts (128) at the LONGEST jump this config builds.
function ringingStatusOf(p, kpps) {
  if (!p.ringing_comp_enabled || p.ring_freq_hz <= 1 || !kpps) {
    return { active: false, shift_pts: 0 };
  }
  const zeta = clamp(p.ring_damping_ratio, 0, 0.9);
  const wn = TAU * p.ring_freq_hz;
  const wd = Math.sqrt(Math.max(1 - zeta * zeta, 1e-6));
  const K = Math.exp(-zeta * PI / wd);
  const A2 = K / (1 + K);
  let shift = Math.max(1, Math.round((PI / (wn * wd)) / (1 / (kpps * 1000))));
  const hi = Math.min(Math.max(p.blank_samples, p.min_blank_samples), 128);
  const settle = clamp(Math.min(p.min_blank_samples, Math.floor(hi / 2)), 1, hi);
  const need = (hi - settle) + shift + 1;
  return { active: A2 > 0 && need <= 128, shift_pts: shift };
}

function buildOptProfile(i) {
  const base = OPT_PROFILES[i];
  const p = {
    opt_corner_angle_deg: base.cornerAngle,
    opt_min_corner_pts: base.cornerMin,
    opt_max_corner_pts: base.cornerMax,
    opt_pts_per_1000_units: base.pts,
    opt_blank_samples: base.blank,
    opt_max_pts_per_frame: base.maxPts,
    opt_min_blank_samples: base.minBlank,
    opt_blank_pts_per_1000_units: OPT_PROFILE_BLPPU[i],
    opt_min_interior_pts_per_segment: OPT_PROFILE_MINIP[i],
    opt_stage1_blank_target: OPT_PROFILE_STAGE1[i]
  };
  for (const k in OPT_PROFILE_EXTRA) p['opt_' + k] = OPT_PROFILE_EXTRA[k];

  const r = ppsRatio(SIM_PROJECTION.rated_kpps, SIM_PROJECTION.kpps);
  p.opt_eff_pts_per_1000_units       = base.pts / r;
  p.opt_eff_resample_spacing_units   = OPT_PROFILE_EXTRA.resample_spacing_units * r;
  p.opt_eff_blank_pts_per_1000_units = OPT_PROFILE_BLPPU[i] / r;
  p.opt_eff_max_step_units           = OPT_PROFILE_EXTRA.max_step_units * r;
  p.opt_eff_max_accel_units          = OPT_PROFILE_EXTRA.max_accel_units * r * r;
  const rs = ringingStatusOf({
    ringing_comp_enabled: OPT_PROFILE_EXTRA.ringing_comp_enabled,
    ring_freq_hz: OPT_PROFILE_EXTRA.ring_freq_hz,
    ring_damping_ratio: OPT_PROFILE_EXTRA.ring_damping_ratio,
    blank_samples: base.blank, min_blank_samples: base.minBlank
  }, SIM_PROJECTION.kpps);
  p.opt_eff_ringing_active = rs.active;
  p.opt_eff_ring_shift_pts = rs.shift_pts;
  return p;
}

function buildConfigResponse() {
  const profiles = OPT_PROFILES.map(function(_, i) { return buildOptProfile(i); });
  const active = profiles[SIM_STORE.activeProfile];
  const cfg = {
    dmx_address: 1, artnet_universe: 0, bpm_manual: 120, bpm_dmx_channel: 1,
    osc_enabled: false, sacn_enabled: false, helios_net_enabled: false,
    artnet_enabled: false, etherdream_enabled: false,
    debug_log_dmx: false, debug_log_artnet: false, debug_log_etherdream: false,
    debug_log_helios_net: false, debug_log_osc: false, debug_log_sacn: false,
    galvo_x_offset: 0, galvo_y_offset: 0, galvo_x_gain: 24200, galvo_y_gain: 24200,
    swap_xy: false, invert_x: false, invert_y: false,
    gain_r: 255, gain_g: 255, gain_b: 255,
    thresh_r: 0, thresh_g: 0, thresh_b: 0,
    hostname: 'galvos-sim', ntp_server: '', ntp_tz: '', ntp_synced: false,
    wifi_ssid: '', wifi_static: false, wifi_ip: '', wifi_gw: '', wifi_mask: '',
    wifi_dns: '', wifi_connected: false, wifi_ip_current: 'simulator',
    dac_debug_log: false,
    wifi_watchdog_reboot_enabled: true,
    wifi_watchdog_soft_timeout_ms: 30000,
    wifi_watchdog_timeout_ms: 300000,
    gw_watchdog_age_ms: 0, gw_watchdog_soft_recoveries: 0,
    dac_limit_min: 0x2E18, dac_limit_max: 0xD28C, output_scale: 0.91,
    gamma_enable: true,
    galvo_rated_kpps: SIM_PROJECTION.rated_kpps,
    safety_override: false,
    ota_pass: SIM_MOCK_STATE.ota_pass,
    opt_active_profile: SIM_STORE.activeProfile,
    opt_profiles: profiles,
    opt_profile_members: PROFILE_MEMBER_NAMES
  };
  // Top-level opt_* mirror the active profile, same as the firmware's
  // backwards-compat block.
  for (const k in active) if (k.indexOf('opt_eff_') !== 0) cfg[k] = active[k];
  return cfg;
}

/* ── Live optimizer telemetry ------------------------------------ */
// Measured off the frame the sim canvas actually drew, so /api/optimizer-stats
// reports this simulation's own numbers rather than invented ones.
const SIM_OPT_STATS = {
  emitted_lit: 0, emitted_blank: 0, truncated: 0, planned_total: 0,
  jump_count: 0, jump_distance_total: 0, calls: 1,
  stage2_scale: 1.0, stage1_triggered: false, stage15_triggered: false,
  ringing_active: false
};

function updateOptStats(pts, cfg) {
  let lit = 0, blank = 0, jumps = 0, dist = 0, inJump = false;
  let jx = 0, jy = 0;
  for (let i = 0; i < pts.length; i++) {
    const p = pts[i];
    if (p.blank) {
      blank++;
      if (!inJump) { inJump = true; jumps++; jx = i > 0 ? pts[i - 1].x : p.x; jy = i > 0 ? pts[i - 1].y : p.y; }
    } else {
      if (inJump) { dist += Math.hypot(p.x - jx, p.y - jy); inJump = false; }
      lit++;
    }
  }
  SIM_OPT_STATS.emitted_lit = lit;
  SIM_OPT_STATS.emitted_blank = blank;
  SIM_OPT_STATS.planned_total = lit + blank;
  SIM_OPT_STATS.truncated = 0;
  SIM_OPT_STATS.jump_count = jumps;
  SIM_OPT_STATS.jump_distance_total = Math.round(dist);
  SIM_OPT_STATS.stage2_scale = simDensityScale;
  SIM_OPT_STATS.stage1_triggered = simDensityScale < 0.999;
  SIM_OPT_STATS.stage15_triggered = (lit + blank) >= cfg.maxPts;
  SIM_MOCK_STATE.frame_lit = lit;
  SIM_MOCK_STATE.frame_blank = blank;
}

/* ── Demo log entries -------------------------------------------- */
(function() {
  var _base = Date.now() - 120000;
  var _entries = [
    {ts: _base +   0, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: '=== GalvOS Laser FW ' + FW_VERSION + ' === ESP32-S3 N16R8'},
    {ts: _base +  80, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: 'LittleFS mounted OK (2.1 MB free)'},
    {ts: _base + 150, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: 'PSRAM detected: 8 MB'},
    {ts: _base + 210, wall: '', lvl: 'INFO',  cat: 'WIFI',   msg: 'AP mode started — SSID: GalvOS-SIM'},
    {ts: _base + 290, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: 'DAC8562 init OK — SPI2 @ 20 MHz'},
    {ts: _base + 370, wall: '', lvl: 'INFO',  cat: 'GALVO',  msg: 'Galvo driver ready — rated 15 kpps, output 30 kpps'},
    {ts: _base + 450, wall: '', lvl: 'INFO',  cat: 'TEMP',   msg: 'DS18B20 bus scan: 5 sensors found'},
    {ts: _base + 510, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: 'Optimizer pipeline init — 8 profiles, Vector active'},
    {ts: _base + 600, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: 'WebUI ready — UI ' + SIM_UI_VERSION + ', heap 285 kB free'},
    {ts: _base + 700, wall: '', lvl: 'INFO',  cat: 'SAFETY', msg: 'E-Stop OK — watchdog armed'},
    {ts: _base + 780, wall: '', lvl: 'INFO',  cat: 'WIFI',   msg: 'Gateway watchdog armed — soft 30 s, hard 300 s'},
    {ts: _base + 800, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: 'Preset #0 (Circle) activated'},
    {ts: _base +2100, wall: '', lvl: 'WARN',  cat: 'TEMP',   msg: 'Sensor 2 (PSU) approaching WARN threshold (43.1 °C)'},
    {ts: _base +4500, wall: '', lvl: 'INFO',  cat: 'USER',   msg: 'Preset changed → #16 (Lissajous 1:2), profile Smooth'},
    {ts: _base +6200, wall: '', lvl: 'DEBUG', cat: 'GALVO',  msg: 'Frame 1024: lit=890 blank=120 fps=25'},
    {ts: _base +7800, wall: '', lvl: 'INFO',  cat: 'DMX',    msg: 'DMX frame received — addr=1 universe=0'},
    {ts: _base+10500, wall: '', lvl: 'WARN',  cat: 'SYS',    msg: 'Heap low event: free=241 kB (min_ever)'},
    {ts: _base+14000, wall: '', lvl: 'INFO',  cat: 'USER',   msg: 'Master dimmer set to 180'},
    {ts: _base+18000, wall: '', lvl: 'INFO',  cat: 'SAFETY', msg: 'Scan-fail detector: OK'},
    {ts: _base+25000, wall: '', lvl: 'ERROR', cat: 'WIFI',   msg: 'NTP sync failed — no upstream (AP mode)'},
    {ts: _base+32000, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: 'BPM tap: 128.0 BPM (source=tap)'},
    {ts: _base+45000, wall: '', lvl: 'DEBUG', cat: 'GALVO',  msg: 'ZV shaper inactive at 200 Hz / 30 kpps (shift 75 pts)'},
    {ts: _base+58000, wall: '', lvl: 'INFO',  cat: 'TEMP',   msg: 'Fan 1 auto: 47% (Galvo X 51.7 °C)'},
    {ts: _base+72000, wall: '', lvl: 'INFO',  cat: 'USER',   msg: 'Color override enabled — R=0 G=255 B=136'},
    {ts: _base+89000, wall: '', lvl: 'INFO',  cat: 'SYS',    msg: 'Sequencer started — 4 steps, loop=true'},
    {ts: _base+105000,wall: '', lvl: 'DEBUG', cat: 'DMX',    msg: 'BPM DMX ch=1 val=153 → 120.0 BPM'},
    {ts: _base+118000,wall: '', lvl: 'INFO',  cat: 'SAFETY', msg: 'All systems nominal — uptime 120 s'}
  ];
  _entries.forEach(function(e) {
    var d = new Date(e.ts);
    e.wall = d.toTimeString().substr(0, 8) + '.' + String(d.getMilliseconds()).padStart(3, '0');
  });
  window.DEMO_LOG_ENTRIES = _entries;
})();

/* ── Mock fetch -------------------------------------------------- */
// Every /api/* request the WebUI makes is answered here; anything without an
// explicit branch falls through to {ok:true}, which is what the real firmware
// returns for the plain "apply this" POSTs.
const _simOrigFetch = window.fetch.bind(window);
window.fetch = async function(url, opts) {
  const urlStr = typeof url === 'string' ? url : String(url && url.url || url);
  if (!urlStr.startsWith('/api/')) return _simOrigFetch(url, opts);

  const method = (opts && opts.method || 'GET').toUpperCase();
  const path = urlStr.split('?')[0];
  let body = {};
  if (opts && opts.body) {
    try { body = JSON.parse(opts.body); } catch (e) { body = {}; }
  }

  let resp = {ok: true};

  if (path === '/api/state') {
    SIM_MOCK_STATE.uptime_s = Math.floor((performance.now() - SIM_START_MS) / 1000);
    SIM_MOCK_STATE.frame_n = simPhase;
    SIM_MOCK_STATE.preset_idx = simUI.preset;
    SIM_MOCK_STATE.ui_master_dimmer = Math.round(simUI.dimmer);
    SIM_MOCK_STATE.cpu0 = 30 + Math.round(Math.sin(Date.now() / 5000) * 10 + Math.random() * 8);
    SIM_MOCK_STATE.cpu1 = 6 + Math.round(Math.random() * 5);
    SIM_MOCK_STATE.starfield_stars = live.starfieldCount;
    var _t = Date.now();
    SIM_MOCK_STATE.temps = [
      parseFloat((42.3 + Math.sin(_t / 8000) * 2.5  + Math.random() * 0.3).toFixed(1)),
      parseFloat((38.1 + Math.sin(_t / 11000) * 1.8 + Math.random() * 0.2).toFixed(1)),
      parseFloat((51.7 + Math.sin(_t / 6000) * 3.2  + Math.random() * 0.4).toFixed(1)),
      parseFloat((35.4 + Math.sin(_t / 9500) * 1.5  + Math.random() * 0.2).toFixed(1)),
      parseFloat((44.9 + Math.sin(_t / 7000) * 2.8  + Math.random() * 0.35).toFixed(1))
    ];
    SIM_MOCK_STATE.temp_raw = SIM_MOCK_STATE.temps.slice();
    resp = Object.assign({}, SIM_MOCK_STATE);

  } else if (path === '/api/presets') {
    resp = PRESETS.map(function(p, i) { return {idx: i, name: p[0], cat: p[1]}; });

  } else if (path === '/api/projection') {
    resp = Object.assign({}, SIM_PROJECTION);

  } else if (path === '/api/config') {
    resp = buildConfigResponse();

  } else if (path === '/api/optimizer-stats') {
    resp = {last: Object.assign({}, SIM_OPT_STATS), frame: Object.assign({}, SIM_OPT_STATS)};

  } else if (path === '/api/optimizer-profile-switch' && method === 'POST') {
    if (body.profile !== undefined) {
      const pr = parseInt(body.profile, 10);
      if (pr >= 0 && pr < OPT_PROFILES.length) SIM_STORE.activeProfile = pr;
    }

  } else if (path === '/api/seg_colors') {
    if (method === 'POST') {
      if (body.enabled !== undefined) live.segColors = !!body.enabled;
      if (body.r && body.g && body.b) {
        live.segR = body.r.slice(0, 10);
        live.segG = body.g.slice(0, 10);
        live.segB = body.b.slice(0, 10);
      }
    }
    resp = {enabled: live.segColors, r: live.segR, g: live.segG, b: live.segB};

  } else if (path === '/api/weld' || path === '/api/weld/set') {
    if (method === 'POST') {
      // Mirror web_ui.cpp's /api/weld handler: enabled false->true starts a
      // fresh run, and a direction POST landing on Reverse seeks the head to
      // the far end (both fire on assignment order, not a value comparison,
      // so read the "was it already true" flag before Object.assign()).
      const wasEnabled = SIM_STORE.weld.enabled;
      Object.assign(SIM_STORE.weld, body);
      if (body.enabled && !wasEnabled) weldReset();
      if (body.direction === 1) weldState.seekEnd = true;
    }
    resp = Object.assign({}, SIM_STORE.weld);

  } else if (path === '/api/warp/get') {
    resp = {enabled: SIM_STORE.warp.enabled, gridSize: SIM_STORE.warp.gridSize,
            points: SIM_STORE.warp.points};
  } else if (path === '/api/warp/set') {
    if (body.gridSize !== undefined && body.gridSize !== SIM_STORE.warp.gridSize && !body.points) {
      SIM_STORE.warp.points = identityWarp(parseInt(body.gridSize, 10));
    }
    if (body.gridSize !== undefined) SIM_STORE.warp.gridSize = parseInt(body.gridSize, 10);
    if (body.points) SIM_STORE.warp.points = body.points;
    if (body.enabled !== undefined) SIM_STORE.warp.enabled = !!body.enabled;
    SIM_PROJECTION.scan_warp_enabled = SIM_STORE.warp.enabled;
  } else if (path === '/api/warp/reset') {
    SIM_STORE.warp.points = identityWarp(SIM_STORE.warp.gridSize);
  } else if (path === '/api/warp/test') {
    SIM_STORE.warp.test = !!body.enabled;

  } else if (path === '/api/brightness/get') {
    resp = {enabled: SIM_STORE.brightness.enabled, gridSize: SIM_STORE.brightness.gridSize,
            gain: SIM_STORE.brightness.gain};
  } else if (path === '/api/brightness/set') {
    if (body.gridSize !== undefined && body.gridSize !== SIM_STORE.brightness.gridSize && !body.gain) {
      SIM_STORE.brightness.gain = identityGain(parseInt(body.gridSize, 10));
    }
    if (body.gridSize !== undefined) SIM_STORE.brightness.gridSize = parseInt(body.gridSize, 10);
    if (body.gain) SIM_STORE.brightness.gain = body.gain;
    if (body.enabled !== undefined) SIM_STORE.brightness.enabled = !!body.enabled;
  } else if (path === '/api/brightness/reset') {
    SIM_STORE.brightness.gain = identityGain(SIM_STORE.brightness.gridSize);

  } else if (path === '/api/zone') {
    resp = {enabled: SIM_STORE.zone.enabled, count: SIM_STORE.zone.x.length,
            x: SIM_STORE.zone.x, y: SIM_STORE.zone.y};
  } else if (path === '/api/zone/enable') {
    SIM_STORE.zone.enabled = !!body.enabled;

  } else if (path === '/api/text') {
    if (method === 'POST') {
      // setText(true) (web_ui.cpp) clears gPaint.active on activation --
      // Text outranks Paint in pattern_engine.cpp's source priority -- and
      // resets the Welding head/sparks on a false->true activation edge.
      const wasActive = SIM_STORE.text.active;
      Object.assign(SIM_STORE.text, body);
      if (body.active && !wasActive) { SIM_STORE.paint.active = false; weldReset(); }
    }
    resp = Object.assign({}, SIM_STORE.text);
  } else if (path === '/api/text/vertices') {
    const q = new URL(urlStr, 'http://sim').searchParams;
    const paths = glyphOutlinePathsJs(q.get('text') || '', sizeToScaleJs(clamp(parseInt(q.get('size'), 10) || 128, 0, 255)));
    resp = {paths: paths, truncated: false};
  } else if (path === '/api/text/off') {
    SIM_STORE.text.active = false;

  } else if (path === '/api/paint') {
    resp = Object.assign({}, SIM_STORE.paint);
  } else if (path === '/api/paint/set') {
    // setPaintActive(true) (pattern_engine.cpp) clears gTextConfig.active and
    // resets Welding on a false->true activation edge -- same pair as /api/text above.
    const wasActive = SIM_STORE.paint.active;
    if (body.strokes) SIM_STORE.paint.strokes = body.strokes;
    if (body.active !== undefined) SIM_STORE.paint.active = !!body.active;
    if (SIM_STORE.paint.active && !wasActive) { SIM_STORE.text.active = false; weldReset(); }
  } else if (path === '/api/paint/clear') {
    SIM_STORE.paint.strokes = [];
  } else if (path === '/api/paint/off') {
    SIM_STORE.paint.active = false;

  } else if (path === '/api/timer/state') {
    resp = Object.assign({}, SIM_STORE.timer);
  } else if (path === '/api/timer/set') {
    if (body.seconds !== undefined) SIM_STORE.timer.remaining = parseInt(body.seconds, 10);
  } else if (path === '/api/timer/start') {
    SIM_STORE.timer.running = true;
  } else if (path === '/api/timer/stop' || path === '/api/timer/pause') {
    SIM_STORE.timer.running = false;
  } else if (path === '/api/timer/reset') {
    SIM_STORE.timer.running = false; SIM_STORE.timer.expired = false;

  } else if (path === '/api/sequencer') {
    resp = {running: false, loop: true, currentStep: 0, stepCount: 0, steps: []};

  } else if (path === '/api/modulators/meta') {
    resp = SIM_MOD_META;
  } else if (path === '/api/modulators') {
    resp = {modulators: SIM_MODULATORS, bindings: SIM_BINDINGS};
  } else if (path === '/api/modulators/bindings') {
    resp = SIM_BINDINGS;
  } else if (path === '/api/modulators/reset') {
    buildModulatorSlots();
    resp = {ok: true};

  } else if (path === '/api/community/list') {
    resp = [];
  } else if (path === '/api/community/fs-info') {
    resp = {total: 1048576, used: 0, free: 1048576, count: 0};

  } else if (path === '/api/log/stats') {
    resp = {count: DEMO_LOG_ENTRIES.length, capacity: 1000, full: false};
  } else if (path === '/api/log/client' || path === '/api/log/clear') {
    resp = {ok: true};
  } else if (path.startsWith('/api/log')) {
    var afterTs = parseInt((urlStr.match(/after=(\d+)/) || [0, 0])[1]) || 0;
    resp = DEMO_LOG_ENTRIES.filter(function(e) { return e.ts > afterTs; });

  } else if (path === '/api/meminfo') {
    var mhf = Math.round(285000 + Math.sin(Date.now() / 6000) * 15000);
    var mpf = Math.max(7100000, 7800000 - simPhase * 180);
    resp = {
      heap: {free: mhf, total: 330000, min_ever: 245000},
      psram: {free: mpf, total: 8388608, min_ever: 7050000},
      owners: [
        {name: 'PatternBuf', bytes: 30720, psram: false},
        {name: 'WebUI', bytes: 14336, psram: false},
        {name: 'FrameBuf', bytes: 8388608 - mpf - 49152, psram: true}
      ]
    };

  } else if (path === '/api/wifi-status') {
    resp = {connected: false, ssid: '', ip: '', rssi: 0, ap_clients: 1, mode: 'ap'};
  } else if (path === '/api/wifi-scan') {
    resp = {networks: []};

  } else if (path === '/api/sd/info') {
    resp = {ready: false, fs_type: '', total_kb: 0, free_kb: 0, used_kb: 0,
            used_pct: 0, file_count: 0, error: 'Simulator mode — no SD card'};
  } else if (path.startsWith('/api/sd')) {
    resp = {ready: false, error: 'Simulator mode — no SD card', files: []};

  } else if (path === '/api/svg/list') {
    resp = {ready: false, file_count: 0, max_bytes: 65536, files: []};
  } else if (path.startsWith('/api/svg')) {
    resp = {ok: false, error: 'Simulator mode — no SD card'};

  } else if (path === '/api/ilda/status') {
    resp = {active: false, loading: false, error: 'OK', enabled: false, paused: false,
            file_idx: -1, frame: 0, total: 0, speed: 128, size: 128, loop: true,
            invert_x: false, invert_y: false, col_override: false,
            col_r: 255, col_g: 255, col_b: 255, blank_reshape_enabled: false};
  } else if (path.startsWith('/api/ilda')) {
    resp = {ok: false, error: 'Simulator mode — no SD card'};

  } else if (path === '/api/calib-pattern/list') {
    resp = {
      patterns: SIM_CALIB_PATTERNS.map(function(p, i) {
        return {idx: i, name: p[0], desc: p[1], check: p[2]};
      }),
      active: false, idx: 0, bright: 200, channel: 0
    };
  } else if (path.startsWith('/api/calib')) {
    resp = {active: false, patterns: [], ok: true};

  } else if (path === '/api/arm') {
    resp = {ok: false, error: 'Simulator — no hardware connected, arming refused'};

  } else if (path === '/api/reboot' || path === '/api/factory-reset') {
    resp = {ok: false, error: 'Simulator — nothing to reboot'};

  } else if (path.startsWith('/api/preset') && method === 'POST') {
    if (body.idx !== undefined) {
      simUI.preset = parseInt(body.idx) || 0;
      SIM_MOCK_STATE.preset_idx = simUI.preset;
      // patterns::setPreset() switches the active optimizer profile to the new
      // preset's class (pattern_engine.cpp), so /api/config's
      // opt_active_profile follows the preset here too -- otherwise the
      // Optimizer tab would keep describing whichever profile was last opened.
      SIM_STORE.activeProfile = PRESET_PROFILE[simUI.preset] || 0;
    }
    if (body.speed !== undefined)  simUI.speed  = Number(body.speed);
    if (body.size !== undefined)   simUI.size   = Number(body.size);
    if (body.rotation !== undefined) simUI.rotation = Number(body.rotation);
    if (body.rot_x !== undefined)  simUI.rotX   = !!body.rot_x;
    if (body.rot_y !== undefined)  simUI.rotY   = !!body.rot_y;
    if (body.rot_z !== undefined)  simUI.rotZ   = !!body.rot_z;
    if (body.rot_speed !== undefined) simUI.rotSpeed = Number(body.rot_speed);
    if (body.autoscaleSpeed !== undefined) simUI.autoscaleSpeed = Number(body.autoscaleSpeed);
    if (body.autoscaleMode !== undefined)  simUI.autoscaleMode  = parseInt(body.autoscaleMode);
    if (body.kaleido_enabled !== undefined) simUI.kaleido = !!body.kaleido_enabled;
    if (body.kaleido_mode !== undefined) simUI.kaleidoMode = parseInt(body.kaleido_mode);
    if (body.kaleido_segments !== undefined) simUI.kalSeg = parseInt(body.kaleido_segments);
    if (body.kaleido_mirror_h !== undefined) simUI.kalMH = !!body.kaleido_mirror_h;
    if (body.kaleido_mirror_v !== undefined) simUI.kalMV = !!body.kaleido_mirror_v;
    if (body.mirror_mode !== undefined) simUI.mirrorMode = parseInt(body.mirror_mode);
    if (body.col_override !== undefined) simUI.colOverride = !!body.col_override;
    if (body.col_r !== undefined) simUI.col = [Number(body.col_r), Number(body.col_g || 0), Number(body.col_b || 0)];
    if (body.master_dimmer !== undefined) simUI.dimmer = Number(body.master_dimmer);
    // Points-Only Mode. Enabling it forces the Particles profile regardless of
    // the preset's own class, exactly like web_ui.cpp's /api/preset-live
    // handler does (dwelling dots want no corner dwell and long blank jumps).
    if (body.points_mode_enabled !== undefined) {
      simUI.pointsOnly = !!body.points_mode_enabled;
      if (simUI.pointsOnly) SIM_STORE.activeProfile = OPT_PROFILE_PARTICLES;
      else SIM_STORE.activeProfile = PRESET_PROFILE[simUI.preset] || 0;
    }
    if (body.points_count !== undefined)   simUI.pointsCount = parseInt(body.points_count);
    if (body.points_static_on !== undefined) simUI.pointsStatic = !!body.points_static_on;
    if (body.points_fade_in_on !== undefined)  simUI.fadeInOn  = !!body.points_fade_in_on;
    if (body.points_fade_out_on !== undefined) simUI.fadeOutOn = !!body.points_fade_out_on;
    if (body.points_fade_in_ms !== undefined)  simUI.fadeInMs  = parseInt(body.points_fade_in_ms);
    if (body.points_fade_out_ms !== undefined) simUI.fadeOutMs = parseInt(body.points_fade_out_ms);
    if (body.points_fade_dir !== undefined)    simUI.fadeDir   = parseInt(body.points_fade_dir);
    // preset-specific
    if (body.wave_amp !== undefined)        live.waveAmp         = Number(body.wave_amp);
    if (body.wave_freq !== undefined)       live.waveFreq        = Number(body.wave_freq);
    if (body.hline_bounce !== undefined)    live.hlineBounce     = !!body.hline_bounce;
    if (body.bp_trail_len !== undefined)    live.trail           = parseInt(body.bp_trail_len);
    if (body.spiral_arms !== undefined)     live.spiralArms      = parseInt(body.spiral_arms);
    if (body.tunnel_rings !== undefined)    live.tunnelRings     = parseInt(body.tunnel_rings);
    if (body.tunnel_sides !== undefined)    live.tunnelSides     = parseInt(body.tunnel_sides);
    if (body.explosion_rays !== undefined)  live.explosionRays   = parseInt(body.explosion_rays);
    if (body.fw_max_shells !== undefined)   live.fwShells        = parseInt(body.fw_max_shells);
    if (body.fw_glitter !== undefined)      live.fwGlitter       = !!body.fw_glitter;
    if (body.mw_dots !== undefined)         live.mwDots          = parseInt(body.mw_dots);
    if (body.mw_tilt !== undefined)         live.mwTilt          = parseInt(body.mw_tilt);
    if (body.random_pts_hold_ms !== undefined) live.randomHoldMs = parseInt(body.random_pts_hold_ms);

  } else if (path === '/api/ui-control' && method === 'POST') {
    if (body.master_dimmer !== undefined) simUI.dimmer = Number(body.master_dimmer);
    if (body.ui_override !== undefined) SIM_MOCK_STATE.ui_override = !!body.ui_override;
  }

  await new Promise(function(r) { setTimeout(r, 6 + Math.random() * 8); });
  return new Response(JSON.stringify(resp), {
    status: 200, headers: {'Content-Type': 'application/json'}
  });
};

/* ── Modulator engine mock (modulator_engine.cpp registry) -------- */
const SIM_MOD_META = {
  types: [
    {id: 0, key: 'oscillator', label: 'Oscillator', usesShape: true,  usesEnvelope: false, usesSequencer: false, usesShapeParam: false, supportsTrigger: false},
    {id: 1, key: 'noise',      label: 'Noise',      usesShape: false, usesEnvelope: false, usesSequencer: false, usesShapeParam: false, supportsTrigger: false},
    {id: 2, key: 'envelope',   label: 'Envelope',   usesShape: false, usesEnvelope: true,  usesSequencer: false, usesShapeParam: false, supportsTrigger: true},
    {id: 3, key: 'sequencer',  label: 'Sequencer',  usesShape: false, usesEnvelope: false, usesSequencer: true,  usesShapeParam: false, supportsTrigger: false}
  ],
  shapes: [
    {id: 0, key: 'sine',     label: 'Sine',     supportsShapeParam: false},
    {id: 1, key: 'triangle', label: 'Triangle', supportsShapeParam: true},
    {id: 2, key: 'square',   label: 'Square',   supportsShapeParam: true},
    {id: 3, key: 'saw',      label: 'Saw',      supportsShapeParam: false}
  ],
  targets: [
    {id: 0, key: 'transform_scale_x', label: 'Scale X',       category: 'Transform', min: 0.1,     max: 3.0,   defaultVal: 1.0, step: 0.01, unit: 'x',   description: 'Horizontal scale multiplier', wraps: false},
    {id: 1, key: 'transform_scale_y', label: 'Scale Y',       category: 'Transform', min: 0.1,     max: 3.0,   defaultVal: 1.0, step: 0.01, unit: 'x',   description: 'Vertical scale multiplier', wraps: false},
    {id: 2, key: 'transform_shift_x', label: 'Shift X',       category: 'Transform', min: -20000,  max: 20000, defaultVal: 0,   step: 1,    unit: '',    description: 'Horizontal position offset (DAC units)', wraps: false},
    {id: 3, key: 'transform_shift_y', label: 'Shift Y',       category: 'Transform', min: -20000,  max: 20000, defaultVal: 0,   step: 1,    unit: '',    description: 'Vertical position offset (DAC units)', wraps: false},
    {id: 4, key: 'transform_rotation', label: 'Rotation',     category: 'Transform', min: -3600,   max: 3600,  defaultVal: 0,   step: 1,    unit: 'deg', description: 'Rotation offset in degrees', wraps: false},
    {id: 5, key: 'color_hue',         label: 'Hue',           category: 'Color',     min: 0,       max: 1,     defaultVal: 0,   step: 0.01, unit: '',    description: 'Hue shift (wraps instead of clamping)', wraps: true},
    {id: 6, key: 'color_saturation',  label: 'Saturation',    category: 'Color',     min: 0,       max: 2,     defaultVal: 1,   step: 0.01, unit: 'x',   description: 'Saturation multiplier', wraps: false},
    {id: 7, key: 'color_brightness',  label: 'Brightness',    category: 'Color',     min: 0,       max: 2,     defaultVal: 1,   step: 0.01, unit: 'x',   description: 'Brightness (value) multiplier', wraps: false},
    {id: 8, key: 'opt_speed',         label: 'Speed',         category: 'Optimizer', min: 0,       max: 255,   defaultVal: 127, step: 1,    unit: '',    description: 'Animation speed', wraps: false},
    {id: 9, key: 'opt_density',       label: 'Point Density', category: 'Optimizer', min: 0.1,     max: 5,     defaultVal: 1,   step: 0.01, unit: 'x',   description: 'Optimizer output point density multiplier', wraps: false}
  ],
  curveTypes: ['Linear', 'Ease In', 'Ease Out', 'Exponential', 'Logarithmic', 'S-Curve']
    .map(function(l, i) { return {id: i, label: l}; }),
  loopModes: ['One Shot', 'Loop', 'Ping-Pong', 'Trigger']
    .map(function(l, i) { return {id: i, label: l}; })
};

let SIM_MODULATORS = [], SIM_BINDINGS = [];
function buildModulatorSlots() {
  SIM_MODULATORS = [];
  for (let i = 0; i < 8; i++) {
    SIM_MODULATORS.push({
      idx: i, enabled: false, type: 0, typeName: 'oscillator', shape: 0,
      cycles: 1, phaseOffset: 0, phaseSpeed: 1, level: 1,
      bpmSync: false, bpmDiv: 2, name: 'Mod ' + (i + 1), shapeParam: 0.5,
      envAttackMs: 100, envSustainMs: 200, envReleaseMs: 300,
      envData: {pointCount: 0, loopMode: 1, points: []},
      seqStepCount: 4, seqValues: [0, 0.33, 0.66, 1, 0, 0, 0, 0],
      noiseSeed: 1234 + i, output: 0
    });
  }
  SIM_BINDINGS = [];
  for (let i = 0; i < 16; i++) {
    SIM_BINDINGS.push({idx: i, active: false, modulatorIdx: 0, targetParam: 0, depth: 1, offset: 0});
  }
}
buildModulatorSlots();

// calib_patterns::CALIB_INFO[] (names/descriptions only -- the sim canvas
// renders presets, not the calibration patterns themselves).
const SIM_CALIB_PATTERNS = [
  ['Blanking Test', 'Alternating on/off segments — checks blanking accuracy',
   'Dark segments must be completely dark (no light leakage)'],
  ['Aspect Ratio', 'Square + circle of identical size — checks X/Y gain match',
   'Circle must fit exactly inside the square corners'],
  ['ILDA Test Pattern', 'Official ILDA standard test pattern — galvo alignment & scanner tuning',
   'Circle must be perfectly round and touch inner square at 4 points'],
  ['DAC Range Box', 'Rectangle + circle at exact dac_limit_max boundary — set safe scan range',
   'Raise dac_limit_max until box corners just clip, then back off 5%']
];

/* ── Keep "Simulator" in the page title ── */
// The WebUI's state handler rewrites #page-title to "FW: x - UI:y" on every
// poll, which on a browser tab is indistinguishable from a real device. Re-add
// the marker whenever it does; an observer (not a timer) so it never lags
// behind a poll and costs nothing while the title is stable.
(function() {
  var el = document.getElementById('page-title');
  if (!el || typeof MutationObserver === 'undefined') return;
  const SUFFIX = ' — Simulator';
  function mark() {
    if (el.textContent.indexOf(SUFFIX) < 0) el.textContent = el.textContent + SUFFIX;
  }
  new MutationObserver(mark).observe(el, {childList: true, characterData: true, subtree: true});
  mark();
})();

/* ── Topbar height sync for mobile drawer ── */
(function() {
  var wrap = document.getElementById('topbar-wrap');
  if (!wrap) return;
  function sync() {
    document.documentElement.style.setProperty('--topbar-h', wrap.offsetHeight + 'px');
  }
  sync();
  if (typeof ResizeObserver !== 'undefined') {
    new ResizeObserver(sync).observe(wrap);
  } else {
    window.addEventListener('resize', sync);
  }
})();

/* ── Sim render loop --------------------------------------------- */
// simPhase is the firmware's integer animation phase and must advance by
// wall-clock time / ANIM_FRAME_MS (pattern_engine.cpp's advancePhaseDt), not
// once per requestAnimationFrame -- a 60fps browser would otherwise run every
// preset ~2.4x faster than the device does.
let simPhase = 0, simPhaseAcc = 0, simFrames = 0;
let simLastFpsMs = performance.now(), simLastTickMs = performance.now();
let simDensityScale = 1.0;

// Autoscale (AUTOSCALE_SMALL_BIG_SMALL / SMALL_BIG / BIG_SMALL, config.h) --
// a size multiplier driven by its own phase, disabled at speed 0.
let simAutoPhase = 0;
function autoscaleFactor(dtFrames) {
  if (!simUI.autoscaleSpeed) return 1;
  simAutoPhase = mod(simAutoPhase + simUI.autoscaleSpeed / 400 * dtFrames, 1);
  if (simUI.autoscaleMode === 1) return 0.25 + simAutoPhase * 0.75;              // Grow
  if (simUI.autoscaleMode === 2) return 1.0 - simAutoPhase * 0.75;               // Shrink
  const tri = simAutoPhase < 0.5 ? simAutoPhase * 2 : 2 - simAutoPhase * 2;      // Pulse
  return 0.25 + tri * 0.75;
}

function simTick() {
  if (canvas && ctx) {
    const now = performance.now();
    let dtMs = now - simLastTickMs;
    simLastTickMs = now;
    if (dtMs < 0) dtMs = 0;
    if (dtMs > 240) dtMs = 240;                       // cap after a tab-switch stall
    const dtFrames = dtMs / ANIM_FRAME_MS;
    simPhaseAcc += dtFrames;
    if (simPhaseAcc >= 1) { simPhase += Math.floor(simPhaseAcc); simPhaseAcc %= 1; }

    // Render with the profile the device would have active -- normally the
    // preset's own class, but Points-Only Mode forces Particles, and an
    // explicit switch from the Optimizer tab wins until the next preset change
    // (SIM_STORE.activeProfile tracks all three, like gActiveOptimizerProfile).
    optCfg = OPT_PROFILES[SIM_STORE.activeProfile] || profileForPreset(simUI.preset);

    // Source priority mirrors pattern_engine.cpp's task() loop: Text Mode
    // outranks Paint Mode outranks the Preset engine. Welding is not a
    // fourth source -- it's an alternate renderer swapped in under whichever
    // of Text/Paint is currently active (weld_patterns.h).
    var f, source;
    var textOn = SIM_STORE.text.active && SIM_STORE.text.text;
    var paintOn = !textOn && SIM_STORE.paint.active && SIM_STORE.paint.strokes.length;
    if (textOn) {
      source = 'text';
      f = SIM_STORE.weld.enabled ? renderWeldTorch(weldPathFromText(SIM_STORE.text), dtMs)
                                  : buildTextFrame(SIM_STORE.text, simPhase);
    } else if (paintOn) {
      source = 'paint';
      f = SIM_STORE.weld.enabled ? renderWeldTorch(weldPathFromPaint(SIM_STORE.paint.strokes), dtMs)
                                  : buildPaintFrame(SIM_STORE.paint.strokes);
    } else {
      source = 'preset';
      f = new Frame();
      var gen = GEN[simUI.preset] || GEN[0];
      var sizeVal = clamp(Math.round(simUI.size * autoscaleFactor(dtFrames)), 0, 255);
      try { gen(f, simPhase, Math.round(simUI.speed), sizeVal); } catch (e) {}
    }
    var pts = optimize(f, optCfg);
    simDensityScale = simLastDensityScale;

    if (simUI.rotZ) simUI.rotAngleZ += simUI.rotSpeed * dtFrames;
    if (simUI.rotY) simUI.rotAngleY += simUI.rotSpeed * dtFrames;
    if (simUI.rotX) simUI.rotAngleX += simUI.rotSpeed * dtFrames;
    // Text Mode never runs through gLivePreset's 3-axis rotation or the
    // kaleido/mirror/points-only post-passes in pattern_engine.cpp -- both
    // stages are Preset-only there. Paint Mode shares the rotation engine
    // (same gLivePreset.rot_* state) but not the kaleido/mirror/points-only
    // stages, which only run in the Preset branch further down the loop.
    if (source !== 'text') applyRotations(pts, simUI);

    if (source === 'preset') {
      if (simUI.kaleido) {
        pts = simUI.kaleidoMode === 0
          ? radialCopy(pts, simUI.kalSeg, simUI.kalMH, simUI.kalMV, optCfg)
          : mirrorKaleido(pts, simUI.kalSeg, optCfg);
      }
      if (simUI.mirrorMode === 1) pts = mirrorCopy(pts, false, true, optCfg);
      else if (simUI.mirrorMode === 2) pts = mirrorCopy(pts, true, false, optCfg);
      else if (simUI.mirrorMode === 3) pts = radial4(pts, optCfg);

      if (simUI.pointsOnly) pts = pointsOnlyMode(pts, simUI, optCfg, dtMs);
    }

    applyColor(pts, simUI);
    render(pts, simUI);
    updateOptStats(pts, optCfg);

    // Frame rate is a property of the SIMULATED device, not of the browser:
    // the galvo consumes points at a fixed rate, so fps = points/sec divided
    // by the points this frame cost -- exactly what galvo::fps() reports. The
    // browser's own rAF rate is irrelevant to the user and would contradict
    // the Dashboard's own numbers if shown here instead.
    var pps = SIM_PROJECTION.kpps * 1000;
    var devFps = Math.max(1, Math.round(pps / Math.max(1, pts.length)));
    SIM_MOCK_STATE.fps = devFps;
    SIM_MOCK_STATE.points_per_sec = pps;
    var ptsEl = document.getElementById('sim-pts');
    if (ptsEl) ptsEl.textContent = pts.length + ' pts';
    simFrames++;
    if (now - simLastFpsMs >= 1000) {
      var fpsEl = document.getElementById('sim-fps');
      if (fpsEl) fpsEl.textContent = devFps + ' fps';
      simFrames = 0; simLastFpsMs = now;
    }
  }
  requestAnimationFrame(simTick);
}
requestAnimationFrame(simTick);

// Debug/inspection handle -- the only sim symbol on the global object besides
// the patched fetch and DEMO_LOG_ENTRIES.
window.GALVOS_SIM = {
  SIM_VERSION: SIM_VERSION, FW_VERSION: FW_VERSION, UI_VERSION: SIM_UI_VERSION,
  FW_COMMIT: FW_COMMIT,
  presets: PRESETS, profiles: OPT_PROFILES, presetProfile: PRESET_PROFILE,
  ui: simUI, live: live, store: SIM_STORE, stats: SIM_OPT_STATS,
  config: buildConfigResponse
};

})();
