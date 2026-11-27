/* Offline, synthetic survey and well picks for the workspace design prototype. */
(() => {
  'use strict';
  const W = 1000,
    H = 704,
    CELL = 2,
    COLS = W / CELL,
    ROWS = H / CELL;
  const east0 = 462000,
    north0 = 2468000,
    metres = 18;
  const home = { x: W / 2, y: H / 2, z: 1 };
  const facies = [
    { key: 'channel', name: '水下分流河道', color: '#DCAF69' },
    { key: 'bar', name: '河口坝', color: '#E9B595' },
    { key: 'sand', name: '席状砂', color: '#DBDDA9' },
    { key: 'bay', name: '水下分流间湾', color: '#ABD0C0' },
    { key: 'unassigned', name: '待赋相', color: '#D5D9D5' }
  ].map((row) => ({ ...row, id: row.key }));
  const symbols = new Map(facies.map((f) => [f.key, f]));
  const clusters = facies.map((f, i) => {
    // Same HSV formula as the desktop raw-cluster renderer.
    const h = (((i + 1) * 137) % 360) / 60,
      v = 211 / 255,
      c = (v * 156) / 255,
      m = v - c;
    const a = c * (1 - Math.abs((h % 2) - 1));
    const rgb = [
      [c, a, 0],
      [a, c, 0],
      [0, c, a],
      [0, a, c],
      [a, 0, c],
      [c, 0, a]
    ][Math.floor(h)];
    return {
      id: i + 1,
      name: '簇 ' + (i + 1),
      color:
        '#' +
        rgb
          .map((n) =>
            Math.round((n + m) * 255)
              .toString(16)
              .padStart(2, '0')
          )
          .join(''),
      facies: f.key,
      votes: {},
      wells: [],
      patches: []
    };
  });
  const survey = [
    [40, 147],
    [416, 64],
    [427, 78],
    [619, 37],
    [638, 51],
    [834, 93],
    [847, 114],
    [911, 126],
    [954, 301],
    [936, 307],
    [962, 416],
    [918, 428],
    [944, 541],
    [650, 604],
    [642, 590],
    [371, 649],
    [361, 632],
    [201, 667],
    [181, 630],
    [151, 637],
    [115, 491],
    [91, 496],
    [52, 338],
    [76, 331],
    [39, 184],
    [49, 181]
  ];
  function inside(x, y) {
    let ok = false;
    for (let i = 0, j = survey.length - 1; i < survey.length; j = i++) {
      const a = survey[i],
        b = survey[j];
      if (a[1] > y !== b[1] > y && x < ((b[0] - a[0]) * (y - a[1])) / (b[1] - a[1]) + a[0]) ok = !ok;
    }
    return ok;
  }
  const boundary = new Path2D();
  survey.forEach(([x, y], i) => (i ? boundary.lineTo(x, y) : boundary.moveTo(x, y)));
  boundary.closePath();
  function wiggle(x, y) {
    return (
      Math.sin(x * 0.031 + y * 0.013) * 0.47 +
      Math.sin(y * 0.044 - x * 0.017) * 0.3 +
      Math.sin(x * 0.067 + y * 0.048) * 0.15 +
      Math.sin(y * 0.102 - x * 0.087) * 0.08
    );
  }
  function spline(a, b, c, d, t) {
    return (
      0.5 *
      (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t * t + (-a + 3 * b - 3 * c + d) * t * t * t)
    );
  }
  const reach = new Float32Array(COLS * ROWS).fill(200);
  function channel(spine, width, endWidth, phase) {
    const line = [];
    for (let i = 0; i < spine.length - 1; i++) {
      const a = spine[Math.max(0, i - 1)],
        b = spine[i],
        c = spine[i + 1],
        d = spine[Math.min(spine.length - 1, i + 2)];
      const steps = Math.max(4, Math.ceil(Math.hypot(c[0] - b[0], c[1] - b[1]) / 6));
      for (let j = 0; j < steps; j++) {
        const t = j / steps,
          ratio = (i + t) / (spine.length - 1);
        const radius =
          (width + (endWidth - width) * ratio) *
          (1 + 0.21 * Math.sin(16 * ratio + phase) + 0.14 * Math.cos(34 * ratio + phase));
        line.push([spline(a[0], b[0], c[0], d[0], t), spline(a[1], b[1], c[1], d[1], t), radius]);
      }
    }
    line.push([...spine[spine.length - 1], endWidth]);
    for (let i = 1; i < line.length; i++) {
      const [x, y, r] = line[i - 1],
        [u, v, r2] = line[i],
        dx = u - x,
        dy = v - y,
        dd = dx * dx + dy * dy;
      const padding = 45 + Math.max(r, r2);
      for (
        let by = Math.max(0, Math.floor((Math.min(y, v) - padding) / CELL));
        by < Math.min(ROWS, (Math.max(y, v) + padding) / CELL);
        by++
      )
        for (
          let bx = Math.max(0, Math.floor((Math.min(x, u) - padding) / CELL));
          bx < Math.min(COLS, (Math.max(x, u) + padding) / CELL);
          bx++
        ) {
          const px = (bx + 0.5) * CELL - x,
            py = (by + 0.5) * CELL - y;
          const t = Math.max(0, Math.min(1, (px * dx + py * dy) / (dd || 1)));
          const distance = Math.hypot(px - dx * t, py - dy * t) - (r + (r2 - r) * t);
          const n = by * COLS + bx;
          if (distance < reach[n]) reach[n] = distance;
        }
    }
  }
  channel(
    [
      [-40, 159],
      [76, 171],
      [156, 220],
      [228, 221],
      [308, 177],
      [360, 182],
      [393, 214],
      [403, 261],
      [445, 282],
      [504, 257],
      [539, 255],
      [577, 289],
      [593, 330],
      [637, 353],
      [710, 346],
      [791, 366],
      [871, 412],
      [990, 427]
    ],
    22,
    6,
    1
  );
  channel(
    [
      [289, 185],
      [331, 173],
      [379, 137],
      [439, 120],
      [500, 141],
      [551, 182],
      [607, 187],
      [676, 150],
      [731, 151],
      [806, 198],
      [865, 201],
      [1009, 166]
    ],
    11,
    3,
    2
  );
  channel(
    [
      [412, 264],
      [436, 287],
      [446, 333],
      [416, 377],
      [430, 410],
      [486, 428],
      [528, 403],
      [575, 411],
      [604, 459],
      [660, 489],
      [731, 500],
      [789, 542],
      [876, 556],
      [944, 570]
    ],
    15,
    4,
    4
  );
  channel(
    [
      [506, 421],
      [534, 442],
      [543, 493],
      [570, 537],
      [640, 559],
      [687, 594],
      [715, 667]
    ],
    8,
    3,
    1
  );
  channel(
    [
      [88, 438],
      [159, 443],
      [221, 485],
      [248, 530],
      [294, 542],
      [346, 522],
      [384, 534],
      [423, 578],
      [468, 599]
    ],
    13,
    2,
    2
  );
  channel(
    [
      [652, 114],
      [700, 99],
      [747, 115],
      [770, 145],
      [803, 155],
      [851, 149]
    ],
    6,
    2,
    0
  );
  // Lens centres, axial lengths, widths and orientation in local survey coordinates.
  const bars = [
    [157, 173, 66, 20, 0.2],
    [262, 255, 80, 27, -0.47],
    [402, 137, 71, 21, -0.29],
    [508, 208, 70, 25, 0.38],
    [550, 299, 64, 19, 0.56],
    [482, 370, 59, 23, -0.65],
    [666, 299, 88, 33, -0.26],
    [744, 392, 83, 29, 0.53],
    [842, 316, 84, 38, 0.02],
    [807, 463, 76, 28, 0.25],
    [732, 548, 54, 20, 0.75],
    [586, 519, 79, 29, 0.8],
    [361, 468, 93, 25, -0.25],
    [214, 398, 75, 29, 0.53],
    [311, 320, 52, 18, -0.6],
    [306, 585, 54, 21, 0.32],
    [858, 205, 44, 18, -0.14],
    [569, 123, 47, 18, 0.19],
    [156, 289, 48, 16, -0.06],
    [910, 507, 44, 20, 0.3]
  ];
  const lenses = bars.map(([x, y, a, b, r], i) => ({
    x,
    y,
    a,
    b,
    co: Math.cos(r),
    si: Math.sin(r),
    phase: i * 2.399
  }));
  function barValue(x, y) {
    let nearest = 5;
    for (const l of lenses) {
      const dx = x - l.x,
        dy = y - l.y;
      if (Math.abs(dx) > l.a * 1.5 || Math.abs(dy) > l.a * 1.4) continue;
      const u = (dx * l.co + dy * l.si) / l.a,
        v = (dy * l.co - dx * l.si) / l.b;
      // Asymmetric crests pick out migrating bars. The straight axis is a local
      // depositional trend, not an ellipse drawn over the interpreted coverage.
      const crest = 0.36 * Math.sin(2.7 * u + l.phase) + 0.21 * (u * u - 0.3) * Math.sin(l.phase);
      const spread = Math.max(0.42, 1 + 0.42 * u * Math.cos(l.phase) + 0.12 * Math.sin(4 * u + l.phase));
      const cross = (v - crest) / spread;
      const r = u * u + cross * cross;
      if (r < nearest) nearest = r;
    }
    return nearest;
  }
  const classes = new Uint8Array(COLS * ROWS);
  for (let row = 0; row < ROWS; row++)
    for (let col = 0; col < COLS; col++) {
      const x = (col + 0.5) * CELL,
        y = (row + 0.5) * CELL,
        i = row * COLS + col;
      if (!inside(x, y)) continue;
      const texture = wiggle(x, y),
        r = reach[i] + texture * 4.3,
        l = barValue(x + 4 * texture, y + 5 * texture);
      const shelf =
        0.5 * Math.sin(x * 0.012 + y * 0.016 + 0.8 * Math.sin(x * 0.012)) +
        0.43 * Math.sin(y * 0.023 - x * 0.007 + 0.85 * Math.cos(y * 0.015)) +
        0.24 * Math.cos(x * 0.019 + y * 0.007) +
        texture * 0.18;
      const abandoned = ((x - 323) / 97) ** 2 + ((y - 387 + 15 * Math.sin(x * 0.027)) / 23) ** 2;
      const southeast = ((x - 795) / 68) ** 2 + ((y - 574 - 0.34 * (x - 795)) / 15) ** 2;
      classes[i] =
        r < 0
          ? 1
          : l < 0.88 + texture * 0.22
            ? 2
            : Math.min(abandoned, southeast) < 0.88 + texture * 0.13
              ? 5
              : r < 8 + 3 * texture || l < 1.47 + texture * 0.17 || shelf > 0.22
                ? 3
                : 4;
    }
  function neighbours(n, fn) {
    const x = n % COLS;
    if (x > 0) fn(n - 1);
    if (x < COLS - 1) fn(n + 1);
    if (n >= COLS) fn(n - COLS);
    if (n < COLS * (ROWS - 1)) fn(n + COLS);
  }
  const ids = new Int32Array(classes.length),
    queue = new Int32Array(classes.length);
  function regions(removeSmall) {
    ids.fill(0);
    const found = [];
    let region = 0;
    for (let n = 0; n < classes.length; n++) {
      if (!classes[n] || ids[n]) continue;
      const k = classes[n],
        touching = [0, 0, 0, 0, 0, 0];
      let count = 1,
        sx = 0,
        sy = 0;
      ids[n] = ++region;
      queue[0] = n;
      for (let i = 0; i < count; i++) {
        const p = queue[i];
        sx += ((p % COLS) + 0.5) * CELL;
        sy += (Math.floor(p / COLS) + 0.5) * CELL;
        neighbours(p, (v) => {
          if (classes[v] === k && !ids[v]) {
            ids[v] = region;
            queue[count++] = v;
          } else if (classes[v] !== k && classes[v]) touching[classes[v]]++;
        });
      }
      if (removeSmall && count < 32) {
        const next = touching.indexOf(Math.max(...touching));
        if (next) for (let i = 0; i < count; i++) classes[queue[i]] = next;
      } else if (!removeSmall) {
        let distance = Infinity,
          centre = queue[0];
        const cx = sx / count,
          cy = sy / count;
        for (let i = 0; i < count; i++) {
          const p = queue[i];
          const dx = ((p % COLS) + 0.5) * CELL - cx,
            dy = (Math.floor(p / COLS) + 0.5) * CELL - cy;
          if (dx * dx + dy * dy < distance) {
            distance = dx * dx + dy * dy;
            centre = p;
          }
        }
        found.push({
          region,
          cluster: k,
          facies: clusters[k - 1].facies,
          count,
          areaKm2: +(count * ((metres * CELL) / 1000) ** 2).toFixed(2),
          center: { x: ((centre % COLS) + 0.5) * CELL, y: (Math.floor(centre / COLS) + 0.5) * CELL },
          wells: []
        });
      }
    }
    return found;
  }
  regions(true);
  const patches = regions(false);
  // Number one easy-to-identify central mouth bar consistently across screen sizes.
  const featured = patches
    .filter((p) => p.cluster === 2)
    .sort(
      (a, b) =>
        Math.hypot(a.center.x - 666, a.center.y - 299) - Math.hypot(b.center.x - 666, b.center.y - 299)
    )[0];
  let nextId = 1;
  for (const p of patches) {
    if (nextId === 17) nextId++;
    p.id = 'P-' + String(p === featured ? 17 : nextId++).padStart(3, '0');
    clusters[p.cluster - 1].patches.push(p.id);
  }
  const byId = new Map(patches.map((p) => [p.id, p]));
  function patchAt(x, y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return null;
    return patches[ids[Math.floor(y / CELL) * COLS + Math.floor(x / CELL)] - 1] || null;
  }
  const wellTargets = [
    [1, 169, 223, 'channel'],
    [1, 562, 284, 'channel'],
    [1, 376, 529, 'bar'],
    [1, 725, 346, 'channel'],
    [2, 253, 251, 'bar'],
    [2, 666, 300, 'bar'],
    [2, 383, 468, 'bar'],
    [2, 796, 461, 'sand'],
    [3, 365, 93, 'sand'],
    [3, 852, 250, 'sand'],
    [3, 281, 568, 'channel'],
    [4, 175, 352, 'bay'],
    [4, 547, 354, 'bay'],
    [4, 656, 513, 'sand'],
    [4, 770, 120, 'bay'],
    [5, 310, 390, 'sand'],
    [5, 809, 578, 'bar']
  ];
  const wells = wellTargets.map(([cluster, x, y, facies], i) => {
    let chosen = 0,
      closest = Infinity;
    for (let n = 0; n < classes.length; n++)
      if (classes[n] === cluster) {
        const a = ((n % COLS) + 0.5) * CELL,
          b = (Math.floor(n / COLS) + 0.5) * CELL;
        const dist = (a - x) ** 2 + (b - y) ** 2;
        if (dist < closest) {
          closest = dist;
          chosen = n;
        }
      }
    const p = patches[ids[chosen] - 1],
      c = clusters[cluster - 1],
      id = 'HY-' + String(i + 1).padStart(2, '0');
    p.wells.push(id);
    c.wells.push(id);
    c.votes[facies] = (c.votes[facies] || 0) + 1;
    return {
      id,
      name: id,
      x: ((chosen % COLS) + 0.5) * CELL,
      y: (Math.floor(chosen / COLS) + 0.5) * CELL,
      cluster,
      facies,
      patchId: p.id
    };
  });
  // Extract the categorical cell boundaries once. Drawing and hit testing share the same grid.
  const borders = new Path2D(),
    regionEdges = patches.map(() => new Path2D());
  function edge(a, b, x, y, u, v) {
    if (a === b) return;
    if (a && b) {
      borders.moveTo(x, y);
      borders.lineTo(u, v);
    }
    if (a) {
      const path = regionEdges[a - 1];
      path.moveTo(x, y);
      path.lineTo(u, v);
    }
    if (b) {
      const path = regionEdges[b - 1];
      path.moveTo(x, y);
      path.lineTo(u, v);
    }
  }
  for (let y = 0; y < ROWS; y++)
    for (let x = 0; x < COLS; x++) {
      const n = y * COLS + x;
      edge(ids[n], x ? ids[n - 1] : 0, x * CELL, y * CELL, x * CELL, (y + 1) * CELL);
      edge(ids[n], y ? ids[n - COLS] : 0, x * CELL, y * CELL, (x + 1) * CELL, y * CELL);
    }
  function horizon(x, y) {
    const a = Math.sin(0.011 * x + 0.012 * y),
      b = Math.cos(0.007 * x - 0.011 * y);
    return (
      1640 +
      0.15 * x +
      0.09 * y +
      13 * a +
      18 * b +
      10 * Math.sin(0.018 * x + 0.012 * y) +
      22 * Math.exp(-(((x - 621) / 108) ** 2 + ((y - 246) / 98) ** 2)) -
      16 * Math.exp(-(((x - 422) / 88) ** 2 + ((y - 419) / 66) ** 2))
    );
  }
  const contourPaths = [new Path2D(), new Path2D()],
    contourLabels = [];
  function contours() {
    const step = 9,
      labelSites = [
        [214, 308],
        [621, 128],
        [574, 554],
        [782, 436],
        [388, 399]
      ];
    const best = labelSites.map(() => ({ d: 1e6 }));
    for (let y = 0; y < H; y += step)
      for (let x = 0; x < W; x += step) {
        if (!inside(x + step / 2, y + step / 2)) continue;
        const pts = [
          [x, y],
          [x + step, y],
          [x + step, y + step],
          [x, y + step]
        ];
        const v = pts.map(([u, w]) => horizon(u, w)),
          lo = Math.min(...v),
          hi = Math.max(...v);
        for (let level = Math.ceil(lo / 5) * 5; level <= hi; level += 5) {
          const hits = [];
          for (let i = 0; i < 4; i++) {
            const j = (i + 1) % 4;
            if (v[i] < level === v[j] < level) continue;
            const t = (level - v[i]) / (v[j] - v[i]);
            hits.push([pts[i][0] + (pts[j][0] - pts[i][0]) * t, pts[i][1] + (pts[j][1] - pts[i][1]) * t]);
          }
          const major = level % 20 === 0,
            path = contourPaths[major ? 1 : 0];
          for (let i = 1; i < hits.length; i += 2) {
            const [a, b] = [hits[i - 1], hits[i]];
            path.moveTo(...a);
            path.lineTo(...b);
            if (major)
              labelSites.forEach(([u, w], j) => {
                const d = (a[0] - u) ** 2 + (a[1] - w) ** 2;
                if (d < best[j].d)
                  best[j] = {
                    x: a[0],
                    y: a[1],
                    angle: Math.atan2(b[1] - a[1], b[0] - a[0]),
                    value: level,
                    d
                  };
              });
          }
        }
      }
    contourLabels.push(...best.filter((p) => p.d < 7000));
  }
  contours();
  const grains = [];
  function noise(n) {
    const v = Math.sin(n * 78.233) * 43758.5453;
    return v - Math.floor(v);
  }
  for (let j = 0; j < H / 19; j++)
    for (let i = 0; i < W / 22; i++) {
      const seed = j * 71 + i,
        x = 22 * (i + 0.2 + 0.66 * noise(seed)),
        y = 19 * (j + 0.3 + 0.5 * noise(seed + 451));
      const p = patchAt(x, y);
      if (p && patchAt(x - 4, y - 2) === p && patchAt(x + 4, y + 2) === p)
        grains.push({ x, y, id: p.id, facies: p.facies });
    }
  const mono = 'PaleoMono,"JetBrains Mono","Cascadia Mono",Consolas,monospace';
  const geoFont = 'Paleo,"IBM Plex Sans SC","Microsoft YaHei UI",sans-serif';
  function rgba(hex) {
    return [1, 3, 5].map((n) => parseInt(hex.slice(n, n + 2), 16));
  }
  function create(canvas, handlers = {}) {
    const ctx = canvas.getContext('2d'),
      base = document.createElement('canvas');
    base.width = COLS;
    base.height = ROWS;
    const baseCtx = base.getContext('2d'),
      pixels = baseCtx.createImageData(COLS, ROWS);
    let state = {
      product: 'well',
      selected: null,
      overrides: {},
      wells: true,
      contours: true,
      showSurface: true,
      tool: 'select',
      view: { ...home },
      cursor: null
    };
    let width = 1,
      height = 1,
      scale = 1,
      frame = 0,
      bitmapKey = '',
      grainPath = null,
      down = null,
      suppressClick = false;
    const paper = '#F5F5EE';
    function screen(x, y) {
      return { x: (x - state.view.x) * scale + width / 2, y: (y - state.view.y) * scale + height / 2 };
    }
    function point(x, y) {
      const ux = (x - width / 2) / scale + state.view.x,
        uy = (y - height / 2) / scale + state.view.y;
      return {
        x: ux,
        y: uy,
        easting: Math.round(east0 + ux * metres),
        northing: Math.round(north0 - uy * metres)
      };
    }
    function bitmaps() {
      const key = state.product + JSON.stringify(state.overrides || {});
      if (key === bitmapKey) return;
      bitmapKey = key;
      const palette = patches.map((p) =>
        rgba(
          state.product === 'raw'
            ? clusters[p.cluster - 1].color
            : (
                symbols.get((state.product === 'manual' && state.overrides?.[p.id]) || p.facies) ||
                symbols.get('unassigned')
              ).color
        )
      );
      for (let n = 0; n < ids.length; n++) {
        const offset = n * 4,
          id = ids[n];
        if (!id) {
          pixels.data[offset + 3] = 0;
          continue;
        }
        const color = palette[id - 1];
        pixels.data[offset] = color[0];
        pixels.data[offset + 1] = color[1];
        pixels.data[offset + 2] = color[2];
        pixels.data[offset + 3] = 255;
      }
      baseCtx.putImageData(pixels, 0, 0);
      grainPath = new Path2D();
      if (state.product === 'raw') return;
      for (const g of grains) {
        const kind = (state.product === 'manual' && state.overrides?.[g.id]) || g.facies,
          x = g.x,
          y = g.y;
        if (kind === 'bar') {
          grainPath.moveTo(x - 2.7, y + 0.5);
          grainPath.quadraticCurveTo(x, y - 1.3, x + 2.7, y + 0.5);
          grainPath.moveTo(x - 1.6, y + 1.8);
          grainPath.lineTo(x + 1.2, y + 1.8);
        } else if (kind === 'channel') {
          grainPath.moveTo(x - 2.8, y + 1.2);
          grainPath.lineTo(x + 1, y - 0.8);
          grainPath.moveTo(x - 0.8, y + 1.6);
          grainPath.lineTo(x + 2.2, y + 0.1);
        } else if (kind === 'sand') {
          grainPath.moveTo(x - 2.4, y);
          grainPath.lineTo(x - 0.9, y);
          grainPath.moveTo(x + 0.5, y + 1.4);
          grainPath.lineTo(x + 1.2, y + 1.4);
        } else if (kind === 'bay') {
          grainPath.moveTo(x - 2.4, y);
          grainPath.bezierCurveTo(x - 0.4, y - 1.4, x + 0.3, y + 1.5, x + 2.8, y);
        } else {
          grainPath.moveTo(x - 1.4, y + 1.4);
          grainPath.lineTo(x + 1.4, y - 1.4);
        }
      }
    }
    function grid() {
      const a = point(0, 0),
        b = point(width, height);
      const span = (100 * metres) / scale;
      const spacing = [100, 250, 500, 1000, 2000, 5000, 10000, 20000].find((n) => n >= span) || 50000;
      const ticks = { x: [], y: [] };
      ctx.save();
      ctx.lineWidth = 0.65;
      for (let e = Math.ceil(a.easting / spacing) * spacing; e <= b.easting; e += spacing) {
        const x = screen((e - east0) / metres, 0).x;
        ticks.x.push([x, e]);
        ctx.strokeStyle = '#71888148';
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, height);
        ctx.stroke();
      }
      for (let n = Math.ceil(b.northing / spacing) * spacing; n <= a.northing; n += spacing) {
        const y = screen(0, (north0 - n) / metres).y;
        ticks.y.push([y, n]);
        ctx.strokeStyle = '#71888148';
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(width, y);
        ctx.stroke();
      }
      ctx.font = '10px ' + mono;
      ctx.fillStyle = '#5D7168';
      ctx.textAlign = 'center';
      for (const [x, e] of ticks.x) {
        if (x > 44 && x < width - 34) {
          ctx.fillText(String(e), x, height - 5);
        }
      }
      ctx.textAlign = 'left';
      for (const [y, n] of ticks.y)
        if (y > 29 && y < height - 28) {
          ctx.save();
          ctx.translate(11, y);
          ctx.rotate(-Math.PI / 2);
          ctx.textAlign = 'center';
          ctx.fillText(String(n), 0, 0);
          ctx.restore();
        }
      ctx.restore();
    }
    function inWorld(fn) {
      ctx.save();
      ctx.translate(width / 2, height / 2);
      ctx.scale(scale, scale);
      ctx.translate(-state.view.x, -state.view.y);
      fn();
      ctx.restore();
    }
    function cartography() {
      inWorld(() => {
        if (state.showSurface) {
          bitmaps();
          ctx.imageSmoothingEnabled = scale < 1.25;
          ctx.drawImage(base, 0, 0, W, H);
          if (scale >= 0.6 && state.product !== 'raw') {
            ctx.strokeStyle = 'rgba(76,83,62,.29)';
            ctx.lineWidth = 0.8 / scale;
            ctx.stroke(grainPath);
          }
          ctx.strokeStyle = 'rgba(91,104,74,.36)';
          ctx.lineWidth = 0.6 / scale;
          ctx.stroke(borders);
        }
        if (state.contours) {
          ctx.save();
          ctx.clip(boundary);
          ctx.strokeStyle = 'rgba(70,83,65,.25)';
          ctx.lineWidth = 0.65 / scale;
          ctx.stroke(contourPaths[0]);
          ctx.strokeStyle = 'rgba(67,80,62,.47)';
          ctx.lineWidth = 0.9 / scale;
          ctx.stroke(contourPaths[1]);
          ctx.restore();
        }
        ctx.lineWidth = 1.3 / scale;
        ctx.strokeStyle = '#5D7368';
        ctx.stroke(boundary);
        if (state.highlightCluster && state.showSurface) {
          ctx.strokeStyle = '#FFFDF1';
          ctx.lineWidth = 3 / scale;
          for (const p of patches)
            if (p.cluster === state.highlightCluster) ctx.stroke(regionEdges[p.region - 1]);
          ctx.strokeStyle = '#253D45';
          ctx.lineWidth = 1.4 / scale;
          for (const p of patches)
            if (p.cluster === state.highlightCluster) ctx.stroke(regionEdges[p.region - 1]);
        }
        if (state.selected && state.showSurface && byId.has(state.selected)) {
          const p = byId.get(state.selected);
          ctx.strokeStyle = '#FFFDF1';
          ctx.lineWidth = 4 / scale;
          ctx.stroke(regionEdges[p.region - 1]);
          ctx.strokeStyle = '#253D45';
          ctx.lineWidth = 2 / scale;
          ctx.stroke(regionEdges[p.region - 1]);
        }
      });
      if (state.contours && width > 400) {
        for (const p of contourLabels) {
          const pos = screen(p.x, p.y);
          ctx.save();
          ctx.translate(pos.x, pos.y);
          let angle = p.angle;
          if (angle > Math.PI / 2) angle -= Math.PI;
          else if (angle < -Math.PI / 2) angle += Math.PI;
          ctx.rotate(angle);
          ctx.font = 'italic 9px ' + mono;
          ctx.fillStyle = '#52665A';
          ctx.textAlign = 'center';
          ctx.lineWidth = 2.6;
          ctx.strokeStyle = 'rgba(242,242,217,.86)';
          ctx.strokeText(String(p.value), 0, -2);
          ctx.fillText(String(p.value), 0, -2);
          ctx.restore();
        }
      }
    }
    function wellSymbols() {
      if (!state.wells) return;
      ctx.save();
      ctx.font = '600 10px ' + mono;
      ctx.lineJoin = 'round';
      const occupied = [];
      for (let i = 0; i < wells.length; i++) {
        const well = wells[i],
          { x, y } = screen(well.x, well.y);
        if (x < 11 || x > width - 7 || y < 8 || y > height - 14) continue;
        ctx.beginPath();
        ctx.arc(x, y, 3.1, 0, Math.PI * 2);
        ctx.fillStyle = '#243A3B';
        ctx.fill();
        ctx.strokeStyle = '#FFFDF5';
        ctx.lineWidth = 1.15;
        ctx.stroke();
        if (width < 500 && i % 2 && state.view.z < 1.7) continue;
        const len = ctx.measureText(well.id).width,
          side = x + 9 + len < width - 10 ? 1 : -1;
        const lx = x + side * 6,
          ly = y - 5,
          bounds = [side === 1 ? lx : lx - len, ly - 9, side === 1 ? lx + len : lx, ly + 1];
        if (
          occupied.some(
            (b) =>
              bounds[0] < b[2] + 3 && bounds[2] > b[0] - 3 && bounds[1] < b[3] + 2 && bounds[3] > b[1] - 2
          )
        )
          continue;
        occupied.push(bounds);
        ctx.textAlign = side === 1 ? 'left' : 'right';
        ctx.lineWidth = 2.8;
        ctx.strokeStyle = 'rgba(255,253,242,.92)';
        ctx.strokeText(well.id, lx, ly);
        ctx.fillStyle = '#2C403F';
        ctx.fillText(well.id, lx, ly);
      }
      ctx.restore();
    }
    function furniture() {
      ctx.save();
      ctx.fillStyle = '#344A43';
      ctx.font = '600 11px ' + geoFont;
      ctx.textAlign = 'center';
      const nx = 32,
        ny = 29;
      ctx.fillText('N', nx, ny - 3);
      ctx.beginPath();
      ctx.moveTo(nx, ny);
      ctx.lineTo(nx - 5, ny + 18);
      ctx.lineTo(nx, ny + 13);
      ctx.closePath();
      ctx.fill();
      ctx.beginPath();
      ctx.moveTo(nx, ny);
      ctx.lineTo(nx + 5, ny + 18);
      ctx.lineTo(nx, ny + 13);
      ctx.closePath();
      ctx.fillStyle = '#FBFCF7';
      ctx.fill();
      ctx.strokeStyle = '#344A43';
      ctx.lineWidth = 0.8;
      ctx.stroke();
      const length =
        [50, 100, 250, 500, 1000, 2000, 5000, 10000]
          .filter((v) => (v * scale) / metres <= Math.max(65, Math.min(width * 0.15, 100)))
          .pop() || 50;
      const sx = width > 440 ? 32 : 23,
        sy = height - 26,
        dx = (length * scale) / metres;
      ctx.fillStyle = '#344A43';
      ctx.font = '10px ' + mono;
      ctx.textAlign = 'left';
      ctx.fillText(length >= 1000 ? length / 1000 + ' km' : length + ' m', sx, sy - 5);
      ctx.fillRect(sx, sy, dx / 2, 3);
      ctx.fillStyle = '#FFFDF5';
      ctx.fillRect(sx + dx / 2, sy, dx / 2, 3);
      ctx.lineWidth = 1;
      ctx.strokeStyle = '#344A43';
      ctx.strokeRect(sx + 0.5, sy + 0.5, dx - 1, 2);
      if (state.cursor) {
        const c = screen(state.cursor.x, state.cursor.y);
        ctx.strokeStyle = '#415C63';
        ctx.setLineDash([3, 3]);
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(c.x - 11, c.y);
        ctx.lineTo(c.x + 11, c.y);
        ctx.moveTo(c.x, c.y - 11);
        ctx.lineTo(c.x, c.y + 11);
        ctx.stroke();
      }
      ctx.restore();
    }
    function redraw() {
      frame = 0;
      const box = canvas.getBoundingClientRect();
      width = box.width;
      height = box.height;
      if (width < 1 || height < 1) return;
      const dpr = Math.min(3, window.devicePixelRatio || 1);
      if (canvas.width !== Math.round(width * dpr) || canvas.height !== Math.round(height * dpr)) {
        canvas.width = Math.round(width * dpr);
        canvas.height = Math.round(height * dpr);
      }
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      scale = Math.min((width - 25) / W, (height - 32) / H) * state.view.z;
      scale = Math.max(0.01, scale);
      ctx.fillStyle = paper;
      ctx.fillRect(0, 0, width, height);
      cartography();
      grid();
      wellSymbols();
      furniture();
    }
    function invalidate() {
      if (!frame) frame = requestAnimationFrame(redraw);
    }
    function moveView(v, notify = true) {
      state.view = { x: v.x, y: v.y, z: Math.max(0.55, Math.min(12, v.z)) };
      invalidate();
      if (notify) handlers.onView?.({ ...state.view });
    }
    function zoom(factor, anchor) {
      const v = state.view,
        z = Math.max(0.55, Math.min(12, v.z * factor));
      const about = anchor || { x: v.x, y: v.y };
      moveView({ x: about.x - ((about.x - v.x) * v.z) / z, y: about.y - ((about.y - v.y) * v.z) / z, z });
    }
    function relative(e) {
      const b = canvas.getBoundingClientRect();
      return { x: e.clientX - b.left, y: e.clientY - b.top };
    }
    canvas.style.touchAction = 'none';
    canvas.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 && e.button !== 1) return;
      const pos = relative(e);
      down = {
        ...pos,
        world: { ...state.view },
        id: e.pointerId,
        pan: state.tool === 'pan' || e.button === 1
      };
      suppressClick = false;
      canvas.setPointerCapture(e.pointerId);
      if (down.pan) canvas.style.cursor = 'grabbing';
    });
    canvas.addEventListener('pointermove', (e) => {
      const pos = relative(e);
      if (down && down.id === e.pointerId) {
        const dx = pos.x - down.x,
          dy = pos.y - down.y;
        if (Math.hypot(dx, dy) > 4 && down.pan) suppressClick = true;
        if (down.pan && suppressClick)
          moveView({ x: down.world.x - dx / scale, y: down.world.y - dy / scale, z: down.world.z });
      }
      handlers.onCursor?.(point(pos.x, pos.y));
    });
    function finish(e, cancelled) {
      if (!down || down.id !== e.pointerId) return;
      const pos = relative(e),
        p = point(pos.x, pos.y);
      if (!cancelled && !down.pan && !suppressClick && Math.hypot(pos.x - down.x, pos.y - down.y) < 6) {
        const hit = state.showSurface ? patchAt(p.x, p.y) : null;
        handlers.onSelect?.(hit?.id || null);
      }
      down = null;
      canvas.style.cursor = state.tool === 'pan' ? 'grab' : 'crosshair';
      if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
    }
    canvas.addEventListener('pointerup', (e) => finish(e, false));
    canvas.addEventListener('pointercancel', (e) => finish(e, true));
    canvas.addEventListener('pointerleave', () => {
      if (!down) handlers.onCursor?.(null);
    });
    canvas.addEventListener(
      'wheel',
      (e) => {
        e.preventDefault();
        const p = relative(e);
        zoom(Math.exp(-Math.sign(e.deltaY) * 0.16), point(p.x, p.y));
      },
      { passive: false }
    );
    const observer = new ResizeObserver(invalidate);
    observer.observe(canvas);
    invalidate();
    document.fonts?.ready.then(invalidate);
    return {
      setState(update) {
        const { view, ...rest } = update;
        state = { ...state, ...rest };
        if (view) moveView(view, false);
        canvas.style.cursor = state.tool === 'pan' ? 'grab' : 'crosshair';
        invalidate();
      },
      get view() {
        return { ...state.view };
      },
      getView() {
        return { ...state.view };
      },
      fit() {
        moveView({ ...home });
      },
      zoom,
      redraw,
      getScaleDenominator() {
        return Math.round(((metres / scale) * 96) / 0.0254);
      },
      pointForPatch(id) {
        const p = byId.get(id);
        return p ? screen(p.center.x, p.center.y) : null;
      },
      screenToWorld(x, y) {
        return point(x, y);
      },
      worldToScreen(x, y) {
        return screen(x, y);
      },
      destroy() {
        observer.disconnect();
        cancelAnimationFrame(frame);
      }
    };
  }
  window.GeoWorkspace = { create, patches, wells, facies, clusters, featuredPatchId: featured.id };
})();
