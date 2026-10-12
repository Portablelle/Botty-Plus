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
