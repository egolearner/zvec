# Standard-library unittest keeps the storage harness independent of pytest.
# ruff: noqa: PT009
"""Linux-only executable checks of the preload library built by the workflow."""

from __future__ import annotations

import errno
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class IoFaultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Path(os.environ["STORAGE_FAULT_LIBRARY"]).resolve(strict=True)

    def probe(self, operation, once=False, armed=True, after=1, path_filter=""):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / "data"
            data.mkdir()
            arm = root / "armed"
            audit = root / "audit"
            if armed:
                arm.touch()
            env = {
                **os.environ,
                "LD_PRELOAD": str(self.library),
                "ZVEC_FAULT_ROOT": str(data),
                "ZVEC_FAULT_ARM": str(arm),
                "ZVEC_FAULT_AUDIT": str(audit),
                "ZVEC_FAULT_OPERATION": operation,
                "ZVEC_FAULT_AFTER": str(after),
                "ZVEC_FAULT_PATH": path_filter,
            }
            if once:
                env["ZVEC_FAULT_ONCE"] = "1"
            code = """import errno, mmap, os, sys
root, operation = sys.argv[1:]
# Neither paths outside the database nor unarmed operations may be faulted.
with open(root + "/outside", "wb", buffering=0) as f:
    os.write(f.fileno(), b"outside")
    os.fsync(f.fileno())
with open(root + "/data/probe", "w+b", buffering=0) as f:
    if operation == "msync":
        os.ftruncate(f.fileno(), 4096)
        mapping = mmap.mmap(f.fileno(), 4096)
        mapping[0] = 1
    outcomes = []
    for _ in range(2):
        try:
            if operation == "write":
                os.write(f.fileno(), b"data")
            elif operation == "msync":
                mapping.flush()
            else:
                os.fsync(f.fileno())
            outcomes.append(0)
        except OSError as error:
            outcomes.append(error.errno)
    print(outcomes)
"""
            result = subprocess.run(
                [sys.executable, "-c", code, str(root), operation],
                check=False,
                env=env,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            evidence = audit.read_text() if audit.exists() else ""
            return result.stdout.strip(), evidence

    def test_write_failure_is_persistent_and_path_scoped(self):
        output, audit = self.probe("write")
        self.assertEqual(output, str([errno.EIO, errno.EIO]))
        self.assertEqual(audit.count("INJECT write"), 2)
        self.assertNotIn("outside", audit)

    def test_sync_failure_can_be_one_shot(self):
        output, audit = self.probe("sync", once=True)
        self.assertEqual(output, str([errno.EIO, 0]))
        self.assertEqual(audit.count("INJECT sync"), 1)

    def test_delayed_failure_requires_successful_partial_write(self):
        output, audit = self.probe("write", after=2)
        self.assertEqual(output, str([0, errno.EIO]))
        self.assertIn("PASS write", audit.split("INJECT", 1)[0])
        self.assertEqual(audit.count("INJECT write"), 1)

    def test_category_filter_does_not_consume_other_paths(self):
        output, audit = self.probe("write", path_filter="/manifest.")
        self.assertEqual(output, "[0, 0]")
        self.assertNotIn("INJECT", audit)
        output, audit = self.probe("write", path_filter="/probe", after=2)
        self.assertEqual(output, str([0, errno.EIO]))
        self.assertEqual(audit.count("INJECT write"), 1)

    def test_vector_mmap_flush_failure_is_path_scoped(self):
        for once, expected in ((False, [errno.EIO, errno.EIO]), (True, [errno.EIO, 0])):
            output, audit = self.probe("msync", once=once, path_filter="/probe")
            self.assertEqual(output, str(expected))
            self.assertIn("INJECT msync", audit)
        output, audit = self.probe("msync", path_filter="/other")
        self.assertEqual(output, "[0, 0]")
        self.assertNotIn("INJECT", audit)

    def test_failed_directory_creation_is_audited(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            existing = root / "existing"
            existing.mkdir()
            audit = root / "audit"
            result = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import os, sys; os.mkdir(sys.argv[1])",
                    str(existing),
                ],
                check=False,
                env={
                    **os.environ,
                    "LD_PRELOAD": str(self.library),
                    "ZVEC_FAULT_ROOT": str(root),
                    "ZVEC_FAULT_AUDIT": str(audit),
                },
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                f"ERROR mkdir errno={errno.EEXIST} {existing}", audit.read_text()
            )

    def test_no_fault_before_arming(self):
        output, audit = self.probe("write", armed=False)
        self.assertEqual(output, "[0, 0]")
        self.assertEqual(audit, "")


if __name__ == "__main__":
    unittest.main()
