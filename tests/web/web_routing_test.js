const assert = require('node:assert/strict');
const { apiContext } = require('./support/api-context.js');
const { test } = require('node:test');
const routing = require('../../web/embedded/routing.js');
const fs = require('node:fs');
const vm = require('node:vm');

const nodes = [
  { id: 'output', kind: 'device', inputs: 2, outputs: 0, width: 260, height: 100 },
  { id: 'effect', kind: 'effect', inputs: 2, outputs: 2, width: 300, height: 520 },
  { id: 'input', kind: 'device', inputs: 0, outputs: 2, width: 260, height: 100 },
  { id: 'helper', kind: 'routing_helper', inputs: 16, outputs: 16, width: 260, height: 400 },
  { id: 'input2', kind: 'device', inputs: 2, outputs: 2, width: 280, height: 300 },
];

function canvasFixture() {
  const calls = [];
  const canvas = () => {
    const ctx = { save: () => calls.push(['save']), restore: () => calls.push(['restore']),
      setTransform: (...args) => calls.push(['transform', ...args]),
      drawImage: (...args) => calls.push(['image', ...args]) };
    return { width: 0, height: 0, style: {}, getContext: () => ctx };
  };
  const graphCanvas = { canvas: canvas(), bgcanvas: canvas(), ds: { scale: 1, offset: [0, 0],
    computeVisibleArea: (viewport) => calls.push(['viewport', ...viewport]) },
    resize(w, h) { this.canvas.width = this.bgcanvas.width = w; this.canvas.height = this.bgcanvas.height = h; },
    setDirty: (...args) => calls.push(['dirty', ...args]),
    drawBackCanvas() { calls.push(['back', this.viewport]); },
    drawFrontCanvas() { calls.push(['front', this.viewport]); this.ctx.drawImage(this.bgcanvas, 0, 0); },
  };
  return { graphCanvas, calls };
}

test('high-DPI graph renders both layers at display density while keeping logical viewport and zoom', () => {
  for (const ratio of [1, 1.25, 1.5, 2, 3]) {
    const { graphCanvas, calls } = canvasFixture();
    const resolution = routing.createCanvasResolution(graphCanvas);
    graphCanvas.ds.scale = 0.75;
    graphCanvas.ds.offset = [40, 20];
    resolution.resize(801, 451, ratio);
    assert.equal(graphCanvas.canvas.width, Math.round(801 * ratio));
    assert.equal(graphCanvas.bgcanvas.height, Math.round(451 * ratio));
    assert.deepEqual(resolution.size(), [801, 451]);
    assert.equal(graphCanvas.canvas.style.width, '801px');
    assert.equal(graphCanvas.viewport, null);
    assert.equal(graphCanvas.ds.viewport, null);
    assert.equal(graphCanvas.render_canvas_border, false);
    assert.equal(graphCanvas.ds.scale, 0.75);
    assert.deepEqual(graphCanvas.ds.offset, [40, 20]);
    graphCanvas.drawBackCanvas();
    graphCanvas.drawFrontCanvas();
    assert.deepEqual(calls.find(([method]) => method === 'back')[1], [0, 0, 801, 451]);
    assert.deepEqual(calls.find(([method]) => method === 'front')[1], [0, 0, 801, 451]);
    assert.equal(graphCanvas.viewport, null);
    const transforms = calls.filter(([method]) => method === 'transform');
    assert.equal(transforms.length, 2);
    assert.equal(transforms[0][1], Math.round(801 * ratio) / 801);
    assert.equal(transforms[1][4], Math.round(451 * ratio) / 451);
    const copy = calls.find(([method]) => method === 'image');
    assert.deepEqual(copy.slice(2), [0, 0, 801, 451]);
    assert.equal(calls.filter(([method]) => method === 'save').length, 2);
    assert.equal(calls.filter(([method]) => method === 'restore').length, 2);
    resolution.resize(3840, 2160, ratio);
    assert.equal(graphCanvas.canvas.width, Math.round(3840 * ratio));
    resolution.resize(801, 451, 1);
    assert.equal(graphCanvas.canvas.width, 801);
  }
});

test('high-DPI rendering restores context and background-copy override after an error', () => {
  const { graphCanvas, calls } = canvasFixture();
  graphCanvas.drawFrontCanvas = function () { throw new Error('render failed'); };
  const original = graphCanvas.canvas.getContext('2d').drawImage;
  const resolution = routing.createCanvasResolution(graphCanvas);
  resolution.resize(800, 450, 2);
  assert.throws(() => graphCanvas.drawFrontCanvas(), /render failed/);
  assert.equal(graphCanvas.ctx.drawImage, original);
  assert.equal(graphCanvas.viewport, null);
  assert.equal(calls.at(-1)[0], 'restore');
});

