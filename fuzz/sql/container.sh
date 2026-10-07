#!/bin/bash
# fuzz/sql/container.sh MODE [regex_fuzz.pl options]
# Runs inside the docker/Dockerfile (MODE pgdg) or docker/Dockerfile.source
# (MODE release, assert or valgrind) images, started by fuzz/sql/run.sh with
# the repository at /src (read-only) and a writable /out. Builds and installs
# the extension, creates a cluster with it preloaded, and runs
# fuzz/sql/regex_fuzz.pl as the postgres user. In valgrind mode the server
# runs under Valgrind as in docker/run-tests.sh and any Valgrind error fails.
set -euo pipefail

MODE=${1:?usage: container.sh MODE [options]}
shift
SRC=/src
BUILD=/build
OUT=/out
WORK=/var/lib/postgresql/fuzz
PGDATA_DIR=$WORK/data
VGLOG=$WORK/valgrind
PGBIN=$(pg_config --bindir)

finish() {
	rc=$?
	# Results are copied as root, and the server log keeps pg_ctl's 0600:
	# give them to the owner of the bind-mounted /out (the host user) so the
	# host and CI can read them.
	cp -rf "$WORK"/out/. "$OUT"/ 2>/dev/null || true
	chown -R "$(stat -c %u:%g "$OUT")" "$OUT" 2>/dev/null && chmod -R u+rwX "$OUT" 2>/dev/null || true
	exit $rc
}
trap finish EXIT

echo "PostgreSQL: $(pg_config --version) (mode: $MODE)"
rm -rf "$BUILD" "$WORK"
mkdir -p "$BUILD" "$WORK/out"
tar -C "$SRC" --exclude=./.git --exclude=./tmp \
	--exclude='*.o' --exclude='*.so' --exclude='*.dylib' --exclude='*.bc' --exclude='*.dSYM' \
	-cf - . | tar -C "$BUILD" -xf -
chown -R postgres:postgres "$BUILD" "$WORK"
cd "$BUILD"
gosu postgres make -s clean >/dev/null
gosu postgres make -s -j4 PG_CFLAGS=-Werror >/dev/null
make -s install >/dev/null
gosu postgres "$PGBIN/initdb" -D "$PGDATA_DIR" --no-sync -A trust -E UTF8 --locale=C >/dev/null
echo "shared_preload_libraries = 'pg_stat_statement_context'" >> "$PGDATA_DIR/postgresql.conf"

VGOPT=()
if [ "$MODE" = assert ] || [ "$MODE" = valgrind ]; then
	pg_config --configure | grep -q -- --enable-cassert || { echo "server not built with --enable-cassert" >&2; exit 1; }
fi
if [ "$MODE" = valgrind ]; then
	SUPP="$(pg_config --sharedir)/valgrind.supp"
	[ -f "$SUPP" ] || { echo "missing $SUPP" >&2; exit 1; }
	mkdir -p "$VGLOG" && chown postgres:postgres "$VGLOG"
	[ -e "$PGBIN/postgres.orig" ] || mv "$PGBIN/postgres" "$PGBIN/postgres.orig"
	cat > "$PGBIN/postgres" <<-WRAPPER
	#!/bin/sh
	exec valgrind --quiet --trace-children=yes --track-origins=yes \\
	  --read-var-info=no --num-callers=40 --leak-check=no --error-limit=no \\
	  --gen-suppressions=all --suppressions="$SUPP" \\
	  --error-markers=VALGRINDERROR-BEGIN,VALGRINDERROR-END --error-exitcode=128 \\
	  --log-file="$VGLOG/%p.log" "$PGBIN/postgres.orig" "\$@"
	WRAPPER
	chmod 755 "$PGBIN/postgres"
	VGOPT=(--valgrind)
fi

rc=0
gosu postgres perl "$SRC/fuzz/sql/regex_fuzz.pl" --bindir "$PGBIN" --pgdata "$PGDATA_DIR" \
	--work "$WORK" --out "$WORK/out" ${VGOPT[@]+"${VGOPT[@]}"} "$@" || rc=$?

if [ "$MODE" = valgrind ]; then
	n=$(find "$VGLOG" -name '*.log' | wc -l)
	bad=$(grep -l VALGRINDERROR-BEGIN "$VGLOG"/*.log || true)
	if [ -n "$bad" ]; then
		mkdir -p "$WORK/out/valgrind"
		for f in $bad; do echo "--- $f"; cat "$f"; cp "$f" "$WORK/out/valgrind/"; done
		echo "FAIL: Valgrind reported errors in $(echo "$bad" | wc -l) process(es)"
		rc=1
	elif [ "$n" -lt 3 ]; then
		echo "FAIL: only $n Valgrind log files: server not under Valgrind?"
		rc=1
	else
		echo "Valgrind: no errors in $n processes"
	fi
fi
exit $rc
