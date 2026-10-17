#pragma once

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_context.h"

namespace dxvk {

  /**
   * \brief Gamma control point
   */
  struct DxvkGammaCp {
    uint16_t r, g, b, a;
  };
  
  /**
   * \brief Swap chain blitter
   *
   * Provides common rendering code for blitting
   * rendered images to a swap chain image.
   */
  class DxvkSwapchainBlitter : public RcObject {
    
  public:

    DxvkSwapchainBlitter(const Rc<DxvkDevice>& device);
    ~DxvkSwapchainBlitter();

    /**
     * \brief Records presentation commands
     *
     * \param [in] ctx Context
     * \param [in] dstView Swap chain image view
     * \param [in] srcView Image to present
     * \param [in] dstRect Destination rectangle
     * \param [in] srcRect Back buffer rectangle
     */
    void presentImage(
            DxvkContext*        ctx,
      const Rc<DxvkImageView>&  dstView,
            VkRect2D            dstRect,
      const Rc<DxvkImageView>&  srcView,
            VkRect2D            srcRect);

    /**
     * \brief Sets gamma ramp
     *
     * If the number of control points is non-zero, this
     * will create a texture containing a gamma ramp that
     * will be used for presentation.
     * \param [in] cpCount Number of control points
     * \param [in] cpData Control point data
     */
    void setGammaRamp(
            uint32_t            cpCount,
      const DxvkGammaCp*        cpData);

  private:

    enum BindingIds : uint32_t {
      Image = 0,
      Gamma = 1,
    };

    struct PresenterArgs {
      VkOffset2D srcOffset;
      union {
        VkExtent2D srcExtent;
        VkOffset2D dstOffset;
      };
    };

    struct FsrArgs {
      VkExtent2D srcExtent;
      VkExtent2D dstExtent;
      float      rcasStops;
    };

    struct AaArgs {
      VkExtent2D extent;
    };

    enum class AaMode : uint32_t {
      None,
      Fxaa,
      Smaa,
    };

    Rc<DxvkDevice>      m_device;

    Rc<DxvkShader>      m_fsCopy;
    Rc<DxvkShader>      m_fsBlit;
    Rc<DxvkShader>      m_fsResolve;
    Rc<DxvkShader>      m_vs;

    Rc<DxvkShader>      m_fsEasu;
    Rc<DxvkShader>      m_fsRcas;
    Rc<DxvkShader>      m_fsFxaa;

    Rc<DxvkBuffer>      m_gammaBuffer;
    Rc<DxvkImage>       m_gammaImage;
    Rc<DxvkImageView>   m_gammaView;
    uint32_t            m_gammaCpCount = 0;
    bool                m_gammaDirty = false;
    DxvkBufferSliceHandle m_gammaSlice = { };

    Rc<DxvkImage>       m_resolveImage;
    Rc<DxvkImageView>   m_resolveView;

    Rc<DxvkImage>       m_fsrImage;
    Rc<DxvkImageView>   m_fsrTargetView;
    Rc<DxvkImageView>   m_fsrSampleView;

    Rc<DxvkImage>       m_aaImage;
    Rc<DxvkImageView>   m_aaTargetView;
    Rc<DxvkImageView>   m_aaSampleView;

    Rc<DxvkSampler>     m_samplerPresent;
    Rc<DxvkSampler>     m_samplerGamma;

    bool                m_fsrEnabled   = false;
    float               m_fsrSharpness = 0.8f;
    float               m_fsrRcasStops = 0.4f;

    VkExtent3D          m_fsrLogSrc    = { 0, 0, 0 };
    VkExtent3D          m_fsrLogDst    = { 0, 0, 0 };
    bool                m_fsrLogActive = false;

    AaMode              m_aaMode       = AaMode::None;
    VkExtent3D          m_aaLogSrc     = { 0, 0, 0 };
    bool                m_aaLogActive  = false;

    void draw(
            DxvkContext*        ctx,
      const Rc<DxvkShader>&     fs,
      const Rc<DxvkImageView>&  dstView,
            VkRect2D            dstRect,
      const Rc<DxvkImageView>&  srcView,
            VkRect2D            srcRect,
      const Rc<DxvkSampler>&    sampler  = nullptr,
      const void*               pushData = nullptr,
            uint32_t            pushSize = 0);

    void resolve(
            DxvkContext*        ctx,
      const Rc<DxvkImageView>&  dstView,
      const Rc<DxvkImageView>&  srcView);

    void updateGammaTexture(DxvkContext* ctx);

    void readFsrConfig();

    const char* fsrBlockReason(
      const Rc<DxvkImageView>&  dstView,
            VkRect2D            dstRect,
      const Rc<DxvkImageView>&  srcView,
            VkRect2D            srcRect) const;

    void presentFsr(
            DxvkContext*        ctx,
      const Rc<DxvkImageView>&  dstView,
      const Rc<DxvkImageView>&  srcView);

    void logFsr(
            VkExtent3D          src,
            VkExtent3D          dst,
            const char*         reason);

    void readAaConfig();

    const char* aaBlockReason(
      const Rc<DxvkImageView>&  srcView,
            VkRect2D            srcRect) const;

    void presentAa(
            DxvkContext*        ctx,
      const Rc<DxvkImageView>&  srcView);

    void logAa(
            VkExtent3D          src,
            const char*         reason);

    void createAaImage(
            VkExtent3D          extent,
            VkFormat            format);

    void createSampler();

    void createShaders();

    void createResolveImage(
      const DxvkImageCreateInfo&  info);

    void destroyResolveImage();

    void createFsrImage(
            VkExtent3D          extent,
            VkFormat            format);

  };
  
}
