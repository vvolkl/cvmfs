# Legacy Signature Compatibility and SHA-256 Transition

## Status

**Proposal.** This document specifies a migration that removes the bundled
LibreSSL dependency from the normal crypto path without invalidating existing
repositories or clients.  It intentionally keeps the legacy signature format
for as long as old clients consume it.

## Problem statement

The current repository bootstrap format uses a detached, binary signature at
the end of `.cvmfspublished`.  `Publisher::PushManifest()` constructs it as:

```text
<manifest payload>
--
<hash of manifest payload, in hexadecimal>
<binary signature>
```

`SignatureManager::Sign()` signs the ASCII hexadecimal hash with the legacy
OpenSSL `EVP_Sign*` API and `EVP_sha1()`.  `VerifyLetter()` verifies the same
format.  The format has no signature-algorithm field and its trailing data is
one opaque signature blob.

System OpenSSL 3 can compile this code, but distribution crypto policy can
reject RSA/SHA-1 signing or verification.  In particular, CernVM-FS must not
require an EL9 `DEFAULT:SHA1` exception or an EL `LEGACY` policy to publish or
consume repositories.

At the same time, an unmodified old client only fetches and understands
`.cvmfspublished` and `.cvmfswhitelist`.  It needs a valid legacy signature on
every new revision that it is expected to consume.

## Goals

1. Preserve byte-level compatibility for old clients and existing
   repositories.
2. Let new clients authenticate a repository revision with a SHA-256
   signature, without performing the legacy manifest SHA-1 verification.
3. Keep X.509, PEM, CRL, PKCS#7, and TLS handling on one system OpenSSL 3.
4. Limit the policy-bypassing legacy implementation to a small, explicit
   compatibility backend.
5. Make downgrade, replay, and mixed-revision behavior testable and explicit.

## Non-goals

- FIPS certification.  A client that uses a Nettle legacy backend is not a
  FIPS-only crypto configuration.
- Removing legacy signatures while old clients must read current repository
  revisions.
- Changing the meaning of existing manifest or whitelist files.
- Making a host-wide crypto-policy exception on behalf of the administrator.

## Compatibility constraints

### A second signature cannot be appended to the current trailer

Appending a second signature after the legacy signature is not compatible:
`VerifyLetter()` passes all bytes after `--` to the RSA verifier as one
signature.  Extra bytes make the old verification fail.

It is, however, possible to put a SHA-256 signature *before* the existing
`--` delimiter.  The legacy SHA-1 signature then remains last and signs the
whole extended payload.  The SHA-256 signature signs only the original
manifest payload, so there is no circular dependency.  The extension must be
encoded as one otherwise-ignored line; it cannot contain arbitrary binary data
or additional key-value lines that an old parser might interpret.

### Old clients require v1 publication

A repository can be v2-only only if it intentionally drops old clients.  For
an existing repository, or a new repository that must support old clients,
every published revision continues to require:

- a legacy `.cvmfspublished` file with its RSA/SHA-1 signature; and
- the legacy whitelist representation expected by old clients.

The v1 signature is a compatibility artifact.  SHA-256 does not remove it
until the old-client support commitment ends.

## Proposed protocol: in-band v2 extension

Extend each existing bootstrap file before its existing legacy delimiter:

```text
<existing manifest or whitelist payload>\n
===<base64url-encoded v2 record>\n
--\n
<legacy payload hash>\n
<legacy binary SHA-1 signature>
```

`===` is not a second legacy delimiter.  It is the prefix of one extension
line.  Old parsers see its first character, `=`, as an unknown key and ignore
it.  The v2 record must be base64url encoded without line wrapping, so it
cannot contain `\n--\n`, create another line, or accidentally overwrite a
known one-character manifest key.

The legacy payload hash and its SHA-1 signature are calculated over everything
before `--`, including the extension line.  Consequently, an old client sees a
normal, validly signed v1 object.  A new client extracts the original payload
and the v2 record before considering the legacy trailer.

### v2 manifest record

