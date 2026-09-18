# ADR-0004 — Bazel 8.8.0 with bzlmod, hermetic host builds

Date: 2026-09-18. Status: accepted.

## Context
One build root must drive C++/CUDA, Python tooling, RTL simulation and (later) Lattice Radiant. Bazel 9
removed WORKSPACE and the native `cc_*`/`py_*` rules; several BCR modules Holoscan depends on (spdlog,
yaml-cpp, magic_enum, cli11) still use those native rules and fail to load on Bazel 9.

## Decision
- `.bazelversion` = 8.8.0 (LTS, same line as the orochi repo); `rules_cc 0.2.25`, `rules_cuda 0.3.0`,
  `rules_python 2.3.3`, `rules_shell 0.8.0`, `rules_foreign_cc 0.16.0`, `googletest 1.18.0.bcr.1`,
  `buildifier_prebuilt 10.0.1`, `hedron_compile_commands` from the helly25 fork.
- Builds run directly on the host. Bazel fetches everything third-party: CUDA via rules_cuda
  `cuda.redist_json`, sources via `tools/workspace/<name>/repository.bzl` (the orochi convention), autotools
  projects (UCX, hwloc) via rules_foreign_cc. `.bazelrc` pins gcc-13, PATH and shared caches under
  `~/.cache/bazel/`.
- Non-hermetic tools (Radiant) are wrapped in rules tagged `manual`/`no-sandbox` (milestone M6).

## Consequences
- Move to Bazel 9 once the BCR modules above carry explicit `load()`s.
- The host must provide gcc-13, autotools, the NVIDIA driver and the graphics/RDMA packages listed in
  `docs/machines.md`; no Docker, no system CUDA, no Holoscan install.
