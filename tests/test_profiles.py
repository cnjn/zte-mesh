#!/usr/bin/env python3
"""Offline WLAN/identity metadata and no-default-read regression tests."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "dev"))
import profile_schema as profiles

ARTIFACTS = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "out"
SOURCE = profiles.TABLE_DEFS
CSPD = ARTIFACTS / "analysis/cspd"
LIBDB = ARTIFACTS / "runtime/lib/libdb.so"


class Profiles(unittest.TestCase):
    def changed(self, table, old, new):
        text = (SOURCE / profiles.SPECS[table]["file"]).read_text()
        self.assertIn(old, text)
        with tempfile.TemporaryDirectory() as directory:
            f = Path(directory) / "bad.def"
            f.write_text(text.replace(old, new, 1))
            with self.assertRaises(ValueError):
                profiles.registry(table, f)

    def test_exact_layouts(self):
        for name, spec in profiles.SPECS.items():
            r = profiles.registry(name)
            self.assertEqual((len(r["fields"]), r["size"]), (spec["count"], spec["size"]))
            self.assertFalse(r["controller_ready"])
            self.assertFalse(r["wlan_profile_ready"])
            self.assertFalse(r["runtime_radio_limits_recovered"])

    def test_total_fields(self):
        self.assertEqual(sum(len(profiles.registry(n)["fields"]) for n in profiles.SPECS), 180)

    def test_keys(self):
        for n in profiles.SPECS:
            keys = [f["name"] for f in profiles.registry(n)["fields"] if f["primary"]]
            self.assertEqual(keys, [] if n == "DevInfo" else ["ViewName"])

    def test_public_defaults(self):
        for n in profiles.SPECS:
            for f in profiles.registry(n)["fields"]:
                self.assertEqual(f["default"], profiles.public_default(f["type"]))
                if f["type"] < 12:
                    self.assertEqual(f["default"], "")

    def test_source_metadata(self):
        for name in profiles.SPECS:
            profiles.verify(CSPD, name)

    def test_native_lengths(self):
        profiles.verify_lengths(LIBDB)

    def test_source_defaults_never_dereferenced(self):
        original = profiles._decode_constructor
        def poison_defaults(*args, **kwargs):
            image, table, domains = original(*args, **kwargs)
            domains = bytearray(domains)
            # Invalid pointer in EVERY source-default slot, including binary,
            # numeric and string fields. Recovery must still yield identical
            # public metadata/independent zeros without touching these bytes.
            for i in range(0, len(domains), 68):
                domains[i + 24:i + 28] = b"\xed\xfe\xad\xde"
            return image, table, bytes(domains)
        with patch.object(profiles, "_decode_constructor", poison_defaults):
            for name in profiles.SPECS:
                profiles.verify(CSPD, name)

    def test_unknown_table(self):
        for f in (profiles.registry, lambda n: profiles.recover_fields(CSPD, n)):
            with self.assertRaises(ValueError):
                f("NotATable")

    def test_unknown_type(self):
        self.changed("WLANPSK", '"ViewName", 2,', '"ViewName", 10,')

    def test_password_default_forbidden(self):
        self.changed("WLANPSK", '"KeyPassphrase", 3, 0, 1, ""', '"KeyPassphrase", 3, 0, 1, "not-allowed"')

    def test_serial_default_forbidden(self):
        self.changed("DevInfo", '"SerialNumber", 3, 0, 0, ""', '"SerialNumber", 3, 0, 0, "not-allowed"')

    def test_wrong_binary_default(self):
        self.changed("WLANBase", '"PeerBSSID", 18, 0, 1, "00:00:00:00:00:00"',
                     '"PeerBSSID", 18, 0, 1, "11:22:33:44:55:66"')

    def test_bad_primary(self):
        self.changed("WLANPSK", '"ViewName", 2, 1,', '"ViewName", 2, 0,')

    def test_bad_primary_domain(self):
        self.changed("DevInfo", '"ModelName", 3, 0,', '"ModelName", 3, 1,')

    def test_bad_saved_flag(self):
        self.changed("WLANPSK", '"ViewName", 2, 1, 1,', '"ViewName", 2, 1, 2,')

    def test_missing_field(self):
        self.changed("WLANPSK", 'PROFILE_FIELD("ViewName", 2, 1, 1, "")', '')

    def test_duplicate_field(self):
        self.changed("WLANPSK", '"WLANViewName"', '"ViewName"')

    def test_wrong_layout_size(self):
        self.changed("WLANPSK", '"KeyPassphrase", 3,', '"KeyPassphrase", 2,')

    def test_long_name(self):
        self.changed("WLANPSK", '"ViewName"', '"' + 'X' * 33 + '"')

    def test_non_ascii_name(self):
        self.changed("WLANPSK", '"ViewName"', '"密码"')

    def test_malformed_line(self):
        self.changed("WLANPSK", 'PROFILE_FIELD("ViewName", 2, 1, 1, "")', 'PROFILE_FIELD(broken)')

    def test_wrong_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            f = Path(directory) / "wrong-cspd"
            b = bytearray(CSPD.read_bytes())
            b[-1] ^= 1
            f.write_bytes(b)
            with self.assertRaisesRegex(ValueError, "baseline mismatch"):
                profiles.recover_fields(f, "WLANPSK")

    def test_dimension_model_is_bounded(self):
        spec = dict(profiles.SPECS["WLANCfg"])
        spec["dimension"] = (0x1ed48c, (1, 1))
        with patch.dict(profiles.SPECS, {"WLANCfg": spec}):
            with self.assertRaisesRegex(ValueError, "dimension call arguments"):
                profiles.recover_fields(CSPD, "WLANCfg")

    def test_unknown_constructor_call_rejected(self):
        spec = dict(profiles.SPECS["WLANPSK"])
        spec.pop("hardware")
        with patch.dict(profiles.SPECS, {"WLANPSK": spec}):
            with self.assertRaisesRegex(ValueError, "unknown initializer call"):
                profiles.recover_fields(CSPD, "WLANPSK")

    def test_emission_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            (d / 'wlan_psk.def').write_text("KEEP")
            with self.assertRaises(ValueError):
                profiles.emit(CSPD, d)
            self.assertEqual(list(d.iterdir()), [d / 'wlan_psk.def'])
            self.assertEqual((d / 'wlan_psk.def').read_text(), 'KEEP')

    def test_emission_has_no_source_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            profiles.emit(CSPD, d)
            for n, spec in profiles.SPECS.items():
                self.assertEqual(profiles.registry(n, d / spec['file']), profiles.registry(n))

    def test_failed_evidence_emits_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(profiles, 'recover_fields', side_effect=ValueError('bad evidence')):
                with self.assertRaises(ValueError):
                    profiles.emit(CSPD, Path(directory))
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_json_contains_no_source_pointer(self):
        for n in profiles.SPECS:
            text = json.dumps(profiles.registry(n))
            self.assertNotIn('default_ptr', text)
            self.assertNotIn('hardware_default', text)


if __name__ == "__main__":
    unittest.main()
