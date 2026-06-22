# Spec & Implementation Plan: Bearer-Token Authenticated Repositories

**Status:** Draft / proposal
**Author:** (fill in)
**Date:** 2026-06-20
**Supersedes:** the x509/VOMS `authz/` subsystem (to be deprecated)

---

## 1. Summary

Replace the heavyweight x509/VOMS client-side authorization (`cvmfs/authz/`) with a
lightweight, **per-user OAuth2 bearer-token** scheme in which:

- **Access enforcement happens at the HTTP edge** (a token-validating reverse proxy
  in front of the Stratum-1 / Squid), not inside the FUSE client.
- **The cvmfs client only attaches a short-lived bearer token** to its HTTP requests,
  read from the standard WLCG Bearer Token Discovery location.
- **Per-user scope is achieved by running authenticated repositories in userspace via
  `cvmfsexec`**, so each mount belongs to exactly one uid in one namespace. This
  removes the need for any privileged, per-process credential interception.

The result deletes the entire `authz/` subsystem and its external-helper protocol,
leaving only a small, generic "attach `Authorization: Bearer` and retry on 401"
mechanism in the download layer.

---

## 2. Motivation

### Goals

- Retire `cvmfs/authz/` (x509 SSL client certs, VOMS membership, the authz-helper
  process protocol, the `AuthzSessionManager`/`AuthzExternalFetcher` stack).
- Support restricting access to repositories containing licensed software.
- **Per-user** access scope, with **short-lived** credentials (minutes–hours) and/or
  **easy revocation** (disable user at the IdP; access dies within the token TTL).
- Keep the client side minimal; push policy into infrastructure outside cvmfs.
- Align with where WLCG is going anyway (OAuth2/JWT tokens via Indigo IAM / SciTokens /
  the WLCG token profile, replacing VOMS proxies).

### Non-goals (the "not paranoid" trust model)

We are **not** defending against:

- An authorized user reading or copying out files they already have access to (inherent
  to any read filesystem).
- The cache/mirror operators (Stratum-1, Squid) — they are trusted infrastructure.
  Content is therefore **not** encrypted; blobs stay cacheable in plaintext.

We **are** ensuring that only holders of a valid, current token can *fetch the bytes*.
Edge token validation is proportionate to that goal.

Explicitly out of scope: content-at-rest encryption, defending cached blobs on a shared
local disk (see §6.4), and system-wide autofs mounting of authenticated repositories
(see §5.3).

---

## 3. Background: how authz works today

Relevant files:

- `cvmfs/network/download.h:104` — `CredentialsAttachment` abstract interface
  (`ConfigureCurlHandle(curl, pid, info_data)` / `ReleaseCurlHandle`).
- `cvmfs/authz/authz_curl.cc` — `AuthzAttachment`, the only implementation. Already
  knows how to build an `Authorization: Bearer <token>` header
  (`ConfigureSciTokenCurl`, `authz_curl.cc:107`) **and** how to install an x509 client
  cert/key into the TLS context (`CallbackSslCtx`, `authz_curl.cc:57`).
- `cvmfs/authz/authz_session.{cc,h}` — `AuthzSessionManager`: caches per-pid tokens,
  maps a calling process to its credentials.
- `cvmfs/authz/authz_fetch.{cc,h}` — `AuthzExternalFetcher`: spawns and speaks the
  helper protocol to an external `cvmfs_authz_*` binary.
- `cvmfs/mountpoint.cc:1283` `CreateAuthz()` / `cvmfs/mountpoint.cc:1906`
  `ReEvaluateAuthz()` — wires the helper from `CVMFS_AUTHZ_HELPER` /
  `CVMFS_AUTHZ_SEARCH_PATH`, reads the membership requirement from the catalog
  (`GetVOMSAuthz`, `cvmfs/catalog.cc:488`, stored as the `voms_authz` catalog property),
  and calls `download_mgr_->SetCredentialsAttachment(...)` (`mountpoint.cc:1365`).
- `cvmfs/network/download.cc:1107` — per-request, **only for `https://` URLs and when
  `info->pid() != -1`**, the download manager calls
  `credentials_attachment_->ConfigureCurlHandle(...)`. Released at
  `download.cc:1349`.

Why it is heavy: the **shared autofs mount** forces the client to discover *which user*
is behind each `open()` (per-pid token lookup, `/proc` inspection inside the helper), and
to manage SSL client certs and VOMS attribute validation. This is the machinery we remove.

---

