#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace dxmt {

static std::atomic<uint32_t> s_total_shaders{0};
static std::atomic<uint32_t> s_burst_count{0};
static std::atomic<uint64_t> s_burst_duration_ns{0};
static std::once_flag s_monitor_flag;

static void shader_stats_monitor() {
  uint32_t last_count = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    uint32_t count = s_burst_count.load(std::memory_order_relaxed);
    if (count > 0 && count == last_count) {
      auto duration_ns = s_burst_duration_ns.load(std::memory_order_relaxed);
      auto elapsed_ms = (int)(duration_ns / 1'000'000);
      auto avg_us = duration_ns / (count * 1000);
      auto total = s_total_shaders.load();
      Logger::info(str::format(
        "shaders: ", count, " compiled in ", elapsed_ms, "ms",
        " (avg ", avg_us / 1000, ".", (avg_us / 100) % 10, "ms, ",
        total, " total)"));
      s_burst_count.store(0, std::memory_order_relaxed);
      s_burst_duration_ns.store(0, std::memory_order_relaxed);
      last_count = 0;
    } else {
      last_count = count;
    }
  }
}

uint64_t ShaderCache::nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ShaderCache::recordShader(uint64_t start_ns) {
  auto duration = nowNs() - start_ns;
  s_total_shaders.fetch_add(1, std::memory_order_relaxed);
  s_burst_count.fetch_add(1, std::memory_order_relaxed);
  s_burst_duration_ns.fetch_add(duration, std::memory_order_relaxed);
  std::call_once(s_monitor_flag, []() {
    std::thread(shader_stats_monitor).detach();
  });
}

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  static dxmt::mutex mutex;
  static std::unordered_map<WMTMetalVersion, std::unique_ptr<ShaderCache>> caches;

  std::lock_guard<dxmt::mutex> lock(mutex);
  auto iter = caches.find(version);
  if (iter == caches.end()) {
    auto inserted = caches.insert({version, std::make_unique<ShaderCache>(version)});
    return *inserted.first->second;
  }
  return *iter->second;
}

ShaderCache::ShaderCache(WMTMetalVersion metal_version) {
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0")
    return;
  std::string path;
  if (path = env::getEnvVar("DXMT_SHADER_CACHE_PATH"); !path.empty() && path.starts_with("/")) {
    if (!path.ends_with('/'))
      path += "/";
  } else {
    path = str::format("dxmt/", env::getExeName(), "/");
  }
  path += str::format("shaders_", (unsigned int)metal_version, ".db");
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
}

void ShaderCache::preload(WMT::Device device) {
  if (!scache_reader_)
    return;

  auto reader = getReader();
  if (!reader)
    return;

  preload_done_.store(false, std::memory_order_release);
  s_total_shaders.store(0, std::memory_order_relaxed);

  auto start_ns = nowNs();
  auto count = reader->preload(device);
  auto duration_ns = nowNs() - start_ns;
  auto elapsed_ms = (int)(duration_ns / 1'000'000);
  if (count > 0) {
    auto avg_us = duration_ns / (count * 1000);
    Logger::info(str::format("shader preload: ", count, " libraries in ", elapsed_ms, "ms",
      " (avg ", avg_us / 1000, ".", (avg_us / 100) % 10, "ms)"));
    s_total_shaders.fetch_add(count, std::memory_order_relaxed);
  }

  preload_done_.store(true, std::memory_order_release);
}

WMT::Reference<WMT::Library> ShaderCache::findPreloadedLibrary(const void *key, size_t len) {
  if (!preload_done_.load(std::memory_order_acquire))
    return {};

  auto reader = getReader();
  if (!reader)
    return {};
  auto result = reader->getPreloaded(key, len);
  return result;
}

} // namespace dxmt