The v2 record is a canonical binary or text envelope carried on the extension
line.  It has a version, an explicit algorithm identifier, and one or more
signature records.  Their signed input is domain-separated and binds the exact
original manifest payload, excluding the extension line.  For example:

```text
"cvmfs manifest signature v2\\0" || algorithm || length(payload) || payload
```

The payload already contains the repository name, revision, catalog root, and
publisher-certificate hash; signing its exact bytes binds all of them.  The
initial implementation should support `rsa-pss-sha256`; a v2 parser must
reject unknown algorithms rather than downgrade.

A v2 client obtains the publisher certificate named in the original payload,
checks it against the v2 whitelist, and verifies the v2 record.  It can then
trust the payload without invoking the legacy manifest SHA-1 verifier.

### v2 whitelist record

Use the same extension construction for `.cvmfswhitelist`.  The v2 record
binds the exact original whitelist payload and is signed by an existing
configured master key with a permitted v2 SHA-256 signature suite.  A v2
client verifies the
record with its locally configured master public keys, then uses the bound
whitelist payload to authorize the publisher certificate.

This preserves the current trust hierarchy while letting a v2 client avoid
both legacy bootstrap signature verifiers.  Old clients ignore the extension
line and validate the legacy trailer as before.

The exact v2 record syntax, field order, line endings, base64 variant, maximum
sizes, and duplicate-field behavior must be defined in a separate format
document before implementation.  Parsers must reject invalid encoding,
unsupported algorithms, embedded NULs, and non-canonical records.

### Required-v2 configuration and downgrade resistance

A new client cannot safely infer that v2 is mandatory from the fetched object:
an attacker can replay an older, valid v1 object from before the extension was
introduced.  Add an out-of-band client setting, for example:

```text
CVMFS_REQUIRE_SIGNATURE_V2=yes
```

For repositories configured this way, a client fails closed if the v2 extension
is missing, malformed, or fails verification.  It must not fall back to the v1
signature.  The legacy signature also covers the extension, so an attacker
cannot remove or alter it from a current object without invalidating v1; the
required setting protects against replay of an old pre-v2 object.

Before this setting is enabled, new clients may support v1-only repositories
for compatibility.  That mode is explicitly legacy-capable and is not suitable
for a policy-restricted installation.

## Signature agility and key rotation

The v2 extension is the format boundary for future signature changes.  It
should be extensible, but it must not implement algorithm agility as "accept
any signature that happens to verify".

### Protected record

Define a canonical, length-delimited v2 record with:

```text
magic and format version
object type                 manifest or whitelist
signature suite identifier  for example rsa-pss-sha256
signer key identifier       SHA-256 of the signer's SubjectPublicKeyInfo
payload length and exact payload bytes
signature bytes
```

The to-be-signed bytes must include every protected field, including the
format version, object type, suite identifier, key identifier, payload length,
and payload.  Prefix them with a fixed domain-separation string such as
`cvmfs signature v2`.  Thus, a signature for a manifest cannot be replayed as
a whitelist signature, and an attacker cannot relabel a signature as a weaker
algorithm.

Use a strictly specified binary encoding, then base64url encode that record on
the `===` line.  It must have unambiguous lengths, bounded allocation sizes,
and one canonical serialization.  Do not sign a casually serialized JSON
object or reconstruct data from parsed key-value fields.

### Multiple signatures

The extension format should allow a canonical list of independently protected
signature records over the same payload.  The first v2 publisher may emit one
record, but a later publisher can emit both the current suite and a replacement
suite during a transition.  This supports key and algorithm rotation without
another change to the legacy file framing.

A verifier has a configured signature profile: accepted suites, key-size or
curve requirements, trusted key rules, and a minimum format version.  It
accepts a record only when that record satisfies its profile.  It must not
silently fall back to the v1 SHA-1 trailer, an unknown suite, or a weaker v2
record merely because that record verifies.  Unknown non-critical records may
be skipped only when another record satisfying the profile is present;
unknown critical record types fail verification.

