#include "dxvk_swapchain_blitter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "../util/util_env.h"

#include <dxvk_fsr_easu_frag.h>
#include <dxvk_fsr_rcas_frag.h>
#include <dxvk_fxaa_frag.h>
#include <dxvk_present_frag.h>
#include <dxvk_present_frag_blit.h>
#include <dxvk_present_frag_ms.h>
#include <dxvk_present_frag_ms_amd.h>
#include <dxvk_present_vert.h>

namespace dxvk {
  
  DxvkSwapchainBlitter::DxvkSwapchainBlitter(const Rc<DxvkDevice>& device)
  : m_device(device) {
    this->readFsrConfig();
    this->readAaConfig();
    this->createSampler();
    this->createShaders();
  }


  DxvkSwapchainBlitter::~DxvkSwapchainBlitter() {
    
  }


  void DxvkSwapchainBlitter::presentImage(
          DxvkContext*        ctx,
    const Rc<DxvkImageView>&  dstView,
          VkRect2D            dstRect,
    const Rc<DxvkImageView>&  srcView,
          VkRect2D            srcRect) {
    if (m_gammaDirty)
      this->updateGammaTexture(ctx);

    // Fix up default present areas if necessary
    if (!dstRect.extent.width || !dstRect.extent.height) {
      dstRect.offset = { 0, 0 };
      dstRect.extent = {
        dstView->imageInfo().extent.width,
        dstView->imageInfo().extent.height };
    }

    if (!srcRect.extent.width || !srcRect.extent.height) {
      srcRect.offset = { 0, 0 };
      srcRect.extent = {
        srcView->imageInfo().extent.width,
        srcView->imageInfo().extent.height };
    }

    Rc<DxvkImageView> source = srcView;

    if (m_aaMode == AaMode::Fxaa) {
      const char* aaReason = this->aaBlockReason(srcView, srcRect);

      this->logAa(srcView->imageInfo().extent, aaReason);

      if (!aaReason) {
        this->presentAa(ctx, srcView);
        source = m_aaSampleView;
      }
    }

    if (m_fsrEnabled) {
      const char* reason = this->fsrBlockReason(dstView, dstRect, source, srcRect);

      this->logFsr(source->imageInfo().extent, dstView->imageInfo().extent, reason);

      if (!reason) {
        this->presentFsr(ctx, dstView, source);
        this->destroyResolveImage();
        return;
      }
    }

    bool sameSize = dstRect.extent == srcRect.extent;
    bool usedResolveImage = false;

    if (source->imageInfo().sampleCount == VK_SAMPLE_COUNT_1_BIT) {
      this->draw(ctx, sameSize ? m_fsCopy : m_fsBlit,
        dstView, dstRect, source, srcRect);
    } else if (sameSize) {
      this->draw(ctx, m_fsResolve,
        dstView, dstRect, source, srcRect);
    } else {
        if (m_resolveImage == nullptr
         || m_resolveImage->info().extent != source->imageInfo().extent
         || m_resolveImage->info().format != source->imageInfo().format)
          this->createResolveImage(source->imageInfo());

      this->resolve(ctx, m_resolveView, source);
      this->draw(ctx, m_fsBlit, dstView, dstRect, m_resolveView, srcRect);

      usedResolveImage = true;
    }

    if (!usedResolveImage)
      this->destroyResolveImage();
  }


