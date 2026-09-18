"""Dear ImGui at the commit Holoviz pins (modules/holoviz/thirdparty/imgui/CMakeLists.txt:
f3373780668fba1f9bd64c208d05c20b781c9a39, docking branch, "1.88 WIP") with Holoscan's
imconfig.h patch (thread-local ImGui context shared with the holoviz library).
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_COMMIT = "f3373780668fba1f9bd64c208d05c20b781c9a39"

_SHA256 = "7667cdf744e414b9d82f30bcdb0ce4a996cde8fa619a11500b5b66e352a32399"

def imgui_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/ocornut/imgui/archive/{c}.tar.gz".format(c = _COMMIT)],
        sha256 = _SHA256,
        strip_prefix = "imgui-" + _COMMIT,
        build_file = "//tools/workspace/imgui:package.BUILD.bazel",
        patches = ["//tools/workspace/imgui/patches:0001-holoscan-imconfig-thread-local-context.patch"],
    )
