const assert = require('node:assert/strict');
const { test } = require('node:test');
const { highlight, attach, setValue } = require('../../web/embedded/lua-editor.js');

const plain = (html) => html.replace(/<\/?span[^>]*>/g, '')
  .replace(/&gt;/g, '>').replace(/&lt;/g, '<').replace(/&amp;/g, '&');

test('Lua keywords, API names, strings, numbers and comments are highlighted without changing source', () => {
  const source = 'local n = 0xff + 1.5e-2\nhook("run", function(event)\n  -- note\n  log(event.sound)\nend)\n';
  const html = highlight(source);
  for (const kind of ['keyword', 'builtin', 'string', 'number', 'comment', 'operator']) {
    assert.match(html, new RegExp(`class="lua-${kind}"`));
  }
  assert.equal(plain(html), source);
  assert.match(highlight('1..2'), /lua-number">1<\/span><span class="lua-operator">\.\.<\/span><span class="lua-number">2/);
});

test('long strings/comments, escaped quotes and incomplete input retain their lexical boundaries', () => {
  for (const source of [
    '[==[local "-- not a comment"\n]==]',
    '--[=[ hook("run")\nlocal x = 1 ]=]',
    '"escaped \\" quote"',
    "'escaped \\' quote'",
    '-- unfinished comment',
    '[=[unfinished\nlocal',
    '"unfinished',
  ]) {
    const html = highlight(source);
    assert.equal(plain(html), source);
    assert.equal((html.match(/<span /g) || []).length, 1, source);
  }
});

test('script markup is escaped and maximum-sized scripts retain all text', () => {
  const source = '-- </span><img src=x onerror=alert(1)> &\n' + 'x'.repeat(17408);
  const html = highlight(source);
  assert.doesNotMatch(html, /<img|onerror[^<]*>/);
  assert.match(html, /&lt;img/);
  assert.equal(plain(html), source);
});

test('input, scroll and programmatic load/reset refresh the overlay without input events or selection changes', () => {
  const handlers = {};
  const classes = new Set();
  const textarea = {
    value: 'local x = 1\n', scrollTop: 16, scrollLeft: 24,
    selectionStart: 2, selectionEnd: 5,
    parentElement: { classList: { add: (name) => classes.add(name) } },
    addEventListener: (name, handler) => { handlers[name] = handler; },
    dispatchEvent: (event) => { handlers[event.type]?.(event); },
  };
  const preview = {};
  attach(textarea, preview);
  assert(classes.has('lua-highlighted'));
  assert.equal(plain(preview.innerHTML), textarea.value + '\n');
  textarea.value = '-- editing';
  handlers.input();
  assert.match(preview.innerHTML, /lua-comment/);
  assert.equal(textarea.selectionStart, 2);
  assert.equal(textarea.selectionEnd, 5);
  textarea.scrollTop = 90;
  textarea.scrollLeft = 120;
  handlers.scroll();
  assert.equal(preview.scrollTop, 90);
  assert.equal(preview.scrollLeft, 120);
  let inputEvents = 0;
  handlers.input = () => { inputEvents++; };
  setValue(textarea, 'return "loaded"');
  assert.equal(plain(preview.innerHTML), 'return "loaded"\n');
  assert.equal(inputEvents, 0);
});
