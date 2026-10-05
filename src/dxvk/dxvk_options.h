#pragma once

#include "../util/config/config.h"

namespace dxvk {

  struct DxvkOptions {
    DxvkOptions() { }
    DxvkOptions(const Config& config);

    /// Enable debug utils (alternative to DXVK_PERF_EVENTS=1)
    bool enableDebugUtils;

    /// Enable state cache
    bool enableStateCache;

    /// Number of compiler threads
    /// when using the state cache
    int32_t numCompilerThreads;

    /// Compile graphics pipelines asynchronously
    bool enableAsync;

    /// Compile graphics pipelines asynchronously
    /// using graphics pipeline libraries
    bool enableGplAsync;

    /// Compile graphics pipelines asynchronously
    /// with dynamic state
    bool enableDyAsync;

    /// Number of async compiler threads
    int32_t numAsyncThreads;

    /// Shader-related options
    Tristate useRawSsbo;

    /// Workaround for NVIDIA driver bug 3114283
    Tristate shrinkNvidiaHvvHeap;

    /// HUD elements
    std::string hud;
  };

}
