#!/usr/bin/env bash

source common.sh

# Pins the JSON schema and accessor coverage of
# `NIX_DUMP_FILE_ACCESS`. The fixture exercises every access type that
# routes through `rootFS`; each must land in the right bucket. The
# `absent.flag` check is the one that would silently regress if a
# future refactor changed how pathExists routes through the accessor —
# the negative dep the feature exists for.

fixture="$TEST_ROOT/dfa"
rm -rf "$fixture"
mkdir -p "$fixture/subdir" "$fixture/tree"
echo "hello" > "$fixture/data.txt"
echo "never" > "$fixture/untouched.txt"
echo "kept"  > "$fixture/tree/keep.txt"
echo "drop"  > "$fixture/tree/drop.txt"
ln -s data.txt "$fixture/link"
cat > "$fixture/imported.nix" <<'EOF'
42
EOF
cat > "$fixture/expr.nix" <<'EOF'
let
  a = import ./imported.nix;                    # -> files (parser readFile)
  b = builtins.readFile ./data.txt;              # -> files
  c = builtins.readDir ./subdir;                 # -> dirs
  d = builtins.pathExists ./data.txt;            # -> files (stat hit)
  e = builtins.pathExists ./absent.flag;         # -> files (stat miss; negative dep)
  f = "${./subdir}";                             # -> trees + dirs
  g = builtins.hashFile "sha256" ./data.txt;     # -> files
  h = builtins.readFileType ./subdir;            # -> files (stat)
  i = builtins.getEnv "DFA_TEST_ENV";            # -> env
  j = builtins.readFile ./link;                  # -> files (readlink via resolveSymlinks)
  k = builtins.filterSource                      # -> trees; every entry lstat'd by
        (p: t: baseNameOf p == "keep.txt")       #    callPathFilter (for the `t` arg)
        ./tree;
in
  builtins.deepSeq [ a b c d e f g h i j k ] (derivation {
    name = "dfa-probe";
    system = builtins.currentSystem;
    builder = "/bin/sh";
  })
EOF

out="$TEST_ROOT/dfa-access.json"
rm -f "$out"

DFA_TEST_ENV=probe NIX_DUMP_FILE_ACCESS="$out" nix-instantiate "$fixture/expr.nix" > /dev/null

[[ -f "$out" ]] || fail "NIX_DUMP_FILE_ACCESS did not write $out"

# Resolve the fixture root the same way the trace will have
# (getPhysicalPath returns a realpath), so prefix matches work
# regardless of whether $TEST_ROOT goes through a symlink.
canon="$(readlink -f "$fixture")"

has() {
    local bucket="$1" needle="$2"
    jq -e --arg b "$bucket" --arg p "$needle" \
        '.[$b] | index($p) != null' "$out" > /dev/null \
      || fail "expected $needle in bucket '$bucket'; got $(jq -c --arg b "$bucket" '.[$b]' "$out")"
}

hasnt() {
    local bucket="$1" needle="$2"
    jq -e --arg b "$bucket" --arg p "$needle" \
        '.[$b] | index($p) == null' "$out" > /dev/null \
      || fail "$needle should not be in bucket '$bucket'"
}

has files "$canon/imported.nix"
has files "$canon/data.txt"
has files "$canon/subdir"       # readFileType / ancestor stat
has files "$canon/absent.flag"  # negative dep
has files "$canon/link"

has dirs  "$canon/subdir"
has dirs  "$canon/tree"

has trees "$canon/subdir"
has trees "$canon/tree"

# filterSource: every entry is lstat'd by callPathFilter (to pass the
# type as the 2nd arg) regardless of the filter's verdict, so both
# land in `files`. That's sound: if drop.txt flipped file→dir, a
# type-sensitive filter could change its mind.
has files "$canon/tree/keep.txt"
has files "$canon/tree/drop.txt"

# Negative control: a file the expression never references is absent.
hasnt files "$canon/untouched.txt"

# env records name -> observed value.
[[ "$(jq -r '.env.DFA_TEST_ENV' "$out")" == "probe" ]] \
  || fail "env.DFA_TEST_ENV != probe; got $(jq -c '.env' "$out")"

echo "all buckets covered"