For the first v2 suite, prefer `rsa-pss-sha256` with explicit PSS parameters
when the existing publisher key permits it.  It is modern OpenSSL 3 provider
functionality and has no old-client interoperability requirement.  If a
transitional `rsa-pkcs1v15-sha256` suite is needed to reuse constrained keys,
it must have a distinct identifier and must not be confused with PSS.

### Rotation procedure

1. Add the new public key or certificate fingerprint to the authenticated v2
   whitelist while retaining the old one.
2. Publish v2 records signed by both the old and new accepted keys or suites.
3. Update client signature profiles to require the new suite or key.
4. After the support window, remove the old v2 record and authorization.

This does not make an unmodified client understand a new algorithm.  It makes
future migrations additive for clients that understand the v2 container, while
the configured minimum profile prevents downgrade to an old valid signature.

## Publication order and consistency

A dual-format publish transaction creates each original payload, adds its v2
extension, and finally creates the existing legacy hash and signature.  No
extra fetch path or sidecar cache invalidation is needed.

Publish catalog, certificate, history, and other content-addressed objects
first.  Publish the extended whitelist before the extended manifest, then
publish `.cvmfspublished` last as the compatibility-visible commit point.
Both v1 and v2 verification apply to the same object, so a client cannot
obtain a mixed manifest/extension pair.  Rollback republishes matching
extended whitelist and manifest revisions together.

## Crypto implementation split

### System OpenSSL 3

Keep the following functionality in system OpenSSL:

- PEM key and certificate parsing and serialization;
- X.509 certificate stores, CRLs, and chain verification;
- PKCS#7 parsing and verification;
- certificate generation, using SHA-256;
- v2 RSA/SHA-256 signing and verification through provider-aware EVP APIs;
- the OpenSSL-based curl TLS backend; and
- error reporting and BIO handling.

OpenSSL policy can still reject a legacy SHA-1-signed certificate or CMS
object during X.509/PKCS#7 validation.  This proposal does not bypass such
validation.

### Nettle/hogweed compatibility backend

The current vendored Nettle build contains symmetric and hash primitives only.
A legacy RSA backend requires the Nettle public-key library (hogweed) and GMP,
or an equivalently maintained build of those dependencies.

The backend is responsible only for legacy RSA operations:

- `SignatureManager::Sign()` and `SignatureManager::Verify()` for v1
  RSA/SHA-1 manifest and letter signatures; and
- preferably `SignRsa()` and `VerifyRsa()` for the legacy raw PKCS#1-v1.5
  whitelist format.

It must use Nettle's RSA and PKCS#1 APIs, including a timing-resistant/blinded
private-key operation.  It must not implement RSA padding, DER decoding, or
modular arithmetic in CernVM-FS.

For v1 manifests, the backend computes SHA-1 over the same ASCII hexadecimal
manifest hash that the current `EVP_Sign*` code receives, performs standard
RSASSA-PKCS1-v1_5 signing, and serializes the signature to exactly the RSA
modulus width.  Compatibility tests must prove byte-for-byte identity with
existing signatures for fixed keys and inputs.

For the raw whitelist operation, use Nettle's generic PKCS#1 signature
primitive to reproduce the existing type-1 padded input.  Do not substitute
an ordinary RSA/SHA-256 signature: old whitelist clients expect the old bytes.

### Key-type contract

`SignatureManager::Sign()` currently accepts an `EVP_PKEY`, so it can in
principle sign with an RSA, ECDSA, or DSA certificate.  The minimal Nettle
backend is RSA-only.

CernVM-FS-generated publisher certificates are RSA, but externally supplied
keys might not be.  Before switching the default backend:

1. inventory deployed publisher certificate key types;
2. define RSA as the supported legacy-v1 key type, if the inventory permits;
3. reject a non-RSA legacy key with a precise diagnostic; or
4. retain an alternative backend for any deployed non-RSA repositories.

Do not silently attempt RSA verification with a non-RSA certificate.  The v1
format has no independent algorithm field; the certificate key type determines
how an old client interprets the signature.

## Code-level plan

