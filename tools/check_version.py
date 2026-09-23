#!/usr/bin/env python3
"""Validate release metadata only; never compile or launch the game."""

import argparse
from dataclasses import dataclass
from datetime import date
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
CHANNEL_PRECEDENCE = {"alpha": 0, "beta": 1, "rc": 2, None: 3}
PATCH_INDEX = 2
CHANGELOG_DATE_GROUP = 2
VERSION_PATTERN = re.compile(
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-(alpha|beta|rc)\.([1-9][0-9]*))?\+build\.([1-9][0-9]*)"
)


@dataclass(frozen=True)
class Version:
    core: tuple[int, int, int]
    stage: int
    iteration: int
    build: int

    @property
    def precedence(self):
        return (*self.core, self.stage, self.iteration)


def parse_version(value):
    match = VERSION_PATTERN.fullmatch(value)
    if not match:
        raise ValueError(
            "VERSION must be MAJOR.MINOR.PATCH[-alpha.N|-beta.N|-rc.N]+build.B "
            "with no leading zeroes and positive N/B."
        )
    major, minor, patch, channel, iteration, build = match.groups()
    return Version(
        (int(major), int(minor), int(patch)),
        CHANNEL_PRECEDENCE[channel],
        int(iteration or 0),
        int(build),
    )


def check_changelog(version, changelog):
    entries = list(re.finditer(r"^## \[([^\]]+)\] - (\d{4}-\d{2}-\d{2})$", changelog, re.M))
    if not entries or entries[0][1] != version:
        raise ValueError("The first dated CHANGELOG.md entry must match VERSION exactly.")
    seen = set()
    for entry in entries:
        date.fromisoformat(entry[CHANGELOG_DATE_GROUP])
        if entry[1] in seen:
            raise ValueError(f"Duplicate changelog version: {entry[1]}")
        seen.add(entry[1])
    end = entries[1].start() if len(entries) > 1 else len(changelog)
    if not re.search(r"^- \S", changelog[entries[0].end():end], re.M):
        raise ValueError("The current changelog entry must describe at least one change.")


def check_update(current, previous, changed, feature):
    if feature and not {"VERSION", "CHANGELOG.md"}.issubset(changed):
        raise ValueError("Every feat commit must stage both VERSION and CHANGELOG.md.")
    if "VERSION" not in changed:
        return
    if "CHANGELOG.md" not in changed:
        raise ValueError("A VERSION change requires a staged CHANGELOG.md entry.")
    if previous is None:
        return  # Introducing VERSION into the historical prototype.
    if current.build <= previous.build:
        raise ValueError("Increment the build number whenever VERSION changes.")
    if current.precedence < previous.precedence:
        raise ValueError("The new version must not precede the current version.")
    if feature and (current.core[:PATCH_INDEX] <= previous.core[:PATCH_INDEX] or current.core[PATCH_INDEX] != 0):
        raise ValueError("A feature must increase MINOR (or MAJOR) and reset PATCH to zero.")


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staged", action="store_true", help="Read Git's index, not working files")
    parser.add_argument("--feature", action="store_true", help="Require a feature version bump")
    args = parser.parse_args()
    if args.feature and not args.staged:
        parser.error("--feature requires --staged")
    try:
        def read(path):
            return git("show", f":{path}") if args.staged else (ROOT / path).read_text(encoding="utf-8")

        value = read("VERSION").strip()
        current = parse_version(value)
        check_changelog(value, read("CHANGELOG.md"))
        if args.staged:
            changed = set(git("diff", "--cached", "--name-only", "-z").split("\0"))
            previous = None
            if "VERSION" in changed:
                # A missing VERSION in HEAD is expected only during adoption.
                probe = subprocess.run(
                    ["git", "cat-file", "-e", "HEAD:VERSION"], cwd=ROOT, capture_output=True
                )
                if probe.returncode == 0:
                    previous = parse_version(git("show", "HEAD:VERSION").strip())
            check_update(current, previous, changed, args.feature)
        print(f"Version/changelog OK: {value}")
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Version check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
