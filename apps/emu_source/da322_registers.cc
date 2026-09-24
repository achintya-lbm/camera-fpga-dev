#include "apps/emu_source/da322_registers.hpp"

#include <stdexcept>
#include <string>

namespace hsb::emu {
namespace {

// Addresses from hsb/board/da322/da322_regs.hpp and hololink's GPIO class, spelled out here
// because that header (via hololink/core/hololink.hpp) cannot be included next to the emulator.
constexpr uint32_t kMipiIpCoreBase = 0x3000'0000;
constexpr uint32_t kMipiIpCoreEnd = 0x3000'4000;  // four interfaces, 0x1000 apart
constexpr uint32_t kUserCsr = 0x7000'0000;
constexpr uint32_t kMipiDtCtrl = 0x7000'0004;
constexpr uint32_t kMipiDtStat = 0x7000'0008;
// The emulator maps a VSYNC stub over exactly this range; registering the same range replaces it.
constexpr uint32_t kVendorBlockEnd = 0x7000'0018;
constexpr uint32_t kUserCsrStClear = 1u << 0;
constexpr uint32_t kGpioOutput = 0x0000'000C;
constexpr uint32_t kGpioDirection = 0x0000'002C;
constexpr uint32_t kGpioStatus = 0x0000'008C;
constexpr uint32_t kRegisterSize = 4;

struct Range {
  uint32_t start, end;  // [start, end)
};
constexpr Range kRanges[] = {
    {kMipiIpCoreBase, kMipiIpCoreEnd},
    {kUserCsr, kVendorBlockEnd},
    {kGpioOutput, kGpioOutput + kRegisterSize},
    {kGpioDirection, kGpioDirection + kRegisterSize},
    {kGpioStatus, kGpioStatus + kRegisterSize},
};

}  // namespace

bool Da322RegisterFile::InRange(uint32_t address) {
  for (const Range& r : kRanges) {
    if (address >= r.start && address < r.end) return true;
  }
  return false;
}

void Da322RegisterFile::Attach(hololink::emulation::HSBEmulator& hsb) {
  for (const Range& r : kRanges) {
    if (hsb.register_write_callback(r.start, r.end, &Da322RegisterFile::WriteCallback, this) != 0 ||
        hsb.register_read_callback(r.start, r.end, &Da322RegisterFile::ReadCallback, this) != 0) {
      throw std::runtime_error("Da322RegisterFile: cannot register emulator callbacks for range starting at " +
                               std::to_string(r.start));
    }
  }
}

bool Da322RegisterFile::Write(uint32_t address, uint32_t value) {
  if (!InRange(address)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (address == kUserCsr) {
    if (value & kUserCsrStClear) registers_[kMipiDtStat] = 0;
    registers_[kUserCsr] = value;
    return true;
  }
  if (address == kMipiDtCtrl) {
    // "Detected" follows "configured": the emulated sensors always send what the host asked for.
    registers_[kMipiDtStat] = value;
  }
  if (address == kGpioStatus || address == kMipiDtStat) return true;  // read-only
  registers_[address] = value;
  return true;
}

bool Da322RegisterFile::ReadInto(uint32_t address, uint32_t& value) const {
  if (!InRange(address)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t key = address == kGpioStatus ? kGpioOutput : address;
  auto it = registers_.find(key);
  value = it == registers_.end() ? 0 : it->second;
  return true;
}

uint32_t Da322RegisterFile::Read(uint32_t address) const {
  uint32_t value = 0;
  ReadInto(address, value);
  return value;
}

int Da322RegisterFile::WriteCallback(void* ctxt, AddressValuePair* address_values, int count) {
  auto* self = static_cast<Da322RegisterFile*>(ctxt);
  int n = 0;
  while (n < count && self->Write(AVP_GET_ADDRESS(address_values + n), AVP_GET_VALUE(address_values + n))) ++n;
  return n;
}

int Da322RegisterFile::ReadCallback(void* ctxt, AddressValuePair* address_values, int count) {
  auto* self = static_cast<Da322RegisterFile*>(ctxt);
  int n = 0;
  while (n < count) {
    uint32_t value = 0;
    if (!self->ReadInto(AVP_GET_ADDRESS(address_values + n), value)) break;
    AVP_SET_VALUE(address_values + n, value);
    ++n;
  }
  return n;
}

}  // namespace hsb::emu
