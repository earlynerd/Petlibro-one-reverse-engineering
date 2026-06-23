#pragma once

// ===========================================================================
//  Phase 0 harness dashboard — a deliberately dumb bench instrument.
// ===========================================================================
//  Vanilla JS, no build step, no external assets (the device can't reach a CDN
//  and a strict offline appliance shouldn't anyway). It:
//    * polls /api/state (1 Hz) and shows the live snapshot,
//    * fetches /api/commands and AUTO-RENDERS a control per registered command
//      (so new drivers' controls appear here with zero edits to this file),
//    * tails /api/events.
//  Phase 4 refines this same page into the analytics + config dashboard.

static const char DASHBOARD_HTML[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Feeder bench</title>
<style>
 :root{--bg:#0f1216;--card:#1a1f27;--ink:#e6edf3;--mut:#8b97a7;--acc:#4cc2ff;--ok:#3fb950;--err:#f85149}
 *{box-sizing:border-box} body{margin:0;font:14px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--ink)}
 header{padding:12px 16px;background:var(--card);display:flex;gap:16px;align-items:center;flex-wrap:wrap}
 header b{font-size:15px} .dot{width:9px;height:9px;border-radius:50%;background:var(--err);display:inline-block;margin-right:6px}
 .dot.up{background:var(--ok)} .mut{color:var(--mut)}
 main{display:grid;grid-template-columns:1fr 1fr;gap:14px;padding:16px;max-width:1100px}
 @media(max-width:760px){main{grid-template-columns:1fr}}
 .card{background:var(--card);border-radius:10px;padding:14px} .card h2{margin:0 0 10px;font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--mut)}
 pre{margin:0;white-space:pre-wrap;word-break:break-word;font:12px/1.4 ui-monospace,monospace;color:var(--ink)}
 .cmd{display:flex;gap:6px;align-items:center;margin:6px 0;flex-wrap:wrap}
 .cmd button{background:var(--acc);color:#001;border:0;border-radius:6px;padding:6px 12px;font-weight:600;cursor:pointer}
 .cmd input{background:#0f1216;border:1px solid #2b3340;color:var(--ink);border-radius:6px;padding:5px 7px;width:90px}
 .cmd .nm{font-family:ui-monospace,monospace;min-width:130px} .cmd .hp{color:var(--mut);font-size:12px}
 #log{max-height:260px;overflow:auto;font:12px/1.4 ui-monospace,monospace}
 #log div{padding:2px 0;border-bottom:1px solid #222} #res{color:var(--mut);min-height:16px;margin-top:8px}
</style></head><body>
<header>
 <b>🐾 Feeder bench</b>
 <span><i class="dot" id="dot"></i><span id="conn" class="mut">connecting…</span></span>
 <span class="mut">uptime <span id="up">–</span></span>
 <span class="mut">ip <span id="ip">–</span></span>
</header>
<main>
 <section class="card"><h2>State</h2><pre id="state">…</pre></section>
 <section class="card"><h2>Controls</h2><div id="cmds"></div><div id="res"></div></section>
 <section class="card" style="grid-column:1/-1"><h2>Event log</h2><div id="log"></div></section>
</main>
<script>
const $=s=>document.querySelector(s);
async function j(u,o){const r=await fetch(u,o);if(!r.ok)throw new Error(r.status);return r.json();}
function setConn(ok){$('#dot').classList.toggle('up',ok);$('#conn').textContent=ok?'connected':'no response';}

async function loadCmds(){
 try{const cmds=await j('/api/commands');const box=$('#cmds');box.innerHTML='';
  cmds.forEach(c=>{
   const row=document.createElement('div');row.className='cmd';
   const args=(c.args||'').split(',').filter(Boolean);
   let inputs='';args.forEach(a=>{const[nm,ty]=a.split(':');inputs+=`<input data-k="${nm}" placeholder="${nm}${ty?':'+ty:''}">`;});
   row.innerHTML=`<span class="nm">${c.name}</span>${inputs}<button>run</button><span class="hp">${c.help||''}</span>`;
   row.querySelector('button').onclick=()=>run(c.name,row);
   box.appendChild(row);
  });
 }catch(e){$('#cmds').textContent='failed to load commands';}
}
async function run(name,row){
 const p=new URLSearchParams({cmd:name});
 row.querySelectorAll('input').forEach(i=>{if(i.value!=='')p.set(i.dataset.k,i.value);});
 try{const r=await j('/api/cmd?'+p.toString(),{method:'POST'});$('#res').textContent=name+' → '+JSON.stringify(r);}
 catch(e){$('#res').textContent=name+' → error '+e.message;}
 pollEvents();
}
async function pollState(){
 try{const s=await j('/api/state');setConn(true);
  $('#state').textContent=JSON.stringify(s,null,2);
  if(s.sys){$('#up').textContent=Math.floor((s.sys.uptime_ms||0)/1000)+'s';$('#ip').textContent=s.sys.ip||'–';}
 }catch(e){setConn(false);}
}
let lastSeq=0;
async function pollEvents(){
 try{const evs=await j('/api/events?since='+lastSeq);const log=$('#log');
  evs.forEach(e=>{lastSeq=Math.max(lastSeq,e.seq);const d=document.createElement('div');
   d.innerHTML=`<span class="mut">#${e.seq} +${e.ts}ms</span> <b>${e.type}</b> ${e.detail||''}`;log.prepend(d);});
 }catch(e){}
}
loadCmds();pollState();pollEvents();
setInterval(pollState,1000);setInterval(pollEvents,2000);
</script></body></html>)HTML";
