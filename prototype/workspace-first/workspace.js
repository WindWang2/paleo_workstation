'use strict';
(() => {
  const $ = (id) => document.getElementById(id);
  const D = window.GeoWorkspace;
  const shapes = {
    strata: 'M3 8 12 3l9 5-9 5z M3 12l9 5 9-5 M3 16l9 5 9-5',
    layers: 'M3 8l9-5 9 5-9 5z M3 12l9 5 9-5 M3 16l9 5 9-5',
    save: 'M4 3h13l3 3v15H4z M8 3v7h8V3 M8 21v-7h8v7',
    undo: 'M9 5 4 10l5 5 M5 10h10a5 5 0 0 1 0 10',
    redo: 'M15 5l5 5-5 5 M19 10H9a5 5 0 0 0 0 10',
    theme: 'M20 15a8 8 0 1 1-11-11 8 8 0 0 0 11 11z',
    keys: 'M3 6h18v12H3z M6 10h1m3 0h1m3 0h1m3 0h0 M7 14h1m3 0h6',
    ribbon: 'M3 5h18v14H3z M3 10h18',
    sidebar: 'M3 4h18v16H3z M9 4v16',
    panel: 'M3 4h18v16H3z M15 4v16',
    table: 'M3 4h18v16H3z M3 10h18 M3 15h18 M9 4v16',
    arrow: 'm9 6 6 6-6 6',
    down: 'm6 9 6 6 6-6',
    up: 'm6 14 6-6 6 6',
    focus: 'M3 9V4h5m8 0h5v5m0 6v5h-5m-8 0H3v-5',
    split: 'M3 4h18v16H3z M12 4v16',
    close: 'M6 6l12 12M18 6 6 18',
    pointer: 'M5 3l13 10-6 1-3 6z',
    hand: 'M7 12V7a1.5 1.5 0 0 1 3 0v4-7a1.5 1.5 0 0 1 3 0v7-6a1.5 1.5 0 0 1 3 0v7-4a1.5 1.5 0 0 1 3 0v7c0 4-2 6-6 6h-1c-2 0-4-1-5-3l-3-4c-1-2 0-3 2-2l2 2',
    plus: 'M12 5v14M5 12h14',
    minus: 'M5 12h14',
    extent: 'M4 9V4h5m6 0h5v5m0 6v5h-5M9 20H4v-5 M9 9h6v6H9z',
    link: 'M10 14l4-4 M8 16l-1 1a3.5 3.5 0 0 1-5-5l4-4a3.5 3.5 0 0 1 5 0 M16 8l1-1a3.5 3.5 0 0 1 5 5l-4 4a3.5 3.5 0 0 1-5 0',
    circle: 'M12 7a5 5 0 1 0 0 10 5 5 0 1 0 0-10',
    list: 'M8 6h12M8 12h12M8 18h12M4 6h0m0 6h0m0 6h0',
    agent: 'M4 4h16v13H9l-5 4z M8 9h8 M8 12h5',
    map: 'M3 6l6-3 6 3 6-3v15l-6 3-6-3-6 3z M9 3v15m6-12v15',
    eye: 'M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z M12 9a3 3 0 1 0 0 6 3 3 0 1 0 0-6',
    off: 'M3 3l18 18 M3 12s3-5 9-5c6 0 9 5 9 5 M4 14s3 4 8 4c2 0 4-1 5-2',
    search: 'M10 4a6 6 0 1 0 0 12 6 6 0 1 0 0-12 M15 15l6 6',
    well: 'M12 4v15 M7 19h10 M7 5h10 M9 12h6',
    contour: 'M2 14c5-9 11 9 20-3M2 9c5-9 11 9 20-3 M2 19c5-9 11 9 20-3',
    index: 'M4 4h16v16H4z M8 8h8m-8 4h8m-8 4h5',
    time: 'M12 3a9 9 0 1 0 0 18 9 9 0 1 0 0-18 M12 7v6l4 2',
    chart: 'M3 3v17h18M6 14l4-5 4 2 5-7',
    wave: 'M2 12h4l2-7 3 14 3-13 2 9 2-3h4',
    check: 'm4 12 5 5L20 6',
    pins: 'M4 6h4v4H4zM15 12h4v4h-4z M8 8l8 5',
    edit: 'M4 16 16 4l4 4L8 20H4z M13 7l4 4',
    folder: 'M3 6h7l2 2h9v12H3z'
  };
  const ico = (name) =>
    '<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linejoin="round" stroke-linecap="round"><path d="' +
    (shapes[name] || shapes.layers) +
    '"/></svg>';
  function decorate(root = document) {
    root.querySelectorAll('[data-icon]').forEach((el) => {
      if (!el.firstElementChild) el.innerHTML = ico(el.dataset.icon);
      if (el.tagName === 'BUTTON' && el.title) el.setAttribute('aria-label', el.title);
    });
  }
  const h = (value) =>
    String(value ?? '').replace(
      /[&<>"']/g,
      (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]
    );
  const symbol = (k) => '<i data-icon="' + k + '"></i>';
  const action = (id, label, icon = '') =>
    '<button class="action" data-act="' + id + '">' + (icon ? symbol(icon) : '') + label + '</button>';
  const names = { map: 'QGIS 视图', logs: '单井图道', seismic: '地震剖面', catalog: '资料目录' };
  const icons = { map: 'map', logs: 'chart', seismic: 'wave', catalog: 'table' };
  const flows = ['数据管理', '地层对比', '单井相分析', '地震相分析', '古地理编图'];
  const commands = [
    [
      ['catalog', 'table', '资料目录'],
      ['map', 'map', '工区地图']
    ],
    [
      ['logs', 'chart', '查看井道'],
      ['seismic', 'wave', '查看剖面']
    ],
    [
      ['logs', 'chart', '单井图道'],
      ['wells', 'well', '井位分布']
    ],
    [
      ['seismic', 'wave', '查看剖面'],
      ['raw', 'layers', '原始簇图'],
      ['wellPreview', 'pins', '用井预测赋相']
    ],
    [
      ['select', 'pointer', '选择图斑'],
      ['wellPreview', 'pins', '用井预测赋相'],
      ['compare', 'split', '参考对照'],
      ['save', 'save', '保存版本']
    ]
  ];
  const baseVersions = [
    { id: 1, name: '原始地震簇图', product: 'raw', overrides: {}, parent: null },
    { id: 2, name: '井赋相解释', product: 'well', overrides: {}, parent: 1 }
  ];
  const storageKey = 'paleo-workspace-prototype-v1';
  function restore() {
    try {
      const v = JSON.parse(localStorage.getItem(storageKey));
      return Array.isArray(v) &&
        v.every(
          (x) =>
            x &&
            Number.isInteger(x.id) &&
            x.id > 2 &&
            ['manual', 'well'].includes(x.product) &&
            x.overrides &&
            typeof x.overrides === 'object'
        )
        ? v
        : [];
    } catch {
      return [];
    }
  }
  const versions = [...baseVersions, ...restore()];
  const st = {
    flow: 4,
    doc: 'map',
    docs: ['map'],
    v: versions.at(-1).id,
    ref: 1,
    reference: false,
    linked: true,
    selected: null,
    left: innerWidth > 730,
    panel: null,
    focus: false,
    ribbon: false,
    resource: 'layers',
    query: '',
    legend: true,
    wells: true,
    contours: true,
    surface: true,
    tool: 'select',
    activity: false,
    log: ['已打开 HY 演示工区'],
    draft: null,
    undone: [],
    view: null,
    refView: null,
    refLegend: true,
    showRef: false,
    scopeAll: false,
    targetKey: ''
  };
  const closedGroups = new Set();
  let main,
    ref,
    dialogAction = null,
    dialogAlternate = null,
    compareBeforeFocus = false;
  const ver = (id = st.v) => versions.find((v) => v.id === Number(id));
  const patch = () => D.patches.find((p) => p.id === st.selected);
  const fac = (k) => D.facies.find((f) => (f.key || f.id) === k);
  const fName = (k) => fac(k)?.name || '待赋相';
  const cluster = (p) => D.clusters.find((c) => c.id === p.cluster);
  const selectedState = () => (st.draft ? { product: 'manual', overrides: st.draft.at(-1) } : ver());
  function currentValue(p, v = selectedState()) {
    return v.overrides?.[p.id] ?? (v.product === 'raw' ? 'unassigned' : cluster(p)?.facies || 'unassigned');
  }
  function note(msg) {
    $('statusText').textContent = msg;
    st.log.unshift(msg);
    if (st.activity) showActivity();
  }
  function dialog(title, body, confirm, callback, alternate, alternateCallback) {
    const el = $('dialog');
    if (el.open) el.close();
    dialogAction = callback;
    dialogAlternate = alternateCallback;
    $('dialogTitle').textContent = title;
    $('dialogBody').innerHTML =
      body +
      '<div class="button-row">' +
      (confirm ? '<button class="action" data-act="cancelDialog">取消</button>' : '') +
      (alternate ? '<button class="action" data-act="alternateDialog">' + h(alternate) + '</button>' : '') +
      '<button class="action primary" data-act="' +
      (confirm ? 'confirmDialog' : 'cancelDialog') +
      '">' +
      h(confirm || '关闭') +
      '</button></div>';
    decorate(el);
    el.showModal();
  }
  function leaveDraft(then) {
    if (!st.draft) {
      then();
      return;
    }
    dialog(
      '当前有解释草稿',
      '<p>保存为新版本，或放弃本轮更改后切换图件。</p>',
      '保存并继续',
      () => {
        storeDraft();
        then();
      },
      '放弃更改',
      () => {
        st.draft = null;
        st.undone = [];
        then();
      }
    );
  }
  function storeDraft() {
    if (!st.draft) return;
    const id = Math.max(...versions.map((v) => v.id)) + 1;
    versions.push({
      id,
      name: '人工解释',
      product: 'manual',
      parent: st.v,
      overrides: { ...st.draft.at(-1) }
    });
    st.v = id;
    st.draft = null;
    st.undone = [];
    persist('已保存人工解释 v' + id);
    render();
  }
  function persist(message) {
    try {
      localStorage.setItem(storageKey, JSON.stringify(versions.slice(2)));
      note(message);
    } catch {
      note(message + '（仅本次会话）');
    }
  }
  function switchVersion(id) {
    if (st.v === id && !st.draft) {
      openDoc('map');
      return;
    }
    leaveDraft(() => {
      st.v = id;
      st.draft = null;
      st.undone = [];
      st.scopeAll = false;
      st.targetKey = '';
      st.surface = true;
      openDoc('map');
      if (st.ref === st.v) st.ref = versions.find((v) => v.id !== id).id;
      note('当前图件 · ' + ver().name + ' v' + id);
      render();
    });
  }
  function openDoc(id) {
    if (!st.docs.includes(id)) st.docs.push(id);
    st.doc = id;
    render();
  }
  function selectPatch(id) {
    if (!id || !D.patches.some((p) => p.id === id)) return;
    if (id !== st.selected) {
      st.scopeAll = false;
      st.targetKey = '';
    }
    st.selected = id;
    if (!st.focus) st.panel = 'object';
    if (innerWidth <= 730) st.left = false;
    st.doc = 'map';
    render();
    note('已选择图斑 ' + id);
  }
  function mapSettings(v) {
    return {
      product: v.product,
      overrides: v.overrides || {},
      wells: st.wells,
      contours: st.contours,
      showSurface: st.surface,
      selected: st.selected,
      tool: st.tool
    };
  }
  function applyMaps() {
    if (!main || st.doc !== 'map') return;
    main.setState({
      ...mapSettings(selectedState()),
      highlightCluster: st.panel === 'object' && st.scopeAll ? patch()?.cluster : null,
      ...(st.view ? { view: st.view } : {})
    });
    if (st.reference)
      ref.setState({
        ...mapSettings(ver(st.ref)),
        showSurface: true,
        ...(st.refView ? { view: st.refView } : {})
      });
    requestAnimationFrame(() => {
      main.redraw();
      if (st.reference) ref.redraw();
    });
  }
  function setView(v, fromRef) {
    if (fromRef) {
      st.refView = v;
      if (st.linked && st.reference) {
        st.view = v;
        main.setState({ view: v });
      }
    } else {
      st.view = v;
      if (st.linked && st.reference) {
        st.refView = v;
        ref.setState({ view: v });
      }
    }
  }
  function cursor(pt, fromRef) {
    if (!pt) {
      $('xy').textContent = '—';
    } else {
      $('xy').textContent =
        Math.round(pt.easting ?? pt.x).toLocaleString('en-US') +
        ' E   ' +
        Math.round(pt.northing ?? pt.y).toLocaleString('en-US') +
        ' N';
    }
    if (st.linked && st.reference) (fromRef ? main : ref)?.setState({ cursor: pt });
  }
  function tree() {
    const button = (cmd, name, icon, on = false, extra = '') =>
      '<button class="tree-row ' +
      (on ? 'is-current' : '') +
      '" data-act="' +
      cmd +
      '">' +
      symbol(icon) +
      '<span>' +
      name +
      '</span>' +
      extra +
      '</button>';
    const group = (title, body) => {
      if (!body) return '';
      const expanded = !closedGroups.has(title) || !!st.query;
      return (
        '<section class="tree-section"><button class="group-name group-toggle" data-group="' +
        title +
        '" aria-expanded="' +
        expanded +
        '">' +
        symbol('down') +
        title +
        '</button>' +
        (expanded ? body : '') +
        '</section>'
      );
    };
    const matches = (x) => x.toLowerCase().includes(st.query.trim().toLowerCase());
    if (st.resource === 'versions')
      return (
        '<div class="ins-block"><h3>图件版本</h3>' +
        [...versions]
          .reverse()
          .map(
            (v) =>
              '<button class="version-item" aria-current="' +
              (st.v === v.id) +
              '" data-version="' +
              v.id +
              '"><strong>' +
              h(v.name) +
              ' <span class="ver">v' +
              v.id +
              '</span></strong><small>' +
              (v.parent ? '来源 · ' + h(ver(v.parent)?.name) + ' v' + v.parent : '地震聚类 · 原始成果') +
              '</small></button>'
          )
          .join('') +
        '</div>'
      );
    const products = [...new Set([...versions].reverse().map((v) => v.product))].map((t) =>
      versions.filter((v) => v.product === t).at(-1)
    );
    if (!products.some((v) => v.id === st.v)) products.unshift(ver());
    const mapped = products
      .filter((v) => matches(v.name))
      .map(
        (v) =>
          '<button class="tree-row ' +
          (v.id === st.v ? 'is-current' : '') +
          '" data-version="' +
          v.id +
          '" aria-pressed="' +
          (v.id === st.v) +
          '"><i class="layer-sample sample-' +
          v.product +
          '"></i><span>' +
          h(v.name) +
          '</span><span class="ver">v' +
          v.id +
          '</span></button>'
      )
      .join('');
    const overlays = [
      ['wells', '井位与井名', 'well'],
      ['contours', '构造等值线', 'contour'],
      ['surface', '填色面', 'layers']
    ]
      .filter((o) => matches(o[1]))
      .map(
        ([id, label, icon]) =>
          '<button class="tree-row ' +
          (st[id] ? '' : 'is-off') +
          '" data-act="overlay-' +
          id +
          '" aria-pressed="' +
          st[id] +
          '">' +
          symbol(icon) +
          label +
          '<small>' +
          symbol(st[id] ? 'eye' : 'off') +
          '</small></button>'
      )
      .join('');
    const index = matches('图斑索引')
      ? button('index', '图斑索引', 'index', false, '<small>' + D.patches.length + '</small>')
      : '';
    const body = group('解释成果', mapped) + group('叠加图层', overlays) + group('空间对象', index);
    return (
      '<label class="search">' +
      symbol('search') +
      '<input id="filter" aria-label="查找图层" placeholder="查找图层" value="' +
      h(st.query) +
      '"></label><div id="treeResults">' +
      (body || '<p class="empty-note">没有匹配的图层</p>') +
      '</div>'
    );
  }
  function property(label, value) {
    return '<div class="property"><span>' + label + '</span><strong>' + value + '</strong></div>';
  }
  function inspector() {
    if (st.panel === 'agent') {
      $('inspectHead').innerHTML = symbol('agent') + '工作区助手';
      return (
        '<div class="ins-block"><div class="eyebrow">本地演示</div><h2>解释上下文</h2><p class="note">查看当前图件和选中图斑的来源。</p>' +
        action('ask', '生成来源摘要') +
        '</div><div id="answer" class="ins-block" hidden></div>'
      );
    }
    $('inspectHead').innerHTML = symbol('panel') + '对象属性';
    const p = patch();
    if (!p)
      return (
        '<div class="ins-block"><h3>选择图斑</h3><p class="note">在主图点选一个图斑，或从索引中选择。</p>' +
        action('index', '打开图斑索引') +
        '</div>'
      );
    const c = cluster(p),
      v = selectedState(),
      key = currentValue(p),
      supported = c?.wells?.length || 0;
    return (
      '<div class="ins-block"><div class="eyebrow">图斑 <span class="numeric">' +
      h(p.id) +
      '</span> · 簇 ' +
      p.cluster +
      '</div><h2>' +
      h(v.product === 'raw' ? '原始聚类面' : fName(key)) +
      '</h2>' +
      property('面积', '<span class="numeric">' + Number(p.areaKm2).toFixed(2) + ' km²</span>') +
      property('本斑块支持井', String(p.wells?.length || 0) + ' 口') +
      property('全簇支持井', supported + ' 口') +
      '</div>' +
      '<div class="ins-block"><h3>调整微相</h3><label class="field"><span>目标微相</span><select id="target"><option value="">选择微相…</option>' +
      D.facies
        .filter((f) => (f.key || f.id) !== 'unassigned')
        .map((f) => '<option value="' + h(f.key || f.id) + '">' + h(f.name) + '</option>')
        .join('') +
      '</select></label><label class="scope-choice"><input type="checkbox" id="whole">应用到同簇的所有图斑</label><p class="note" id="affected">本次将修改 1 个图斑。</p><button class="action primary wide" data-act="apply" disabled>应用到草稿</button></div>' +
      '<div class="ins-block"><h3>井预测依据</h3>' +
      Object.entries(c?.votes || {})
        .filter((e) => e[1] > 0)
        .map(([k, n]) => property(h(fName(k)), n + ' 票'))
        .join('') +
      (!supported ? '<p class="note">当前簇没有有效井票。</p>' : '') +
      '<details><summary>查看支持井</summary><p class="note">' +
      h((c?.wells || []).join('、') || '无') +
      '</p></details></div>' +
      (st.draft
        ? '<div class="ins-block"><h3>解释草稿</h3><p class="note">来源为 ' +
          h(ver().name) +
          ' v' +
          st.v +
          '。</p><button class="action primary wide" data-act="save">保存新版本</button></div>'
        : '')
    );
  }
  function render() {
    const oldFocus = document.activeElement;
    const focusKey = oldFocus?.matches('button')
      ? ['group', 'flow', 'doc', 'version', 'act'].find((k) => oldFocus.dataset[k] !== undefined)
      : null;
    $('app').classList.toggle('is-focused', st.focus);
    $('flows').innerHTML = flows
      .map(
        (f, i) =>
          '<button class="flow" data-flow="' +
          i +
          '" ' +
          (i === st.flow ? 'aria-current="page"' : '') +
          '><b>0' +
          (i + 1) +
          '</b>' +
          f +
          '</button>'
      )
      .join('');
    $('ribbon').hidden = !st.ribbon || st.focus;
    $('ribbon').innerHTML = commands[st.flow]
      .map(([id, icon, txt]) => '<button data-act="' + id + '">' + symbol(icon) + txt + '</button>')
      .join('');
    $('layers').hidden = !st.left || st.focus;
    $('restoreLeft').hidden = st.left || st.focus;
    $('inspect').hidden = !st.panel || st.focus || (st.doc !== 'map' && st.panel !== 'agent');
    $('inspectBody').innerHTML = inspector();
    if ($('target')) {
      $('target').value = st.targetKey;
      $('whole').checked = st.scopeAll;
      updateTarget();
    }
    $('layerTab').setAttribute('aria-selected', st.resource === 'layers');
    $('versionTab').setAttribute('aria-selected', st.resource === 'versions');
    $('resourceBody').innerHTML = tree();
    $('docs').innerHTML = st.docs
      .map(
        (d) =>
          '<button class="doc" role="tab" aria-selected="' +
          (st.doc === d) +
          '" data-doc="' +
          d +
          '">' +
          symbol(icons[d]) +
          names[d] +
          '</button>'
      )
      .join('');
    const map = st.doc === 'map',
      v = selectedState();
    $('maps').hidden = !map;
    $('otherDoc').hidden = map;
    $('refSheet').hidden = !st.reference;
    $('maps').classList.toggle('two-up', st.reference);
    $('refVersion').innerHTML = versions
      .map(
        (x) =>
          '<option ' +
          (x.id === st.ref ? 'selected ' : '') +
          'value="' +
          x.id +
          '">' +
          h(x.name) +
          ' v' +
          x.id +
          '</option>'
      )
      .join('');
    $('focusBtn').lastElementChild.textContent = st.focus ? '退出专注' : '专注';
    $('focusBtn').title = st.focus ? '退出专注' : '专注工作区';
    $('compareBtn').hidden = !map;
    $('objectBtn').hidden = !map;
    $('viewInfo').textContent = map
      ? st.draft
        ? '解释草稿 · 未保存'
        : ver().name + ' · v' + st.v
      : '演示数据';
    $('mapMeta').textContent = 'T70–T72 / S1  ·  ' + (st.draft ? '解释草稿' : 'v' + st.v);
    $('mapTitle').textContent = v.product === 'raw' ? '地震原始簇图' : '三角洲前缘微相图';
    $('legendTitle').textContent = v.product === 'raw' ? '簇号图例' : '微相图例';
    const present = new Set(D.patches.map((p) => currentValue(p)));
    const rows = v.product === 'raw' ? D.clusters : D.facies.filter((f) => present.has(f.key || f.id));
    $('legendRows').innerHTML = st.surface
      ? rows
          .map(
            (r) =>
              '<div class="legend-row"><span class="swatch" style="--swatch:' +
              h(r.color) +
              '"></span>' +
              h(r.name) +
              '</div>'
          )
          .join('')
      : '<div class="note">填色面已隐藏</div>';
    $('legendRows').hidden = !st.legend;
    document.querySelector('.legend-h').setAttribute('aria-expanded', st.legend);
    const refData = ver(st.ref),
      refKeys = new Set(D.patches.map((p) => currentValue(p, refData)));
    $('refLegendTitle').textContent = refData.product === 'raw' ? '参考 · 簇号' : '参考 · 微相';
    $('refLegendRows').innerHTML = (
      refData.product === 'raw' ? D.clusters : D.facies.filter((f) => refKeys.has(f.key))
    )
      .map(
        (r) =>
          '<div class="legend-row"><span class="swatch" style="--swatch:' +
          h(r.color) +
          '"></span>' +
          h(r.name) +
          '</div>'
      )
      .join('');
    $('refLegendRows').hidden = !st.refLegend;
    document.querySelector('[data-act=refLegend]').setAttribute('aria-expanded', st.refLegend);
    sizeCompare();
    for (const k of [
      'select',
      'pan',
      'focus',
      'compare',
      'link',
      'inspector',
      'ribbon',
      'activity',
      'agent'
    ]) {
      const on = {
        select: st.tool === 'select',
        pan: st.tool === 'pan',
        focus: st.focus,
        compare: st.reference,
        link: st.linked,
        inspector: st.panel === 'object',
        ribbon: st.ribbon,
        activity: st.activity,
        agent: st.panel === 'agent'
      }[k];
      document.querySelectorAll('[data-act="' + k + '"]').forEach((b) => b.setAttribute('aria-pressed', on));
    }
    $('mainSheet').classList.toggle('pan', st.tool === 'pan');
    for (const [k, available] of [
      ['save', !!st.draft],
      ['undo', !!st.draft],
      ['redo', st.undone.length > 0]
    ])
      document.querySelectorAll('[data-act="' + k + '"]').forEach((b) => {
        const enabled = available && map;
        b.disabled = !enabled;
        b.title = enabled
          ? { save: '保存解释版本', undo: '撤销改相', redo: '重做改相' }[k]
          : map
            ? '当前没有可' + { save: '保存的改相', undo: '撤销的改相', redo: '重做的改相' }[k]
            : '返回 QGIS 视图管理解释草稿';
      });
    $('crs').textContent = map
      ? 'EPSG:32650'
      : st.doc === 'logs'
        ? 'MD · m'
        : st.doc === 'seismic'
          ? 'TWT · ms'
          : '资料目录';
    if (!map) renderOther();
    $('activity').hidden = !st.activity;
    if (st.activity) showActivity();
    decorate();
    applyMaps();
    if (focusKey && !oldFocus.isConnected) {
      const candidate = [...document.querySelectorAll('button[data-' + focusKey + ']')].find(
        (b) => b.dataset[focusKey] === oldFocus.dataset[focusKey] && b.getClientRects().length && !b.disabled
      );
      candidate?.focus({ preventScroll: true });
    }
  }
  function showActivity() {
    $('activity').innerHTML =
      '<div class="dock-h"><strong>本次工作区活动</strong><button data-act="activity" data-icon="close" title="收起活动"></button></div><ul>' +
      st.log
        .slice(0, 8)
        .map((s) => '<li>' + h(s) + '</li>')
        .join('') +
      '</ul>';
    decorate($('activity'));
  }
  function previewWells() {
    const table =
      '<table class="data-table"><thead><tr><th>簇号</th><th>井票</th><th>建议微相</th></tr></thead><tbody>' +
      D.clusters
        .map(
          (c) =>
            '<tr><td>' +
            h(c.name) +
            '</td><td>' +
            Object.values(c.votes).reduce((a, b) => a + b, 0) +
            '</td><td>' +
            h(fName(c.facies)) +
            '</td></tr>'
        )
        .join('') +
      '</tbody></table>';
    dialog(
      '井预测赋相',
      '<p class="note">演示井预测 · ' +
        D.wells.length +
        ' 口井。每井一票，超过半数的微相成为该簇的建议；平票留空。</p>' +
        table,
      '保存井建议版本',
      () => {
        leaveDraft(() => {
          const id = Math.max(...versions.map((v) => v.id)) + 1;
          versions.push({ id, name: '井赋相解释', parent: 1, product: 'well', overrides: {} });
          st.v = id;
          st.scopeAll = false;
          st.targetKey = '';
          st.undone = [];
          st.surface = true;
          persist('已依据演示井票保存井赋相 v' + id);
          openDoc('map');
        });
      }
    );
  }
  function changeDraft() {
    const p = patch(),
      key = $('target')?.value;
    if (!p || !fac(key)) return;
    const whole = $('whole').checked,
      affected = whole ? D.patches.filter((q) => q.cluster === p.cluster) : [p];
    if (!st.draft) st.draft = [];
    let prior = st.draft.at(-1) || ver().overrides;
    if (!st.draft.length && ver().product === 'raw')
      prior = Object.fromEntries(D.patches.map((q) => [q.id, 'unassigned']));
    st.draft.push({ ...prior, ...Object.fromEntries(affected.map((q) => [q.id, key])) });
    st.undone = [];
    note('已将 ' + affected.length + ' 个图斑改为' + fName(key) + ' · 未保存');
    render();
  }
  function undo() {
    if (!st.draft) return;
    st.undone.push(st.draft.pop());
    if (!st.draft.length) st.draft = null;
    note('已撤销本次改相');
    render();
  }
  function redo() {
    if (!st.undone.length) return;
    (st.draft ||= []).push(st.undone.pop());
    note('已重做本次改相');
    render();
  }
  function changeFlow(i) {
    st.flow = i;
    st.ribbon = true;
    render();
    note('已切换至' + flows[i] + '工具');
  }
  function toggleCompare() {
    if (st.doc !== 'map') return;
    st.reference = !st.reference;
    st.showRef = false;
    if (st.reference && st.linked) st.refView = main.getView();
    render();
    note(st.reference ? '已打开只读参考图' : '已关闭参考图');
  }
  function sizeCompare() {
    const compact = st.doc === 'map' && st.reference && $('work').clientWidth < 780;
    $('maps').classList.toggle('single-compare', compact);
    $('maps').classList.toggle('show-reference', st.showRef);
    $('maps').classList.toggle('show-main', !st.showRef);
    $('swapBtn').hidden = !compact;
    $('swapBtn').textContent = st.showRef ? '切至主图' : '切至参考';
  }
  function updateTarget() {
    const p = patch();
    if (!p || !$('whole')) return;
    const affected = st.scopeAll ? D.patches.filter((q) => q.cluster === p.cluster) : [p];
    const enabled =
      !!st.targetKey &&
      affected.some((q) => currentValue(q) !== st.targetKey || selectedState().product === 'raw');
    $('affected').textContent = '本次将修改 ' + affected.length + ' 个图斑。';
    const apply = document.querySelector('[data-act=apply]');
    apply.disabled = !enabled;
    const save = $('inspectBody').querySelector('[data-act=save]');
    if (save) {
      save.classList.toggle('primary', !enabled);
      apply.classList.toggle('primary', enabled);
    }
  }
  function showIndex() {
    const opts = D.patches
      .map(
        (p) =>
          '<option value="' +
          p.id +
          '">' +
          p.id +
          '　·　簇 ' +
          p.cluster +
          '　·　' +
          h(fName(currentValue(p))) +
          '</option>'
      )
      .join('');
    dialog(
      '图斑索引',
      '<label class="field"><span>按图斑编号选择</span><select id="indexChoice">' +
        opts +
        '</select></label>',
      '选择图斑',
      () => {
        selectPatch($('indexChoice').value);
      }
    );
    $('indexChoice').value = st.selected || D.featuredPatchId || D.patches[0].id;
  }
  function showHelp() {
    dialog(
      '工作区操作',
      '<div class="help-keys">' +
        [
          ['选择工作流工具', 'Ctrl + 1…5'],
          ['切换已打开的文档', 'Ctrl + Tab'],
          ['专注工作区 / 还原', 'F8'],
          ['退出专注或关闭属性', 'Esc'],
          ['聚焦地图后选择图斑', 'Enter / ← →'],
          ['地图缩放', '滚轮 / + −'],
          ['平移地图', '平移工具 / 中键拖动'],
          ['保存解释草稿', 'Ctrl + S'],
          ['撤销 / 重做改相', 'Ctrl + Z / Y']
        ]
          .map((r) => '<span>' + r[0] + '</span><kbd>' + r[1] + '</kbd>')
          .join('') +
        '</div><p class="note">此交互原型使用合成地质数据。保存的演示版本保存在当前浏览器。</p>'
    );
  }
  const handlers = {
    ribbon() {
      st.ribbon = !st.ribbon;
      render();
    },
    left() {
      st.left = !st.left;
      if (innerWidth <= 730 && st.left) st.panel = null;
      render();
    },
    focus() {
      if (!st.focus) compareBeforeFocus = st.activity;
      st.focus = !st.focus;
      st.activity = st.focus ? false : compareBeforeFocus;
      render();
      note(st.focus ? '专注工作区 · 按 Esc 还原' : '已还原工作区布局');
    },
    layers() {
      st.resource = 'layers';
      render();
    },
    versions() {
      st.resource = 'versions';
      render();
    },
    inspector() {
      st.panel = st.panel === 'object' ? null : 'object';
      st.focus = false;
      st.showRef = false;
      if (innerWidth <= 730) st.left = false;
      render();
    },
    closeInspector() {
      st.panel = null;
      render();
    },
    legend() {
      st.legend = !st.legend;
      render();
    },
    refLegend() {
      st.refLegend = !st.refLegend;
      render();
    },
    compare: toggleCompare,
    swap() {
      st.showRef = !st.showRef;
      if (st.showRef) st.panel = null;
      render();
    },
    link() {
      st.linked = !st.linked;
      if (st.linked) st.refView = main.getView();
      else ref.setState({ cursor: null });
      render();
      note(st.linked ? '两幅图的视野与光标已联动' : '参考图可独立浏览');
    },
    select() {
      openDoc('map');
      st.tool = 'select';
      render();
      note('点选图斑查看属性和井预测依据');
    },
    pan() {
      st.tool = 'pan';
      render();
      note('拖动以平移地图');
    },
    zoomIn() {
      main.zoom(1.25);
    },
    zoomOut() {
      main.zoom(0.8);
    },
    fit() {
      main.fit();
      note('已显示全区');
    },
    'overlay-wells'() {
      st.wells = !st.wells;
      render();
    },
    'overlay-contours'() {
      st.contours = !st.contours;
      render();
    },
    'overlay-surface'() {
      st.surface = !st.surface;
      render();
    },
    wells() {
      st.wells = true;
      openDoc('map');
    },
    theme() {
      document.documentElement.classList.toggle('dark');
      $('themeText').textContent = document.documentElement.classList.contains('dark') ? '浅色' : '深色';
      render();
    },
    activity() {
      st.activity = !st.activity;
      render();
    },
    agent() {
      st.focus = false;
      st.panel = st.panel === 'agent' ? null : 'agent';
      if (innerWidth <= 730) st.left = false;
      render();
    },
    ask() {
      const p = patch(),
        v = ver();
      $('answer').hidden = false;
      $('answer').textContent =
        '当前图件为' +
        v.name +
        ' v' +
        v.id +
        '，层段 T70–T72 / S1。' +
        (p
          ? '图斑 ' +
            p.id +
            ' 属于簇 ' +
            p.cluster +
            '。本图斑 ' +
            (p.wells || []).length +
            ' 口井，全簇 ' +
            cluster(p).wells.length +
            ' 口井。'
          : '选择图斑可查看井票归属。');
    },
    logs() {
      openDoc('logs');
    },
    seismic() {
      openDoc('seismic');
    },
    catalog() {
      openDoc('catalog');
    },
    map() {
      openDoc('map');
    },
    wellMap() {
      switchVersion(versions.filter((v) => v.product === 'well').at(-1).id);
    },
    raw() {
      switchVersion(1);
    },
    save: storeDraft,
    apply: changeDraft,
    undo,
    redo,
    wellPreview: previewWells,
    index: showIndex,
    help: showHelp,
    file() {
      dialog(
        '演示工程',
        '<div class="property"><span>工区</span><strong>HY 三角洲演示工区</strong></div><p class="note">当前浏览器共有 ' +
          versions.length +
          ' 个已保存版本。可重置本地演示并恢复初始图件。</p>' +
          action('reset', '重置演示…')
      );
    },
    reset() {
      dialog(
        '重置本地演示',
        '<p>将移除这个原型保存在当前浏览器中的演示版本和未保存草稿。</p>',
        '重置演示',
        () => {
          try {
            localStorage.removeItem(storageKey);
          } catch {}
          location.reload();
        }
      );
    },
    cancelDialog() {
      $('dialog').close();
    },
    confirmDialog() {
      const f = dialogAction;
      $('dialog').close();
      f?.();
    },
    alternateDialog() {
      const f = dialogAlternate;
      $('dialog').close();
      f?.();
    }
  };
  document.addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (!b || b.disabled) return;
    if (b.dataset.act) handlers[b.dataset.act]?.();
    else if (b.dataset.flow !== undefined) changeFlow(Number(b.dataset.flow));
    else if (b.dataset.doc) openDoc(b.dataset.doc);
    else if (b.dataset.version) switchVersion(Number(b.dataset.version));
    else if (b.dataset.group) {
      const name = b.dataset.group;
      if (closedGroups.has(name)) closedGroups.delete(name);
      else closedGroups.add(name);
      render();
    }
  });
  document.addEventListener('input', (e) => {
    if (e.target.id === 'filter') {
      st.query = e.target.value;
      const t = document.createElement('div');
      t.innerHTML = tree();
      $('treeResults').innerHTML = t.querySelector('#treeResults').innerHTML;
      decorate($('treeResults'));
    }
  });
  document.addEventListener('change', (e) => {
    if (e.target.id === 'refVersion') {
      st.ref = Number(e.target.value);
      render();
      note('参考图 · ' + ver(st.ref).name + ' v' + st.ref);
    }
    if (e.target.id === 'target' || e.target.id === 'whole') {
      st.targetKey = $('target').value;
      st.scopeAll = $('whole').checked;
      updateTarget();
      applyMaps();
    }
  });
  document.addEventListener('keydown', (e) => {
    if ($('dialog').open) return;
    const ctrl = e.ctrlKey || e.metaKey,
      inField = e.target.closest('input,textarea,select,[contenteditable]');
    if (inField && !(ctrl && e.key.toLowerCase() === 's')) return;
    const key = e.key.toLowerCase();
    if (ctrl && /^[1-5]$/.test(key)) {
      e.preventDefault();
      changeFlow(Number(key) - 1);
      return;
    }
    if (ctrl && key === 'tab') {
      e.preventDefault();
      const i = st.docs.indexOf(st.doc);
      openDoc(st.docs[(i + (e.shiftKey ? -1 : 1) + st.docs.length) % st.docs.length]);
      return;
    }
    if (ctrl && ['z', 'y', 's'].includes(key) && st.doc === 'map') {
      e.preventDefault();
      if (key === 's') storeDraft();
      else if (key === 'y' || e.shiftKey) redo();
      else undo();
      return;
    }
    if (key === 'f8') {
      e.preventDefault();
      handlers.focus();
      return;
    }
    if (key === 'escape') {
      if (st.focus) handlers.focus();
      else if (st.activity) handlers.activity();
      else if (st.panel) handlers.closeInspector();
      return;
    }
    if (e.target.id === 'map') {
      if (key === 'enter' || key === 'arrowright' || key === 'arrowleft') {
        e.preventDefault();
        const i = D.patches.findIndex((p) => p.id === st.selected);
        selectPatch(
          i < 0
            ? D.featuredPatchId
            : D.patches[(i + (key === 'arrowleft' ? -1 : 1) + D.patches.length) % D.patches.length].id
        );
      }
      if (key === '+' || key === '=') main.zoom(1.25);
      if (key === '-') main.zoom(0.8);
    }
  });
  function renderOther() {
    const id = st.doc,
      $el = $('otherDoc');
    if (id === 'catalog') {
      $el.innerHTML =
        '<div class="other-h"><div class="eyebrow">HY 演示工区</div><h2>资料目录</h2></div><table class="data-table"><thead><tr><th>资料名称</th><th>类型</th><th>查看</th></tr></thead><tbody>' +
        [
          ['地震原始簇图', '分类栅格与聚类面', 'raw'],
          ['三角洲前缘微相图', '井赋相成果', 'wellMap'],
          ['HY-01 测井曲线', '井曲线', 'logs'],
          ['Inline 240', '地震剖面', 'seismic']
        ]
          .map(
            (r) => '<tr><td>' + r[0] + '</td><td>' + r[1] + '</td><td>' + action(r[2], '打开') + '</td></tr>'
          )
          .join('') +
        '</tbody></table>';
      return;
    }
    $el.innerHTML =
      '<div class="other-h"><div><div class="eyebrow">HY 演示工区 · T70–T72 / S1</div><h2>' +
      (id === 'logs' ? 'HY-01 · 单井图道' : 'Inline 240 · 地震剖面') +
      '</h2></div><span class="muted">合成示意</span></div><canvas id="auxChart" class="chart" role="img" aria-label="' +
      (id === 'logs' ? '按测深绘制的伽马与电阻率示意井道' : '按双程时间绘制的地震振幅示意剖面') +
      '"></canvas>';
    requestAnimationFrame(drawAux);
  }
  function drawAux() {
    const canvas = $('auxChart');
    if (!canvas) return;
    const { width: w, height: hh } = canvas.getBoundingClientRect();
    if (!w || !hh) return;
    const dpr = Math.min(devicePixelRatio || 1, 2);
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(hh * dpr);
    const g = canvas.getContext('2d');
    g.scale(dpr, dpr);
    g.fillStyle = '#fff';
    g.fillRect(0, 0, w, hh);
    g.font = '12px Paleo,"Microsoft YaHei UI",sans-serif';
    const l = 66,
      t = 42,
      b = hh - 26,
      r = w - 24,
      depth = b - t;
    g.fillStyle = '#24303e';
    const logs = st.doc === 'logs';
    g.fillText(logs ? 'MD / m' : 'TWT / ms', 8, 23);
    for (let i = 0; i <= 8; i++) {
      let y = t + (depth * i) / 8;
      g.fillStyle = '#5d6e80';
      g.fillText(String((logs ? 2120 : 1120) + i * (logs ? 5 : 20)), 12, y + 4);
      g.strokeStyle = '#dfe5ec';
      g.beginPath();
      g.moveTo(l, y);
      g.lineTo(r, y);
      g.stroke();
    }
    if (logs) {
      const widths = [0.32, 0.32, 0.36],
        titles = ['自然伽马 GR / API', '深电阻率 Rt / Ω·m', '微相解释'];
      let offset = l;
      widths.forEach((part, j) => {
        const tw = (r - l) * part;
        g.fillStyle = '#24303e';
        g.fillText(titles[j], offset + 8, 24);
        g.strokeStyle = '#a3b1bf';
        g.strokeRect(offset, t, tw, depth);
        if (j < 2) {
          for (let q = 1; q < 5; q++) {
            g.strokeStyle = '#edf1f5';
            g.beginPath();
            g.moveTo(offset + (tw * q) / 5, t);
            g.lineTo(offset + (tw * q) / 5, b);
            g.stroke();
          }
          g.beginPath();
          g.strokeStyle = j ? '#8b5839' : '#18796b';
          g.lineWidth = 1.5;
          for (let u = 0; u <= depth; u++) {
            let x =
              offset +
              tw *
                (0.46 +
                  0.22 * Math.sin(u / (24 + j * 8) + j) +
                  0.11 * Math.sin(u / 8.1) +
                  0.055 * Math.sin(u / 2.3 + j));
            if (!u) g.moveTo(x, t);
            else g.lineTo(x, t + u);
          }
          g.stroke();
          g.lineWidth = 1;
        } else {
          for (let k = 0; k < 8; k++) {
            const f = D.facies[[0, 0, 2, 1, 3, 0, 2, 1][k]];
            g.fillStyle = f.color;
            g.fillRect(offset + 1, t + (k * depth) / 8 + 1, tw - 2, depth / 8 - 1);
            g.fillStyle = '#24303e';
            g.fillText(f.name, offset + 8, t + ((k + 0.55) * depth) / 8);
          }
        }
        offset += tw;
      });
      return;
    }
    const traces = Math.max(28, Math.floor((r - l) / 8)),
      dx = (r - l) / traces;
    for (let i = 0; i < traces; i++) {
      const x = l + i * dx;
      g.strokeStyle = '#465560';
      g.lineWidth = 0.8;
      g.beginPath();
      for (let y = 0; y <= depth; y += 2) {
        let phase = (y / depth) * 65 + 2 * Math.sin(i / 14) + Math.sin((y / depth) * 9 + i / 22);
        let a = Math.sin(phase) * Math.cos(phase * 0.18) * dx * 0.9;
        if (!y) g.moveTo(x + a, t);
        else g.lineTo(x + a, t + y);
      }
      g.stroke();
    }
    for (const [frac, col, label] of [
      [0.29, '#c83c32', 'T70'],
      [0.72, '#18796b', 'T72']
    ]) {
      g.beginPath();
      g.strokeStyle = col;
      g.lineWidth = 2;
      for (let i = 0; i <= traces; i++) {
        const y = t + depth * frac - 8 * Math.sin(i / 14);
        if (!i) g.moveTo(l, y);
        else g.lineTo(l + i * dx, y);
      }
      g.stroke();
      g.fillStyle = col;
      g.fillText(label, l + 7, t + depth * frac - 10);
    }
    g.fillStyle = '#24303e';
    g.fillText('Crossline  320', l, 23);
    g.fillText('440', r - 26, 23);
  }
  if (!D) {
    $('mapTitle').textContent = '地图组件未加载';
    $('statusText').textContent = '请从完整原型目录打开 index.html';
    return;
  }
  main = D.create($('map'), {
    onSelect: selectPatch,
    onView: (v) => setView(v, false),
    onCursor: (p) => cursor(p, false)
  });
  ref = D.create($('refMap'), { onView: (v) => setView(v, true), onCursor: (p) => cursor(p, true) });
  const sizeObserver = new ResizeObserver(() => {
    sizeCompare();
    if (st.doc !== 'map') drawAux();
  });
  sizeObserver.observe($('work'));
  window.addEventListener('beforeunload', (e) => {
    if (st.draft) {
      e.preventDefault();
      e.returnValue = '';
    }
  });
  render();
})();
