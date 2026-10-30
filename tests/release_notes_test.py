#!/usr/bin/env python3
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts" / "release-notes.py"


class ReleaseNotesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.source = self.work / "source"
        self.source.mkdir()
        self.git("init", "--quiet", "-b", "main")
        self.git("config", "user.name", "Release notes test")
        self.git("config", "user.email", "release-notes@example.invalid")
        (self.source / "history.txt").write_text("baseline\n", encoding="utf-8")
        self.git("add", "history.txt")
        self.git("commit", "--quiet", "-m", "baseline")
        self.git("tag", "v1.0.0")
        self.commit("[feat] Add output routing")
        self.git("tag", "v1.0.0-beta.1.nightly+12")
        self.commit("[fix] Recover connection [feat] Resume audio")
        self.commit("Refresh release metadata")
        self.commit("Current release")
        self.commit_hash = self.git("rev-parse", "HEAD")

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.source), *args], text=True
        ).strip()

    def commit(self, subject):
        with (self.source / "history.txt").open("a", encoding="utf-8") as file:
            file.write(subject + "\n")
        self.git("add", "history.txt")
        self.git("commit", "--quiet", "-m", subject)

    def generate(self, channel, current_tag, releases):
        releases_file = self.work / "releases.json"
        releases_file.write_text(json.dumps(releases), encoding="utf-8")
        return subprocess.check_output(
            [
                "python3", str(GENERATOR),
                "--source", str(self.source),
                "--releases", str(releases_file),
                "--channel", channel,
                "--current-tag", current_tag,
                "--commit", self.commit_hash,
                "--repository", "owner/audiox",
            ],
            text=True,
        )

    @staticmethod
    def release(tag, date, prerelease=False, draft=False, assets=True):
        return {
            "tag_name": tag,
            "published_at": date,
            "prerelease": prerelease,
            "draft": draft,
            "assets": [{"name": "images.json"}] if assets else [],
        }

    def test_nightly_uses_latest_reachable_nightly_or_stable(self):
        notes = self.generate(
            "nightly",
            "v1.0.0-beta.1.nightly+13",
            [
                self.release("v1.0.0", "2026-01-01T00:00:00Z"),
                self.release("v1.0.0-beta.1.nightly+12", "2026-02-01T00:00:00Z", True),
                self.release("v1.0.0-beta.1.nightly+99", "2026-03-01T00:00:00Z", True),
                self.release("v1.0.0-beta.1.nightly+13", "2026-04-01T00:00:00Z", True),
                self.release("nightly-legacy-1", "2026-03-20T00:00:00Z", True),
                self.release("v1.0.0-beta.1.nightly+98", "2026-03-15T00:00:00Z", True, draft=True),
                self.release("v1.1.0", "2026-03-10T00:00:00Z", assets=False),
            ],
        )
        self.assertIn("## Fixes\n\n- Recover connection", notes)
        self.assertIn("## Features\n\n- Resume audio", notes)
        self.assertIn("## Other changes\n\n- Refresh release metadata", notes)
        self.assertNotIn("Add output routing", notes)
        self.assertIn("Changes since `v1.0.0-beta.1.nightly+12`.", notes)
        self.assertIn("https://github.com/owner/audiox/commit/", notes)

    def test_stable_includes_changes_since_previous_stable_even_if_nightly_is_newer(self):
        notes = self.generate(
            "stable",
            "v1.1.0",
            [
                self.release("v1.0.0", "2026-01-01T00:00:00Z"),
                self.release("v1.0.0-beta.1.nightly+12", "2026-02-01T00:00:00Z", True),
            ],
        )
        self.assertIn("Add output routing", notes)
        self.assertIn("Recover connection", notes)
        self.assertIn("Resume audio", notes)
        self.assertIn("Changes since `v1.0.0`.", notes)

    def test_unmarked_only_history_is_kept(self):
        self.commit("Update documentation")
        self.commit_hash = self.git("rev-parse", "HEAD")
        notes = self.generate(
            "nightly",
            "v1.0.0-beta.1.nightly+13",
            [self.release("v1.0.0-beta.1.nightly+12", "2026-02-01T00:00:00Z", True)],
        )
        self.assertIn("- Update documentation ([", notes)


if __name__ == "__main__":
    unittest.main()