1. Introduce explicit APIs rather than a generic `Sign()` operation:

   ```text
   SignManifestV1RsaSha1 / VerifyManifestV1RsaSha1
   SignWhitelistV1RawRsa / VerifyWhitelistV1RawRsa
   SignV2RsaSha256       / VerifyV2RsaSha256
   ```

   Keep their wire-format ownership at the manifest and whitelist layers.

2. Implement a small OpenSSL-to-Nettle RSA-key adapter.  OpenSSL parses PEM
   and X.509; the adapter extracts RSA parameters and initializes temporary
   Nettle key objects.  Prefer provider-aware OpenSSL parameter access for new
   code.  Cache only if profiling justifies it, and securely clear private
   material on destruction.

3. Implement v1 operations with Nettle/hogweed.  Preserve the existing
   OpenSSL implementation behind a temporary build option only for comparison
   and migration testing; it is not the portable production fallback.

4. Implement provider-aware OpenSSL 3 v2 signing with SHA-256.  Use
   `EVP_DigestSign*` and `EVP_DigestVerify*`, not the legacy `EVP_Sign*`
   interface.

5. Add v2 manifest and whitelist extension parsers, serializers, publication,
   and required-v2 configuration.  No additional bootstrap fetch path is
   required.

6. Remove the bundled LibreSSL link from `cvmfs_crypto` only after v1 Nettle
   compatibility tests and v2 OpenSSL tests pass on the supported platforms.

## Rollout phases

### Phase 0: inventory and fixtures

Collect representative public certificates and manifests from supported
repositories.  Identify non-RSA publisher certificates and retain fixed,
versioned v1 manifest/whitelist fixtures.

### Phase 1: internal crypto split

Add hogweed/GMP and the Nettle v1 backend.  Keep the external file formats
unchanged.  Verify that legacy fixtures and newly generated fixed-key fixtures
match the prior implementation exactly.

### Phase 2: v2 reader

Ship a client capable of parsing and verifying the in-band v2 extension, but
leave v1 fallback enabled by default.  Add the explicit required-v2 setting.

### Phase 3: dual-format publisher

Publish a v2 extension and the existing v1 trailer in each bootstrap file for
opt-in repositories.  Existing clients continue to consume and verify the v1
trailer.  Enable required-v2 only after all target clients have been upgraded.

### Phase 4: hardened clients

Policy-restricted deployments set required-v2 and use OpenSSL SHA-256
verification for the bootstrap chain.  Legacy clients and ordinary compatible
deployments continue to receive v1 artifacts.

### Phase 5: retirement

Only after the old-client support commitment ends may a repository become
v2-only and remove SHA-1 signing.  This is intentionally not part of the
near-term plan.

## Test plan

- Unit-test v1 RSA/SHA-1 and raw-whitelist output against fixed legacy bytes.
- Verify existing historical manifest and whitelist fixtures with the Nettle
  backend.
- Verify that system OpenSSL SHA-256 v2 signatures are accepted on EL8, EL9,
  EL10, Debian 11, Debian 12, and Debian 13 default configurations.
- Verify a dual-format object with an old client: it ignores the extension and
  accepts the legacy trailer.
- Verify required-v2 clients reject a missing, malformed, replayed, or
  algorithm-downgraded extension.
- Test that changing or removing an extension invalidates the legacy trailer,
  and that a replayed pre-v2 object is rejected in required-v2 mode.
- Run the signature suite with an OpenSSL configuration that rejects SHA-1
  signatures; v2 must pass and legacy v1 must use only the explicit Nettle
  backend.
- Test RSA, RSA-PSS, ECDSA, and DSA input certificates and ensure unsupported
  legacy key types fail clearly rather than being misinterpreted.

## Security and operational notes

The Nettle v1 backend is an intentional compatibility exception.  It must be
visible in build documentation and diagnostics, and it must not be described
as FIPS operation.  A strict deployment can require v2 and avoid invoking the
backend for repository bootstrap verification.

No application code should set `OPENSSL_CONF`, alter the host's global crypto
policy, or load OpenSSL's legacy provider merely to make v1 work.  Those
choices affect unrelated TLS behavior and are not portable across supported
distributions.
