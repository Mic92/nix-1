# Plan: Google Cloud Storage (GCS) binary cache support

## Goal

Allow Nix to use Google Cloud Storage buckets as binary caches via a `gs://`
store URI, with read/write support and native GCP authentication (Application
Default Credentials, service accounts, GCE/GKE metadata server), mirroring the
existing `s3://` support.

## Architecture summary (existing S3 support, for reference)

- `src/libstore/s3-url.{cc,hh}` — parse `s3://bucket/key?region=...&endpoint=...`
  into `ParsedS3URL` and convert it to an HTTPS `ParsedURL`.
- `src/libstore/aws-creds.{cc,hh}` — wrap `aws-crt-cpp` to resolve AWS
  credentials (profile chain, IMDS, SSO, etc.). Guarded by `NIX_WITH_AWS_AUTH`.
- `src/libstore/filetransfer.cc` — `FileTransferRequest::setupForS3()` rewrites
  `s3://` → `https://` and configures libcurl AWS SigV4 signing
  (`CURLOPT_AWS_SIGV4`). Also parses S3 XML error bodies for retry.
- `src/libstore/s3-binary-cache-store.{cc,hh}` — `S3BinaryCacheStoreConfig` /
  `S3BinaryCacheStore` extend `HttpBinaryCacheStore` and add multipart upload,
  storage class, compression headers.
- Build: `meson.options` `s3-aws-auth`, `src/libstore/meson.build` pulls in
  `aws-crt-cpp` and sets `NIX_WITH_AWS_AUTH`.
- Tests: `src/libstore-tests/{s3-url,s3-binary-cache-store}.cc`,
  `tests/nixos/s3-binary-cache-store.nix`.

GCS support follows the same shape but uses OAuth2 bearer tokens instead of
SigV4, and the GCS XML API (which is intentionally S3-compatible for
PUT/GET/HEAD/DELETE and multipart upload).

---

## PR 1 — `gs://` URL parsing and HTTPS rewrite

Scope: pure URL handling, no auth, no store registration. Enables anonymous
reads from public buckets via `builtins.fetchurl "gs://..."` once filetransfer
is wired up (PR 3).

Files:
- `src/libstore/include/nix/store/gcs-url.hh`
- `src/libstore/gcs-url.cc`
- `src/libstore-tests/gcs-url.cc`
- `src/libstore/meson.build`, `src/libstore-tests/meson.build`

Contents:
- `struct ParsedGCSURL { std::string bucket; std::vector<std::string> key;
  std::optional<std::string> endpoint; std::optional<std::string> scheme;
  std::optional<std::string> userProject; /* requester-pays */ }`
- `static ParsedGCSURL parse(const ParsedURL &)` — accept
  `gs://bucket/key?endpoint=...&scheme=...&userProject=...`.
- `ParsedURL toHttpsUrl() const` — default endpoint
  `https://storage.googleapis.com`, path-style addressing (`/bucket/key`).
  GCS XML API uses path-style by default and also supports
  `bucket.storage.googleapis.com`; start with path-style only (simpler, no
  bucket-name DNS restrictions) and document it.
- Validate bucket name against
  https://cloud.google.com/storage/docs/buckets#naming (subset: length 3–63,
  lowercase, digits, `-`, `_`, `.`; reject `goog` prefix etc. can be a TODO).

Tests: round-trip parsing, `toHttpsUrl()` for default and custom endpoint,
error cases (missing bucket, missing key, bad scheme).

---

## PR 2 — GCP credential provider

Scope: resolve an OAuth2 access token from Application Default Credentials
without adding a heavy SDK dependency. The token is short-lived (~1 h) and used
as `Authorization: Bearer <token>`.

Files:
- `src/libstore/include/nix/store/gcp-creds.hh`
- `src/libstore/gcp-creds.cc`
- `src/libstore-tests/gcp-creds.cc`
- `src/libstore/meson.options`: add `feature` option `gcs-auth` (default
  `auto`), next to the existing `s3-aws-auth`.
- `src/libstore/meson.build`: gate `gcp-creds.cc` on `gcs-auth`, define
  `NIX_WITH_GCS_AUTH`. Add `dependency('openssl', required: gcs_auth)` to
  `deps_private` here — OpenSSL is currently only a private dep of **libutil**
  (`src/libutil/meson.build:80`), so libstore must declare it explicitly to
  link the RS256 signer. No new third-party deps.
