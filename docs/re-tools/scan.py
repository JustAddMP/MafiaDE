#!/usr/bin/env python3
"""IDA-style byte pattern scanner for PE executables (MafiaCoop RE tooling).

Usage:
    python scan.py <exe> "<pattern>" [more patterns...]
    python scan.py <exe> --file patterns.txt      (lines: name<TAB/space>pattern ; '#' comments)

Pattern syntax: hex bytes separated by spaces, '??' or '?' = wildcard byte.
Scans the .text section only. Prints the number of matches and each match as an RVA
(relative virtual address, add the module base at runtime) plus the absolute VA for the
PE's preferred image base.
"""
import re
import sys
import struct


def load_text(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    image_base = struct.unpack_from('<Q', data, opt + 24)[0]
    sec = opt + optsz
    for i in range(nsec):
        off = sec + i * 40
        name = data[off:off + 8].rstrip(b'\0')
        vsize, va, rsize, rptr = struct.unpack_from('<IIII', data, off + 8)
        if name == b'.text':
            return data[rptr:rptr + min(vsize, rsize)], va, image_base
    raise SystemExit('no .text section')


def compile_pattern(pat):
    toks = pat.split()
    if not toks:
        raise ValueError('empty pattern')
    rx = b''
    for t in toks:
        if t in ('?', '??'):
            rx += b'.'
        else:
            rx += re.escape(bytes([int(t, 16)]))
    return re.compile(rx, re.DOTALL)


def scan(text, pat):
    rx = compile_pattern(pat)
    out = []
    pos = 0
    while True:
        m = rx.search(text, pos)
        if not m:
            break
        out.append(m.start())
        pos = m.start() + 1
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    exe = argv[1]
    text, text_va, base = load_text(exe)
    pats = []
    if argv[2] == '--file':
        for line in open(argv[3], encoding='utf-8'):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            name, _, p = line.partition('\t')
            if not p:
                name, _, p = line.partition(' ')
            pats.append((name, p.strip()))
    else:
        pats = [('pattern%d' % i, p) for i, p in enumerate(argv[2:])]
    rc = 0
    for name, p in pats:
        hits = scan(text, p)
        tag = 'OK  ' if len(hits) == 1 else 'FAIL'
        if len(hits) != 1:
            rc = 1
        print('%s %-40s %d match(es)' % (tag, name, len(hits)))
        for h in hits[:10]:
            rva = text_va + h
            print('       rva 0x%X  va 0x%X' % (rva, base + rva))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv))
