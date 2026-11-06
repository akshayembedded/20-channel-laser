/* Compare the actual named firmware functions with the HTML frame generators. */
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..');
const html=fs.readFileSync(path.join(root,'laser_animation_studio (2).html'),'utf8');
const firmware=fs.readFileSync(path.join(root,'main.c'),'utf8');
// Validate the archived matrix implementation separately from the active port.
const main=firmware.split('/* Port of the 10_Channel_EEPROM playlist.')[0].replace(/^\/\/ /gm,'');
const ref=vm.createContext({});
vm.runInContext(html.slice(html.indexOf('const patterns=[',html.indexOf('id="archived-matrix-presets"')),html.indexOf('</script>',html.indexOf('id="archived-matrix-presets"')))+
 '\nglobalThis.presets=patterns.map(([name,make])=>({name,frames:make()}));',ref,{timeout:1000});
const presets=JSON.parse(JSON.stringify(ref.presets));
const names=presets.map(p=>p.name.toLowerCase().replace(/[^a-z0-9]+/g,'_'));
assert.equal(names.length,18);
assert.deepEqual(require('./pattern_names.json'),names);
assert.match(main,/#define COLUMN_ON_LEVEL 1\b/);
assert.match(main,/#define ROW_ON_LEVEL\s+0\b/);
const extra=['chase_rows_10_to_1_columns_10_to_1','chase_circle_pairs_ends_to_center_positions_1_to_10',
 'chase_full_circle_end_to_end','chase_full_circle_end_to_end_reverse'];
const playlist=main.slice(main.lastIndexOf('    while (1)'));
assert.deepEqual([...playlist.matchAll(/^\s+(\w+)\(\);/gm)].map(m=>m[1]),[...names,...extra]);

function body(name) {
 const match=main.match(new RegExp('^(?:void|unsigned int) '+name+'\\([^)]*\\)\\s*\\{[\\s\\S]*?^\\}', 'm'));
 assert(match,name);
 return match[0];
}
const functions=[...names,...extra,'studio_bit','hold_circle_with_pot'];
const js=functions.map(body).join('\n')
 .replace(/\/\*[\s\S]*?\*\//g,'')
 .replace(/void (\w+)\(void\)/g,'function $1()')
 .replace('unsigned int studio_bit(int position)','function studio_bit(position)')
 .replace(/\b(?:unsigned (?:int|char)|int) (\w+);/g,'let $1;')
 .replace(/\((?:unsigned (?:int|char)|int)\)/g,'')
 .replace(/\b(\d+)U\b/g,'$1');
const circle_frame=Array(10).fill(0),rows=Array(10).fill(0),columns=Array(10).fill(0);
const rendered=[],rowEvents=[];
const ctx=vm.createContext({ROWS:10,COLS:10,circle_frame,
 frame_clear:()=>circle_frame.fill(0),
 frame_add:(r,c)=>{circle_frame[r]|=1<<c;},
 frame_show:()=>rendered.push({ms:160,masks:[...circle_frame]}),
 frame_show_duration:ms=>rendered.push({ms,masks:[...circle_frame]}),
 select_rows:mask=>{rows.forEach((on,r)=>{rows[r]=(mask>>r)&1;});rowEvents.push({phase:rendered.length});},
 show_columns:mask=>{columns.forEach((on,c)=>{columns[c]=(mask>>c)&1;});},
 ALL_ROWS_MASK:1023,
 hold_step_with_pot:ms=>{
  const mask=columns.reduce((value,on,c)=>value|(on<<c),0);
  rendered.push({ms,masks:rows.map(on=>on?mask:0)});
 }
});
vm.runInContext(js,ctx,{timeout:1000});
for(const [i,preset] of presets.entries()){
 rendered.length=0;
 rowEvents.length=0;
 ctx[names[i]]();
 assert.deepEqual(rendered,preset.frames.map(({ms,masks})=>({ms,masks})),preset.name);
 if(names[i]==='strobing') assert(!rowEvents.some(e=>e.phase===1),'Rows must stay enabled during the dark phase');
 assert(columns.every(v=>v===0),preset.name+' leaves direct-drive columns off');
}
for(const [name,expected] of [
 ['chase_full_circle_end_to_end',[0,1,2,3,4,5,6,7,8,9,8,7,6,5,4,3,2,1,0]],
 ['chase_full_circle_end_to_end_reverse',[9,8,7,6,5,4,3,2,1,0]]
]){
 rendered.length=0;
 ctx[name]();
 assert.deepEqual(rendered,expected.map(r=>({ms:220,masks:Array.from({length:10},(_,i)=>i===r?1023:0)})));
}
console.log('PASS: 18 HTML presets match firmware masks and timing weights, 22 playlist entries, strobing holds rows on, and both full-circle chases match.');
