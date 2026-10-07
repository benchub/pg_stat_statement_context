#!/bin/sh
# Runs inside docker/Dockerfile.deb. Builds one .deb per PostgreSQL major from
# the sources mounted read-only at /src, writing them to /out.
# Usage: build-deb.sh <version> <pg-major>...
set -eu

version="$1"
shift
arch=$(dpkg --print-architecture)

for major in "$@"; do
	pg_config="/usr/lib/postgresql/$major/bin/pg_config"
	pkg="postgresql-$major-pg-stat-statement-context"
	work="/build/$major"
	stage="$work/stage"

	echo "=== PostgreSQL $major ($arch)"
	rm -rf "$work"
	mkdir -p "$work"
	cp -a /src/. "$work/src"
	cd "$work/src"
	make clean PG_CONFIG="$pg_config" >/dev/null
	# with_llvm=no: no JIT bitcode, so the package needs no clang toolchain.
	make PG_CONFIG="$pg_config" with_llvm=no
	make install PG_CONFIG="$pg_config" with_llvm=no DESTDIR="$stage"

	docdir="$stage/usr/share/doc/$pkg"
	mkdir -p "$docdir" "$stage/DEBIAN"
	cp README.md CHANGELOG.md "$docdir/"
	cp LICENSE "$docdir/copyright"

	size=$(du -sk "$stage" | cut -f1)
	cat > "$stage/DEBIAN/control" <<EOF
Package: $pkg
Version: $version
Architecture: $arch
Maintainer: pg_stat_statement_context maintainers
Installed-Size: $size
Depends: postgresql-$major, libc6
Section: database
Priority: optional
Homepage: https://github.com/benchub/pg_stat_statement_context
Description: per-tag statement statistics from SQL comments for PostgreSQL $major
 pg_stat_statement_context is a companion to pg_stat_statements that groups
 statement statistics by the tags in SQL comments (sqlcommenter, marginalia).
 It must be listed in shared_preload_libraries.
EOF

	dpkg-deb --root-owner-group --build "$stage" "/out/${pkg}_${version}_${arch}.deb"
done
