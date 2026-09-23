# Contribution rules

Use a topic branch (`feat/…`, `fix/…`, `docs/…`, `refactor/…`, `build/…` etc.),
Conventional Commits with an English imperative title of at most 72 characters,
and a focused PR merged by squash. Only the first bootstrap commit may go
directly to `main`. Enable hooks with `git config --local core.hooksPath .githooks`.
Keep the configured human Git author; never add assistant co-author trailers.

`VERSION` is the sole engine version source. A feature increases MINOR (or MAJOR),
resets PATCH and increases build; a behavior fix increases PATCH/build. Include
a dated entry for that exact version in `CHANGELOG.md`. See docs/VERSIONING.md.
Run `python3 tools/check_version.py` and `python3 tools/check_parameters.py`.
The hooks inspect staged metadata; these checks do not execute C++ programs.
The first bootstrap establishes 0.1.0-alpha.1+build.1; later features must advance it.

Keep core/content/render/resources/authoring independent of Qt and SDL devices.
No includes or links into a host game's code, assets, namespace or scripting VM.
Public headers use `paper/…`, namespaces use `paper`, CMake exports `Paper::…`.
Review meaningful numeric parameters for their owner, unit and range. Test fixtures
are independently authored regression inputs with explained literal fingerprints.
Use `.clang-format`; do not reformat pinned upstream headers or archives.

Building libraries, shaders, editor and test targets is authorized for this task.
Do not run any application, editor, CTest, test executable, preview or packaging
without a later explicit owner instruction. Report compilation separately from
runtime verification. Do not push, create repositories or merge without task
scope authorization. Commit coherent completed changes separately.

Keep pinned dependencies, notices and required example data tracked. Do not track
build output, project locks, machine settings, local saves, credentials or secrets.
