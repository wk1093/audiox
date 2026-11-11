const assert = require('node:assert/strict');
const { apiContext } = require('./support/api-context.js');
const { test } = require('node:test');
const vm = require('node:vm');

const source = require('./support/app-source.js').appSource();

function fixture({ pi5 = true, loaded = true } = {}) {
  const classes = new Set();
  const elements = new Map();
  const makeElement = (id) => {
    const element = {
      id, hidden: false, value: '', checked: false, disabled: false, textContent: '',
      attributes: {}, classList: {
        toggle(name, enabled) { enabled ? classes.add(name) : classes.delete(name); },
      },
      setAttribute(name, value) { this.attributes[name] = value; },
      append(...children) { this.children = children; },
      replaceChildren(...children) { this.children = children; },
      showModal() { this.open = true; },
      focus() { this.focused = true; },
    };
    elements.set(id, element);
    return element;
  };
  const svg = makeElement('pi5-fan-graph');
  svg.getScreenCTM = () => ({ inverse: () => ({}) });
  svg.createSVGPoint = () => ({
    matrixTransform: () => ({ x: pointer.x, y: pointer.y }),
  });
  const pointer = { x: 82, y: 38 };
  for (const id of [
    'btn-pi5-fan-save', 'pi5-fan-enabled', 'pi5-fan-hysteresis',
    'pi5-fan-hysteresis-value', 'pi5-fan-curve-summary', 'pi5-fan-status',
    'pi5-fan-warning', 'pi5-fan-dialog',
  ]) makeElement(id);
  const focusedPoint = makeElement('focused-point');
  const dirtyStates = [];
  const context = apiContext({
    document: {
      getElementById: (id) => elements.get(id),
      createElement: (name) => makeElement(name),
      querySelector: () => focusedPoint,
    },
    AudioxUI: { setUnsaved: (...args) => dirtyStates.push(args) },
    state: {
      pi5FanIsPi5: pi5,
      pi5FanSettingsLoaded: loaded,
      pi5FanSettingsLoading: false,
      pi5FanEditor: {
        enabled: true,
        temperatures: [50, 60, 68, 75],
        speeds: [75, 125, 175, 250],
        hysteresis: 5,
      },
    },
    pointer,
    classes,
    dirtyStates,
  });
  vm.runInContext(source.slice(source.indexOf('function validFanCurve('),
    source.indexOf('const pi5FanEnabled =')), context);
  return { context, elements, classes, dirtyStates, pointer };
}

test('fan-curve validation enforces ordered thresholds, speeds, and hysteresis gaps', () => {
  const { context } = fixture();
  const valid = { enabled: true, temperatures: [50, 60, 68, 75], speeds: [75, 125, 175, 250], hysteresis: 5 };
  assert.equal(context.validFanCurve(valid), true);
  assert.equal(context.validFanCurve({ ...valid, temperatures: [50, 55, 68, 75] }), false);
  assert.equal(context.validFanCurve({ ...valid, speeds: [75, 75, 175, 250] }), false);
  assert.equal(context.validFanCurve({ ...valid, hysteresis: 0 }), false);
});

test('fan graph renders accessible draggable points and keyboard edits preserve curve constraints', () => {
  const { context, elements, dirtyStates } = fixture();
  context.renderPi5FanCurve();
  const svg = elements.get('pi5-fan-graph');
  assert.match(svg.innerHTML, /role="slider"/);
  assert.match(svg.innerHTML, /data-step="3"/);
  assert.equal(elements.get('btn-pi5-fan-save').disabled, false);

  context.editPi5FanPoint(1, -100, -100, true);
  assert.equal(context.state.pi5FanEditor.temperatures[1], 56);
  assert.equal(context.state.pi5FanEditor.speeds[1], 76);
  assert.equal(elements.get('pi5-fan-graph').innerHTML.includes('56° · 76'), true);
  assert.equal(dirtyStates.at(-1)[0], 'pi5-fan');
  assert.equal(dirtyStates.at(-1)[1], true);
});

test('Pi 4 opens the interactive preview with a warning and cannot save fan settings', () => {
  const { context, elements, classes } = fixture({ pi5: false, loaded: false });
  context.openPi5FanDialog();
  assert.equal(elements.get('pi5-fan-dialog').open, true);
  assert.equal(elements.get('pi5-fan-warning').hidden, false);
  assert.match(elements.get('pi5-fan-status').textContent, /Preview mode/);
  assert.equal(elements.get('btn-pi5-fan-save').disabled, true);
  assert.equal(elements.get('pi5-fan-enabled').disabled, false);
  assert.equal(elements.get('pi5-fan-hysteresis').disabled, false);
  assert.match(elements.get('pi5-fan-graph').innerHTML, /tabindex="0" role="slider"/);
  assert.equal(classes.has('is-disabled'), false);

  const pointer = { pointerId: 1, clientX: 724, clientY: 38 };
  context.updatePi5FanPointFromPointer(pointer, 3);
  assert.equal(context.state.pi5FanEditor.temperatures[3], 74);
  assert.equal(context.state.pi5FanEditor.speeds[3], 255);
});
