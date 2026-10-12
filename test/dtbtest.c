/*
 * Host unit test for arm/dtb.c.
 *
 * Usage:
 *   dtbtest <in.dtb> <out.dtb> [cmdline] [initrd_start initrd_size]
 *
 * Validates the input blob, runs dtb_prepare() with the given command
 * line / initrd parameters and writes the patched blob to out.dtb.
 * The shell driver then decompiles out.dtb with dtc to check the
 * /chosen properties.  Also runs a few built-in negative tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dtb.h"

static void *xread(const char *name, unsigned int *size_out)
{
	FILE *f = fopen(name, "rb");
	unsigned char *buf;
	long len;

	if (!f) {
		fprintf(stderr, "cannot open %s\n", name);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = calloc(len + 64, 1);
	if (fread(buf, 1, len, f) != (size_t)len) {
		fprintf(stderr, "short read on %s\n", name);
		exit(2);
	}
	fclose(f);
	*size_out = (unsigned int)len;
	return buf;
}

static UInt32 get_be32(const unsigned char *p)
{
	return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) |
	       ((UInt32)p[2] << 8) | p[3];
}

int main(int argc, char **argv)
{
	unsigned char *src, *dest;
	unsigned int size;
	unsigned int total;
	UInt32 rstart = 0, rsize = 0;
	const char *cmdline = NULL;
	int err;
	FILE *f;

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s in.dtb out.dtb [cmdline] [rstart rsize]\n",
			argv[0]);
		return 2;
	}
	if (argc >= 4)
		cmdline = argv[3];
	if (argc >= 6) {
		rstart = (UInt32)strtoul(argv[4], NULL, 0);
		rsize = (UInt32)strtoul(argv[5], NULL, 0);
	}

	src = xread(argv[1], &size);

	printf("validate: %d\n", dtb_validate(src, size));

	/* negative tests */
	{
		unsigned char bad[64];
		memcpy(bad, src, 64);
		bad[0] = 0xde;
		printf("validate-bad-magic: %d (expect -1)\n",
		       dtb_validate(bad, 64));
		memcpy(bad, src, 64);
		bad[23] = 0x02;			/* version = 2 */
		printf("validate-old-version: %d (expect -2)\n",
		       dtb_validate(bad, 64));
		memcpy(bad, src, 64);
		bad[get_be32(bad + 8)] ^= 0xff;	/* corrupt first struct token */
		printf("validate-corrupt: %d (expect -4)\n",
		       dtb_validate(bad, size));
	}

	dest = calloc(size + FDT_SLACK + 64, 1);
	err = dtb_prepare(dest, src, size, cmdline, rstart, rsize);
	printf("prepare: %d\n", err);
	if (err)
		return 1;

	total = get_be32(dest + 4);
	printf("revalidate: %d\n", dtb_validate(dest, total));

	f = fopen(argv[2], "wb");
	if (!f) {
		fprintf(stderr, "cannot write %s\n", argv[2]);
		return 2;
	}
	fwrite(dest, 1, total, f);
	fclose(f);

	printf("wrote %s (%u -> %u bytes)\n", argv[2], size, total);
	return 0;
}
