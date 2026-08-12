#!/usr/bin/env python3
"""Render the electrical microscope: one page, six orders of magnitude in time.
Reads tb_microscope.cir outputs + cosim_traces.json (day/attack runs).
Usage: python3 microscope_scope.py [out.html]"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))

def load_wrdata(path, nvec):
    t, vecs = [], [[] for _ in range(nvec)]
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) < 2*nvec: continue
            t.append(float(p[0]))
            for i in range(nvec): vecs[i].append(float(p[2*i+1]))
    return t, vecs

def decimate(t, v, n=1800):
    if len(t) <= n: return list(zip(t, v))
    out, step, i = [], len(t)/(n//2), 0.0
    while int(i) < len(t):
        j = min(len(t), int(i+step)); seg = range(int(i), j)
        lo = min(seg, key=lambda k: v[k]); hi = max(seg, key=lambda k: v[k])
        for k in sorted({lo, hi}): out.append((t[k], v[k]))
        i += step
    return out

def window(t, vecs, t0, t1):
    idx = [i for i, tt in enumerate(t) if t0 <= tt <= t1]
    return [t[i] for i in idx], [[v[i] for i in idx] for v in vecs]

ROLE = {'power': ('#2a78d6', '#3987e5'), 'coin': ('#eb6834', '#d95926'), 'ctrl': ('#1baf7a', '#199e70')}

def strip(name, role, t, v, scale):
    cl, cd = ROLE[role]
    return {'name': name, 'cl': cl, 'cd': cd,
            'pts': [[round(tt*scale, 6), round(vv, 4)] for tt, vv in decimate(t, v)]}

def main():
    sections = []
    cs = json.load(open(os.path.join(HERE, 'cosim_traces.json')))

    d = cs['runs']['dayinlife']['traces']
    sections.append({'id': 'sec_s', 'title': 'SECONDS — a day in the life', 'unit': 's', 'tmax': 4.5,
        'note': 'The story scale: boot, session, coins, brownout ride-through, dropout reset. '
                'Firmware co-sim verdicts ride on these exact waveforms (see the co-sim capture page).',
        'strips': [{'name': k, 'cl': ROLE[r][0], 'cd': ROLE[r][1], 'pts': d[k]} for k, r in
                   [('12V rail','power'), ('3V3','power'), ('coin line','coin'), ('GPIO4','coin'), ('GPIO14','ctrl')]]})

    z = cs['runs']['attackzoom']['traces']
    sections.append({'id': 'sec_ms', 'title': 'MILLISECONDS — the pitik attack', 'unit': 'ms', 'tmax': 60,
        'note': '300V spark ring into the coin wire at 5ms (D1 clamps the line at 7.0V, GPIO4 dips only to 2.1V '
                '— never a logic edge), direct trace pickup at 15ms, one honest 20ms coin at 30ms.',
        'strips': [{'name': k, 'cl': ROLE[r][0], 'cd': ROLE[r][1],
                    'pts': [[round(t*1000, 5), v] for t, v in z[k]]} for k, r in
                   [('coin line','coin'), ('GPIO4','coin')]]})

    tf, (coinf, g4f, anodef) = load_wrdata(os.path.join(HERE, 'microscope_fall.dat'), 3)
    sections.append({'id': 'sec_us_f', 'title': 'MICROSECONDS — a coin edge arrives', 'unit': 'µs', 'tmax': 40,
        'note': 'Acceptor transistor closes at 5µs: the line falls through 300nH of harness, the opto LED '
                'lights (anode sags as R2 delivers current), the phototransistor answers after its lag, and '
                'GPIO4 crosses logic-low 0.66µs after contact. This lag is the anti-pitik band-limit: '
                'a 5µs spark pulse cannot hold the LED on long enough to matter.',
        'strips': [strip('coin line', 'coin', tf, coinf, 1e6),
                   strip('opto anode', 'ctrl', tf, anodef, 1e6),
                   strip('GPIO4', 'coin', tf, g4f, 1e6)]})

    tr, (coinr, g4r, anoder) = load_wrdata(os.path.join(HERE, 'microscope_rise.dat'), 3)
    sections.append({'id': 'sec_us_r', 'title': 'MICROSECONDS — the coin edge releases', 'unit': 'µs', 'tmax': 40,
        'note': 'Acceptor opens at 5µs: the opto stays on for ~9µs of storage time (the FOD817 emptying its '
                'base), then R1 (2.2k — the Monte-Carlo-chosen value) recharges GPIO4 + pin capacitance in '
                'tens of ns. Width measurement error from these asymmetric edges: ~9µs on a 20,000µs pulse.',
        'strips': [strip('coin line', 'coin', tr, coinr, 1e6),
                   strip('opto anode', 'ctrl', tr, anoder, 1e6),
                   strip('GPIO4', 'coin', tr, g4r, 1e6)]})

    tb, (vina, diea, vouta) = load_wrdata(os.path.join(HERE, 'microscope_buck.dat'), 3)
    tbw, (vinaw, dieaw, voutaw) = window(tb, [vina, diea, vouta], 400e-6, 500e-6)
    sections.append({'id': 'sec_buck', 'title': 'NANOSECONDS — the buck, switching-level (100µs view)', 'unit': 'µs', 'tmax': 500,
        'tmin': 400,
        'note': 'The MPM3620A model at its real 2MHz switching rate, driven through the board\'s fuse + TVS + '
                'C1 input network. The 12V pin ripple is 31mV peak-to-peak — the 30V/-7V ring that killed the '
                'LM2596 design is structurally absent because the switch loop is on-die. (The slow ~34kHz '
                'wobble on the 5V trace is residual model settling, not real; true output ripple is mV-scale.)',
        'strips': [strip('12V @ pin', 'power', tbw, vinaw, 1e6),
                   strip('5V out', 'power', tbw, voutaw, 1e6)]})

    t2, (vina2, diea2, vouta2) = window(tb, [vina, diea, vouta], 480e-6, 484e-6)
    sections.append({'id': 'sec_ns', 'title': 'NANOSECONDS — eight switching cycles (4µs view)', 'unit': 'µs', 'tmax': 484,
        'tmin': 480,
        'note': 'Individual 2MHz cycles: the internal die node chopping at duty ≈ 5/12 while the external pin '
                'barely notices. This is the whole reason the MPM3620A was chosen — compare kicad/sim/ng_ref_rail.cir '
                'for what the same plot looked like on the LM2596.',
        'strips': [strip('internal die node', 'ctrl', t2, diea2, 1e6),
                   strip('12V @ pin', 'power', t2, vina2, 1e6)]})

    payload = json.dumps({'sections': sections}, separators=(',', ':'))
    html = TEMPLATE.replace('/*__DATA__*/null', payload)
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'microscope.html')
    open(out, 'w').write(html)
    print(f"wrote {out} ({len(html)//1024} KB)")

TEMPLATE = r'''<title>EllaFi — Electrical Microscope</title>
<style>
:root{
  --page:#f9f9f7; --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781;
  --grid:#e1e0d9; --axis:#c3c2b7; --ring:rgba(11,11,11,.10);
}
@media (prefers-color-scheme: dark){ :root:where(:not([data-theme="light"])){
  --page:#0d0d0d; --surface:#1a1a19; --ink:#fff; --ink2:#c3c2b7; --muted:#898781;
  --grid:#2c2c2a; --axis:#383835; --ring:rgba(255,255,255,.10);
}}
:root[data-theme="dark"]{
  --page:#0d0d0d; --surface:#1a1a19; --ink:#fff; --ink2:#c3c2b7; --muted:#898781;
  --grid:#2c2c2a; --axis:#383835; --ring:rgba(255,255,255,.10);
}
*{box-sizing:border-box} body{margin:0;background:var(--page);color:var(--ink);
  font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
.wrap{max-width:1080px;margin:0 auto;padding:28px 20px 60px}
.eyebrow{font-family:ui-monospace,Menlo,monospace;font-size:11px;letter-spacing:.14em;
  text-transform:uppercase;color:var(--muted)}
header h1{font-size:22px;margin:0 0 2px;letter-spacing:-.01em}
header .sub{color:var(--ink2);margin:0 0 8px;max-width:72ch}
.zoombar{display:flex;gap:6px;flex-wrap:wrap;margin:14px 0 26px}
.zoombar a{font-family:ui-monospace,Menlo,monospace;font-size:11px;letter-spacing:.06em;
  padding:3px 10px;border:1px solid var(--ring);border-radius:99px;color:var(--ink2);
  text-decoration:none}
.zoombar a:hover{border-color:var(--ink2)}
section{margin:34px 0}
section>h2{font-size:15px;margin:0 0 4px;font-family:ui-monospace,Menlo,monospace;letter-spacing:.04em}
section>.note{color:var(--ink2);font-size:13.5px;margin:0 0 12px;max-width:80ch}
.scope{background:var(--surface);border:1px solid var(--ring);border-radius:6px;
  padding:10px 12px 6px;position:relative;overflow-x:auto}
.striplbl{fill:var(--ink2);font-size:11px;font-family:ui-monospace,Menlo,monospace}
.ylbl{fill:var(--muted);font-size:9.5px;font-family:ui-monospace,Menlo,monospace}
.xtick{fill:var(--muted);font-size:10px;font-family:ui-monospace,Menlo,monospace}
.tip{position:absolute;pointer-events:none;background:var(--surface);border:1px solid var(--ring);
  border-radius:5px;padding:6px 9px;font-size:11px;display:none;z-index:3;
  box-shadow:0 2px 10px rgba(0,0,0,.18);min-width:140px}
.tip .t{color:var(--muted);margin-bottom:3px;font-family:ui-monospace,Menlo,monospace}
.tip .row{display:flex;justify-content:space-between;gap:12px}
.tip .row b{font-weight:600;font-family:ui-monospace,Menlo,monospace}
footer{margin-top:40px;color:var(--muted);font-size:12.5px;max-width:80ch}
footer code{font-family:ui-monospace,Menlo,monospace;font-size:11.5px}
</style>
<div class="wrap">
<header>
  <div class="eyebrow">EllaFi vendo board · ngspice, netlist-faithful models</div>
  <h1>Electrical microscope — six decades of time</h1>
  <p class="sub">The same board, from the whole day down to individual 2MHz switching cycles.
  Every trace is simulated from the real netlist; behavioral models are tuned to the parts'
  datasheets and cross-checked against the co-sim and Monte Carlo suites.</p>
  <div class="zoombar" id="zoombar"></div>
</header>
<div id="sections"></div>
<footer>
  <p>Sources: <code>tb_microscope.cir</code> (edges + switching-level buck, 2–5ns steps),
  <code>tb_dayinlife.cir</code> / <code>tb_attack_zoom.cir</code> via <code>cosim_traces.json</code>.
  Regenerate: <code>ngspice -b tb_microscope.cir && python3 microscope_scope.py</code>.
  Model honesty: the buck's slow output wobble is settling artifact (annotated); opto lag/storage are
  first-order approximations of FOD817 datasheet dynamics; harness inductance 300nH per wire.</p>
</footer>
</div>
<script>
const DATA=/*__DATA__*/null;
const dark=()=>document.documentElement.dataset.theme==='dark'||
  (document.documentElement.dataset.theme!=='light'&&matchMedia('(prefers-color-scheme: dark)').matches);
const fmt=(v,d)=>Number(v).toFixed(d);
function scope(host,sec){
  const W=1020,LX=118,RX=W-14,SH=72,GAP=10,TOPPAD=8,XAX=24;
  const strips=sec.strips,H=TOPPAD+strips.length*(SH+GAP)+XAX;
  const tmin=sec.tmin||0, tmax=sec.tmax;
  const px=t=>LX+(RX-LX)*(t-tmin)/(tmax-tmin);
  let s=`<svg viewBox="0 0 ${W} ${H}" width="100%" style="min-width:760px;display:block">`;
  strips.forEach((st,i)=>{
    const y0=TOPPAD+i*(SH+GAP);
    let lo=Infinity,hi=-Infinity;
    for(const[t,v]of st.pts){if(v<lo)lo=v;if(v>hi)hi=v}
    if(hi-lo<0.05){const m=(hi+lo)/2;lo=m-0.025;hi=m+0.025}
    const pad=(hi-lo)*0.1;lo-=pad;hi+=pad;
    st._lo=lo;st._hi=hi;
    const py=v=>y0+SH-(SH*(v-lo)/(hi-lo));
    s+=`<rect x="${LX}" y="${y0}" width="${RX-LX}" height="${SH}" fill="none" stroke="var(--grid)"/>`;
    s+=`<text class="striplbl" x="8" y="${y0+13}">${st.name}</text>`;
    s+=`<text class="ylbl" x="8" y="${y0+27}">${fmt(hi,hi-lo<1?3:1)} V</text>`;
    s+=`<text class="ylbl" x="8" y="${y0+SH-3}">${fmt(lo,hi-lo<1?3:1)} V</text>`;
    let d='';
    for(const[t,v]of st.pts)d+=(d?'L':'M')+fmt(px(t),1)+' '+fmt(py(v),1)+' ';
    s+=`<path d="${d}" fill="none" stroke="${dark()?st.cd:st.cl}" stroke-width="1.5"
        stroke-linejoin="round"/>`;
  });
  const y=H-XAX+14;
  for(let i=0;i<=6;i++){const t=tmin+(tmax-tmin)*i/6;
    s+=`<text class="xtick" x="${px(t)}" y="${y}" text-anchor="middle">${fmt(t,(tmax-tmin)<1?2:(tmax-tmin)<100?1:0)}</text>`;}
  s+=`<text class="xtick" x="${RX}" y="${y}" text-anchor="start" dx="6">${sec.unit}</text>`;
  s+=`<line id="ch_${sec.id}" x1="0" y1="${TOPPAD}" x2="0" y2="${H-XAX}" stroke="var(--ink2)"
      stroke-width="1" opacity="0" pointer-events="none"/>`;
  s+='</svg><div class="tip"></div>';
  host.innerHTML=s;
  const svg=host.querySelector('svg'),tip=host.querySelector('.tip'),ch=host.querySelector(`#ch_${sec.id}`);
  svg.addEventListener('mousemove',e=>{
    const r=svg.getBoundingClientRect();
    const sx=(e.clientX-r.left)*W/r.width;
    if(sx<LX||sx>RX){tip.style.display='none';ch.setAttribute('opacity',0);return}
    const t=tmin+(sx-LX)/(RX-LX)*(tmax-tmin);
    ch.setAttribute('x1',sx);ch.setAttribute('x2',sx);ch.setAttribute('opacity',.5);
    let rows='';
    for(const st of strips){
      let a=0,b=st.pts.length-1;
      while(b-a>1){const m=(a+b)>>1;(st.pts[m][0]<t)?a=m:b=m}
      rows+=`<div class="row"><span>${st.name}</span><b>${fmt(st.pts[a][1],3)} V</b></div>`;
    }
    tip.innerHTML=`<div class="t">t = ${fmt(t,(tmax-tmin)<1?3:2)} ${sec.unit}</div>`+rows;
    tip.style.display='block';
    const hx=e.clientX-host.getBoundingClientRect().left;
    tip.style.left=Math.min(hx+14,host.clientWidth-170)+'px';
    tip.style.top=(e.clientY-host.getBoundingClientRect().top+12)+'px';
  });
  svg.addEventListener('mouseleave',()=>{tip.style.display='none';ch.setAttribute('opacity',0)});
}
function renderAll(){
  const root=document.getElementById('sections');root.innerHTML='';
  const zb=document.getElementById('zoombar');zb.innerHTML='';
  for(const sec of DATA.sections){
    const s=document.createElement('section');s.id=sec.id;
    s.innerHTML=`<h2>${sec.title}</h2><p class="note">${sec.note}</p><div class="scope"></div>`;
    root.appendChild(s);
    scope(s.querySelector('.scope'),sec);
    const a=document.createElement('a');a.href='#'+sec.id;a.textContent=sec.title.split(' — ')[0];
    zb.appendChild(a);
  }
}
renderAll();
new MutationObserver(renderAll).observe(document.documentElement,{attributes:true,attributeFilter:['data-theme']});
matchMedia('(prefers-color-scheme: dark)').addEventListener('change',renderAll);
</script>
'''

if __name__ == '__main__':
    main()
