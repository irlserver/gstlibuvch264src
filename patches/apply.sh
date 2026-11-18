#!/bin/sh
# Apply the libuvc patches to the libuvc tree in the current directory, in
# the order of their number prefix.
set -e
for p in "$(dirname "$0")"/*.patch; do
  patch -p1 < "$p"
done
