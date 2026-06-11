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
