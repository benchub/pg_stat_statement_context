#!/bin/bash
# Build and install an exact PostgreSQL release from the official source
# tarball. Shared by docker/Dockerfile.source (Linux harness and CI) and the
# macOS CI cells, so every source build uses the same download, checksum and
# configure steps.
#
#   docker/build-postgres.sh <version> <prefix> [flavor]
#
# flavor:
#   release   (default) plain build with --enable-tap-tests
#   assert    + --enable-cassert --enable-debug (USE_ASSERT_CHECKING)
#   valgrind  + --enable-cassert --enable-debug, -DUSE_VALGRIND, -Og; also
#             installs src/tools/valgrind.supp as <prefix>/share/valgrind.supp
#
# Besides the server it installs the TAP Perl modules (src/test/perl),
# pg_regress, pg_stat_statements (for the parity checks of the TAP tests) and
# auto_explain (for test/t/036_hook_coexistence.pl).
set -euo pipefail

PG_VERSION=${1:?usage: build-postgres.sh <version> <prefix> [release|assert|valgrind]}
PREFIX=${2:?usage: build-postgres.sh <version> <prefix> [release|assert|valgrind]}
FLAVOR=${3:-release}

configure_flags=(--prefix="$PREFIX" --enable-tap-tests
	--without-readline --without-zlib --without-icu)
# Only set when needed: an empty but set CFLAGS makes configure drop -O2.
build_env=()
case $FLAVOR in
release) ;;
assert) configure_flags+=(--enable-cassert --enable-debug) ;;
valgrind)
	configure_flags+=(--enable-cassert --enable-debug)
	build_env=(CFLAGS="-Og -g3 -fno-omit-frame-pointer" CPPFLAGS="-DUSE_VALGRIND")
	;;
*) echo "build-postgres.sh: unknown flavor '$FLAVOR'" >&2; exit 2 ;;
esac

if command -v sha256sum >/dev/null; then
	sha256_check() { sha256sum --check --strict "$1"; }
else
	sha256_check() { shasum -a 256 --check "$1"; }
fi
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

work=$(mktemp -d "${TMPDIR:-/tmp}/pgbuild.XXXXXX")
trap 'rm -rf "$work"' EXIT
cd "$work"

# Download the tarball and its official .sha256 to files (no pipelines, so a
# failed or truncated download cannot be masked), verify, and only then extract.
base="https://ftp.postgresql.org/pub/source/v${PG_VERSION}"
tarball="postgresql-${PG_VERSION}.tar.bz2"
curl -fSL --retry 5 --retry-delay 3 --retry-connrefused -o "$tarball" "$base/$tarball"
curl -fSL --retry 5 --retry-delay 3 --retry-connrefused -o "$tarball.sha256" "$base/$tarball.sha256"
grep -Eqx "[0-9a-f]{64}  $tarball" "$tarball.sha256" \
	|| { echo "malformed checksum file for $tarball:" >&2; cat "$tarball.sha256" >&2; exit 1; }
sha256_check "$tarball.sha256"
tar -xjf "$tarball"
rm -f "$tarball"

cd "postgresql-${PG_VERSION}"
env ${build_env[@]+"${build_env[@]}"} ./configure -q "${configure_flags[@]}"
make -s -j"$jobs"
make -s install
make -s -C src/test/perl install
make -s -C src/test/regress install
make -s -C contrib/pg_stat_statements install
make -s -C contrib/auto_explain install
if [ "$FLAVOR" = valgrind ]; then
	mkdir -p "$PREFIX/share"
	cp src/tools/valgrind.supp "$PREFIX/share/valgrind.supp"
fi
echo "build-postgres.sh: installed PostgreSQL $PG_VERSION ($FLAVOR) in $PREFIX"
