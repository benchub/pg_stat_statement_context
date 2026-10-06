/*
 * standalone_main.c
 *		Driver for the fuzz targets without libFuzzer (any C compiler): feeds
 *		each file named on the command line (directories are read one level
 *		deep), then N pseudo-random inputs built from the target's
 *		alphabet of special bytes (fuzz_alphabet()), to
 *		LLVMFuzzerTestOneInput().
 *
 *	fuzz_X_standalone [--random N] FILE|DIR...
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fuzz_check.h"

static size_t nfiles;

static void
run_file(const char *path)
{
	FILE	   *f = fopen(path, "rb");
	static uint8_t buf[1 << 20];
	size_t		n;

	if (!f)
	{
		perror(path);
		exit(2);
	}
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	LLVMFuzzerTestOneInput(buf, n);
	nfiles++;
}

static void
run_path(const char *path)
{
	struct stat st;
	DIR		   *d;
	struct dirent *e;

	if (stat(path, &st) != 0)
	{
		perror(path);
		exit(2);
	}
	if (!S_ISDIR(st.st_mode))
	{
		run_file(path);
		return;
	}
	d = opendir(path);
	if (!d)
	{
		perror(path);
		exit(2);
	}
	while ((e = readdir(d)) != NULL)
	{
		char		p[4096];

		if (e->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
		if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
			run_file(p);
	}
	closedir(d);
}

int
main(int argc, char **argv)
{
	size_t		nalpha;
	const char *alphabet = fuzz_alphabet(&nalpha);
	unsigned long nrandom = 0;
	uint32_t	seed = 12345;
	unsigned long i;
	int			a = 1;

	if (argc > 2 && strcmp(argv[1], "--random") == 0)
	{
		nrandom = strtoul(argv[2], NULL, 10);
		a = 3;
	}
	for (; a < argc; a++)
		run_path(argv[a]);
	for (i = 0; i < nrandom; i++)
	{
		uint8_t		buf[64];
		size_t		len,
					k;

		seed = seed * 1103515245u + 12345u;
		len = (seed >> 16) % sizeof(buf);
		for (k = 0; k < len; k++)
		{
			seed = seed * 1103515245u + 12345u;
			buf[k] = (uint8_t) alphabet[(seed >> 16) % nalpha];
		}
		LLVMFuzzerTestOneInput(buf, len);
	}
	printf("ok: %zu files, %lu random inputs\n", nfiles, nrandom);
	return 0;
}
