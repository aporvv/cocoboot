#!/bin/sh
# Build and run the host unit tests for arm/dtb.c
set -e
cd "$(dirname "$0")"

gcc -Wall -Wextra -Wno-unused-parameter -O1 -g \
	-Ifakeinc -I../arm -I../include -o /tmp/dtbtest dtbtest.c ../arm/dtb.c

echo "=== test 1: DTB with existing /chosen, cmdline + initrd ==="
dtc -q -I dts -O dtb -o /tmp/base.dtb t5.dts
/tmp/dtbtest /tmp/base.dtb /tmp/out1.dtb "console=ttyS0,115200 root=/dev/mmcblk0p1" 0xa0400000 0x123456
dtc -q -I dtb -O dts -o /tmp/out1.dts /tmp/out1.dtb
grep -q 'bootargs = "console=ttyS0,115200 root=/dev/mmcblk0p1"' /tmp/out1.dts \
	&& echo "PASS: bootargs replaced"
grep -q "0xa0400000" /tmp/out1.dts \
	&& echo "PASS: initrd-start"
grep -q "0xa0523456" /tmp/out1.dts \
	&& echo "PASS: initrd-end"

echo "=== test 2: DTB without /chosen, cmdline only ==="
dtc -q -I dts -O dtb -o /tmp/nochosen.dtb nochosen.dts
/tmp/dtbtest /tmp/nochosen.dtb /tmp/out2.dtb "root=/dev/mmcblk0p2 earlyprintk"
dtc -q -I dtb -O dts -o /tmp/out2.dts /tmp/out2.dtb
grep -q 'bootargs = "root=/dev/mmcblk0p2 earlyprintk"' /tmp/out2.dts \
	&& echo "PASS: chosen created with bootargs"
grep -c 'chosen' /tmp/out2.dts | grep -q '^1$' && echo "PASS: exactly one chosen node"

echo "=== test 3: blank cmdline keeps the DTB's own bootargs ==="
/tmp/dtbtest /tmp/base.dtb /tmp/out3.dtb "   "
dtc -q -I dtb -O dts -o /tmp/out3.dts /tmp/out3.dtb
grep -q 'bootargs = "ORIGINAL_ARGS"' /tmp/out3.dts \
	&& echo "PASS: original bootargs kept"

echo "=== test 4: long cmdline (255 chars) with initrd ==="
LONG=$(printf 'a%.0s' $(seq 1 200))
/tmp/dtbtest /tmp/base.dtb /tmp/out4.dtb "root=/dev/ram0 $LONG" 0xa1000000 0x800000
dtc -q -I dtb -O dts -o /tmp/out4.dts /tmp/out4.dtb
grep -q "$LONG" /tmp/out4.dts && echo "PASS: long cmdline survives"
grep -q "0xa1800000" /tmp/out4.dts && echo "PASS: initrd-end"

echo "=== test 5: patched blob round-trips through dtc/fdtput ==="
fdtget /tmp/out1.dtb /chosen bootargs >/dev/null && echo "PASS: fdtget reads it"
fdtget /tmp/out1.dtb /chosen linux,initrd-start && echo "PASS: fdtget initrd-start"

echo
echo "ALL TESTS DONE"
