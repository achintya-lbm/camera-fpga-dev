# Patches applied to the Holoscan SDK v3.9.0 source tree (in order, `patch -p1`)

| Patch | Why |
|---|---|
| `0001-generated-proto-include-paths.patch` | Upstream's CMake writes protoc/gRPC output to `src/core/distributed/generated/` and the sources include it as `"../generated/x.pb.h"`. Bazel's `proto_library` (import root = the proto directory, matching the bare `import` statements) exposes the generated headers by bare name, so the includes become `"x.pb.h"`. Include-path change only. |

Rules: patches must stay build-system-only (no behavioural changes), so that moving to a newer
Holoscan release is a re-diff, not a port.
