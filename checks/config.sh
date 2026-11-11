#!/usr/bin/env bash
# Checks zwwm's configuration language against a built binary.
# Usage: checks/config.sh path/to/zwwm
#
#   zwwm/data/config.zw       the shipped default must load without warnings
#   checks/config/valid/      every file must load without warnings
#   checks/config/invalid/    every file must be rejected with the message on its
#                             first line: -- expect: <part of the error>
#   *.md                      every ```zw block must load; warnings are allowed.
#                             A block is skipped when the line before it is
#                             <!-- config-check: skip -->
set -uo pipefail

zwwm=${1:?usage: config.sh path/to/zwwm}
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=0
checked=0

fail() {
  failed=$((failed + 1))
  echo "FAIL $1"
  sed 's/^/     /' <<<"$2"
}

# must_load NAME FILE strict|lenient: strict also rejects warnings.
must_load() {
  local output
  checked=$((checked + 1))
  if ! output=$("$zwwm" --check-config "$2" 2>&1); then
    fail "$1" "$output"
  elif [[ $3 == strict && $output == *": warning: "* ]]; then
    fail "$1 (warnings are not allowed here)" "$output"
  fi
}

must_load zwwm/data/config.zw "$root/zwwm/data/config.zw" strict

for file in "$root"/checks/config/valid/*.zw; do
  must_load "valid/$(basename "$file")" "$file" strict
done

for file in "$root"/checks/config/invalid/*.zw; do
  name=invalid/$(basename "$file")
  checked=$((checked + 1))
  expected=$(sed -n '1s/^-- expect: //p' "$file")
  if [[ -z $expected ]]; then
    fail "$name" "first line must be: -- expect: <part of the error>"
  elif output=$("$zwwm" --check-config "$file" 2>&1); then
    fail "$name" "accepted, but should be rejected with: $expected"
  elif [[ $output != *": error: "*"$expected"* ]]; then
    fail "$name" "expected: $expected"$'\n'"$output"
  fi
done

# Documentation snippets, one file per fenced block, named after its source line.
while IFS= read -r doc; do
  relative=${doc#"$root"/}
  prefix=$work/$(tr '/' '_' <<<"$relative")
  awk -v prefix="$prefix" '
    /^```zw$/ { if (previous !~ /config-check: skip/) { file = sprintf("%s:%d.zw", prefix, NR + 1); inside = 1 } next }
    /^```/ { inside = 0 }
    inside { print > file }
    NF { previous = $0 }
  ' "$doc"
done < <(find "$root" -name '*.md' -not -path '*/.git/*' | sort)
for file in "$work"/*.zw; do
  [[ -e $file ]] || continue
  name=$(basename "$file" .zw)
  must_load "docs ${name//_//}" "$file" lenient
done

echo "$checked configuration checks, $failed failed"
[[ $failed -eq 0 ]]
