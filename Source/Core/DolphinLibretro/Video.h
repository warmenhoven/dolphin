#pragma once

#include <libretro.h>
#include "DolphinLibretro/Common/Globals.h"
#include "DolphinLibretro/Common/Options.h"
#include "VideoBackends/Null/NullGfx.h"
#include "VideoBackends/Software/SWOGLWindow.h"
#include "VideoBackends/Software/SWGfx.h"
#include "VideoBackends/Software/SWTexture.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "Common/Logging/Log.h"
#ifdef _WIN32
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D/D3DSwapChain.h"
#endif
#ifdef HAS_VULKAN
#ifndef __APPLE__
#include "VideoBackends/Vulkan/VulkanLoader.h"
#endif
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "DolphinLibretro/Vulkan.h"
#include <libretro_vulkan.h>
#endif

namespace Libretro
{
namespace Video
{
void Init(void);
bool Video_InitializeBackend();
bool SetHWRender(retro_hw_context_type type, const int version_major = -1, const int version_minor = -1);
void ContextReset(void);
void ContextDestroy(void);

class SWGfx : public SW::SWGfx
{
public:
  SWGfx()
    : SW::SWGfx(SWOGLWindow::Create(
            WindowSystemInfo(WindowSystemType::Libretro, nullptr, nullptr, nullptr)))
  {
  }
  void ShowImage(const AbstractTexture* source_texture,
                 const MathUtil::Rectangle<int>& source_rc) override
  {
    SW::SWGfx::ShowImage(source_texture, source_rc);
    video_cb(
      static_cast<const SW::SWTexture*>(source_texture)->GetData(0, 0),
      source_rc.GetWidth(),
      source_rc.GetHeight(),
      source_texture->GetWidth() * 4
    );
    UpdateActiveConfig();
  }
};

class NullGfx : public Null::NullGfx
{
public:
  void ShowImage(const AbstractTexture* source_texture,
                 const MathUtil::Rectangle<int>& source_rc) override
  {
    video_cb(NULL, 512, 512, 512 * 4);
    UpdateActiveConfig();
  }
};

#ifdef _WIN32
class DX11SwapChain : public DX11::SwapChain
{
public:
  DX11SwapChain(const WindowSystemInfo& wsi, int width, int height,
                IDXGIFactory* dxgi_factory, ID3D11Device* d3d_device)
      : DX11::SwapChain(wsi, dxgi_factory, d3d_device)
  {
    m_width = width;
    m_height = height;
    m_stereo = WantsStereo();
    CreateSwapChainBuffers();
  }

  bool Present() override
  {
    auto* tex = GetTexture();
    if (!tex)
    {
        ERROR_LOG_FMT(VIDEO, "Present aborted: no swap chain texture");
        return false;
    }

    auto* srv = tex->GetD3DSRV();
    if (!srv)
    {
        ERROR_LOG_FMT(VIDEO, "Present aborted: no SRV for swap chain texture");
        return false;
    }

    ID3D11RenderTargetView* nullView = nullptr;
    DX11::D3D::context->OMSetRenderTargets(1, &nullView, nullptr);
    DX11::D3D::context->PSSetShaderResources(0, 1, &srv);

    Libretro::Video::video_cb(RETRO_HW_FRAME_BUFFER_VALID,
                              m_width, m_height,
                              m_width);

    DX11::D3D::stateman->Restore();
    return true;
  }

protected:
  bool CreateSwapChainBuffers() override
  {
    TextureConfig config(m_width, m_height, 1, 1, 1,
                         AbstractTextureFormat::RGBA8,
                         AbstractTextureFlag_RenderTarget,
                         AbstractTextureType::Texture_2D);

    auto tex = DX11::DXTexture::Create(config, "LibretroSwapChainTexture");
    if (!tex)
    {
      ERROR_LOG_FMT(VIDEO, "Backbuffer texture creation failed");
      return false;
    }
    SetTexture(std::move(tex));

    auto fb = DX11::DXFramebuffer::Create(GetTexture(), nullptr, {});
    if (!fb)
    {
      ERROR_LOG_FMT(VIDEO, "Backbuffer framebuffer creation failed");
      return false;
    }
    SetFramebuffer(std::move(fb));

    return true;
  }
};

#endif

#ifdef HAS_VULKAN
namespace Vk
{
const VkApplicationInfo* GetApplicationInfo(void);

#ifdef __APPLE__
VkInstance CreateInstance(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                          const VkApplicationInfo* app,
                          retro_vulkan_create_instance_wrapper_t create_instance_wrapper,
                          void* opaque);
#endif
bool CreateDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                  VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                  const char** required_device_extensions,
                  unsigned num_required_device_extensions,
                  const char** required_device_layers, unsigned num_required_device_layers,
                  const VkPhysicalDeviceFeatures* required_features);
} // namespace Vk
#endif

}  // namespace Video
}  // namespace Libretro