- `packaging/components.nix` / `dependencies.nix`: add a `withGCSAuth ? true`
  knob mirroring `withAWSAuth`; OpenSSL is already in the closure.

API:
```cpp
struct GcpCredentials {
    std::string accessToken;
    std::chrono::steady_clock::time_point expiresAt;
};

class GcpCredentialProvider {
public:
    virtual ~GcpCredentialProvider();
    /** Returns std::nullopt when no credentials are available (anonymous). */
    virtual std::optional<GcpCredentials> maybeGetCredentials() = 0;
};

MakeError(GcpAuthError, Error);

/**
 * Build the default ADC chain:
 *  1. $GOOGLE_APPLICATION_CREDENTIALS → service-account or authorized-user JSON
 *  2. gcloud well-known file:
 *     $CLOUDSDK_CONFIG or $XDG_CONFIG_HOME/gcloud/application_default_credentials.json
 *  3. GCE/GKE metadata server:
 *     http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token
 *     (header `Metadata-Flavor: Google`, short connect timeout, only tried when
 *      the host resolves / $GCE_METADATA_HOST is set, to avoid hangs off-GCP)
 */
ref<GcpCredentialProvider> makeDefaultGcpCredentialProvider();

/**
 * Process-wide singleton, mirroring getAwsCredentialsProvider().
 * Tests override it with setGcpCredentialsProviderForTesting().
 */
ref<GcpCredentialProvider> getGcpCredentialsProvider();
void setGcpCredentialsProviderForTesting(ref<GcpCredentialProvider>);
```

Implementation notes:
- Service-account JSON: build a JWT
  (`{"alg":"RS256","typ":"JWT"}`, claims `iss`, `scope`
  `https://www.googleapis.com/auth/devstorage.read_write`, `aud`
  `https://oauth2.googleapis.com/token`, `iat`, `exp`), sign with the
  `private_key` (PEM) via OpenSSL `EVP_DigestSign` / RSA-SHA256, then POST
  `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=<jwt>` to
  `https://oauth2.googleapis.com/token` using the existing `FileTransfer`.
- Authorized-user JSON (`"type":"authorized_user"`): POST
  `client_id`/`client_secret`/`refresh_token` with `grant_type=refresh_token`
  to the token endpoint.
- Metadata server: simple GET, parse `{"access_token":..., "expires_in":...}`.
  Probe with a **1 s connect timeout** and only when
  `metadata.google.internal` resolves or `$GCE_METADATA_HOST` /
  `$GOOGLE_CLOUD_METADATA_HOST` is set, so off-GCP machines do not stall.
- Cache the token per-provider; refresh when `< 5 min` remaining. Use a
  `Sync<std::optional<GcpCredentials>>` keyed on the credentials-file path.
  **Drop the lock before performing the blocking token HTTP request** and
  re-check on re-acquire (double-checked refresh) so concurrent callers do not
  serialise on network I/O.
- All HTTP done through `getFileTransfer()` so proxy / CA settings are
  honoured. This is safe w.r.t. re-entrancy: `setupForGCS()` (PR 3) runs on
  the **caller** thread before the request is enqueued (same call site as
  `setupForS3()` at `filetransfer.cc:1207`), not on the curl worker thread, so
  a nested synchronous `download()` to `https://oauth2.googleapis.com/token`
  cannot deadlock the transfer pool. The token request itself is plain
  `https://`, so it never recurses into `setupForGCS()`.

Tests:
- Unit-test JWT header/claim serialisation and base64url encoding with a fixed
  clock and a test RSA key (generated at test time, not checked in).
- Unit-test ADC file discovery with `$GOOGLE_APPLICATION_CREDENTIALS` /
  `$CLOUDSDK_CONFIG` pointing into a tmpdir.
- Metadata-server path: unit-test against a local HTTP stub bound to
  `127.0.0.1` and pointed to via `$GCE_METADATA_HOST`. (The NixOS VM test in
  PR 6 also exercises it end-to-end.)

---

## PR 3 — `filetransfer` integration for `gs://`

Scope: make `FileTransfer` understand `gs://` so any consumer
(`builtins.fetchurl`, `HttpBinaryCacheStore`, the future `GCSBinaryCacheStore`)
gets transparent rewrite + auth.

Files:
- `src/libstore/include/nix/store/filetransfer.hh`
- `src/libstore/filetransfer.cc`
- `src/libstore-tests/filetransfer-request.cc`

