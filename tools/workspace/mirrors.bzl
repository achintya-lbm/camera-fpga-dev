"""Download mirrors, keyed by the kind of artifact. Same convention as orochi."""

MIRRORS = {
    # NVIDIA's CUDA apt repository also hosts the Holoscan SDK .deb packages.
    "cuda_debs_ubuntu2404": [
        "https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/{basename}",
    ],
    "cuda_debs_ubuntu2204": [
        "https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/{basename}",
    ],
    "github": [
        "https://github.com/{repository}/archive/refs/tags/{tag}.tar.gz",
    ],
}
