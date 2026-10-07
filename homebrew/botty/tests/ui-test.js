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
}
const elements=new Map();
const context=vm.createContext({
 document:{getElementById(id){if(!elements.has(id))elements.set(id,new Element('section'));return elements.get(id);},createElement:tag=>new Element(tag),querySelectorAll:()=>[]},
 window:{addEventListener(){}},fetch:()=>new Promise(()=>{}),setInterval(){},requestAnimationFrame(){}
});
vm.runInContext(fs.readFileSync(require('node:path').join(__dirname,'../ui/app.js'),'utf8'),context);
async function render(task){
 context.fixture=task;
 await vm.runInContext("token='fixture';api=async()=>({tasks:[fixture]});refreshProcessing()",context);
 return elements.get('processing').children[0].children.filter(child=>child.tag==='p').map(child=>child.textContent);
}
(async()=>{
 const task={name:'Library move',phase:'Moving game',unit:'bytes',bytes:100,total:1000,rate:0,eta:120};
 assert((await render(task)).includes('0 B/s · ETA ~2 min'));
 assert(!(await render({...task,eta:-1})).some(text=>text.includes('ETA')));
 assert((await render({...task,rate:1024,eta:-1})).includes('1.0 KiB/s · ETA calculating…'));
 assert((await render({...task,eta:0})).includes('0 B/s · ETA < 1 min'));
 assert(!(await render({...task,unit:'items'})).some(text=>text.includes('ETA')));
 console.log('Processing UI ETA tests passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
