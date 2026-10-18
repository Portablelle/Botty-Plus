'use strict';
const $=id=>document.getElementById(id);
let token='',state=null,tab='all',busy=false,modalAction=null,lastFocus=null,modalOpened=0,processingTasks=[],processingUnavailable=false;
const size=bytes=>{if(!Number.isFinite(bytes))return 'Unknown';for(const unit of ['B','KiB','MiB','GiB','TiB']){if(bytes<1024||unit==='TiB')return bytes.toFixed(unit==='B'?0:1)+' '+unit;bytes/=1024;}};
function node(tag,text,cls){const element=document.createElement(tag);if(text!==undefined)element.textContent=text;if(cls)element.className=cls;return element;}
async function api(path,body){const response=await fetch(path,{method:body===undefined?'GET':'POST',headers:{'X-Botty-Token':token,'Content-Type':'application/json'},body:body===undefined?undefined:JSON.stringify(body),cache:'no-store'});const result=await response.json();if(!response.ok)throw Error(result.error||'Request failed');return result;}
function restModeText(mode){
 if(mode?.status==='active'&&mode.active)return 'Rest mode enabled for background services.';
 if(mode?.status==='failed'||mode?.status==='expired')return 'Rest-mode maintenance unavailable. Keep your PS5 awake.';
 return 'Keep your PS5 awake during downloads and file operations.';
}
function say(text){$('message').textContent=text;}
async function action(path,body,message){if(busy)return;busy=true;try{await api(path,body);say(message);await refresh();}catch(error){say(error.message);}finally{busy=false;}}
function button(text,id,callback,disabled=false){const result=node('button',text);result.id=id;result.disabled=disabled;result.addEventListener('click',callback);return result;}
function modal(title,text,confirm,callback,field){$('storage-options').replaceChildren();$('confirm').disabled=false;modalOpened=performance.now();lastFocus=document.activeElement;$('modal-title').textContent=title;$('modal-text').textContent=text;$('confirm').textContent=confirm;$('field-label').hidden=!field;$('field').value='';$('field').type=field&&field.type||'text';$('field-name').textContent=field&&field.name||'';modalAction=callback;$('modal').hidden=false;(field?$('field'):$('confirm')).focus();}
function closeModal(){$('modal').hidden=true;modalAction=null;if(lastFocus&&lastFocus.isConnected)lastFocus.focus();else $('tab-'+tab).focus();}
$('cancel').onclick=closeModal;$('confirm').onclick=()=>{if(performance.now()-modalOpened<250)return;const fn=modalAction,value=$('field').value;closeModal();if(fn)fn(value);};
function chooseStorage(current,workflow,next){
 const adding=['download','import'].includes(workflow),transfer=workflow==='transfer',engineChoice=workflow==='download';
 modal(adding?'Where should this game go?':'Choose destination storage','Choose a disk using its available space. '+(adding?'Full auto detects the format and prepares on that disk. FPKG conversion runs on Server only. Preparation needs extra space; estimates follow inspection.':'Existing files are never overwritten.'),'Continue',()=>next({storage:$('storage-choice').value,...(adding?{automatic:$('process-choice').value==='auto',...(engineChoice?{engine:$('engine-choice').value}:{})}:{})}));
 if(adding&&engineChoice){const label=node('label','Download on'),engine=node('select');engine.id='engine-choice';for(const [value,text] of [['rtorrent','PS5 (rTorrent)'],['qbittorrent','Dedicated server (qBittorrent)']]){const option=node('option',text);option.value=value;option.disabled=value==='qbittorrent'&&!state.remoteDownloads?.enabled;engine.appendChild(option);}label.appendChild(engine);$('storage-options').appendChild(label);}
 const label=node('label','Storage'),select=node('select');select.id='storage-choice';
 for(const disk of state.storage||[]){const option=node('option',disk.label+' — '+(disk.available?size(disk.freeBytes)+' free':'Disconnected')+(disk.id===current?' (current disk)':''));option.value=disk.id;option.disabled=!disk.available||(transfer&&disk.id===current);select.appendChild(option);}
 if(!transfer)select.value=current||'internal';else select.value=Array.from(select.options).find(o=>!o.disabled)?.value||'';label.appendChild(select);$('storage-options').appendChild(label);
 if(adding){const modeLabel=node('label','Process'),mode=node('select');mode.id='process-choice';for(const [value,text] of [['auto','Full auto: Prepare and add to Library'],['download','Download only']]){const option=node('option',text);option.value=value;mode.appendChild(option);}modeLabel.appendChild(mode);$('storage-options').appendChild(modeLabel);}
 $('confirm').disabled=!Array.from(select.options).some(o=>!o.disabled);select.focus();
}
function storageLabel(id){return (state.storage||[]).find(d=>d.id===id)?.label||(id&&id!=='internal'?'External disk unavailable':'Internal SSD');}
function transferItem(kind,item){chooseStorage(item.storage||'internal','transfer',choice=>modal('Transfer to another disk?','Move to the selected disk and follow progress here. Keep both disks connected. Torrents remain paused after transfer.','Transfer',()=>action('/api/transfer',{kind,id:item.id,...choice},'Transfer started. Keep both disks connected.')));}
function archives(torrent){return (torrent.files||[]).filter(file=>{if(!file.name.toLowerCase().endsWith('.rar'))return false;const part=/\.part(\d+)\.rar$/i.exec(file.name);return !part||Number(part[1])===1;});}
const needsServerPreparation=torrent=>!!torrent.remotePreparation?.status&&torrent.leftUntilDone!==0;
function torrentCard(torrent){
 const card=node('article',undefined,'card'),completed=torrent.leftUntilDone===0,paused=torrent.status===0;
 card.appendChild(node('span',torrent.remoteCandidate?'AVAILABLE ON SERVER':torrent.error?'NEEDS ATTENTION':completed?'COMPLETED':paused?'PAUSED':torrent.remotePhase==='transfer'?'IMPORTING TO PS5':torrent.remote?'SERVER DOWNLOAD':'DOWNLOADING','badge'));
 card.appendChild(node('h2',torrent.name));if(torrent.phase)card.appendChild(node('p',torrent.phase,'muted'));if(torrent.formatHint)card.appendChild(node('p',torrent.formatHint,'muted'));
 if(torrent.remoteCandidate){card.appendChild(node('p','PS5 content candidate. Archive contents are checked after extraction. Originals stay on the dedicated server.'));card.appendChild(button('Import to PS5','torrent-'+torrent.id+'-import',()=>chooseStorage('internal','import',choice=>action('/api/remote/import',{hash:torrent.hashString,...choice},'Import queued.'))));return card;}card.appendChild(node('p',storageLabel(torrent.storage),'muted'));
 const progress=node('progress');progress.max=1;progress.value=torrent.percentDone||0;progress.setAttribute('aria-label','Download progress');card.appendChild(progress);
 card.appendChild(node('p',Math.round((torrent.percentDone||0)*100)+'% · '+size(torrent.totalSize)+' · ↓ '+size(torrent.rateDownload)+'/s · ↑ '+size(torrent.rateUpload)+'/s'));
 if(torrent.errorString)card.appendChild(node('p',torrent.errorString,'error'));
 const actions=node('div',undefined,'actions');
 actions.appendChild(button(paused?'Resume':'Pause','torrent-'+torrent.id+'-toggle',()=>action('/api/torrent',{id:torrent.id,action:paused?'resume':'pause'},paused?'Torrent resumed.':'Torrent paused.')));
 if(!torrent.remote)actions.appendChild(button('Verify','torrent-'+torrent.id+'-verify',()=>modal('Verify downloaded files','rTorrent will recheck downloaded pieces. Extraction waits until verification is complete.','Verify',()=>action('/api/torrent',{id:torrent.id,action:'verify'},'Verification requested.'))));
 if(state.torrentRemovalSupported)actions.appendChild(button("Delete torrent & files","torrent-"+torrent.id+"-delete",()=>modal("Delete torrent and downloaded files?","This permanently deletes the torrent and its downloaded archives. Games in Library are kept.","Delete",()=>action("/api/torrent",{id:torrent.id,action:"remove-data",confirmed:true},"Torrent removed; downloaded-file deletion requested.")),!!state.extracting));
 const starts=archives(torrent);
 if(((completed&&(torrent.looseContent||torrent.emulationContent))||["failed","download-only"].includes(torrent.remotePreparation?.status)))actions.appendChild(button("Prepare game","torrent-"+torrent.id+"-prepare",()=>prepareTorrent(torrent),!!state.extracting));
 if(!needsServerPreparation(torrent))starts.forEach((file,index)=>actions.appendChild(button(starts.length===1?'Prepare game':'Prepare '+file.name.split('/').pop(),'torrent-'+torrent.id+'-extract-'+index,()=>modal('Prepare game on PS5',file.name+'\nArchive volumes are kept for seeding. The game is checked and added to Library automatically.','Extract',password=>chooseStorage(torrent.storage||'internal','storage',choice=>action('/api/extract',{id:torrent.id,archive:file.name,password,automatic:true,...choice},'Preparation started. Follow it in Activity.')),{name:'Archive password (leave empty if none)',type:'password'}),!completed||!!torrent.error||!!state.extracting||[1,2].includes(torrent.status))));
 if(state.storageSupported&&(!torrent.remote||completed))actions.appendChild(button('Transfer to another disk','torrent-'+torrent.id+'-transfer',()=>transferItem('torrent',torrent),!!state.extracting));
 card.appendChild(actions);
 if(completed&&!starts.length&&!torrent.looseContent&&!torrent.emulationContent)card.appendChild(node('p','No supported game found. Check the files before preparing.'));
 return card;
}
function prepareTorrent(torrent){
 const packages=torrent.remotePreparation?.packages||[];
 const start=archive=>{
  if(needsServerPreparation(torrent))modal('Convert on Server','The prepared game will use '+storageLabel(torrent.storage)+', selected for this download. Originals stay on Server.','Prepare',()=>action('/api/extract',{id:torrent.id,archive,automatic:true,storage:torrent.storage||'internal'},'Preparation requested. Follow it in Activity.'));
  else chooseStorage(torrent.storage||'internal','storage',choice=>action('/api/extract',{id:torrent.id,archive,automatic:true,...choice},'Preparation requested. Follow it in Activity.'));
 };
 if(packages.length>1){
  modal('Choose server package','Select the game or backport to convert. DLC packages are not standalone games. Originals stay on Server.','Continue',()=>start($('package-choice').value));
  const select=node('select');select.id='package-choice';
  for(const name of packages){const option=node('option',name);option.value=name;select.appendChild(option);}
  $('storage-options').appendChild(select);select.focus();
 }else start(packages[0]||'');
}
function activityGroup(value,torrent=false){
 if(torrent){if(value.error||['failed','download-only'].includes(value.remotePreparation?.status))return 'jobs';return value.leftUntilDone===0?'complete':'all';}
 if(value.error||['failed','interrupted','move-error','uncertain','waiting-close'].includes(value.status))return 'jobs';
 if(value.libraryStatus==='pending')return 'jobs';
 return ['ready','moved','complete','completed','cancelled','restored','deleted'].includes(value.status)?'complete':'all';
}
const replacesJob=task=>!['complete','completed','failed','cancelled','interrupted','restored','deleted'].includes(task.status);
function reconcileActivity(){
 const allJobs=state?.jobs||[];
 const trackedJobs=allJobs.filter(j=>!j.dismissed).map(job=>{
  const failure=processingTasks.find(task=>task.id===job.id&&task.status==='failed');
  const errors=[job.error,job.compression?.error,failure?.error].filter(Boolean);
  return {...job,error:Array.from(new Set(errors)).join(' · ')};
 });
 const tasks=processingUnavailable?[]:processingTasks.filter(t=>!allJobs.some(j=>j.id===t.id&&!replacesJob(t)));
 const jobs=trackedJobs.filter(j=>!tasks.some(t=>t.id===j.id&&replacesJob(t)));
 const sources=(state?.torrents||[]).filter(t=>!t.adopted);
 const downloads=sources.filter(t=>!t.remoteCandidate&&!trackedJobs.some(j=>j.hash&&j.hash===t.hashString&&j.torrentId===t.id&&!['failed','interrupted','cancelled'].includes(j.status)));
 return {sources,tasks,rows:downloads.map(value=>({value,torrent:true})).concat(jobs.map(value=>({value,torrent:false})))};
}
function activityRows(){
 const activity=reconcileActivity();
 if(tab==='server')return activity.sources.filter(t=>t.remoteCandidate).map(value=>({value,torrent:true}));
 if(tab==='seed')return activity.sources.filter(t=>!t.remoteCandidate&&t.leftUntilDone===0).map(value=>({value,torrent:true}));
 return activity.rows.filter(row=>activityGroup(row.value,row.torrent)===tab);
}
const extractionETA=seconds=>!Number.isFinite(seconds)||seconds<0?'ETA calculating…':seconds<60?'ETA < 1 min':seconds<3600?'ETA ~'+Math.ceil(seconds/60)+' min':'ETA ~'+Math.floor(seconds/3600)+' h '+Math.ceil((seconds%3600)/60)+' min';
function jobCard(job){const card=node('article',undefined,'card');card.appendChild(node('span',job.status.toUpperCase(),'badge'));card.appendChild(node('h2',job.name));card.appendChild(node('p',job.phase||''));if(job.total){const progress=node('progress');progress.max=job.total;progress.value=job.bytes||0;progress.setAttribute('aria-label','Extraction progress');card.appendChild(progress);card.appendChild(node('p',size(job.bytes)+' / '+size(job.total)));}if(job.status==='extracting')card.appendChild(node('p',size(job.extractionRate||0)+'/s · '+extractionETA(job.eta)));if(job.file&&job.status==='extracting')card.appendChild(node('p',job.file));if(job.error)card.appendChild(node('p',job.error,'error'));if(job.content&&job.content.reason)card.appendChild(node('p',job.content.reason));if(job.destination)card.appendChild(node('p',job.destination));
 appendJobControls(card,job);return card;}
