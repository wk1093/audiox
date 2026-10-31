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
