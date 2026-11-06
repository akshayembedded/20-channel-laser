/* Generate the browser's column previews from the active firmware playlist. */
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const file = path.join(root, 'laser_animation_studio (2).html');
const firmware = fs.readFileSync(path.join(root, 'main.c'), 'utf8');
const mainStart = firmware.lastIndexOf('void main(void)');
const playlist = [...firmware.slice(mainStart).matchAll(/(?:for \(repeat = 0; repeat < (\d+); repeat\+\+\)\s+)?\b(mode_\w+)\(\);/g)];
assert.equal(playlist.length, 35);
assert(playlist.every(([, count]) => !count || count === "1"));
const body = firmware.slice(firmware.indexOf('/* Forward Fill / Forward Empty. */', firmware.indexOf('/* Port of the 10_Channel_EEPROM playlist.')), mainStart);
const executable = body.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, '')
  .replace(/void (mode_\w+)\((?:void)?\)/g, 'function $1()')
  .replace(/\b(?:unsigned int|int) ([^;]+);/g, 'let $1;')
  .replace(/\b(\d+)U\b/g, '$1')
  .replace(/\((?:unsigned int|unsigned char|int)\)/g, '');
const mirror = firmware.match(/mirror_columns\[COLS\] = \{([^}]+)\}/)[1].split(',').map(Number);
assert.deepEqual(mirror.slice(0,5).map(i => [i+1, mirror[9-i]+1]), [[1,10],[2,9],[3,8],[4,7],[5,6]]);
let mask = 0;
let frames;
function bit(column) {
  assert(Number.isInteger(column) && column >= 0 && column < 10, 'Invalid column: ' + column);
  return 1 << column;
}
const context = vm.createContext({
  COLS: 10,
  mirror_column: column => { bit(column); return mirror[column]; },
  column_sequence_delay: () => 160,
  sequence_columns_on: () => { mask = 1023; },
  sequence_columns_off: () => { mask = 0; },
  sequence_column_on: column => { mask |= bit(column); },
  sequence_column_off: column => { mask &= ~bit(column); },
  sequence_column_bit: column => column < 0 || column >= 10 ? 0 : bit(column),
  show_columns: value => { mask = value & 1023; },
  chase_delay: ms => { frames.push([mask, ms]); }
});
vm.runInContext(executable, context, {timeout: 1000});
const effects = playlist.map(([, count, fn]) => {
  const repeats = Number(count || 1);
  frames = [];
  vm.runInContext('for (let i=0;i<'+repeats+';i++) '+fn+'();', context, {timeout: 1000});
  assert(frames.length && frames.length <= 500, fn);
  const title = fn.slice(5).replaceAll('_', ' ').replace(/^./, c => c.toUpperCase());
  return [title, repeats, frames];
});
const old = fs.readFileSync(file, 'utf8');
const start = '// BEGIN GENERATED COLUMN EFFECTS';
const end = '// END GENERATED COLUMN EFFECTS';
const generated = start+'\nconst columnEffects = [\n'+effects.map(effect => '  '+JSON.stringify(effect)).join(',\n')+'\n];\n'+end;
assert(old.includes(start) && old.includes(end));
const updated = old.slice(0, old.indexOf(start)) + generated + old.slice(old.indexOf(end)+end.length);
if (process.argv.includes('--check')) assert.equal(updated, old, 'Regenerate HTML with node tools/export_column_previews.js');
else fs.writeFileSync(file, updated);
console.log('Verified '+effects.length+' effects and '+effects.reduce((n,e)=>n+e[2].length,0)+' timed column steps.');