  void DxvkSwapchainBlitter::setGammaRamp(
          uint32_t            cpCount,
    const DxvkGammaCp*        cpData) {
    VkDeviceSize size = cpCount * sizeof(*cpData);

    if (cpCount) {
      if (m_gammaBuffer == nullptr || m_gammaBuffer->info().size < size) {
        DxvkBufferCreateInfo bufInfo;
        bufInfo.size = size;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        bufInfo.access = VK_ACCESS_TRANSFER_READ_BIT;

        m_gammaBuffer = m_device->createBuffer(bufInfo,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      }

      if (!m_gammaSlice.handle)
        m_gammaSlice = m_gammaBuffer->allocSlice();

      std::memcpy(m_gammaSlice.mapPtr, cpData, size);
    } else {
      m_gammaBuffer = nullptr;
      m_gammaSlice = DxvkBufferSliceHandle();
    }

    m_gammaCpCount = cpCount;
    m_gammaDirty = true;
  }


  void DxvkSwapchainBlitter::draw(
          DxvkContext*        ctx,
    const Rc<DxvkShader>&     fs,
    const Rc<DxvkImageView>&  dstView,
          VkRect2D            dstRect,
    const Rc<DxvkImageView>&  srcView,
          VkRect2D            srcRect,
    const Rc<DxvkSampler>&    sampler,
    const void*               pushData,
          uint32_t            pushSize) {
    DxvkInputAssemblyState iaState;
    iaState.primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    iaState.primitiveRestart  = VK_FALSE;
    iaState.patchVertexCount  = 0;
    ctx->setInputAssemblyState(iaState);
    ctx->setInputLayout(0, nullptr, 0, nullptr);
    
    DxvkRasterizerState rsState;
    rsState.polygonMode        = VK_POLYGON_MODE_FILL;
    rsState.cullMode           = VK_CULL_MODE_BACK_BIT;
    rsState.frontFace          = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.depthClipEnable    = VK_FALSE;
    rsState.depthBiasEnable    = VK_FALSE;
    rsState.conservativeMode   = VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
    rsState.sampleCount        = VK_SAMPLE_COUNT_1_BIT;
    ctx->setRasterizerState(rsState);
    
    DxvkMultisampleState msState;
    msState.sampleMask            = 0xffffffff;
    msState.enableAlphaToCoverage = VK_FALSE;
    ctx->setMultisampleState(msState);
    
    VkStencilOpState stencilOp;
    stencilOp.failOp      = VK_STENCIL_OP_KEEP;
    stencilOp.passOp      = VK_STENCIL_OP_KEEP;
    stencilOp.depthFailOp = VK_STENCIL_OP_KEEP;
    stencilOp.compareOp   = VK_COMPARE_OP_ALWAYS;
    stencilOp.compareMask = 0xFFFFFFFF;
    stencilOp.writeMask   = 0xFFFFFFFF;
    stencilOp.reference   = 0;
    
    DxvkDepthStencilState dsState;
    dsState.enableDepthTest   = VK_FALSE;
    dsState.enableDepthWrite  = VK_FALSE;
    dsState.enableStencilTest = VK_FALSE;
    dsState.depthCompareOp    = VK_COMPARE_OP_ALWAYS;
    dsState.stencilOpFront    = stencilOp;
    dsState.stencilOpBack     = stencilOp;
    ctx->setDepthStencilState(dsState);
    
    DxvkLogicOpState loState;
    loState.enableLogicOp = VK_FALSE;
    loState.logicOp       = VK_LOGIC_OP_NO_OP;
    ctx->setLogicOpState(loState);

    DxvkBlendMode blendMode;
    blendMode.enableBlending  = VK_FALSE;
    blendMode.colorSrcFactor  = VK_BLEND_FACTOR_ONE;
    blendMode.colorDstFactor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendMode.colorBlendOp    = VK_BLEND_OP_ADD;
    blendMode.alphaSrcFactor  = VK_BLEND_FACTOR_ONE;
    blendMode.alphaDstFactor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendMode.alphaBlendOp    = VK_BLEND_OP_ADD;
    blendMode.writeMask       = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                              | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    ctx->setBlendMode(0, blendMode);

    VkViewport viewport;
    viewport.x        = float(dstRect.offset.x);
    viewport.y        = float(dstRect.offset.y);
    viewport.width    = float(dstRect.extent.width);
    viewport.height   = float(dstRect.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    ctx->setViewports(1, &viewport, &dstRect);

    DxvkRenderTargets renderTargets;
    renderTargets.color[0].view   = dstView;
    renderTargets.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    ctx->bindRenderTargets(renderTargets);

    VkExtent2D dstExtent = {
      dstView->imageInfo().extent.width,
      dstView->imageInfo().extent.height };

    if (dstRect.extent == dstExtent)
      ctx->discardImageView(dstView, VK_IMAGE_ASPECT_COLOR_BIT);
    else
      ctx->clearRenderTarget(dstView, VK_IMAGE_ASPECT_COLOR_BIT, VkClearValue());

    Rc<DxvkSampler> imageSampler = m_samplerPresent;

    if (sampler != nullptr)
      imageSampler = sampler;

    ctx->bindResourceSampler(BindingIds::Image, imageSampler);
    ctx->bindResourceSampler(BindingIds::Gamma, m_samplerGamma);

    ctx->bindResourceView(BindingIds::Image, srcView, nullptr);
    ctx->bindResourceView(BindingIds::Gamma, m_gammaView, nullptr);

    ctx->bindShader(VK_SHADER_STAGE_VERTEX_BIT, m_vs);
    ctx->bindShader(VK_SHADER_STAGE_FRAGMENT_BIT, fs);

    if (pushData != nullptr) {
      ctx->pushConstants(0, pushSize, pushData);
    } else {
      PresenterArgs args;
      args.srcOffset = srcRect.offset;

      if (dstRect.extent == srcRect.extent)
        args.dstOffset = dstRect.offset;
      else
        args.srcExtent = srcRect.extent;

      ctx->pushConstants(0, sizeof(args), &args);
    }

    ctx->setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, 0, srcView->imageInfo().sampleCount);
    ctx->draw(3, 1, 0, 0);
    ctx->setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0);
  }

