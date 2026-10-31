#!/usr/bin/env python3
"""Compile a LuCI .po catalog into the .lmo the dispatcher loads.

The package build does this with `po2lmo` from the OpenWrt tree (luci.mk, one
`luci-i18n-<app>-<lang>` package per po directory). This copy exists for two
things it cannot do there: installing a catalog on a lab device that was set up
by copying files instead of installing packages, and letting the test suite
prove that the catalog really compiles and really maps every string.

The format is po2lmo's, not gettext's (see modules/luci-base/src/po2lmo.c and
lib/lmo.c):

    [translated string][0..3 pad][translated string][0..3 pad] ...
    [index: 16 bytes per entry, big endian][uint32 size of the string block]

Each index entry is (key hash, plural count, offset into the block, length),
sorted by the hash. Keys hash with SuperFastHash seeded with the key length --
`sfh_hash(key, len, len)` -- and the header entry (the plural formula) goes in
with hash 0. A string whose hash equals its plural count is dropped, which is
how an untranslated msgid never makes it into the file.

usage: po2lmo.py <input.po> <output.lmo>
       po2lmo.py --list <input.lmo>
"""

import struct
import sys

MASK = 0xFFFFFFFF


def sfh_hash(data):
    """SuperFastHash as luci uses it: seeded with the length, signed tail bytes."""
    if isinstance(data, str):
        data = data.encode('utf-8')
    length = len(data)
    if length <= 0:
        return 0

    value = length
    rest = length & 3
    words = length >> 2
    at = 0

    def half(i):
        return data[i] | (data[i + 1] << 8)

    while words:
        value = (value + half(at)) & MASK
        value = ((value << 16) ^ ((half(at + 2) << 11) ^ value)) & MASK
        at += 4
        value = (value + (value >> 11)) & MASK
        words -= 1

    if rest == 3:
        value = (value + half(at)) & MASK
        value ^= (value << 16) & MASK
        third = data[at + 2]
        signed = third - 256 if third >= 128 else third
        value = (value ^ ((signed << 18) & MASK)) & MASK
        value = (value + (value >> 11)) & MASK
    elif rest == 2:
        value = (value + half(at)) & MASK
        value = (value ^ ((value << 11) & MASK)) & MASK
        value = (value + (value >> 17)) & MASK
    elif rest == 1:
        byte = data[at]
        signed = byte - 256 if byte >= 128 else byte
        value = (value + signed) & MASK
        value = (value ^ ((value << 10) & MASK)) & MASK
        value = (value + (value >> 1)) & MASK

    value = (value ^ ((value << 3) & MASK)) & MASK
    value = (value + (value >> 5)) & MASK
    value = (value ^ ((value << 4) & MASK)) & MASK
    value = (value + (value >> 17)) & MASK
    value = (value ^ ((value << 25) & MASK)) & MASK
    value = (value + (value >> 6)) & MASK
    return value


ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'"}


def _unescape(text):
    out = []
    i = 0
    while i < len(text):
        if text[i] == '\\' and i + 1 < len(text):
            out.append(ESCAPES.get(text[i + 1], text[i + 1]))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def parse_po(path):
    """[{ctxt, id, plural, str: {n: text}}] in file order."""
    entries = []
    current = {'ctxt': None, 'id': None, 'plural': None, 'str': {}}
    target = None           # (field, plural index) of the line being continued

    def flush():
        if current['id'] is not None or current['ctxt'] is not None:
            entries.append(dict(current))

    for raw in open(path, encoding='utf-8'):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue

        for prefix, field in (('msgctxt ', 'ctxt'), ('msgid_plural ', 'plural'), ('msgid ', 'id')):
            if line.startswith(prefix + '"'):
                if field == 'ctxt' and (current['id'] is not None or current['ctxt'] is not None):
                    flush()
                    current.update(ctxt=None, id=None, plural=None, str={})
                elif field == 'id' and current['id'] is not None:
                    flush()
                    current.update(ctxt=None, id=None, plural=None, str={})
                current[field] = _unescape(line[len(prefix) + 1:-1])
                target = (field, None)
                break
        else:
            if line.startswith('msgstr['):
                index = int(line[7:line.index(']')])
                current['str'][index] = _unescape(line[line.index('"') + 1:-1])
                target = ('str', index)
            elif line.startswith('msgstr "'):
                current['str'][0] = _unescape(line[8:-1])
                target = ('str', 0)
            elif line.startswith('"') and target:
                field, index = target
                if field == 'str':
                    current['str'][index] += _unescape(line[1:-1])
                else:
                    current[field] += _unescape(line[1:-1])

    flush()
    return entries


