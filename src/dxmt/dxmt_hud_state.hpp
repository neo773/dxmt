#pragma once

#include "Metal.hpp"
#include <string>
#include <vector>

namespace dxmt {

class HUDState {
public:
  // Initialize with heading only (D3D11 style — dynamic line creation)
  void initialize(const std::string &heading,
                  const std::string &key_prefix = "com.github.3shain.dxmt");

  // Initialize with heading + pre-allocated lines (D3D9 style — no runtime alloc)
  void initialize(const std::string &heading, unsigned line_count,
                  const std::string &key_prefix);

  // Dynamic API (D3D11): begin/printLine/end per frame
  void begin();
  void printLine(const std::string &str) { printLine(str.c_str()); }
  void printLine(const char *c_str);
  void end();

  // Pre-allocated API (D3D9): single pool for all updates
  void beginUpdate();
  void updateLine(unsigned index, const char *c_str);
  void endUpdate();

  HUDState(WMT::DeveloperHUDProperties hud) : hud_(hud){};

private:
  WMT::DeveloperHUDProperties hud_;
  std::string key_prefix_;
  std::vector<WMT::Reference<WMT::String>> line_labels_;
  WMT::Reference<WMT::Object> pool_;
  unsigned current_line_ = 0;
};

} // namespace dxmt