## 4. Proposed design

### 4.1 Architecture

```
   ┌──────────────┐   token (OIDC/device flow / htgettoken / oidc-agent)
   │ Identity     │◄──────────────────────────────────────────────┐
   │ Provider     │                                                │
   │ (IAM/Keycloak)│──► JWKS / introspection ──┐                   │
   └──────────────┘                            │                   │
                                               ▼                   │
   user@node ──cvmfsexec──► cvmfs client ──HTTP+Bearer──► Edge auth proxy ──► Stratum-1
              (own namespace, own uid)     (Authorization:    (validates JWT:   (plain HTTP,
                                            Bearer <jwt>)       sig/aud/exp/scope) unchanged)
```

Three components, only one of which is cvmfs-specific:

1. **Identity Provider (token issuer).** Reuse an existing OIDC IdP (CERN SSO, EGI
   Check-in, an Indigo IAM instance) or self-host (Keycloak / Indigo IAM). Issues
   short-lived JWT access tokens with an audience/scope identifying the repo(s).

2. **Edge auth proxy** in front of the Stratum-1 / Squid. Validates the bearer token on
   every request (signature via cached JWKS; `aud`, `scope`, `exp`; optional
   introspection for instant revocation) and serves the otherwise-unmodified,
   **cacheable** content. Reference implementations: nginx `auth_request` + a JWT
   validator, `mod_auth_openidc`, OAuth2-Proxy, or Envoy `ext_authz`.

