# Split SQL scripts into one seed file per statement (a statement ends at a
# line ending in ';'), skipping psql meta-commands and comment-only lines
# that do not belong to a statement: fuzz seeds from the regression inputs.
#   awk -v out=DIR -f split-sql.awk FILE...
FNR == 1 { base = FILENAME; sub(/.*\//, "", base); sub(/\.sql$/, "", base); n = 0; buf = "" }
buf == "" && ($0 ~ /^\\/ || $0 ~ /^--/ || $0 ~ /^[ \t]*$/) { next }
{
	buf = buf $0 "\n"
	if ($0 ~ /;[ \t]*$/) {
		f = sprintf("%s/sql-%s-%03d.sql", out, base, n++)
		printf "%s", buf > f
		close(f)
		buf = ""
	}
}