Changes:
- Add `void FileTransferRequest::setupForGCS()` mirroring `setupForS3()`:
  - Parse the `gs://` URI with `ParsedGCSURL`, replace `uri` with the HTTPS
    URL.
  - Add `std::optional<std::string> preResolvedGcpAccessToken` on
    `FileTransferRequest`. If set, use it directly and skip the provider
    chain (used by PR 7 for the sandboxed `builtin:fetchurl`).
  - Otherwise, if `NIX_WITH_GCS_AUTH`, resolve a token via
    `getGcpCredentialsProvider()->maybeGetCredentials()` and append
    `Authorization: Bearer <token>` to `headers`. On `GcpAuthError`, log at
    `lvlTalkative` and continue anonymously (matches the S3 behaviour for
    public buckets).
- In `enqueueFileTransfer` / the request-dispatch path where
  `request.uri.scheme() == "s3"` is handled, add the analogous
  `== "gs"` branch calling `setupForGCS()`.
- GCS XML API returns S3-style `<Error><Code>…</Code></Error>` bodies; extend
  the existing retryable-error parsing to also match when the request was
  originally `gs://` (or simply drop the scheme check and rely on the XML
  shape). Add GCS-specific retryable codes if any differ
  (`InternalError`, `ServiceUnavailable`, `SlowDown` already overlap).

Tests:
- Extend `filetransfer-request.cc` with a `setupForGCS` test asserting the
  rewritten URL and the presence/absence of the `Authorization` header. Inject
  a stub via `setGcpCredentialsProviderForTesting()` (added in PR 2) so the
  test does not touch the network.

Known gap (closed by PR 7): `builtin:fetchurl` runs inside the build sandbox
with no access to ADC files, env vars, or the metadata server, so `gs://`
fixed-output derivations only work for public buckets until PR 7 lands.

---

## PR 4 — `GCSBinaryCacheStore`

Scope: register the `gs://` store scheme.

Files:
- `src/libstore/include/nix/store/gcs-binary-cache-store.hh`
- `src/libstore/gcs-binary-cache-store.cc`
- `src/libstore/gcs-binary-cache-store.md`
- `src/libstore-tests/gcs-binary-cache-store.cc`
- `src/libstore/meson.build`

Contents:
- `struct GCSBinaryCacheStoreConfig : HttpBinaryCacheStoreConfig` with settings:
  - `endpoint` (default empty → `storage.googleapis.com`)
  - `scheme` (`https` / `http`)
  - `user-project` (for requester-pays buckets → header
    `x-goog-user-project`)
  - `storage-class` (→ header `x-goog-storage-class`; values `STANDARD`,
    `NEARLINE`, `COLDLINE`, `ARCHIVE`)
  - `multipart-upload`, `multipart-chunk-size`, `multipart-threshold` — GCS
    XML API supports the S3 multipart protocol verbatim
    (`?uploads`, `?partNumber=`, `?uploadId=`, `<CompleteMultipartUpload>`),
    so these can share limits (`5 MiB` min part, `10 000` parts; max object
    `5 TiB`).
  - `gcsUriSettings` set analogous to `s3UriSettings` so `endpoint`/`scheme`
    round-trip through the store reference.
- `class GCSBinaryCacheStore : HttpBinaryCacheStore` overriding `upsertFile`:
  - Compute and send `x-goog-hash: md5=<base64>` (GCS equivalent of
    `Content-MD5`; GCS also accepts `Content-MD5` so this can reuse the S3
    code path initially).
  - Set `x-goog-storage-class` / `x-goog-user-project` when configured.
  - Multipart implementation: GCS XML API implements the S3 multipart protocol
    verbatim, so factor the existing `S3BinaryCacheStore::MultipartSink`,
    `createMultipartUpload`, `uploadPart`, `completeMultipartUpload`,
    `abortMultipartUpload` into a shared helper
    (`src/libstore/xml-multipart-upload.{cc,hh}`, or a protected base mixin on
    `HttpBinaryCacheStore`) parameterised on a `prepareRequest` hook (S3 calls
    `setupForS3()`, GCS calls `setupForGCS()`). This refactor lands as the
    first commit inside this PR; the second commit adds `GCSBinaryCacheStore`
    on top. Existing `s3-binary-cache-store` tests guard against regressions
    from the refactor.
- `RegisterStoreImplementation<GCSBinaryCacheStoreConfig>` and
  `uriSchemes() = {"gs"}`.