function appendJobControls(card,job){
 const actions=node('div',undefined,'actions');
 if(job.status==='extracting')actions.appendChild(button('Cancel extraction','job-'+job.id+'-cancel',()=>modal('Cancel extraction?','Stop at the next safe point. Partial files and original archives are kept.','Stop',()=>action('/api/cancel-extraction',{id:job.id},'Cancellation requested.'))));
 if(!job.dismissed&&!(job.adoptedImport&&job.status!=='moved')&&['ready','moved','failed','cancelled','interrupted'].includes(job.status))actions.appendChild(button('Remove from list','job-'+job.id+'-dismiss',()=>modal('Remove from list?',['failed','cancelled','interrupted'].includes(job.status)?'Delete partial files and remove this row. Original downloads and archives are kept.':'Files are kept. This only hides the history row.','Remove',()=>action('/api/dismiss-extraction',{id:job.id},'Removed from the list.')),!!state.extracting));
 if(job.status==='ready'&&(job.adoptedImport||job.content&&job.content.kind!=='unsupported'))actions.appendChild(button('Move to library','job-'+job.id+'-move',()=>chooseStorage(job.storage||'internal','storage',choice=>action('/api/move',{id:job.id,...choice},'Library preparation started.')),!!state.extracting));
 if(!(job.adoptedImport&&job.status!=='moved')&&['ready','failed','interrupted','cancelled'].includes(job.status))actions.appendChild(button('Delete extraction','job-'+job.id+'-delete',()=>modal('Delete extracted files?','This deletes this extraction and any partial output. The original torrent and its archive volumes are kept.','Delete extraction',()=>action('/api/delete-extraction',{id:job.id},'Extracted copy deleted. Original downloads kept.')),!!state.extracting));
 if(state.storageSupported&&job.platform!=='switch'&&['ready','moved'].includes(job.status))actions.appendChild(button('Transfer to another disk','job-'+job.id+'-transfer',()=>transferItem('job',job),!!state.extracting||!!state.compression?.busy));
 if(job.status==='moved'&&state.compression?.supported&&job.compression?.status!=='ready'&&job.content?.kind==='folder')actions.appendChild(button('Compress game','job-'+job.id+'-compress',()=>chooseStorage(job.storage||'internal','storage',choice=>modal('Compress game?','Create a separate compressed copy on the selected disk. Keep the original. Full verification is optional. Close Botty+ when prompted.','Compress',()=>action('/api/compress-game',{id:job.id,confirmed:true,...choice},'Compression requested.'))),!!state.extracting||!!state.compression?.busy));
 if(job.compression?.status==='ready'){
  card.appendChild(node('p',job.compression.verified?'Verified':'Not verified','badge'));
  if(job.compression.originalKept)actions.appendChild(button('Verify compressed copy','job-'+job.id+'-verify-copy',()=>action('/api/verify-compressed',{id:job.id},'Optional verification queued. Close Botty+ and games.'),!!state.compression?.busy));
 }
 if(state.compression?.jobId===job.id&&state.compression?.verifyRequested&&['waiting-close','verifying'].includes(state.compression.status))actions.appendChild(button('Skip verification','job-'+job.id+'-skip-verify',()=>action('/api/skip-verification',{id:job.id},'Stopping verification safely. Original kept.')));
 card.appendChild(node('p',storageLabel(job.storage),'muted'));card.appendChild(actions);}