3. **cvmfs client token attachment** (this spec's code): read a bearer token from the
   WLCG discovery location, attach it, refresh it, retry on 401.

### 4.2 The cache invariant (must hold)

Validate the token **per request** but **never let it enter the cache key**. Gate by URL;
one cached blob serves all authorized users. Getting this wrong either breaks caching or
leaks content. Gate the whole repo URL space (`.cvmfspublished`, the signing certificate,
catalogs, and data chunks) so metadata does not leak.

### 4.3 Why per-user falls out for free

Under `cvmfsexec`, the mount is owned by a single uid in a user namespace. The token is
simply *that user's* token, read from their environment. There is no "which pid is
calling" problem, so `AuthzSessionManager`, the per-pid cache, and the helper protocol
all disappear. `ConfigureCurlHandle`'s `pid` argument becomes irrelevant for token auth.

### 4.4 Transport security & caching topology

A bearer token is a **plaintext, replayable secret** (no proof-of-possession): anyone who
can read the request gets the token and can replay it until `exp`. Therefore **every
network segment that carries the token must be TLS.** A short TTL shrinks the replay
window; it does not remove the requirement.

This is a real departure from cvmfs's status quo. Plaintext HTTP is safe for public repos
*only* because integrity comes from content-addressing plus the signed manifest, and the
content needs no confidentiality. Token auth adds a **confidentiality requirement on the
transport** that does not exist today. This cost is inherent to bearer tokens, not
something the design can engineer away while still using them.

#### TLS is hop-by-hop here, not opaque end-to-end

This is the distinction that lets token confidentiality coexist with caching. Because the
trust model treats **cache operators as trusted** (§2), an intermediary is *permitted* to
see the token — indeed it must, in order to validate it. So:

- A **TLS-terminating caching reverse proxy** terminates the client's TLS, reads and
  validates the token, serves the (plaintext, content-addressed) blob from its cache, and
  re-encrypts to the next hop.
- Caching still works because the proxy reads URLs — it is not a blind tunnel.
- The token is in cleartext only *inside* trusted processes; every wire is its own TLS
  session.

Requirement, precisely: *every network link carrying the token is TLS; trusted caches may
terminate it.* **Not** "end-to-end opaque TLS."

```
   client ──HTTPS──► TLS-terminating caching proxy ──(private net / TLS)──► Stratum-1
                     validates token; caches plaintext blobs keyed by URL
```

#### The trap: forward Squid + CONNECT tunneling

cvmfs's classic topology is a **forward** proxy (site Squid) the client talks plain HTTP
to. Doing HTTPS-to-origin *through* a forward Squid makes Squid open a `CONNECT` tunnel:
it blindly pipes the TLS bytes and therefore **can neither cache nor validate** — which
defeats the entire performance model.

```
   client ──HTTPS CONNECT tunnel──► forward Squid ──► origin   # BROKEN: no cache, no auth
```

Consequences for the design:

- Authenticated repos must **not** use transparent forward-proxy tunneling. They route
  through a TLS-terminating caching reverse proxy / edge (the diagram above).
- This is a deployment-topology constraint, documented for site admins (§7), not a client
  code change. The client already attaches the bearer header only on the `https://` branch
  (`download.cc:1097`), so the "never send a token over cleartext" invariant is preserved
  in code (§6.1).

#### Residual risk even with correct TLS

Because the token is a bearer credential, a leak by *any other* channel (a debug log, a
compromised trusted proxy, header truncation) is replayable for its TTL from anywhere.
Mitigations: short TTL, **audience binding** (token valid only for that repo's edge, so a
leak cannot be replayed elsewhere), and never logging token contents (§6.2). Stronger
sender-constrained tokens (DPoP or mTLS-bound) would close the replay hole entirely but
add client weight and are out of scope under the "not paranoid" model (§2).

---

## 5. User experience

### 5.1 Authenticated repo, per-user (the target)

```bash
# Once per session: obtain a token with standard WLCG/OIDC tooling.
# Token lands in the Bearer Token Discovery path
# ($BEARER_TOKEN, $BEARER_TOKEN_FILE, $XDG_RUNTIME_DIR/bt_u$UID, /tmp/bt_u$UID).
htgettoken -a iam.example.org -i licensed       # or: oidc-token, device flow, etc.

# Run inside the userspace mount; cvmfs runs as me and uses my token.
cvmfsexec licensed.example.org -- bash
ls /cvmfs/licensed.example.org                   # works
```

The user does nothing cvmfs-specific beyond launching under `cvmfsexec`. The token
refreshes in the background (re-read from the discovery path); cvmfs retries on 401.

### 5.2 Batch / HTCondor

HTCondor already ships bearer tokens into the job sandbox. The job runs its payload under
`cvmfsexec`; the token is in the environment. No cvmfs-specific batch integration needed.

### 5.3 Deliberate limitation

Authenticated repositories become **userspace-only**: there is no transparent
system-wide `/cvmfs/licensed…` visible to every process. This is the conscious trade —
no privileged code, in exchange for ubiquity. Public repositories keep normal autofs
mounting; authenticated ones require opt-in via `cvmfsexec` (or the user-namespace
client). For licensed software (specific workflows, not system-wide) this is acceptable.

---

## 6. cvmfs client changes

### 6.1 New `TokenCredentialsAttachment` (replaces `AuthzAttachment`)

Implement `CredentialsAttachment` (`download.h:104`) with a token-only backend:

- **`ConfigureCurlHandle(curl, pid, info_data)`**: obtain the current token (see §6.2),
  build `Authorization: Bearer <token>`, set `CURLOPT_HTTPHEADER`. Ignore `pid`.
  Reuse the header-building logic from `ConfigureSciTokenCurl` (`authz_curl.cc:107`);
  drop everything x509 (`CallbackSslCtx`, the `kTokenX509` branch, all OpenSSL code).
- **`ReleaseCurlHandle`**: free the header list (the `kTokenBearer` branch of the
  existing `ReleaseCurlHandle`, `authz_curl.cc:301`).
- Keep the existing `https`-only gate at `download.cc:1097` — a bearer token must never
  be sent over plaintext HTTP. (Decouple from `pid != -1`; see §6.5.)
- Thread-safety: the attachment may be shared across cloned `DownloadManager`s
  (`download.cc:3271`), so the token cache must be guarded by a lock.

New files (suggested): `cvmfs/authz_token/token_attachment.{cc,h}` (or repurpose
`cvmfs/authz/` slimmed down — see §6.6).

### 6.2 Token discovery & refresh

Implement WLCG Bearer Token Discovery resolution order:

1. `$BEARER_TOKEN` (raw token in env),
2. `$BEARER_TOKEN_FILE` (path),
3. `$XDG_RUNTIME_DIR/bt_u$UID`,
4. `/tmp/bt_u$UID`.

Config overrides:

- `CVMFS_AUTH_TOKEN_FILE` — explicit path, bypasses discovery.
- `CVMFS_AUTH_TOKEN_COMMAND` — optional helper command printing a token to stdout
  (covers non-standard IdPs and on-demand minting). This is a *thin* exec, not the old
  bidirectional helper protocol.

Caching/refresh: cache the token and its `exp` (decode the JWT `exp` claim, no signature
check needed client-side — the edge validates). Re-read from source when within a
configurable skew of expiry (`CVMFS_AUTH_TOKEN_REFRESH_MARGIN`, default e.g. 300s) or on
a 401 (§6.3). Never log token contents above debug; redact in `MagicXattr`/telemetry.

### 6.3 401/403 handling (retry-on-refresh)

Today HTTP status handling lives in `VerifyAndFinalize` (`download.cc:1437`) and
`CanRetry` (`download.cc:1277`); 5xx/400/404/429 are special-cased
(`download.cc:181`, `download.cc:187`). Add:

- On **401 Unauthorized** (and optionally 403): force a token re-read from source and
  retry the request once with the fresh token, bounded by the existing retry budget
  (`CanRetry`). If the refreshed token still yields 401/403 → fail with a clear,
  non-retried `EACCES`-style error so the user sees "not authorized / token expired,"
  not a generic network error.
- Make sure a 401 does **not** poison the host as "unreachable" in host-failover logic.

### 6.4 Local cache isolation (config, not blocker)

`cvmfsexec` mounts default to per-user caches → natural isolation, less dedup. A shared
cache is defensible under our trust model (a cached blob is not access; fetching still
needs a live token), but a local user could read another user's already-cached licensed
blobs off disk. Leave this to the existing cache-dir configuration; document the
trade-off. No code needed.

### 6.5 Decoupling the credential gate from `pid`

Currently credential attachment requires `info->pid() != -1` (`download.cc:1107`), a relic
of per-process authz. For token auth the token is process-wide. Introduce an explicit
"this manager uses token auth" flag set when a `TokenCredentialsAttachment` is installed,
and attach the header whenever that flag is set and the URL is `https`. Audit all callers
that pass `pid` to `DownloadManager::Fetch` to confirm no behavior depends on the old
gating.

### 6.6 Removal of the x509/VOMS authz subsystem

Delete or gut:

- `cvmfs/authz/authz_fetch.{cc,h}`, `authz_session.{cc,h}`, the x509 paths in
  `authz_curl.cc`, `helper_*.{cc,h}`, `helper_util.*`.
- `MountPoint::CreateAuthz` / `ReEvaluateAuthz` (`mountpoint.cc:1283`, `:1906`) and the
  `authz_fetcher_`/`authz_session_mgr_`/`authz_attachment_` members
  (`mountpoint.h:651`); replace with a `TokenCredentialsAttachment` wired in the same
  place that calls `SetCredentialsAttachment` (`mountpoint.cc:1365`).
- Config knobs `CVMFS_AUTHZ_HELPER`, `CVMFS_AUTHZ_SEARCH_PATH` (`mountpoint.cc:1286`):
  deprecate (warn-and-ignore) then remove.
- The CMake target(s) for the `cvmfs_authz_*` helper binaries and their packaging.

Keep, but rename conceptually: the catalog membership hint (`GetVOMSAuthz`,
`catalog.cc:488`, property `voms_authz`). See §7.

### 6.7 Server / publish side

- The `voms_authz` catalog property currently signals "this repo requires
  authorization." Keep it as an opaque **"authenticated" marker** (optionally carry the
  required token `scope`/`aud` so the client can pick the right token / give better error
  messages). Decide: retain the `voms_authz` key name for back-compat or add a new
  `auth_requirement` property and migrate.