test('panning and resizing retain logical visible bounds without constraining pointer events', () => {
  const { graphCanvas, calls } = canvasFixture();
  const resolution = routing.createCanvasResolution(graphCanvas);
  resolution.resize(800, 400, 2);
  graphCanvas.ds.offset = [0, -600];
  graphCanvas.ds.computeVisibleArea();
  assert.deepEqual(calls.at(-1), ['viewport', 0, 0, 800, 400]);
  graphCanvas.drawBackCanvas();
  graphCanvas.drawFrontCanvas();
  assert.equal(graphCanvas.viewport, null);
  assert.equal(graphCanvas.ds.viewport, null);
  resolution.resize(500, 300, 1.25);
  graphCanvas.ds.computeVisibleArea();
  assert.deepEqual(calls.at(-1), ['viewport', 0, 0, 500, 300]);
  assert.deepEqual(graphCanvas.ds.offset, [0, -600]);
  assert.equal(graphCanvas.viewport, null);
});

test('display density changes resize the graph and rearm the resolution listener', () => {
  const source = require('./support/app-source.js').appSource();
  const queries = [];
  let resized = 0;
  const context = apiContext({ window: { devicePixelRatio: 1,
    matchMedia(query) { const entry = { query }; queries.push(entry); return {
      addEventListener(type, callback, options) { Object.assign(entry, { type, callback, options }); },
    }; } }, resizeRoutingCanvas: () => resized++ });
  vm.runInContext(source.slice(source.indexOf('function watchRoutingPixelRatio()'),
    source.indexOf("if (typeof ResizeObserver === 'function')")), context);
  assert.equal(queries[0].query, '(resolution: 1dppx)');
  context.window.devicePixelRatio = 2;
  queries[0].callback();
  assert.equal(resized, 1);
  assert.equal(queries[1].query, '(resolution: 2dppx)');
  assert.equal(queries[1].options.once, true);
});

test('advanced effect templates remain registered when the Cut template already exists', () => {
  const source = require('./support/app-source.js').appSource();
  const registered = { 'effects/cut': function () {} };
  const context = apiContext({ window: { LiteGraph: { registered_node_types: registered,
    registerNodeType: (key, node) => { registered[key] = node; } } } });
  vm.runInContext(source.slice(source.indexOf('  const advancedEffects ='),
    source.indexOf('  state.graphCanvas.background_image =')), context);
  for (const type of ['compressor', 'limiter', 'eq', 'multiband', 'noise_reduction']) {
    const Node = registered[`effects/${type}`];
    assert.equal(typeof Node, 'function');
    const node = Object.create(Node.prototype);
    const inputs = [], outputs = [];
    node.addInput = (...args) => inputs.push(args);
    node.addOutput = (...args) => outputs.push(args);
    Node.call(node);
    assert.equal(node.properties.effectTypeTemplate, type);
    assert.equal(inputs.length, 2);
    assert.equal(outputs.length, 2);
  }
});
test('routing channels use mono, left/right, or one-based channel names', () => {
  assert.equal(routing.channelLabel(0, 1), 'Mono');
  assert.equal(routing.channelLabel(0, 2), 'Left');
  assert.equal(routing.channelLabel(1, 2), 'Right');
  assert.equal(routing.channelLabel(7, 8), 'Channel 8');
});

test('routing is graph-only with no technical sidebar or Details toggle', () => {
  const source = require('./support/app-source.js').appSource();
  const html = fs.readFileSync(require.resolve('../../web/embedded/index.html'), 'utf8');
  const css = fs.readFileSync(require.resolve('../../web/embedded/styles.css'), 'utf8');
  const page = html.split('data-page="routing"')[1].split('data-page="bluetooth"')[0];
  assert.doesNotMatch(page, /<aside|Routing Overview|routing-details|edge-list|route-inspector|btn-route-details/);
  assert.match(page, /id="graph-canvas"/);
  for (const id of ['btn-route-save', 'btn-route-arrange', 'btn-zoom-fit', 'btn-graph-fullscreen'])
    assert.ok(page.includes(`id="${id}"`));
  assert.doesNotMatch(source, /renderEdgesList|renderInspector|routingThingLabel|routeLabel|selectedNodeId|btn-route-details/);
  assert.doesNotMatch(css, /side-stack|show-details|btn-route-details|\.edge-list|\.edge-item/);
});

test('clicking a graph wire still removes it and marks routing unsaved without a sidebar', () => {
  const source = require('./support/app-source.js').appSource();
  const notices = [], unsaved = [];
  let redraws = 0;
  const state = { graph: { getNodeOnPos: () => null }, graphCanvas: { ds: { scale: 1 } },
    edges: [{ src: 'soundboard_out', dst: 'usb_gadget_out', srcChannel: 0, dstChannel: 0 }] };
  const context = apiContext({ state, linkDistance: () => 4,
    AudioxUI: { setUnsaved: (...args) => unsaved.push(args) },
    syncEdgesFromGraph: () => redraws++, recordRoutingHistory: () => {}, setStatus: (...args) => notices.push(args) });
  const slice = (start, end) => source.slice(source.indexOf(start), source.indexOf(end));
  vm.runInContext([slice('function edgeKey(', 'function findEdgeIndex('),
    slice('function routingWires(', 'function addRoutingSlots('),
    slice('function removeRoutingWire(', 'function removeVirtualRoutingNode('),
    slice('function removeNearestLinkAt(', 'function removeEdgeByIndex(')].join('\n'), context);
  assert.equal(context.removeNearestLinkAt(100, 100), true);
  assert.equal(state.edges.length, 0);
  assert.equal(redraws, 1);
  assert.deepEqual(unsaved, [['routing']]);
  assert.match(notices[0][0], /route removed.*not saved/);
  assert.equal(context.removeNearestLinkAt(100, 100), false);
});

