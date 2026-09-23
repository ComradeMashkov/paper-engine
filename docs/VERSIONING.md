# Engine versioning

`VERSION` uses `MAJOR.MINOR.PATCH[-alpha.N|-beta.N|-rc.N]+build.B`. It is independent
of the host game's version. CMake reads and validates it; there is no second editable
version in project configuration.

Every new feature increases MINOR or MAJOR, resets PATCH and increases B. Behavior
fixes increase PATCH/B; documentation alone may retain the version. During pre-1.0,
an intentional API break increases MINOR/B and uses `!` in its Conventional Commit
with compatibility notes. Keep the exact dated entry at the top of CHANGELOG.md;
never invent past releases or claim unperformed verification.

The initial repository snapshot is 0.1.0-alpha.1+build.1, extracted from DCMO on
2026-09-23. DCMO's historical versions are not engine releases. The original source
history remains in that repository; provenance is recorded in THIRD_PARTY.md.

Run `python3 tools/check_version.py`; the commit-msg hook validates the staged
version/changelog and feature increment. Enable `.githooks` in each clone.
Compilation alone is not runtime acceptance or a beta/release promotion.
