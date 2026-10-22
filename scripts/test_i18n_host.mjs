/* Execute the shipped language module with only DOM boot services stubbed. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
const source=fs.readFileSync(path.resolve(import.meta.dirname,'../assets/i18n.js'),'utf8');
function run({query='',saved=null,browser='en-US',deny=false}={}){
 let assigned='',stored=saved;
 const context={URL,NodeFilter:{SHOW_TEXT:4},navigator:{languages:[browser]},window:{},
  location:{href:'http://example.test/?'+query+'#%2Fdata%2Fowned',assign(value){assigned=value;}},
  localStorage:{getItem(){if(deny)throw Error('denied');return stored;},setItem(key,value){if(deny)throw Error('denied');stored=value;}},
  document:{documentElement:{},body:{querySelectorAll(){return[];}},createTreeWalker(){return{nextNode(){return null;}};}}};
 vm.runInNewContext(source,context);
 return {api:context.window.BFpilotI18n,document:context.document,result:()=>({assigned,stored})};
}
const english=run();assert.equal(english.api.locale,'en');assert.equal(english.api.t('Copy'),'Copy');
const chinese=run({browser:'zh-CN'});assert.equal(chinese.api.locale,'zh-CN');assert.equal(chinese.api.preference,'auto');assert.equal(chinese.document.documentElement.lang,'zh-CN');assert.equal(chinese.api.t('Copy'),'复制');
assert.equal(run({browser:'zh-TW'}).api.locale,'zh-CN');
assert.equal(run({browser:'zh-CN',saved:'en'}).api.locale,'en');
assert.equal(run({browser:'en-US',saved:'en',query:'lang=zh-CN'}).api.locale,'zh-CN');
assert.equal(run({browser:'en-US',saved:'bogus',query:'lang=bogus'}).api.locale,'en');
assert.equal(chinese.api.t('/data/Copy/Name'),'/data/Copy/Name');
assert.equal(chinese.api.t('toString'),'toString');assert.equal(chinese.api.t(0),0);
const template='{kind} conflict: "{name}" already exists in the target directory. {action}?';
const name='Copy"{kind}<script>中文';
const message=chinese.api.t(template,{kind:'文件',name,action:'覆盖'});
assert.equal(message,'文件冲突：目标目录中已存在“'+name+'”。是否覆盖？');
assert.equal(english.api.t('{missing}',{other:'x'}),'{missing}');
const denied=run({deny:true,query:'lang=zh-CN'});denied.api.setPreference('en');
const next=new URL(denied.result().assigned);assert.equal(next.searchParams.get('lang'),'en');assert.equal(next.hash,'#%2Fdata%2Fowned');
english.api.setPreference('zh-CN');assert.equal(english.result().stored,'zh-CN');
console.log('Language resolution/override, storage failure, original unknown data and nonrecursive named interpolation pass');
