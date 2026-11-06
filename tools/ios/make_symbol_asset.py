#!/usr/bin/env python3
"""Writes the crash symbol asset the diagnostics Worker symbolicates with.

  make_symbol_asset.py --exe EXE --dwarf DWARF_OR_EXE [--lines] --uuid UUID \
      --textvmaddr 0x100000000 --root REPO_ROOT --out symbols-UUID.bin

Called by tools/ios/make_symbol_map.sh. Format:

  16 bytes   "xsym2 " + the header's length, 10 digits, zero padded
  header     JSON {v: 2, uuid, textvmaddr, chunks: [[startOffset, byteOffset, byteLength], ...]}
  chunks     gzip, concatenated; byteOffset counts from the end of the header

A chunk is gzip of the JSON {s: [strings], c: [chains], r: [delta, chain, ...]}:
  r      entries, each "from this address on" (until the next entry, or the next
         chunk): the address as a delta from the previous entry (the first from
         the chunk's startOffset, which is the chunk's first address) and a chain;
  c[i]   frames, innermost first, three numbers each: name and file as indexes
         into s (file -1: none) and the line (0: none). The last frame is the
         function the code belongs to, the ones before it were inlined into it;
  chain -1: no symbol.
"""
import argparse
import concurrent.futures
import gzip
import json
import os
import re
import subprocess
import sys
import time

ENTRIES_PER_CHUNK = 900
ATOS_WORKERS = 4
ATOS_SHARD = 25000  # addresses per atos run
ATOS_LINE = re.compile(r'^(.*) \(in [^()]*\)(?: \((.*):(\d+)\)| \+ \d+)?$')
SAFE_PATH = re.compile(r'^[A-Za-z0-9_.+\-/<>@ ]+$')


def repo_path(path, root):
    """A source path as the Worker shows it: repo-relative, '<sdk>/...' for the SDK
    and toolchain headers, '<ext>/name' for anything else outside the repo."""
    if not path:
        return None
    if path.startswith(root + '/'):
        rel = path[len(root) + 1:]
        if SAFE_PATH.match(rel) and '..' not in rel.split('/'):
            return rel
        return '<ext>/' + re.sub(r'[^A-Za-z0-9_.+\-]', '_', os.path.basename(rel))
    base = re.sub(r'[^A-Za-z0-9_.+\-]', '_', os.path.basename(path))
    if '/usr/include/' in path and ('/SDKs/' in path or '/Contents/Developer/' in path):
        rest = path.split('/usr/include/', 1)[1]
        return '<sdk>/' + (rest if SAFE_PATH.match(rest) and '..' not in rest.split('/') else base)
    if '/Contents/Developer/' in path or '/SDKs/' in path:
        return '<sdk>/' + base
    if not path.startswith('/') and SAFE_PATH.match(path) and '..' not in path.split('/'):
        return path
    return '<ext>/' + base


def function_addresses(exe, textvmaddr):
    out = subprocess.run(['nm', '-n', '-U', exe], check=True, capture_output=True, text=True,
                         errors='replace').stdout
    addrs = set()
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in ('t', 'T'):
            addrs.add(int(parts[0], 16))
    return {a for a in addrs if a >= textvmaddr}


def line_table_addresses(dwarf, textvmaddr):
    """Every address a line table row starts at (and every sequence's end)."""
    proc = subprocess.Popen(['xcrun', 'llvm-dwarfdump', '--debug-line', dwarf],
                            stdout=subprocess.PIPE, text=True, errors='replace')
    addrs = set()
    for line in proc.stdout:
        if line.startswith('0x') and len(line) > 18 and line[18] == ' ':
            a = int(line[:18], 16)
            if a >= textvmaddr:
                addrs.add(a)
    proc.wait()
    return addrs


