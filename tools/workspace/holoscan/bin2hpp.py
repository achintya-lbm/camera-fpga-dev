"""Embed a binary file as a C++ uint8_t array header.

Replacement for Holoscan's cmake/modules/GenHeaderFromBinaryFile.cmake, which turns
modules/holoviz/src/fonts/Roboto-Bold.ttf into fonts/roboto_bold_ttf.hpp.

usage: bin2hpp.py <input> <output.hpp> <array_name>
"""

import sys


def main() -> int:
    src, dst, name = sys.argv[1:4]
    data = open(src, "rb").read()
    lines = ["#include <cstdint>", "", f"static uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 16])
        lines.append(f"  {chunk},")
    lines.append("};")
    lines.append("")
    open(dst, "w").write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
