#!/usr/bin/env python3
"""
Splits up all the files in the images directory into 32kb chunks suitable
for adding to a prc. Also writes an image table.

If the images directory does not exist (or is empty) an image table with
just the end sentinel is written, which means "no builtin images".
"""

import os
import struct

DIR = 'images'
CHUNKSIZE = 32 * 1024
RSCNAME = 'imgP'


def chunk(fn, i):
    with open(fn, 'rb') as f:
        size = 0
        while True:
            data = f.read(CHUNKSIZE)
            if not data:
                break
            with open(RSCNAME + ('%04x' % i) + '.bin', 'wb') as out:
                out.write(data)
            size += len(data)
            i += 1

    return i, size


def main():
    if not os.path.isdir(DIR):
        os.mkdir(DIR)

    with open('iTbl0000.bin', 'wb') as table:
        i = 0
        num = 0
        for fn in sorted(os.listdir(DIR)):
            if not os.path.isfile(os.path.join(DIR, fn)):
                continue
            end, size = chunk(os.path.join(DIR, fn), i)
            print(i, end, fn)
            name = fn.encode('utf-8')[:31]
            table.write(struct.pack('>iiI1s31s', i, end, size, b'/', name))
            num += 1
            i = end

        table.write(struct.pack('>iiI1s31s', -1, -1, 0, b'/',
                                b'www.hackndev.com'))


main()