function render(){renderProcessing();if(!state)return;const focused=document.activeElement&&document.activeElement.id;const items=$('items');while(items.firstChild)items.removeChild(items.firstChild);
 $('rest-mode').textContent=restModeText(state.restMode);$('space').textContent=(state.storage||[{label:'Internal SSD',available:true,freeBytes:state.freeBytes}]).map(d=>d.label+': '+(d.available?size(d.freeBytes)+' free':'Disconnected')).join(' · ');if(state.transfer&&['running','uncertain'].includes(state.transfer.status))say(state.transfer.error||state.transfer.phase);$('connection').hidden=state.transmissionReady;$('connection').textContent=state.error||'';
 const values=activityRows();
 values.forEach(({value,torrent})=>items.appendChild(torrent?torrentCard(value):jobCard(value)));
 if(!values.length)items.appendChild(node('p',tab==='jobs'?'Nothing needs your attention.':tab==='server'?'No server candidates.':tab==='seed'?'No retained downloads.':tab==='complete'?'No completed tasks.':'No active preparation. Add a game to get started.','empty'));
 for(const key of ['all','complete','jobs','seed','server'])$('tab-'+key).setAttribute('aria-pressed',String(tab===key));
 if(focused&&$(focused)&&!$(focused).disabled)$(focused).focus({preventScroll:true});
}
function renderProcessing(){
  const focused=document.activeElement&&document.activeElement.id;
  const section=$('processing');section.replaceChildren();
  if(processingUnavailable){section.hidden=false;section.appendChild(node('p','Live task status unavailable. Reconnecting…','notice'));return;}
  const visible=reconcileActivity().tasks.filter(task=>activityGroup(task)===tab);section.hidden=!visible.length;
  for(const task of visible){
   const card=node('article',undefined,'card');card.appendChild(node('h2',task.name||'File operation'));
   card.appendChild(node('p',task.phase||task.status));
   if(task.total>0){const bar=node('progress');bar.max=task.total;bar.value=Math.min(task.bytes||0,task.total);bar.setAttribute('aria-label',task.phase||'Progress');card.appendChild(bar);
    card.appendChild(node('p',task.unit==='items'?`${task.bytes||0} / ${task.total} items`:`${size(task.bytes||0)} / ${size(task.total)}`));}
   if(task.unit!=='items'&&(task.rate>0||Number.isFinite(task.eta)&&task.eta>=0))card.appendChild(node('p',`${size(task.rate||0)}/s · ${extractionETA(task.eta)}`));
   if(task.error)card.appendChild(node('p',task.error,'error'));
   const job=(state?.jobs||[]).find(j=>j.id===task.id);
   if(job&&replacesJob(task))appendJobControls(card,job);
   section.appendChild(card);
  }
  if(focused&&$(focused)&&!$(focused).disabled)$(focused).focus({preventScroll:true});
}
async function refreshProcessing(){
 if(!token)return;
 try{const result=await api('/api/processing');processingTasks=result.tasks||[];processingUnavailable=false;}
 catch(error){processingUnavailable=true;}
 if($('modal').hidden)render();else renderProcessing();
}
async function refresh(){try{if(!token)token=(await api('/api/bootstrap')).token;state=await api('/api/state');if($('modal').hidden)render();}catch(error){$('rest-mode').textContent='Rest-mode status unavailable. Keep your PS5 awake.';$('connection').hidden=false;$('connection').textContent=error.message+' If the PS5 was restarted, start a session from the Botty portal.';}}
for(const element of document.querySelectorAll('[data-tab]'))element.onclick=()=>{tab=element.dataset.tab;render();};
$('refresh').onclick=refresh;$('add').onclick=()=>modal('Add a download','Paste a magnet link, then choose PS5 or dedicated server.','Add magnet',magnet=>chooseStorage('internal','download',choice=>action('/api/torrent',{action:'add',magnet:magnet.trim(),...choice},'Torrent added.')),{name:'Magnet link'});
$('upload').onclick=()=>{if(!busy){$('torrent-file').value='';$('torrent-file').click();}};
$('torrent-file').onchange=async()=>{
 const file=$('torrent-file').files[0];if(!file||busy)return;
 if(!/\.torrent$/i.test(file.name)){say('Choose a .torrent file.');return;}
 if(!file.size||file.size>1024*1024){say('Torrent files must be between 1 byte and 1 MiB.');return;}
 busy=true;$('upload').disabled=true;
 try{
  const bytes=new Uint8Array(await file.arrayBuffer());let binary='';
  for(let offset=0;offset<bytes.length;offset+=8192)binary+=String.fromCharCode(...bytes.subarray(offset,offset+8192));
  const metainfo=btoa(binary);
  // Refresh disks before offering a destination; external storage may have changed.
  state=await api('/api/state');busy=false;
  const add=choice=>action('/api/torrent',{action:'add',metainfo,...choice},'Torrent added.');
  if(state.storageSupported)chooseStorage('internal','download',add);
  else await add({storage:'internal',automatic:false});
 }catch(error){say(error.message);}finally{busy=false;$('upload').disabled=false;}
};
function focusables(){const scope=$('modal').hidden?document:$('modal');return Array.from(scope.querySelectorAll('button:not(:disabled),input,select')).filter(el=>el.getClientRects().length);}
function navigate(direction){const choices=focusables(),current=document.activeElement;if(!choices.includes(current)){if(choices[0])choices[0].focus();return;}const rect=current.getBoundingClientRect(),x=rect.left+rect.width/2,y=rect.top+rect.height/2;let best=null,score=Infinity;for(const item of choices){if(item===current)continue;const r=item.getBoundingClientRect(),dx=r.left+r.width/2-x,dy=r.top+r.height/2-y;const primary=direction==='left'?-dx:direction==='right'?dx:direction==='up'?-dy:dy;const secondary=direction==='left'||direction==='right'?Math.abs(dy):Math.abs(dx);if(primary>5&&primary+secondary*2<score){score=primary+secondary*2;best=item;}}if(best){best.focus();best.scrollIntoView({block:'nearest'});}}
window.addEventListener('keydown',event=>{if(event.key==='Escape'||event.key==='Backspace'&&document.activeElement.tagName!=='INPUT'){if(!$('modal').hidden){event.preventDefault();closeModal();}return;}if(event.key==='Tab'&&!$('modal').hidden){event.preventDefault();const list=focusables(),index=list.indexOf(document.activeElement);list[(index+(event.shiftKey?-1:1)+list.length)%list.length].focus();return;}if(document.activeElement.tagName==='SELECT')return;if(document.activeElement.tagName==='INPUT'&&!['ArrowUp','ArrowDown'].includes(event.key))return;const directions={ArrowLeft:'left',ArrowRight:'right',ArrowUp:'up',ArrowDown:'down'};if(directions[event.key]){event.preventDefault();navigate(directions[event.key]);}});
let lastPad=0,previous=[];function gamepad(now){const pads=navigator.getGamepads?navigator.getGamepads():[];const pad=Array.from(pads).find(Boolean);if(pad){const pressed=pad.buttons.map(b=>b.pressed);if(pressed[0]&&!previous[0]&&document.activeElement)document.activeElement.click();if(pressed[1]&&!previous[1]&&!$('modal').hidden)closeModal();if(now-lastPad>180){const direction=pressed[12]||pad.axes[1]<-.6?'up':pressed[13]||pad.axes[1]>.6?'down':pressed[14]||pad.axes[0]<-.6?'left':pressed[15]||pad.axes[0]>.6?'right':null;if(direction&&(document.activeElement.tagName!=='INPUT'||direction==='up'||direction==='down')){navigate(direction);lastPad=now;}}previous=pressed;}requestAnimationFrame(gamepad);}
$('tab-all').focus();refresh();setInterval(()=>{if(!busy&&$('modal').hidden)refresh();},3000);requestAnimationFrame(gamepad);
setInterval(refreshProcessing,1000);
