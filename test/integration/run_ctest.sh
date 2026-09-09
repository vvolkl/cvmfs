#!/bin/bash

set -eu

if [ $# -ne 3 ]; then
  echo "Usage: $0 <test-root> <test-dir-relative-to-test-root> <output-dir>" >&2
  exit 1
fi

test_root="$1"
test_dir="$2"
output_dir="$3"

mkdir -p "$output_dir"
rm -rf "$output_dir/scratch"
mkdir -p "$output_dir/scratch"
rm -f "$output_dir/run.log" "$output_dir/xunit.xml"

cd "$test_root"
exec env CVMFS_TEST_SCRATCH="$output_dir/scratch" \
  bash "$test_root/run.sh" "$output_dir/run.log" -o "$output_dir/xunit.xml" "$test_dir"