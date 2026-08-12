#!/usr/bin/env python3
"""One-command JLCPCB fabrication package. Run from kicad/:

    python3 export_jlc.py

Produces in fab/ (gitignored): EllaFi-gerbers.zip, EllaFi-bom-jlc.csv,
EllaFi-cpl-jlc.csv. BOM part numbers come from each symbol's LCSC / Mfg_PN
fields in the schematic — update those fields (not the CSV) when a part
changes, so every future order regenerates identically.
"""
import csv, re, zipfile, os, glob, subprocess, sys

KC = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'
HERE = os.path.dirname(os.path.abspath(__file__)) or '.'
os.chdir(HERE)
os.makedirs('fab', exist_ok=True)

# gerbers + drill + placement
subprocess.run([KC,'pcb','export','gerbers','--output','fab/gerbers/','EllaFi-PCB.kicad_pcb'],check=True,capture_output=True)
subprocess.run([KC,'pcb','export','drill','--output','fab/gerbers/','--format','excellon','--excellon-separate-th','EllaFi-PCB.kicad_pcb'],check=True,capture_output=True)
subprocess.run([KC,'pcb','export','pos','--output','fab/_pos.csv','--format','csv','--units','mm','--side','both','EllaFi-PCB.kicad_pcb'],check=True,capture_output=True)

with open('fab/EllaFi-cpl-jlc.csv','w',newline='') as f:
    w=csv.writer(f); w.writerow(['Designator','Mid X','Mid Y','Layer','Rotation'])
    for r in csv.DictReader(open('fab/_pos.csv')):
        w.writerow([r['Ref'], f"{float(r['PosX']):.3f}mm", f"{float(r['PosY']):.3f}mm",
                    'Top' if r['Side']=='top' else 'Bottom', r['Rot']])
os.remove('fab/_pos.csv')

# BOM from schematic fields (LCSC + Mfg_PN are the source of truth)
s=open('EllaFi-PCB.kicad_sch').read()
comps={}
for m in re.finditer(r'\(property "Reference" "([A-Z]+\d+)"',s):
    chunk=s[m.start():m.start()+5000]
    def g(p, chunk=chunk):
        mm=re.search(r'\(property "'+p+r'" "([^"]*)"',chunk)
        return mm.group(1) if mm else ''
    comps[m.group(1)]=(g('Value'), g('Mfg_PN'), g('Footprint').split(':')[-1], g('LCSC'))
groups={}
for ref,key in comps.items(): groups.setdefault(key,[]).append(ref)
missing=[]
with open('fab/EllaFi-bom-jlc.csv','w',newline='') as f:
    w=csv.writer(f); w.writerow(['Comment','Designator','Footprint','LCSC Part #'])
    for (val,mpn,fp,lcsc),refs in sorted(groups.items(), key=lambda x:x[1][0]):
        w.writerow([mpn or val, ','.join(sorted(refs)), fp, lcsc])
        if not lcsc: missing.append(','.join(sorted(refs)))

with zipfile.ZipFile('fab/EllaFi-gerbers.zip','w',zipfile.ZIP_DEFLATED) as z:
    for p in glob.glob('fab/gerbers/*'): z.write(p, os.path.basename(p))

print(f"fab/: gerbers.zip + BOM ({len(groups)} lines) + CPL ({len(comps)} placements)")
print(f"LCSC numbers missing for: {', '.join(missing) if missing else 'none — fully pinned'}")