Tests:
- Config parsing / validation (chunk-size bounds, `getHumanReadableURI`).
- `upsertFile` against a `MockFileTransfer` (reuse the pattern from
  `s3-binary-cache-store.cc` tests) asserting headers and multipart sequencing.

---

## PR 5 — Documentation & release notes

Files:
- `src/libstore/gcs-binary-cache-store.md` (already added in PR 4; expand).
- `doc/manual/source/store/types/` — ensure the generated store-type page is
  picked up (same mechanism as `s3-binary-cache-store.md`).
- `doc/manual/source/command-ref/conf-file.md` / package-management docs:
  mention `gs://` alongside existing `s3://` examples.
- `doc/manual/rl-next/gcs-binary-cache-store.md`.

Contents: usage examples (`nix copy --to gs://my-bucket`), auth setup
(`gcloud auth application-default login`, service-account JSON,
GKE Workload Identity), requester-pays, storage classes, custom endpoint for
`fake-gcs-server`.

---

## PR 6 — NixOS VM test

Depends on: PR 4.

Files:
- `tests/nixos/gcs-binary-cache-store.nix`
- `tests/nixos/gcs-mock-server.py`
- `tests/nixos/default.nix` (register the test)

Approach: mirror `tests/nixos/s3-binary-cache-store.nix`, but instead of an
external emulator run a **self-contained Python mock** that implements the
subset of the GCS XML API and OAuth2/metadata endpoints that Nix actually
uses. `fake-gcs-server` was evaluated and rejected: it supports XML
GET/HEAD/PUT/DELETE but **not** the S3-style XML multipart endpoints
(`fakestorage/server.go:300-337` on upstream `main`), and we already need a
Python stub for the token endpoints anyway, so a single in-process server is
simpler and gives full control over assertions. Precedent:
`tests/filetransfer-retry-backoff/test_retry_backoff.py`.

`gcs-mock-server.py` (~150–200 lines, stdlib `http.server` + `ThreadingMixIn`,
in-memory `dict` storage):

| Route | Behaviour |
|---|---|
| `POST /token` | OAuth2 token endpoint: accept any `assertion`/`refresh_token`, return `{"access_token":"test-token","expires_in":3600}`. Record the request body so the test can assert a well-formed JWT was sent. |
| `GET /computeMetadata/v1/instance/service-accounts/default/token` | Require `Metadata-Flavor: Google`, return same token JSON. |
| `PUT /{bucket}/{key}` | Store body; honour/record `Content-MD5`, `x-goog-storage-class`, `x-goog-user-project`, `Authorization`. Return `ETag`. |
| `GET/HEAD /{bucket}/{key}` | Serve stored bytes / 404. |
| `DELETE /{bucket}/{key}` | Remove. |
| `POST /{bucket}/{key}?uploads` | Allocate `uploadId`, return `<InitiateMultipartUploadResult><UploadId>…</UploadId></InitiateMultipartUploadResult>`. |
| `PUT /{bucket}/{key}?partNumber=N&uploadId=X` | Store part bytes keyed by `(X,N)`, return `ETag: "md5-of-part"`. |
| `POST /{bucket}/{key}?uploadId=X` | Parse `<CompleteMultipartUpload>`, validate part numbers/ETags, concatenate parts into the object, return `<CompleteMultipartUploadResult>`. |
| `DELETE /{bucket}/{key}?uploadId=X` | Drop pending parts. |
| `GET /_internal/requests` | Dump recorded request metadata as JSON so the NixOS `testScript` can assert on headers without scraping logs. |

The server runs as a `systemd` unit on the `server` VM node, listening on a
fixed port; the store URI in the test is
`gs://test-bucket?endpoint=http://server:<port>&scheme=http`.

Auth fixtures inside the VM:
- A throwaway service-account JSON (RSA key generated in the test derivation,
  `token_uri` pointing at `http://server:<port>/token`) exported via
  `GOOGLE_APPLICATION_CREDENTIALS`.
- `$GCE_METADATA_HOST=server:<port>` for the metadata-server path.

Scenarios:
- anonymous read from a public object (no `Authorization` header recorded),
- authenticated write + read via service-account JSON; assert the mock saw
  `Authorization: Bearer test-token` and a syntactically valid RS256 JWT at
  `/token`,
- authenticated read via metadata-server provider (unset
  `GOOGLE_APPLICATION_CREDENTIALS`, rely on `$GCE_METADATA_HOST`),
