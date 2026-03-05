# cargo-resolve — vendored Rust library for `builtins.resolveCargoWorkspace`

## Upstream

<https://github.com/Mic92/cargo-nix-plugin> — commit `cc5b047`

The upstream project is a Nix *plugin* (dlopen'd `.so`).  This directory
embeds the same Rust core library directly into `libexpr` as a built-in
primop, removing the need for `--option plugin-files`.

## What is vendored

Only the Rust static library (`rust/`) and its lockfile are copied.
The files are **unmodified** from upstream — a plain `diff -r` against
`cargo-nix-plugin/rust/src/` should show no changes.

## What is NOT vendored

- `cpp/plugin.cc` / `CMakeLists.txt` — replaced by
  `primops/resolveCargoWorkspace.cc` which registers the primop via
  `RegisterPrimOp` (built-in) instead of `__attribute__((constructor))`
  (plugin).
- `lib/default.nix` — the Nix-language wrapper that connects
  `builtins.resolveCargoWorkspace` to `buildRustCrate`.  Consumers
  bring their own copy (it lives in the cargo-nix-plugin repo).
- `tests/` and `rust/tests/fixtures/` — large fixture files (11 MB
  metadata JSON).  Tests are run in the upstream repo; the Nix build
  here sets `doCheck = false`.
- `nix/`, `flake.nix`, `flake.lock` — upstream packaging that is
  specific to the plugin build.

## Updating

```bash
# From the nix repo root:
cp -a ../cargo-nix-plugin/rust/src/  src/libexpr/cargo-resolve/rust/src/
cp    ../cargo-nix-plugin/rust/Cargo.toml src/libexpr/cargo-resolve/rust/
cp    ../cargo-nix-plugin/rust/Cargo.lock src/libexpr/cargo-resolve/rust/
# Then verify nothing else changed:
diff -r ../cargo-nix-plugin/rust/src/ src/libexpr/cargo-resolve/rust/src/
```

If the C FFI interface (`resolve_cargo_workspace` / `free_string`)
changes signature, update `primops/resolveCargoWorkspace.cc` to match.