def atos_shard(dwarf, textvmaddr, addrs):
    data = ''.join('0x%x\n' % a for a in addrs)
    out = subprocess.run(['atos', '-o', dwarf, '-arch', 'arm64', '-l', hex(textvmaddr), '-i', '-fullPath'],
                         input=data, check=True, capture_output=True, text=True, errors='replace').stdout
    blocks = out.split('\n\n')
    if blocks and blocks[-1].strip() == '':
        blocks.pop()
    if len(blocks) != len(addrs):
        raise SystemExit('atos answered %d blocks for %d addresses' % (len(blocks), len(addrs)))
    return blocks


def parse_block(block, root):
    """((name, file, line), ...) innermost first, or None when atos found nothing."""
    chain = []
    for line in block.split('\n'):
        m = ATOS_LINE.match(line)
        if not m:
            return None
        name, path, number = m.group(1), m.group(2), m.group(3)
        chain.append((name, repo_path(path, root), int(number) if number else 0))
    return tuple(chain) or None


def build_chunk(entries, base):
    strings, index = [], {}

    def sid(s):
        i = index.get(s)
        if i is None:
            i = index[s] = len(strings)
            strings.append(s)
        return i

    chains, chain_ids, flat, prev = [], {}, [], base
    for addr, chain in entries:
        cid = -1
        if chain is not None:
            cid = chain_ids.get(chain)
            if cid is None:
                cid = chain_ids[chain] = len(chains)
                chains.append([x for name, path, line in chain
                               for x in (sid(name), sid(path) if path else -1, line)])
        flat += [addr - prev, cid]
        prev = addr
    return json.dumps({'s': strings, 'c': chains, 'r': flat}, separators=(',', ':'), ensure_ascii=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--dwarf', required=True)
    ap.add_argument('--uuid', required=True)
    ap.add_argument('--textvmaddr', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--lines', action='store_true', help='the DWARF file has line tables')
    args = ap.parse_args()
    textvmaddr = int(args.textvmaddr, 16)
    root = args.root.rstrip('/')
    t0 = time.time()

    addrs = function_addresses(args.exe, textvmaddr)
    nm_count = len(addrs)
    if args.lines:
        addrs |= line_table_addresses(args.dwarf, textvmaddr)
    addrs = sorted(addrs)
    print('addresses: %d (%d functions) in %.1fs' % (len(addrs), nm_count, time.time() - t0), file=sys.stderr)

    # atos over shards of consecutive addresses, a few at a time; the results are
    # taken in order and consecutive addresses with the same answer merged.
    size = ATOS_SHARD
    shards = [addrs[i:i + size] for i in range(0, len(addrs), size)]
    entries, last = [], None
    with concurrent.futures.ThreadPoolExecutor(ATOS_WORKERS) as pool:
        futures = [pool.submit(atos_shard, args.dwarf, textvmaddr, s) for s in shards]
        for shard, future in zip(shards, futures):
            for addr, block in zip(shard, future.result()):
                chain = parse_block(block, root)
                if chain != last or not entries:
                    entries.append((addr, chain))
                    last = chain
    print('entries: %d in %.1fs' % (len(entries), time.time() - t0), file=sys.stderr)

    chunks, blobs, offset = [], [], 0
    for i in range(0, len(entries), ENTRIES_PER_CHUNK):
        part = entries[i:i + ENTRIES_PER_CHUNK]
        blob = gzip.compress(build_chunk(part, part[0][0]).encode('utf-8'), 9, mtime=0)
        chunks.append([part[0][0] - textvmaddr, offset, len(blob)])
        blobs.append(blob)
        offset += len(blob)
    header = json.dumps({'v': 2, 'uuid': args.uuid, 'textvmaddr': textvmaddr, 'chunks': chunks},
                        separators=(',', ':')).encode('ascii')
    with open(args.out, 'wb') as f:
        f.write(b'xsym2 %010d' % len(header))
        f.write(header)
        for blob in blobs:
            f.write(blob)
    print('%s: %d chunks, %d bytes (header %d), %.1fs' % (
        args.out, len(chunks), 16 + len(header) + offset, len(header), time.time() - t0), file=sys.stderr)


if __name__ == '__main__':
    main()
