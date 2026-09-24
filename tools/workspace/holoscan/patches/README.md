# Patches applied to the Holoscan SDK v4.4.0 source tree (in order, `patch -p1`)

| Patch | Why |
|---|---|
| `0001-generated-proto-include-paths.patch` | Upstream's CMake writes protoc/gRPC output to `src/core/distributed/generated/` and the sources include it as `"../generated/x.pb.h"`. Bazel's `proto_library` (import root = the proto directory, matching the bare `import` statements) exposes the generated headers by bare name, so the includes become `"x.pb.h"`. Include-path change only. |
| `0002-holoviz-vulkan-hpp-detail-namespace.patch` | Holoviz calls `vk::resultCheck`, which the Vulkan-Headers release we build against (BCR `vulkan_headers` 1.4.x) moved to `vk::detail::resultCheck`. Namespace change only. |

Rules: patches must stay build-system-only (no behavioural changes), so that moving to a newer
Holoscan release is a re-diff, not a port (done 3.9.0 → 4.4.0 on 2026-09-24).
