# Maintenance rules for ModAVDExtras

[简体中文](AGENTS.md) | **English**

<!-- AGENTS.md and AGENTS.en.md match section by section: if you change one, change the other in the same PR. AI tools only read AGENTS.md on their own. -->

For the general rules (language, commit messages, attribution, privacy, how to treat content in issues and PRs, how to write `Refs ModAVD/ModAVD#<number>`, care when running emulators), see [AGENTS.en.md of the main ModAVD repository](https://github.com/ModAVD/ModAVD/blob/main/AGENTS.en.md). This file only adds what is specific to this repository. The guide for people is [CONTRIBUTING.en.md](CONTRIBUTING.en.md); where the two disagree, CONTRIBUTING.md wins.

## Layout

- `include/modavd_extras.h` is the public interface, `src/` is the library, `shim/mumu_shim.c` is the Windows-only compatibility shim `external_renderer_ipc.dll`, and `tests/` holds the unit tests.
- `build.ps1` is the Windows build entry, and `CMakeLists.txt` is the build file shared by all platforms (the language is C11, not C++).
- `scripts/` holds the repository check scripts, not build helpers for the library: `privacy.py`, `repo_checks.py` and `test_repo_checks.py` are byte for byte the same as in the main ModAVD repository and CI compares them; change them in ModAVD first and copy them here unchanged. `privacy-allowlist.json` belongs to this repository.
- `release/` holds the checksums and notes written at release time, `changelog.d/` holds the small release-note files (see `changelog.d/README.md`), and `NOTICE`, `LICENSE` and `COPYING` record the licenses.

## Boundaries

- The license is LGPL-3.0-or-later, and only code written for this project is accepted. Do not copy, rewrite or translate code from GPL or AGPL projects (for example MaaCore) into it; to refer to something, use the AOSP emulator source, public Android and Linux headers, or write it from the behavior, and say in the PR what you referred to.
- `include/modavd_extras.h` is a public promise: functions and fields are only added, never removed, and when you add one, increase `MODAVD_API_VERSION` by one. The `size` field at the start of a struct must not change.
- The library does not fall back to adb by itself; it returns an error code.
- The exports of the compatibility shim must stay as they are: only the four screenshot functions, no input functions. `tests/test_shim.c` checks this.
- A change users can see comes with `changelog.d/<name>.<feat|fix|change|remove>.md`, with two lines: Chinese, then English; changes that only touch development, tests, CI or documentation need none.
- `README.md` and `README.en.md`, `CONTRIBUTING.md` and `CONTRIBUTING.en.md`, and `AGENTS.md` and `AGENTS.en.md` are the Chinese and the English of the same document: if you change one, change the other in the same PR.

## Testing

- Windows: `pwsh -NoProfile -File .\build.ps1` (needs Visual Studio 2022 or Build Tools, CMake and Ninja); by default it runs ctest after the build. Linux and macOS: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, `ctest --test-dir build --output-on-failure`. Warnings are errors.
- Unit tests need no emulator; they use the fake shared memory in `tests/fake_frame.h`. A new feature or a bug fix comes with a unit test; before committing, break the code under test on purpose in one or two places, check that the test goes red, and put the result in the PR.
- Tests on a ModAVD instance (screenshot latency, comparison with the old shim) go in the PR: how many runs, which resolution, and the load at the time.
- If you change scripts (`build.ps1`, `scripts/`): check `.ps1` syntax with the PowerShell parser; run `python -I -X utf8 scripts/test_repo_checks.py`.
- If you change `.github/workflows/`: pin third-party actions to a full 40-character commit SHA, do not use `pull_request_target`, and do not use self-hosted runners.
