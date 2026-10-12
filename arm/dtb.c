/*
 * dtb.c: minimal flattened device tree (FDT) support for DT-based boot.
 *
 * Recent ARM Linux kernels (>= 3.x on PXA) want a device tree passed
 * in r2 instead of an ATAG list.  The kernel recognises what r2 points
 * at by looking at the magic word: 0xd00dfeed means "device tree",
 * anything else is treated as ATAGs.
 *
 * This file implements just enough of the libfdt format to
 *
 *   - sanity-check a DTB before we commit to booting with it, and
 *   - inject /chosen properties (bootargs, linux,initrd-start/end) so
 *     that the command line edited in the Cocoboot GUI and the loaded
 *     initrd image are passed to the kernel, exactly like the ATAG
 *     path does.
 *
 * The code is deliberately self contained (no libfdt dependency): it
 * runs on the ARM side with the MMU switched off, in a very hostile
 * environment.  All operations work on a private copy of the blob
 * that carries FDT_SLACK extra padding bytes, so insertions never
 * overflow.
 *
 * This program is free software ; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <PalmOS.h>
#include "dtb.h"

#define FDT_MAGIC	0xd00dfeed

#define FDT_BEGIN_NODE	0x00000001
#define FDT_END_NODE	0x00000002
#define FDT_PROP	0x00000003
#define FDT_NOP		0x00000004
#define FDT_END		0x00000009

#define FDT_ALIGN(x, a)	(((x) + ((a) - 1)) & ~((UInt32)(a) - 1))

/* fdt_header field offsets */
#define HDR_TOTALSIZE		4
#define HDR_OFF_DT_STRUCT	8
#define HDR_OFF_DT_STRINGS	12
#define HDR_VERSION		20
#define HDR_SIZE_DT_STRINGS	32
#define HDR_SIZE_DT_STRUCT	36

struct fdt_ctx {
	UInt8 *b;		/* blob base */
	UInt32 total;		/* current totalsize (may grow) */
	UInt32 buf_size;	/* size of the buffer the blob lives in
				 * (total + unused slack) */
	UInt32 struct_off;
	UInt32 struct_size;
	UInt32 strings_off;
	UInt32 strings_size;
};

/* big endian accessors (FDT is big endian, the ARM runs little endian) */
static UInt32 be_get(const UInt8 *p)
{
	return EndianSwap32(*(const UInt32 *)p);
}

static void be_put(UInt8 *p, UInt32 v)
{
	*(UInt32 *)p = EndianSwap32(v);
}

/* 64 bit big endian store (initrd addresses are 2-cell properties) */
static void be_put64(UInt8 *p, UInt32 hi, UInt32 lo)
{
	be_put(p, hi);
	be_put(p + 4, lo);
}

static UInt32 fdt_strlen(const char *s)
{
	UInt32 i = 0;
	while (s[i]) i++;
	return i;
}

static int fdt_streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static void fdt_strcpy(char *dest, const char *src)
{
	while ((*dest++ = *src++))
		;
}

static void fdt_memcpy(UInt8 *dest, const UInt8 *src, UInt32 n)
{
	while (n--)
		*dest++ = *src++;
}

static void fdt_memmove(UInt8 *dest, const UInt8 *src, UInt32 n)
{
	if (dest == src)
		return;
	if (dest < src) {
		while (n--)
			*dest++ = *src++;
	} else {
		dest += n;
		src += n;
		while (n--)
			*--dest = *--src;
	}
}

static void fdt_sync_header(struct fdt_ctx *c)
{
	be_put(c->b + HDR_TOTALSIZE, c->total);
	be_put(c->b + HDR_OFF_DT_STRUCT, c->struct_off);
	be_put(c->b + HDR_OFF_DT_STRINGS, c->strings_off);
	be_put(c->b + HDR_SIZE_DT_STRINGS, c->strings_size);
	be_put(c->b + HDR_SIZE_DT_STRUCT, c->struct_size);
}

/*
 * Parse and sanity check the header.  max_size is the size of the
 * buffer the blob lives in (>= totalsize).
 */
static int fdt_open(struct fdt_ctx *c, void *blob, UInt32 buf_size)
{
	c->b = (UInt8 *)blob;
	c->buf_size = buf_size;

	if (buf_size < 40)
		return -3;
	if (be_get(c->b) != FDT_MAGIC)
		return -1;
	if (be_get(c->b + HDR_VERSION) < 16)
		return -2;

	c->total	= be_get(c->b + HDR_TOTALSIZE);
	c->struct_off	= be_get(c->b + HDR_OFF_DT_STRUCT);
	c->strings_off	= be_get(c->b + HDR_OFF_DT_STRINGS);
	c->strings_size	= be_get(c->b + HDR_SIZE_DT_STRINGS);
	c->struct_size	= be_get(c->b + HDR_SIZE_DT_STRUCT);

	if (c->total > buf_size)
		return -3;
	if (c->struct_off + c->struct_size > c->total)
		return -3;
	if (c->strings_off + c->strings_size > c->total)
		return -3;
	/* the insert logic assumes the string block trails the struct
	 * block, which is how dtc lays blobs out */
	if (c->strings_off < c->struct_off + c->struct_size)
		return -3;

	return 0;
}