  void DxvkSwapchainBlitter::resolve(
          DxvkContext*        ctx,
    const Rc<DxvkImageView>&  dstView,
    const Rc<DxvkImageView>&  srcView) {
    VkImageResolve resolve;
    resolve.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    resolve.srcOffset      = { 0, 0, 0 };
    resolve.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    resolve.dstOffset      = { 0, 0, 0 };
    resolve.extent         = dstView->imageInfo().extent;
    ctx->resolveImage(dstView->image(), srcView->image(), resolve, VK_FORMAT_UNDEFINED);
  }


  void DxvkSwapchainBlitter::updateGammaTexture(DxvkContext* ctx) {
    uint32_t n = m_gammaCpCount;

    if (n) {
      // Reuse existing image if possible
      if (m_gammaImage == nullptr || m_gammaImage->info().extent.width != n) {
        DxvkImageCreateInfo imgInfo;
        imgInfo.type        = VK_IMAGE_TYPE_1D;
        imgInfo.format      = VK_FORMAT_R16G16B16A16_UNORM;
        imgInfo.flags       = 0;
        imgInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.extent      = { n, 1, 1 };
        imgInfo.numLayers   = 1;
        imgInfo.mipLevels   = 1;
        imgInfo.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT
                            | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        imgInfo.access      = VK_ACCESS_TRANSFER_WRITE_BIT
                            | VK_ACCESS_SHADER_READ_BIT;
        imgInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.layout      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        
        m_gammaImage = m_device->createImage(
          imgInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        DxvkImageViewCreateInfo viewInfo;
        viewInfo.type       = VK_IMAGE_VIEW_TYPE_1D;
        viewInfo.format     = VK_FORMAT_R16G16B16A16_UNORM;
        viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
        viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.minLevel   = 0;
        viewInfo.numLevels  = 1;
        viewInfo.minLayer   = 0;
        viewInfo.numLayers  = 1;
        
        m_gammaView = m_device->createImageView(m_gammaImage, viewInfo);
      }

      ctx->invalidateBuffer(m_gammaBuffer, m_gammaSlice);
      ctx->copyBufferToImage(m_gammaImage,
        VkImageSubresourceLayers { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        VkOffset3D { 0, 0, 0 },
        VkExtent3D { n, 1, 1 },
        m_gammaBuffer, 0, 0, 0);

      m_gammaSlice = DxvkBufferSliceHandle();
    } else {
      m_gammaImage = nullptr;
      m_gammaView  = nullptr;
    }

    m_gammaDirty = false;
  }


  void DxvkSwapchainBlitter::readFsrConfig() {
    m_fsrEnabled = env::getEnvVar("PANDXVK_FSR") == "1";

    if (!m_fsrEnabled)
      return;

    std::string value = env::getEnvVar("PANDXVK_FSR_SHARPNESS");

    if (!value.empty()) {
      try {
        m_fsrSharpness = std::stof(value);
      } catch (const std::exception&) {
        Logger::warn(str::format("panDXVK: fsr: invalid PANDXVK_FSR_SHARPNESS '", value, "', using ", m_fsrSharpness));
      }
    }

    if (!std::isfinite(m_fsrSharpness))
      m_fsrSharpness = 0.8f;

    m_fsrSharpness = std::min(std::max(m_fsrSharpness, 0.0f), 1.0f);
    m_fsrRcasStops = 2.0f * (1.0f - m_fsrSharpness);

    Logger::info(str::format("panDXVK: fsr: enabled, sharpness=", m_fsrSharpness));
  }


  const char* DxvkSwapchainBlitter::fsrBlockReason(
    const Rc<DxvkImageView>&  dstView,
          VkRect2D            dstRect,
    const Rc<DxvkImageView>&  srcView,
          VkRect2D            srcRect) const {
    VkExtent3D dst = dstView->imageInfo().extent;
    VkExtent3D src = srcView->imageInfo().extent;

    if (srcView->imageInfo().sampleCount != VK_SAMPLE_COUNT_1_BIT)
      return "multisampled back buffer";

    if (m_gammaCpCount)
      return "gamma ramp active";

    if (dstRect.offset.x || dstRect.offset.y || srcRect.offset.x || srcRect.offset.y
     || dstRect.extent.width  != dst.width || dstRect.extent.height != dst.height
     || srcRect.extent.width  != src.width || srcRect.extent.height != src.height)
      return "partial present rectangle";

    if (dst.width < src.width || dst.height < src.height)
      return "output is smaller than the back buffer";

    return nullptr;
  }


  void DxvkSwapchainBlitter::presentFsr(
          DxvkContext*        ctx,
    const Rc<DxvkImageView>&  dstView,
    const Rc<DxvkImageView>&  srcView) {
    VkExtent3D dstExtent = dstView->imageInfo().extent;
    VkExtent3D srcExtent = srcView->imageInfo().extent;
    VkFormat   format    = dstView->info().format;

    FsrArgs args;
    args.srcExtent = { srcExtent.width, srcExtent.height };
    args.dstExtent = { dstExtent.width, dstExtent.height };
    args.rcasStops = m_fsrRcasStops;

    VkRect2D dstRect = { { 0, 0 }, { dstExtent.width, dstExtent.height } };
    VkRect2D srcRect = { { 0, 0 }, { srcExtent.width, srcExtent.height } };

    if (srcExtent.width == dstExtent.width && srcExtent.height == dstExtent.height) {
      this->draw(ctx, m_fsRcas, dstView, dstRect,
        srcView, srcRect, m_samplerGamma, &args, sizeof(args));
      return;
    }

    if (m_fsrImage == nullptr
     || m_fsrImage->info().extent != dstExtent
     || m_fsrImage->info().format != format)
      this->createFsrImage(dstExtent, format);

    this->draw(ctx, m_fsEasu, m_fsrTargetView, dstRect,
      srcView, srcRect, m_samplerGamma, &args, sizeof(args));

    this->draw(ctx, m_fsRcas, dstView, dstRect,
      m_fsrSampleView, dstRect, m_samplerGamma, &args, sizeof(args));
  }


  void DxvkSwapchainBlitter::logFsr(
          VkExtent3D          src,
          VkExtent3D          dst,
          const char*         reason) {
    bool active = reason == nullptr;

    bool changed = m_fsrLogSrc != src
                || m_fsrLogDst != dst
                || m_fsrLogActive != active;

    if (!changed)
      return;

    m_fsrLogSrc    = src;
    m_fsrLogDst    = dst;
    m_fsrLogActive = active;

    if (active && !(src != dst)) {
      Logger::info(str::format("panDXVK: fsr: sharpening only at ",
        dst.width, "x", dst.height, ", sharpness=", m_fsrSharpness));
    } else if (active) {
      Logger::info(str::format("panDXVK: fsr: upscaling ",
        src.width, "x", src.height, " -> ", dst.width, "x", dst.height,
        ", sharpness=", m_fsrSharpness));
    } else {
      Logger::info(str::format("panDXVK: fsr: not applied (", reason, "), back buffer ",
        src.width, "x", src.height, ", output ", dst.width, "x", dst.height));
    }
  }


  void DxvkSwapchainBlitter::readAaConfig() {
    std::string value = env::getEnvVar("PANDXVK_AA");

    if (value.empty())
      return;

    for (char& ch : value)
      ch = char(std::toupper(static_cast<unsigned char>(ch)));

    if (value == "FXAA") {
      m_aaMode = AaMode::Fxaa;
      Logger::info("panDXVK: aa: FXAA enabled");
    } else if (value == "SMAA") {
      Logger::warn("panDXVK: aa: SMAA is not available in this build yet, anti-aliasing stays off");
    } else if (value != "0" && value != "OFF" && value != "NONE") {
      Logger::warn(str::format("panDXVK: aa: unknown PANDXVK_AA value '", value, "', anti-aliasing stays off"));
    }
  }


  const char* DxvkSwapchainBlitter::aaBlockReason(
    const Rc<DxvkImageView>&  srcView,
          VkRect2D            srcRect) const {
    VkExtent3D src = srcView->imageInfo().extent;

    if (srcView->imageInfo().sampleCount != VK_SAMPLE_COUNT_1_BIT)
      return "multisampled back buffer";

    if (srcRect.offset.x || srcRect.offset.y
     || srcRect.extent.width != src.width || srcRect.extent.height != src.height)
      return "partial present rectangle";

    return nullptr;
  }


  void DxvkSwapchainBlitter::presentAa(
          DxvkContext*        ctx,
    const Rc<DxvkImageView>&  srcView) {
    VkExtent3D extent = srcView->imageInfo().extent;
    VkFormat   format = srcView->info().format;

    if (m_aaImage == nullptr
     || m_aaImage->info().extent != extent
     || m_aaImage->info().format != format)
      this->createAaImage(extent, format);

    AaArgs args;
    args.extent = { extent.width, extent.height };

    VkRect2D rect = { { 0, 0 }, { extent.width, extent.height } };

    this->draw(ctx, m_fsFxaa, m_aaTargetView, rect,
      srcView, rect, m_samplerGamma, &args, sizeof(args));
  }


  void DxvkSwapchainBlitter::logAa(
          VkExtent3D          src,
    const char*               reason) {
    bool active = reason == nullptr;

    bool changed = m_aaLogSrc != src
                || m_aaLogActive != active;

    if (!changed)
      return;

    m_aaLogSrc    = src;
    m_aaLogActive = active;

    if (active) {
      Logger::info(str::format("panDXVK: aa: FXAA applied at ",
        src.width, "x", src.height));
    } else {
      Logger::info(str::format("panDXVK: aa: not applied (", reason, "), back buffer ",
        src.width, "x", src.height));
    }
  }


  void DxvkSwapchainBlitter::createSampler() {
    DxvkSamplerCreateInfo samplerInfo;
    samplerInfo.magFilter       = VK_FILTER_LINEAR;
    samplerInfo.minFilter       = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode      = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.mipmapLodBias   = 0.0f;
    samplerInfo.mipmapLodMin    = 0.0f;
    samplerInfo.mipmapLodMax    = 0.0f;
    samplerInfo.useAnisotropy   = VK_FALSE;
    samplerInfo.maxAnisotropy   = 1.0f;
    samplerInfo.addressModeU    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.compareToDepth  = VK_FALSE;
    samplerInfo.compareOp       = VK_COMPARE_OP_ALWAYS;
    samplerInfo.borderColor     = VkClearColorValue();
    samplerInfo.usePixelCoord   = VK_TRUE;
    samplerInfo.nonSeamless     = VK_FALSE;
    m_samplerPresent = m_device->createSampler(samplerInfo);

    samplerInfo.addressModeU    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.usePixelCoord   = VK_FALSE;
    m_samplerGamma = m_device->createSampler(samplerInfo);
  }

  void DxvkSwapchainBlitter::createShaders() {
    SpirvCodeBuffer vsCode(dxvk_present_vert);
    SpirvCodeBuffer fsCodeBlit(dxvk_present_frag_blit);
    SpirvCodeBuffer fsCodeCopy(dxvk_present_frag);
    SpirvCodeBuffer fsCodeResolve(dxvk_present_frag_ms);
    SpirvCodeBuffer fsCodeResolveAmd(dxvk_present_frag_ms_amd);
    SpirvCodeBuffer fsCodeEasu(dxvk_fsr_easu_frag);
    SpirvCodeBuffer fsCodeRcas(dxvk_fsr_rcas_frag);
    SpirvCodeBuffer fsCodeFxaa(dxvk_fxaa_frag);
    
    const std::array<DxvkResourceSlot, 2> fsResourceSlots = {{
      { BindingIds::Image, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D },
      { BindingIds::Gamma, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_1D },
    }};

    DxvkShaderCreateInfo vsInfo;
    vsInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vsInfo.outputMask = 0x1;
    m_vs = new DxvkShader(vsInfo, std::move(vsCode));
    
    DxvkShaderCreateInfo fsInfo;
    fsInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fsInfo.resourceSlotCount = fsResourceSlots.size();
    fsInfo.resourceSlots = fsResourceSlots.data();
    fsInfo.pushConstSize = sizeof(PresenterArgs);
    fsInfo.inputMask = 0x1;
    fsInfo.outputMask = 0x1;
    m_fsBlit = new DxvkShader(fsInfo, std::move(fsCodeBlit));
    
    fsInfo.inputMask = 0;
    m_fsCopy = new DxvkShader(fsInfo, std::move(fsCodeCopy));
    m_fsResolve = new DxvkShader(fsInfo, m_device->extensions().amdShaderFragmentMask
      ? std::move(fsCodeResolveAmd)
      : std::move(fsCodeResolve));

    if (m_fsrEnabled) {
      DxvkShaderCreateInfo fsrInfo = fsInfo;
      fsrInfo.inputMask = 0;
      fsrInfo.pushConstSize = sizeof(FsrArgs);
      m_fsEasu = new DxvkShader(fsrInfo, std::move(fsCodeEasu));
      m_fsRcas = new DxvkShader(fsrInfo, std::move(fsCodeRcas));
    }

    if (m_aaMode == AaMode::Fxaa) {
      DxvkShaderCreateInfo aaInfo = fsInfo;
      aaInfo.inputMask = 0;
      aaInfo.pushConstSize = sizeof(AaArgs);
      m_fsFxaa = new DxvkShader(aaInfo, std::move(fsCodeFxaa));
    }
  }

  void DxvkSwapchainBlitter::createResolveImage(const DxvkImageCreateInfo& info) {
    DxvkImageCreateInfo newInfo;
    newInfo.type = VK_IMAGE_TYPE_2D;
    newInfo.format = info.format;
    newInfo.flags = 0;
    newInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    newInfo.extent = info.extent;
    newInfo.numLayers = 1;
    newInfo.mipLevels = 1;
    newInfo.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT;
    newInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                   | VK_PIPELINE_STAGE_TRANSFER_BIT
                   | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    newInfo.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                   | VK_ACCESS_TRANSFER_WRITE_BIT
                   | VK_ACCESS_SHADER_READ_BIT;
    newInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    newInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_resolveImage = m_device->createImage(newInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.format;
    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel  = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer  = 0;
    viewInfo.numLayers = 1;
    m_resolveView = m_device->createImageView(m_resolveImage, viewInfo);
  }


  void DxvkSwapchainBlitter::destroyResolveImage() {
    m_resolveImage = nullptr;
    m_resolveView = nullptr;
  }


  void DxvkSwapchainBlitter::createFsrImage(
          VkExtent3D          extent,
          VkFormat            format) {
    DxvkImageCreateInfo imgInfo;
    imgInfo.type = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.flags = 0;
    imgInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.extent = extent;
    imgInfo.numLayers = 1;
    imgInfo.mipLevels = 1;
    imgInfo.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                   | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    imgInfo.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                   | VK_ACCESS_SHADER_READ_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_fsrImage = m_device->createImage(imgInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel  = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer  = 0;
    viewInfo.numLayers = 1;

    viewInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    m_fsrTargetView = m_device->createImageView(m_fsrImage, viewInfo);

    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    m_fsrSampleView = m_device->createImageView(m_fsrImage, viewInfo);
  }

  void DxvkSwapchainBlitter::createAaImage(
          VkExtent3D          extent,
          VkFormat            format) {
    DxvkImageCreateInfo imgInfo;
    imgInfo.type = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.flags = 0;
    imgInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.extent = extent;
    imgInfo.numLayers = 1;
    imgInfo.mipLevels = 1;
    imgInfo.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                   | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    imgInfo.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                   | VK_ACCESS_SHADER_READ_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_aaImage = m_device->createImage(imgInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel  = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer  = 0;
    viewInfo.numLayers = 1;

    viewInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    m_aaTargetView = m_device->createImageView(m_aaImage, viewInfo);

    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    m_aaSampleView = m_device->createImageView(m_aaImage, viewInfo);
  }

}