test('default graph layout uses input, helper, effect, output columns regardless of API order', () => {
  const positions = routing.layoutNodes(nodes);
  assert.equal(positions.input.x, positions.input2.x);
  assert.ok(positions.helper.x > positions.input2.x + 280);
  assert.ok(positions.effect.x > positions.helper.x + 260);
  assert.ok(positions.output.x > positions.effect.x + 300);
  assert.ok(positions.input2.y > positions.input.y + 100 + 30);
  assert.deepEqual(routing.layoutNodes(nodes), positions);
});

test('layout handles empty columns and large nodes without overlapping headers', () => {
  assert.deepEqual(routing.layoutNodes([]), {});
  const effects = [
    { ...nodes[1], id: 'fx_1' },
    { ...nodes[1], id: 'fx_2', width: 600 },
    nodes[0],
  ];
  const positions = routing.layoutNodes(effects);
  assert.equal(positions.fx_1.x, positions.fx_2.x);
  assert.ok(positions.fx_2.y >= positions.fx_1.y + effects[0].height + 30);
  assert.ok(positions.output.x >= positions.fx_2.x + effects[1].width + 30);
});

const edge = (src, dst) => ({ src, dst, srcChannel: 0, dstChannel: 0 });
const box = (id, width = 260, height = 100) =>
  ({ id, kind: 'effect', inputs: 2, outputs: 2, width, height });

test('wire picking samples the same smooth cubic used for drawing, including vertical and reverse routes', () => {
  for (const [a, b] of [[[300, 135], [840, 435]], [[300, 75], [40, 225]],
    [[300, 375], [40, 75]], [[300, 75], [300, 75]]]) {
    const path = routing.wirePath(a, b);
    const curve = routing.wireCurve(a, b);
    assert.equal(path.length, 49);
    assert.deepEqual(path[0], a);
    assert.deepEqual(path.at(-1), b);
    assert.deepEqual(path[24], a.map((value, axis) =>
      (value + 3 * curve[1][axis] + 3 * curve[2][axis] + b[axis]) / 8));
    assert.ok(path.every(point => point.every(Number.isFinite)));
  }
});

test('screenshot-style microphone chain keeps compact vertical effect pairs', () => {
  const boxes = [
    { ...box('mic', 200, 90), kind: 'device' }, box('mute', 280, 170),
    box('noise', 280, 330), box('gate', 280, 260), box('pitch', 280, 330),
    box('reverb', 280, 260), box('monitor', 280, 170), box('phoneMute', 280, 170),
    { ...box('phone', 200, 90), kind: 'device' }, { ...box('computer', 200, 90), kind: 'device' },
    { ...box('soundboard', 200, 90), kind: 'device' },
    { ...box('mixer', 200, 90), kind: 'routing_helper' },
    { ...box('usbOut', 200, 90), kind: 'device' }, { ...box('headphones', 200, 90), kind: 'device' },
  ];
  const routes = [edge('mic', 'mute'), edge('mute', 'noise'), edge('noise', 'gate'),
    edge('gate', 'pitch'), edge('pitch', 'reverb'), edge('reverb', 'monitor'),
    edge('reverb', 'usbOut'), edge('monitor', 'mixer'), edge('phone', 'phoneMute'),
    edge('phoneMute', 'usbOut'), edge('phone', 'mixer'), edge('computer', 'mixer'),
    edge('soundboard', 'usbOut'), edge('soundboard', 'mixer'), edge('mixer', 'headphones')];
  const positions = routing.layoutNodes(boxes, routes);
  assert.equal(positions.mute.x, positions.noise.x);
  assert.equal(positions.gate.x, positions.pitch.x);
  assert.ok(positions.noise.y > positions.mute.y);
  assertNoOverlap(boxes, positions);
  const placed = boxes.map(node => ({ ...node, ...positions[node.id] }));
  const width = Math.max(...placed.map(node => node.x + node.width)) - 40;
  assert.ok(width < 2400, `graph still too wide: ${width}`);
  for (const route of routes) {
    const src = placed.find(node => node.id === route.src);
    const dst = placed.find(node => node.id === route.dst);
    for (let channel = 0; channel < 2; channel++) {
      const a = [src.x + src.width, src.y + 15 + channel * 20];
      const b = [dst.x, dst.y + 15 + channel * 20];
      const path = routing.wirePath(a, b);
      assert.deepEqual(path[0], a);
      assert.deepEqual(path.at(-1), b);
    }
  }
});

