import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare_sources as sources


class ResumeTests(unittest.TestCase):
    def test_interrupted_fetch_can_resume_without_reset(self):
        row = {"name": "u-boot", "url": sources.ALLOW["u-boot"], "ref": "refs/tags/v2026.10", "commit": "a" * 40}
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "u-boot"
            (target / ".git").mkdir(parents=True)
            calls = []
            def fake_git(*args, **kwargs):
                calls.append(args)
                if args[:3] == ("remote", "get-url", "origin"):
                    return row["url"]
                if args[:3] == ("rev-parse", "--verify", "HEAD"):
                    raise subprocess.CalledProcessError(128, "git")
                if args[0] == "rev-parse":
                    return row["commit"]
                return ""
            with patch.object(sources, "git", side_effect=fake_git):
                result = sources.prepare(row, temp)
            self.assertTrue(result["resumed_initial_fetch"])
            self.assertFalse(any(c[0] in ("reset", "clean", "init") for c in calls))

    def test_unborn_repo_with_ignored_file_refused(self):
        row = {"name": "u-boot", "url": sources.ALLOW["u-boot"], "ref": "refs/tags/v2026.10", "commit": "a" * 40}
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "u-boot"
            (target / ".git").mkdir(parents=True)
            (target / "valuable-local-file").write_text("preserve")
            def fake_git(*args, **kwargs):
                if args[0] == "remote": return row["url"]
                if args[0] == "status": return ""
                raise subprocess.CalledProcessError(128, "git")
            with patch.object(sources, "git", side_effect=fake_git):
                with self.assertRaisesRegex(ValueError, "existing files"):
                    sources.prepare(row, temp)
            self.assertEqual("preserve", (target / "valuable-local-file").read_text())


if __name__ == "__main__":
    unittest.main()
