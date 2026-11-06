#!/usr/bin/env python3
"""Regression tests for the DB-table reverse engineering tool (cmapi-tables.py).

The numbers asserted here are the ones quoted in docs/research/BLOB_SHIM.md section 3.3.
If a future firmware changes them, this test fails first and forces the doc to be updated
instead of silently drifting.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / "out"
sys.path.insert(0, str(ROOT / "tools" / "lab"))

import importlib.util

_spec = importlib.util.spec_from_file_location("cmapi_tables", ROOT / "tools/lab/cmapi-tables.py")
cmapi = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cmapi)

LIBDIR = ARTIFACTS / "runtime/lib"


@unittest.skipUnless((LIBDIR / "libcfapi.so").exists(), "requires out/runtime/lib (run build.sh)")
class VendorLibraries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.libs = cmapi.load_dir(str(LIBDIR))
        cls.cfapi = cls.libs.get("libcfapi.so")
        assert cls.cfapi is not None, "libcfapi.so must be present"

    def test_string_table_has_adjacent_strings(self):
        """'\\x00(.*?)\\x00' skips every other string in .rodata; WLANPSK was the casualty."""
        for name in ("WLANCfg", "WLANPSK", "WLANBase"):
            self.assertIn(name, self.cfapi.strs.values())

    def test_db_function_tables(self):
        """Each Wlan*Db* hard-codes its table as the r0 of dbAPIGetView/dbAPISetView."""
        expected = {
            "WlanCfgDbRead": "WLANCfg", "WlanCfgDbWrite": "WLANCfg",
            "WlanBaseDbRead": "WLANBase", "WlanBaseDbWrite": "WLANBase",
            "WlanPSKDbRead": "WLANPSK", "WlanPSKDbWrite": "WLANPSK",
            "WlanSSIDInstIdDbRead": "WLANSSID",
            "WlanSTAIFDbRead": "STAIF",
            "WlanSTAProfileInstIdDbRead": "STAProfile",
            "WlanAclCfgInstIdDbWrite": "AclCfg",
            "WlanMultiAPCfgDbRead": "MultiAPCfg",
            "WlanTimeCfgDbRead": "WlanTimeCfg",
        }
        for func, table in expected.items():
            va = self.cfapi.exports[func]
            views = cmapi.db_views(self.cfapi, va)
            self.assertIn(table, views, "%s should resolve to %s, got %s"
                          % (func, table, sorted(views)))

    def test_every_db_function_resolves(self):
        """No 'view name passed in as a parameter' left: the whole family is readable."""
        unfinished, total = [], 0
        for lname, lib in self.libs.items():
            for name, va in lib.exports.items():
                if not cmapi.re.match(r"^Wlan\w*Db(Read|Write)$", name):
                    continue
                total += 1
                views = cmapi.db_views(lib, va)
                if not any(v in cmapi.TABLES for v in views):
                    unfinished.append("%s/%s" % (lname, name))
        self.assertEqual(unfinished, [], "unresolved table names: %s" % unfinished)
        self.assertGreaterEqual(total, 30, "expected the full Wlan*Db* family, got %d" % total)

    def test_db_users_cover_mesh_tables(self):
        """The Wlan*Db* naming rule alone misses the mesh side (Map*DB / CmMultiAP*)."""
        found = set()
        for lname, lib in self.libs.items():
            if not ({"dbAPIGetView", "dbAPISetView"} & set(lib.plt.values())):
                continue
            for name, va in lib.exports.items():
                for c in lib.scan(va, lib.span(va))["calls"]:
                    if c["arg0"] in cmapi.TABLES:
                        found.add(c["arg0"])
        for table in ("WLANCfg", "WLANBase", "WLANPSK", "MultiAPCfg", "MultiAPMaster",
                      "MultiAPDomain", "AclCfg", "STAIF"):
            self.assertIn(table, found)


if __name__ == "__main__":
    unittest.main()
