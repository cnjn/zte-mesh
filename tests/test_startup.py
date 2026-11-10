#!/usr/bin/env python3
"""Pinned metadata only; no factory default contents or native ELF execution."""
import importlib.util
import sys
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
ARTIFACTS = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else SOURCE / "out"
sys.path.insert(0, str(SOURCE / "tools" / "dev"))
spec = importlib.util.spec_from_file_location("startup_schema", SOURCE / "tools" / "dev" / "startup-schema.py")
startup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(startup)


class StartupMetadata(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tables = startup.recover(ARTIFACTS / "analysis/cspd", ARTIFACTS / "runtime/lib/libdb.so")

    def test_exact_layouts(self):
        self.assertEqual({n: (v["size"], len(v["fields"]), v["max_rows"]) for n, v in self.tables.items()}, {
            "P1905ComCfg": (328, 8, 1), "IPInterface": (2404, 48, 25),
            "P1905_ACD": (456, 9, 1), "P1905AcdBssCfg": (264, 5, 8), "AclCfg": (268, 5, 256),
            "WLANWEP": (232, 3, 32), "P1905DeviceInfo": (788, 11, 1024),
            "MultiAPRoamCatonCfg": (232, 7, 1), "WLANSetting": (520, 14, 1), "MultiAPCacheMac": (108, 2, 200)})

    def test_bss_layout_used_by_native_reader(self):
        fields = self.tables["P1905AcdBssCfg"]["fields"]
        self.assertEqual([(f["name"], f["value_offset"]) for f in fields], [
            ("ViewName", 36), ("WLANCfgInst", 100), ("WLANBaseName", 164),
            ("CustomTag", 228), ("Enable", 260)])

    def test_getter_layouts(self):
        # MapGetRoamCatonCfgDB reads 0xe8 bytes, WlanSettingDbRead 0x208; the daemon picks fields by offset.
        roam = self.tables["MultiAPRoamCatonCfg"]["fields"]
        self.assertEqual([f["name"] for f in roam], ["OfflineRetrieveEn", "TxDataAfterBaEn", "TxBaDelayMs",
                         "ZeroDropCfgEn", "BtmCacheEn", "TxConDelayMs", "TxDropCnt"])
        self.assertEqual([f["value_length"] for f in roam], [1, 1, 4, 1, 1, 4, 4])
        setting = self.tables["WLANSetting"]["fields"]
        self.assertEqual([f["name"] for f in setting][:9], ["ViewName", "DualBandSync", "SSIDSync", "AirMonitorDuration",
                         "SpecialMode", "ESSIDSuffix", "ACLIsFrom1905Slave", "MLOEnable", "MLOSSID"])
        self.assertEqual(setting[7]["value_offset"], 296 + 28)

    def test_primary_keys(self):
        self.assertEqual({n: [f["name"] for f in t["fields"] if f["primary"]] for n, t in self.tables.items()}, {
            "P1905ComCfg": [], "IPInterface": ["ViewName"], "P1905_ACD": [], "P1905AcdBssCfg": ["ViewName"], "AclCfg": ["ViewName"],
            "WLANWEP": ["ViewName"], "P1905DeviceInfo": ["ViewName", "MacAddr"],
            "MultiAPRoamCatonCfg": [], "WLANSetting": ["ViewName"],
            "MultiAPCacheMac": ["ViewName", "MacAddr"]})

    def test_defaults_not_read(self):
        for table in self.tables.values():
            self.assertIs(table["source_defaults_read"], False)
            self.assertEqual(table["source_sha256"], startup.CSPD_SHA256)

    def test_payloads_bounded(self):
        for table in self.tables.values():
            at = 8
            for field in table["fields"]:
                self.assertEqual(field["value_offset"], at + 28)
                self.assertLessEqual(field["value_offset"] + field["value_length"], at + field["record_length"])
                at += field["record_length"]
            self.assertEqual(at, table["size"])

    def test_generated_registry(self):
        self.assertEqual(startup.declaration(self.tables), (SOURCE / "src" / "startup-views.inc").read_text())


if __name__ == "__main__":
    unittest.main()