- `nix copy --to gs://… --from gs://…` round-trip,
- multipart upload: copy a NAR larger than `multipart-threshold` with
  `multipart-upload=true`, assert via `/_internal/requests` that
  `?uploads` / `?partNumber=` / complete were called and the reassembled
  object matches a direct `nix-store --dump`,
- multipart abort: kill the client mid-upload (or have the mock 500 on
  `partNumber=2`) and assert `DELETE ?uploadId=` was issued.

Multipart remains additionally covered by the `MockFileTransfer` unit tests in
PR 4; the VM test gives end-to-end confidence including the libcurl wire
format.

---

## PR 7 — Credential forwarding into sandboxed `builtin:fetchurl`

Depends on: PR 2, PR 3.

Scope: `builtin:fetchurl` runs in a sandboxed child process that has no access
to `~/.config/gcloud`, env vars, or the metadata server. The existing S3
support handles this by having the **derivation builder** (parent process)
pre-resolve credentials and pass them into the child via
`BuiltinBuilderContext` — see `DerivationBuilderImpl::preResolveAwsCredentials()`
(`src/libstore/unix/build/derivation-builder.cc:622`) and its consumer
`src/libstore/builtins/fetchurl.cc:47`. There is **no** worker-protocol /
daemon serialisation involved. Mirror that mechanism for GCP.

Files:
- `src/libstore/include/nix/store/builtins.hh` — add
  `std::optional<std::string> gcpAccessToken;` to `BuiltinBuilderContext`
  alongside `awsCredentials`.
- `src/libstore/unix/build/derivation-builder-impl.hh` — declare
  `std::optional<std::string> preResolveGcpAccessToken();` and add the field
  to the args struct passed to the child.
- `src/libstore/unix/build/derivation-builder.cc` — implement
  `preResolveGcpAccessToken()`: if the derivation's `url` (or `urls`) has
  scheme `gs`, call `getGcpCredentialsProvider()->maybeGetCredentials()` and
  return the bearer token; pass it via `.gcpAccessToken = …` next to
  `.awsCredentials = preResolveAwsCredentials()`.
- `src/libstore/linux/build/linux-derivation-builder.cc`,
  `src/libstore/freebsd/build/freebsd-derivation-builder.cc` — same
  initialiser addition at their `BuiltinBuilderContext` construction sites.
- `src/libstore/builtins/fetchurl.cc` — alongside the existing
  `if (ctx.awsCredentials && scheme == "s3")` block, add
  `if (ctx.gcpAccessToken && scheme == "gs") request.preResolvedGcpAccessToken = *ctx.gcpAccessToken;`.
- `src/libstore/include/nix/store/filetransfer.hh` / `filetransfer.cc` —
  `preResolvedGcpAccessToken` already added in PR 3; `setupForGCS()` already
  prefers it over the provider chain. No changes needed here beyond PR 3.

Tests:
- Extend `tests/nixos/gcs-binary-cache-store.nix` (PR 6) with a fixed-output
  derivation using `builtins.fetchurl { url = "gs://…"; sha256 = …; }` against
  the fake server, asserting the build succeeds inside the sandbox.

Security note: the token is short-lived (≤1 h) and scoped to
`devstorage.read_write`; passing it to the sandboxed child has the same
exposure as the existing AWS session-token forwarding.

---

## PR ordering / dependencies

```
PR1 (gcs-url) ──┐
                ├─► PR3 (filetransfer) ──► PR4 (store+refactor) ──► PR5 (docs)
PR2 (gcp-creds) ┘          │                       └─► PR6 (nixos test) ◄─┐
                           └─► PR7 (builtin:fetchurl cred forwarding) ────┘
```

PR1 and PR2 are independent and can be opened in parallel.

## Decisions

- **Auth implementation**: hand-rolled OAuth2 (service-account JWT via OpenSSL
  RS256, gcloud `authorized_user` refresh, GCE/GKE metadata server). No new
  third-party dependency. `"type":"external_account"` (workload identity
  federation) is **not** supported initially and is documented as a known
  limitation.
- **Upload protocol**: S3-compatible XML multipart via the GCS XML API, sharing
  the existing S3 multipart implementation. GCS-native JSON resumable upload is
  not used (its only advantages — byte-offset resume and no 5 MiB part floor —
  do not matter for one-shot NAR uploads).
- **Multipart refactor**: folded into PR 4 as a leading no-behaviour-change
  commit, not a separate PR.
- **Daemon credential forwarding**: in scope, delivered as PR 7.
