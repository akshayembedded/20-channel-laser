/* Host simulation of the actual boot selection and EEPROM routines. */
const fs=require('fs'),path=require('path'),vm=require('vm'),assert=require('assert/strict');
const firmware=fs.readFileSync(path.join(__dirname,'../main.c'),'utf8');
// Validate the archived matrix implementation separately from the active port.
const main=firmware.split('/* Port of the 10_Channel_EEPROM playlist.')[0].replace(/^\/\/ /gm,'');
let code=main.slice(main.indexOf('unsigned char eeprom_save_next_boot'),main.indexOf('/* One full circle at a time:'))
 .replace(/unsigned char (\w+)\(unsigned char next_mode\)/,'function $1(next_mode)')
 .replace(/unsigned char (\w+)\(void\)/,'function $1()')
 .replace(/(?:unsigned int|unsigned char) (\w+);/g,'let $1;')
 .replace(/\(unsigned char\)/g,'').replace(/\b(\d+)U\b/g,'$1');
function device(initial,fault='') {
 let data=initial,status=0,key=0,writes=0;
 const env={BOOT_ALL_ON:0xA5,BOOT_PATTERNS:0x5A,BOOT_CHASE_ONLY:0x3C,EEPROM_WAIT_LIMIT:20};
 Object.defineProperties(env,{
  FLASH_DUKR:{set:v=>{if(key===0xAE&&v===0x56&&fault!=='unlock') status|=8;key=v;}},
  FLASH_IAPSR:{get:()=>{const s=status;status&=~5;return s;},set:v=>{status=v;}},
  EEPROM_NEXT_BOOT:{get:()=>data,set:v=>{
   if(!(status&8)||fault==='write')status|=1;
   else if(fault!=='timeout'){data=v;status|=4;writes++;}
  }}
 });
 const ctx=vm.createContext(env);vm.runInContext(code,ctx);
 return {ctx,get data(){return data;},get status(){return status;},get writes(){return writes;}};
}
for(let initial=0;initial<256;initial++) {
 const d=device(initial);
 for(let boot=0;boot<12;boot++) {
  const before=d.data;
  const current=d.ctx.select_boot_mode();
  assert.equal(current,before===0xA5?0xA5:before===0x3C?0x3C:0x5A);
  assert.equal(d.data,current===0xA5?0x3C:current===0x3C?0x5A:0xA5);
  assert.equal(d.status&8,0);
 }
 assert.equal(d.writes,12);
}
for(const fault of ['unlock','write','timeout']) {
 const d=device(0xFF,fault);
 assert.equal(Number(d.ctx.eeprom_save_next_boot(0xA5)),0);
 assert.equal(d.data,0xFF);assert.equal(d.status&8,0);
}
const startup=main.slice(main.indexOf('void main(void)'));
assert(startup.indexOf('select_boot_mode()')<startup.indexOf('speed_adc_init()'));
assert(startup.indexOf('all_on()')<startup.indexOf('speed_adc_init()'));
console.log('PASS: 3072 simulated boots, erased/invalid states, EEPROM relocking, unlock/write/timeout faults, all-on before timer initialization.');



const third=startup.slice(startup.indexOf('if (boot_mode == BOOT_CHASE_ONLY)'),startup.lastIndexOf('    while (1)'));
assert.deepEqual([...third.matchAll(/^\s+([a-z][a-z0-9_]*)\(\);/gm)].map(m=>m[1]),['orbit_relay','chase_rows_10_to_1_columns_10_to_1','chase_full_circle_end_to_end','chase_full_circle_end_to_end_reverse']);
assert(startup.indexOf('speed_sample_timer_init()')<startup.indexOf('if (boot_mode == BOOT_CHASE_ONLY)'));
console.log('PASS: three-boot cycle and exactly four named chases on the third boot, with ADC/timer initialized.');