def _key(entry, index):
    if entry['plural'] is not None:
        base = '%s\2%d' % (entry['id'], index)
    else:
        base = entry['id']
    if entry['ctxt'] is not None:
        return '%s\1%s' % (entry['ctxt'], base)
    return base


def compile_po(entries):
    block = bytearray()
    index = []

    def add(key_id, plural_count, text):
        payload = text.encode('utf-8')
        index.append((key_id, plural_count, len(block), len(payload)))
        block.extend(payload)
        block.extend(b'\0' * (-len(payload) % 4))

    for entry in entries:
        # An empty msgid is the header: po2lmo only allocates msg->id when the
        # string is non-empty, so the header falls through to the branch below.
        if entry['id'] and entry['str'].get(0) is not None:
            plural_count = max(entry['str']) + 1
            for i in sorted(entry['str']):
                key_id = sfh_hash(_key(entry, i))
                # po2lmo: a string whose hash equals its plural count is identity.
                if key_id == plural_count:
                    continue
                add(key_id, plural_count, entry['str'][i])
        elif entry['str'].get(0) is not None:
            # the header: only the plural formula survives
            for line in entry['str'][0].split('\n'):
                if line.lower().startswith('plural-forms:'):
                    add(0, 0, line[len('plural-forms:'):].strip())
                    break

    index.sort(key=lambda item: item[0])

    out = bytearray(block)
    for record in index:
        out.extend(struct.pack('>IIII', *record))
    out.extend(struct.pack('>I', len(block)))
    return bytes(out)


def read_lmo(blob):
    """(index, strings) where index is [(key, plurals, offset, length)]."""
    if len(blob) < 8:
        raise ValueError('not an lmo: %d bytes' % len(blob))
    block_size = struct.unpack_from('>I', blob, len(blob) - 4)[0]
    if block_size > len(blob) - 4 or (len(blob) - 4 - block_size) % 16:
        raise ValueError('lmo index is not a multiple of 16 bytes')
    index = [struct.unpack_from('>IIII', blob, at)
             for at in range(block_size, len(blob) - 4, 16)]
    return index, blob[:block_size]


def lookup(blob, msgid):
    """What the dispatcher would answer for `msgid`, or None."""
    index, block = read_lmo(blob)
    wanted = sfh_hash(msgid)
    low, high = 0, len(index) - 1
    while low <= high:
        middle = (low + high) // 2
        key, plurals, offset, length = index[middle]
        if key == wanted:
            return block[offset:offset + length].decode('utf-8')
        if key < wanted:
            low = middle + 1
        else:
            high = middle - 1
    return None


def main(argv):
    if len(argv) == 3 and argv[1] == '--list':
        index, block = read_lmo(open(argv[2], 'rb').read())
        for key, plurals, offset, length in index:
            print('%08x  plurals=%d  %6d  %r' % (key, plurals, offset,
                                                 block[offset:offset + length].decode('utf-8')))
        return 0

    if len(argv) != 3:
        sys.stderr.write('usage: %s <input.po> <output.lmo>\n       %s --list <input.lmo>\n'
                         % (argv[0], argv[0]))
        return 2

    entries = parse_po(argv[1])
    blob = compile_po(entries)
    if not blob:
        sys.stderr.write('%s: nothing translated, no file written\n' % argv[1])
        return 1
    with open(argv[2], 'wb') as handle:
        handle.write(blob)
    print('%s -> %s: %d entries, %d bytes'
          % (argv[1], argv[2], len(read_lmo(blob)[0]), len(blob)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
