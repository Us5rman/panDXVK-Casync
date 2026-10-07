#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace dxvk {

  class DxvkDevice;


  /**
   * \brief Bind totals accumulated across command lists
   *
   * Filled by DxvkCommandList::endRecording() and read by
   * DxvkPandxvkReport::endSession(). Lives here so that the command
   * list only needs this header rather than the whole report module.
   */
  struct DxvkPandxvkBindTotals {
    uint64_t total     = 0;
    uint64_t distinct  = 0;
    uint64_t skippable = 0;
  };


  /**
   * \brief Retrieves the process-wide bind totals
   *
   * \returns Reference to the accumulator. Never null.
   */
  DxvkPandxvkBindTotals& pandxvkBindTotals();


  /**
   * \brief Checks whether panDXVK telemetry is enabled
   *
   * Reads PANDXVK_TELEMETRY once and caches the result. Enabled
   * when the variable is set to any non-empty value other than
   * "0", matching the PANDXVK_FORCE_TRANSCODE convention.
   *
   * \returns \c true if telemetry file writes are allowed
   */
  bool pandxvkTelemetryEnabled();


  /**
   * \brief panDXVK session telemetry
   *
   * Records per-session frame metrics, submission pacing and device
   * identity, then writes them to a report directory.
   *
   * Every filesystem write is gated on PANDXVK_TELEMETRY. When the
   * gate is closed - the default - this class performs no file I/O
   * at all, so a tester's own logs (d3d11.log, wine_debug.log, ...)
   * remain the only artifacts and stay exactly where they are.
   *
   * Reports are written to a pandxvk-reports/ subdirectory rather
   * than the log directory itself, so telemetry never sits between
   * a tester and the logs they need to attach after a crash. Log
   * files are never copied or moved; attachments.txt records which
   * sibling files matter instead.
   *
   * Crash detection uses a marker file: beginSession() writes it,
   * endSession() removes it. A marker found at the next launch means
   * the previous run died without reaching endSession(), which is
   * when issue.md is emitted.
   *
   * Cost when disabled: one bool test per call. No allocation, no
   * locking, no file handles.
   */
  class DxvkPandxvkReport {

  public:

    DxvkPandxvkReport();
    ~DxvkPandxvkReport();

    /**
     * \brief Whether telemetry file writes are enabled
     *
     * \returns \c true if PANDXVK_TELEMETRY is enabled
     */
    bool enabled() const {
      return m_enabled;
    }

    /**
     * \brief Opens a session
     *
     * Resolves the report directory, looks for an orphaned crash
     * marker from the previous run, emits issue.md when one is
     * found, then writes a fresh marker. No-op when disabled.
     *
     * \param [in] device Device to collect identity from
     */
    void beginSession(DxvkDevice* device);

    /**
     * \brief Records one presented frame
     *
     * Accumulates frame time, the FPS histogram and per-frame
     * counter deltas. No-op when disabled.
     *
     * \param [in] device Device to sample counters from
     */
    void onPresent(DxvkDevice* device);

    /**
     * \brief Records one command list submission
     *
     * Feeds the HAAE pacing simulation with the draw count of the
     * submitted list. Counting only: no submission behaviour is
     * changed by this call. No-op when disabled.
     *
     * \param [in] draws Draw calls in the submitted command list
     */
    void noteSubmit(uint64_t draws);

    /**
     * \brief Closes the session and writes report.json
     *
     * Emits report.json and attachments.txt, then clears the crash
     * marker on a clean exit. No-op when disabled.
     *
     * \param [in] device Device to sample counters from
     */
    void endSession(DxvkDevice* device);

  private:

    // Gate, evaluated once. The remaining fields are only touched
    // when this is true, so a disabled session costs one branch.
    bool     m_enabled       = false;
    bool     m_active        = false;
    bool     m_haveBaseline  = false;
    bool     m_haveFps       = false;
    bool     m_crashedLastRun = false;

    uint64_t m_frames        = 0;
    uint64_t m_frameSumNs    = 0;
    uint64_t m_lastPresentNs = 0;

    double   m_fpsSum        = 0.0;
    double   m_minFps        = 0.0;

    std::array<uint32_t, 25> m_fpsHistogram = { };

    uint64_t m_prevSubmits   = 0;
    uint64_t m_prevDraws     = 0;
    uint64_t m_prevIdleUs    = 0;

    uint64_t m_submitsTotal  = 0;
    uint64_t m_drawsTotal    = 0;
    uint64_t m_idleUsTotal   = 0;
    uint64_t m_presentTotal  = 0;

    // HAAE simulation state. m_haaeRunning[i] holds draws
    // accumulated since threshold i last fired.
    uint64_t m_submitsObserved = 0;
    uint64_t m_drawsPerSubmitMax = 0;
    std::array<uint64_t, 3> m_haaeRunning = { };
    std::array<uint64_t, 3> m_haaeFires   = { };

    // Identity captured at beginSession().
    std::string m_gameName;
    std::string m_reportDir;
    std::string m_markerPath;

    static uint64_t monotonicNs();
    static std::string sanitizeName(const std::string& name);
    static std::string reportRoot();
    static std::string joinPath(const std::string& dir, const std::string& file);

    void writeReport(DxvkDevice* device);
    void writeAttachments();
    void writeIssueMd(DxvkDevice* device);
    void removeMarker();

  };

}
