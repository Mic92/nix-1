---
synopsis: Disallow URL literals
---

- URL literals (unquoted URLs) are now unconditionally disallowed in the Nix language.
  They were previously optionally disallowable via the `no-url-literals` experimental feature, which has now been removed.
  Existing Nix expressions containing URL literals must be updated to use quoted strings (e.g. `"http://example.com"` instead of `http://example.com`).
