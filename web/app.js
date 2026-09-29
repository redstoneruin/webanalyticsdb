'use strict';
const $ = id => document.getElementById(id);
const state = {page:'overview', csrf:null, boot:null, tables:[], table:null, feed:null, previous:null,
  rates:[], stats:null, cursor:null, result:null, errors:[], liveRows:[], liveColumns:[], liveLast:null,
  liveSkipped:0n, livePaused:false, liveDirty:true, liveBusy:false, tablesBusy:false, jobsBusy:false,
  segmentsAfter:'0', segmentsLast:'0', segmentsMore:false, selection:0, byteRate:0, preview:null, authenticated:false, connecting:false};
const names = {overview:'Overview', tables:'Tables', query:'Query', live:'Live events', workload:'Test workload', storage:'Storage & health'};
function node(tag, text, cls) { const n=document.createElement(tag); if(text!==undefined)n.textContent=text; if(cls)n.className=cls; return n; }
function count(value) { try{return BigInt(value || 0).toLocaleString();}catch{return '—';} }
function bytes(value) { const n=Number(value||0); const units=['B','KiB','MiB','GiB','TiB']; let v=n,i=0; while(v>=1024&&i<4){v/=1024;i++;} return `${v.toLocaleString(undefined,{maximumFractionDigits:i?1:0})} ${units[i]}`; }
function latency(value) { const n=Number(value||0); return n>=1000?`${(n/1000).toFixed(1)} ms`:`${n} µs`; }
function duration(us) { const sec=Math.floor(Number(us||0)/1e6); return sec>=3600?`${Math.floor(sec/3600)}h ${Math.floor(sec%3600/60)}m`:`${Math.floor(sec/60)}m ${sec%60}s`; }
function time(us) { if(us===null||us===undefined)return '—'; const d=new Date(Number(BigInt(us)/1000n)); return Number.isFinite(d.getTime())?d.toLocaleString():'Out of date range'; }
function connection(text, connected=false) { $('connection-label').textContent=text; $('connection-dot').classList.toggle('connected',connected); }
function report(message,error=false) {
  $('notice').hidden=false; $('notice').classList.toggle('error',error); $('notice').textContent=message;
  if(error){state.errors.unshift({time:new Date().toLocaleTimeString(),message});state.errors.length=Math.min(30,state.errors.length);renderErrors();}
}
function renderErrors(){ const root=$('recent-errors'); root.replaceChildren(); root.classList.toggle('empty',!state.errors.length); if(!state.errors.length)root.textContent='No errors in this console session.'; for(const e of state.errors)root.append(node('div',`${e.time}  ${e.message}`,'error-entry')); }
function loginNeeded(){state.authenticated=false;state.csrf=null;state.feed?.close();state.feed=null;connection('Sign in required');if(!$('login-dialog').open)$('login-dialog').showModal();}
async function api(path,body,options={}) {
  const headers={}; if(body!==undefined){headers['Content-Type']='application/json';if(state.csrf)headers['X-CSRF-Token']=state.csrf;}
  const response=await fetch(`/api/v1${path}`,{method:body===undefined?'GET':'POST',headers,body:body===undefined?undefined:JSON.stringify(body),credentials:'same-origin',cache:'no-store'});
  let result; try{result=await response.json();}catch{throw new Error(`HTTP ${response.status}: response unavailable; an append may have committed. Check the persisted tail before retrying.`);}
  if(!response.ok){if(response.status===401&&!options.login)loginNeeded();const error=new Error(result.error?.message||`HTTP ${response.status}`);error.status=response.status;throw error;}
  return result;
}
function action(fn){return async event=>{event?.preventDefault();const button=event?.submitter||(event?.currentTarget?.tagName==='BUTTON'?event.currentTarget:null);if(button)button.disabled=true;try{await fn(event);}catch(error){report(error.message,true);}finally{if(button){button.disabled=false;if(['next-page','close-cursor'].includes(button.id))cursorButtons();if(button.id==='segments-next')button.disabled=!state.segmentsMore;}}};}
function selected(){if(!state.table)throw new Error('Select a table first.');return state.table.id;}
function dataTable(root,columns,rows){
  root.replaceChildren();root.classList.toggle('empty',!rows.length);if(!rows.length){root.textContent='No rows to display.';return;}
  const wrap=node('div',undefined,'table-scroll'),table=node('table'),thead=node('thead'),header=node('tr');for(const c of columns)header.append(node('th',c));thead.append(header);table.append(thead);
  const body=node('tbody');for(const row of rows){const tr=node('tr');for(const value of row){const td=node('td');if(value instanceof Node)td.append(value);else{td.textContent=value===null?'null':String(value??'');td.title=td.textContent;td.className='number';}tr.append(td);}body.append(tr);}table.append(body);wrap.append(table);root.append(wrap);
}
function details(root,items){root.replaceChildren();for(const [label,value] of items){const item=node('div',undefined,'detail-item');item.append(node('span',label),node('span',value));root.append(item);}}
function tableLink(table){const button=node('button',table.name,'text-button');button.addEventListener('click',action(async()=>{await selectTable(table.id);showPage('tables');}));return button;}
function renderTables(){
  for(const id of ['overview-tables','all-tables'])dataTable($(id),['Table','Retained rows','Row width','Source bytes','Segments','Status'],state.tables.map(t=>[tableLink(t),count(t.rows),`${t.row_width} B`,bytes(t.bytes),count(t.segments),t.read_only?'Read only':'Ready']));
  const previous=$('table-select').value; $('table-select').replaceChildren(new Option('Select a table',''));
  for(const t of state.tables)$('table-select').append(new Option(t.name,t.id));
  if(state.tables.some(t=>t.id===previous))$('table-select').value=previous;
  if(state.stats)renderStats(state.stats,false);
}
async function refreshTables(){if(state.tablesBusy||!state.authenticated)return;state.tablesBusy=true;try{const result=await api('/tables');state.tables=result.tables;renderTables();if(!state.table&&state.tables.length)await selectTable(state.tables[0].id);else if(state.table){const id=state.table.id;const t=await api(`/tables/${id}`);if(state.table?.id===id){state.table=t;renderSchema(false);}}}finally{state.tablesBusy=false;}}
async function selectTable(id){
  const generation=++state.selection;
  if(!id){state.table=null;$('table-select').value='';return;}
  if(state.cursor)await releaseCursor();const table=await api(`/tables/${id}`);if(generation!==state.selection)return;state.table=table;$('table-select').value=id;
  state.liveRows=[];state.liveColumns=[];state.liveLast=null;state.liveSkipped=0n;state.liveDirty=true;state.preview=null;state.segmentsAfter='0';
  state.result=null;$('query-results').replaceChildren();$('query-results').classList.add('empty');$('query-results').textContent='Run a query for the selected table.';$('query-stats').textContent='';$('query-status').textContent='';$('export-json').disabled=true;$('export-csv').disabled=true;
  renderSchema(true);renderLive();$('retention-apply').hidden=true;$('retention-summary').textContent='';
  if(state.page==='storage')await loadSegments(true);if(state.page==='live')await loadLive();
}
function renderSchema(changed){
  const t=state.table;if(!t)return;$('schema-title').textContent=t.name;$('row-width').textContent=`${t.row_width} bytes / row`;
  dataTable($('schema-detail'),['Field','Type','Width','Nullable'],t.fields.map(f=>[f.name,f.type+(Number(f.size)?`[${f.size}]`:''),`${f.width} B`,f.nullable?'Yes':'No']));
  details($('storage-health'),[['Schema identity',t.schema_hash],['Retained rows',count(t.rows)],['Source bytes',bytes(t.bytes)],['Last sequence',count(t.last_sequence)],['Ingestion range',Number(t.rows)?`${time(t.min_time)} → ${time(t.max_time)}`:'Empty'],['Write state',t.read_only?'Read only':'Ready'],['Maintenance',t.maintenance_status],['Pending deletion',t.pending_retention?'Waiting for readers / cleanup':'None']]);
  if(changed){
    $('retention-days').value=String(Number(t.retention_us)/86400000000);
    for(const id of ['query-field','filter-field']){$(id).replaceChildren(new Option(id==='filter-field'?'No filter':'Select field',''));for(const f of t.fields)$(id).append(new Option(f.name,f.name));}
  }
}
function renderStats(s,record=true){
  state.stats=s;let rate=state.rates.at(-1)||0,byteRate=state.byteRate;
  if(record&&state.previous&&state.previous.boot_id===s.boot_id){const elapsed=(Number(s.uptime_us)-Number(state.previous.uptime_us))/1e6;if(elapsed>0){rate=Math.max(0,Number(BigInt(s.committed_rows)-BigInt(state.previous.committed_rows))/elapsed);byteRate=Math.max(0,Number(BigInt(s.committed_bytes)-BigInt(state.previous.committed_bytes))/elapsed);}}
  if(record){state.byteRate=byteRate;state.previous=s;state.rates.push(rate);if(state.rates.length>60)state.rates.shift();}
  const retained=state.tables.reduce((a,t)=>a+BigInt(t.rows),0n);
  const metrics=[['Committed rows / sec',Math.round(rate).toLocaleString(),`${bytes(byteRate)}/s of committed frames`],['Retained rows',count(retained),`${count(s.table_count)} tables · ${count(s.committed_rows)} committed this boot`],['Append p99',latency(s.append_latency.p99_us),`${count(s.append_latency.samples)} durable acknowledgements`],['Disk free',s.disk_free_available?bytes(s.disk_free_bytes):'Unavailable',`Uptime ${duration(s.uptime_us)}`]];
  $('metrics').replaceChildren(...metrics.map(([label,value,note])=>{const card=node('div',undefined,'metric');card.append(node('div',label,'metric-label'),node('div',value,'metric-value'),node('div',note,'metric-note'));return card;}));
  $('health').textContent=s.read_only?'Write attention required':'Database healthy';$('health').classList.toggle('bad',s.read_only);
  details($('health-metrics'),[['Append p50 / p95',`${latency(s.append_latency.p50_us)} / ${latency(s.append_latency.p95_us)}`],['Sync p99',latency(s.sync_latency.p99_us)],['Query p99',latency(s.query_latency.p99_us)],['Write queue',`${bytes(s.queue_bytes)} / ${bytes(s.queue_limit_bytes)}`],['Read cache / readers',`${bytes(s.cache_bytes)} / ${count(s.readers)}`],['Failed / rejected appends',`${count(s.failed_appends)} / ${count(s.rejected_appends)}`],['Failed queries / clock clamps',`${count(s.failed_queries)} / ${count(s.clock_adjustments)}`]]);
  $('rate-label').textContent=`${Math.round(rate).toLocaleString()} rows/s`;
  const peak=Math.max(1,...state.rates);const points=state.rates.map((v,i)=>[i*720/Math.max(1,state.rates.length-1),165-v/peak*145]);
  const d=points.map((p,i)=>`${i?'L':'M'}${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ');$('chart-line').setAttribute('d',d);$('chart-fill').setAttribute('d',points.length?`${d} L${points.at(-1)[0]},180 L0,180 Z`:'');$('chart-empty').textContent=state.rates.some(v=>v>0)?'':'Waiting for activity';
}
async function connectFeed(){
  if(state.connecting||!state.authenticated)return;state.connecting=true;
  try{
    state.feed?.close();const s=await api('/stats');if(!state.authenticated)return;
    if(state.boot&&state.boot!==s.boot_id){state.previous=null;state.rates=[];state.cursor=null;cursorButtons();state.liveLast=null;state.liveRows=[];state.liveSkipped=0n;}
    state.boot=s.boot_id;renderStats(s);
    const feed=new EventSource(`/api/v1/stream?boot=${encodeURIComponent(s.boot_id)}&after=${encodeURIComponent(s.feed_cursor)}`);state.feed=feed;
    feed.onopen=()=>connection('Live connection',true);
    feed.addEventListener('stats',event=>{try{renderStats(JSON.parse(event.data));}catch(error){report(error.message,true);}});
    feed.addEventListener('commits',event=>{try{const data=JSON.parse(event.data);if(data.gap||data.commits.some(c=>c.table_id===state.table?.id))state.liveDirty=true;}catch(error){report(error.message,true);}});
    feed.addEventListener('reset',()=>{state.liveDirty=true;connection('Refreshing live snapshot');feed.close();setTimeout(()=>{connectFeed().catch(e=>report(e.message,true));refreshTables().catch(e=>report(e.message,true));},100);});
    feed.onerror=()=>{connection('Reconnecting');if(!state.authenticated)return;api('/session').catch(e=>{if(e.status!==401)report(e.message,true);});};
  }finally{state.connecting=false;}
}
async function startSession(){state.authenticated=true;$('login-dialog').close();await refreshTables();await connectFeed();}
function showPage(page){if(!names[page])return;state.page=page;for(const id of Object.keys(names))$(`page-${id}`).hidden=id!==page;document.querySelectorAll('nav button').forEach(b=>{b.classList.toggle('active',b.dataset.page===page);if(b.dataset.page===page)b.setAttribute('aria-current','page');else b.removeAttribute('aria-current');});$('page-label').textContent=names[page].toUpperCase();if(page==='live'){state.liveDirty=true;loadLive().catch(e=>report(e.message,true));}if(page==='storage')loadSegments(true).catch(e=>report(e.message,true));if(page==='workload')loadJobs().catch(e=>report(e.message,true));}
function addField(name='',type='u32',size=''){
  const row=node('div',undefined,'schema-row'),field=node('input'),types=node('select'),capacity=node('input'),nullable=node('label',undefined,'checkbox'),check=node('input'),remove=node('button','×','text-button');
  field.placeholder='field_name';field.value=name;field.required=true;field.pattern='[A-Za-z_][A-Za-z0-9_]*';field.maxLength=63;field.setAttribute('aria-label','Field name');field.dataset.part='name';
  for(const t of ['i8','u8','i16','u16','i32','u32','i64','u64','f32','f64','bool','timestamp_us','uuid','bytes','text','symbol32'])types.append(new Option(t,t));types.value=type;types.dataset.part='type';types.setAttribute('aria-label','Field type');
  capacity.type='number';capacity.min='1';capacity.max='16368';capacity.placeholder='Size';capacity.value=size;capacity.dataset.part='size';capacity.setAttribute('aria-label','Byte or text capacity');
  function toggle(){capacity.disabled=!['bytes','text'].includes(types.value);capacity.required=!capacity.disabled;}types.addEventListener('change',toggle);toggle();
  check.type='checkbox';check.dataset.part='nullable';nullable.append(check,document.createTextNode('Null'));remove.type='button';remove.setAttribute('aria-label','Remove field');remove.addEventListener('click',()=>row.remove());row.append(field,types,capacity,nullable,remove);$('schema-fields').append(row);
}
function list(input){return input.trim()?input.split(',').map(x=>x.trim()).filter(Boolean):[];}
function queryOptions(){
  if($('advanced-enabled').checked)return JSON.parse($('query-json').value);
  const q={limit:Number($('query-limit').value)};for(const [id,key] of [['query-start','start_us'],['query-end','end_us']])if($(id).value)q[key]=(BigInt(new Date($(id).value).getTime())*1000n).toString();
  const projection=list($('query-projection').value),groups=list($('query-groups').value);if(projection.length)q.projection=projection;if(groups.length)q.group_by=groups;
  const agg=$('query-aggregate').value;if(agg){q.aggregates=[{op:agg,name:'value'}];if(agg!=='count_all')q.aggregates[0].field=$('query-field').value;if($('query-order').value){q.order_by='value';q.descending=$('query-order').value==='desc';}}
  const bucket=$('query-bucket').value;if(Number(bucket)>0)q.bucket_width_us=(BigInt(bucket)*1000000n).toString();
  const field=$('filter-field').value;if(field){const op=$('filter-op').value,f={field,op};if(!op.startsWith('is_')){const type=state.table.fields.find(x=>x.name===field).type,value=$('filter-value').value;if(type==='bool'){if(!['true','false'].includes(value))throw new Error('Boolean filter must be true or false.');f.value=value==='true';}else if(type==='f32'||type==='f64'){f.value=Number(value);if(!value||!Number.isFinite(f.value))throw new Error('Enter a finite numeric filter value.');}else f.value=value;}q.filters=[f];}
  if(!agg&&!groups.length&&!q.bucket_width_us)q.cursor=true;
  return q;
}
function cursorButtons(){for(const id of ['next-page','close-cursor'])$(id).disabled=!state.cursor;}
async function releaseCursor(){const c=state.cursor;state.cursor=null;cursorButtons();if(c)try{await api(`/cursors/${c.id}/close`,{boot_id:c.boot});}catch(e){if(e.status!==410)throw e;}}
function renderResult(result){state.result=result;state.cursor=result.cursor_id?{id:result.cursor_id,boot:result.boot_id}:null;cursorButtons();const columns=result.columns.map(c=>c.name);const rows=result.rows.map((r,i)=>result.sequences?[result.sequences[i],...r]:r);if(result.sequences)columns.unshift('sequence');dataTable($('query-results'),columns,rows);
  const s=result.stats;$('query-stats').textContent=`${result.rows.length.toLocaleString()} output rows · ${count(result.matched_rows)} matched · snapshot ${count(s.snapshot_sequence)}\nScanned ${count(s.rows_scanned)} rows / ${bytes(s.bytes_scanned)} · ${count(s.frames_scanned)} frames / ${count(s.segments_scanned)} segments · index ${bytes(s.index_bytes)} · rebuilt ${bytes(s.rebuild_bytes)}`;
  $('query-status').textContent=state.cursor?'Snapshot held until its query TTL expires.':result.has_more?'Output limit reached. Narrow the query or use a scan cursor.':'Query complete';$('export-json').disabled=false;$('export-csv').disabled=false;
}
function download(content,type,name){const url=URL.createObjectURL(new Blob([content],{type})),a=node('a');a.href=url;a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);}
function csvCell(value){let s=value===null?'':String(value);if(/^[=+@\t\r]/.test(s)||(/^[-]/.test(s)&&!/^-[0-9]+(?:\.[0-9]+)?$/.test(s)))s="'"+s;return '"'+s.replaceAll('"','""')+'"';}
function renderLive(){const rows=state.liveRows.map(r=>[r.sequence,...r.values]);dataTable($('live-events'),['sequence',...state.liveColumns],rows);$('live-count').textContent=count(rows.length);$('live-skipped').textContent=count(state.liveSkipped);$('live-pause').textContent=state.livePaused?'Resume':'Pause';}
async function loadLive(){
  if(state.page!=='live'||state.livePaused||!state.liveDirty||state.liveBusy||!state.table||!state.authenticated)return;
  state.liveBusy=true;state.liveDirty=false;const id=state.table.id;
  try{const result=await api(`/tables/${id}/tail?limit=20`);if(state.table?.id!==id){state.liveDirty=true;return;}
    const fresh=result.rows.map((values,i)=>({sequence:result.sequences[i],values})).filter(r=>state.liveLast===null||BigInt(r.sequence)>state.liveLast);
    if(fresh.length){const first=BigInt(fresh[0].sequence);if(state.liveLast!==null&&first>state.liveLast+1n)state.liveSkipped+=first-state.liveLast-1n;state.liveLast=BigInt(fresh.at(-1).sequence);state.liveRows=[...fresh.reverse(),...state.liveRows].slice(0,100);state.liveColumns=result.columns.map(c=>c.name);renderLive();}
  }catch(e){state.liveDirty=true;throw e;}finally{state.liveBusy=false;}
}
async function loadJobs(){if(state.jobsBusy||!state.authenticated)return;state.jobsBusy=true;try{const result=await api('/jobs');const root=$('jobs-list');root.replaceChildren();root.classList.toggle('empty',!result.jobs.length);if(!result.jobs.length)root.textContent='No jobs yet.';
  for(const j of result.jobs.reverse()){const row=node('div',undefined,'job'),head=node('div',undefined,'job-header'),title=node('h3',j.kind==='generator'?j.table_name:`Integrity · table ${j.table_id}`),buttons=node('div',undefined,'actions');buttons.append(node('span',j.state,'pill'+(j.state==='failed'?' bad':'')));if(['starting','running'].includes(j.state)){const stop=node('button','Stop','secondary');stop.addEventListener('click',action(async()=>{await api(`/jobs/${j.id}/stop`,{});await loadJobs();}));buttons.append(stop);}head.append(title,buttons);row.append(head);const meta=node('div',undefined,'job-meta');const elapsed=Number(j.elapsed_ms)/1000;const items=j.kind==='generator'?[['Committed',count(j.committed_rows)],['Achieved / offered',`${elapsed?(Number(j.committed_rows)/elapsed).toFixed(0):'0'} / ${count(j.rows_per_second)} rows/s`],['Elapsed / CPU',`${elapsed.toFixed(1)}s / ${(Number(j.cpu_ns)/1e9).toFixed(2)}s`],['Attempted / rejected batches',`${count(j.attempted_rows)} / ${count(j.rejected_batches)}`]]:[['Rows verified',count(j.integrity.rows)],['Source bytes',bytes(j.integrity.bytes)],['Frames / segments',`${count(j.integrity.frames)} / ${count(j.integrity.segments)}`],['Elapsed / CPU',`${elapsed.toFixed(1)}s / ${(Number(j.cpu_ns)/1e9).toFixed(2)}s`]];for(const [label,value]of items){const item=node('div');item.append(node('span',label),node('strong',value));meta.append(item);}row.append(meta);if(j.error)row.append(node('p',j.error.message,'job-error'));root.append(row);}
  }finally{state.jobsBusy=false;}
}
async function loadSegments(reset=false){if(!state.table)return;const id=state.table.id;if(reset)state.segmentsAfter='0';const result=await api(`/tables/${id}/segments?after_id=${state.segmentsAfter}&limit=100`);if(state.table?.id!==id)return;dataTable($('segments-list'),['Segment','State','Sequence range','Ingestion start','Ingestion end','Source bytes'],result.segments.map(s=>[s.id,s.state,`${s.first_sequence}–${s.last_sequence}`,time(s.min_time),time(s.max_time),bytes(s.bytes)]));state.segmentsLast=result.segments.at(-1)?.id||state.segmentsAfter;state.segmentsMore=result.has_more;$('segments-next').disabled=!result.has_more;}
function retentionSummary(r){return `Cutoff: ${time(r.cutoff)}\nEligible: ${count(r.eligible_segments)} segments / ${count(r.eligible_rows)} rows / ${bytes(r.eligible_bytes)}\nPartially expired segments retained: ${count(r.partially_expired_segments)}\nPending deletion: ${count(r.pending_segments)}`;}

for(const button of document.querySelectorAll('[data-page]'))button.addEventListener('click',()=>showPage(button.dataset.page));for(const button of document.querySelectorAll('[data-go]'))button.addEventListener('click',()=>showPage(button.dataset.go));
$('login-dialog').addEventListener('cancel',e=>e.preventDefault());
$('login-form').addEventListener('submit',async event=>{event.preventDefault();event.submitter.disabled=true;$('login-error').textContent='';try{const result=await api('/session',{token:$('admin-token').value},{login:true});$('admin-token').value='';state.csrf=result.csrf;await startSession();}catch(e){$('login-error').textContent=e.message;}finally{event.submitter.disabled=false;}});
$('logout').addEventListener('click',action(async()=>{await api('/logout',{});loginNeeded();}));
$('refresh').addEventListener('click',action(async()=>{await refreshTables();renderStats(await api('/stats'));if(state.page==='storage')await loadSegments(true);report('Database metadata refreshed.');}));
$('table-select').addEventListener('change',action(()=>selectTable($('table-select').value)));
$('add-field').addEventListener('click',()=>addField());addField('site_id','u32');addField('path','symbol32');
$('create-table').addEventListener('submit',action(async event=>{const form=new FormData(event.target),fields=[];for(const row of $('schema-fields').children){const get=part=>row.querySelector(`[data-part="${part}"]`);const f={name:get('name').value,type:get('type').value,nullable:get('nullable').checked};if(['bytes','text'].includes(f.type))f.size=Number(get('size').value);fields.push(f);}const result=await api('/tables',{name:form.get('name'),fields,retention_us:(BigInt(form.get('retention')||0)*86400000000n).toString()});await refreshTables();await selectTable(result.id);report(`Created ${result.name} with ${result.row_width}-byte rows.`);}));
$('append-form').addEventListener('submit',action(async()=>{const rows=JSON.parse($('append-rows').value),id=selected();const r=await api(`/tables/${id}/append`,{rows});report(`Durably committed ${r.row_count} rows, sequences ${r.first_sequence}–${r.last_sequence}.`);state.liveDirty=true;await refreshTables();}));
$('query-form').addEventListener('submit',action(async()=>{const id=selected(),query=queryOptions();await releaseCursor();$('query-status').textContent='Reading snapshot…';const result=await api(`/tables/${id}/query`,query);if(state.table?.id===id)renderResult(result);else if(result.cursor_id)await api(`/cursors/${result.cursor_id}/close`,{boot_id:result.boot_id});}));
$('next-page').addEventListener('click',action(async()=>{const c=state.cursor;if(!c)return;try{renderResult(await api(`/cursors/${c.id}/next`,{boot_id:c.boot}));}catch(e){if(e.status===410){state.cursor=null;cursorButtons();}throw e;}}));
$('close-cursor').addEventListener('click',action(async()=>{await releaseCursor();$('query-status').textContent='Snapshot released.';}));
$('export-json').addEventListener('click',()=>download(JSON.stringify(state.result,null,2),'application/json','query-result.json'));
$('export-csv').addEventListener('click',()=>{const r=state.result,columns=r.columns.map(c=>c.name),rows=r.rows.map((row,i)=>r.sequences?[r.sequences[i],...row]:row);if(r.sequences)columns.unshift('sequence');download([columns,...rows].map(row=>row.map(csvCell).join(',')).join('\r\n'),'text/csv;charset=utf-8','query-result.csv');});
$('live-pause').addEventListener('click',()=>{state.livePaused=!state.livePaused;state.liveDirty=true;renderLive();loadLive().catch(e=>report(e.message,true));});
$('workload-form').addEventListener('submit',action(async event=>{const f=new FormData(event.target),options={};for(const k of ['rows_per_second','batch_rows','cardinality'])options[k]=Number(f.get(k));options.duration_ms=Number(f.get('seconds'))*1000;options.seed=f.get('seed');if(f.get('table_name'))options.table_name=f.get('table_name');const r=await api('/test-jobs',options);report(`Test job ${r.job_id} started in a dedicated table.`);await loadJobs();await refreshTables();}));
$('jobs-refresh').addEventListener('click',action(loadJobs));
$('integrity').addEventListener('click',action(async()=>{const r=await api(`/tables/${selected()}/integrity-check`,{});report(`Integrity job ${r.job_id} started. Results are in Test workload → Recent jobs.`);await loadJobs();}));
$('segments-reset').addEventListener('click',action(()=>loadSegments(true)));$('segments-next').addEventListener('click',action(async()=>{state.segmentsAfter=state.segmentsLast;await loadSegments();}));
$('retention-form').addEventListener('submit',action(async()=>{const days=$('retention-days').value;const us=(BigInt(days)*86400000000n).toString();await api(`/tables/${selected()}/retention`,{retention_us:us});state.preview=null;$('retention-apply').hidden=true;$('retention-summary').textContent='';report('Retention policy saved. Preview expiry before applying it.');await refreshTables();}));
$('retention-preview').addEventListener('click',action(async()=>{const id=selected(),now=(BigInt(Date.now())*1000n).toString();const result=await api(`/tables/${id}/retention-preview`,{now_us:now});state.preview={id,now,retention:state.table.retention_us};$('retention-summary').textContent=retentionSummary(result);$('retention-apply').hidden=BigInt(result.eligible_segments)===0n;}));
$('retention-apply').addEventListener('click',action(async()=>{const p=state.preview;if(!p||p.id!==selected())throw new Error('Preview expiry again for the selected table.');const current=await api(`/tables/${p.id}`);if(current.retention_us!==p.retention)throw new Error('The retention policy changed. Refresh and preview again.');if(!confirm('Permanently retire the fully expired segments shown in this preview? This cannot be undone.'))return;const result=await api(`/tables/${p.id}/retention`,{now_us:p.now,apply:true});$('retention-summary').textContent=retentionSummary(result);state.preview=null;$('retention-apply').hidden=true;report(`Retention applied. ${result.deleted_segments} segments deleted; readers may defer remaining deletion.`);await refreshTables();await loadSegments(true);}));
setInterval(()=>{loadLive().catch(e=>report(e.message,true));},250);
setInterval(()=>{if(state.authenticated&&!document.hidden&&state.page==='workload')loadJobs().catch(e=>report(e.message,true));},1000);
setInterval(()=>{if(state.authenticated&&!document.hidden)refreshTables().catch(e=>report(e.message,true));},5000);
(async()=>{try{const session=await api('/session');state.csrf=session.csrf;await startSession();}catch(e){loginNeeded();if(e.status!==401)$('login-error').textContent=e.message;}})();
