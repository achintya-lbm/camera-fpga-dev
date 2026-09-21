// Live camera controls exposed to the preview UI. Implemented by the application (which owns the
// sensor driver and the rig); called from HTTP server threads and from the Holoviz UI callback.
#pragma once

#include <map>
#include <string>

namespace hsb::preview {

struct ControlState {
  std::string camera;      // label, e.g. "cam3-J1D"
  std::string mode;        // e.g. "FULL_RAW12"
  int width = 0;
  int height = 0;
  double fps = 0;          // configured frame rate
  double exposure_ms = 0;  // current
  double gain_db = 0;      // current, 0..72
  int black_level = 50;    // BLKLEVEL register (10-bit units)
  bool test_pattern = false;
  int test_pattern_select = 0;
  // Read-only limits for the UI.
  double exposure_max_ms = 0;  // one frame period at the current VMAX
  double gain_max_db = 72.0;
};

class Controls {
 public:
  virtual ~Controls() = default;
  virtual ControlState Get() = 0;
  // Applies the given key/value pairs (keys: exposure_ms, gain_db, black_level, test_pattern (0/1),
  // test_pattern_select). Unknown keys are ignored. Returns "" on success or an error message.
  virtual std::string Apply(const std::map<std::string, std::string>& values) = 0;
  // Requests a raw CSI frame dump of the next frame (SnapshotOp); returns the base path that will be
  // written ("<dir>/<camera>_<timestamp>") or an error message starting with "error:".
  virtual std::string CaptureRaw() = 0;
  // Pipeline statistics as a JSON object string (fps, gbps, frames, gaps, dropped, ...).
  virtual std::string StatusJson() = 0;
};

}  // namespace hsb::preview
