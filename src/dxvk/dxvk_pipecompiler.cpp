#include <algorithm>

#include "../util/util_env.h"

#include "dxvk_graphics.h"
#include "dxvk_pipecompiler.h"

namespace dxvk {

  DxvkPipelineCompiler::DxvkPipelineCompiler(uint32_t numThreads) {
    m_workers.reserve(numThreads);

    for (uint32_t i = 0; i < numThreads; i++)
      m_workers.emplace_back([this] { this->runWorker(); });
  }


  DxvkPipelineCompiler::~DxvkPipelineCompiler() {
    this->stopWorkerThreads();
  }


  void DxvkPipelineCompiler::queueCompilation(
          DxvkGraphicsPipeline*           pipeline,
    const DxvkGraphicsPipelineStateInfo&  state,
    const DxvkRenderPass*                 renderPass) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_stop)
      return;

    for (const auto& entry : m_entries) {
      if (entry.pipeline == pipeline
       && entry.renderPass == renderPass
       && entry.state == state)
        return;
    }

    m_entries.push_back({ pipeline, renderPass, state, false });
    m_cond.notify_one();
  }


  bool DxvkPipelineCompiler::isBusy() const {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    return !m_entries.empty();
  }


  void DxvkPipelineCompiler::stopWorkerThreads() {
    {
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      m_stop = true;
      m_cond.notify_all();
    }

    for (auto& worker : m_workers) {
      if (worker.joinable())
        worker.join();
    }
  }


  void DxvkPipelineCompiler::runWorker() {
    env::setThreadName("dxvk-async");

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    while (!m_stop) {
      auto entry = std::find_if(m_entries.begin(), m_entries.end(),
        [] (const Entry& e) { return !e.started; });

      if (entry == m_entries.end()) {
        m_cond.wait(lock);
        continue;
      }

      entry->started = true;
      lock.unlock();

      entry->pipeline->compilePipelineAsync(entry->state, entry->renderPass);

      lock.lock();
      m_entries.erase(entry);
    }
  }

}