test('wire paths stay stable when unrelated panels move and refresh when sockets move', () => {
  const source = require('./support/app-source.js').appSource();
  const boxes = [{ ...box('src'), x: 40, y: 60 }, { ...box('dst'), x: 600, y: 60 }];
  const destination = [600, 75];
  const state = { edges: [edge('src', 'dst')], nodeByThingId: new Map([
    ['src', { getConnectionPos: () => [300, 75] }],
    ['dst', { getConnectionPos: () => destination }],
  ]) };
  let calls = 0;
  const wire = { key: 'src:dst', type: 'mono', edges: state.edges, src: { id: 'src', slot: 0 }, dst: { id: 'dst', slot: 0 } };
  const context = apiContext({ state, routingNodeBoxes: () => boxes,
    routingWires: () => [wire], routingNodeById: id => state.nodeByThingId.get(id),
    AudioxRouting: { wirePath: (...args) => { calls++; return routing.wirePath(...args); } } });
  vm.runInContext(source.slice(source.indexOf("let routingWireCacheKey ="),
    source.indexOf('function removeNearestLinkAt(')), context);
  assert.equal(context.linkDistance(wire, 450, 75), 0);
  assert.equal(context.linkDistance(wire, 450, 75), 0);
  assert.equal(calls, 1);
  boxes.push({ ...box('obstacle', 160), x: 340, y: 60 });
  assert.equal(context.linkDistance(wire, 450, 75), 0);
  assert.equal(calls, 1);
  boxes[2].y = -100;
  assert.equal(context.linkDistance(wire, 450, 75), 0);
  assert.equal(calls, 1);
  destination[1] = 275;
  assert.ok(context.linkDistance(wire, 450, 75) > 20);
  assert.equal(calls, 2);
  assert.match(source, /const paths = routingWirePaths\(\)/);
  assert.match(source, /ctx\.bezierCurveTo\(first\[0\], first\[1\], second\[0\], second\[1\], b\[0\], b\[1\]\)/);
});

test('curved routes highlight selected node connections and restore canvas state', () => {
  const source = require('./support/app-source.js').appSource();
  const routes = [edge('src', 'dst'), edge('src', 'other'), edge('other', 'src'),
    { ...edge('src', 'dst'), srcChannel: 1, dstChannel: 1 }, edge('other', 'unrelated')];
  const state = { edges: routes, selectedEdgeIdx: 4, graphCanvas: {} };
  const key = edge => `${edge.src}:${edge.srcChannel}:${edge.dst}:${edge.dstChannel}`;
  const paths = new Map(routes.map(route => [key(route), routing.wirePath([300, 75], [600, 275])]));
  const wires = routes.map(route => ({ key: key(route), type: 'mono', edges: [route],
    src: { id: route.src, slot: route.srcChannel }, dst: { id: route.dst, slot: route.dstChannel } }));
  const context = apiContext({ state, AudioxRouting: routing,
    ROUTING_WIRE_COLORS: { stereo: '#c3a6ff', mono: '#79c8ff' },
    routingViewId: node => String(node?.properties?.thingId || node?.properties?.virtualId || '').trim(),
    routingWires: () => wires, routingWirePaths: () => paths });
  vm.runInContext(source.slice(source.indexOf('  state.graphCanvas.onDrawForeground ='),
    source.indexOf('  state.graphCanvas.onMouse =')), context);
  const strokes = [], curves = [];
  let savedAlpha;
  const ctx = { globalAlpha: 1, save() { savedAlpha = this.globalAlpha; },
    restore() { this.globalAlpha = savedAlpha; }, beginPath() {}, moveTo() {},
    bezierCurveTo(...points) { curves.push(points); },
    stroke() { strokes.push({ alpha: this.globalAlpha, color: this.strokeStyle,
      width: this.lineWidth }); } };
  const normal = { alpha: 0.45, color: '#79c8ff', width: 3 };
  const highlighted = { alpha: 0.85, color: '#ffcf66', width: 4 };
  state.graphCanvas.onDrawForeground(ctx);
  assert.deepEqual(strokes, routes.map(() => normal), 'legacy route selection does not highlight wires');
  assert.equal(curves.length, routes.length);
  assert.deepEqual(curves[0], routing.wireCurve([300, 75], [600, 275]).slice(1).flat());
  assert.equal(ctx.globalAlpha, 1);
  const drawWithSelection = selection => {
    state.graphCanvas.selected_nodes = selection;
    strokes.length = 0;
    state.graphCanvas.onDrawForeground(ctx);
    assert.equal(ctx.globalAlpha, 1);
    return strokes;
  };
  assert.deepEqual(drawWithSelection({ 42: { properties: { thingId: 'src' } } }),
    [highlighted, highlighted, highlighted, highlighted, normal]);
  assert.deepEqual(drawWithSelection({ 99: { properties: { thingId: 'dst' } } }),
    [highlighted, normal, normal, highlighted, normal]);
  assert.deepEqual(drawWithSelection({ 42: { properties: { thingId: 'src' } },
    77: { properties: { thingId: 'unrelated' } } }), routes.map(() => highlighted));
  assert.deepEqual(drawWithSelection({}), routes.map(() => normal));
  assert.deepEqual(drawWithSelection({ 10: { properties: {} } }), routes.map(() => normal));
  wires[0].type = 'stereo';
  const strokesWithStereo = drawWithSelection({});
  assert.deepEqual(strokesWithStereo[0], { alpha: 0.6, color: '#c3a6ff', width: 4.5 });
  assert.deepEqual(strokesWithStereo[1], normal);
});

