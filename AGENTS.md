# AGENTS.md

## Workflow

- Commit after each completed feature or major part of one (e.g. a decoder's
  bitstream layer, then its synthesis layer). Do not wait to be asked again.
- Commit message style: imperative subject line, then paragraphs describing
  what was implemented, any interesting bugs found, and how it was verified
  (reference comparison numbers, test counts). No conventional-commit prefixes.

## Build and test

- Use the `build-x64` directory (Visual Studio 17 2022, x64). Other build
  directories in the workspace are stale or misconfigured.
- Build: `cmake --build build-x64 --target player_tests --config RelWithDebInfo`
- Tests: `build-x64\tests\RelWithDebInfo\player_tests.exe` (add
  `--gtest_filter=...` to narrow).
- Player: `cmake --build build-x64 --target fVideoPlayer --config RelWithDebInfo`,
  output `build-x64\RelWithDebInfo\fVideoPlayer.exe`.
- The external backends are optional: kvzdec (HEVC) and f264dec (H.264).
  Check the no-backend paths with a scratch build dir:
  `-DPLAYER_ENABLE_KVZDEC=OFF` and/or `-DPLAYER_ENABLE_F264DEC=OFF` (the test
  suite must pass in every configuration).
- Decoders are verified against ffmpeg: decode the same input with the
  built-in reference path and compute correlation / max sample difference.
