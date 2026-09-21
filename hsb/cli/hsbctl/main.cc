// hsbctl — command-line access to a Holoscan Sensor Bridge board (DA322 aware).
//
//   hsbctl enumerate [--seconds S]          list boards announcing themselves (BOOTP)
//   hsbctl info                             identity, HSB IP version, FPGA date
//   hsbctl rd ADDR [--count N]              read 32-bit register(s)
//   hsbctl wr ADDR VALUE                    write a 32-bit register
//   hsbctl i2c --bus B --addr A [--write "0x30 0x00"] [--read N]
//   hsbctl i2c-scan --bus B                probe every 7-bit address
//   hsbctl gpio [--pin N [--value 0|1] [--dir 0|1]]   HSB GPIO block
//   hsbctl lanes --port J1A [--set 4]       DA322 MIPI lane count per port
//   hsbctl dt [--port J1A --set 0x2C] [--clear]   DA322 data-type filter / detected data type
//   hsbctl ptp [--wait S]                   PTP synchronisation status
//   hsbctl reset                            Hololink::reset()
//   hsbctl sensor --port J1A probe|rd REG|wr REG VAL|power-up|power-down|configure --mode M
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "hololink/core/data_channel.hpp"
#include "hololink/core/enumerator.hpp"
#include "hololink/core/hololink.hpp"
#include "hololink/core/logging.hpp"
#include "hololink/core/metadata.hpp"
#include "hololink/core/timeout.hpp"
#include "hsb/board/da322/da322_board.hpp"
#include "hsb/sensors/imx676/imx676_regs.hpp"
#include "hsb/sensors/imx676/native_imx676_sensor.hpp"

namespace {

uint32_t ParseU32(const std::string& text) { return static_cast<uint32_t>(std::stoul(text, nullptr, 0)); }

std::string FormatElement(const hololink::Metadata::Element& element) {
  if (const auto* v = std::get_if<int64_t>(&element)) return fmt::format("{} ({:#x})", *v, static_cast<uint64_t>(*v));
  if (const auto* s = std::get_if<std::string>(&element)) return *s;
  if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&element)) {
    std::string out;
    for (auto b : *bytes) out += fmt::format("{:02x} ", b);
    return out;
  }
  return "?";
}

void PrintMetadata(const hololink::Metadata& metadata, const std::string& indent = "  ") {
  for (const auto& [key, value] : metadata) {
    if (key == "_socket_fd") continue;
    std::cout << indent << fmt::format("{:<28} {}", key, FormatElement(value)) << "\n";
  }
}

std::vector<uint8_t> ParseBytes(const std::string& text) {
  std::vector<uint8_t> bytes;
  std::string token;
  for (char c : text + " ") {
    if (c == ' ' || c == ',') {
      if (!token.empty()) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 0)));
        token.clear();
      }
    } else {
      token += c;
    }
  }
  return bytes;
}

struct Board {
  hololink::Metadata metadata;
  std::shared_ptr<hololink::Hololink> hololink;
  ~Board() {
    if (hololink) hololink->stop();
  }
};

std::unique_ptr<Board> Connect(const std::string& ip, double timeout_s) {
  auto board = std::make_unique<Board>();
  board->metadata = hololink::Enumerator::find_channel(ip, std::make_shared<hololink::Timeout>(static_cast<float>(timeout_s)));
  board->hololink = hololink::Hololink::from_enumeration_metadata(board->metadata);
  board->hololink->start();
  return board;
}