function assertNoOverlap(boxes, positions) {
  for (const node of boxes) {
    assert.ok(Number.isFinite(positions[node.id].x) && Number.isFinite(positions[node.id].y));
    for (const other of boxes) {
      if (other === node) continue;
      const a = positions[node.id], b = positions[other.id];
      assert.ok(a.x + node.width <= b.x || b.x + other.width <= a.x ||
        a.y + node.height + 30 <= b.y || b.y + other.height + 30 <= a.y,
      `${node.id} overlaps ${other.id}`);
    }
  }
}

function crossingCount(routes, positions) {
  let count = 0;
  for (let i = 0; i < routes.length; i++) {
    const a = routes[i], src = positions[a.src], dst = positions[a.dst];
    for (let j = i + 1; j < routes.length; j++) {
      const b = routes[j], otherSrc = positions[b.src], otherDst = positions[b.dst];
      if (src.x === otherSrc.x && dst.x === otherDst.x &&
          (src.y - otherSrc.y) * (dst.y - otherDst.y) < 0) count++;
    }
  }
  return count;
}

test('route-aware Arrange places each serial effect after its actual predecessor', () => {
  const chain = [nodes[2], box('fx_a', 600, 520), nodes[3], box('fx_b', 400, 700), nodes[0]];
  const routes = chain.slice(1).map((node, i) => edge(chain[i].id, node.id));
  const positions = routing.layoutNodes([...chain].reverse(), routes);
  for (let i = 1; i < chain.length; i++)
    assert.equal(positions[chain[i].id].x, positions[chain[i - 1].id].x + chain[i - 1].width + 60);
  assertNoOverlap(chain, positions);
  assert.deepEqual(routing.layoutNodes(chain, [...routes].reverse()), positions);
});

test('branch ordering measurably removes avoidable wire crossings', () => {
  const boxes = ['start', 'a', 'b', 'c', 'd', 'finish'].map(id => box(id));
  const routes = [edge('start', 'a'), edge('start', 'b'), edge('a', 'd'), edge('b', 'c'),
    edge('c', 'finish'), edge('d', 'finish')];
  const naive = { start: { x: 0, y: 0 }, a: { x: 1, y: 0 }, b: { x: 1, y: 1 },
    c: { x: 2, y: 0 }, d: { x: 2, y: 1 }, finish: { x: 3, y: 0 } };
  assert.equal(crossingCount(routes, naive), 1);
  const positions = routing.layoutNodes(boxes, routes);
  assert.equal(crossingCount(routes, positions), 0);
  assert.equal(positions.d.y - positions.a.y, boxes[1].height + 50);
  assert.equal(positions.a.x, positions.d.x);
  assertNoOverlap(boxes, positions);
  assert.deepEqual(routing.layoutNodes([...boxes].reverse(), [...routes].reverse()), positions);
});

test('unused nodes and separate routed patches use compact header-safe gaps', () => {
  const boxes = ['a', 'b', 'c', 'd', 'unused'].map(id => ({ ...box(id), kind: 'device' }));
  const positions = routing.layoutNodes(boxes, [edge('a', 'b'), edge('c', 'd')]);
  assert.equal(positions.c.y - (positions.a.y + boxes[0].height), 70);
  assert.equal(positions.unused.y - (positions.c.y + boxes[2].height), 70);
  const fallback = routing.layoutNodes(boxes);
  assert.equal(fallback.b.y - (fallback.a.y + boxes[0].height), 50);
  assertNoOverlap(boxes, positions);
  assertNoOverlap(boxes, fallback);
});

