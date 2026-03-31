#include "dxmt_hud_state.hpp"
#include "Metal.hpp"
#include <string>

namespace dxmt {

void
HUDState::initialize(const std::string &heading, const std::string &key_prefix) {
  using namespace WMT;
  key_prefix_ = key_prefix;
  auto pool = MakeAutoreleasePool();
  auto str_heading = MakeString((key_prefix_ + "-version").c_str(), WMTUTF8StringEncoding);
  hud_.addLabel(str_heading, String::string("com.apple.hud-graph.default", WMTUTF8StringEncoding));
  hud_.updateLabel(str_heading, String::string(heading.c_str(), WMTUTF8StringEncoding));
  line_labels_.push_back(std::move(str_heading));
}

void
HUDState::initialize(const std::string &heading, unsigned line_count,
                     const std::string &key_prefix) {
  using namespace WMT;
  key_prefix_ = key_prefix;
  auto pool = MakeAutoreleasePool();

  // Create heading label
  auto str_heading = MakeString((key_prefix_ + "-heading").c_str(), WMTUTF8StringEncoding);
  hud_.addLabel(str_heading, String::string("com.apple.hud-graph.default", WMTUTF8StringEncoding));
  hud_.updateLabel(str_heading, String::string(heading.c_str(), WMTUTF8StringEncoding));
  line_labels_.push_back(std::move(str_heading));

  // Pre-create all line labels
  for (unsigned i = 0; i < line_count; i++) {
    auto key = MakeString(
        (key_prefix_ + "-line" + std::to_string(i)).c_str(), WMTUTF8StringEncoding);
    hud_.addLabel(key, line_labels_.back());
    hud_.updateLabel(key, String::string(" ", WMTUTF8StringEncoding));
    line_labels_.push_back(std::move(key));
  }
}

void
HUDState::begin() {
#ifndef DXMT_PERF
  return;
#endif
  pool_ = WMT::MakeAutoreleasePool();
  current_line_ = 1;
}

void
HUDState::printLine(const char *c_str) {
#ifndef DXMT_PERF
  return;
#endif
  using namespace WMT;
  while (current_line_ >= line_labels_.size()) {
    String prev = line_labels_.back();
    line_labels_.push_back(MakeString(
        (key_prefix_ + "-line" + std::to_string(line_labels_.size())).c_str(), WMTUTF8StringEncoding
    ));
    hud_.addLabel(line_labels_.back(), prev);
  }
  hud_.updateLabel(line_labels_[current_line_], String::string(c_str, WMTUTF8StringEncoding));
  current_line_++;
}

void
HUDState::end() {
#ifndef DXMT_PERF
  return;
#endif
  while (line_labels_.size() > current_line_) {
    hud_.remove(line_labels_.back());
    line_labels_.pop_back();
  }
  pool_ = nullptr;
}

void
HUDState::beginUpdate() {
#ifndef DXMT_PERF
  return;
#endif
  pool_ = WMT::MakeAutoreleasePool();
}

void
HUDState::updateLine(unsigned index, const char *c_str) {
#ifndef DXMT_PERF
  return;
#endif
  using namespace WMT;
  if (index + 1 >= line_labels_.size()) return;
  hud_.updateLabel(line_labels_[index + 1], String::string(c_str, WMTUTF8StringEncoding));
}

void
HUDState::endUpdate() {
#ifndef DXMT_PERF
  return;
#endif
  pool_ = nullptr;
}

} // namespace dxmt
