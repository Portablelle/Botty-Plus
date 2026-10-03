import test from 'node:test';
import assert from 'node:assert/strict';
import { bindLaunchOptions, normalizeLaunchServices } from '../vps-site/src/launch-options.js';

function fixture(saved, unavailable = false) {
  const inputs = ['ftp', 'rtorrent', 'cheatrunner'].map(name => ({ name, addEventListener(_, handler) { this.change = handler; } }));
  const elements = { 'launch-services': { querySelector: selector => inputs.find(input => selector.includes('"' + input.name + '"')) },
    'launch-options-storage': {}, 'launch-options-summary': {}, 'launch-options': { open: true } };
  const browser = { get localStorage() {
    if (unavailable) throw Error('Storage blocked');
    return { getItem: () => saved, setItem: (_, value) => { saved = value; } };
  } };
  const control = bindLaunchOptions({ getElementById: id => elements[id] }, browser);
  return { inputs, elements, control, saved: () => saved };
}

test('preferences default on and only explicit false disables startup', () => {
  for (const value of [undefined, null, {}, [], 'bad', { ftp: 'false', rtorrent: 0 }])
    assert.deepEqual(normalizeLaunchServices(value), { ftp: true, rtorrent: true, cheatrunner: true });
});
test('saved choices restore, changes persist, launch locks a snapshot', () => {
  const f = fixture('{"ftp":false,"rtorrent":true,"cheatrunner":false}');
  assert.deepEqual(f.inputs.map(input => input.checked), [false, true, false]);
  f.inputs[1].checked = false; f.inputs[1].change();
  assert.deepEqual(JSON.parse(f.saved()), { ftp: false, rtorrent: false, cheatrunner: false });
  assert.equal(f.elements['launch-options-summary'].textContent, '0 of 3 services enabled');
  const selected = f.control.lock();
  assert.equal(f.elements['launch-services'].disabled, true);
  assert.equal(f.elements['launch-options'].open, false);
  f.inputs[0].checked = true;
  assert.equal(selected.ftp, false);
});
for (const unavailable of [false, true]) test('invalid or unavailable storage still allows selection: ' + unavailable, () => {
  const f = fixture('{broken', unavailable);
  assert.ok(f.inputs.every(input => input.checked));
  f.inputs[0].checked = false; f.inputs[0].change();
  assert.equal(f.control.lock().ftp, false);
  if (unavailable) assert.match(f.elements['launch-options-storage'].textContent, /this launch only/);
});
