/* Timer-driven hold tests with simulated ISR ticks and live POT input. */
const fs=require('fs'),vm=require('vm'),assert=require('assert/strict');
const main=fs.readFileSync(require('path').join(__dirname,'../main.c'),'utf8');
let code=main.slice(main.indexOf('unsigned char speed_hold_ticks'),main.indexOf('/* Column changes'))
 .replace('unsigned char speed_hold_ticks(unsigned int duration)','function speed_hold_ticks(duration)')
 .replace('void hold_step_with_pot(unsigned int duration)','function hold_step_with_pot(duration)')
 .replace(/unsigned (?:char|int) (\w+);/g,'let $1;')
 .replace(/\(unsigned long\)/g,'').replace(/\b(\d+)(?:UL|U)\b/g,'$1')
 .replace(/\(unsigned char\)\(speed_ticks-start\)/g,'((speed_ticks-start)&255)')
 .replace(/\(unsigned (?:char|int)\)\(/g,'Math.trunc(');
function measure(initial,pot,duration=220){
 let ticks=initial,reads=0;
 const env={speed_adc_read:()=>pot(reads)};
 Object.defineProperty(env,'speed_ticks',{get:()=>{if(reads++===0)return ticks; ticks=(ticks+1)&255;return ticks;}});
 const ctx=vm.createContext(env);vm.runInContext(code,ctx);ctx.hold_step_with_pot(duration);return reads-1;
}
for(const start of [0,200,250,255]) {
 assert.equal(measure(start,()=>0),10);
 assert.equal(measure(start,()=>1023),100);
 assert.equal(measure(start,()=>512),55);
}
assert.equal(measure(250,n=>n<20?1023:0),20,'POT changes shorten an active hold');
assert.equal(measure(250,n=>n<5?0:1023),100,'POT changes lengthen an active hold');
assert(main.includes('speed_ticks++;'));
assert(main.includes('volatile unsigned char speed_ticks'));
console.log('PASS: 10/55/100-tick holds, timer rollover, and live POT changes in both directions.');

assert.equal(measure(250,()=>0,80),4);
assert.equal(measure(250,()=>1023,80),36);
assert.equal(measure(250,()=>1023,400),182);
assert.equal(measure(250,()=>0,160),7);
assert.equal(measure(250,()=>1023,160),73);


const relay=main.slice(main.indexOf('void show_columns'),main.indexOf('void gpio_init'));
const render=main.slice(main.indexOf('void frame_show_duration'),main.indexOf('void frame_show(void)'));
const codeUnderTest=(relay+render).replace(/void (\w+)\((?:unsigned int )?(\w+)\)/g,(_,n,arg)=>'function '+n+'('+(arg==='void'?'':arg)+')')
 .replace(/unsigned (?:char|int) (\w+);/g,'let $1;').replace(/\b(\d+)U\b/g,'$1')
 .replace(/\(unsigned char\)\(speed_ticks - start\)/g,'((speed_ticks-start)&255)');
const masks=Array(10).fill(0),rows=Array(10).fill(0),columns=Array(10).fill(0),events=[];
let ticks=253;
const colMask=()=>columns.reduce((bits,on,c)=>bits|(on<<c),0);
const rowMask=()=>rows.reduce((bits,on,r)=>bits|(on<<r),0);
const screen={ROWS:10,COLS:10,RELAY_SETTLE_TICKS:5,selected_rows:0,circle_frame:masks,
 row_set:(r,on)=>{assert.equal(colMask(),0);rows[r]=on;events.push(['row',r,on,ticks]);},
 column_set:(c,on)=>{columns[c]=Number(on);},
 hold_step_with_pot:d=>events.push(['hold',d,rowMask(),colMask(),ticks])
};
Object.defineProperty(screen,'speed_ticks',{get:()=>{ticks=(ticks+1)&255;return ticks;}});
const ctx=vm.createContext(screen);vm.runInContext(codeUnderTest,ctx);
masks.fill(3);
ctx.frame_show_duration(160);
assert.deepEqual(events.at(-1),['hold',160,1023,3,3]);
events.length=0;masks.fill(7);ctx.frame_show_duration(160);
assert.deepEqual(events,[['hold',160,1023,7,3]],'column animation must not switch or wait on relays');
events.length=0;masks.fill(0);ctx.frame_show_duration(160);
assert.deepEqual(events,[['hold',160,1023,0,3]],'dark phase keeps the rows selected');
events.length=0;masks[0]=1;masks[1]=2;masks[2]=1;
ctx.frame_show_duration(160);
assert.deepEqual(events.filter(e=>e[0]==='hold').map(e=>e.slice(1,4)),[[160,5,1],[160,2,2]]);
assert(!events.some(e=>e[0]==='row'&&(e[1]===0||e[1]===2)&&e[2]===1),'keep selected matching relays on');
const release=events.find(e=>e[0]==='row'&&e[1]===0&&e[2]===0);
const pickup=events.find(e=>e[0]==='row'&&e[1]===1&&e[2]===1);
assert((pickup[3]-release[3]+256)%256>=5);
assert((events.at(-1)[4]-pickup[3]+256)%256>=5);
console.log('PASS: steady relays during column-only changes and blank frames, exact relay groups, break-before-make delays, and tick rollover.');
