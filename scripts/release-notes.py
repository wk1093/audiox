#!/usr/bin/env python3
"""Generate AudioX release notes from marked commit subjects."""

import argparse
import json
import re
import subprocess
from pathlib import Path

STABLE_TAG = re.compile(r"^v\d+\.\d+\.\d+$")
NIGHTLY_TAG = re.compile(r"^nightly-\d+$")
MARKER = re.compile(r"\[(feat|fix)\]\s*(.*?)(?=\[(?:feat|fix)\]|$)", re.IGNORECASE)


def git(source: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(source), *args], text=True
    ).strip()


def choose_baseline(releases: list[dict], channel: str, current_tag: str,
                    source: Path, commit: str) -> str | None:
    candidates = []
    for release in releases:
        tag = release.get("tag_name", "")
        assets = release.get("assets") or []
        has_manifest = any(
            asset.get("name") in {"images.json", "os_list.json"}
            for asset in assets
        )
        if (tag == current_tag or release.get("draft") or
                not has_manifest):
            continue
        if channel == "stable":
            eligible = (STABLE_TAG.fullmatch(tag) and
                        not release.get("prerelease"))
        else:
            eligible = (
                (STABLE_TAG.fullmatch(tag) and not release.get("prerelease")) or
                (NIGHTLY_TAG.fullmatch(tag) and release.get("prerelease"))
            )
        if eligible:
            candidates.append(release)

    candidates.sort(
        key=lambda release: release.get("published_at") or
        release.get("created_at") or "",
        reverse=True,
    )
    for release in candidates:
        tag = release["tag_name"]
        tag_result = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "--verify",
             f"refs/tags/{tag}^{{commit}}"],
            check=False,
            capture_output=True,
            text=True,
        )
        if tag_result.returncode != 0:
            continue
        baseline_commit = tag_result.stdout.strip()
        is_ancestor = subprocess.run(
            ["git", "-C", str(source), "merge-base", "--is-ancestor",
             baseline_commit, commit],
            check=False,
        )
        if is_ancestor.returncode == 0:
            return tag
        if is_ancestor.returncode != 1:
            raise RuntimeError(f"Could not check ancestry of release tag {tag}")
    return None


def summarize_subject(subject: str) -> list[tuple[str, str]]:
    entries = []
    for match in MARKER.finditer(subject):
        title = match.group(2).strip()
        if title:
            entries.append((match.group(1).lower(), title))
    return entries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--releases", type=Path, required=True)
    parser.add_argument("--channel", choices=("stable", "nightly"), required=True)
    parser.add_argument("--current-tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--repository", required=True)
    args = parser.parse_args()

    releases = json.loads(args.releases.read_text(encoding="utf-8"))
    baseline = choose_baseline(
        releases, args.channel, args.current_tag, args.source, args.commit
    )
    revision_range = f"{baseline}..{args.commit}" if baseline else args.commit
    log = git(args.source, "log", "--format=%H%x09%s", revision_range)

    sections = {"feat": [], "fix": [], "other": []}
    for line in log.splitlines():
        commit, subject = line.split("\t", 1)
        entries = summarize_subject(subject)
        if not entries:
            sections["other"].append((commit, subject.strip()))
            continue
        for kind, title in entries:
            sections[kind].append((commit, title))

    headings = {"feat": "Features", "fix": "Fixes", "other": "Other changes"}
    lines = []
    for kind in ("feat", "fix", "other"):
        if not sections[kind]:
            continue
        lines.append(f"## {headings[kind]}")
        lines.append("")
        for commit, title in reversed(sections[kind]):
            short_hash = commit[:7]
            link = f"https://github.com/{args.repository}/commit/{commit}"
            lines.append(f"- {title} ([{short_hash}]({link}))")
        lines.append("")
    if not lines:
        lines = ["No commits in this range."]
    if baseline:
        lines.extend(["", f"Changes since `{baseline}`."])
    else:
        lines.extend(["", "Changes since the beginning of recorded history."])
    print("\n".join(lines).rstrip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
