#include "dxvk_cmdlist.h"
#include "dxvk_device.h"

#include <algorithm>

namespace dxvk {
    
  DxvkCommandList::DxvkCommandList(DxvkDevice* device)
  : m_device        (device),
    m_vkd           (device->vkd()),
    m_vki           (device->instance()->vki()),
    m_cmdBuffersUsed(0),
    m_descriptorPoolTracker(device) {
    const auto& graphicsQueue = m_device->queues().graphics;
    const auto& transferQueue = m_device->queues().transfer;

    VkFenceCreateInfo fenceInfo;
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = nullptr;
    fenceInfo.flags = 0;
    
    if (m_vkd->vkCreateFence(m_vkd->device(), &fenceInfo, nullptr, &m_fence) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to create fence");
    
    VkCommandPoolCreateInfo poolInfo;
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.pNext            = nullptr;
    poolInfo.flags            = 0;
    poolInfo.queueFamilyIndex = graphicsQueue.queueFamily;
    
    if (m_vkd->vkCreateCommandPool(m_vkd->device(), &poolInfo, nullptr, &m_graphicsPool) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to create graphics command pool");
    
    if (m_device->hasDedicatedTransferQueue()) {
      poolInfo.queueFamilyIndex = transferQueue.queueFamily;

      if (m_vkd->vkCreateCommandPool(m_vkd->device(), &poolInfo, nullptr, &m_transferPool) != VK_SUCCESS)
        throw DxvkError("DxvkCommandList: Failed to create transfer command pool");
    }
    
    VkCommandBufferAllocateInfo cmdInfoGfx;
    cmdInfoGfx.sType             = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfoGfx.pNext             = nullptr;
    cmdInfoGfx.commandPool       = m_graphicsPool;
    cmdInfoGfx.level             = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfoGfx.commandBufferCount = 1;
    
    VkCommandBufferAllocateInfo cmdInfoDma;
    cmdInfoDma.sType             = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfoDma.pNext             = nullptr;
    cmdInfoDma.commandPool       = m_transferPool ? m_transferPool : m_graphicsPool;
    cmdInfoDma.level             = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfoDma.commandBufferCount = 1;
    
    if (m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoGfx, &m_execBuffer) != VK_SUCCESS
     || m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoGfx, &m_initBuffer) != VK_SUCCESS
     || m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoDma, &m_sdmaBuffer) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to allocate command buffer");
    
    if (m_device->hasDedicatedTransferQueue()) {
      VkSemaphoreCreateInfo semInfo;
      semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semInfo.pNext = nullptr;
      semInfo.flags = 0;

      if (m_vkd->vkCreateSemaphore(m_vkd->device(), &semInfo, nullptr, &m_sdmaSemaphore) != VK_SUCCESS)
        throw DxvkError("DxvkCommandList: Failed to create semaphore");
    }
  }
  
  
  DxvkCommandList::~DxvkCommandList() {
    this->reset();

    m_vkd->vkDestroySemaphore(m_vkd->device(), m_sdmaSemaphore, nullptr);
    
    m_vkd->vkDestroyCommandPool(m_vkd->device(), m_graphicsPool, nullptr);
    m_vkd->vkDestroyCommandPool(m_vkd->device(), m_transferPool, nullptr);
    
    m_vkd->vkDestroyFence(m_vkd->device(), m_fence, nullptr);
  }
  
  
  VkResult DxvkCommandList::submit(
          VkSemaphore     waitSemaphore,
          VkSemaphore     wakeSemaphore) {
    const auto& graphics = m_device->queues().graphics;
    const auto& transfer = m_device->queues().transfer;

    m_submission.reset();

    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::SdmaBuffer)) {
      m_submission.cmdBuffers.push_back(m_sdmaBuffer);

      if (m_device->hasDedicatedTransferQueue()) {
        m_submission.addWakeSemaphore(m_sdmaSemaphore, 0);
        VkResult status = submitToQueue(transfer.queueHandle, VK_NULL_HANDLE, m_submission);

        if (status != VK_SUCCESS)
          return status;

        m_submission.reset();
        m_submission.addWaitSemaphore(m_sdmaSemaphore, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      }
    }

    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::InitBuffer))
      m_submission.cmdBuffers.push_back(m_initBuffer);
    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::ExecBuffer))
      m_submission.cmdBuffers.push_back(m_execBuffer);
    
    if (waitSemaphore)
      m_submission.addWaitSemaphore(waitSemaphore, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    if (wakeSemaphore)
      m_submission.addWakeSemaphore(wakeSemaphore, 0);
    
    for (const auto& entry : m_waitSemaphores) {
      m_submission.addWaitSemaphore(entry.fence->handle(), entry.value, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }

    for (const auto& entry : m_signalSemaphores) {
      m_submission.addWakeSemaphore(entry.fence->handle(), entry.value);
    }

    return submitToQueue(graphics.queueHandle, m_fence, m_submission);
  }
  
  
  VkResult DxvkCommandList::synchronize() {
    VkResult status = VK_TIMEOUT;
    
    while (status == VK_TIMEOUT) {
      status = m_vkd->vkWaitForFences(
        m_vkd->device(), 1, &m_fence, VK_FALSE,
        1'000'000'000ull);
    }
    
    return status;
  }
  
  
  void DxvkCommandList::beginRecording() {
    m_dbgBinds.clear();
    VkCommandBufferBeginInfo info;
    info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.pNext            = nullptr;
    info.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    info.pInheritanceInfo = nullptr;
    
    if ((m_graphicsPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_graphicsPool, 0) != VK_SUCCESS)
     || (m_transferPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_transferPool, 0) != VK_SUCCESS))
      Logger::err("DxvkCommandList: Failed to reset command buffer");
    
    if (m_vkd->vkBeginCommandBuffer(m_execBuffer, &info) != VK_SUCCESS
     || m_vkd->vkBeginCommandBuffer(m_initBuffer, &info) != VK_SUCCESS
     || m_vkd->vkBeginCommandBuffer(m_sdmaBuffer, &info) != VK_SUCCESS)
      Logger::err("DxvkCommandList: Failed to begin command buffer");
    
    if (m_vkd->vkResetFences(m_vkd->device(), 1, &m_fence) != VK_SUCCESS)
      Logger::err("DxvkCommandList: Failed to reset fence");
    
    // Unconditionally mark the exec buffer as used. There
    // is virtually no use case where this isn't correct.
    m_cmdBuffersUsed = DxvkCmdBuffer::ExecBuffer;
  }
  
  
  void DxvkCommandList::endRecording() {
    if (m_vkd->vkEndCommandBuffer(m_execBuffer) != VK_SUCCESS
     || m_vkd->vkEndCommandBuffer(m_initBuffer) != VK_SUCCESS
     || m_vkd->vkEndCommandBuffer(m_sdmaBuffer) != VK_SUCCESS)
      Logger::err("DxvkCommandList::endRecording: Failed to record command buffer");

    // Bind telemetry. Emitted here rather than from cmdBindPipeline so that
    // no logging ever sits in a per-bind path. Gated at runtime rather than
    // on NDEBUG, following bank Session 23 (8b04f8cc): evidence meant to
    // reach a shipped log is gated on Logger::logLevel(), not compiled out,
    // because NDEBUG-gating loses it from exactly the build that runs.
    // "skippable" is an upper bound: it counts back-to-back repeats of the
    // same (bindPoint, pipeline) pair, which is the only thing a
    // last-bound-pipeline cache could drop, and it does not model the cache
    // invalidation such a cache would need on render pass spill, flush,
    // pipeline unbind or command list boundaries.
    if (!m_dbgBinds.empty() && Logger::logLevel() <= LogLevel::Debug) {
      std::vector<std::pair<VkPipelineBindPoint, VkPipeline>> sorted = m_dbgBinds;
      std::sort(sorted.begin(), sorted.end());

      const uint64_t distinct = uint64_t(std::distance(sorted.begin(),
        std::unique(sorted.begin(), sorted.end())));

      uint64_t skippable = 0;
      for (size_t i = 1; i < m_dbgBinds.size(); i++) {
        if (m_dbgBinds[i] == m_dbgBinds[i - 1])
          skippable += 1;
      }

      // Fold the same three numbers into the process-wide totals so
      // report.json can carry them without a second pass over the
      // log. Only reachable under the debug gate above, because
      // m_dbgBinds itself is only filled when that gate passes -
      // bind totals therefore require DXVK_LOG_LEVEL=debug, same as
      // the log lines they mirror.
      DxvkPandxvkBindTotals& totals = pandxvkBindTotals();
      totals.total     += uint64_t(m_dbgBinds.size());
      totals.distinct  += distinct;
      totals.skippable += skippable;

      Logger::debug(str::format(
        "dxvk bind stats: total=", uint64_t(m_dbgBinds.size()),
        " distinct=", distinct,
        " skippable=", skippable));
    }

    m_dbgBinds.clear();
  }
  
  
  void DxvkCommandList::reset() {
    // Free resources and other objects
    // that are no longer in use
    m_resources.reset();

    // Recycle heavy Vulkan objects
    m_descriptorPoolTracker.reset();

    // Return buffer memory slices
    m_bufferTracker.reset();

    // Return query and event handles
    m_gpuQueryTracker.reset();
    m_gpuEventTracker.reset();

    // Less important stuff
    m_signalTracker.reset();
    m_statCounters.reset();

    m_waitSemaphores.clear();
    m_signalSemaphores.clear();
  }


  VkResult DxvkCommandList::submitToQueue(
          VkQueue               queue,
          VkFence               fence,
    const DxvkQueueSubmission&  info) {
    VkTimelineSemaphoreSubmitInfoKHR timelineInfo = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR };
    timelineInfo.waitSemaphoreValueCount    = info.waitValues.size();
    timelineInfo.pWaitSemaphoreValues       = info.waitValues.data();
    timelineInfo.signalSemaphoreValueCount  = info.wakeValues.size();
    timelineInfo.pSignalSemaphoreValues     = info.wakeValues.data();

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.waitSemaphoreCount   = info.waitSync.size();
    submitInfo.pWaitSemaphores      = info.waitSync.data();
    submitInfo.pWaitDstStageMask    = info.waitMask.data();
    submitInfo.commandBufferCount   = info.cmdBuffers.size();
    submitInfo.pCommandBuffers      = info.cmdBuffers.data();
    submitInfo.signalSemaphoreCount = info.wakeSync.size();
    submitInfo.pSignalSemaphores    = info.wakeSync.data();

    if (m_device->features().khrTimelineSemaphore.timelineSemaphore)
      submitInfo.pNext = &timelineInfo;
    
    return m_vkd->vkQueueSubmit(queue, 1, &submitInfo, fence);
  }
  
  void DxvkCommandList::cmdBeginDebugUtilsLabel(VkDebugUtilsLabelEXT *pLabelInfo) {
    m_vki->vkCmdBeginDebugUtilsLabelEXT(m_execBuffer, pLabelInfo);
  }

  void DxvkCommandList::cmdEndDebugUtilsLabel() {
    m_vki->vkCmdEndDebugUtilsLabelEXT(m_execBuffer);
  }

  void DxvkCommandList::cmdInsertDebugUtilsLabel(VkDebugUtilsLabelEXT *pLabelInfo) {
    m_vki->vkCmdInsertDebugUtilsLabelEXT(m_execBuffer, pLabelInfo);
  }
}
