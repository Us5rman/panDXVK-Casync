#include "dxvk_pandxvk_report.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "dxvk_device.h"

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"
#include "../util/util_version.h"

namespace dxvk {

  namespace {

    // Draws accumulated before HAAE would submit an empty pacemaker
    // list. Matches the per-tier thresholds documented for the VEGAS
    // implementation ({50, 100, 150}, tier 1 pacing most frequently).
    constexpr std::array<uint64_t, 3> g_haaeThresholds = { { 50, 100, 150 } };

    constexpr const char* g_reportDirName = "pandxvk-reports";
    constexpr const char* g_markerName    = ".crash_marker";

    // JSON has no notion of a raw byte string; device and driver
    // names come from the driver and could in principle contain a
    // quote or a backslash. Escape rather than trust the input.
    std::string jsonEscape(const std::string& value) {
      std::string result;
      result.reserve(value.size() + 8);

      for (const char c : value) {
        switch (c) {
          case '"':  result += "\\\""; break;
          case '\\': result += "\\\\"; break;
          case '\n': result += "\\n";  break;
          case '\r': result += "\\r";  break;
          case '\t': result += "\\t";  break;
          default:
            if (static_cast<unsigned char>(c) < 0x20) {
              char buf[8];
              std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
              result += buf;
            } else {
              result += c;
            }
            break;
        }
      }

      return result;
    }

    std::string formatDouble(double value, int precision) {
      std::ostringstream stream;
      stream.setf(std::ios::fixed);
      stream.precision(precision);
      stream << value;
      return stream.str();
    }

    std::string formatVersion(uint32_t version) {
      // Vulkan API version convention: major << 22 | minor << 12 | patch.
      const uint32_t major = (version >> 22) & 0x7f;
      const uint32_t minor = (version >> 12) & 0x3ff;
      const uint32_t patch =  version        & 0xfff;
      return str::format(major, ".", minor, ".", patch);
    }

  }


  static DxvkPandxvkBindTotals g_bindTotals;


  DxvkPandxvkBindTotals& pandxvkBindTotals() {
    return g_bindTotals;
  }


  bool pandxvkTelemetryEnabled() {
    static const bool enabled = [] {
      const std::string v = env::getEnvVar("PANDXVK_TELEMETRY");
      return !v.empty() && v != "0";
    }();
    return enabled;
  }


  DxvkPandxvkReport::DxvkPandxvkReport() {
    m_enabled = pandxvkTelemetryEnabled();
  }


  DxvkPandxvkReport::~DxvkPandxvkReport() { }


