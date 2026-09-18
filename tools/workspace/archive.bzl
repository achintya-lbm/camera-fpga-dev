"""Shared helper for tarball-based repository rules (orochi/Drake style).

`archive_repository_impl` downloads one archive, strips its top-level directory,
applies optional patches and symlinks the package's BUILD file. Each dependency
directory wraps it in a `repository_rule` with pinned URL/sha256 defaults so that
`tools/workspace/<name>/repository.bzl` stays a short, reviewable pin.
"""

load("@bazel_tools//tools/build_defs/repo:utils.bzl", "patch")

def archive_repository_impl(repo_ctx):
    """Downloads one archive, strips its prefix, patches it and attaches the BUILD file.

    Args:
      repo_ctx: repository context of the rule using this implementation.
    """
    repo_ctx.download_and_extract(
        url = repo_ctx.attr.urls,
        sha256 = repo_ctx.attr.sha256,
        stripPrefix = repo_ctx.attr.strip_prefix,
        type = repo_ctx.attr.type,
    )
    patch(repo_ctx, patch_args = repo_ctx.attr.patch_args)
    for path, content in repo_ctx.attr.extra_files.items():
        repo_ctx.file(path, content)

    # Upstream Bazel files (if any) are replaced by our package.BUILD.bazel.
    for stale in ["BUILD.bazel", "BUILD", "WORKSPACE", "WORKSPACE.bazel", "MODULE.bazel"]:
        repo_ctx.delete(stale)
    repo_ctx.symlink(repo_ctx.attr.build_file, "BUILD.bazel")

ARCHIVE_ATTRS = {
    "urls": attr.string_list(mandatory = True),
    "sha256": attr.string(mandatory = True),
    "strip_prefix": attr.string(default = ""),
    "type": attr.string(default = ""),
    "build_file": attr.label(mandatory = True),
    "patches": attr.label_list(default = []),
    "patch_args": attr.string_list(default = ["-p1"]),
    "patch_tool": attr.string(default = ""),
    "patch_cmds": attr.string_list(default = []),
    # Extra files written into the repository root (path -> content), e.g. a
    # generated version header that upstream produces with CMake configure_file.
    "extra_files": attr.string_dict(default = {}),
}

archive_repository = repository_rule(
    implementation = archive_repository_impl,
    attrs = ARCHIVE_ATTRS,
)