std::unique_ptr<hsb::da322::Da322Board> RequireDa322(Board& board, bool force) {
  if (!force && !hsb::da322::IsDa322(board.metadata)) {
    throw std::runtime_error("this board does not identify as a Tauro DA322 (use --force to override)");
  }
  return std::make_unique<hsb::da322::Da322Board>(board.hololink);
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"hsbctl — Holoscan Sensor Bridge / DA322 control"};
  app.require_subcommand(1);
  std::string ip = "192.168.0.2";
  double timeout_s = 10.0;
  bool verbose = false;
  bool force = false;
  app.add_option("--ip", ip, "HSB control-plane IP address")->capture_default_str();
  app.add_option("--timeout", timeout_s, "enumeration timeout in seconds")->capture_default_str();
  app.add_flag("-v,--verbose", verbose, "hololink debug logging");
  app.add_flag("--force", force, "skip the DA322 identity check");

  // enumerate
  auto* enumerate = app.add_subcommand("enumerate", "list boards announcing themselves");
  double seconds = 5.0;
  enumerate->add_option("--seconds", seconds, "how long to listen")->capture_default_str();

  auto* info = app.add_subcommand("info", "board identity and versions");

  auto* rd = app.add_subcommand("rd", "read 32-bit register(s)");
  std::string rd_addr;
  unsigned rd_count = 1;
  rd->add_option("address", rd_addr, "register address (hex ok)")->required();
  rd->add_option("--count", rd_count, "consecutive words")->capture_default_str();

  auto* wr = app.add_subcommand("wr", "write a 32-bit register");
  std::string wr_addr, wr_value;
  wr->add_option("address", wr_addr)->required();
  wr->add_option("value", wr_value)->required();

  auto* i2c = app.add_subcommand("i2c", "raw I2C transaction");
  unsigned i2c_bus = hololink::CAM_I2C_BUS;
  std::string i2c_addr = "0x1A", i2c_write;
  unsigned i2c_read = 0;
  i2c->add_option("--bus", i2c_bus, "I2C bus (DA322: 1..4 = J1A..J1D)")->capture_default_str();
  i2c->add_option("--addr", i2c_addr, "7-bit peripheral address")->capture_default_str();
  i2c->add_option("--write", i2c_write, "bytes to write, e.g. \"0x30 0x00\"");
  i2c->add_option("--read", i2c_read, "bytes to read back")->capture_default_str();

  auto* i2c_scan = app.add_subcommand("i2c-scan", "probe every 7-bit address on an I2C bus");
  unsigned scan_bus = hololink::CAM_I2C_BUS;
  i2c_scan->add_option("--bus", scan_bus, "I2C bus")->capture_default_str();

  auto* gpio = app.add_subcommand("gpio", "HSB GPIO pins: dump all, or set --pin/--value");
  int gpio_pin = -1;
  int gpio_value = -1;
  int gpio_dir = -1;
  gpio->add_option("--pin", gpio_pin, "pin number");
  gpio->add_option("--value", gpio_value, "0/1 to drive the pin (sets direction to output)");
  gpio->add_option("--dir", gpio_dir, "0 = output, 1 = input");

  auto* lanes = app.add_subcommand("lanes", "DA322 MIPI lane count");
  std::string lanes_port = "J1A";
  int lanes_set = -1;
  lanes->add_option("--port", lanes_port, "J1A..J1D, CAM1..CAM4 or 0..3")->capture_default_str();
  lanes->add_option("--set", lanes_set, "lane count to program (1..4)");

  auto* dt = app.add_subcommand("dt", "DA322 CSI data-type filter and detection");
  std::string dt_port;
  std::string dt_set;
  bool dt_clear = false;
  dt->add_option("--port", dt_port, "port to program");
  dt->add_option("--set", dt_set, "data type to accept, e.g. 0x2B (RAW10) or 0x2C (RAW12)");
  dt->add_flag("--clear", dt_clear, "clear the detected data-type latch");

  auto* ptp = app.add_subcommand("ptp", "PTP synchronisation");
  double ptp_wait = 10.0;
  ptp->add_option("--wait", ptp_wait, "seconds to wait for lock")->capture_default_str();

  auto* reset = app.add_subcommand("reset", "reset the HSB");

  auto* sensor = app.add_subcommand("sensor", "IMX676 access through the driver");
  std::string sensor_port = "J1A";
  std::string sensor_action;
  std::vector<std::string> sensor_args;
  std::string sensor_mode = "FULL_RAW12";
  double sensor_fps = 0;
  bool no_p22 = false;
  sensor->add_option("--port", sensor_port)->capture_default_str();
  sensor->add_option("action", sensor_action, "probe | rd REG | wr REG VALUE | power-up | power-down | configure")->required();
  sensor->add_option("args", sensor_args, "action arguments");
  sensor->add_option("--mode", sensor_mode, "mode for 'configure'")->capture_default_str();
  sensor->add_option("--fps", sensor_fps, "frame rate for 'configure' (0 = mode default)");
  sensor->add_flag("--no-p22", no_p22, "do not touch the P22 expander");

  CLI11_PARSE(app, argc, argv);
  if (verbose) hololink::logging::hsb_log_level = hololink::logging::HSB_LOG_LEVEL_DEBUG;

  try {
    if (*enumerate) {
      std::map<std::string, hololink::Metadata> seen;
      hololink::Enumerator::enumerated(
          [&](hololink::Metadata& md) {
            const auto peer = md.get<std::string>("peer_ip");
            const auto sensor_number = md.get<int64_t>("sensor");
            const std::string key = fmt::format("{}#{}", peer ? *peer : "?", sensor_number ? *sensor_number : -1);
            if (seen.emplace(key, md).second) {
              const auto desc = md.get<std::string>("board_description");
              const auto version = md.get<int64_t>("hsb_ip_version");
              const auto serial = md.get<std::string>("serial_number");
              std::cout << fmt::format("{:<16} sensor={:<2} {:<24} hsb_ip_version={:#06x} serial={} {}\n", peer ? *peer : "?",
                                       sensor_number ? *sensor_number : -1, desc ? *desc : "?", version ? *version : 0,
                                       serial ? *serial : "?", hsb::da322::IsDa322(md) ? "[DA322]" : "");
            }
            return true;
          },
          std::make_shared<hololink::Timeout>(static_cast<float>(seconds)));
      if (seen.empty()) {
        std::cout << "no HSB enumeration packets received in " << seconds << " s\n";
        return 2;
      }
      return 0;
    }

    auto board = Connect(ip, timeout_s);
    auto& hl = *board->hololink;

    if (*info) {
      std::cout << fmt::format("hsb_ip_version={:#06x} fpga_date={:#010x} da322={}\n", hl.get_hsb_ip_version(),
                               hl.get_fpga_date(), hsb::da322::IsDa322(board->metadata));
      PrintMetadata(board->metadata);
    } else if (*rd) {
      const uint32_t base = ParseU32(rd_addr);
      for (unsigned i = 0; i < rd_count; ++i) {
        const uint32_t address = base + 4 * i;
        std::cout << fmt::format("{:#010x} = {:#010x}\n", address, hl.read_uint32(address));
      }
    } else if (*wr) {
      const uint32_t address = ParseU32(wr_addr);
      const uint32_t value = ParseU32(wr_value);
      hl.write_uint32(address, value);
      std::cout << fmt::format("{:#010x} <- {:#010x}; readback {:#010x}\n", address, value, hl.read_uint32(address));
    } else if (*i2c) {
      auto bus = hl.get_i2c(i2c_bus);
      auto reply = bus->i2c_transaction(ParseU32(i2c_addr), ParseBytes(i2c_write), i2c_read, hololink::Timeout::i2c_timeout());
      std::cout << "read:";
      for (auto b : reply) std::cout << fmt::format(" {:#04x}", b);
      std::cout << "\n";
    } else if (*i2c_scan) {
      auto bus = hl.get_i2c(scan_bus);
      std::vector<unsigned> found;
      for (unsigned addr = 0x08; addr <= 0x77; ++addr) {
        try {
          bus->i2c_transaction(addr, {}, 1, std::make_shared<hololink::Timeout>(0.2f));
          found.push_back(addr);
        } catch (const std::exception&) {
        }
      }
      std::cout << fmt::format("bus {}: {} device(s) answered:", scan_bus, found.size());
      for (auto a : found) std::cout << fmt::format(" {:#04x}", a);
      std::cout << "\n";
    } else if (*gpio) {
      auto pins = hl.get_gpio(board->metadata);
      const uint32_t count = pins->get_supported_pin_num();
      if (gpio_pin >= 0) {
        if (gpio_dir >= 0) pins->set_direction(static_cast<uint32_t>(gpio_pin), gpio_dir ? hololink::Hololink::GPIO::IN : hololink::Hololink::GPIO::OUT);
        if (gpio_value >= 0) {
          pins->set_direction(static_cast<uint32_t>(gpio_pin), hololink::Hololink::GPIO::OUT);
          pins->set_value(static_cast<uint32_t>(gpio_pin), gpio_value ? 1u : 0u);
        }
        std::cout << fmt::format("pin {}: dir={} value={}\n", gpio_pin,
                                 pins->get_direction(static_cast<uint32_t>(gpio_pin)) == hololink::Hololink::GPIO::IN ? "in" : "out",
                                 pins->get_value(static_cast<uint32_t>(gpio_pin)));
      } else {
        std::cout << fmt::format("{} GPIO pins\n", count);
        for (uint32_t pin = 0; pin < count; ++pin) {
          std::cout << fmt::format("  pin {:2}: dir={} value={}\n", pin,
                                   pins->get_direction(pin) == hololink::Hololink::GPIO::IN ? "in " : "out", pins->get_value(pin));
        }
      }
    } else if (*lanes) {
      auto da322 = RequireDa322(*board, force);
      const unsigned camera = hsb::da322::ParsePort(lanes_port);
      if (lanes_set > 0) da322->SetLanes(camera, static_cast<unsigned>(lanes_set));
      const uint32_t raw = da322->ReadLaneSetting(camera);
      std::cout << fmt::format("{}: lane register {:#010x} = {:#x} -> {} lane(s)\n", hsb::da322::Port(camera).connector,
                               hsb::da322::LaneSettingAddress(camera), raw, ((raw >> hsb::da322::kLaneSettingShift) & 0x3) + 1);
    } else if (*dt) {
      auto da322 = RequireDa322(*board, force);
      if (dt_clear) da322->ClearDetectedDataTypes();
      if (!dt_set.empty()) {
        if (dt_port.empty()) throw std::runtime_error("--set needs --port");
        da322->SetDataType(hsb::da322::ParsePort(dt_port), static_cast<uint8_t>(ParseU32(dt_set)));
      }
      for (unsigned camera = 0; camera < hsb::da322::kCameraCount; ++camera) {
        std::cout << fmt::format("{} ({}): filter={:#04x} detected={:#04x}\n", hsb::da322::Port(camera).connector,
                                 hsb::da322::Port(camera).label, da322->ConfiguredDataType(camera),
                                 da322->DetectedDataType(camera));
      }
    } else if (*ptp) {
      const bool ok = hl.ptp_synchronize(std::make_shared<hololink::Timeout>(static_cast<float>(ptp_wait)));
      std::cout << fmt::format("ptp_synchronized={} FPGA_PTP_SYNC_STAT={:#010x} FPGA_PTP_OFM={:#010x}\n", ok,
                               hl.read_uint32(hololink::FPGA_PTP_SYNC_STAT), hl.read_uint32(hololink::FPGA_PTP_OFM));
      return ok ? 0 : 3;
    } else if (*reset) {
      hl.reset();
      std::cout << "reset done\n";
    } else if (*sensor) {
      const unsigned camera = hsb::da322::ParsePort(sensor_port);
      hololink::Metadata md = board->metadata;
      hololink::DataChannel::use_sensor(md, camera);
      hololink::DataChannel channel(md, [&](const hololink::Metadata&) { return board->hololink; });
      hsb::imx676::Imx676Options options;
      options.use_p22_adapter = !no_p22;
      options.fps = sensor_fps;
      hsb::imx676::NativeImx676Sensor imx(channel, hsb::da322::I2cBusForCamera(camera), options);
      if (sensor_action == "probe") {
        const bool ok = imx.probe();
        std::cout << fmt::format("{} bus {}: {}\n", hsb::da322::Port(camera).connector, imx.i2c_bus(), ok ? "sensor answers" : "no answer");
        if (ok) {
          std::cout << fmt::format("  STANDBY={:#04x} XMSTA={:#04x} LANEMODE={:#04x} INCK_SEL={:#04x} DATARATE_SEL={:#04x}\n",
                                   imx.read_register(hsb::imx676::reg::STANDBY), imx.read_register(hsb::imx676::reg::XMSTA),
                                   imx.read_register(hsb::imx676::reg::LANEMODE), imx.read_register(hsb::imx676::reg::INCK_SEL),
                                   imx.read_register(hsb::imx676::reg::DATARATE_SEL));
        }
        return ok ? 0 : 4;
      } else if (sensor_action == "rd") {
        if (sensor_args.empty()) throw std::runtime_error("rd needs REG");
        const uint16_t reg = static_cast<uint16_t>(ParseU32(sensor_args[0]));
        std::cout << fmt::format("{:#06x} = {:#04x}\n", reg, imx.read_register(reg));
      } else if (sensor_action == "wr") {
        if (sensor_args.size() < 2) throw std::runtime_error("wr needs REG VALUE");
        const uint16_t reg = static_cast<uint16_t>(ParseU32(sensor_args[0]));
        imx.write_register(reg, static_cast<uint8_t>(ParseU32(sensor_args[1])));
        std::cout << fmt::format("{:#06x} <- {:#04x}; readback {:#04x}\n", reg, ParseU32(sensor_args[1]), imx.read_register(reg));
      } else if (sensor_action == "power-up") {
        imx.power_up();
        std::cout << "P22 power-up sequence done\n";
      } else if (sensor_action == "power-down") {
        imx.power_down();
        std::cout << "P22 power-down done\n";
      } else if (sensor_action == "configure") {
        auto mode = hsb::imx676::ParseMode(sensor_mode);
        if (!mode) throw std::runtime_error("unknown mode " + sensor_mode);
        auto da322 = hsb::da322::IsDa322(board->metadata) ? std::make_unique<hsb::da322::Da322Board>(board->hololink) : nullptr;
        imx.set_mode(*mode);
        if (da322) da322->ConfigurePort(camera, 4, imx.get_pixel_format());
        imx.configure(*mode);
        std::cout << hsb::imx676::Describe(imx.mode_info(), imx.timing()) << "\n";
      } else {
        throw std::runtime_error("unknown sensor action " + sensor_action);
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "hsbctl: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
