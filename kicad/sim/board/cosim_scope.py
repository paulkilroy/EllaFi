#!/usr/bin/env python3
"""Render cosim_traces.json (from cosim_replay.py) into a self-contained
virtual-scope HTML dashboard. Usage: python3 cosim_scope.py [out.html]"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# trace color assignment by entity (fixed, light/dark pairs)
ROLE = {
    '12V rail':  ('power',  '#2a78d6', '#3987e5', 'V'),
    '5V':        ('power',  '#2a78d6', '#3987e5', 'V'),
    '3V3':       ('power',  '#2a78d6', '#3987e5', 'V'),
    'coin line': ('coin',   '#eb6834', '#d95926', 'V'),
    'GPIO4':     ('coin',   '#eb6834', '#d95926', 'V'),
    'GPIO14':    ('ctrl',   '#1baf7a', '#199e70', 'V'),
    'Q1 drain':  ('ctrl',   '#1baf7a', '#199e70', 'V'),
}
DAY_ORDER  = ['12V rail', '5V', '3V3', 'coin line', 'GPIO4', 'GPIO14', 'Q1 drain']
ZOOM_ORDER = ['coin line', 'GPIO4', '3V3']

def classify(msg):
    if 'ACCEPTED' in msg: return 'good'
    if 'ABORTED' in msg or 'fraud' in msg: return 'critical'
    if 'suppressed' in msg or 'discarded' in msg: return 'muted'
    return 'muted'

def pack(traces, order):
    out = []
    for name in order:
        pts = traces[name]
        out.append({'name': name, 'role': ROLE[name][0],
                    'cl': ROLE[name][1], 'cd': ROLE[name][2],
                    'pts': [[round(t, 6), round(v, 4)] for t, v in pts]})
    return out

def main():
    rep = json.load(open(os.path.join(HERE, 'cosim_traces.json')))
    day, zoom = rep['runs']['dayinlife'], rep['runs']['attackzoom']
    C = rep['constants']
    data = {
        'constants': C,
        'day': {'strips': pack(day['traces'], DAY_ORDER),
                'events': [[round(t,5), m, classify(m)] for t, m in day['events']],
                'accepted': day['accepted'], 'fraud': day['fraud'],
                'aborted': day['aborted'], 'brownout': day['brownout_at'],
                'tmax': 4.5,
                'bands': [[0.7, 0.7 + C['boot_suppress_ms']/1000.0, 'suppress'],
                          [0.7 + C['boot_suppress_ms']/1000.0, 2.5204, 'accept']]},
        'zoom': {'strips': pack(zoom['traces'], ZOOM_ORDER),
                 'events': [[round(t,6), m, classify(m)] for t, m in zoom['events']],
                 'accepted': zoom['accepted'], 'fraud': zoom['fraud'], 'tmax': 0.06,
                 'bands': [], 'attacks': [0.005, 0.015]},
    }
    payload = json.dumps(data, separators=(',', ':'))
    html = TEMPLATE.replace('/*__DATA__*/null', payload)
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'scope.html')
    open(out, 'w').write(html)
    print(f"wrote {out} ({len(html)//1024} KB)")

TEMPLATE = r'''<title>EllaFi — Virtual Scope</title>
<style>
:root{
  --page:#f9f9f7; --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781;
  --grid:#e1e0d9; --axis:#c3c2b7; --ring:rgba(11,11,11,.10);
  --good:#0ca30c; --serious:#ec835a; --critical:#d03b3b;
  --band-suppress:rgba(137,135,129,.12); --band-accept:rgba(12,163,12,.07);
}
@media (prefers-color-scheme: dark){ :root:where(:not([data-theme="light"])){
  --page:#0d0d0d; --surface:#1a1a19; --ink:#fff; --ink2:#c3c2b7; --muted:#898781;
  --grid:#2c2c2a; --axis:#383835; --ring:rgba(255,255,255,.10);
  --band-suppress:rgba(137,135,129,.14); --band-accept:rgba(12,163,12,.10);
}}
:root[data-theme="dark"]{
  --page:#0d0d0d; --surface:#1a1a19; --ink:#fff; --ink2:#c3c2b7; --muted:#898781;
  --grid:#2c2c2a; --axis:#383835; --ring:rgba(255,255,255,.10);
  --band-suppress:rgba(137,135,129,.14); --band-accept:rgba(12,163,12,.10);
}
*{box-sizing:border-box} body{margin:0;background:var(--page);color:var(--ink);
  font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-variant-numeric:tabular-nums}
.wrap{max-width:1080px;margin:0 auto;padding:28px 20px 60px}
header h1{font-size:22px;margin:0 0 2px;letter-spacing:-.01em}
header .sub{color:var(--ink2);margin:0 0 20px;max-width:70ch}
.eyebrow{font-family:ui-monospace,Menlo,monospace;font-size:11px;letter-spacing:.14em;
  text-transform:uppercase;color:var(--muted)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin:18px 0 34px}
.tile{background:var(--surface);border:1px solid var(--ring);border-radius:6px;padding:12px 14px}
.tile .v{font-size:24px;font-weight:650;margin-top:2px}
.tile .v.good{color:var(--good)} .tile .v.critical{color:var(--critical)} .tile .v.serious{color:var(--serious)}
.tile .d{font-size:12px;color:var(--ink2);margin-top:2px}
section{margin:34px 0}
section>h2{font-size:16px;margin:0 0 4px}
section>.note{color:var(--ink2);font-size:13.5px;margin:0 0 14px;max-width:78ch}
.scope{background:var(--surface);border:1px solid var(--ring);border-radius:6px;
  padding:10px 12px 6px;position:relative;overflow-x:auto}
.striplbl{fill:var(--ink2);font-size:11px;font-family:ui-monospace,Menlo,monospace}
.ylbl{fill:var(--muted);font-size:9.5px;font-family:ui-monospace,Menlo,monospace}
.xtick{fill:var(--muted);font-size:10px;font-family:ui-monospace,Menlo,monospace}
.evflag{fill:var(--muted);font-size:9.5px;font-family:ui-monospace,Menlo,monospace}
.tip{position:absolute;pointer-events:none;background:var(--surface);border:1px solid var(--ring);
  border-radius:5px;padding:6px 9px;font-size:11px;display:none;z-index:3;
  box-shadow:0 2px 10px rgba(0,0,0,.18);min-width:130px}
.tip .t{color:var(--muted);margin-bottom:3px}
.tip .row{display:flex;justify-content:space-between;gap:12px}
.tip .row b{font-weight:600}
table{border-collapse:collapse;width:100%;font-size:12.5px;background:var(--surface);
  border:1px solid var(--ring);border-radius:6px;overflow:hidden}
th{text-align:left;font-size:11px;letter-spacing:.08em;text-transform:uppercase;
  color:var(--muted);font-weight:600;padding:8px 12px;border-bottom:1px solid var(--grid)}
td{padding:6px 12px;border-bottom:1px solid var(--grid);vertical-align:top}
tr:last-child td{border-bottom:none}
.pill{display:inline-block;padding:1px 8px;border-radius:99px;font-size:11px;font-weight:600}
.pill.good{background:color-mix(in srgb,var(--good) 14%,transparent);color:var(--good)}
.pill.critical{background:color-mix(in srgb,var(--critical) 14%,transparent);color:var(--critical)}
.pill.muted{background:color-mix(in srgb,var(--muted) 16%,transparent);color:var(--ink2)}
footer{margin-top:40px;color:var(--muted);font-size:12.5px;max-width:80ch}
footer code{font-family:ui-monospace,Menlo,monospace;font-size:11.5px}
</style>
<div class="wrap">
<header>
  <div class="eyebrow">EllaFi vendo board · ngspice × coin.cpp co-simulation</div>
  <h1>Virtual scope — the board's day, electrically</h1>
  <p class="sub">Every trace below is SPICE-simulated from the real netlist under real-world abuse
  (supply ripple, brownout, dropout, a worn acceptor, a piezo attack). The verdicts come from replaying
  the <span class="mono">coin.cpp</span> decision logic — constants parsed live from
  <span class="mono">globals.h</span> — over the simulated GPIO4 waveform.</p>
</header>
<div class="tiles" id="tiles"></div>
<section id="daysec">
  <h2>Capture 1 — day in the life <span class="eyebrow" style="margin-left:8px">4.5 s · 20 µs step</span></h2>
  <p class="note">Boot (pin floating) → session start → two coins during the 500 ms suppress window →
  two clean coins (one only 4 ms wide) → 9 V brownout (rides through) → a worn acceptor's bouncing coin
  (watch the fraud counter) → a jammed double-insert → 3 ms mains dropout (rails collapse, CPU resets) →
  a coin after session end that dies at Q1 before software ever sees it.</p>
  <div class="scope" id="dayscope"></div>
</section>
<section id="zoomsec">
  <h2>Capture 2 — pitik attack zoom <span class="eyebrow" style="margin-left:8px">60 ms · 100 ns step</span></h2>
  <p class="note">A piezo lighter spark (300 V ring) into the coin wire at 5 ms, a second spark coupled
  straight onto the GPIO4 trace at 15 ms, then one honest coin at 30 ms. D1 clamps the wire at ~7 V and
  the opto never passes a logic edge — the firmware's width filter is never even needed.</p>
  <div class="scope" id="zoomscope"></div>
</section>
<section>
  <h2>Firmware event log</h2>
  <p class="note">Output of the replayed <span class="mono">coinSlotPulse()</span> ISR +
  <span class="mono">coinPulseTask()</span> filters.</p>
  <table><thead><tr><th>t</th><th>capture</th><th>event</th></tr></thead><tbody id="log"></tbody></table>
</section>
<footer>
  <p>Method: layer 1 = <code>tb_dayinlife.cir</code> / <code>tb_attack_zoom.cir</code> (whole-board SPICE,
  behavioral MPM3620A / FOD817 / AOD4184A / thermal polyfuse); layer 2 = <code>cosim_replay.py</code>
  (ratiometric ESP32 input thresholds, brownout reset at 2.8 V, verbatim ISR/task semantics).
  Firmware constants this run: width ≥ <span id="cw"></span> µs, interval ≥ <span id="ci"></span> ms,
  boot suppress <span id="cb"></span> ms, fraud limit <span id="cf"></span>.
  Regenerate: <code>ngspice -b tb_*.cir && python3 cosim_replay.py && python3 cosim_scope.py</code>.</p>
</footer>
</div>
<script>
const DATA=/*__DATA__*/null;
const dark=()=>document.documentElement.dataset.theme==='dark'||
  (document.documentElement.dataset.theme!=='light'&&matchMedia('(prefers-color-scheme: dark)').matches);
const css=v=>getComputedStyle(document.documentElement).getPropertyValue(v).trim();
const fmt=(v,d)=>Number(v).toFixed(d);
function tiles(){
  const d=DATA.day,z=DATA.zoom,el=document.getElementById('tiles');
  const t=(lbl,val,cls,det)=>`<div class="tile"><div class="eyebrow">${lbl}</div>
    <div class="v ${cls||''}">${val}</div><div class="d">${det}</div></div>`;
  el.innerHTML=
    t('coins accepted',d.accepted.length,'good',`of 8 inserted — the other 5 correctly refused`)+
    t('fraud events',d.fraud,d.fraud?'critical':'good','all three from one bouncing coin')+
    t('session outcome','ABORTED','critical','fraud limit hit at t=2.520 s')+
    t('attack pulses passed',z.fraud+z.accepted.length-1,'good','both sparks died in the analog layer')+
    t('brownout reset',d.brownout?fmt(d.brownout,2)+' s':'—','serious','3 ms dropout — RAM coin count lost');
}
function scope(host,run,tfmt,tunit){
  const W=1020,LX=118,RX=W-14,SH=64,GAP=10,TOPPAD=26,XAX=24;
  const strips=run.strips,H=TOPPAD+strips.length*(SH+GAP)+XAX;
  const px=t=>LX+(RX-LX)*t/run.tmax;
  let s=`<svg viewBox="0 0 ${W} ${H}" width="100%" style="min-width:760px;display:block">`;
  // bands
  for(const[b0,b1,kind]of run.bands)
    s+=`<rect x="${px(b0)}" y="${TOPPAD-4}" width="${px(b1)-px(b0)}" height="${strips.length*(SH+GAP)-GAP+8}"
        fill="var(--band-${kind})"/>`;
  // band labels
  for(const[b0,b1,kind]of run.bands)
    s+=`<text class="evflag" x="${px(b0)+4}" y="${TOPPAD+8}">${kind==='suppress'?'boot suppress':'accepting coins'}</text>`;
  // event flag lane
  const lanes={};
  for(const[t,m,cls]of run.events){
    const x=px(t);
    const key=Math.round(x/34);
    if(lanes[key]){lanes[key].n++;continue}
    lanes[key]={x,n:1,cls};
  }
  for(const k in lanes){const f=lanes[k];
    const col=f.cls==='good'?'var(--good)':f.cls==='critical'?'var(--critical)':'var(--muted)';
    s+=`<line x1="${f.x}" y1="${TOPPAD-6}" x2="${f.x}" y2="${H-XAX}" stroke="${col}"
         stroke-width="1" stroke-dasharray="2 3" opacity=".55"/>
        <path d="M ${f.x-4} ${TOPPAD-14} h8 l-4 7 z" fill="${col}"/>`;
    if(f.n>1)s+=`<text class="evflag" x="${f.x+5}" y="${TOPPAD-8}">×${f.n}</text>`;
  }
  if(run.attacks)for(const t of run.attacks){
    s+=`<text class="evflag" x="${px(t)+5}" y="${TOPPAD+8}" fill="var(--critical)">spark</text>`;}
  // strips
  strips.forEach((st,i)=>{
    const y0=TOPPAD+i*(SH+GAP);
    let lo=Infinity,hi=-Infinity;
    for(const[t,v]of st.pts){if(v<lo)lo=v;if(v>hi)hi=v}
    if(hi-lo<0.5){const m=(hi+lo)/2;lo=m-0.25;hi=m+0.25}
    const pad=(hi-lo)*0.08;lo-=pad;hi+=pad;
    st._lo=lo;st._hi=hi;st._y0=y0;st._sh=SH;
    const py=v=>y0+SH-(SH*(v-lo)/(hi-lo));
    s+=`<rect x="${LX}" y="${y0}" width="${RX-LX}" height="${SH}" fill="none" stroke="var(--grid)"/>`;
    s+=`<line x1="${LX}" y1="${py(0)>y0&&py(0)<y0+SH?py(0):y0+SH}" x2="${RX}"
         y2="${py(0)>y0&&py(0)<y0+SH?py(0):y0+SH}" stroke="var(--axis)" stroke-width="1"/>`;
    s+=`<text class="striplbl" x="8" y="${y0+13}">${st.name}</text>`;
    s+=`<text class="ylbl" x="8" y="${y0+27}">${fmt(hi,1)} V</text>`;
    s+=`<text class="ylbl" x="8" y="${y0+SH-3}">${fmt(lo,1)} V</text>`;
    let d='';
    for(const[t,v]of st.pts)d+=(d?'L':'M')+fmt(px(t),1)+' '+fmt(py(v),1)+' ';
    s+=`<path d="${d}" fill="none" stroke="${dark()?st.cd:st.cl}" stroke-width="1.6"
        stroke-linejoin="round" vector-effect="non-scaling-stroke"/>`;
  });
  // x axis
  const y=H-XAX+14;
  for(let i=0;i<=6;i++){const t=run.tmax*i/6;
    s+=`<text class="xtick" x="${px(t)}" y="${y}" text-anchor="middle">${tfmt(t)}</text>`;}
  s+=`<text class="xtick" x="${RX}" y="${y+0}" text-anchor="start" dx="6">${tunit}</text>`;
  s+=`<line id="ch" x1="0" y1="${TOPPAD}" x2="0" y2="${H-XAX}" stroke="var(--ink2)"
      stroke-width="1" opacity="0" pointer-events="none"/>`;
  s+='</svg><div class="tip"></div>';
  host.innerHTML=s;
  // crosshair + tooltip
  const svg=host.querySelector('svg'),tip=host.querySelector('.tip'),ch=host.querySelector('#ch');
  svg.addEventListener('mousemove',e=>{
    const r=svg.getBoundingClientRect();
    const sx=(e.clientX-r.left)*W/r.width;
    if(sx<LX||sx>RX){tip.style.display='none';ch.setAttribute('opacity',0);return}
    const t=(sx-LX)/(RX-LX)*run.tmax;
    ch.setAttribute('x1',sx);ch.setAttribute('x2',sx);ch.setAttribute('opacity',.5);
    let rows='';
    for(const st of strips){
      let a=0,b=st.pts.length-1;
      while(b-a>1){const m=(a+b)>>1;(st.pts[m][0]<t)?a=m:b=m}
      rows+=`<div class="row"><span>${st.name}</span><b class="mono">${fmt(st.pts[a][1],2)} V</b></div>`;
    }
    tip.innerHTML=`<div class="t mono">t = ${tfmt(t)} ${tunit}</div>`+rows;
    tip.style.display='block';
    const hx=e.clientX-host.getBoundingClientRect().left;
    tip.style.left=Math.min(hx+14,host.clientWidth-160)+'px';
    tip.style.top=(e.clientY-host.getBoundingClientRect().top+12)+'px';
  });
  svg.addEventListener('mouseleave',()=>{tip.style.display='none';ch.setAttribute('opacity',0)});
}
function log(){
  const rows=[...DATA.day.events.map(([t,m,c])=>[t,'day',m,c]),
              ...DATA.zoom.events.map(([t,m,c])=>[t,'zoom',m,c])];
  document.getElementById('log').innerHTML=rows.map(([t,run,m,c])=>
    `<tr><td class="mono">${run==='day'?fmt(t,4)+' s':fmt(t*1000,3)+' ms'}</td>
     <td>${run}</td><td><span class="pill ${c}">${m}</span></td></tr>`).join('');
  for(const[id,k]of[['cw','width_min_us'],['ci','interval_min_ms'],['cb','boot_suppress_ms'],['cf','max_fraud']])
    document.getElementById(id).textContent=DATA.constants[k];
}
function renderAll(){
  tiles();
  scope(document.getElementById('dayscope'),DATA.day,t=>fmt(t,1),'s');
  scope(document.getElementById('zoomscope'),DATA.zoom,t=>fmt(t*1000,0),'ms');
  log();
}
renderAll();
new MutationObserver(renderAll).observe(document.documentElement,{attributes:true,attributeFilter:['data-theme']});
matchMedia('(prefers-color-scheme: dark)').addEventListener('change',renderAll);
</script>
'''

if __name__ == '__main__':
    main()
