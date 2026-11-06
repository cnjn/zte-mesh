"""The Chinese catalog stays in step with the page.

Every string the view hands to `_()` has to be in po/zh_Hans/zte-mesh.po, every
entry in that file has to be used by something, and the whole thing has to
compile into the .lmo the dispatcher loads -- a translation that is present but
does not compile is invisible, which is the state the lab routers were in until
the catalog was installed by hand.

The extractor reads JavaScript string literals the way the browser would (quotes,
backslash escapes, `+` concatenation), because msgids are compared byte for byte.
"""

import json
import re
import sys
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / 'tools/lab'))

import po2lmo

APP = SOURCE / 'openwrt/luci-app-zte-mesh'
VIEW = APP / 'htdocs/luci-static/resources/view/network/zte-mesh.js'
PO = APP / 'po/zh_Hans/zte-mesh.po'
MENU = APP / 'root/usr/share/luci/menu.d/luci-app-zte-mesh.json'

ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\', "'": "'", '"': '"'}


def js_literal(source, at):
    """(decoded literal, index after it) for the quoted string starting at `at`."""
    quote = source[at]
    if quote not in "'\"":
        return None, at
    at += 1
    out = []
    while at < len(source):
        char = source[at]
        if char == '\\':
            nxt = source[at + 1]
            if nxt == 'u':
                out.append(chr(int(source[at + 2:at + 6], 16)))
                at += 6
                continue
            out.append(ESCAPES.get(nxt, nxt))
            at += 2
            continue
        if char == quote:
            return ''.join(out), at + 1
        out.append(char)
        at += 1
    return None, at


def translated_strings(path):
    """Every msgid passed to _() in a view, in source order, without duplicates."""
    source = path.read_text(encoding='utf-8')
    found = []
    for match in re.finditer(r'(?<![A-Za-z0-9_$.])_\(', source):
        at = match.end()
        parts = []
        while True:
            while at < len(source) and source[at] in ' \t\r\n':
                at += 1
            text, after = js_literal(source, at)
            if text is None:
                break
            parts.append(text)
            at = after
            while at < len(source) and source[at] in ' \t\r\n':
                at += 1
            if at < len(source) and source[at] == '+':
                at += 1
                continue
            # only a call that ends here is a plain literal; _('a' + x) is not
            if at < len(source) and source[at] in '),':
                found.extend([''.join(parts)])
            break
    return list(dict.fromkeys(found))


def menu_titles(path):
    menu = json.loads(path.read_text(encoding='utf-8'))
    return [entry['title'] for entry in menu.values() if entry.get('title')]


class CatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.strings = translated_strings(VIEW)
        cls.titles = menu_titles(MENU)
        cls.entries = po2lmo.parse_po(PO)
        cls.catalog = {}
        for entry in cls.entries:
            if entry['id']:
                cls.catalog.setdefault(entry['id'], entry['str'].get(0))

    def test_the_view_has_strings(self):
        self.assertGreater(len(self.strings), 100)

    def test_every_string_is_translated(self):
        missing = [s for s in self.strings if s not in self.catalog]
        self.assertEqual(missing, [], 'not in the catalog: %r' % missing[:3])

    def test_every_menu_title_is_translated(self):
        missing = [title for title in self.titles if title not in self.catalog]
        self.assertEqual(missing, [])

    def test_nothing_is_translated_that_is_no_longer_used(self):
        used = set(self.strings) | set(self.titles)
        stale = [msgid for msgid in self.catalog if msgid not in used]
        self.assertEqual(stale, [], 'left over in the catalog: %r' % stale[:3])

    def test_no_duplicate_or_empty_entries(self):
        ids = [entry['id'] for entry in self.entries if entry['id']]
        self.assertEqual(len(ids), len(set(ids)))
        empty = [entry['id'] for entry in self.entries if entry['id'] and not entry['str'].get(0)]
        self.assertEqual(empty, [])

    def test_format_specifiers_survive_translation(self):
        specifier = re.compile(r'%(?:%|\d+\$[-+ 0#]*\d*(?:\.\d+)?[sdifuxXc])')
        for msgid, msgstr in self.catalog.items():
            with self.subTest(msgid=msgid[:40]):
                self.assertEqual(sorted(specifier.findall(msgid or '')), sorted(specifier.findall(msgstr or '')),
                                 'placeholders differ: %r -> %r' % (msgid[:60], msgstr))

    def test_header_names_the_language_and_the_plural_rule(self):
        header = self.entries[0]
        self.assertEqual(header['id'], '')
        self.assertIn('charset=UTF-8', header['str'].get(0, ''))
        # the dispatcher reads nplurals from here; without it the catalog still
        # loads, but po2lmo writes no hash-0 entry and plural lookups fall back
        self.assertIn('Plural-Forms: nplurals=1; plural=0;', header['str'].get(0, ''))

    def test_descriptions_are_translated(self):
        # an English description that skips _() cannot be translated at all
        source = VIEW.read_text(encoding='utf-8')
        self.assertEqual(re.findall(r"\.description\s*=\s*'", source), [])
        self.assertEqual(re.findall(r"\.placeholder\s*=\s*'[^']* [^']*'", source), [])


class CompiledCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = po2lmo.parse_po(PO)
        cls.blob = po2lmo.compile_po(cls.entries)

    def test_every_translation_is_reachable_by_hash(self):
        for entry in self.entries:
            if not entry['id'] or entry['str'].get(0) is None:
                continue
            with self.subTest(msgid=entry['id'][:40]):
                self.assertEqual(po2lmo.lookup(self.blob, entry['id']), entry['str'][0])

    def test_index_is_sorted_and_the_trailer_agrees_with_it(self):
        index, block = po2lmo.read_lmo(self.blob)
        self.assertEqual([record[0] for record in index], sorted(record[0] for record in index))
        self.assertEqual(len(self.blob) - 4 - len(block), 16 * len(index))
        for _key, _plurals, offset, length in index:
            self.assertLessEqual(offset + length, len(block))

    def test_plural_formula_is_the_first_entry(self):
        # po2lmo writes it with hash 0, which sorts first and is what the
        # loader uses to learn nplurals
        index, block = po2lmo.read_lmo(self.blob)
        key, plurals, offset, length = index[0]
        self.assertEqual((key, plurals), (0, 0))
        self.assertEqual(block[offset:offset + length], b'nplurals=1; plural=0;')

    def test_an_unknown_string_stays_untranslated(self):
        self.assertIsNone(po2lmo.lookup(self.blob, 'this string is not in the catalog'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
