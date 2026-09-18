# Patches applied to rules_cuda 0.3.0 (MODULE.bazel `single_version_override`)

| Patch | Why |
|---|---|
| `0001-nvcc-wrapper-device-link-feature.patch` | Adds an opt-in `supports_wrapper_device_link` feature to the nvcc toolchain config (the clang config already has it). With the hermetic redistributable toolkit, `nvcc -dlink` cannot compile its link stub because nvcc's own include path lacks the `crt/` headers (they live in a separate redist component). Targets with `rdc = True` set `features = ["-supports_compiler_device_link", "supports_wrapper_device_link"]` to use nvlink + fatbinary + bin2c with the cc toolchain instead. Worth upstreaming. |
| `0002-wrapper-device-link-header-inputs.patch` | The wrapper device link compiles its generated link stub with the target's include *paths* but without the corresponding header *files* as action inputs, so the stub fails to find `crt/host_defines.h` inside the sandbox. Adds `transitive_headers = [common.headers]`. Worth upstreaming. |
