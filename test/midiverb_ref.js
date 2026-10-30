// Reference vectors for dsp/midiverb.cpp from the original JavaScript of
// http://ibawizard.net/midiverb/ . The page's inline script (up to its UI code) is evaluated in a
// Node vm context, its Machine and Filterx2 are driven exactly as the page's onaudioprocess
// callback drives them, and the outputs are hashed bit for bit. test/midiverb_test.cpp runs the
// same cases through the C++ port and compares.
// usage: node test/midiverb_ref.js <downloaded index.html> <dir with the .rom files> > test/expected/midiverb.txt
'use strict';
const fs = require('fs');
const vm = require('vm');
const path = require('path');

const html = fs.readFileSync(process.argv[2], 'utf8');
const romDir = process.argv[3];
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]);
const main = scripts.find(s => s.includes('class Machine'));
const core = main.slice(0, main.indexOf('// Configuration - default device'));
const ctx = vm.createContext({ console, Math, Uint8Array, Int16Array });
vm.runInContext(core + '\n;this.__x = { Machine, Filterx2, ROM_TYPES, LFO_PATCHES };', ctx);
const { Machine, Filterx2, ROM_TYPES } = ctx.__x;
const DEVICES = ['midiverb', 'midifex', 'midiverb2'];  // ROM_TYPES order, the C++ rom() index

// The cases and the input signal: keep in step with test/midiverb_test.cpp.
const N = 24000;
function input(i, state) {
  // impulse, noise burst, square burst, silence; exact in double on both sides
  let l = 0, r = 0;
  if (i === 0) { l = 0.5; r = 0.25; }
  else if (i >= 1000 && i < 3000) {
    state.s = (Math.imul(state.s, 1664525) + 1013904223) >>> 0;
    l = ((state.s >>> 8) / 16777216 * 2 - 1) * 0.5;
    state.s = (Math.imul(state.s, 1664525) + 1013904223) >>> 0;
    r = ((state.s >>> 8) / 16777216 * 2 - 1) * 0.5;
  } else if (i >= 6000 && i < 8000) {
    l = (Math.floor(i / 50) % 2) ? 0.3 : -0.3;
    r = -l;
  }
  return [l, r];
}

const CASES = [
  { name: 'mv2_p1', rom: 2, program: 1, mix: 0.5, fb: 0 },
  { name: 'mv2_p29_fb', rom: 2, program: 29, mix: 1, fb: 0.3 },
  { name: 'mv2_p51_lfo_tri', rom: 2, program: 51, mix: 0.8, fb: 0 },
  { name: 'mv2_p57_lfo_tri', rom: 2, program: 57, mix: 1, fb: 0.2 },
  { name: 'mv2_p60_lfo_sine', rom: 2, program: 60, mix: 1, fb: 0 },
  { name: 'mv2_p68_lfo_sine', rom: 2, program: 68, mix: 0.7, fb: 0.1 },
  { name: 'mv2_p97_regen', rom: 2, program: 97, mix: 1, fb: 0.9 },
  { name: 'mv1_p1', rom: 0, program: 1, mix: 0.5, fb: 0 },
  { name: 'mv1_p50_fb', rom: 0, program: 50, mix: 1, fb: 1 },
  { name: 'mv1_p61_reverse', rom: 0, program: 61, mix: 1, fb: 0 },
  { name: 'mfx_p37_regen', rom: 1, program: 37, mix: 1, fb: 0.5 },
  { name: 'mfx_p58_thick', rom: 1, program: 58, mix: 0.6, fb: 0 },
  { name: 'changes', rom: 2, program: 5, mix: 1, fb: 0.25,
    changes: [{ at: 4096, program: 53 }, { at: 12288, rom: 0 }, { at: 16384, program: 20 }] },
];

const roms = {};
for (const d of DEVICES) roms[d] = new Uint8Array(fs.readFileSync(path.join(romDir, ROM_TYPES[d].romFile)));

function fnv(h, f64) {
  const b = new Uint8Array(new Float64Array([f64]).buffer);
  for (const x of b) { h ^= BigInt(x); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; }
  return h;
}

console.log('# reference vectors from the JavaScript of http://ibawizard.net/midiverb/ (test/midiverb_ref.js); do not edit');
for (const c of CASES) {
  const machine = new Machine();
  let device = DEVICES[c.rom];
  const select = (dev, prog) => {
    // the page: loadRom (here: the ROM bytes directly), loadProgram, resetMachine
    machine.romData = roms[dev];
    machine.currentRomType = ROM_TYPES[dev];
    machine.memoryShift = ROM_TYPES[dev].memoryShift;
    machine.loadProgram(ROM_TYPES[dev], prog);
    machine.resetMachine();
  };
  select(device, c.program);
  const ds0 = new Filterx2(), ds1 = new Filterx2(), us0 = new Filterx2(), us1 = new Filterx2();
  let lastL = 0, lastR = 0, lfoTickCounter = 0;
  const state = { s: 12345 };
  let h = 0xcbf29ce484222325n;
  const first = [];
  for (let i = 0; i < N; i++) {
    for (const ch of c.changes || []) {
      if (ch.at !== i) continue;
      if (ch.rom !== undefined) { device = DEVICES[ch.rom]; select(device, ROM_TYPES[device].firstProgram); }
      else select(device, ch.program);
    }
    const [left, right] = input(i, state);
    const left_ds = ds0.run(left), right_ds = ds1.run(right);
    if (i % 2 == 0) {
      const [lo, ro] = machine.runEffect((left_ds * 32767.0) | 0, (right_ds * 32767.0) | 0, c.fb);
      lastL = lo / 32767.0;
      lastR = ro / 32767.0;
      if (machine.hasActiveLfo && lfoTickCounter % 8 === 0) {
        machine.patchMachine(machine.lfo1.update(), machine.lfo2.update());
      }
      lfoTickCounter++;
    }
    const ol = (1 - c.mix) * left + c.mix * us0.run(lastL);
    const or = (1 - c.mix) * right + c.mix * us1.run(lastR);
    h = fnv(fnv(h, ol), or);
    if (i < 32 || (i >= 2000 && i < 2016)) first.push(`${i} ${ol} ${or}`);
  }
  console.log(`case ${c.name}`);
  for (const l of first) console.log(l);
  console.log(`hash ${h.toString(16).padStart(16, '0')}`);
}