test('long routes, fan-in/out and duplicate stereo wires retain stable placement', () => {
  const boxes = ['start', 'a', 'b', 'c', 'd', 'finish'].map((id, i) => box(id, 260 + i * 20, 80 + i * 90));
  const routes = [edge('start', 'a'), edge('start', 'b'), edge('a', 'c'), edge('b', 'd'),
    edge('c', 'finish'), edge('d', 'finish'), edge('start', 'finish'), edge('a', 'finish')];
  const positions = routing.layoutNodes(boxes, routes);
  for (const route of routes) assert.ok(positions[route.src].x < positions[route.dst].x ||
    positions[route.src].x === positions[route.dst].x && positions[route.src].y < positions[route.dst].y);
  assertNoOverlap(boxes, positions);
  const stereo = routes.flatMap(route => [route, { ...route, srcChannel: 1, dstChannel: 1 }]);
  assert.deepEqual(routing.layoutNodes(boxes, stereo), positions);
  assert.deepEqual(routing.layoutNodes(boxes, [...routes, edge('missing', 'a'), edge('b', 'missing'),
    edge('c', 'c')]), positions);
});

test('cyclic groups, disconnected patches and unused nodes remain finite and separated', () => {
  const boxes = ['a', 'b', 'c', 'd', 'e', 'f', 'unused'].map(id => box(id));
  const routes = [edge('a', 'b'), edge('b', 'c'), edge('c', 'b'), edge('c', 'd'), edge('e', 'f')];
  const positions = routing.layoutNodes(boxes, routes);
  assert.equal(positions.b.x, positions.c.x);
  assert.ok(positions.a.x < positions.b.x && positions.c.x < positions.d.x);
  assert.ok(positions.e.y > Math.max(...['a', 'b', 'c', 'd'].map(id => positions[id].y + 100)));
  assert.ok(positions.unused.y > Math.max(positions.e.y, positions.f.y) + 100);
  assertNoOverlap(boxes, positions);
  assert.deepEqual(routing.layoutNodes(boxes, routes), positions);
  const cycle = routing.layoutNodes(boxes.slice(0, 3), [edge('a', 'b'), edge('b', 'c'), edge('c', 'a')]);
  assert.equal(cycle.a.x, cycle.b.x);
  assert.equal(cycle.b.x, cycle.c.x);
  assertNoOverlap(boxes.slice(0, 3), cycle);
});

test('Arrange uses current unsaved routes without overwriting manually positioned nodes', () => {
  const source = require('./support/app-source.js').appSource();
  const graphNodes = new Map(['a', 'b'].map(id => [id, { pos: [0, 0], size: [260, 100] }]));
  const state = { nodeByThingId: graphNodes, edges: [edge('b', 'a')], autoArrange: true,
    positions: {}, activeTab: 'routing' };
  let fitted = 0;
  const context = apiContext({ state, AudioxRouting: routing,
    routingGraphNodes: () => [...graphNodes],
    routingNodeBoxes: () => [...graphNodes.keys()].map(id => box(id)),
    fitRoutingGraph: () => fitted++ });
  vm.runInContext(source.slice(source.indexOf('function arrangeRoutingGraph()'),
    source.indexOf('function rebuildRoutingGraph()')), context);
  context.arrangeRoutingGraph();
  assert.equal(graphNodes.get('b').pos[0], graphNodes.get('a').pos[0]);
  assert.ok(graphNodes.get('b').pos[1] < graphNodes.get('a').pos[1]);
  assert.equal(fitted, 1);
  state.autoArrange = false;
  state.positions = { a: { x: 123, y: 456 }, b: { x: 789, y: 100 } };
  context.arrangeRoutingGraph();
  assert.deepEqual(Array.from(graphNodes.get('a').pos), [123, 456]);
  assert.deepEqual(Array.from(graphNodes.get('b').pos), [789, 100]);
  assert.equal(fitted, 1);
  assert.deepEqual(state.edges, [edge('b', 'a')]);
});

test('maximum-size graphs with many long routes keep every node ordered and nonoverlapping', () => {
  const boxes = Array.from({ length: 64 }, (_, i) => box(`n${String(i).padStart(2, '0')}`, 260, 100 + i * 7));
  const routes = boxes.slice(1).map((node, i) => edge(boxes[i].id, node.id));
  for (let i = 0; i < 65; i++) routes.push(edge(boxes[i % 32].id, boxes[63 - i % 16].id));
  const positions = routing.layoutNodes(boxes, routes);
  assert.equal(Object.keys(positions).length, 64);
  assertNoOverlap(boxes, positions);
  for (const route of routes) assert.ok(positions[route.src].x < positions[route.dst].x ||
    positions[route.src].x === positions[route.dst].x && positions[route.src].y < positions[route.dst].y);
  const placed = boxes.map(node => ({ ...node, ...positions[node.id] }));
  const byId = new Map(placed.map(node => [node.id, node]));
  for (const route of routes) {
    const src = byId.get(route.src), dst = byId.get(route.dst);
    const path = routing.wirePath([src.x + src.width, src.y + 15], [dst.x, dst.y + 15]);
    assert.ok(path.every(point => point.every(Number.isFinite)));
  }
  const view = routing.fitNodes(placed, 800, 600);
  assert.ok(Number.isFinite(view.scale) && view.scale > 0);
});

