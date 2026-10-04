import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source=await readFile(new URL('../homebrew/botty/ui/app.js',import.meta.url),'utf8');
const context=vm.createContext({});
vm.runInContext(source.slice(source.indexOf('function restModeText('),source.indexOf('function say(')),context);

test('web rest-mode status advertises only a currently accepted lease',()=>{
 assert.equal(context.restModeText({status:'active',active:true}),'Rest mode enabled for background services.');
 for(const mode of [undefined,{status:'unsupported',active:false},{status:'starting',active:false},{status:'stopped',active:false},{status:'active',active:false}]){
  assert.match(context.restModeText(mode),/Keep your PS5 awake/);
 }
});
test('failed or expired requests cannot retain the enabled label',()=>{
 for(const status of ['failed','expired'])assert.equal(context.restModeText({status,active:false}),'Rest-mode maintenance unavailable. Keep your PS5 awake.');
});
