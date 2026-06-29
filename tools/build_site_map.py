#!/usr/bin/env python3
"""
Build a captive-page AP coverage map (map.svg) for a site.

Input is a Google My Maps share/edit URL *or* an exported .kml file. Each AP placemark must carry its
Omada MAC in the description; placemarks with no MAC (e.g. survey-only pins) are ignored. The MAC is
baked into each dot as data-mac, so the firmware/page match live status by MAC — no other config.

Geometry (coastline / roads / buildings) is traced from OpenStreetMap (Overpass), projected, cropped
to the AP cluster, and thinned. Output: maps/<site>.svg

Usage:
  python3 test/build_site_map.py "https://www.google.com/maps/d/edit?mid=...."
  python3 test/build_site_map.py path/to/site.kml
  python3 test/build_site_map.py <input> --out maps/bakhaw.svg
"""
import sys, os, re, math, argparse, xml.etree.ElementTree as ET
import requests
from urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

UA = "EllaFi-map/1.0 (+https://github.com/paulkilroy/EllaFi)"
MAC_RE = re.compile(r'[0-9A-Fa-f]{2}([-:][0-9A-Fa-f]{2}){5}')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load_kml(src):
    """Return (site_name, kml_text) from a My Maps URL/mid or a local .kml path."""
    if src.endswith(".kml") and os.path.exists(src):
        return os.path.splitext(os.path.basename(src))[0], open(src, encoding="utf-8").read()
    m = re.search(r'mid=([A-Za-z0-9_\-]+)', src) or re.match(r'^([A-Za-z0-9_\-]{20,})$', src)
    if not m:
        sys.exit("input is neither a .kml file nor a My Maps URL/mid")
    mid = m.group(1)
    url = f"https://www.google.com/maps/d/kml?mid={mid}&forcekml=1"
    r = requests.get(url, headers={"User-Agent": UA}, timeout=30)
    if r.status_code != 200:
        sys.exit(f"KML fetch failed (HTTP {r.status_code}). Is the map link-viewable?")
    return None, r.text

def parse_placemarks(kml):
    """Parse KML → (site_name, [ {name, mac, lat, lon} ]). Only placemarks with a MAC kept."""
    kml = re.sub(r'\sxmlns="[^"]+"', '', kml, count=1)   # drop default ns for simple tag access
    root = ET.fromstring(kml)
    site = root.findtext(".//Document/name")
    aps = []
    for pm in root.iter("Placemark"):
        name = (pm.findtext("name") or "").strip()
        desc = (pm.findtext("description") or "")
        coord = pm.findtext(".//coordinates")
        mac = MAC_RE.search(desc) or MAC_RE.search(name)
        if not mac or not coord:
            continue
        lon, lat = map(float, coord.strip().split(",")[:2])
        aps.append({"name": name, "mac": mac.group(0).upper(), "lat": lat, "lon": lon})
    return site, aps

OVERPASS_MIRRORS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
]
def overpass(aps, pad=0.0016):
    lats = [a["lat"] for a in aps]; lons = [a["lon"] for a in aps]
    bb = f"{min(lats)-pad},{min(lons)-pad},{max(lats)+pad},{max(lons)+pad}"
    q = (f'[out:json][timeout:30];('
         f'way["highway"]({bb});way["natural"]({bb});way["building"]({bb});'
         f');out geom;')
    last = None
    for url in OVERPASS_MIRRORS:
        try:
            r = requests.post(url, data={"data": q}, headers={"User-Agent": UA}, timeout=60)
            r.raise_for_status()
            return r.json().get("elements", [])
        except Exception as e:
            last = e
            print(f"  overpass mirror failed ({url.split('/')[2]}): {e} — trying next")
    raise SystemExit(f"all Overpass mirrors failed: {last}")

def rdp(pts, eps):
    if len(pts) < 3: return pts
    p0, pn = pts[0], pts[-1]; dx, dy = pn[0]-p0[0], pn[1]-p0[1]; L = math.hypot(dx, dy) or 1
    dmax = idx = 0
    for i in range(1, len(pts)-1):
        t = ((pts[i][0]-p0[0])*dx + (pts[i][1]-p0[1])*dy)/(L*L)
        d = math.hypot(pts[i][0]-(p0[0]+t*dx), pts[i][1]-(p0[1]+t*dy))
        if d > dmax: dmax, idx = d, i
    return rdp(pts[:idx+1], eps)[:-1] + rdp(pts[idx:], eps) if dmax > eps else [p0, pn]

