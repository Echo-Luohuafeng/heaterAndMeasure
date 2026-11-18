// Run the actual embedded script against a small DOM/HTTP harness. This checks
// interaction logic; it does not replace a real browser or hardware timing test.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../components/device_web/web/index.html'), 'utf8');
const source = html.split('<script>')[1].split('</script>')[0];
const elements = new Map(), listeners = {}, timers = [], requests = [], sockets = [], blobs = [];
const TOKEN = '0123456789abcdef0123456789abcdef';
const FRAME = [1, 7, 20000, [5, 0, 0, 0], [100, null, 120, 130], 13, 2, 0, 0, 2048];
const status = {boot_id:'test', version:'3.0.0-usb-ncm', verified:true, maintenance:false,running:false,recording:false,
 network:{ip:'192.168.7.1',transport:'usb_ncm',connected:true}, store:{psram:true,oldest:1,next:2,capacity:65536,overwritten:0},
 ota:{active:false,error:0},free_internal_bytes:100000};
class Element {
 constructor(){this.value='';this.children=[];this.clientWidth=900;this.files=[];this.textContent='';this.checked=false;}
 set id(value){this._id=value;elements.set(value,this);} get id(){return this._id;}
 append(child){this.children.push(child);} click(){if(this.onclick)this.onclick();}
 getContext(){return new Proxy({}, {get:()=>()=>{}});}
}
const document = {hidden:false,getElementById(id){if(!elements.has(id)){const el=new Element();el.id=id;}return elements.get(id);},
 createElement:()=>new Element(),addEventListener(name,fn){listeners[name]=fn;}};
const context = vm.createContext({document,window:{devicePixelRatio:1,addEventListener(name,fn){listeners[name]=fn;}},
 location:{protocol:'http:',host:'192.168.7.1'},sessionStorage:{getItem:()=>'',setItem(){}},
 URL:{createObjectURL(blob){blobs.push(blob);return 'blob:test';},revokeObjectURL(){}},Blob,URLSearchParams,
 confirm:()=>true,setTimeout:()=>1,clearTimeout(){},setInterval(fn){timers.push(fn);},
 WebSocket:class {constructor(url){this.url=url;sockets.push(this);}close(){}},
 async fetch(url,options){requests.push({url,...options});assert.equal(options.headers.Authorization,'Bearer '+TOKEN);
  if(options.body?.startsWith?.('START')){status.running=true;status.recording=true;status.maintenance=false;}
  if(options.body==='STOP'){status.running=false;status.recording=false;}
  const body=options.body?.startsWith?.('START')?{ack:'START',run_id:1,next:1}:url.startsWith('/api/status')?status:url.startsWith('/api/data')?
   {schema:1,boot_id:'test',next:2,lost:0,more:false,frames:[FRAME]}:{ack:'OK'};
  return {ok:true,json:async()=>structuredClone(body)};
 }});
vm.runInContext(source,context,{filename:'embedded-dashboard.js'});
const element=id=>document.getElementById(id);
async function run(){
 element('token').value=TOKEN;await element('connect').onclick();assert.equal(sockets.length,1);
 sockets[0].onmessage({data:JSON.stringify({schema:1,boot_id:'test',next:2,lost:0,frames:[FRAME]})});
 assert.match(element('r0').textContent,/100\.0000/);assert.equal(element('r1').textContent,'无效');
 element('duty0').value='5.5';await element('set').onclick();assert.match(element('message').textContent,/整数/);
 assert(!requests.some(r=>r.body?.startsWith?.('SET')));
 const pingCount=()=>requests.filter(r=>r.body==='PING').length;
 element('hold').checked=true;await element('hold').onchange();assert.equal(pingCount(),0);
 element('duty0').value='5';await element('set').onclick();assert.equal(requests.at(-1).body,'SET 5 0 0 0');
 await timers[0]();assert.equal(pingCount(),0);assert.equal(status.running,false);
 element('hold').checked=false;await element('start').onclick();await new Promise(setImmediate);
 assert(requests.some(r=>r.body==='START 5 0 0 0'));assert.equal(status.running,true);
 await timers[0]();assert.equal(pingCount(),0);
 element('hold').checked=true;await element('hold').onchange();assert.equal(pingCount(),1);
 await timers[0]();assert.equal(pingCount(),2);
 document.hidden=true;await timers[0]();assert.equal(pingCount(),3);document.hidden=false;
 element('hold').checked=false;await element('hold').onchange();await timers[0]();assert.equal(pingCount(),3);
 await element('csv').onclick();assert.equal(blobs.length,1);const csv=await blobs[0].text();
 assert.match(csv,/test,1,7,20000,5,0,0,0,100,,120,130,13,2,0,0,2048/);
 await element('stop').onclick();assert.equal(requests.at(-1).body,'STOP');
 element('hold').checked=true;await element('hold').onchange();await timers[0]();assert.equal(pingCount(),3);
 status.running=true;status.recording=true;await timers[0]();assert.equal(pingCount(),4);
 await element('stop').onclick();
 status.store.next=3000;await element('connect').onclick();
 assert.match(sockets.at(-1).url,/from=2950/); // new viewers start at live data
 await new Promise(setImmediate);
 status.control_ready=false;status.control_init_error=263;status.maintenance=true;
 await vm.runInContext('poll()',context);
 assert.equal(element('set').disabled,true);assert.equal(element('start').disabled,true);
 assert(!html.includes('id="resume"'));
 assert(!html.includes('/api/wifi'));
 assert.match(element('device').textContent,/USB 已连接/);
 assert(!element('device').textContent.includes('RSSI'));
 status.network.connected=false;await vm.runInContext('poll()',context);
 assert.match(element('device').textContent,/USB 已断开/);
 assert.match(element('diagnostics').textContent,/采样初始化失败/);
 await element('reboot').onclick();assert.equal(requests.at(-1).url,'/api/reboot');
 status.max_duty=2;status.control_ready=true;status.maintenance=false;await vm.runInContext('poll()',context);
 assert.equal(element('duty0').max,2);
 element('duty0').value='3';const beforeLimit=requests.length;await element('set').onclick();
 assert.equal(requests.length,beforeLimit);assert.match(element('message').textContent,/0–2/);
 element('duty0').value='2';await element('set').onclick();assert.equal(requests.at(-1).body,'SET 2 0 0 0');
 console.log('Dashboard explicit START, staged SET, opt-in heartbeat, hidden-tab heartbeat, STOP and CSV tests passed');
}
run().catch(error=>{console.error(error);process.exitCode=1;});
