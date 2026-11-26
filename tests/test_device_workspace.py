#!/usr/bin/env python3
"""No SSH: exercise disk preflight and cleanup on normal/error paths."""
import contextlib
import io
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "dev"))
from device_workspace import DeviceWorkspace, RESERVE_KIB


class FakeRemote:
    def __init__(self, free="222020\n", directory="/tmp/zte-mesh-db.ABC123\n", fail=None):
        self.free, self.directory, self.fail = free, directory, fail
        self.calls = []

    def __call__(self, command, label):
        self.calls.append((command, label))
        if label == self.fail:
            return subprocess.CompletedProcess(command, 1, "", "injected failure")
        text = self.free if label == "tmp-space-before" else self.directory if label == "mkdir" else ""
        return subprocess.CompletedProcess(command, 0, text, "")


class WorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.archive = Path(self.temp.name) / "fixture.tar.gz"
        with tarfile.open(self.archive, "w:gz") as tar:
            item = tarfile.TarInfo("root/file")
            item.size = 1000
            tar.addfile(item, io.BytesIO(b"x" * 1000))

    def tearDown(self):
        self.temp.cleanup()

    def test_success_cleanup_exact_directory(self):
        remote = FakeRemote()
        with DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db.") as ws:
            self.assertFalse(ws.removed)
        self.assertTrue(ws.removed)
        cleanup = remote.calls[-1][0]
        self.assertIn("rm -rf -- /tmp/zte-mesh-db.ABC123", cleanup)
        self.assertNotIn("*", cleanup)
        self.assertIn("test ! -L", cleanup)
        self.assertIn("cleanup deferred", cleanup)

    def test_upload_error_cleanup(self):
        remote = FakeRemote()
        with self.assertRaisesRegex(RuntimeError, "upload"):
            with DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db."):
                raise RuntimeError("upload failure")
        self.assertEqual(remote.calls[-1][1], "cleanup")

    def test_system_exit_cleanup(self):
        remote = FakeRemote()
        with self.assertRaises(SystemExit):
            with DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db."):
                raise SystemExit("checksum failure")
        self.assertEqual(remote.calls[-1][1], "cleanup")

    def test_keyboard_interrupt_cleanup(self):
        remote = FakeRemote()
        with self.assertRaises(KeyboardInterrupt):
            with DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db."):
                raise KeyboardInterrupt()
        self.assertEqual(remote.calls[-1][1], "cleanup")

    def test_low_space_no_mkdir(self):
        remote = FakeRemote(free="1024\n")
        with self.assertRaisesRegex(RuntimeError, "too full"):
            with DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db."):
                self.fail("must not enter")
        self.assertEqual([x[1] for x in remote.calls], ["tmp-space-before"])

    def test_exact_space_boundary(self):
        remote = FakeRemote(free=str(RESERVE_KIB + 512 + 4))
        with DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db.") as ws:
            self.assertEqual(ws.required_kib, ws.free_kib)
        self.assertTrue(ws.removed)

    def test_bad_space_no_mkdir(self):
        for text in ("", "10 20", "-10", "10000\nextra"):
            remote = FakeRemote(free=text)
            with self.subTest(text=text), self.assertRaisesRegex(RuntimeError, "cannot verify"):
                DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db.").__enter__()
            self.assertEqual(len(remote.calls), 1)

    def test_space_command_failure(self):
        remote = FakeRemote(fail="tmp-space-before")
        with self.assertRaisesRegex(RuntimeError, "cannot verify"):
            DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db.").__enter__()

    def test_prefix_whitelist(self):
        for prefix in ("/tmp/", "/tmp/other.", "/tmp/zte-mesh-db.X/../", "/etc/"):
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                DeviceWorkspace(FakeRemote(), self.archive, prefix)

    def test_mkdir_failure_no_cleanup_of_unknown_path(self):
        remote = FakeRemote(fail="mkdir")
        with self.assertRaisesRegex(RuntimeError, "allocate"):
            DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db.").__enter__()
        self.assertEqual(remote.calls[-1][1], "mkdir")

    def test_bad_directory_rejected(self):
        for path in ("/tmp/other.ABC123", "/tmp/zte-mesh-db.ABC123/../other", "/tmp/zte-mesh-db.ABC123 extra", "/tmp/zte-mesh-db.abc"):
            remote = FakeRemote(directory=path)
            with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, "allocate"):
                DeviceWorkspace(remote, self.archive, "/tmp/zte-mesh-db.").__enter__()
            self.assertEqual(remote.calls[-1][1], "mkdir")

    def test_cleanup_failure_not_a_pass(self):
        with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
            with DeviceWorkspace(FakeRemote(fail="cleanup"), self.archive, "/tmp/zte-mesh-db."):
                pass

    def test_cleanup_failure_preserves_original_error(self):
        with contextlib.redirect_stderr(io.StringIO()) as warnings:
            with self.assertRaisesRegex(RuntimeError, "upload failure"):
                with DeviceWorkspace(FakeRemote(fail="cleanup"), self.archive, "/tmp/zte-mesh-db."):
                    raise RuntimeError("upload failure")
        self.assertIn("WARNING:", warnings.getvalue())

    def test_controller_and_loader_prefixes(self):
        for prefix in ("/tmp/zte-mesh-db.ctrl.", "/tmp/zte-mesh-probe."):
            remote = FakeRemote(directory=prefix + "ABC123")
            with DeviceWorkspace(remote, self.archive, prefix) as ws:
                self.assertEqual(ws.directory, prefix + "ABC123")
            self.assertTrue(ws.removed)


if __name__ == "__main__":
    unittest.main()