/*
 * Walk the struct block.  Records the token offset of the first
 * "chosen" node directly below the root (0 if absent) in *chosen and
 * the offset of the root node's FDT_END_NODE token in *root_end.
 * Returns 0 if the walk completed cleanly.
 */
static int fdt_walk(struct fdt_ctx *c, UInt32 *chosen, UInt32 *root_end)
{
	UInt32 p = c->struct_off;
	UInt32 end = c->struct_off + c->struct_size;
	UInt32 tok, nlen;
	int depth = 0;

	*chosen = 0;
	*root_end = 0;

	for (;;) {
		if (p + 4 > end)
			return -4;
		tok = be_get(c->b + p);

		switch (tok) {
		case FDT_BEGIN_NODE:
			/* find the NUL, it must be inside the block */
			nlen = 0;
			while (p + 4 + nlen < end && c->b[p + 4 + nlen])
				nlen++;
			if (p + 4 + nlen >= end)
				return -4;
			if (depth == 1 && *chosen == 0 &&
			    nlen == 6 && fdt_streq((const char *)c->b + p + 4, "chosen"))
				*chosen = p;
			p += 4 + FDT_ALIGN(nlen + 1, 4);
			depth++;
			break;

		case FDT_END_NODE:
			depth--;
			if (depth == 0) {
				*root_end = p;
				return 0;
			}
			if (depth < 0)
				return -4;
			p += 4;
			break;

		case FDT_PROP:
			if (p + 12 > end)
				return -4;
			p += 12 + FDT_ALIGN(be_get(c->b + p + 4), 4);
			break;

		case FDT_NOP:
			p += 4;
			break;

		default:
			/* unknown token (or premature FDT_END) */
			return -4;
		}
	}
}

/*
 * Look for a property inside the node whose BEGIN_NODE token sits at
 * 'node'.  Returns the property's token offset, or 0 if absent.
 */
static UInt32 fdt_find_prop(struct fdt_ctx *c, UInt32 node, const char *name)
{
	UInt32 end = c->struct_off + c->struct_size;
	UInt32 nlen = fdt_strlen((const char *)c->b + node + 4);
	UInt32 p = node + 4 + FDT_ALIGN(nlen + 1, 4);
	UInt32 tok, nameoff;

	while (p + 12 <= end) {
		tok = be_get(c->b + p);
		if (tok == FDT_PROP) {
			nameoff = be_get(c->b + p + 8);
			if (nameoff < c->strings_size &&
			    fdt_streq((const char *)c->b + c->strings_off + nameoff,
				      name))
				return p;
			p += 12 + FDT_ALIGN(be_get(c->b + p + 4), 4);
		} else if (tok == FDT_NOP) {
			p += 4;
		} else {
			/* END_NODE / BEGIN_NODE: properties are over */
			break;
		}
	}
	return 0;
}

/*
 * Add 'name' to the string block, reusing an identical existing
 * string if there is one.  Returns the name offset, or 0xffffffff if
 * the remaining slack cannot hold the new string.
 */
static UInt32 fdt_add_string(struct fdt_ctx *c, const char *name)
{
	UInt32 off = 0, len = fdt_strlen(name) + 1;

	while (off < c->strings_size) {
		if (fdt_streq((const char *)c->b + c->strings_off + off, name))
			return off;
		off += fdt_strlen((const char *)c->b + c->strings_off + off) + 1;
	}

	if (c->total + len > c->buf_size)
		return 0xffffffffu;

	off = c->strings_size;
	fdt_strcpy((char *)c->b + c->strings_off + off, name);
	c->strings_size += len;
	c->total += len;
	fdt_sync_header(c);
	return off;
}

/*
 * Splice 'len' bytes from 'buf' into the blob at offset 'off',
 * shifting everything above it (rest of struct block + string block)
 * upwards and fixing up the header.  'off' must point inside the
 * struct block; the caller must have checked that 'len' fits into the
 * remaining slack.
 */
static void fdt_insert(struct fdt_ctx *c, UInt32 off,
		       const UInt8 *buf, UInt32 len)
{
	UInt32 tail = c->total - off;

	fdt_memmove(c->b + off + len, c->b + off, tail);
	fdt_memcpy(c->b + off, buf, len);

	c->total += len;
	c->struct_size += len;
	c->strings_off += len;
	fdt_sync_header(c);
}

/*
 * Set (create or replace) the property 'name' in the /chosen node,
 * creating the node itself at the end of the root node if needed.
 */
