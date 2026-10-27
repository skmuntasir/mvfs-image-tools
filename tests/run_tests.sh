#!/usr/bin/env bash
# End-to-end tests: build images, add files, verify with tools/inspect_fs.py, round-trip contents.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/mkfs_builder"; A="$ROOT/mkfs_adder"; V="python3 $ROOT/tools/inspect_fs.py"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cd "$T"
pass=0; fail=0
ok()   { echo "PASS: $1"; pass=$((pass+1)); }
bad()  { echo "FAIL: $1"; fail=$((fail+1)); }

head -c 100   /dev/urandom > small.bin
head -c 4096  /dev/urandom > exact1.bin
head -c 4097  /dev/urandom > exact1p.bin
head -c 10000 /dev/urandom > big.bin
head -c 49152 /dev/urandom > max12.bin
head -c 49153 /dev/urandom > toolarge.bin
: > empty.bin

"$B" --image base.img --size-kib 180 --inodes 128 >/dev/null || bad "builder"
$V base.img >/dev/null && ok "empty image verifies" || bad "empty image verifies"

cur=base.img; i=0
for f in small empty exact1 exact1p big max12; do
  i=$((i+1))
  "$A" --input "$cur" --output "s$i.img" --file "$f.bin" >/dev/null 2>&1 || { bad "add $f"; continue; }
  cur="s$i.img"
done
$V "$cur" >/dev/null && ok "image consistent after 6 files" || bad "image consistent after 6 files"
for f in small empty exact1 exact1p big max12; do
  $V "$cur" --extract "$f.bin" "out_$f.bin" >/dev/null
  cmp -s "$f.bin" "out_$f.bin" && ok "round-trip $f.bin" || bad "round-trip $f.bin"
done

"$A" --input "$cur" --output dup.img --file big.bin 2>/dev/null
[ $? -ne 0 ] && [ ! -e dup.img ] && ok "duplicate name rejected, no output left" || bad "duplicate name rejected"
"$A" --input base.img --output big.img --file toolarge.bin 2>/dev/null
[ $? -ne 0 ] && ok "file > 12 blocks rejected" || bad "file > 12 blocks rejected"
head -c 300 base.img > trunc.img
"$A" --input trunc.img --output t.img --file small.bin 2>/dev/null
[ $? -ne 0 ] && ok "truncated image rejected" || bad "truncated image rejected"

# fill the disk with 12-block files; must stop cleanly with a consistent image
"$B" --image full.img --size-kib 180 --inodes 128 >/dev/null; cur=full.img; n=0
while :; do n=$((n+1)); cp max12.bin "f$n.bin"
  "$A" --input "$cur" --output "g$n.img" --file "f$n.bin" >/dev/null 2>&1 || break; cur="g$n.img"; done
$V "$cur" >/dev/null && ok "image consistent after disk full" || bad "image consistent after disk full"

echo; echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
