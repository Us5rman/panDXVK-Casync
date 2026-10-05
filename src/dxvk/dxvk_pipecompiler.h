#pragma once

#include <list>
#include <vector>

#include "../util/thread.h"

#include "dxvk_graphics_state.h"

namespace dxvk {

  class DxvkGraphicsPipeline;
  class DxvkRenderPass;

  /**
   * \brief Async pipeline compiler
   *
   * Compiles graphics pipelines on worker threads so
   * that the thread recording draws does not have to
   * wait for the driver.
   */
  class DxvkPipelineCompiler {

  public:

    DxvkPipelineCompiler(uint32_t numThreads);

    ~DxvkPipelineCompiler();

    /**
     * \brief Queues a pipeline for compilation
     *
     * Does nothing if the same pipeline
     * is already queued or being compiled.
     * \param [in] pipeline The graphics pipeline
     * \param [in] state Pipeline state vector
     * \param [in] renderPass The render pass
     */
    void queueCompilation(
            DxvkGraphicsPipeline*           pipeline,
      const DxvkGraphicsPipelineStateInfo&  state,
      const DxvkRenderPass*                 renderPass);

    /**
     * \brief Checks whether the compiler is busy
     * \returns \c true if pipelines are pending
     */
    bool isBusy() const;

    /**
     * \brief Stops the worker threads
     */
    void stopWorkerThreads();

  private:

    struct Entry {
      DxvkGraphicsPipeline*          pipeline;
      const DxvkRenderPass*          renderPass;
      DxvkGraphicsPipelineStateInfo  state;
      bool                           started;
    };

    mutable dxvk::mutex       m_mutex;
    dxvk::condition_variable  m_cond;

    std::list<Entry>          m_entries;
    std::vector<dxvk::thread> m_workers;

    bool                      m_stop = false;

    void runWorker();

  };

}
