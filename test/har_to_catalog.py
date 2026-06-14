#!/usr/bin/env python3
"""
Turn Chrome DevTools HAR exports into a redacted OpenAPI 3.1 spec + Postman collection
(one pair per host), written to memory/api_captures/.

Usage:
    # In Chrome DevTools → Network: tick "Preserve log", filter Fetch/XHR, click around,
    # then "Export HAR (sanitized)". Then:
    python3 test/har_to_catalog.py ~/Downloads/hotspot.har ~/Downloads/admin.har

Redaction (so the output is safe to commit):
  - schemas are inferred TYPES only — no captured values ever land in the spec
  - path IDs templated: {controllerId} {siteId} {groupId} {clientId} {id} {mac}
  - dicts keyed by IDs become additionalProperties; stray ID/MAC keys are masked
A sanitized HAR already strips cookies/auth; this protects against IDs in URLs/payloads.
Always eyeball the diff before committing.
"""
import json, re, sys, os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "memory", "api_captures")

_IDK  = re.compile(r'^[0-9a-fA-F]{16,}$')
_MACK = re.compile(r'^[0-9A-Fa-f]{2}([-:][0-9A-Fa-f]{2}){5}$')
def _idlike(k): return bool(_IDK.match(k) or _MACK.match(k) or (k.isdigit() and len(k) >= 6))
def _safekey(k): return '{id}' if _IDK.match(k) else '{mac}' if _MACK.match(k) else k

def jschema(v, d=0):
    if d > 6: return {}
    if isinstance(v, dict):
        keys = list(v.keys())
        if keys and all(_idlike(k) for k in keys):           # map keyed by IDs (e.g. {"<siteId>": {...}})
            return {"type": "object", "additionalProperties": jschema(v[keys[0]], d+1)}
        return {"type": "object",
                "properties": {_safekey(k): jschema(val, d+1) for k, val in list(v.items())[:50]}}
    if isinstance(v, list):
        return {"type": "array", "items": jschema(v[0], d+1) if v else {}}
    if isinstance(v, bool):  return {"type": "boolean"}
    if isinstance(v, int):   return {"type": "integer"}
    if isinstance(v, float): return {"type": "number"}
    if v is None:            return {"type": "null"}
    return {"type": "string"}   # never the real value → redacted

def sample(sch):
    t = sch.get("type")
    if t == "object":  return {k: sample(s) for k, s in sch.get("properties", {}).items()}
    if t == "array":   return [sample(sch["items"])] if sch.get("items") else []
    if t == "integer": return 0
    if t == "number":  return 0
    if t == "boolean": return False
    if t == "null":    return None
    return ""

def templatize(path):
    path = re.sub(r'[0-9A-Fa-f]{2}(?:[-:][0-9A-Fa-f]{2}){5}', '{mac}', path)            # MACs
    path = re.sub(r'/sites/[0-9a-fA-F]{16,}', '/sites/{siteId}', path)
    path = re.sub(r'/voucherGroups/[0-9a-fA-F]{16,}', '/voucherGroups/{groupId}', path)
    path = re.sub(r'/clients/[0-9a-fA-F]{16,}', '/clients/{clientId}', path)
    path = re.sub(r'^/[0-9a-fA-F]{16,}(?=/)', '/{controllerId}', path)
    path = re.sub(r'[0-9a-fA-F]{16,}', '{id}', path)                                    # catch-all hex
    seen = {}                                                                           # unique param names
    def uniq(m):
        n = m.group(1); seen[n] = seen.get(n, 0) + 1
        return '{' + (n if seen[n] == 1 else f'{n}{seen[n]}') + '}'
    return re.sub(r'\{(\w+)\}', uniq, path)

