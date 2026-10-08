/*
 * export.h
 *		Visibility of the internal API that the TEST-ONLY modules reach.
 *
 * The library is built with -fvisibility=hidden (Makefile), so it exports
 * only what PostgreSQL looks up (_PG_init, Pg_magic_func, the SQL-callable
 * functions and their pg_finfo_* records, the reclaim worker's entry point,
 * all marked PGDLLEXPORT). The functions and variables that the modules in
 * test/modules/ reach are declared with PSSC_TEST_API: exported in the
 * testing build (make PSSC_TESTING=1, which also compiles the test hooks),
 * hidden in the release build. scripts/check-release-exports.sh checks the
 * release build's exports against test/release-exports.txt.
 */
#ifndef PSSC_EXPORT_H
#define PSSC_EXPORT_H

#if defined(PSSC_TESTING)
#define PSSC_TEST_API PGDLLEXPORT
#elif defined(__GNUC__)
#define PSSC_TEST_API __attribute__((visibility("hidden")))
#else
#define PSSC_TEST_API
#endif

#endif							/* PSSC_EXPORT_H */
