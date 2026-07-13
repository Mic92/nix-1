---
synopsis: S3 credential fixes — STS region fallback, provider caching, 403 retry
prs: [15609]
---

Three improvements to AWS authentication for S3 binary caches:

- When authenticating via STS WebIdentity (EKS IRSA, GitHub Actions OIDC), Nix
  now uses the `?region=` parameter from the S3 URL as a fallback for the STS
  endpoint region if neither `AWS_REGION` nor `AWS_DEFAULT_REGION` is set.
  Previously, IRSA setups that exported `AWS_WEB_IDENTITY_TOKEN_FILE` and
  `AWS_ROLE_ARN` but no region would fail.

- The credential provider chain is now cached. Previously every S3 request
  triggered a fresh STS / SSO / IMDS round-trip; credentials are now held for
  up to 15 minutes (or until their embedded expiration, if shorter).

- When an S3 request fails with 403 `ExpiredToken`, Nix now asynchronously
  refreshes credentials and retries. Previously a session token expiring
  mid-build was a hard failure.
