#ifndef _DTB_H_
#define _DTB_H_

#include <PalmOS.h>
#include "shared.h"

/* Slack bytes appended to the DTB copy so that /chosen properties can
 * be inserted or replaced without ever running out of blob space.
 * The loader must reserve at least this much extra room at the
 * destination address. */
#define FDT_SLACK	4096

/* Validate the flattened device tree blob (read-only, may be called
 * while the MMU is still on).  Returns 0 if the blob is structurally
 * sane, or a negative error code:
 *	-1	bad magic
 *	-2	unsupported version (need >= 16, i.e. anything dtc emits)
 *	-3	header sizes/offsets inconsistent
 *	-4	struct block walk failed (bad token or unterminated name)
 */
int dtb_validate(void *fdt, UInt32 size);

/* Copy the DTB from src to the physical address dest, reserving
 * FDT_SLACK bytes of padding, then patch /chosen:
 *
 *	bootargs		<- cmdline      (only if cmdline is non-blank)
 *	linux,initrd-start	<- initrd_start (only if initrd_size != 0)
 *	linux,initrd-end	<- initrd_start + initrd_size
 *
 * The /chosen node is created if the DTB does not have one.  Returns 0
 * on success or a negative error code; after dtb_validate() succeeded
 * on the same blob no failure should be possible.
 */
int dtb_prepare(void *dest, void *src, UInt32 size, const char *cmdline,
		UInt32 initrd_start, UInt32 initrd_size);

#endif /* _DTB_H_ */
