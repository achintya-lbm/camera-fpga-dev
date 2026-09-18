# ADR-0004 — Bazel 9.2.0 with bzlmod, builds inside the dev container

Date: 2026-09-18. Status: accepted.

## Context
One build root must drive C++/CUDA, Python tooling, RTL simulation and (later) Lattice Radiant. Bazel 9
removed WORKSPACE and native `cc_*`/`py_*` rules; several rule sets lag behind.

## Decision
- `.bazelversion` = 9.2.0; `rules_cc 0.2.25`, `rules_cuda 0.3.0`, `rules_python 2.3.3`, `rules_shell 0.8.0`,
  `googletest 1.18.0.bcr.1` (older googletest BUILD files use native rules and fail on Bazel 9),
  `buildifier_prebuilt 10.0.1`, `hedron_compile_commands` from the helly25 fork (upstream broken on Bazel 9).
- All builds run inside `tools/dev.sh` (Holoscan container) so the CUDA toolkit, Holoscan SDK and gcc match;
  Bazel caches live in a named Docker volume.
- Non-hermetic tools (Radiant) are wrapped in rules tagged `manual`/`no-sandbox` (milestone M6).

## Consequences
- Fallback to Bazel 8.8.0 only if a required rule set breaks; nothing so far needs it.
- Host-side `bazel` is only for BUILD-file formatting; everything else goes through the container.