- Publishing licensed content is otherwise unchanged — the bytes are published normally;
  protection is entirely at the edge.

---

## 7. Edge infrastructure (reference deployment)

Provide a documented reference config (in `doc/`), not cvmfs code:

- **IdP**: minimal Keycloak or Indigo IAM realm, one OIDC client per authenticated repo
  (or one client with per-repo scopes), short access-token TTL (e.g. 20 min).
- **Edge proxy** (e.g. nginx):
  - `auth_request` to a JWT-validation subrequest (cache JWKS; verify `aud` == repo,
    required `scope`, `exp`).
  - Optional token introspection for instant revocation.
  - **Cache key = URL only**; strip `Authorization` from the cache key; forward validated
    requests to the Stratum-1 origin.
  - **TLS-terminating caching reverse proxy**, not a transparent forward proxy — the token
    hop must be TLS and the cache must read URLs (see §4.4). Origin can stay plain HTTP on
    a private network.
- **Do not** route authenticated repos through a forward Squid in `CONNECT`-tunnel mode:
  it can neither cache nor validate (§4.4). Document the supported topology explicitly for
  site admins.
- Document the **bootstrap** case: a fully-gated repo needs a token present before first
  mount (fine for the `cvmfsexec` flow, where the user fetches a token first).

**Setup difficulty:** reuse-an-IdP + per-user ≈ a few days (mostly proxy + caching
tuning); self-hosted Keycloak ≈ ~1 week; full htgettoken/Vault/IAM stack ≈ medium-high,
but typically already present at WLCG sites.

