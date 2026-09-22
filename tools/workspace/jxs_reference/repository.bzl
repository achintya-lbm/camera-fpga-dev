"""ISO/IEC 21122-5 (JPEG XS Part 5) reference software, 3rd edition (libjxs 3.0), from the ISO
public download. Licence (LICENSE.md in the archive): copyright licence for evaluation and
conformance testing only, no patent licence — this repository is an ORACLE for our own codec and
must never ship in a product.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_URL = "https://standards.iso.org/iso-iec/21122/-5/ed-3/en/ISO_IEC_21122-5_ED-3.zip"

_SHA256 = "36efc26b92a09b7447bc6e580c902d5c19ee9e919b324609768e390d38621835"

def jxs_reference_repository(name):
    archive_repository(
        name = name,
        urls = [_URL],
        sha256 = _SHA256,
        type = "zip",
        build_file = "//tools/workspace/jxs_reference:package.BUILD.bazel",
    )
