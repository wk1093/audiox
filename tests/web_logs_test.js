const assert = require('node:assert/strict');
const { test } = require('node:test');
const { create, createHealthNotifier } = require('../web/logs.js');

class Element extends EventTarget {
  constructor(text = '') {
    super();
    this.textContent = text;
    this.dataset = {};
    this.children = [];
    this.scrollHeight = 1000;
    this.clientHeight = 100;
    this.scrollTop = 900;
  }
  get firstChild() { return this.children[0] || null; }
  get nextSibling() {
    const siblings = this.parent?.children || [];
    return siblings[siblings.indexOf(this) + 1] || null;
  }
  contains(node) { return node === this || this.children.some((child) => child.contains(node)); }
  append(...nodes) { for (const node of nodes) this.insertBefore(node, null); }
  insertBefore(node, cursor) {
    node.remove();
    node.parent = this;
    this.children.splice(cursor ? this.children.indexOf(cursor) : this.children.length, 0, node);
  }
  remove() {
    if (this.parent) this.parent.children.splice(this.parent.children.indexOf(this), 1);
    this.parent = null;
  }
  setAttribute(name, value) { this[name] = value; }
}

function fixture(fetcher) {
  const document = new EventTarget();
  document.createElement = () => new Element();
  document.createTextNode = (text) => new Element(text);
  global.document = document;
  const events = new EventTarget();
  global.addEventListener = events.addEventListener.bind(events);
  const selection = { isCollapsed: true, anchorNode: null, focusNode: null, containsNode: () => false };
  global.getSelection = () => selection;
  const controls = Object.fromEntries(['view', 'summary', 'source', 'level', 'text', 'limit',
    'follow', 'refresh', 'clear'].map((key) => [key, new Element()]));
  controls.source.value = 'both'; controls.level.value = 'all';
  controls.text.value = ''; controls.limit.value = '600';
  const errors = [];
  const controller = create({ ...controls, fetcher, notify: (...args) => errors.push(args), resolve: () => {} });
  return { ...controls, controller, errors, selection, events, document };
}
const row = (id) => ({ id, source: 'stdout', level: 'info', line: `Message ${id}`, truncated: false });
const response = (ids) => Response.json({
  rows: ids.map(row), missing_sources: [], has_more: false, scan_limited: false,
});
const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

test('log requests send all filters to the bounded backend and reuse unchanged DOM nodes', async () => {
  const requests = [];
  let ids = ['1', '2'];
  const f = fixture(async (url) => { requests.push(url); return response(ids); });
  f.source.value = 'stderr'; f.level.value = 'warn'; f.text.value = 'USB & MIDI'; f.limit.value = '15';
  await f.controller.load();
  const query = new URL(requests[0], 'http://audiox').searchParams;
  assert.equal(query.get('text'), 'USB & MIDI');
  assert.equal(query.get('level'), 'warn');
  assert.equal(query.get('source'), 'stderr');
  assert.equal(query.get('limit'), '15');
  assert.match(requests[0], /^\/api\/system\/logs\?/);
  const existing = f.view.children[1];
  ids = ['2', '3'];
  await f.controller.load();
  assert.equal(f.view.firstChild, existing);
  assert.deepEqual(f.view.children.map((node) => node.dataset.logId), ids);
});

test('selection and scrollback pause polling and preserve DOM, text and scroll until resumed', async () => {
  let requests = 0;
  let ids = ['1', '2'];
  const f = fixture(async () => { ++requests; return response(ids); });
  await f.controller.load();
  f.selection.isCollapsed = false;
  f.selection.anchorNode = f.view.firstChild;
  const selectedNode = f.view.firstChild;
  ids = ['2', '3'];
  await f.controller.load();
  assert.equal(requests, 1);
  assert.equal(f.view.firstChild, selectedNode);
  assert.match(f.summary.textContent, /paused while selecting/);
  f.selection.isCollapsed = true;
  f.view.scrollTop = 150;
  f.view.dispatchEvent(new Event('scroll'));
  await f.controller.load();
  assert.equal(requests, 1);
  assert.equal(f.view.scrollTop, 150);
  assert.equal(f.view.firstChild, selectedNode);
  f.follow.dispatchEvent(new Event('click'));
  await tick();
  assert.equal(requests, 2);
  assert.deepEqual(f.view.children.map((node) => node.dataset.logId), ids);
});

test('a response already in flight cannot disrupt an active selection or pointer interaction', async () => {
  let finish;
  const f = fixture(() => new Promise((resolve) => { finish = resolve; }));
  const load = f.controller.load();
  f.view.dispatchEvent(new Event('pointerdown'));
  finish(response(['1']));
  await load;
  assert.equal(f.view.children.length, 0);
  f.events.dispatchEvent(new Event('pointerup'));
  assert.equal(f.view.children.length, 1);
});

test('stale responses cannot overwrite newer filter requests', async () => {
  const pending = [];
  const f = fixture(() => new Promise((resolve) => pending.push(resolve)));
  const first = f.controller.load({ manual: true });
  f.text.value = 'new filter';
  const second = f.controller.load({ manual: true });
  pending[1](response(['new']));
  await second;
  pending[0](response(['old']));
  await first;
  assert.equal(f.view.firstChild.dataset.logId, 'new');
  assert.equal(f.errors.length, 0);
});

test('invalid input, old firmware, malformed data and read failures surface errors without full-file fallback', async () => {
  let requests = 0;
  const f = fixture(async () => { ++requests; return new Response('Not found', { status: 404 }); });
  f.limit.value = '0';
  await f.controller.load();
  assert.equal(requests, 0);
  assert.match(f.errors.pop()[0].message, /between 1 and 5000/);
  f.limit.value = '10';
  await f.controller.load();
  assert.equal(requests, 1);
  assert.match(f.errors.pop()[0].message, /Full log files will not be downloaded/);
  const malformed = fixture(async () => Response.json({ rows: [{}], missing_sources: [] }));
  await malformed.controller.load();
  assert.match(malformed.errors[0][0].message, /invalid log data/);
  const failed = fixture(async () => { throw new Error('Connection lost'); });
  await failed.controller.load();
  assert.equal(failed.errors[0][0].message, 'Connection lost');
});

test('health popups use backend decisions and deduplicate events, including page reloads', () => {
  const saved = new Map();
  global.sessionStorage = { getItem: (key) => saved.get(key), setItem: (key, value) => saved.set(key, value) };
  const notices = [];
  const notify = createHealthNotifier((...args) => notices.push(args));
  const info = (id, active = true) => ({ audio_health: { warning_id: id, warning_active: active } });
  notify(info('0')); notify(info('100', false)); notify(info('100', 'false'));
  notify(info('100')); notify(info('100'));
  createHealthNotifier((...args) => notices.push(args))(info('100'));
  assert.equal(notices.length, 1);
  assert.match(notices[0][0], /system may be overloaded/);
  notify(info('200'));
  assert.equal(notices.length, 2);
  assert.equal(notices[0][2], 'audio-xruns');
  delete global.sessionStorage;
});
