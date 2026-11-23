import copy
import sys
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tools" / "dev"))
from schema import Image, CSPD_SHA256, registry, recover_constructor, verify_constructor, verify_lengths

ARTIFACTS = Path(sys.argv[1]) if len(sys.argv) == 2 else SOURCE / "out"
DEFINITION = (SOURCE / "src" / "multiap_cfg.def").read_text()


class RegistryTests(unittest.TestCase):
    def parse_changed(self, before, after):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "schema.def"
            path.write_text(DEFINITION.replace(before, after))
            return registry(path)

    def test_exact_layout(self):
        schema = registry()
        self.assertEqual(schema["size"], 2952)
        self.assertEqual(len(schema["fields"]), 50)
        fields = {f["name"]: f for f in schema["fields"]}
        self.assertEqual(fields["Mode"]["value_offset"], 36)
        self.assertEqual(fields["MeshName"]["value_offset"], 868)
        self.assertEqual(fields["IFLink"]["value_offset"], 1444)
        self.assertEqual(fields["NoSteeringOUIs"]["value_offset"], 1540)
        self.assertEqual(fields["EnP1905DevSave"]["value_offset"], 2948)
        self.assertFalse(schema["controller_ready"])

    def test_u8_not_assumed_boolean(self):
        field = next(f for f in registry()["fields"] if f["name"] == "TopoDiscoverInterval")
        self.assertEqual((field["type"], field["default"], field["value_length"]), (15, "60", 1))

    def test_reject_unknown_type(self):
        with self.assertRaises(ValueError):
            self.parse_changed('"Mode", 14', '"Mode", 99')

    def test_reject_missing_field(self):
        with self.assertRaises(ValueError):
            self.parse_changed('MAP_FIELD(MODE, "Mode", 14, 1, "0")', '')

    def test_reject_duplicate_name(self):
        with self.assertRaises(ValueError):
            self.parse_changed('"Enable", 15', '"Mode", 15')

    def test_reject_duplicate_id(self):
        with self.assertRaises(ValueError):
            self.parse_changed('MAP_FIELD(ENABLE,', 'MAP_FIELD(MODE,')

    def test_reject_bad_save_flag(self):
        with self.assertRaises(ValueError):
            self.parse_changed('"Mode", 14, 1', '"Mode", 14, 2')

    def test_reject_changed_total_size(self):
        with self.assertRaises(ValueError):
            self.parse_changed('"MeshName", 5', '"MeshName", 3')

    def test_reject_default_overflow(self):
        with self.assertRaises(ValueError):
            self.parse_changed('"TopoDiscoverInterval", 15, 1, "60"', '"TopoDiscoverInterval", 15, 1, "256"')

    def test_reject_nonnumeric_default(self):
        with self.assertRaises(ValueError):
            self.parse_changed('"Mode", 14, 1, "0"', '"Mode", 14, 1, "not-a-mode"')

    def test_reject_long_string_default(self):
        with self.assertRaises(ValueError):
            self.parse_changed('"IFLink", 3, 1, "br0"', '"IFLink", 3, 1, "' + 'x' * 64 + '"')


class BaselineEvidenceTests(unittest.TestCase):
    def test_all_descriptors_match_original_machine_initializer(self):
        verify_constructor(ARTIFACTS / "analysis/cspd", registry())
        self.assertEqual(len(recover_constructor(ARTIFACTS / "analysis/cspd")), 50)

    def test_lengths_match_original_db_table(self):
        verify_lengths(ARTIFACTS / "runtime/lib/libdb.so")

    def test_registry_mismatch_not_accepted(self):
        changed = copy.deepcopy(registry())
        changed["fields"][0]["default"] = "2"
        with self.assertRaisesRegex(ValueError, "registry differs"):
            verify_constructor(ARTIFACTS / "analysis/cspd", changed)

    def test_reject_wrong_baseline_hash(self):
        with self.assertRaisesRegex(ValueError, "baseline mismatch"):
            Image(ARTIFACTS / "analysis/cspd", "0" * 64)

    def test_reject_bad_elf_address(self):
        with self.assertRaisesRegex(ValueError, "outside file-backed"):
            Image(ARTIFACTS / "analysis/cspd", CSPD_SHA256).read(0xdeadbeef, 4)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