static int fdt_set_prop(struct fdt_ctx *c, const char *name,
			const UInt8 *val, UInt32 vlen)
{
	UInt8 scratch[512];
	UInt32 chosen, root_end, prop, nameoff, pos;
	UInt32 oldlen, oldtot, newtot, slen, i;
	Int32 delta;

	if (fdt_walk(c, &chosen, &root_end))
		return -4;

	nameoff = fdt_add_string(c, name);
	if (nameoff == 0xffffffffu)
		return -5;
	newtot = 12 + FDT_ALIGN(vlen, 4);

	if (chosen) {
		prop = fdt_find_prop(c, chosen, name);
		if (prop) {
			/* replace the value, resizing in place.  The
			 * memmove shifts everything above the property
			 * (including the string block) by delta, so all
			 * three size/offset fields must follow. */
			oldlen = be_get(c->b + prop + 4);
			oldtot = 12 + FDT_ALIGN(oldlen, 4);
			delta = (Int32)newtot - (Int32)oldtot;
			if (delta > 0 && c->total + (UInt32)delta > c->buf_size)
				return -5;
			fdt_memmove(c->b + prop + newtot,
				    c->b + prop + oldtot,
				    c->total - (prop + oldtot));
			be_put(c->b + prop + 4, vlen);
			fdt_memcpy(c->b + prop + 12, val, vlen);
			for (i = vlen; i < FDT_ALIGN(vlen, 4); i++)
				c->b[prop + 12 + i] = 0;
			c->total = (UInt32)((Int32)c->total + delta);
			c->struct_size = (UInt32)((Int32)c->struct_size + delta);
			c->strings_off = (UInt32)((Int32)c->strings_off + delta);
			fdt_sync_header(c);
			return 0;
		}

		/* insert a fresh property as the first one in /chosen */
		if (newtot > sizeof(scratch))
			return -5;
		if (c->total + newtot > c->buf_size)
			return -5;
		be_put(scratch, FDT_PROP);
		be_put(scratch + 4, vlen);
		be_put(scratch + 8, nameoff);
		fdt_memcpy(scratch + 12, val, vlen);
		for (i = vlen; i < FDT_ALIGN(vlen, 4); i++)
			scratch[12 + i] = 0;
		pos = chosen + 4 +
		      FDT_ALIGN(fdt_strlen((const char *)c->b + chosen + 4) + 1, 4);
		fdt_insert(c, pos, scratch, newtot);
		return 0;
	}

	/* no /chosen yet: build a whole node and splice it in before
	 * the root's END_NODE token */
	if (newtot + 16 > sizeof(scratch))
		return -5;
	slen = 0;
	be_put(scratch + slen, FDT_BEGIN_NODE);
	slen += 4;
	fdt_memcpy(scratch + slen, (const UInt8 *)"chosen", 7);
	scratch[slen + 7] = 0;			/* padding byte */
	slen += 8;				/* "chosen\0" padded to 8 */
	be_put(scratch + slen, FDT_PROP);
	slen += 4;
	be_put(scratch + slen, vlen);
	slen += 4;
	be_put(scratch + slen, nameoff);
	slen += 4;
	fdt_memcpy(scratch + slen, val, vlen);
	slen += vlen;
	for (i = vlen; i < FDT_ALIGN(vlen, 4); i++)
		scratch[slen++] = 0;
	be_put(scratch + slen, FDT_END_NODE);
	slen += 4;

	if (c->total + slen > c->buf_size)
		return -5;
	fdt_insert(c, root_end, scratch, slen);
	return 0;
}

int dtb_validate(void *fdt, UInt32 size)
{
	struct fdt_ctx c;
	UInt32 chosen, root_end;
	int err;

	err = fdt_open(&c, fdt, size);
	if (err)
		return err;
	return fdt_walk(&c, &chosen, &root_end);
}

int dtb_prepare(void *dest, void *src, UInt32 size, const char *cmdline,
		UInt32 initrd_start, UInt32 initrd_size)
{
	struct fdt_ctx c;
	UInt8 val[8];
	UInt32 i;
	int err;

	/* copy the blob word-wise, then zero the slack area */
	for (i = 0; i < size; i += 4)
		*((UInt32 *)dest + (i >> 2)) = *((UInt32 *)src + (i >> 2));
	for (i = size; i < size + FDT_SLACK; i++)
		*((UInt8 *)dest + i) = 0;

	err = fdt_open(&c, dest, size + FDT_SLACK);
	if (err)
		return err;

	/* every mutation below is budget-checked against the slack:
	 * totalsize only ever grows by the bytes actually inserted */

	/* command line -> /chosen/bootargs (blank keeps the DTB's own) */
	if (cmdline) {
		const char *s = cmdline;
		while (*s == ' ') s++;
		if (*s) {
			err = fdt_set_prop(&c, "bootargs",
					   (const UInt8 *)s, fdt_strlen(s) + 1);
			if (err)
				return err;
		}
	}

	/* initrd -> /chosen/linux,initrd-start + linux,initrd-end */
	if (initrd_size) {
		be_put64(val, 0, initrd_start);
		err = fdt_set_prop(&c, "linux,initrd-start", val, 8);
		if (err)
			return err;
		be_put64(val, 0, initrd_start + initrd_size);
		err = fdt_set_prop(&c, "linux,initrd-end", val, 8);
		if (err)
			return err;
	}

	return 0;
}
