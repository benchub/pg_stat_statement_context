/*
 * tagout.h
 *		Output of stored tag sets as jsonb (DESIGN.md §6.11): shared by
 *		pg_stat_statement_context_extract() and the views (stats_fn.c).
 */
#ifndef PSSC_TAGOUT_H
#define PSSC_TAGOUT_H

#include "utils/jsonb.h"

/*
 * Pushes the tag set tags[0, len) (serialized k \0 v \0 ..., as stored:
 * src/tagset.h) as one jsonb object onto *st, from WJB_BEGIN_OBJECT to
 * WJB_END_OBJECT; use it as a value after a WJB_KEY, or alone. encoding is
 * the encoding the tags were stored in (their database's): keys and values
 * from a SQL_ASCII database are escaped (pssc_tag_escape()), others are
 * converted to the server encoding with pg_any_to_server(), which raises an
 * error if they are not representable in it. Allocates in the current
 * memory context.
 */
extern void pssc_tags_push_jsonb(JsonbParseState **st, const char *tags,
								 size_t len, int encoding);

/*
 * The tag set as a standalone jsonb object, like pssc_tags_push_jsonb(), but
 * never raising an error for tags that cannot be converted: if any key or
 * value of a tag set from a non-SQL_ASCII database is not representable in
 * the server encoding (or no conversion exists), the whole set is output
 * escaped as for SQL_ASCII (pssc_tag_escape(): \xHH for bytes >= 0x80,
 * \\ for '\'), and *escaped_fallback (if not NULL) is set. The conversion
 * runs in the conversion functions' noError mode, so no error is caught;
 * other errors (out of memory) still propagate. Used by the stats views, so
 * that one unconvertible entry does not fail the whole read.
 */
extern Jsonb *pssc_tags_jsonb_noerror(const char *tags, size_t len,
									  int encoding, bool *escaped_fallback);

#endif							/* PSSC_TAGOUT_H */
