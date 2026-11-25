#!/usr/bin/env python3
"""Run with: python3 tools/test_generate_version.py"""

from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
import subprocess
import tempfile
import unittest

from generate_version import format_version, project_version, version_parts, write_header


class BuildVersionTest(unittest.TestCase):
    def test_carries(self):
        for count, expected in [
            (0, "1.00.00"), (9, "1.00.09"), (99, "1.00.99"), (100, "1.01.00"),
            (9999, "1.99.99"), (10_000, "2.00.00"),
        ]:
            self.assertEqual(format_version(count), expected)

    def test_override_and_header(self):
        version = project_version(override="2.08.09")
        parts = version_parts(version)
        for invalid in ["1.8.9", "0.00.01", "01.00.01", "1.00.01\n", "65536.00.00"]:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                version_parts(invalid)
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "generated" / "jianssh_version.h"
            write_header(header, version, parts)
            self.assertIn('#define JIANSSH_VERSION_STRING "2.08.09"', header.read_text())
            self.assertIn("#define JIANSSH_VERSION_TUPLE 2,8,9,0", header.read_text())
            mtime = header.stat().st_mtime_ns
            write_header(header, version, parts)
            self.assertEqual(header.stat().st_mtime_ns, mtime)

    def test_project_history_and_source_export(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / "repo"
            repo.mkdir()

            def git(*args):
                subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)

            def commit(message):
                git("add", ".")
                git("-c", "user.email=test@example.invalid", "-c", "user.name=Version Test",
                    "-c", "commit.gpgsign=false", "commit", "-m", message)

            git("init", "-b", "main")
            project = repo / "JianSSH"
            project.mkdir()
            entry = project / "build.zig"
            entry.write_text("initial")
            commit("add project")
            self.assertEqual(project_version(project), "1.00.01")
            (repo / "sibling.txt").write_text("unrelated")
            commit("sibling change")
            self.assertEqual(project_version(project), "1.00.01")
            entry.write_text("updated")
            self.assertEqual(project_version(project), "1.00.01")
            commit("project change")
            self.assertEqual(project_version(project), "1.00.02")
            git("checkout", "-b", "feature")
            entry.write_text("feature version one")
            commit("feature change one")
            entry.write_text("feature version two")
            commit("feature change two")
            git("checkout", "main")
            git("-c", "user.email=test@example.invalid", "-c", "user.name=Version Test",
                "-c", "commit.gpgsign=false", "merge", "--no-ff", "--no-edit", "feature")
            self.assertEqual(project_version(project), "1.00.03")

            exported = Path(tmp) / "export"
            exported.mkdir()
            (exported / "build.zig").write_text("source export")
            with redirect_stderr(StringIO()):
                self.assertEqual(project_version(exported), "1.00.00")
                shallow = Path(tmp) / "shallow"
                git("clone", "--depth=1", repo.as_uri(), str(shallow))
                self.assertEqual(project_version(shallow / "JianSSH"), "1.00.00")


if __name__ == "__main__":
    unittest.main()
