// Single-page web UI for the WiFi firmware updater (served by web_ota.cpp).
// Fully self-contained: the phone has no internet while it is on the hotspot.
#ifndef WHEELIE_WEB_PAGE_H
#define WHEELIE_WEB_PAGE_H

#include <Arduino.h>

static const char WEB_PAGE[] PROGMEM = R"WAPAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<meta name="theme-color" content="#0a0f1c">
<title>WheelieAssist &middot; Update</title>
<link rel="icon" href="data:,">
<style>
:root{--bg:#0a0f1c;--card:rgba(15,23,42,.78);--tile:rgba(148,163,184,.06);--line:rgba(148,163,184,.14);--tx:#e7edf7;--mu:#8b9ab4;--cy:#06b6d4;--cy2:#22d3ee;--am:#f59e0b;--rd:#ef4444;--gn:#10b981;--sh:0 1px 0 rgba(255,255,255,.04) inset,0 20px 40px -24px rgba(0,0,0,.8);--g1:rgba(6,182,212,.18);--g2:rgba(245,158,11,.08);color-scheme:dark}
@media (prefers-color-scheme:light){:root{--bg:#eef2f8;--card:rgba(255,255,255,.9);--tile:rgba(15,23,42,.035);--line:rgba(15,23,42,.1);--tx:#0b1324;--mu:#5a6780;--cy:#0891b2;--cy2:#0e7490;--am:#d97706;--rd:#dc2626;--gn:#059669;--sh:0 12px 30px -18px rgba(15,23,42,.35);--g1:rgba(6,182,212,.14);--g2:rgba(245,158,11,.08);color-scheme:light}}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%;-webkit-tap-highlight-color:transparent}
body{margin:0;min-height:100vh;background:var(--bg);color:var(--tx);font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif}
body:before{content:"";position:fixed;top:0;left:0;right:0;bottom:0;z-index:-1;pointer-events:none;background:radial-gradient(900px 520px at 0% -10%,var(--g1),transparent 60%),radial-gradient(700px 420px at 110% 0%,var(--g2),transparent 60%)}
.wrap{max-width:600px;margin:0 auto;padding:calc(12px + env(safe-area-inset-top)) calc(16px + env(safe-area-inset-right)) calc(28px + env(safe-area-inset-bottom)) calc(16px + env(safe-area-inset-left))}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 2px 18px}
.brand{display:flex;align-items:center;gap:12px;min-width:0}
.logo{width:42px;height:42px;flex:none;border-radius:12px;display:grid;place-items:center;background:linear-gradient(140deg,#0e2a3a,#0a1626);box-shadow:0 0 0 1px rgba(6,182,212,.35),0 8px 24px -8px rgba(6,182,212,.6)}
h1{margin:0;font-size:18px;line-height:1.2;font-weight:700;letter-spacing:-.01em}
.sub{margin:2px 0 0;color:var(--mu);font-size:13px}
.hr{display:flex;align-items:center;gap:10px;flex:none}
.badge{font:600 12px/1 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;padding:7px 10px;border-radius:99px;color:var(--cy2);background:rgba(6,182,212,.12);box-shadow:0 0 0 1px rgba(6,182,212,.3) inset;min-width:62px;text-align:center}
.dot{width:10px;height:10px;border-radius:50%;background:var(--mu);position:relative}
.dot.ok{background:var(--gn);box-shadow:0 0 10px var(--gn)}
.dot.ok:after{content:"";position:absolute;top:-4px;left:-4px;right:-4px;bottom:-4px;border-radius:50%;border:2px solid var(--gn);opacity:0;animation:pl 2s ease-out infinite}
.dot.bad{background:var(--rd);box-shadow:0 0 10px var(--rd)}
@keyframes pl{0%{transform:scale(.5);opacity:.7}100%{transform:scale(1.4);opacity:0}}
.card{background:var(--card);border:1px solid var(--line);border-radius:20px;padding:18px;margin:0 0 14px;box-shadow:var(--sh);-webkit-backdrop-filter:blur(10px);backdrop-filter:blur(10px)}
.ch{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:0 0 12px}
h2{margin:0;font-size:12px;font-weight:700;letter-spacing:.1em;text-transform:uppercase;color:var(--mu)}
.chip{font-size:12px;font-weight:600;padding:4px 10px;border-radius:99px;color:var(--mu);background:var(--tile);white-space:nowrap}
.chip.ok{color:var(--gn);background:rgba(16,185,129,.12)}
.chip.warn{color:var(--am);background:rgba(245,158,11,.14)}
.speed{display:flex;align-items:baseline;gap:8px;height:74px}
.big{font-size:68px;line-height:1;font-weight:750;letter-spacing:-.03em;font-variant-numeric:tabular-nums;text-shadow:0 0 28px rgba(6,182,212,.35)}
.unit{color:var(--mu);font-weight:600;font-size:16px}
.sbar{height:6px;border-radius:9px;background:var(--tile);overflow:hidden;margin:6px 0 16px}
.sbar i{display:block;height:100%;width:0;border-radius:9px;background:linear-gradient(90deg,var(--cy),var(--am) 70%,var(--rd));transition:width .6s ease}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.stat{background:var(--tile);border:1px solid var(--line);border-radius:14px;padding:10px 12px}
.k{display:block;color:var(--mu);font-size:12px;font-weight:600}
.v{display:block;font-size:20px;font-weight:700;font-variant-numeric:tabular-nums;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.v small{font-size:12px;color:var(--mu);font-weight:600}
.drop{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center;min-height:156px;padding:22px 16px;border-radius:16px;border:1.5px dashed rgba(6,182,212,.45);background:linear-gradient(180deg,rgba(6,182,212,.06),transparent);cursor:pointer;transition:background .2s,border-color .2s,transform .1s}
.drop:hover,.drop.over{background:rgba(6,182,212,.12);border-color:var(--cy2)}
.drop:active{transform:scale(.99)}
.drop strong{font-size:16px}
.drop span{color:var(--mu);font-size:13px}
.ico{width:52px;height:52px;border-radius:50%;display:grid;place-items:center;margin-bottom:4px;background:linear-gradient(140deg,var(--cy),#0e7490);box-shadow:0 8px 24px -8px rgba(6,182,212,.8);color:#fff}
.vh{position:absolute;width:1px;height:1px;opacity:0;overflow:hidden;clip:rect(0 0 0 0)}
code{font:600 .92em ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;color:var(--cy2)}
.file{border:1px solid var(--line);background:var(--tile);border-radius:16px;padding:12px}
.fr{display:flex;align-items:center;gap:12px}
.fi{width:40px;height:40px;flex:none;border-radius:10px;display:grid;place-items:center;background:rgba(6,182,212,.14);color:var(--cy2)}
.fn{min-width:0;flex:1}
.fn b{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fn span{color:var(--mu);font-size:13px}
.x{flex:none;width:36px;height:36px;border-radius:10px;border:1px solid var(--line);background:none;color:var(--mu);font-size:20px;line-height:1;cursor:pointer}
.checks{list-style:none;margin:12px 0 0;padding:0;display:grid;gap:6px}
.checks li{display:flex;gap:8px;align-items:flex-start;font-size:14px}
.checks li:before{content:"\2713";flex:none;width:18px;height:18px;margin-top:1px;border-radius:50%;display:grid;place-items:center;font-size:11px;font-weight:800;color:#fff;background:var(--gn)}
.checks li.warn:before{content:"!";background:var(--am)}
.checks li.bad:before{content:"\2715";background:var(--rd)}
details{margin:12px 0 0;border-radius:12px}
summary{cursor:pointer;color:var(--mu);font-size:14px;font-weight:600;padding:6px 2px;list-style-position:inside}
input[type=text]{width:100%;margin-top:6px;padding:12px 14px;font:15px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;color:var(--tx);background:var(--tile);border:1px solid var(--line);border-radius:12px;outline:0}
input[type=text]:focus{border-color:var(--cy);box-shadow:0 0 0 3px rgba(6,182,212,.25)}
.hint{color:var(--mu);font-size:13px;margin:8px 2px 0}
.btn{display:flex;align-items:center;justify-content:center;gap:8px;width:100%;min-height:52px;margin-top:14px;padding:0 18px;border-radius:14px;border:0;font-family:inherit;font-size:16px;font-weight:700;line-height:1;text-decoration:none;cursor:pointer;transition:transform .1s,opacity .2s,box-shadow .2s}
.btn:active{transform:scale(.985)}
.pri{color:#04121b;background:linear-gradient(135deg,var(--cy2),var(--cy));box-shadow:0 10px 26px -10px rgba(6,182,212,.9)}
.pri:disabled{opacity:.4;box-shadow:none;cursor:not-allowed}
.sec{color:var(--tx);background:var(--tile);border:1px solid var(--line)}
.dan{color:var(--rd);background:rgba(239,68,68,.08);border:1px solid rgba(239,68,68,.35)}
.dan.arm{color:#fff;background:var(--rd)}
:focus-visible{outline:3px solid var(--cy2);outline-offset:2px}
.drop:focus-within{outline:3px solid var(--cy2);outline-offset:2px}
.prog{margin-top:14px}
.pt{display:flex;justify-content:space-between;align-items:baseline;font-weight:600}
.pt b{font-size:22px;font-variant-numeric:tabular-nums}
.pbar{height:12px;border-radius:9px;background:var(--tile);border:1px solid var(--line);overflow:hidden;margin:8px 0}
.pbar i{display:block;height:100%;width:0;border-radius:9px;background:linear-gradient(90deg,var(--cy),var(--cy2));background-size:28px 28px;transition:width .25s}
.pbar.run i{background-image:linear-gradient(45deg,rgba(255,255,255,.18) 25%,transparent 25%,transparent 50%,rgba(255,255,255,.18) 50%,rgba(255,255,255,.18) 75%,transparent 75%),linear-gradient(90deg,var(--cy),var(--cy2));animation:st 1s linear infinite}
@keyframes st{to{background-position:28px 0,0 0}}
.pm{display:flex;justify-content:space-between;gap:8px;color:var(--mu);font-size:13px;font-variant-numeric:tabular-nums}
.msg{margin-top:14px;padding:12px 14px;border-radius:12px;font-size:14px;font-weight:500}
.msg.err{color:var(--rd);background:rgba(239,68,68,.1);border:1px solid rgba(239,68,68,.3)}
.msg.warn{color:var(--am);background:rgba(245,158,11,.1);border:1px solid rgba(245,158,11,.3)}
.msg.ok{color:var(--gn);background:rgba(16,185,129,.1);border:1px solid rgba(16,185,129,.3)}
.boot{display:flex;flex-direction:column;align-items:center;text-align:center;gap:10px;padding:18px 6px 6px}
.ring{width:56px;height:56px;border-radius:50%;border:4px solid var(--tile);border-top-color:var(--cy2);animation:sp 1s linear infinite}
.ring.done{animation:none;border-color:var(--gn);display:grid;place-items:center;color:var(--gn);font-size:26px;font-weight:800}
@keyframes sp{to{transform:rotate(360deg)}}
.boot b{font-size:18px}
.boot p{margin:0;color:var(--mu);font-size:14px;max-width:420px}
ol{margin:0;padding-left:22px;display:grid;gap:6px;font-size:14px}
ol li::marker{color:var(--cy2);font-weight:700}
.kv{display:grid;grid-template-columns:auto 1fr;gap:8px 14px;margin:0;font-size:14px;min-height:260px}
.kv dt{color:var(--mu)}
.kv dd{margin:0;text-align:right;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
footer{text-align:center;color:var(--mu);font-size:12px;padding:6px 0 0}
[hidden]{display:none!important}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style></head><body><div class="wrap">
<header>
<div class="brand"><div class="logo" aria-hidden="true"><svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#22d3ee" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M13 2 4 14h7l-1 8 9-12h-7z"/></svg></div>
<div><h1>WheelieAssist</h1><p class="sub">Dashboard firmware</p></div></div>
<div class="hr"><span class="badge" id="ver" title="Installed firmware">v&hellip;</span><span class="dot" id="dot" role="img" aria-label="Connecting"></span></div>
</header>
<main>
<section class="card" aria-labelledby="h-live">
<div class="ch"><h2 id="h-live">Live</h2><span class="chip" id="safe">&hellip;</span></div>
<div class="speed"><span class="big" id="spd">0</span><span class="unit" id="su">km/h</span></div>
<div class="sbar" aria-hidden="true"><i id="sb"></i></div>
<div class="grid">
<div class="stat"><span class="k">Odometer</span><span class="v"><span id="odo">&ndash;</span> <small class="du">km</small></span></div>
<div class="stat"><span class="k">Trip</span><span class="v"><span id="trip">&ndash;</span> <small class="du">km</small></span></div>
<div class="stat"><span class="k">Top speed</span><span class="v"><span id="max">&ndash;</span> <small class="su2">km/h</small></span></div>
<div class="stat"><span class="k">Ride time</span><span class="v" id="ride">&ndash;</span></div>
</div>
</section>
<section class="card" aria-labelledby="h-upd">
<div class="ch"><h2 id="h-upd">Firmware update</h2><span class="chip" id="slot">&hellip;</span></div>
<div class="msg warn" id="lock" hidden>The bike is moving. Stop to install an update.</div>
<div id="pick">
<label class="drop" id="drop" for="file">
<input type="file" id="file" class="vh" aria-describedby="drophint">
<span class="ico" aria-hidden="true"><svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 16V4M6 10l6-6 6 6M4 20h16"/></svg></span>
<strong>Choose firmware file</strong><span id="drophint">Tap to browse, or drop the <code>.bin</code> here</span>
</label>
<div class="file" id="finfo" hidden>
<div class="fr"><span class="fi" aria-hidden="true"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/></svg></span>
<div class="fn"><b id="fname"></b><span id="fmeta"></span></div>
<button class="x" id="clr" type="button" aria-label="Remove file">&times;</button></div>
<ul class="checks" id="checks" aria-live="polite"></ul>
</div>
<details><summary>MD5 checksum (optional)</summary>
<label class="vh" for="md5">MD5 checksum</label>
<input type="text" id="md5" maxlength="32" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false" placeholder="32 hex characters">
<p class="hint">If the release lists an MD5, paste it here and the dashboard refuses a corrupted download.</p>
</details>
</div>
<div class="prog" id="prog" hidden>
<div class="pt"><span id="stage">Uploading&hellip;</span><b id="pct">0%</b></div>
<div class="pbar run" id="pbar" role="progressbar" aria-labelledby="stage" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"><i id="pf"></i></div>
<div class="pm"><span id="pspd">&ndash; MB/s</span><span id="pbytes"></span><span id="peta">&ndash;</span></div>
</div>
<div class="boot" id="boot" hidden><div class="ring" id="ring" aria-hidden="true"></div><b id="bt">Rebooting&hellip;</b><p id="bp">Installing is done. The dashboard restarts now.</p></div>
<div class="msg" id="msg" role="alert" hidden></div>
<button class="btn pri" id="go" type="button" disabled>Install update</button>
</section>
<section class="card" aria-labelledby="h-get">
<div class="ch"><h2 id="h-get">Get firmware</h2><span class="chip">GitHub</span></div>
<ol><li>While your phone still has internet, open the releases page and download the newest <code>.bin</code>.</li>
<li>Turn on the update hotspot in the dashboard settings and join it (scan the QR code).</li>
<li>Pick the file above and tap <b>Install update</b>.</li></ol>
<a class="btn sec" id="rel" href="#" target="_blank" rel="noopener">Open releases page <span aria-hidden="true">&#8599;</span></a>
<p class="hint">This hotspot has no internet. GitHub only opens on mobile data or another WiFi, so download first, then join.</p>
</section>
<section class="card" aria-labelledby="h-dev">
<div class="ch"><h2 id="h-dev">Device</h2><span class="chip" id="up">&hellip;</span></div>
<dl class="kv" id="kv"></dl>
<button class="btn dan" id="rb" type="button">Reboot dashboard</button>
</section>
</main>
<footer>WheelieAssist &middot; 192.168.4.1</footer>
</div>
<script>
(function(){
'use strict';
var $=function(i){return document.getElementById(i)};
var info=null,file=null,fileOk=false,busy=false,safe=true,imp=false,newVer='',tele=0,fails=0;
function fmtB(n){return n>=1048576?(n/1048576).toFixed(2)+' MB':Math.round(n/1024)+' KB'}
function fmtT(s){s=Math.max(0,Math.round(s));var h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;return(h?h+':'+(m<10?'0':''):'')+m+':'+(x<10?'0':'')+x}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function api(url,o,to){o=o||{};return new Promise(function(res,rej){var x=new XMLHttpRequest();x.open(o.m||'GET',url);x.timeout=to||4000;
if(o.m==='POST')x.setRequestHeader('X-Requested-With','WheelieAssist');
x.onload=function(){var j=null;try{j=JSON.parse(x.responseText)}catch(e){}if(x.status>=200&&x.status<300)res(j||{});else rej(new Error(j&&j.error||'Error '+x.status))};
x.onerror=x.ontimeout=function(){rej(new Error('offline'))};x.send(o.b||null)})}
function dot(s){var d=$('dot');d.className='dot '+s;d.setAttribute('aria-label',s==='ok'?'Connected':s==='bad'?'Disconnected':'Connecting')}
function show(el,on){$(el).hidden=!on}
function msg(t,c){var m=$('msg');if(!t){m.hidden=true;return}m.className='msg '+(c||'err');m.textContent=t;m.hidden=false}

function poll(){clearTimeout(tele);if(busy)return;
api('/api/telemetry',{},2500).then(function(d){fails=0;dot('ok');imp=!!d.imperial;
var f=imp?0.621371:1,dl=imp?'mi':'km',sl=imp?'mph':'km/h';
var sp=d.speed_kmh*f;$('spd').textContent=Math.round(sp);$('su').textContent=sl;
$('sb').style.width=Math.min(100,d.speed_kmh/85*100)+'%';
$('odo').textContent=(d.odo_km*f).toFixed(d.odo_km*f<1000?1:0);
$('trip').textContent=(d.trip_km*f).toFixed(1);
$('max').textContent=Math.round(d.max_speed_kmh*f);
[].forEach.call(document.querySelectorAll('.du'),function(e){e.textContent=dl});
[].forEach.call(document.querySelectorAll('.su2'),function(e){e.textContent=sl});
$('ride').textContent=fmtT(d.ride_s);
setSafe(d.safe)}).catch(function(){if(++fails>1)dot('bad')}).then(function(){tele=setTimeout(poll,1000)})}
function setSafe(s){safe=!!s;var c=$('safe');c.textContent=safe?'Parked':'Moving';c.className='chip '+(safe?'ok':'warn');show('lock',!safe&&!busy);upd()}

function loadInfo(){return api('/api/info').then(function(d){info=d;dot('ok');
$('ver').textContent='v'+d.version;$('rel').href=d.releases;
$('slot').textContent=d.next+' \u00b7 '+fmtB(d.next_size);$('up').textContent='up '+fmtT(d.uptime);
var r=[['Firmware',d.name+' v'+d.version],['Built',d.build],['Chip',d.chip+' '+d.rev+' \u00b7 '+d.cores+'\u00d7'+d.mhz+' MHz'],
['Flash',fmtB(d.flash)],['App size',fmtB(d.sketch)],['Running slot',d.running],['Update slot',d.next+' ('+fmtB(d.next_size)+')'],
['Free heap',fmtB(d.heap)+' (min '+fmtB(d.heap_min)+')'],['Free PSRAM',fmtB(d.psram)+' of '+fmtB(d.psram_total)],
['Hotspot',d.ssid],['Clients',d.clients],['Signal','n/a (access point)'],['ESP-IDF',d.idf]];
$('kv').innerHTML=r.map(function(x){return'<dt>'+esc(x[0])+'</dt><dd>'+esc(x[1])+'</dd>'}).join('');
if(file)check(file)})}

var CHIPS={0:'ESP32',2:'ESP32-S2',5:'ESP32-C3',9:'ESP32-S3',12:'ESP32-C2',13:'ESP32-C6',16:'ESP32-H2'};
function readBuf(b){return new Promise(function(res,rej){var r=new FileReader();r.onload=function(){res(new Uint8Array(r.result))};r.onerror=function(){rej(r.error)};r.readAsArrayBuffer(b)})}
function findTag(u){var p=[1,87,65,70,87,124],n=u.length-6;
for(var i=0;i<n;i++){if(u[i]!==1)continue;var k=1;while(k<6&&u[i+k]===p[k])k++;
if(k===6){var e=i+6,s='';while(e<u.length&&u[e]!==1&&e-i<120)s+=String.fromCharCode(u[e++]);return s.split('|')}}return null}
function check(f){var max=info?info.next_size:3145728,L=[],bad=false;
function add(c,t){L.push('<li class="'+c+'">'+esc(t)+'</li>');if(c==='bad')bad=true}
return readBuf(f.size<=max+65536?f:f.slice(0,64)).then(function(u){
if(u[0]!==0xE9)add('bad','Not an ESP32 firmware image. Pick the .bin from the release.');
else{var id=u[12]|u[13]<<8;
if(id!==9)add('bad','Built for '+(CHIPS[id]||'another chip')+'. This dashboard needs an ESP32-S3 build.');
else if(!(u[32]===0x32&&u[33]===0x54&&u[34]===0xCD&&u[35]===0xAB))add('bad','This looks like the -full.bin USB image (bootloader + app). Upload the plain WheelieAssist-x.y.z.bin instead.');
else add('ok','ESP32-S3 application image')}
if(f.size>max)add('bad','Too big: '+fmtB(f.size)+', the update slot holds '+fmtB(max)+'.');
else if(f.size<65536)add('bad','File is too small to be dashboard firmware.');
else add('ok','Fits the update slot ('+fmtB(f.size)+' of '+fmtB(max)+')');
var t=bad?null:findTag(u);newVer='';
if(t&&t.length>=2){newVer=t[1];var cur=info&&info.version;
if(cur&&cur===newVer)add('warn',t[0]+' v'+newVer+' \u00b7 same version as installed');
else add('ok',t[0]+' v'+newVer+(t[2]?' \u00b7 built '+t[2]:'')+(cur?' (installed: v'+cur+')':''))}
else if(!bad)add('warn','No WheelieAssist version tag found. Make sure this is the right firmware.');
$('checks').innerHTML=L.join('');fileOk=!bad;upd()}).catch(function(){$('checks').innerHTML='<li class="bad">Could not read the file.</li>';fileOk=false;upd()})}
function setFile(f){file=f||null;fileOk=false;msg();show('finfo',!!f);show('drop',!f);
if(!f){$('file').value='';$('checks').innerHTML='';upd();return}
$('fname').textContent=f.name;$('fmeta').textContent=fmtB(f.size);$('checks').innerHTML='<li class="warn">Checking&hellip;</li>';check(f)}
function upd(){$('go').disabled=busy||!file||!fileOk||!safe}

var fi=$('file'),dr=$('drop');
var ios=/iP(hone|od|ad)/.test(navigator.userAgent)||(navigator.platform==='MacIntel'&&navigator.maxTouchPoints>1);
if(!ios)fi.accept='.bin,application/octet-stream';
fi.addEventListener('change',function(){setFile(fi.files&&fi.files[0])});
$('clr').addEventListener('click',function(){setFile(null);fi.focus()});
['dragenter','dragover'].forEach(function(e){dr.addEventListener(e,function(v){v.preventDefault();dr.classList.add('over')})});
['dragleave','drop'].forEach(function(e){dr.addEventListener(e,function(v){v.preventDefault();dr.classList.remove('over')})});
dr.addEventListener('drop',function(v){var f=v.dataTransfer&&v.dataTransfer.files;if(f&&f[0])setFile(f[0])});
window.addEventListener('dragover',function(e){e.preventDefault()});window.addEventListener('drop',function(e){e.preventDefault()});

function setP(p,stage){p=Math.max(0,Math.min(100,p));$('pf').style.width=p+'%';$('pct').textContent=Math.floor(p)+'%';$('pbar').setAttribute('aria-valuenow',Math.floor(p));if(stage)$('stage').textContent=stage}
function fail(t){busy=false;show('prog',false);show('pick',true);show('go',true);$('clr').disabled=false;msg(t,'err');upd();poll()}
function install(){if(busy||!file||!fileOk)return;
var md5=$('md5').value.trim().toLowerCase();
if(md5&&!/^[0-9a-f]{32}$/.test(md5)){msg('The MD5 must be 32 hexadecimal characters.');$('md5').focus();return}
busy=true;clearTimeout(tele);msg();upd();show('lock',false);show('go',false);$('clr').disabled=true;
setP(0,'Checking\u2026');$('pspd').textContent='\u2013 MB/s';$('peta').textContent='\u2013';$('pbytes').textContent='';show('prog',true);$('pbar').className='pbar run';
var bootUp=info?info.uptime:0;
api('/api/precheck?size='+file.size).then(function(){
var x=new XMLHttpRequest(),fd=new FormData(),t0=Date.now(),lt=t0,lb=0,spd=0;
fd.append('firmware',file,file.name);
x.open('POST','/update?size='+file.size+(md5?'&md5='+md5:''));
x.setRequestHeader('X-Requested-With','WheelieAssist');x.setRequestHeader('X-FW-Size',String(file.size));
var stg=newVer?'Installing v'+newVer:'Uploading';setP(0,stg+'\u2026');
x.upload.onprogress=function(e){if(!e.lengthComputable)return;var now=Date.now(),dt=(now-lt)/1000;
if(dt>=0.4){var s=(e.loaded-lb)/dt;spd=spd?spd*0.7+s*0.3:s;lt=now;lb=e.loaded;
$('pspd').textContent=(spd/1048576).toFixed(2)+' MB/s';$('peta').textContent=spd>0?fmtT((e.total-e.loaded)/spd)+' left':'\u2013'}
$('pbytes').textContent=fmtB(Math.min(e.loaded,file.size))+' / '+fmtB(file.size);
setP(e.loaded/e.total*100,e.loaded>=e.total?'Verifying\u2026':stg+'\u2026')};
x.upload.onload=function(){setP(100,'Verifying\u2026');$('peta').textContent=''};
x.onload=function(){var j=null;try{j=JSON.parse(x.responseText)}catch(e){}
if(x.status===200&&j&&j.ok)rebooting(bootUp);else fail(j&&j.error?j.error:'Update failed (HTTP '+x.status+').')};
x.onerror=function(){fail('Connection lost during the upload. The dashboard keeps its current firmware; try again.')};
x.send(fd)}).catch(function(e){fail(e.message==='offline'?'Can\u2019t reach the dashboard. Are you still on its WiFi?':e.message)})}
$('go').addEventListener('click',install);
window.addEventListener('beforeunload',function(e){if(busy){e.preventDefault();e.returnValue=''}});

function rebooting(bootUp){show('prog',false);show('pick',false);show('go',false);show('boot',true);$('ring').className='ring';
$('bt').textContent='Rebooting\u2026';$('bp').textContent='Update installed and verified. The dashboard is restarting.';dot('');
var t0=Date.now(),old=info&&info.version;
(function tick(){var el=Date.now()-t0;
if(el>30000){done(false);return}
if(el<3000){setTimeout(tick,1000);return}
api('/api/info',{},1500).then(function(d){if(d.uptime<bootUp||d.uptime<el/1000+5||d.version!==old){info=d;done(true)}else setTimeout(tick,1500)})
.catch(function(){setTimeout(tick,1500)})})();
function done(ok){busy=false;dot(ok?'ok':'bad');$('ring').className='ring done';$('ring').textContent='\u2713';
if(ok){$('bt').textContent='Back online \u00b7 v'+info.version;$('bp').textContent=newVer&&newVer!==info.version?'Heads-up: expected v'+newVer+'. The dashboard may have rolled back.':'The new firmware is running. You can switch the hotspot off in the dashboard settings.';loadInfo();poll()}
else{$('bt').textContent='Update installed';$('bp').textContent='The dashboard restarted and its hotspot is off now. To confirm the new version, turn the update hotspot on again in the dashboard settings and reload this page.'}}}

var arm=0;$('rb').addEventListener('click',function(){var b=$('rb');
if(!arm){b.classList.add('arm');b.textContent='Tap again to reboot';arm=setTimeout(function(){arm=0;b.classList.remove('arm');b.textContent='Reboot dashboard'},4000);return}
clearTimeout(arm);arm=0;b.classList.remove('arm');b.textContent='Rebooting\u2026';b.disabled=true;
api('/api/reboot',{m:'POST'}).then(function(){busy=true;clearTimeout(tele);dot('bad');b.textContent='Rebooting. Hotspot turns off; re-enable it in settings.'}).catch(function(e){b.disabled=false;b.textContent='Reboot dashboard';msg(e.message==='offline'?'Can\u2019t reach the dashboard.':e.message)})});

loadInfo().catch(function(){dot('bad')});poll();
setInterval(function(){if(!busy)loadInfo().catch(function(){})},10000);
})();
</script></body></html>)WAPAGE";

#endif
