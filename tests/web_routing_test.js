const assert = require('node:assert/strict');
const { test } = require('node:test');
const routing = require('../web/routing.js');
const fs = require('node:fs');
const vm = require('node:vm');

const nodes = [
  { id: 'output', kind: 'device', inputs: 2, outputs: 0, width: 260, height: 100 },
  { id: 'effect', kind: 'effect', inputs: 2, outputs: 2, width: 300, height: 520 },
  { id: 'input', kind: 'device', inputs: 0, outputs: 2, width: 260, height: 100 },
  { id: 'helper', kind: 'routing_helper', inputs: 16, outputs: 16, width: 260, height: 400 },
  { id: 'input2', kind: 'device', inputs: 2, outputs: 2, width: 280, height: 300 },
];

test('advanced effect templates remain registered when the Cut template already exists', () => {
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const registered = { 'audiox/effect_cut': function () {} };
  const context = vm.createContext({ window: { LiteGraph: { registered_node_types: registered,
    registerNodeType: (key, node) => { registered[key] = node; } } } });
  vm.runInContext(source.slice(source.indexOf('  const advancedEffects ='),
    source.indexOf('  state.graphCanvas.background_image =')), context);
  for (const type of ['compressor', 'limiter', 'eq', 'multiband', 'noise_reduction']) {
    const Node = registered[`audiox/effect_${type}`];
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
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
  const css = fs.readFileSync(require.resolve('../web/styles.css'), 'utf8');
  const page = html.split('data-page="routing"')[1].split('data-page="bluetooth"')[0];
  assert.doesNotMatch(page, /<aside|Routing Overview|routing-details|edge-list|route-inspector|btn-route-details/);
  assert.match(page, /id="graph-canvas"/);
  for (const id of ['btn-route-save', 'btn-route-arrange', 'btn-zoom-fit', 'btn-graph-fullscreen'])
    assert.ok(page.includes(`id="${id}"`));
  assert.doesNotMatch(source, /renderEdgesList|renderInspector|routingThingLabel|routeLabel|selectedNodeId|btn-route-details/);
  assert.doesNotMatch(css, /side-stack|show-details|btn-route-details|\.edge-list|\.edge-item/);
});

test('clicking a graph wire still removes it and marks routing unsaved without a sidebar', () => {
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const notices = [], unsaved = [];
  let redraws = 0;
  const state = { graph: { getNodeOnPos: () => null }, graphCanvas: { ds: { scale: 1 } },
    edges: [{ src: 'soundboard_out', dst: 'usb_gadget_out', srcChannel: 0, dstChannel: 0 }] };
  const context = vm.createContext({ state, linkDistance: () => 4,
    AudioxUI: { setUnsaved: (...args) => unsaved.push(args) },
    syncEdgesFromGraph: () => redraws++, setStatus: (...args) => notices.push(args) });
  vm.runInContext(source.slice(source.indexOf('function removeNearestLinkAt('),
    source.indexOf('function removeEdgeByIndex(')), context);
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
  assert.equal(positions.b.y - positions.a.y, boxes[1].height + 50);
  assertNoOverlap(boxes, positions);
  assert.deepEqual(routing.layoutNodes([...boxes].reverse(), [...routes].reverse()), positions);
});

test('unused nodes and separate routed patches use compact header-safe gaps', () => {
  const boxes = ['a', 'b', 'c', 'd', 'unused'].map(id => box(id));
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
  for (const route of routes) assert.ok(positions[route.src].x < positions[route.dst].x);
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
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const graphNodes = new Map(['a', 'b'].map(id => [id, { pos: [0, 0], size: [260, 100] }]));
  const state = { nodeByThingId: graphNodes, edges: [edge('b', 'a')], autoArrange: true,
    positions: {}, activeTab: 'routing' };
  let fitted = 0;
  const context = vm.createContext({ state, AudioxRouting: routing,
    routingNodeBoxes: () => [...graphNodes.keys()].map(id => box(id)),
    fitRoutingGraph: () => fitted++ });
  vm.runInContext(source.slice(source.indexOf('function arrangeRoutingGraph()'),
    source.indexOf('function rebuildRoutingGraph()')), context);
  context.arrangeRoutingGraph();
  assert.ok(graphNodes.get('b').pos[0] < graphNodes.get('a').pos[0]);
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
  for (const route of routes) assert.ok(positions[route.src].x < positions[route.dst].x);
  const view = routing.fitNodes(boxes.map(node => ({ ...node, ...positions[node.id] })), 800, 600);
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
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const notices = [];
  const unsaved = [];
  let graph = { ok: true, skippedEdges: 16, status: 'routing helper capacity: 128 active paths, 16 skipped path(s)' };
  const context = vm.createContext({
    routingPath: '/routing.txt',
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