---

## 8. Migration & deprecation plan

1. **Land token auth alongside authz** (both `CredentialsAttachment`s selectable). New
   code path off by default.
2. **Deprecation notices** on `CVMFS_AUTHZ_HELPER` / `CVMFS_AUTHZ_SEARCH_PATH` and the
   x509 path (one release).
3. **Switch default** for authenticated repos to token auth; provide a reference edge
   deployment and migration guide for existing x509-protected repos.
4. **Remove** the `authz/` x509/VOMS code and helper binaries (next major: cvmfs 2.x →
   note in release engineering).

No on-disk repository format change is required (the membership hint already exists).

---

## 9. Implementation phases

- **Phase 0 — Spike (small):** `TokenCredentialsAttachment` that attaches a token from a
  fixed `CVMFS_AUTH_TOKEN_FILE`; manual edge proxy; prove an authenticated `cvmfsexec`
  mount end-to-end. Validates the cache invariant and the `pid` decoupling (§6.5).
- **Phase 1 — Discovery & refresh:** WLCG discovery order, `exp`-based refresh,
  `CVMFS_AUTH_TOKEN_COMMAND` (§6.2).
- **Phase 2 — 401 retry & error surfacing:** integrate with `VerifyAndFinalize`/`CanRetry`
  (§6.3); clean `EACCES` on hard auth failure; host-failover safety.
- **Phase 3 — Wire into MountPoint:** replace `CreateAuthz`/`ReEvaluateAuthz`; read the
  authenticated marker from the catalog; deprecate old knobs (§6.6).
- **Phase 4 — Reference edge deployment + docs** (§7) and integration tests (§10).
- **Phase 5 — Remove x509/VOMS authz** and helper packaging (§8.4).

Each phase is independently compilable and atomic (per CLAUDE.md git workflow).

## 10. Testing

- **Unit:** token discovery resolution order; `exp` parsing & refresh-margin logic;
  header construction; 401 → refresh → retry → give-up state machine (mock download).
- **Integration (`test/src/`, 5xx server range):** stand up a token-validating proxy in
  front of a test Stratum-1; assert: (a) no token → denied; (b) valid token → served and
  cached; (c) second user with own token served from cache (cache invariant); (d) expired
  token → refresh path; (e) revoked user → denied within TTL; (f) plain-HTTP never carries
  a token (assert no `Authorization` header on any `http://` request, §4.4); (g) the token
  traverses only TLS hops and a `CONNECT`-tunnel forward proxy is rejected, not silently
  uncached (§4.4).
- **Micro-benchmark:** confirm per-request header attach adds negligible overhead vs.
  the deleted SSL-context path.

## 11. Open questions

1. **Catalog marker:** reuse `voms_authz` key or introduce `auth_requirement` (with
   `aud`/`scope`)? Affects back-compat and tooling.
2. **`CVMFS_AUTH_TOKEN_COMMAND`:** ship it, or insist on standard discovery only? (Trade
   flexibility vs. resurrecting a mini-helper.)
3. **403 vs 401:** do we retry-refresh on 403, or treat it strictly as "authenticated but
   not entitled" (no retry)?
4. **Non-`cvmfsexec` deployments:** do we support token auth on a privileged autofs mount
   at all (per-node/service token), or hard-restrict authenticated repos to userspace?
5. **Token size:** very large JWTs in a header vs. proxy/Squid header limits — cap and
   document.

## 12. Risks

- **Cache-key mistakes** at the edge (token in key) silently break caching or leak — make
  it the first integration test.
- **Wrong proxy topology** (forward Squid + `CONNECT` tunnel) silently disables caching
  and validation (§4.4); the token also cannot be protected without TLS on every hop. A
  misconfigured site degrades to no-cache or, worse, a cleartext token. Detect and refuse.
- **Token lifetime vs. mount lifetime:** mounts outlive tokens by far; the refresh path
  (§6.2/§6.3) is the correctness-critical piece.
- **Error legibility:** auth failures must not masquerade as network errors, or users
  will mis-debug. Surface a distinct, documented error.
- **Ecosystem dependency:** relies on sites having an OIDC IdP + edge proxy; mitigated by
  shipping a turnkey reference deployment.
```