test('fit contains every node and header within desktop and mobile graph viewports', () => {
  const positions = routing.layoutNodes(nodes);
  const boxes = nodes.map((node) => ({ ...node, ...positions[node.id] }));
  for (const [width, height] of [[1200, 700], [300, 450], [1800, 300]]) {
    const view = routing.fitNodes(boxes, width, height);
    assert.ok(view.scale > 0 && view.scale <= 1);
    for (const node of boxes) {
      assert.ok((node.x + view.offset[0]) * view.scale >= 23);
      assert.ok((node.y - 30 + view.offset[1]) * view.scale >= 23);
      assert.ok((node.x + node.width + view.offset[0]) * view.scale <= width - 23);
      assert.ok((node.y + node.height + view.offset[1]) * view.scale <= height - 23);
    }
  }
  assert.equal(routing.fitNodes([], 500, 300), null);
  assert.equal(routing.fitNodes(boxes, 0, 300), null);
});

test('fit remains contained for tiny viewports and very large layouts', () => {
  const box = { x: -1000, y: 2000, width: 100000, height: 100000 };
  for (const [width, height] of [[30, 20], [1, 1], [800, 600]]) {
    const view = routing.fitNodes([box], width, height);
    assert.ok(view.scale > 0);
    assert.ok((box.x + view.offset[0]) * view.scale >= 0);
    assert.ok((box.y - 30 + view.offset[1]) * view.scale >= 0);
    assert.ok((box.x + box.width + view.offset[0]) * view.scale <= width);
    assert.ok((box.y + box.height + view.offset[1]) * view.scale <= height);
  }
});

test('saving routing preserves capacity warnings instead of overwriting them with success', async () => {
  const source = require('./support/app-source.js').appSource();
  const notices = [];
  const unsaved = [];
  let graph = { ok: true, skippedEdges: 16, status: 'routing helper capacity: 128 active paths, 16 skipped path(s)' };
  const context = apiContext({
    routingPath: '/routing.txt',
    state: { edges: [], dormantEdges: [] },
    routingHistory: { saved: () => unsaved.push(['routing', false]) },
    formatRoutingText: () => 'saved wires',
    setStatus: (...args) => notices.push(args),
    AudioxUI: { setUnsaved: (...args) => unsaved.push(args) },
    fetch: async url => ({ ok: true, text: async () => url === '/routing.txt' ? '' : JSON.stringify(graph) }),
  });
  vm.runInContext(source.slice(source.indexOf('async function reloadRoutingFast('),
    source.indexOf('async function loadVersion(')), context);
  assert.equal(await context.saveRouting(), true);
  assert.deepEqual(notices.at(-1), [`routing loaded with warnings: ${graph.status}`, 'warn']);
  assert.deepEqual(unsaved.at(-1), ['routing', false]);
  graph = { ok: true, skippedEdges: 0 };
  assert.equal(await context.saveRouting(), true);
  assert.deepEqual(notices.at(-1), ['routing saved and reloaded', 'ok']);
  context.fetch = async () => ({ ok: true, text: async () => 'invalid JSON' });
  assert.equal(await context.reloadRoutingFast(), false);
  assert.match(notices.at(-1)[0], /routing reload error/);
});

const stereoThings = [
  { id: 'mic', inputs: 0, outputs: 2 }, { id: 'fx', inputs: 2, outputs: 2 },
  { id: 'out', inputs: 2, outputs: 0 }, { id: 'mono', inputs: 1, outputs: 1 },
  { id: 'multi', inputs: 4, outputs: 4 },
];
const route = (src, dst, srcChannel, dstChannel) => ({ src, dst, srcChannel, dstChannel });
const viewOf = (edges, extra = {}) => {
  let seq = 0;
  return routing.buildRoutingView({ things: stereoThings, edges, nextId: kind => `v:${kind}:${++seq}`, ...extra });
};

test('simple routing view pairs left/right edges into one stereo wire', () => {
  const view = viewOf([route('mic', 'fx', 0, 0), route('mic', 'fx', 1, 1), route('fx', 'out', 0, 0), route('fx', 'out', 1, 1)]);
  assert.deepEqual(view.wires.map(w => [w.type, w.src.id, w.dst.id, w.edges.length]),
    [['stereo', 'mic', 'fx', 2], ['stereo', 'fx', 'out', 2]]);
  assert.equal(view.virtuals.length, 0);
  assert.deepEqual(view.slots.get('fx').inputs, [{ label: 'Stereo', type: 'stereo', channels: [0, 1] }]);
  assert.deepEqual(view.slots.get('mono').inputs.map(s => s.type), ['mono']);
  assert.deepEqual(view.slots.get('multi').outputs.map(s => s.type), ['mono', 'mono', 'mono', 'mono']);
});

