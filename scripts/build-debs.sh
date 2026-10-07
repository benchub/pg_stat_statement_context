#!/bin/sh
# Builds Ubuntu 24.04 (noble) .deb packages for PostgreSQL 14-18 on amd64 and
# arm64 in Docker, and puts them in binaries/.
# Usage: scripts/build-debs.sh [pg-major...]
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

majors="${*:-14 15 16 17 18}"
upstream=$(sed -n 's/^## \[\([0-9][0-9.]*\)\].*/\1/p' CHANGELOG.md | head -1)
version="${upstream}-1.noble"
out="$root/tmp/debs"

rm -rf "$out"
mkdir -p "$out/src" binaries

# Package the committed tree only (no working-tree edits, no tmp/).
git archive HEAD | tar -x -C "$out/src"

for arch in amd64 arm64; do
	image="pssc-deb-noble-$arch"
	docker build --platform "linux/$arch" -t "$image" \
		-f docker/Dockerfile.deb docker
	docker run --rm --platform "linux/$arch" \
		-v "$out/src:/src:ro" -v "$out:/out" \
		"$image" "$version" $majors
done

cp "$out"/*.deb binaries/
ls -l binaries/
