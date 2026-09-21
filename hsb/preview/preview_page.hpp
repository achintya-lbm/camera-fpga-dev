// The embedded HTML/JS control page served at "/". Kept in a .cc as a raw string.
#pragma once

#include <string_view>

namespace hsb::preview {
std::string_view PreviewPageHtml();
}  // namespace hsb::preview
