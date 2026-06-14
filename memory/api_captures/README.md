# API Capture Archive

Raw Omada API request/response captures, one file per **endpoint**. Kept verbatim for
reference and reverse-engineering.

**Conventions:**
- Filename = the endpoint (path tail), e.g. `sites-dst.md`, `vouchers-statistics-history.md`.
- All identifiers **redacted** — controller/site/device IDs, MAC, SN, IP, rate-limit/portal IDs,
  voucher codes, session tokens (`{REDACTED-*}` / `{siteId}` etc.). This folder is committed to git.
- Newest capture at the top of each file; keep prior ones below.

**Standing instruction:** every API request/response shared during a session gets archived
here, redacted, in the file for its endpoint (create the file if new).

## Full catalog (OpenAPI 3.1 + Postman)

`*.openapi.yaml` / `*.postman.json`, one pair per host, generated from sanitized Chrome HAR
exports. The big one — `use1-api-omada-controller-connector-*.openapi.yaml` — is the full
controller API (~144 operations). Schemas are **inferred types only** (no captured values);
IDs are templated (`{controllerId}`, `{siteId}`, `{groupId}`, `{clientId}`, `{id}`, `{mac}`).

**Regenerate:** in Chrome DevTools → Network, tick *Preserve log*, filter *Fetch/XHR*, click
through the UI, *Export HAR (sanitized)*, then:

```
python3 test/har_to_catalog.py ~/Downloads/hotspot.har ~/Downloads/admin.har
```

The `.md` per-endpoint files above are the hand-curated, deeper notes; the OpenAPI is the
machine-readable breadth.
