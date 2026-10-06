/*
 * tagout.h
 *		Output of stored tag sets as jsonb (DESIGN.md §6.11): shared by
 *		pg_stat_statement_context_extract() and the views.
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

#endif							/* PSSC_TAGOUT_H */