test('non-pairwise stereo routes go through automatic split and merge nodes', () => {
  const view = viewOf([route('mic', 'fx', 0, 0), route('mic', 'fx', 0, 1)]);
  assert.deepEqual(view.virtuals.map(v => [v.kind, v.thing, v.auto]), [['split', 'mic', true], ['merge', 'fx', true]]);
  const mono = view.wires.filter(w => w.type === 'mono');
  assert.deepEqual(mono.map(w => [w.src, w.dst]), [
    [{ id: 'v:split:1', slot: 0 }, { id: 'v:merge:2', slot: 0 }],
    [{ id: 'v:split:1', slot: 0 }, { id: 'v:merge:2', slot: 1 }],
  ]);
  const bindings = view.wires.filter(w => w.binding);
  assert.deepEqual(bindings.map(w => [w.src.id, w.dst.id, w.edges.length]),
    [['mic', 'v:split:1', 2], ['v:merge:2', 'fx', 2]]);
  assert.equal(view.slots.get('v:split:1').outputs.length, 2);
});

test('routes to mono and multichannel sockets stay per channel', () => {
  const view = viewOf([route('mic', 'mono', 1, 0), route('multi', 'multi', 2, 3)]);
  assert.deepEqual(view.wires.filter(w => !w.binding).map(w => [w.type, w.src, w.dst]), [
    ['mono', { id: 'v:split:1', slot: 1 }, { id: 'mono', slot: 0 }],
    ['mono', { id: 'multi', slot: 2 }, { id: 'multi', slot: 3 }],
  ]);
});

test('advanced routing view keeps every channel separate and drops virtual nodes', () => {
  const view = viewOf([route('mic', 'fx', 0, 0), route('mic', 'fx', 1, 1), route('mic', 'fx', 0, 1)],
    { advanced: true, virtuals: [{ id: 'v:split:9', kind: 'split', thing: 'mic' }] });
  assert.equal(view.virtuals.length, 0);
  assert.ok(view.wires.every(w => w.type === 'mono' && !w.binding));
  assert.equal(view.wires.length, 3);
  assert.equal(view.slots.get('fx').inputs.length, 2);
});

test('duplicate or invalid split and merge nodes are left unbound', () => {
  const view = viewOf([], { virtuals: [
    { id: 'a', kind: 'split', thing: 'mic' }, { id: 'b', kind: 'split', thing: 'mic' },
    { id: 'c', kind: 'merge', thing: 'mono' }, { id: 'd', kind: 'bogus', thing: 'mic' },
  ] });
  assert.deepEqual(view.virtuals.map(v => [v.id, v.thing]), [['a', 'mic'], ['b', null], ['c', null]]);
  assert.deepEqual(view.wires.map(w => w.key), ['bind|a']);
});

test('stereo connections resolve to backend edges or split/merge bindings', () => {
  const ctx = { things: stereoThings, virtuals: [
    { id: 's', kind: 'split', thing: 'mic' }, { id: 'm', kind: 'merge', thing: null },
    { id: 's2', kind: 'split', thing: null },
  ] };
  const resolve = (src, dst) => routing.resolveRoutingConnection(ctx, src, dst);
  assert.deepEqual(resolve({ id: 'mic', slot: 0 }, { id: 'fx', slot: 0 }).edges,
    [route('mic', 'fx', 0, 0), route('mic', 'fx', 1, 1)]);
  assert.deepEqual(resolve({ id: 's', slot: 1 }, { id: 'mono', slot: 0 }).edges, [route('mic', 'mono', 1, 0)]);
  assert.deepEqual(resolve({ id: 'm', slot: 0 }, { id: 'out', slot: 0 }), { bind: { id: 'm', thing: 'out' } });
  assert.deepEqual(resolve({ id: 'fx', slot: 0 }, { id: 's2', slot: 0 }), { bind: { id: 's2', thing: 'fx' } });
  assert.match(resolve({ id: 'mic', slot: 0 }, { id: 's2', slot: 0 }).error, /already has a split/);
  assert.match(resolve({ id: 'mono', slot: 0 }, { id: 's2', slot: 0 }).error, /stereo socket/);
  assert.match(resolve({ id: 's', slot: 0 }, { id: 'm', slot: 0 }).error, /stereo wire to this merge/);
  assert.match(resolve({ id: 'm', slot: 0 }, { id: 's', slot: 0 }).error, /straight to its destination/);
  const bound = { ...ctx, virtuals: [ctx.virtuals[0], { id: 'm', kind: 'merge', thing: 'fx' }] };
  assert.deepEqual(routing.resolveRoutingConnection(bound, { id: 's', slot: 0 }, { id: 'm', slot: 1 }).edges,
    [route('mic', 'fx', 0, 1)]);
  assert.match(resolve({ id: 'mono', slot: 0 }, { id: 'm', slot: 1 }).error, /stereo wire to this merge/);
  assert.match(resolve({ id: 'mono', slot: 0 }, { id: 'fx', slot: 0 }).error, /split or merge/);
  assert.deepEqual(routing.resolveRoutingConnection({ ...ctx, advanced: true },
    { id: 'mic', slot: 1 }, { id: 'fx', slot: 0 }).edges, [route('mic', 'fx', 1, 0)]);
});
