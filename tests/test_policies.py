import copy
import sys
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tools" / "dev"))
from schema import (CONSTRUCTORS, POLICY_TABLES, constructor_data, policy_registry,
                    policy_constructor_fields, verify_policy_constructor,
                    verify_policy_registration, emit_policy_definitions)

ARTIFACTS = Path(sys.argv[1]) if len(sys.argv) == 2 else SOURCE / "out"
CSPD = ARTIFACTS / "analysis/cspd"
MASTER = (SOURCE / "src" / "multiap_master.def").read_text()
DOMAIN = (SOURCE / "src" / "multiap_domain.def").read_text()


class PolicyLayoutTests(unittest.TestCase):
    def changed(self, text, table, before, after):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "schema.def"
            path.write_text(text.replace(before, after))
            return policy_registry(table, path)

    def test_master_layout(self):
        schema = policy_registry("MultiAPMaster")
        self.assertEqual((len(schema["fields"]), schema["size"], schema["max_rows"]), (55, 2312, 1))
        self.assertEqual((schema["registration_arg5"], schema["view_flags"]), (1024, 63))
        self.assertEqual(schema["fields"][0]["value_offset"], 36)
        self.assertEqual(schema["fields"][1]["value_offset"], 100)
        self.assertFalse(schema["controller_ready"])
        self.assertFalse(schema["wlan_profile_ready"])

    def test_domain_layout_and_query_field(self):
        schema = policy_registry("MultiAPDomain")
        self.assertEqual((len(schema["fields"]), schema["size"], schema["max_rows"]), (106, 3464, 32))
        fields = {f["name"]: f for f in schema["fields"]}
        self.assertEqual(fields["ESSID"]["value_offset"], 100)
        self.assertEqual(fields["RoamRssiLmt24G"]["default"], "-60")
        self.assertEqual(fields["DomainUsed"]["record_offset"], 2952)
        self.assertEqual(fields["DomainUsed"]["save"], 0)

    def test_all_string_defaults_empty(self):
        for table in POLICY_TABLES:
            for field in policy_registry(table)["fields"]:
                if field["type"] in (2, 3, 5, 7):
                    self.assertEqual(field["default"], "")

    def test_reject_unknown_table(self):
        with self.assertRaises(ValueError):
            policy_registry("other")

    def test_reject_missing_domain_field(self):
        with self.assertRaises(ValueError):
            self.changed(DOMAIN, "MultiAPDomain", 'MESH_FIELD("DelayBSInterval", 13, 0, 1, "0")', '')

    def test_reject_duplicate_name(self):
        with self.assertRaises(ValueError):
            self.changed(MASTER, "MultiAPMaster", '"EnRoam",', '"EnMultiDomain",')

    def test_reject_primary_key_changes(self):
        with self.assertRaises(ValueError):
            self.changed(DOMAIN, "MultiAPDomain", '"ViewName", 2, 1, 1', '"ViewName", 2, 0, 1')

    def test_reject_extra_primary_key(self):
        with self.assertRaises(ValueError):
            self.changed(DOMAIN, "MultiAPDomain", '"ESSID", 2, 0, 0', '"ESSID", 2, 1, 0')

    def test_reject_nonempty_essid_registration_default(self):
        with self.assertRaisesRegex(ValueError, "nonempty"):
            self.changed(DOMAIN, "MultiAPDomain", '"ESSID", 2, 0, 0, ""', '"ESSID", 2, 0, 0, "not-a-factory-profile"')

    def test_reject_any_nonempty_string_registration_default(self):
        with self.assertRaisesRegex(ValueError, "nonempty"):
            self.changed(MASTER, "MultiAPMaster", '"SupportBSMode", 5, 0, 1, ""', '"SupportBSMode", 5, 0, 1, "unexpected"')

    def test_reject_u8_overflow(self):
        with self.assertRaises(ValueError):
            self.changed(MASTER, "MultiAPMaster", '"EnBandSteer", 15, 0, 1, "0"', '"EnBandSteer", 15, 0, 1, "256"')

    def test_reject_signed_default_overflow(self):
        with self.assertRaises(ValueError):
            self.changed(DOMAIN, "MultiAPDomain", '"RoamRssiLmt24G", 12, 0, 1, "-60"', '"RoamRssiLmt24G", 12, 0, 1, "-2147483649"')

    def test_reject_size_change(self):
        with self.assertRaises(ValueError):
            self.changed(MASTER, "MultiAPMaster", '"SupportBSMode", 5', '"SupportBSMode", 2')

    def test_reject_bad_save_flag(self):
        with self.assertRaises(ValueError):
            self.changed(MASTER, "MultiAPMaster", '"ViewName", 2, 1, 1', '"ViewName", 2, 1, 2')


class PolicyEvidenceTests(unittest.TestCase):
    def test_all_161_descriptors_match_original(self):
        for table in POLICY_TABLES:
            verify_policy_constructor(CSPD, policy_registry(table))

    def test_exact_original_view_registration(self):
        for table in POLICY_TABLES:
            verify_policy_registration(CSPD, table)

    def test_reject_changed_registry(self):
        changed = copy.deepcopy(policy_registry("MultiAPDomain"))
        changed["fields"][2]["default"] = "-65"
        with self.assertRaisesRegex(ValueError, "differs"):
            verify_policy_constructor(CSPD, changed)

    def test_reject_unapproved_constructor(self):
        with self.assertRaisesRegex(ValueError, "unknown constructor"):
            constructor_data(CSPD, "dbDefMultiAPMaster")
        self.assertEqual(len(CONSTRUCTORS), 3)

    def test_nonexecuting_generator_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            emit_policy_definitions(CSPD, Path(tmp))
            for table, spec in POLICY_TABLES.items():
                generated = policy_registry(table, Path(tmp) / spec["file"])
                self.assertEqual(generated, policy_registry(table))

    def test_generator_never_overwrites_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "multiap_master.def"
            path.write_text("do-not-overwrite\n")
            with self.assertRaisesRegex(ValueError, "overwrite"):
                emit_policy_definitions(CSPD, Path(tmp))
            self.assertEqual(path.read_text(), "do-not-overwrite\n")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
