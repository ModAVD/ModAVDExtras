# Contributing to ModAVDExtras

[简体中文](CONTRIBUTING.md) | **English**

For the general rules of the ModAVD organization, see [CONTRIBUTING.en.md of ModAVD](https://github.com/ModAVD/ModAVD/blob/main/CONTRIBUTING.en.md) and [Contributing](https://modavd.github.io/en/develop/) on the docs site. In short: open an issue to discuss first, open the PR from your own fork, use your GitHub noreply email for your commits, do not write `Co-authored-by`, and if you use an AI tool, have it test its own work and say so in the PR.

These are the rules of this repository.

## License boundary

- This library is LGPL-3.0-or-later and **only accepts code written for this project**.
- Do not put code from GPL or AGPL projects here, whether copied, rewritten or translated. MaaCore (MAA) is AGPL-3.0, so its code (for example `MumuExtras`, `MumuController`, `SwipeHelper`) can only stay in the adapter that goes to the MAA upstream and cannot come into this repository.
- When you need a reference, use a cleaner source: the AOSP emulator source (Apache-2.0), the public headers of Android and Linux, or write it yourself from the behavior. Say in the PR description what you referred to.
- Check this point specifically when reviewing a PR.

## Interface

- `include/modavd_extras.h` is the public promise: functions and fields are only added, never removed, and when you add one, increase `MODAVD_API_VERSION` by one. The `size` field at the start of a struct must not change.
- The library never falls back to adb by itself. On an error it returns an error code.
- The exports of the compatibility shell `shim/` must stay the same as now: only the four capture functions, and no input functions. `tests/test_shim.c` checks this.

## Tests

- When you change code, say in the PR what you ran: `build.ps1` (Windows) or CMake plus `ctest` (Linux, macOS).
- New features and bug fixes come with unit tests. The tests do not need an emulator. They use the fake shared memory in `tests/fake_frame.h`.
- Before you commit, break the code under test on purpose in one or two places, make sure the tests turn red, and put the result in the PR.
- Put tests on a ModAVD instance (capture latency, comparison with the old shell) in the PR: how many runs, what resolution, and the load at the time.

## Workflows

Pin third-party actions to a 40-character commit SHA. Do not use `pull_request_target` or self-hosted runners. A PR that changes a workflow is reviewed by someone else.

`scripts/privacy.py` is byte for byte the same as in the ModAVD main repository, and CI checks this. When you change the scanner, change ModAVD first and then sync it here unchanged. `privacy-allowlist.json` is still maintained in this repository.