def build_svg(aps, els):
    geom = lambda e: [(p["lon"], p["lat"]) for p in e.get("geometry", [])]
    coast = [geom(e) for e in els if e.get("tags", {}).get("natural") == "coastline"]
    roads = [geom(e) for e in els if "highway" in e.get("tags", {})]
    builds = [geom(e) for e in els if "building" in e.get("tags", {})]
    lat = [a["lat"] for a in aps]; lon = [a["lon"] for a in aps]
    clat, clon = (min(lat)+max(lat))/2, (min(lon)+max(lon))/2
    half_lat = (max(lat)-min(lat))/2 * 1.35
    k = math.cos(math.radians(clat)); half_lon = (half_lat*110900*0.72)/(111320*k)
    latmax, lonmin = clat+half_lat, clon-half_lon
    W = 2*half_lon*k; H = 2*half_lat; SCALE = 300/max(W, H)
    VW, VH, PAD = W*SCALE, H*SCALE, 12
    S = lambda lon, lat: ((lon-lonmin)*k*SCALE, (latmax-lat)*SCALE)
    inbox = lambda p: -PAD <= p[0] <= VW+PAD and -PAD <= p[1] <= VH+PAD
    proj = lambda g: [S(*q) for q in g]
    fmt = lambda pts, c=False: ("M"+" L".join(f"{x:.0f},{y:.0f}" for x, y in pts)+(" Z" if c else "")) if pts else ""

    water = coastline = ""
    cp = proj(coast[0]) if coast else []
    ct = rdp([p for p in cp if inbox(p)], 0.8)
    bvis = [proj(g) for g in builds if any(inbox(S(*q)) for q in g) and len(g) > 2]
    rvis = [proj(g) for g in roads if any(inbox(S(*q)) for q in g)]
    if ct and bvis:
        bp = [p for g in bvis for p in g]; cx = sum(p[0] for p in bp)/len(bp); cy = sum(p[1] for p in bp)/len(bp)
        p0, pn = ct[0], ct[-1]; dx, dy = pn[0]-p0[0], pn[1]-p0[1]; L = math.hypot(dx, dy) or 1
        nx, ny = -dy/L, dx/L; ls = 1 if ((cx-p0[0])*nx + (cy-p0[1])*ny) > 0 else -1
        wx, wy = -nx*ls*3000, -ny*ls*3000
        poly = ct + [(pn[0]+wx, pn[1]+wy), (p0[0]+wx, p0[1]+wy)]
        water = '<path d="M' + " L".join(f"{x:.0f},{y:.0f}" for x, y in poly) + ' Z" fill="#16242e"/>'
        coastline = f'<path d="{fmt(ct)}" fill="none" stroke="#3f6177" stroke-width="1.8"/>'
    bsvg = "".join(f'<path d="{fmt(g,True)}" fill="#2c3741" stroke="#37444f" stroke-width="0.4"/>' for g in bvis)
    rsvg = "".join(f'<path d="{fmt(g)}" fill="none" stroke="#5a6876" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>' for g in rvis)
    dots = "".join(f'\n  <circle class="dot" data-mac="{a["mac"]}" data-name="{a["name"]}" '
                   f'cx="{S(a["lon"],a["lat"])[0]:.0f}" cy="{S(a["lon"],a["lat"])[1]:.0f}" r="7"/>' for a in aps)
    return (f'<!-- coverage map — OpenStreetMap geometry + AP placement. data-mac = live Omada AP MAC. -->\n'
            f'<svg viewBox="0 0 {VW:.0f} {VH:.0f}" xmlns="http://www.w3.org/2000/svg">\n'
            f'  <rect width="{VW:.0f}" height="{VH:.0f}" fill="#222b22"/>\n  {water}{coastline}{bsvg}{rsvg}{dots}\n</svg>\n')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", help="My Maps URL/mid or .kml file")
    ap.add_argument("--out", help="output svg path (default maps/<site>.svg)")
    a = ap.parse_args()

    name_hint, kml = load_kml(a.input)
    site, aps = parse_placemarks(kml)
    site = site or name_hint or "site"
    if not aps:
        sys.exit("no APs with a MAC in their description were found")
    print(f"site: {site}  —  {len(aps)} AP(s) with MACs:")
    for x in aps: print(f"  {x['name']:16} {x['mac']}  ({x['lat']:.6f},{x['lon']:.6f})")

    print("querying OpenStreetMap (Overpass)…")
    els = overpass(aps)
    svg = build_svg(aps, els)
    out = a.out or os.path.join(ROOT, "data", "map.svg")   # data/ is the LittleFS source → flashed as /map.svg
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w").write(svg)
    print(f"wrote {out}  ({len(svg)/1024:.1f} KB)")

if __name__ == "__main__":
    main()
