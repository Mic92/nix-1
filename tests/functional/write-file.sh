#!/usr/bin/env bash
source common.sh

clearStore

# Basic file
out=$(nix-build -E '
  derivation {
    name = "test.txt";
    system = "builtin";
    builder = "builtin:write-file";
    content = "hello world";
  }
' --no-out-link)
[[ $(cat "$out") = "hello world" ]] || fail "Basic file content mismatch"

# Executable file
out=$(nix-build -E '
  derivation {
    name = "test.sh";
    system = "builtin";
    builder = "builtin:write-file";
    content = "#!/bin/sh\necho hello";
    executable = "1";
  }
' --no-out-link)
[[ -x "$out" ]] || fail "File is not executable"
[[ $($out) = "hello" ]] || fail "Executable output mismatch"

# Test missing content parameter (should fail)
(! nix-build -E '
  derivation {
    name = "missing.txt";
    system = "builtin";
    builder = "builtin:write-file";
  }
' --no-out-link 2>&1)

# Test with string context (dependency tracking)
dep=$(nix-build -E '
  derivation {
    name = "dependency";
    system = builtins.currentSystem;
    builder = "/bin/sh";
    args = [ "-c" "echo dependency > $out" ];
  }
' --no-out-link)

out=$(nix-build -E '
  let
    dep = builtins.storePath "'$dep'";
  in derivation {
    name = "with-dep.sh";
    system = "builtin";
    builder = "builtin:write-file";
    content = "#!/bin/sh\necho using ${dep}";
  }
' --no-out-link)

grepQuiet "$dep" "$out" || fail "Dependency store path not found in content"
# Check that the output references the dependency
nix-store -q --references "$out" | grepQuiet "$dep" || fail "Output does not reference dependency"
