#pragma once

// ===========================================================================
//  Feeder control panel  —  Phase 4: owner UI on top of the Phase-0 APIs.
// ===========================================================================
//  Self-contained, served straight from flash: no build step, no CDN, no
//  external assets (a strict offline appliance can't reach one and shouldn't
//  want to). Vanilla JS + inline CSS, hand-rolled — same four endpoints the
//  bench harness always used:
//      GET /api/state            live snapshot (registry state contributors)
//      GET /api/commands         command manifest
//      GET /api/cmd?cmd=NAME...   dispatch a command
//      GET /api/events?since=N    event-log tail
//
//  Structure: a tabbed app. HOME is the polished daily-driver (feed now + live
//  status). BENCH preserves the original auto-render harness verbatim (raw
//  state + a control per registered command + the event tail) so nothing is
//  lost for bring-up. Schedule / History / Access are roadmap placeholders that
//  drive the same APIs once built out (feeder.sched.*, the meal/visit event
//  stream, acl.*).

static const char DASHBOARD_HTML[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#17140f">
<title>Feeder</title>
<style>
 :root{
   --bg:#17140f; --bg2:#1e1913; --card:#241e16; --card2:#2c251b; --line:#3a3127;
   --ink:#f5efe4; --ink2:#d9cdb9; --mut:#a89a85; --mut2:#7d715f;
   --honey:#ffb454; --honey-d:#e0921f; --mint:#5fd0a0; --sky:#74b6ff;
   --amber:#ffcf6b; --rose:#ff7d7d;
   --r:16px; --rs:10px; --sh:0 1px 2px rgba(0,0,0,.4),0 8px 24px rgba(0,0,0,.28);
   --ff:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
   --fm:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
 }
 *{box-sizing:border-box} html,body{margin:0}
 body{font:15px/1.5 var(--ff);background:
   radial-gradient(1200px 600px at 85% -10%,#2a2114 0,transparent 60%),
   radial-gradient(900px 500px at -10% 110%,#221c2a 0,transparent 55%),var(--bg);
   background-attachment:fixed;color:var(--ink);-webkit-font-smoothing:antialiased;
   padding-bottom:env(safe-area-inset-bottom)}
 a{color:var(--honey)}
 /* ---- header / nav ---- */
 header{position:sticky;top:0;z-index:20;backdrop-filter:blur(10px);
   background:rgba(23,20,15,.82);border-bottom:1px solid var(--line)}
 .bar{display:flex;align-items:center;gap:14px;padding:11px 16px;max-width:980px;margin:0 auto;flex-wrap:wrap}
 .brand{display:flex;align-items:center;gap:9px;font-weight:700;font-size:17px;letter-spacing:.2px}
 .brand .paw{font-size:20px;filter:saturate(1.1)}
 nav{display:flex;gap:4px;flex:1;flex-wrap:wrap}
 nav button{appearance:none;border:0;background:transparent;color:var(--mut);font:600 13px var(--ff);
   padding:7px 12px;border-radius:999px;cursor:pointer;transition:.15s;white-space:nowrap}
 nav button:hover{color:var(--ink2);background:#ffffff0d}
 nav button.on{color:#241a08;background:var(--honey);box-shadow:0 2px 10px #ffb45433}
 nav .sep{flex:1}
 .conn{display:flex;align-items:center;gap:7px;color:var(--mut);font-size:12.5px;white-space:nowrap}
 .led{width:9px;height:9px;border-radius:50%;background:var(--rose);box-shadow:0 0 0 0 #ff7d7d66;transition:.2s}
 .led.up{background:var(--mint);animation:pulse 2.4s infinite}
 @keyframes pulse{0%{box-shadow:0 0 0 0 #5fd0a055}70%{box-shadow:0 0 0 7px #5fd0a000}100%{box-shadow:0 0 0 0 #5fd0a000}}
 /* ---- status strip ---- */
 .strip{display:flex;gap:8px;overflow-x:auto;padding:10px 16px 0;max-width:980px;margin:0 auto;scrollbar-width:none}
 .strip::-webkit-scrollbar{display:none}
 .chip{display:flex;align-items:center;gap:8px;background:var(--card);border:1px solid var(--line);
   border-radius:999px;padding:6px 12px;font-size:13px;white-space:nowrap;color:var(--ink2)}
 .chip b{color:var(--ink);font-weight:600} .chip .k{color:var(--mut);font-size:11.5px;text-transform:uppercase;letter-spacing:.05em}
 .batt{width:26px;height:12px;border:1.5px solid var(--mut);border-radius:3px;position:relative;padding:1.5px}
 .batt:after{content:"";position:absolute;right:-4px;top:3px;width:2.5px;height:4px;background:var(--mut);border-radius:0 2px 2px 0}
 .batt i{display:block;height:100%;background:var(--mint);border-radius:1px;transition:width .4s}
 /* ---- layout ---- */
 main{max-width:980px;margin:0 auto;padding:16px}
 .tab{display:none} .tab.on{display:block;animation:fade .25s ease}
 @keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
 .grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}
 @media(max-width:680px){.grid{grid-template-columns:1fr}}
 .card{background:linear-gradient(180deg,var(--card2),var(--card));border:1px solid var(--line);
   border-radius:var(--r);padding:18px;box-shadow:var(--sh)}
 .card h2,.card h3{margin:0 0 12px;font-size:12px;font-weight:700;text-transform:uppercase;
   letter-spacing:.07em;color:var(--mut)}
 .span{grid-column:1/-1}
 /* ---- feed hero ---- */
 .hero{display:flex;gap:22px;align-items:center;flex-wrap:wrap}
 .feed-btn{appearance:none;border:0;cursor:pointer;color:#2a1c05;font:800 18px var(--ff);letter-spacing:.3px;
   width:132px;height:132px;border-radius:50%;flex:none;
   background:radial-gradient(120% 120% at 30% 25%,#ffd089,var(--honey) 45%,var(--honey-d));
   box-shadow:0 10px 30px #e0921f44,inset 0 2px 6px #fff6,inset 0 -8px 16px #b96f0033;
   transition:transform .12s,box-shadow .2s}
 .feed-btn:hover{transform:translateY(-2px)}
 .feed-btn:active{transform:translateY(1px) scale(.98)}
 .feed-btn:disabled{filter:grayscale(.5) brightness(.7);cursor:default;transform:none}
 .feed-btn small{display:block;font-weight:600;font-size:12px;opacity:.8;margin-top:2px}
 .hero-ctl{flex:1;min-width:220px}
 .stepper{display:flex;align-items:center;gap:14px}
 .step{appearance:none;width:42px;height:42px;border-radius:12px;border:1px solid var(--line);
   background:var(--card2);color:var(--ink);font-size:24px;font-weight:600;cursor:pointer;line-height:1}
 .step:hover{border-color:var(--honey);color:var(--honey)}
 .step:disabled{opacity:.35;cursor:default}
 .step-val{text-align:center;min-width:64px}
 .step-val b{display:block;font-size:30px;font-weight:800;line-height:1}
 .step-val small{color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.05em}
 .cup{margin-top:12px;color:var(--ink2);font-size:14px} .cup b{color:var(--honey)}
 .hint{margin-top:8px;color:var(--mut);font-size:12.5px}
 /* feeding state */
 .feeding{flex:1;min-width:220px}
 .feeding .lbl{display:flex;align-items:center;gap:10px;font-weight:600;color:var(--ink)}
 .spin{width:18px;height:18px;border-radius:50%;border:2.5px solid #ffffff22;border-top-color:var(--honey);
   animation:spin .8s linear infinite;flex:none}
 @keyframes spin{to{transform:rotate(360deg)}}
 .prog{height:10px;border-radius:999px;background:#ffffff10;overflow:hidden;margin:12px 0}
 .prog i{display:block;height:100%;background:linear-gradient(90deg,var(--honey),#ffd089);transition:width .5s}
 /* ---- stat tiles ---- */
 .big{font-size:30px;font-weight:800;line-height:1.1} .lead{font-size:18px;font-weight:700}
 .sub{color:var(--mut);font-size:13px;margin-top:4px}
 .pill{display:inline-flex;align-items:center;gap:7px;padding:5px 11px;border-radius:999px;font-size:13px;font-weight:600}
 .pill.ok{background:#5fd0a01a;color:var(--mint)} .pill.warn{background:#ffcf6b1a;color:var(--amber)}
 .pill.bad{background:#ff7d7d1a;color:var(--rose)} .pill.idle{background:#ffffff0d;color:var(--mut)}
 .pill .d{width:7px;height:7px;border-radius:50%;background:currentColor}
 .rows{display:flex;flex-direction:column;gap:9px}
 .row{display:flex;justify-content:space-between;align-items:center;gap:12px}
 .row .k{color:var(--mut);font-size:13px} .row .v{font-weight:600;text-align:right}
 .row .v small{color:var(--mut);font-weight:400}
 /* toggle */
 .tg{position:relative;display:inline-block;width:44px;height:25px;flex:none;vertical-align:middle}
 .tg input{position:absolute;inset:0;opacity:0;cursor:pointer;margin:0}
 .tg .tr{position:absolute;inset:0;background:#ffffff14;border-radius:999px;transition:.2s}
 .tg .tr:before{content:"";position:absolute;width:19px;height:19px;left:3px;top:3px;border-radius:50%;
   background:var(--ink2);transition:.2s}
 .tg input:checked+.tr{background:var(--mint)} .tg input:checked+.tr:before{transform:translateX(19px);background:#fff}
 /* meal result face */
 .face{display:flex;align-items:center;gap:14px}
 .glyph{width:54px;height:54px;border-radius:14px;display:grid;place-items:center;font-size:26px;flex:none}
 .glyph.ok{background:#5fd0a01a;color:var(--mint)} .glyph.bad{background:#ff7d7d1a;color:var(--rose)}
 .glyph.warn{background:#ffcf6b1a;color:var(--amber)} .glyph.idle{background:#ffffff0d;color:var(--mut)}
 /* alert banner */
 .alert{display:flex;align-items:center;gap:12px;border-radius:var(--r);padding:13px 16px;margin-bottom:14px;
   font-weight:600;border:1px solid;animation:fade .25s}
 .alert .ic{font-size:20px}
 .alert.p3{background:#ff7d7d14;border-color:#ff7d7d44;color:var(--rose)}
 .alert.p2{background:#ffcf6b14;border-color:#ffcf6b44;color:var(--amber)}
 .alert.p1{background:#74b6ff14;border-color:#74b6ff44;color:var(--sky)}
 .alert small{font-weight:400;color:var(--mut)}
 /* placeholder */
 .soon{text-align:center;padding:54px 20px;color:var(--mut)}
 .soon .ic{font-size:42px;opacity:.5} .soon h3{color:var(--ink);font-size:18px;margin:14px 0 6px;text-transform:none;letter-spacing:0}
 .soon p{max-width:420px;margin:0 auto;font-size:13.5px}
 .soon code{background:#ffffff0d;padding:2px 6px;border-radius:6px;font:12px var(--fm);color:var(--ink2)}
 /* ---- bench ---- */
 .bench pre{margin:0;white-space:pre-wrap;word-break:break-word;font:12px/1.45 var(--fm);color:var(--ink2);
   max-height:75vh;overflow:auto}
 .cmd{display:flex;gap:6px;align-items:center;margin:7px 0;flex-wrap:wrap}
 .cmd .nm{font:600 12.5px var(--fm);min-width:148px;color:var(--ink)}
 .cmd input{background:var(--bg);border:1px solid var(--line);color:var(--ink);border-radius:8px;padding:6px 8px;width:84px;font:12px var(--fm)}
 .cmd input:focus{outline:0;border-color:var(--honey)}
 .cmd .go{background:var(--honey);color:#2a1c05;border:0;border-radius:8px;padding:6px 13px;font-weight:700;cursor:pointer}
 .cmd .hp{color:var(--mut);font-size:12px}
 #res{color:var(--ink2);min-height:18px;margin-top:10px;font:12px/1.4 var(--fm);word-break:break-word}
 #log{max-height:300px;overflow:auto;font:12px/1.5 var(--fm)}
 #log div{padding:3px 0;border-bottom:1px solid #ffffff0a}
 #log .s{color:var(--mut2)} #log .t{color:var(--honey);font-weight:600}
 .ghost{appearance:none;background:#ffffff0d;border:1px solid var(--line);color:var(--ink2);border-radius:10px;
   padding:9px 16px;font:600 14px var(--ff);cursor:pointer}
 .ghost:hover{border-color:var(--mut)} .ghost.danger{color:var(--rose);border-color:#ff7d7d44}
 .muted{color:var(--mut)} .mt{margin-top:14px}
 /* ---- owner controls / lists / chart ---- */
 .btn{appearance:none;border:0;border-radius:10px;padding:9px 15px;font:600 14px var(--ff);cursor:pointer;
   background:var(--honey);color:#2a1c05;transition:.15s}
 .btn:hover{filter:brightness(1.05)} .btn:active{transform:translateY(1px)}
 .btn:disabled{opacity:.5;cursor:default;filter:none;transform:none}
 .btn.sm{padding:7px 12px;font-size:13px}
 .btn.sec{background:#ffffff0d;color:var(--ink2);border:1px solid var(--line)}
 .btn.sec:hover{border-color:var(--mut);filter:none}
 .btn.danger{background:transparent;color:var(--rose);border:1px solid #ff7d7d44}
 .btn.danger:hover{background:#ff7d7d14;filter:none}
 .btnrow{display:flex;gap:8px;flex-wrap:wrap}
 .in{background:var(--bg);border:1px solid var(--line);color:var(--ink);border-radius:10px;padding:9px 11px;font:14px var(--ff)}
 .in:focus{outline:0;border-color:var(--honey)} .in.sm{padding:7px 9px} .in.mono{font-family:var(--fm)}
 .field{display:flex;gap:9px;align-items:center;flex-wrap:wrap;margin-top:14px}
 .field label{color:var(--mut);font-size:13px}
 .li{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:11px 1px;border-bottom:1px solid #ffffff0a}
 .li:last-child{border-bottom:0} .li-main{display:flex;align-items:center;gap:10px}
 .li-main b{font-variant-numeric:tabular-nums}
 .x{appearance:none;background:transparent;border:1px solid var(--line);color:var(--mut);border-radius:8px;
   padding:5px 10px;font-size:12.5px;cursor:pointer} .x:hover{border-color:var(--rose);color:var(--rose)}
 .empty{color:var(--mut);text-align:center;padding:26px 0;font-size:13.5px}
 .chart{display:flex;align-items:flex-end;gap:6px;height:170px;margin-top:8px}
 .bar{flex:1;height:100%;display:flex;flex-direction:column;align-items:center;justify-content:flex-end;min-width:0}
 .bar .col{flex:1;width:100%;display:flex;flex-direction:column;justify-content:flex-end;align-items:center}
 .bar i{width:62%;max-width:34px;min-height:3px;background:linear-gradient(180deg,#ffd089,var(--honey-d));
   border-radius:6px 6px 0 0;transition:height .45s}
 .bar.z i{background:#ffffff12}
 .bar .bv{font-size:10.5px;color:var(--ink2);margin-bottom:4px;height:13px;font-variant-numeric:tabular-nums}
 .bar .bl{font-size:11px;color:var(--mut);margin-top:7px;text-align:center;line-height:1.2}
 .kpis{display:flex;gap:26px;margin-bottom:14px;flex-wrap:wrap}
 .kpi b{font-size:25px;font-weight:800;display:block;line-height:1.1} .kpi span{color:var(--mut);font-size:12px}
 .toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(20px);background:var(--card2);
   border:1px solid var(--line);color:var(--ink);padding:11px 16px;border-radius:12px;box-shadow:var(--sh);
   font-size:13.5px;opacity:0;pointer-events:none;transition:.25s;z-index:50;max-width:90vw;text-align:center}
 .toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
</style></head><body>

<header>
 <div class="bar">
  <span class="brand"><span class="paw">🐾</span>Feeder</span>
  <nav id="nav">
   <button data-t="home" class="on">Home</button>
   <button data-t="schedule">Schedule</button>
   <button data-t="history">History</button>
   <button data-t="access">Access</button>
   <span class="sep"></span>
   <button data-t="bench">⚙ Bench</button>
  </nav>
  <span class="conn"><i class="led" id="led"></i><span id="connTxt">connecting…</span></span>
 </div>
 <div class="strip" id="strip"></div>
</header>

<main>
 <!-- ================= HOME ================= -->
 <section class="tab on" data-t="home">
  <div id="banner"></div>
  <div class="grid">
   <div class="card span hero">
    <div id="feedMain" style="display:flex;gap:22px;align-items:center;flex-wrap:wrap;flex:1">
     <button class="feed-btn" id="feedBtn">Feed Now<small id="feedBtnSub">1 portion</small></button>
     <div class="hero-ctl">
      <h3 style="margin-bottom:10px">Portion</h3>
      <div class="stepper">
       <button class="step" id="dec">−</button>
       <div class="step-val"><b id="pVal">1</b><small>portions</small></div>
       <button class="step" id="inc">+</button>
      </div>
      <div class="cup" id="cupEq">≈ <b>1/12</b> cup</div>
      <div class="hint">Chute-confirmed delivery — food only counts when it falls.</div>
     </div>
    </div>
    <div class="feeding" id="feedingBox" hidden>
     <div class="lbl"><span class="spin"></span>Dispensing…</div>
     <div class="prog"><i id="progFill" style="width:0"></i></div>
     <div class="sub" id="dispTxt">delivering</div>
     <button class="ghost danger mt" id="stopBtn">Stop</button>
    </div>
   </div>

   <div class="card">
    <h2>Last meal</h2>
    <div class="face">
     <div class="glyph idle" id="mealGlyph">–</div>
     <div>
      <div class="lead" id="mealResult">No meals yet</div>
      <div class="sub" id="mealSub">manual or scheduled feeds appear here</div>
     </div>
    </div>
   </div>

   <div class="card">
    <h2>Hopper &amp; supply</h2>
    <div class="rows">
     <div class="row"><span class="k">Food level</span><span class="v" id="hopperPill"><span class="pill idle"><i class="d"></i>unknown</span></span></div>
     <div class="row"><span class="k">Chute</span><span class="v" id="chutePill">–</span></div>
     <div class="row"><span class="k">Battery</span><span class="v" id="battTxt">–</span></div>
    </div>
   </div>

   <div class="card">
    <h2>Lid</h2>
    <div class="row" style="margin-bottom:14px"><span class="k">Position</span><span class="v" id="lidPos">–</span></div>
    <div class="btnrow">
     <button class="btn sec" id="lidOpen">⬆ Open</button>
     <button class="btn sec" id="lidClose">⬇ Close</button>
     <button class="btn danger" id="lidStop">Stop</button>
    </div>
    <div class="hint mt" id="lidAclNote" hidden>Access control is managing the lid — disable it on the Access tab for manual control.</div>
   </div>

   <div class="card">
    <h2>Auto schedule</h2>
    <div class="rows">
     <div class="row"><span class="k">Scheduled feeding</span>
      <span class="v"><label class="tg"><input type="checkbox" id="autoTg"><span class="tr"></span></label></span></div>
     <div class="row"><span class="k">Daily slots</span><span class="v" id="slotsTxt">–</span></div>
     <div class="row" id="clockWarnRow" hidden><span class="k">Clock</span><span class="v"><span class="pill warn"><i class="d"></i>not synced</span></span></div>
    </div>
    <div class="hint mt">Manage feeding times on the <a href="#" data-goto="schedule">Schedule</a> tab.</div>
   </div>

   <div class="card span">
    <h2>Device</h2>
    <div class="grid" style="gap:9px 22px">
     <div class="row"><span class="k">Wi-Fi</span><span class="v" id="wifiTxt">–</span></div>
     <div class="row"><span class="k">Address</span><span class="v" id="ipTxt">–</span></div>
     <div class="row"><span class="k">Clock</span><span class="v" id="clockTxt">–</span></div>
     <div class="row"><span class="k">Uptime</span><span class="v" id="upTxt">–</span></div>
     <div class="row"><span class="k">Timezone</span><span class="v" id="tzVal">–</span></div>
    </div>
    <button class="btn sec sm" id="tzBtn" style="margin-top:14px">Match this device</button>
   </div>
  </div>
 </section>

 <!-- ============== placeholders ============== -->
 <section class="tab" data-t="schedule">
  <div class="card">
   <div class="row" style="margin-bottom:8px"><h2 style="margin:0">Auto feeding</h2>
    <label class="tg"><input type="checkbox" id="schAuto"><span class="tr"></span></label></div>
   <div class="hint" id="schHint"></div>
  </div>
  <div class="card mt">
   <div class="row"><h2 style="margin:0">Feeding times</h2><span class="sub" id="schCount">–</span></div>
   <div id="schedList"><div class="empty">loading…</div></div>
   <div class="field">
    <input type="time" class="in" id="schTime" value="08:00">
    <input type="number" class="in sm" id="schPortions" min="1" max="24" value="1" style="width:72px">
    <span class="muted" style="font-size:13px">portions</span>
    <button class="btn" id="schAdd">Add time</button>
    <button class="btn sec sm" id="schClear" style="margin-left:auto">Clear all</button>
   </div>
  </div>
 </section>
 <section class="tab" data-t="history">
  <div class="card">
   <div class="kpis">
    <div class="kpi"><b id="kToday">–</b><span>cups today</span></div>
    <div class="kpi"><b id="kMeals">–</b><span>meals today</span></div>
    <div class="kpi"><b id="kWeek">–</b><span>cups · 7 days</span></div>
   </div>
   <h2>Cups dispensed · last 7 days</h2>
   <div class="chart" id="chart"></div>
  </div>
  <div class="card mt"><h2>Recent meals</h2><div id="mealList"><div class="empty">loading…</div></div></div>
 </section>
 <section class="tab" data-t="access">
  <div class="card">
   <div class="row" style="margin-bottom:6px"><h2 style="margin:0">RFID-gated lid</h2>
    <label class="tg"><input type="checkbox" id="aclEnable"><span class="tr"></span></label></div>
   <div class="hint">When on, the lid opens only for whitelisted collars and closes after the hold time. (Enabling also powers the RFID reader.)</div>
   <div class="field"><label>Hold open</label>
    <input type="number" class="in sm" id="aclHold" min="0" max="600" style="width:80px"><span class="muted" style="font-size:13px">seconds</span>
    <button class="btn sec sm" id="aclHoldSet">Set</button>
    <span class="sub" id="aclNow" style="margin-left:auto"></span>
   </div>
  </div>
  <div class="card mt">
   <div class="row"><h2 style="margin:0">Whitelisted collars</h2><button class="btn sec sm" id="aclClear">Clear all</button></div>
   <div id="tagList"><div class="empty">loading…</div></div>
   <div class="field">
    <input type="text" class="in mono" id="aclTagIn" placeholder="tag id (15 digits)" style="flex:1;min-width:150px">
    <button class="btn" id="aclAdd">Add</button>
    <button class="btn sec" id="aclAddCurrent">Add current collar</button>
   </div>
  </div>
  <div class="card mt"><h2>Recent visits</h2><div id="visitList"><div class="empty">loading…</div></div></div>
 </section>

 <!-- ================= BENCH ================= -->
 <section class="tab bench" data-t="bench">
  <div class="grid">
   <div class="card"><h2>Live state</h2><pre id="state">…</pre></div>
   <div class="card"><h2>Controls</h2><div id="cmds" class="muted">loading…</div><div id="res"></div></div>
   <div class="card span"><h2>Event log</h2><div id="log"></div></div>
  </div>
 </section>
</main>

<div id="toast" class="toast"></div>

<script>
"use strict";
const $=s=>document.querySelector(s), $$=s=>[...document.querySelectorAll(s)];
const T=(el,v)=>{if(el)el.textContent=v;};
let S={};            // last /api/state
let portions=1;      // feed-now selection
let online=false;

async function getJ(u,o){const r=await fetch(u,o);if(!r.ok)throw new Error(r.status);return r.json();}
function cmd(name,params){
 const p=new URLSearchParams({cmd:name});
 if(params)for(const k in params)if(params[k]!=='')p.set(k,params[k]);
 return getJ('/api/cmd?'+p.toString(),{method:'POST'});
}

/* ---- tabs ---- */
function show(t){
 $$('#nav button').forEach(b=>b.classList.toggle('on',b.dataset.t===t));
 $$('.tab').forEach(s=>s.classList.toggle('on',s.dataset.t===t));
 if(t==='bench'){loadCmds();pollEvents();}
 else if(t==='schedule')loadSchedule();
 else if(t==='history')loadHistory();
 else if(t==='access')loadAccess();
}
$('#nav').addEventListener('click',e=>{const b=e.target.closest('button');if(b)show(b.dataset.t);});
document.addEventListener('click',e=>{const g=e.target.closest('[data-goto]');if(g){e.preventDefault();show(g.dataset.goto);}});

/* ---- helpers ---- */
const gcd=(a,b)=>b?gcd(b,a%b):a;
function cupFrac(parcels){          // parcels -> reduced fraction of a cup (12 parcels = 1 cup)
 const per=(S.feeder&&S.feeder.parcels_per_cup)||12;
 if(parcels<=0)return '0';
 const d=gcd(parcels,per), n=parcels/d, den=per/d;
 const whole=Math.floor(n/den), rem=n%den;
 if(den===1)return String(n);
 return (whole?whole+' ':'')+(rem?rem+'/'+den:'')||'0';
}
function ago(ms){
 if(!ms||ms<0)return '';
 const s=Math.floor(ms/1000);
 if(s<60)return s+'s ago'; const m=Math.floor(s/60);
 if(m<60)return m+'m ago'; const h=Math.floor(m/60);
 if(h<24)return h+'h ago'; return Math.floor(h/24)+'d ago';
}
function hhmm(d){const m=d%(24*60);return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');}

/* ---- feed-now control ---- */
function ppp(){return (S.feeder&&S.feeder.parcels_per_portion)||1;}
function renderFeedCtl(){
 const parcels=portions*ppp();
 T($('#pVal'),portions);
 $('#feedBtnSub').textContent=portions+' portion'+(portions>1?'s':'');
 $('#cupEq').innerHTML='≈ <b>'+cupFrac(parcels)+'</b> cup';
 $('#dec').disabled=portions<=1;
}
$('#inc').onclick=()=>{if(portions<24){portions++;renderFeedCtl();}};
$('#dec').onclick=()=>{if(portions>1){portions--;renderFeedCtl();}};
$('#feedBtn').onclick=async()=>{
 const b=$('#feedBtn');b.disabled=true;
 try{await cmd('feeder.feed',{portions});}catch(e){flash('Feed failed: '+e.message);}
 setTimeout(()=>{b.disabled=false;poll();},300);
};
$('#stopBtn').onclick=async()=>{try{await cmd('feeder.stop');}catch(e){}poll();};
let toastT;
function flash(m){const el=$('#toast');el.textContent=m;el.classList.add('show');
 clearTimeout(toastT);toastT=setTimeout(()=>el.classList.remove('show'),2800);}

/* ---- render from state ---- */
function setConn(ok){online=ok;$('#led').classList.toggle('up',ok);T($('#connTxt'),ok?'online':'offline');}

function renderStrip(){
 const f=S.feeder||{},sn=S.sensors||{},ck=S.clock||{};
 const hop=f.hopper_low? '<span class="pill bad" style="padding:3px 9px"><i class="d"></i>low</span>'
                       : '<span class="pill ok" style="padding:3px 9px"><i class="d"></i>ok</span>';
 const battRaw=(sn.batt!=null)?sn.batt:null;
 const battPct=battRaw!=null?Math.max(0,Math.min(100,Math.round(battRaw/4095*100))):0;
 const battCol=battPct<20?'var(--rose)':battPct<45?'var(--amber)':'var(--mint)';
 let t='—';
 if(ck.local_iso){const m=String(ck.local_iso).match(/T(\d\d:\d\d)/);if(m)t=m[1];}
 else if(ck.synced===false)t='no clock';
 $('#strip').innerHTML=
  '<span class="chip"><span class="k">status</span><b style="color:'+(online?'var(--mint)':'var(--rose)')+'">'+(online?'Online':'Offline')+'</b></span>'+
  '<span class="chip" title="battery ADC '+(battRaw==null?'?':battRaw)+'/4095 (uncalibrated)"><span class="k">batt</span><span class="batt"><i style="width:'+battPct+'%;background:'+battCol+'"></i></span></span>'+
  '<span class="chip"><span class="k">hopper</span>'+hop+'</span>'+
  '<span class="chip"><span class="k">clock</span><b>'+t+'</b></span>'+
  '<span class="chip"><span class="k">auto</span><b style="color:'+(f.auto?'var(--mint)':'var(--mut)')+'">'+(f.auto?'on':'off')+'</b></span>';
}

function renderBanner(){
 const a=(S.alerts&&S.alerts.active)||[];
 if(!a.length){$('#banner').innerHTML='';return;}
 const top=a.slice().sort((x,y)=>(y.prio||0)-(x.prio||0))[0];
 const ic=top.prio>=3?'⛔':top.prio===2?'⚠️':'ℹ️';
 $('#banner').innerHTML='<div class="alert p'+(top.prio||1)+'"><span class="ic">'+ic+'</span>'+
   '<span>'+esc(top.msg||top.key)+'</span>'+(a.length>1?'<small>+'+(a.length-1)+' more</small>':'')+'</div>';
}

const MEAL={done:['ok','✓','Delivered'],jam:['bad','⚠','Jam'],timeout:['warn','⏱','Timed out'],
            aborted:['idle','■','Stopped'],none:['idle','–','No meals yet']};
function renderMeal(){
 const lm=(S.feeder&&S.feeder.last_meal)||{};
 const r=lm.result||'none', meta=MEAL[r]||MEAL.none;
 const g=$('#mealGlyph');g.className='glyph '+meta[0];g.textContent=meta[1];
 if(r==='none'){T($('#mealResult'),'No meals yet');T($('#mealSub'),'manual or scheduled feeds appear here');return;}
 const cups=lm.cups!=null?lm.cups:0;
 let line=meta[2]+' · '+cups+' cup';
 if(lm.delivered!=null&&lm.wanted!=null&&lm.delivered<lm.wanted)line+=' ('+lm.delivered+'/'+lm.wanted+' parcels)';
 T($('#mealResult'),line);
 T($('#mealSub'),(lm.portions||0)+' portion'+(lm.portions===1?'':'s')+' · '+(ago(lm.age_ms)||'just now'));
}

function renderHopper(){
 const f=S.feeder||{};
 const hp=$('#hopperPill');
 if(f.hopper_low===true)hp.innerHTML='<span class="pill bad"><i class="d"></i>Low — refill soon</span>';
 else if(f.hopper_low===false)hp.innerHTML='<span class="pill ok"><i class="d"></i>OK</span>';
 else hp.innerHTML='<span class="pill idle"><i class="d"></i>unknown</span>';
 const ch=$('#chutePill');
 if(f.chute_blocked===true)ch.innerHTML='<span class="pill warn"><i class="d"></i>blocked</span>';
 else if(f.chute_blocked===false)ch.innerHTML='<span class="pill ok"><i class="d"></i>clear</span>';
 else ch.textContent='–';
 const sn=S.sensors||{};
 $('#battTxt').innerHTML=sn.batt!=null?('<small>adc</small> '+sn.batt):'–';
}

function renderAuto(){
 const f=S.feeder||{};
 const tg=$('#autoTg');if(document.activeElement!==tg)tg.checked=!!f.auto;
 T($('#slotsTxt'),(f.n_slots!=null?f.n_slots:'–'));
 $('#clockWarnRow').hidden=!(f.auto&&f.time_known===false);
}
$('#autoTg').onchange=async e=>{try{await cmd('feeder.auto',{v:e.target.checked?1:0});}catch(err){}poll();};

function renderDevice(){
 const sy=S.sys||{},ck=S.clock||{};
 $('#wifiTxt').innerHTML=sy.wifi?(esc(sy.ssid||'')+' <small>'+(sy.rssi||0)+' dBm</small>'):'<span class="muted">down</span>';
 T($('#ipTxt'),sy.ip||'–');
 if(ck.local_iso){const iso=String(ck.local_iso).replace('T',' ');$('#clockTxt').innerHTML=esc(iso)+(ck.source?' <small>('+esc(ck.source)+')</small>':'');}
 else T($('#clockTxt'),ck.synced===false?'not synced':'–');
 const u=Math.floor((sy.uptime_ms||0)/1000);
 const d=Math.floor(u/86400),h=Math.floor(u%86400/3600),m=Math.floor(u%3600/60);
 T($('#upTxt'),(d?d+'d ':'')+(h?h+'h ':'')+m+'m');
 renderTz();
}

function renderFeeding(){
 const f=S.feeder||{},fd=S.feed||{};
 const on=!!f.feeding;
 $('#feedMain').style.display=on?'none':'flex';
 $('#feedingBox').hidden=!on;
 if(on){
  const tgt=fd.target||0, got=fd.parcels||0;
  const pct=tgt>0?Math.min(100,Math.round(got/tgt*100)):8;
  $('#progFill').style.width=pct+'%';
  T($('#dispTxt'),tgt>0?(got+' / '+tgt+' parcels'):'starting…');
 }
}

function renderHome(){renderBanner();renderMeal();renderHopper();renderLid();renderAuto();renderDevice();renderFeeding();renderFeedCtl();}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}

/* ---- polling ---- */
async function poll(){
 try{S=await getJ('/api/state');setConn(true);renderStrip();renderHome();ensureTz();
  if($('.tab.on').dataset.t==='bench')T($('#state'),JSON.stringify(S,null,2));
 }catch(e){setConn(false);}
}

/* ---- bench: auto-rendered command harness (preserved) ---- */
let cmdsLoaded=false;
async function loadCmds(){
 if(cmdsLoaded)return; cmdsLoaded=true;
 try{const cmds=await getJ('/api/commands');const box=$('#cmds');box.innerHTML='';box.classList.remove('muted');
  cmds.forEach(c=>{
   const row=document.createElement('div');row.className='cmd';
   const args=(c.args||'').split(',').filter(Boolean);
   let ins='';args.forEach(a=>{const[nm,ty]=a.split(':');ins+='<input data-k="'+nm+'" placeholder="'+nm+(ty?':'+ty:'')+'">';});
   row.innerHTML='<span class="nm">'+c.name+'</span>'+ins+'<button class="go">run</button><span class="hp">'+(c.help||'')+'</span>';
   row.querySelector('.go').onclick=()=>runCmd(c.name,row);
   box.appendChild(row);
  });
 }catch(e){cmdsLoaded=false;$('#cmds').textContent='failed to load commands';}
}
async function runCmd(name,row){
 const params={};row.querySelectorAll('input').forEach(i=>{if(i.value!=='')params[i.dataset.k]=i.value;});
 try{const r=await cmd(name,params);T($('#res'),name+' → '+JSON.stringify(r));}
 catch(e){T($('#res'),name+' → error '+e.message);}
 pollEvents();poll();
}
let lastSeq=0;
async function pollEvents(){
 try{const evs=await getJ('/api/events?since='+lastSeq);const log=$('#log');
  evs.forEach(e=>{lastSeq=Math.max(lastSeq,e.seq);
   const ts=e.ts>=1e9?new Date(e.ts*1000).toLocaleString():'+'+e.ts+'ms';
   const d=document.createElement('div');
   d.innerHTML='<span class="s">#'+e.seq+' '+ts+'</span> <span class="t">'+esc(e.type)+'</span> '+esc(e.detail||'');
   log.prepend(d);});
 }catch(e){}
}

/* ---- lid (home) ---- */
// Endstops are bench-verified inverse: closed_end==0 at the OPEN end, open_end==0 at the CLOSED end.
function renderLid(){
 const l=S.lid||{};
 let pos='–',cls='idle';
 if(l.closed_end===0){pos='Open';cls='ok';}
 else if(l.open_end===0){pos='Closed';cls='idle';}
 else if(l.drive===1){pos='Opening…';cls='warn';}
 else if(l.drive===-1){pos='Closing…';cls='warn';}
 $('#lidPos').innerHTML='<span class="pill '+cls+'"><i class="d"></i>'+pos+'</span>';
 $('#lidAclNote').hidden=!(S.acl&&S.acl.enabled);
}
$('#lidOpen').onclick =()=>cmd('lid.goto',{target:'open'}).then(poll).catch(e=>flash('Lid open failed'));
$('#lidClose').onclick=()=>cmd('lid.goto',{target:'close'}).then(poll).catch(e=>flash('Lid close failed'));
$('#lidStop').onclick =()=>cmd('lid.stop').then(poll).catch(()=>{});

/* ---- schedule ---- */
const pad2=n=>String(n).padStart(2,'0');
async function loadSchedule(){
 const f=S.feeder||{};
 $('#schAuto').checked=!!f.auto;
 $('#schHint').textContent = f.time_known===false
   ? '⚠ Clock not synced yet — scheduled feeds are paused until the time is known.'
   : (!f.auto ? 'Auto feeding is off — saved times won’t fire until you turn it on.'
              : 'Auto feeding is on. Times fire once per day at the device’s local time.');
 try{
  const r=await cmd('feeder.sched.list');const slots=(r&&r.slots)||[];
  $('#schCount').textContent=slots.length+' / 8';
  $('#schedList').innerHTML = slots.length ? slots.map(s=>
    '<div class="li"><span class="li-main"><b>'+pad2(s.h)+':'+pad2(s.m)+'</b> <span class="muted">'+s.portions+' portion'+(s.portions>1?'s':'')+'</span></span>'+
    '<span class="pill '+(s.on?'ok':'idle')+'" style="padding:3px 9px"><i class="d"></i>'+(s.on?'on':'off')+'</span></div>').join('')
    : '<div class="empty">No feeding times yet — add one below.</div>';
 }catch(e){$('#schedList').innerHTML='<div class="empty">failed to load</div>';}
}
$('#schAuto').onchange=async e=>{try{await cmd('feeder.auto',{v:e.target.checked?1:0});}catch(err){}poll();loadSchedule();};
$('#schAdd').onclick=async()=>{
 const tv=$('#schTime').value;if(!tv){flash('Pick a time first');return;}
 const p=tv.split(':');const portions=Math.max(1,parseInt($('#schPortions').value||'1',10));
 try{const r=await cmd('feeder.sched.add',{h:+p[0],m:+p[1],portions});
   if(r&&r.ok===false)flash('Could not add — schedule full (max 8) or invalid time.');
   else flash('Added '+tv);
 }catch(e){}
 loadSchedule();
};
$('#schClear').onclick=async()=>{if(confirm('Clear all feeding times?')){try{await cmd('feeder.sched.clear');}catch(e){}loadSchedule();}};

/* ---- history (client-side analytics from the meal event stream) ---- */
async function loadHistory(){
 try{
  const evs=await getJ('/api/events?since=0');
  const meals=[];
  evs.forEach(e=>{ if(e.type!=='meal') return;
   const m=String(e.detail).match(/^(\w+)\s+(\d+)\/(\d+)p\s+([\d.]+)cup\s+(\w+)/);
   if(m) meals.push({ts:e.ts,src:m[1],delivered:+m[2],wanted:+m[3],cups:+m[4],result:m[5]});
  });
  renderHistory(meals);
 }catch(e){$('#mealList').innerHTML='<div class="empty">failed to load events</div>';$('#chart').innerHTML='';}
}
function renderHistory(meals){
 const real=meals.filter(m=>m.ts>=1e9);   // only events with a real epoch can be dated
 const now=new Date(),days=[];
 for(let i=6;i>=0;i--){const d=new Date(now);d.setDate(now.getDate()-i);d.setHours(0,0,0,0);days.push({d,key:d.toDateString(),cups:0,n:0});}
 const byKey={};days.forEach(b=>byKey[b.key]=b);
 real.forEach(m=>{const d=new Date(m.ts*1000);d.setHours(0,0,0,0);const b=byKey[d.toDateString()];if(b){b.cups+=m.cups;b.n++;}});
 const max=Math.max(0.5,...days.map(b=>b.cups));
 const DOW=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
 $('#chart').innerHTML=days.map(b=>{
  const pct=b.cups>0?Math.max(4,Math.round(b.cups/max*100)):0;
  const lbl=b.key===now.toDateString()?'Today':DOW[b.d.getDay()];
  return '<div class="bar'+(b.cups<=0?' z':'')+'"><span class="bv">'+(b.cups>0?b.cups.toFixed(2):'')+'</span>'+
    '<div class="col"><i style="height:'+pct+'%"></i></div><span class="bl">'+lbl+'</span></div>';
 }).join('');
 $('#kToday').textContent=days[6].cups.toFixed(2);
 $('#kMeals').textContent=days[6].n;
 $('#kWeek').textContent=days.reduce((s,b)=>s+b.cups,0).toFixed(2);
 const RES={done:['ok','✓'],jam:['bad','⚠'],timeout:['warn','⏱'],aborted:['idle','■'],running:['warn','…']};
 const list=real.slice(-14).reverse();
 $('#mealList').innerHTML=list.length?list.map(m=>{
  const r=RES[m.result]||['idle','·'],dt=new Date(m.ts*1000);
  const when=dt.toLocaleDateString([],{month:'short',day:'numeric'})+' '+dt.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});
  let amt=m.cups.toFixed(2)+' cup'+(m.delivered<m.wanted?' ('+m.delivered+'/'+m.wanted+'p)':'');
  return '<div class="li"><span class="li-main"><span class="pill '+r[0]+'" style="padding:3px 9px"><i class="d"></i>'+r[1]+'</span> <b>'+amt+'</b> <span class="muted">'+esc(m.src)+'</span></span><span class="sub">'+when+'</span></div>';
 }).join(''):'<div class="empty">No dated meals yet (needs the clock synced).</div>';
}

/* ---- access (whitelist + visit log) ---- */
async function loadAccess(){
 const a=S.acl||{};
 $('#aclEnable').checked=!!a.enabled;
 if(document.activeElement!==$('#aclHold'))$('#aclHold').value=(a.hold_s!=null?a.hold_s:5);
 $('#aclNow').textContent=a.present?('At reader: '+(a.last_tag||'unknown')+' · '+(a.authorized?'authorized':'denied')):'';
 try{const r=await cmd('acl.list');renderTags((r&&r.tags)||[]);}catch(e){$('#tagList').innerHTML='<div class="empty">failed to load</div>';}
 try{const evs=await getJ('/api/events?since=0');renderVisits(evs);}catch(e){}
}
function renderTags(tags){
 $('#tagList').innerHTML=tags.length?tags.map(t=>
   '<div class="li"><span class="li-main mono">'+esc(t)+'</span><button class="x" data-tag="'+esc(t)+'">remove</button></div>').join('')
   :'<div class="empty">No collars whitelisted.</div>';
}
function renderVisits(evs){
 const vs=evs.filter(e=>e.type==='visit').slice(-15).reverse();
 $('#visitList').innerHTML=vs.length?vs.map(e=>{
  const d=String(e.detail);let cls,icon,label;
  if(d.indexOf('end ')===0){const m=d.match(/^end\s+(\S+)\s+(\d+)s/);cls='idle';icon='↩';label='Left'+(m?' after '+m[2]+'s · '+m[1]:'');}
  else{const m=d.match(/^(\S+)\s+(auth|deny)/);const ok=m&&m[2]==='auth';cls=ok?'ok':'bad';icon=ok?'✓':'⛔';label=(ok?'Authorized':'Denied')+(m?' · '+m[1]:'');}
  const ts=e.ts>=1e9?new Date(e.ts*1000).toLocaleString([],{month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'}):'+'+e.ts+'ms';
  return '<div class="li"><span class="li-main"><span class="pill '+cls+'" style="padding:3px 9px"><i class="d"></i>'+icon+'</span> '+esc(label)+'</span><span class="sub">'+ts+'</span></div>';
 }).join(''):'<div class="empty">No visits recorded yet.</div>';
}
$('#tagList').addEventListener('click',async e=>{const b=e.target.closest('[data-tag]');if(b){await cmd('acl.remove',{tag:b.dataset.tag}).catch(()=>{});loadAccess();}});
$('#aclAdd').onclick=async()=>{const v=$('#aclTagIn').value.trim();if(!v)return;await cmd('acl.add',{tag:v}).catch(()=>{});$('#aclTagIn').value='';loadAccess();};
$('#aclAddCurrent').onclick=async()=>{const t=(S.acl&&S.acl.last_tag)||(S.rfid&&S.rfid.last_tag);
 if(!t){flash('No collar seen yet — present one near the reader, then try again.');return;}
 await cmd('acl.add',{tag:t}).catch(()=>{});flash('Added '+t);loadAccess();};
$('#aclEnable').onchange=async e=>{await cmd('acl.enable',{v:e.target.checked?1:0}).catch(()=>{});poll();};
$('#aclHoldSet').onclick=async()=>{const s=Math.max(0,parseInt($('#aclHold').value||'5',10));await cmd('acl.hold',{s}).catch(()=>{});flash('Hold set to '+s+'s');};
$('#aclClear').onclick=async()=>{if(confirm('Remove all whitelisted collars?')){await cmd('acl.clear').catch(()=>{});loadAccess();}};

/* ---- timezone + DST (browser-derived; transitions pushed to the device) ---- */
function tzAt(ms){return -new Date(ms).getTimezoneOffset();}     // minutes; local = utc + this
function fmtOff(min){const s=min<0?'-':'+',a=Math.abs(min|0);return 'UTC'+s+pad2(Math.floor(a/60))+':'+pad2(a%60);}
// Current offset + up to 4 upcoming DST transitions over the next ~13 months.
function computeTz(){
 const DAY=86400000,now=Date.now(),cur=tzAt(now),trans=[];
 let t=now,lastOff=cur,guard=0;
 while(t<now+400*DAY && trans.length<4 && guard++<420){
  const next=t+DAY,off=tzAt(next);
  if(off!==lastOff){
   let lo=t,hi=next;                              // refine the transition to the minute
   while(hi-lo>60000){const mid=Math.floor((lo+hi)/2);if(tzAt(mid)===lastOff)lo=mid;else hi=mid;}
   trans.push({at:Math.round(hi/1000),off});lastOff=off;
  }
  t=next;
 }
 return {cur,trans};
}
async function applyTz(){
 const {cur,trans}=computeTz();
 try{
  await cmd('time.tz',{min:cur});
  await cmd('time.dst',{list:trans.map(x=>x.at+':'+x.off).join(',')});
  flash('Timezone set to '+fmtOff(cur)+(trans.length?' · DST automatic':' · no DST here'));
  poll();
 }catch(e){flash('Could not set timezone');}
}
let tzChecked=false;
function ensureTz(){     // auto-sync once per page load when the device disagrees with this browser
 if(tzChecked||!S.clock)return;tzChecked=true;
 const {cur,trans}=computeTz(),ck=S.clock,nextAt=trans.length?trans[0].at:0;
 if(ck.tz_min!==cur || (ck.dst_next||0)!==nextAt) applyTz();
}
function renderTz(){
 const ck=S.clock||{},cur=tzAt(Date.now());
 $('#tzVal').innerHTML=fmtOff(ck.tz_min||0)+(ck.dst_n>0?' <small class="muted">· DST auto</small>':'');
 $('#tzBtn').textContent=(ck.tz_min!==undefined&&ck.tz_min!==cur)?('Match this device ('+fmtOff(cur)+')'):'Re-sync timezone';
}
$('#tzBtn').onclick=()=>applyTz();

/* ---- boot ---- */
renderFeedCtl();poll();
setInterval(poll,1500);
setInterval(()=>{if($('.tab.on').dataset.t==='bench'||online)pollEvents();},2500);
</script>
</body></html>)HTML";