from urllib.parse import urlparse, parse_qs
SKIP = ('.js','.css','.png','.jpg','.jpeg','.svg','.woff','.woff2','.ico','.gif','.map','.html')
eps = {}
for f in sys.argv[1:]:
    har = json.load(open(f))
    for e in har['log']['entries']:
        req, resp = e['request'], e['response']
        u = urlparse(req['url'])
        rtype = e.get('_resourceType', '')
        mime  = (resp.get('content') or {}).get('mimeType', '') or ''
        if rtype not in ('fetch','xhr') and 'json' not in mime and '/api/' not in u.path: continue
        if any(u.path.lower().endswith(x) for x in SKIP): continue
        m = eps.setdefault(u.netloc, {}).setdefault(templatize(u.path), {}).setdefault(
            req['method'], {'query': set(), 'req': None, 'resp': {}})
        for q in parse_qs(u.query): m['query'].add(q)
        pd = req.get('postData') or {}
        if pd.get('text'):
            try: m['req'] = jschema(json.loads(pd['text']))
            except Exception: pass
        body = (resp.get('content') or {}).get('text', '') or ''
        st = str(resp.get('status'))
        try: m['resp'][st] = jschema(json.loads(body))
        except Exception: m['resp'].setdefault(st, None)

def slug(h): return re.sub(r'[^a-z0-9]+', '-', h.lower()).strip('-')

os.makedirs(OUT, exist_ok=True)
try:    import yaml; HAVE_YAML = True
except Exception: HAVE_YAML = False

written = []
for host, paths in eps.items():
    spec = {"openapi": "3.1.0",
            "info": {"title": f"Captured API — {host}", "version": "1.0",
                     "description": "Auto-generated from a sanitized HAR. Schemas are inferred (types only — no captured values). IDs templated."},
            "servers": [{"url": f"https://{host}"}], "paths": {}}
    pm = {"info": {"name": f"Captured — {host}",
                   "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json"},
          "variable": [{"key": "baseUrl", "value": f"https://{host}"}], "item": []}
    for path, methods in sorted(paths.items()):
        path_params = re.findall(r'\{(\w+)\}', path)
        item = {}
        for method, info in sorted(methods.items()):
            op = {"summary": f"{method} {path}", "responses": {}}
            params = [{"name": p, "in": "path", "required": True, "schema": {"type": "string"}} for p in path_params]
            params += [{"name": q, "in": "query", "required": False, "schema": {"type": "string"}} for q in sorted(info['query'])]
            if params: op["parameters"] = params
            if info['req'] is not None:
                op["requestBody"] = {"content": {"application/json": {"schema": info['req']}}}
            for st, sch in sorted(info['resp'].items()):
                r = {"description": "captured"}
                if sch is not None: r["content"] = {"application/json": {"schema": sch}}
                op["responses"][st] = r
            item[method.lower()] = op
            pm_path = [re.sub(r'\{(\w+)\}', r'{{\1}}', s) for s in path.strip('/').split('/')]
            entry = {"name": f"{method} {path}",
                     "request": {"method": method,
                                 "url": {"raw": "{{baseUrl}}/" + "/".join(pm_path),
                                         "host": ["{{baseUrl}}"], "path": pm_path,
                                         "query": [{"key": q, "value": ""} for q in sorted(info['query'])]}}}
            if info['req'] is not None:
                entry["request"]["header"] = [{"key": "Content-Type", "value": "application/json"}]
                entry["request"]["body"] = {"mode": "raw", "raw": json.dumps(sample(info['req']), indent=2),
                                            "options": {"raw": {"language": "json"}}}
            pm["item"].append(entry)
        spec["paths"][path] = item

    base = os.path.join(OUT, slug(host))
    if HAVE_YAML:
        open(base + ".openapi.yaml", "w").write(yaml.safe_dump(spec, sort_keys=False, width=120))
        written.append(base + ".openapi.yaml")
    else:
        open(base + ".openapi.json", "w").write(json.dumps(spec, indent=2))
        written.append(base + ".openapi.json")
    open(base + ".postman.json", "w").write(json.dumps(pm, indent=2))
    written.append(base + ".postman.json")
    print(f"{host}: {len(paths)} paths, {sum(len(m) for m in paths.values())} operations")

print("\nWrote:\n  " + "\n  ".join(written))