  uint64_t DxvkPandxvkReport::monotonicNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }


  std::string DxvkPandxvkReport::sanitizeName(const std::string& name) {
    std::string result;
    result.reserve(name.size());

    for (const char c : name) {
      const bool safe = (c >= 'a' && c <= 'z')
                     || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9')
                     ||  c == '-' || c == '_' || c == '.';
      result += safe ? c : '_';
    }

    if (result.empty())
      result = "unknown";

    return result;
  }


  std::string DxvkPandxvkReport::reportRoot() {
    // An explicit report directory is used verbatim: the operator
    // who set it decided where reports belong.
    std::string root = env::getEnvVar("PANDXVK_REPORT_DIR");

    if (!root.empty()) {
      while (!root.empty() && root.back() == '/')
        root.pop_back();
      return root;
    }

    // Otherwise hang off the directory DXVK writes its logs to, so
    // that reports and logs stay siblings and reports never land
    // inside the log directory itself.
    std::string logs = env::getEnvVar("DXVK_LOG_PATH");

    if (logs == "none")
      logs.clear();

    while (!logs.empty() && logs.back() == '/')
      logs.pop_back();

    if (!logs.empty())
      logs += '/';

    return logs + g_reportDirName;
  }


  std::string DxvkPandxvkReport::joinPath(
    const std::string&      dir,
    const std::string&      file) {
    if (dir.empty())
      return file;
    if (dir.back() == '/')
      return dir + file;
    return dir + '/' + file;
  }


  void DxvkPandxvkReport::beginSession(DxvkDevice* device) {
    if (!m_enabled || device == nullptr)
      return;

    m_gameName  = sanitizeName(env::getExeBaseName());
    m_reportDir = reportRoot();
    m_markerPath = joinPath(m_reportDir, g_markerName);

    // CreateDirectoryW reports failure both when a parent is missing and
    // when the directory already exists, and util_env cannot tell the two
    // apart. The second case is the normal state from the second run
    // onward, so this is only a best-effort create: the marker probe
    // below is what decides whether the directory is usable.
    env::createDirectory(m_reportDir);

    // An orphaned marker means the previous session never reached
    // endSession(): the process died first.
    std::ifstream previous(m_markerPath.c_str());
    if (previous.is_open()) {
      previous.close();
      m_crashedLastRun = true;
      writeIssueMd(device);
    }

    std::ofstream marker(m_markerPath.c_str(),
      std::ios_base::out | std::ios_base::trunc);

    if (!marker.is_open()) {
      Logger::warn(str::format(
        "panDXVK telemetry: cannot write ", m_markerPath, ", disabling"));
      m_enabled = false;
      return;
    }

    marker << m_gameName << '\n';
    marker.close();

    m_active        = true;
    m_haveBaseline  = false;
    m_lastPresentNs = monotonicNs();

    Logger::info(str::format(
      "panDXVK telemetry: session ", m_gameName, " -> ", m_reportDir));
  }


  void DxvkPandxvkReport::onPresent(DxvkDevice* device) {
    if (!m_enabled || !m_active || device == nullptr)
      return;

    const uint64_t now = monotonicNs();

    if (m_lastPresentNs != 0 && now > m_lastPresentNs) {
      const uint64_t deltaNs = now - m_lastPresentNs;

      m_frameSumNs += deltaNs;

      const double fps = 1.0e9 / double(deltaNs);
      m_fpsSum += fps;

      if (!m_haveFps || fps < m_minFps) {
        m_minFps  = fps;
        m_haveFps = true;
      }

      size_t bucket = size_t(fps / 5.0);
      if (bucket >= m_fpsHistogram.size())
        bucket = m_fpsHistogram.size() - 1;
      m_fpsHistogram[bucket] += 1;

      m_frames += 1;
    }

    m_lastPresentNs = now;

    const DxvkStatCounters counters = device->getStatCounters();

    const uint64_t submits = counters.getCtr(DxvkStatCounter::QueueSubmitCount);
    const uint64_t draws   = counters.getCtr(DxvkStatCounter::CmdDrawCalls);
    const uint64_t idleUs  = counters.getCtr(DxvkStatCounter::GpuIdleTicks);
    const uint64_t present = counters.getCtr(DxvkStatCounter::QueuePresentCount);

    if (!m_haveBaseline) {
      // First sample only establishes where the counters start;
      // anything before the first present belongs to startup, not
      // to the session being measured.
      m_prevSubmits = submits;
      m_prevDraws   = draws;
      m_prevIdleUs  = idleUs;
      m_presentTotal = present;
      m_haveBaseline = true;
      return;
    }

    m_submitsTotal += submits - m_prevSubmits;
    m_drawsTotal   += draws   - m_prevDraws;
    m_idleUsTotal  += idleUs  - m_prevIdleUs;
    m_presentTotal  = present;

    m_prevSubmits = submits;
    m_prevDraws   = draws;
    m_prevIdleUs  = idleUs;
  }


  void DxvkPandxvkReport::noteSubmit(uint64_t draws) {
    if (!m_enabled || !m_active)
      return;

    m_submitsObserved += 1;

    if (draws > m_drawsPerSubmitMax)
      m_drawsPerSubmitMax = draws;

    // Replay the documented HAAE rule: accumulate draws across
    // submissions, submit an empty pacemaker list and reset once
    // the threshold is reached. This observes only - the real
    // submit path is untouched - so the counts answer "how often
    // would HAAE have fired" before any of it is implemented.
    for (size_t i = 0; i < g_haaeThresholds.size(); i++) {
      m_haaeRunning[i] += draws;

      if (m_haaeRunning[i] >= g_haaeThresholds[i]) {
        m_haaeFires[i] += 1;
        m_haaeRunning[i] = 0;
      }
    }
  }


  void DxvkPandxvkReport::endSession(DxvkDevice* device) {
    if (!m_enabled || !m_active)
      return;

    writeReport(device);
    writeAttachments();

    // A marker still present here is a clean exit: remove it so
    // the next launch does not report a crash that did not happen.
    removeMarker();

    m_active = false;

    Logger::info(str::format(
      "panDXVK telemetry: wrote ", m_reportDir, "/report.json",
      " (", m_frames, " frames)"));
  }


  void DxvkPandxvkReport::removeMarker() {
    if (m_markerPath.empty())
      return;

    if (std::remove(m_markerPath.c_str()) != 0)
      Logger::debug(str::format(
        "panDXVK telemetry: marker not removed: ", m_markerPath));
  }


  void DxvkPandxvkReport::writeReport(DxvkDevice* device) {
    const std::string path = joinPath(m_reportDir, "report.json");

    std::ofstream file(path.c_str(),
      std::ios_base::out | std::ios_base::trunc);

    if (!file.is_open()) {
      Logger::warn(str::format("panDXVK telemetry: cannot write ", path));
      return;
    }

    // ---- identity -------------------------------------------------
    const auto& props = device != nullptr
      ? device->properties().core.properties
      : VkPhysicalDeviceProperties { };

    const auto& driver = device != nullptr
      ? device->properties().khrDeviceDriverProperties
      : VkPhysicalDeviceDriverPropertiesKHR { };

    // VkDriverId has no "unknown" member - the enum starts at 1 -
    // so a zeroed driverID means VK_KHR_driver_properties was not
    // enabled and this struct was never filled in.
    const bool haveDriverProps = device != nullptr
      && VkDriverId(driver.driverID) != VkDriverId(0);

    // ---- rates ----------------------------------------------------
    const double durationSec = m_frames
      ? double(m_frameSumNs) / 1.0e9
      : 0.0;

    const double avgFps = durationSec > 0.0
      ? double(m_frames) / durationSec
      : 0.0;

    const double submitsPerFrame = m_frames
      ? double(m_submitsTotal) / double(m_frames)
      : 0.0;

    const double drawsPerFrame = m_frames
      ? double(m_drawsTotal) / double(m_frames)
      : 0.0;

    const double drawsPerSubmit = m_submitsTotal
      ? double(m_drawsTotal) / double(m_submitsTotal)
      : 0.0;

    // Fraction of wall time the GPU reported itself idle, where the
    // counter is available. 0 when the driver never populated it.
    const double idleFraction = m_idleUsTotal && durationSec > 0.0
      ? (double(m_idleUsTotal) / 1.0e6) / durationSec
      : 0.0;

    // ---- emit -----------------------------------------------------
    file << "{\n";

    file << "  \"schema\": \"panDXVK.report/1\",\n";
    file << "  \"crashed_previous_session\": "
         << (m_crashedLastRun ? "true" : "false") << ",\n";

    file << "  \"build\": {\n";
    file << "    \"version\": \""
         << jsonEscape(util::panDxvkVersionString()) << "\",\n";
    file << "    \"commit\": \"" << jsonEscape(PAN_DXVK_COMMIT) << "\",\n";
    file << "    \"dxvk_base\": \"" << jsonEscape(DXVK_VERSION) << "\"\n";
    file << "  },\n";

    file << "  \"device\": {\n";
    file << "    \"name\": \"" << jsonEscape(props.deviceName) << "\",\n";
    file << "    \"vendorId\": " << props.vendorID << ",\n";
    file << "    \"deviceId\": " << props.deviceID << ",\n";
    file << "    \"apiVersion\": \""
         << jsonEscape(formatVersion(props.apiVersion)) << "\",\n";
    file << "    \"driverVersionRaw\": " << props.driverVersion << ",\n";
    file << "    \"driverVersionVulkanConvention\": \""
         << jsonEscape(formatVersion(props.driverVersion)) << "\",\n";
    file << "    \"driverName\": \""
         << jsonEscape(haveDriverProps ? driver.driverName : "") << "\",\n";
    file << "    \"driverInfo\": \""
         << jsonEscape(haveDriverProps ? driver.driverInfo : "") << "\",\n";
    file << "    \"driverPropertiesExtension\": "
         << (haveDriverProps ? "true" : "false") << "\n";
    file << "  },\n";

    file << "  \"session\": {\n";
    file << "    \"game\": \"" << jsonEscape(m_gameName) << "\",\n";
    file << "    \"duration_sec\": " << formatDouble(durationSec, 3) << ",\n";
    file << "    \"frames\": " << m_frames << ",\n";
    file << "    \"crashed\": false\n";
    file << "  },\n";

    file << "  \"frames\": {\n";
    file << "    \"avg_fps\": " << formatDouble(avgFps, 3) << ",\n";
    file << "    \"min_fps\": " << formatDouble(m_minFps, 3) << ",\n";
    file << "    \"fps_histogram_5fps_bins\": [";
    for (size_t i = 0; i < m_fpsHistogram.size(); i++) {
      if (i)
        file << ", ";
      file << m_fpsHistogram[i];
    }
    file << "]\n";
    file << "  },\n";

    file << "  \"submission\": {\n";
    file << "    \"submits\": " << m_submitsTotal << ",\n";
    file << "    \"draws\": " << m_drawsTotal << ",\n";
    file << "    \"presents\": " << m_presentTotal << ",\n";
    file << "    \"submits_per_frame\": "
         << formatDouble(submitsPerFrame, 3) << ",\n";
    file << "    \"draws_per_frame\": "
         << formatDouble(drawsPerFrame, 3) << ",\n";
    file << "    \"draws_per_submit\": "
         << formatDouble(drawsPerSubmit, 3) << ",\n";
    file << "    \"draws_per_submit_max\": " << m_drawsPerSubmitMax << ",\n";
    file << "    \"gpu_idle_fraction\": "
         << formatDouble(idleFraction, 4) << ",\n";

    file << "    \"haae_simulation\": {\n";
    file << "      \"note\": \"observational replay of the documented rule "
            "accumulate-then-reset; no pacemaker submit was issued\", \n";
    file << "      \"submits_observed\": " << m_submitsObserved << ",\n";

    for (size_t i = 0; i < g_haaeThresholds.size(); i++) {
      file << "      \"threshold_" << g_haaeThresholds[i] << "_fires\": "
           << m_haaeFires[i];
      file << (i + 1 < g_haaeThresholds.size() ? ",\n" : "\n");
    }

    file << "    }\n";
    file << "  },\n";

    const DxvkPandxvkBindTotals& binds = pandxvkBindTotals();

    file << "  \"binds\": {\n";
    file << "    \"total\": " << binds.total << ",\n";
    file << "    \"distinct\": " << binds.distinct << ",\n";
    file << "    \"skippable\": " << binds.skippable << ",\n";
    file << "    \"skippable_ratio\": "
         << formatDouble(binds.total
              ? double(binds.skippable) / double(binds.total)
              : 0.0, 6) << "\n";
    file << "  },\n";

    file << "  \"config\": {\n";
    file << "    \"telemetry\": true,\n";
    file << "    \"force_transcode\": "
         << (util::forceTranscodeEnabled() ? "true" : "false") << "\n";
    file << "  }\n";

    file << "}\n";
    file.close();
  }


  void DxvkPandxvkReport::writeAttachments() {
    // Telemetry never copies or moves a tester's logs: the originals
    // stay where the tester expects them. This file only records
    // which sibling logs existed, so an issue report can name them.
    const std::string path = joinPath(m_reportDir, "attachments.txt");

    std::ofstream file(path.c_str(),
      std::ios_base::out | std::ios_base::trunc);

    if (!file.is_open()) {
      Logger::warn(str::format("panDXVK telemetry: cannot write ", path));
      return;
    }

    file << "panDXVK telemetry - session attachments\n";
    file << "Game: " << m_gameName << "\n";
    file << "Report directory: " << m_reportDir << "\n";
    file << "\n";
    file << "Attach these alongside report.json when filing an issue:\n";

    std::string logs = env::getEnvVar("DXVK_LOG_PATH");

    if (logs == "none")
      logs.clear();

    while (!logs.empty() && logs.back() == '/')
      logs.pop_back();

    const std::filesystem::path dir = logs.empty()
      ? std::filesystem::path(".")
      : std::filesystem::path(logs);

    std::error_code ec;
    bool foundAny = false;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec)
        break;
      if (!entry.is_regular_file(ec))
        continue;
      if (entry.path().extension() != ".log")
        continue;

      file << "  " << entry.path().filename().string() << "\n";
      foundAny = true;
    }

    if (!foundAny)
      file << "  (no .log files found next to the report)\n";

    file << "\n";
    file << "Also useful:\n";
    file << "  wine_debug.log        (when running under Wine)\n";
    file << "  vulkaninfo output     (device/driver capability dump)\n";
    file.close();
  }


  void DxvkPandxvkReport::writeIssueMd(DxvkDevice* device) {
    const std::string path = joinPath(m_reportDir, "issue.md");

    std::ofstream file(path.c_str(),
      std::ios_base::out | std::ios_base::trunc);

    if (!file.is_open())
      return;

    const auto& props = device != nullptr
      ? device->properties().core.properties
      : VkPhysicalDeviceProperties { };

    const auto& driver = device != nullptr
      ? device->properties().khrDeviceDriverProperties
      : VkPhysicalDeviceDriverPropertiesKHR { };

    file << "# panDXVK crash report\n\n";
    file << "The previous session ended without a clean shutdown.\n\n";
    file << "| Field | Value |\n";
    file << "|---|---|\n";
    file << "| Game | " << m_gameName << " |\n";
    file << "| panDXVK | " << util::panDxvkVersionString() << " |\n";
    file << "| DXVK base | " << DXVK_VERSION << " |\n";
    file << "| GPU | " << props.deviceName << " |\n";
    file << "| Vendor / Device | " << props.vendorID << " / "
         << props.deviceID << " |\n";
    file << "| API version | " << formatVersion(props.apiVersion) << " |\n";
    file << "| Driver version | " << props.driverVersion << " (raw) |\n";

    if (VkDriverId(driver.driverID) != VkDriverId(0)) {
      file << "| Driver name | " << driver.driverName << " |\n";
      file << "| Driver info | " << driver.driverInfo << " |\n";
    }

    file << "\n## Attach\n\n";
    file << "See `attachments.txt` in this directory for the log files\n";
    file << "that existed next to this report. Also include:\n\n";
    file << "- `wine_debug.log`\n";
    file << "- `vulkaninfo` output\n";
    file << "- Device and driver (wrapper) versions\n";
    file.close();
  }

}
