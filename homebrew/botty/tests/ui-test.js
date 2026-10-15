'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
class Element {
 constructor(tag){this.tag=tag;this.children=[];this.textContent='';}
 appendChild(child){this.children.push(child);return child;}
 replaceChildren(...children){this.children=children;}
 focus(){}
 setAttribute(){}
 addEventListener(type,callback){this[type]=callback;}
 set id(value){this._id=value;elements.set(value,this);}
 get id(){return this._id;}
 get options(){return this.children;}
 set value(value){this._value=value;}
 get value(){return this._value??(this.tag==='select'?this.children.find(child=>!child.disabled)?.value:'');}
}
const elements=new Map();
const intervals=[];
const context=vm.createContext({
 document:{getElementById(id){if(!elements.has(id))elements.set(id,new Element('section'));return elements.get(id);},createElement:tag=>new Element(tag),querySelectorAll:()=>[]},
 window:{addEventListener(){}},performance:{now:()=>1000},fetch:()=>new Promise(()=>{}),setInterval(callback,delay){intervals.push({callback,delay});},requestAnimationFrame(){}
});
vm.runInContext(fs.readFileSync(require('node:path').join(__dirname,'../ui/app.js'),'utf8'),context);
const processingIntervals=intervals.filter(interval=>interval.delay===1000);
assert.equal(processingIntervals.length,1);
const processingInterval=processingIntervals[0];
assert.equal(processingInterval.callback,vm.runInContext('refreshProcessing',context));
async function render(task){
 context.fixture=task;
 vm.runInContext("token='fixture';api=async()=>({tasks:[fixture]})",context);
 await processingInterval.callback();
 return elements.get('processing').children[0].children.filter(child=>child.tag==='p').map(child=>child.textContent);
}
(async()=>{
 const task={name:'Library move',phase:'Moving game',unit:'bytes',bytes:100,total:1000,rate:0,eta:120};
 assert((await render(task)).includes('0 B/s · ETA ~2 min'));
 assert(!(await render({...task,eta:-1})).some(text=>text.includes('ETA')));
 assert((await render({...task,rate:1024,eta:-1})).includes('1.0 KiB/s · ETA calculating…'));
 assert((await render({...task,eta:0})).includes('0 B/s · ETA < 1 min'));
 assert(!(await render({...task,unit:'items'})).some(text=>text.includes('ETA')));
 vm.runInContext("tab='jobs';render();",context);
 assert(elements.get('processing').hidden); // Re-filter cached tasks immediately, without a poll.
 vm.runInContext("tab='all';render();",context);
 assert(!elements.get('processing').hidden);
 vm.runInContext("state={jobs:[{id:'job',hash:'aaa',torrentId:1,status:'moved'}],torrents:[{id:1,hashString:'aaa',leftUntilDone:0}]};processingTasks=[{id:'job',status:'running'}];tab='complete';",context);
 assert.equal(vm.runInContext('activityRows().length',context),0); // Live processing still owns the source row.
 vm.runInContext('processingUnavailable=true;',context);
 assert.equal(vm.runInContext('activityRows().length',context),1); // State polling restores controls when processing polling fails.
 vm.runInContext('processingUnavailable=false;',context);
 vm.runInContext("processingTasks[0].status='completed';",context);
 assert.equal(vm.runInContext('activityRows().length',context),1); // Completed processing restores actionable game history.
 assert.equal(vm.runInContext('activityRows()[0].torrent',context),false);
 vm.runInContext("state.jobs[0].status='extracting';processingTasks[0].status='running';tab='all';renderProcessing();",context);
 assert(elements.get('job-job-cancel')); // Replacement cards retain cancellation controls.
 vm.runInContext("state.jobs[0].status='moved';state.compression={jobId:'job',verifyRequested:true,status:'verifying',busy:true};renderProcessing();",context);
 assert(elements.get('job-job-skip-verify'));
 vm.runInContext("torrentCard({id:77,name:'Server package',remote:true,status:4,leftUntilDone:1,files:[{name:'game.rar'}],remotePreparation:{status:'failed',packages:['game.pkg']}});",context);
 assert(elements.get('torrent-77-prepare'));
 assert(!elements.get('torrent-77-extract-0'));
 vm.runInContext("torrentCard({id:78,name:'Remote RAR',remote:true,status:6,leftUntilDone:0,files:[{name:'game.rar'}],remotePreparation:{}});",context);
 assert(elements.get('torrent-78-extract-0'));
 assert(!elements.get('torrent-78-extract-0').disabled);
 vm.runInContext("jobCard({id:'retained',status:'ready',adoptedImport:true,content:{kind:'folder'}});",context);
 assert(elements.get('job-retained-move'));
 assert(!elements.get('job-retained-delete')&&!elements.get('job-retained-dismiss'));
 for(const status of ['restored','deleted']){
  context.terminalStatus=status;
  assert.equal(vm.runInContext('activityGroup({status:terminalStatus})',context),'complete');
  assert.equal(vm.runInContext('replacesJob({status:terminalStatus})',context),false);
 }
 vm.runInContext("tab='all';processingTasks=[];",context);
 vm.runInContext("state={storage:[{id:'internal',label:'PS5',available:true,freeBytes:1e9}],remoteDownloads:{enabled:true}};chooseStorage('internal',true,choice=>{selectedChoice=choice;});",context);
 assert.equal(elements.get('engine-choice').options.length,2);
 elements.get('engine-choice').value='qbittorrent';vm.runInContext('modalAction()',context);
 assert.equal(context.selectedChoice.engine,'qbittorrent');
 vm.runInContext("chooseStorage('internal',true,choice=>{selectedChoice=choice;});modalAction();",context);
 assert.equal(context.selectedChoice.engine,'rtorrent');
 vm.runInContext("chooseStorage('internal',true,choice=>{selectedChoice=choice;},false,false);modalAction();",context);
 assert(!('engine' in context.selectedChoice));
 vm.runInContext("state.remoteDownloads.enabled=false;chooseStorage('internal',true,()=>{});",context);
 assert(elements.get('engine-choice').options.find(option=>option.value==='qbittorrent').disabled);
 console.log('Processing UI ETA tests passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
