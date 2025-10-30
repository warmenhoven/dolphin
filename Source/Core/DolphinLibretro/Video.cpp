// needs to be first
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VulkanLoader.h"
#endif
#include "DolphinLibretro/Video.h"

#include <cassert>
#include <libretro.h>
#include <memory>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define HAVE_D3D11
#include <libretro_d3d.h>
#include "Common/DynamicLibrary.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/VideoBackend.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DVertexManager.h"
#include "VideoBackends/D3D/D3DPerfQuery.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#endif

#include "Common/GL/GLContext.h"
#include "Common/GL/GLUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "DolphinLibretro/Common/Options.h"
#include "DolphinLibretro/VideoContexts/ContextStatus.h"
#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/VKSwapChain.h"
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKVertexManager.h"
#include "VideoBackends/Vulkan/VKPerfQuery.h"
#include "VideoBackends/Vulkan/VKBoundingBox.h"
#endif

namespace Libretro
{
extern std::vector<std::string> g_normalized_game_paths;
namespace Video
{
#ifdef _WIN32
static Common::DynamicLibrary d3d11_library;
#endif

void Init()
{
  DEBUG_LOG_FMT(VIDEO, "Video - Init");

  std::string renderer = Libretro::Options::GetCached<std::string>(
    Libretro::Options::gfx_settings::RENDERER);

  if (renderer == "Hardware")
  {
    retro_hw_context_type preferred = RETRO_HW_CONTEXT_NONE;
    if (environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred) && SetHWRender(preferred))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGL_CORE))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGL))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGLES_VERSION, 3, 2))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGLES_VERSION, 3, 1))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGLES3))
      return;
#ifdef _WIN32
    if (SetHWRender(RETRO_HW_CONTEXT_D3D11))
      return;
#endif
#ifdef HAS_VULKAN
    if (SetHWRender(RETRO_HW_CONTEXT_VULKAN))
      return;
#endif
  }
  hw_render.context_type = RETRO_HW_CONTEXT_NONE;
  if (renderer == "Software")
    Config::SetBase(Config::MAIN_GFX_BACKEND, "Software Renderer");
  else
    Config::SetBase(Config::MAIN_GFX_BACKEND, "Null");
}

bool SetHWRender(retro_hw_context_type type, const int version_major, const int version_minor)
{
  DEBUG_LOG_FMT(VIDEO, "Video - SetHWRender!");

  hw_render.context_type = type;
  hw_render.context_reset = ContextReset;
  hw_render.context_destroy = ContextDestroy;
  hw_render.bottom_left_origin = true;
  bool success = false;

  switch (type)
  {
  case RETRO_HW_CONTEXT_OPENGL_CORE:
    // minimum requirements to run is opengl 3.3 (RA will try to use highest version available anyway)
    hw_render.version_major = (version_major != -1) ? version_major : 3;
    hw_render.version_minor = (version_minor != -1) ? version_minor : 3;
    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      Config::SetBase(Config::MAIN_GFX_BACKEND, "OGL");
      success = true;
    } else {
      WARN_LOG_FMT(VIDEO, "Video - SetHWRender - failed to set hw renderer for OpenGL Core");
    }
    break;
  case RETRO_HW_CONTEXT_OPENGLES_VERSION:
  case RETRO_HW_CONTEXT_OPENGLES3:
  case RETRO_HW_CONTEXT_OPENGL:
  {
    // when using RETRO_HW_CONTEXT_OPENGL you can't set version above 3.0 (RA will try to use highest version available anyway)
    // dolphin support OpenGL ES 3.0 too (no support for 2.0) so we are good
    hw_render.version_major = (version_major != -1) ? version_major : 3;
    hw_render.version_minor = (version_minor != -1) ? version_minor : 0;

    std::string api_name =
      (type == RETRO_HW_CONTEXT_OPENGLES3 || type == RETRO_HW_CONTEXT_OPENGLES_VERSION)
          ? "OpenGL ES " + std::to_string(hw_render.version_major) + "." +
                std::to_string(hw_render.version_minor)
          : "OpenGL " + std::to_string(hw_render.version_major) + "." +
                std::to_string(hw_render.version_minor);

    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      // Shared context is required with "gl" video driver
      if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr)) {
        WARN_LOG_FMT(VIDEO, "Video - SetHWRender - unable to set shared context for {}", api_name);
      }
      INFO_LOG_FMT(VIDEO, "Video - SetHWRender - using {}", api_name);
      Config::SetBase(Config::MAIN_GFX_BACKEND, "OGL");
      success = true;
    }
    else
    {
      WARN_LOG_FMT(VIDEO, "Video - SetHWRender - failed to set hw renderer for {}", api_name);
    }
    break;
  }
#ifdef _WIN32
  case RETRO_HW_CONTEXT_D3D11:
    hw_render.version_major = 11;
    hw_render.version_minor = 0;
    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      Config::SetBase(Config::MAIN_GFX_BACKEND, "D3D");
      success = true;
    }
    break;
#endif
#ifdef HAS_VULKAN
  case RETRO_HW_CONTEXT_VULKAN:
    hw_render.version_major = VK_API_VERSION_1_0;
    hw_render.version_minor = 0;
    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      static const struct retro_hw_render_context_negotiation_interface_vulkan iface = {
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
          Vk::GetApplicationInfo,
          Vk::CreateDevice,
          NULL, // destroy_device
#ifdef __APPLE__
          Vk::CreateInstance, // create_instance (v2 API)
          NULL, // create_device2
#endif
      };
      environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&iface);

      Config::SetBase(Config::MAIN_GFX_BACKEND, "Vulkan");
      success = true;
    }
    break;
#endif
  default:
    break;
  }

  return success;
}

void ContextReset(void)
{
  DEBUG_LOG_FMT(VIDEO, "Video - ContextReset!");

  g_context_status.MarkReset();

#ifdef HAS_VULKAN
  if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
  {
    retro_hw_render_interface* vulkan;
    if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&vulkan) ||
        !vulkan)
    {
      ERROR_LOG_FMT(VIDEO, "Failed to get HW rendering interface!");
      return;
    }
    if (vulkan->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
    {
      ERROR_LOG_FMT(VIDEO, "HW render interface mismatch, expected {}, got {}",
                RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION, vulkan->interface_version);
      return;
    }
    Vk::SetHWRenderInterface(vulkan);

    int efbScale = Libretro::Options::GetCached<int>(
      Libretro::Options::gfx_settings::EFB_SCALE, 1);
    Vk::SetSurfaceSize(EFB_WIDTH * efbScale,
                       EFB_HEIGHT * efbScale);
  }
#endif

#ifdef _WIN32
  if (hw_render.context_type == RETRO_HW_CONTEXT_D3D11)
  {
    WindowSystemInfo wsi(WindowSystemType::Libretro, nullptr, nullptr, nullptr);
    int efbScale = Libretro::Options::GetCached<int>(
      Libretro::Options::gfx_settings::EFB_SCALE, 1);
    wsi.render_surface_scale = efbScale;
    g_video_backend->PrepareWindow(wsi);

    retro_hw_render_interface_d3d11* d3d;
    if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&d3d) || !d3d)
    {
      ERROR_LOG_FMT(VIDEO, "Failed to get HW rendering interface!");
      return;
    }
    if (d3d->interface_version != RETRO_HW_RENDER_INTERFACE_D3D11_VERSION)
    {
      ERROR_LOG_FMT(VIDEO, "HW render interface mismatch, expected {}, got {}!",
                RETRO_HW_RENDER_INTERFACE_D3D11_VERSION, d3d->interface_version);
      return;
    }
    DX11::D3D::device = d3d->device;
    DX11::D3D::context = d3d->context;
    DX11::D3D::feature_level = d3d->featureLevel;
    D3DCommon::d3d_compile = d3d->D3DCompile;

    if (!d3d11_library.IsOpen() && !d3d11_library.Open("d3d11.dll"))
    {
      d3d11_library.Close();
      ERROR_LOG_FMT(VIDEO, "Failed to load D3D11 Libraries");
      return;
    }

    if (!D3DCommon::LoadLibraries())
    {
      ERROR_LOG_FMT(VIDEO, "Failed to load dxgi or d3dcompiler Libraries");
      return;
    }

    if (FAILED(DX11::D3D::device.As(&DX11::D3D::device1)))
    {
      WARN_LOG_FMT(VIDEO, "Missing Direct3D 11.1 support. Logical operations will not be supported.");
      g_backend_info.bSupportsLogicOp = false;
    }

    DX11::D3D::stateman = std::make_unique<DX11::D3D::StateManager>();

    DX11::VideoBackend* d3d11 = static_cast<DX11::VideoBackend*>(g_video_backend);

    d3d11->FillD3DBackendInfo();
    UpdateActiveConfig();

    std::unique_ptr<DX11SwapChain> swap_chain = std::make_unique<DX11SwapChain>(
      wsi, EFB_WIDTH * efbScale, EFB_HEIGHT * efbScale,
      nullptr, nullptr);

    auto gfx = std::make_unique<DX11::Gfx>(std::move(swap_chain), wsi.render_surface_scale);
    auto vertex_manager = std::make_unique<DX11::VertexManager>();
    auto perf_query = std::make_unique<DX11::PerfQuery>();
    auto bounding_box = std::make_unique<DX11::D3DBoundingBox>();

    if(d3d11->InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                               std::move(bounding_box)))
      g_context_status.MarkInitialized();

    return;
  }
#endif
  if(Video_InitializeBackend())
    g_context_status.MarkInitialized();
}

bool Video_InitializeBackend()
{
  WindowSystemInfo wsi = {};
  wsi.type = WindowSystemType::Libretro;
  wsi.render_surface_scale = 1.0f;

  g_video_backend->PrepareWindow(wsi);

#ifdef HAS_VULKAN
  if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
  {
    bool enable_surface = wsi.type != WindowSystemType::Headless;
    VkSurfaceKHR surface = Libretro::Video::Vk::GetSurface();

    // Since VulkanContext maintains a copy of the device features and properties, we can use this
    // to initialize the backend information, so that we don't need to enumerate everything again.
    Vulkan::VulkanContext::PopulateBackendInfoFeatures(&g_backend_info, Vulkan::g_vulkan_context->GetPhysicalDevice(),
                                              Vulkan::g_vulkan_context->GetDeviceInfo());
    Vulkan::VulkanContext::PopulateBackendInfoMultisampleModes(
        &g_backend_info, Vulkan::g_vulkan_context->GetPhysicalDevice(), Vulkan::g_vulkan_context->GetDeviceInfo());
    g_backend_info.bSupportsExclusiveFullscreen =
        enable_surface && Vulkan::g_vulkan_context->SupportsExclusiveFullscreen(wsi, surface);

    UpdateActiveConfig();

    // Remaining classes are also dependent on object cache.
    Vulkan::g_object_cache = std::make_unique<Vulkan::ObjectCache>();
    if (!Vulkan::g_object_cache->Initialize())
    {
      PanicAlertFmt("Failed to initialize Vulkan object cache.");
      return false;
    }

    // Create swap chain. This has to be done early so that the target size is correct for auto-scale.
    std::unique_ptr<Vulkan::SwapChain> swap_chain;
    if (surface != VK_NULL_HANDLE)
    {
      swap_chain = Vulkan::SwapChain::Create(wsi, surface, g_ActiveConfig.bVSyncActive);
      if (!swap_chain)
      {
        PanicAlertFmt("Failed to create Vulkan swap chain.");
        return false;
      }
    }

    // Create command buffers. We do this separately because the other classes depend on it.
    Vulkan::g_command_buffer_mgr = std::make_unique<Vulkan::CommandBufferManager>(g_Config.bBackendMultithreading);
    size_t swapchain_image_count =
        surface != VK_NULL_HANDLE ? swap_chain->GetSwapChainImageCount() : 0;
    if (!Vulkan::g_command_buffer_mgr->Initialize(swapchain_image_count))
    {
      PanicAlertFmt("Failed to create Vulkan command buffers");
      return false;
    }

    if (!Vulkan::StateTracker::CreateInstance())
    {
      PanicAlertFmt("Failed to create state tracker");
      return false;
    }

    auto gfx = std::make_unique<Vulkan::VKGfx>(std::move(swap_chain), wsi.render_surface_scale);
    auto vertex_manager = std::make_unique<Vulkan::VertexManager>();
    auto perf_query = std::make_unique<Vulkan::PerfQuery>();
    auto bounding_box = std::make_unique<Vulkan::VKBoundingBox>();

    return g_video_backend->InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                                             std::move(bounding_box));
  }
#endif

  if(!g_video_backend) {
    WARN_LOG_FMT(VIDEO, "Video - g_video_backend - No supported renderer found");
    return false;
  }

  // this calls InitializeGLExtensions, FillBackendInfo and InitializeShared internally
  return g_video_backend->Initialize(wsi);
}

void ContextDestroy(void)
{
  DEBUG_LOG_FMT(VIDEO, "Video - ContextDestroy!");

  g_context_status.MarkDestroyed();

  if (g_gfx &&
      Config::Get(Config::MAIN_GFX_BACKEND) == "OGL")
  {
    static_cast<OGL::OGLGfx*>(g_gfx.get())->SetSystemFrameBuffer(0);
  }

  if (g_video_backend)
  {
    g_video_backend->Shutdown();
  }

  switch (hw_render.context_type)
  {
  case RETRO_HW_CONTEXT_D3D11:
#ifdef _WIN32
    if (DX11::D3D::context)
    {
      DX11::D3D::context->ClearState();
      DX11::D3D::context->Flush();
    }

    DX11::D3D::context.Reset();
    DX11::D3D::device1.Reset();

    DX11::D3D::device = nullptr;
    DX11::D3D::device1 = nullptr;
    DX11::D3D::context = nullptr;
    D3DCommon::d3d_compile = nullptr;
    DX11::D3D::stateman.reset();
    D3DCommon::UnloadLibraries();
    d3d11_library.Close();
#endif
    break;
  case RETRO_HW_CONTEXT_VULKAN:
#ifdef HAS_VULKAN
    Vk::SetHWRenderInterface(nullptr);
#endif
    break;
  case RETRO_HW_CONTEXT_OPENGL:
  case RETRO_HW_CONTEXT_OPENGL_CORE:
  case RETRO_HW_CONTEXT_OPENGLES3:
    break;
  default:
  case RETRO_HW_CONTEXT_NONE:
    break;
  }
}
#ifdef HAS_VULKAN
namespace Vk
{
const VkApplicationInfo* GetApplicationInfo(void)
{
  static VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "Dolphin-Emu";
  app_info.applicationVersion = 5;  // TODO: extract from Common::scm_desc_str
  app_info.pEngineName = "Dolphin-Emu";
  app_info.engineVersion = 2;
  app_info.apiVersion = VK_API_VERSION_1_0;
  return &app_info;
}

#ifdef __APPLE__
VkInstance CreateInstance(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                          const VkApplicationInfo* app,
                          retro_vulkan_create_instance_wrapper_t create_instance_wrapper,
                          void* opaque)
{
  VkInstanceCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = app;

  std::vector<const char*> extensions;
  extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);

  create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  create_info.ppEnabledExtensionNames = extensions.data();


  static VkBool32 disable_arg_buffers = VK_FALSE;

  VkLayerSettingEXT layer_setting = {};
  layer_setting.pLayerName = "MoltenVK";
  layer_setting.pSettingName = "MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS";
  layer_setting.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
  layer_setting.valueCount = 1;
  layer_setting.pValues = &disable_arg_buffers;

  VkLayerSettingsCreateInfoEXT layer_settings = {};
  layer_settings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
  layer_settings.settingCount = 1;
  layer_settings.pSettings = &layer_setting;

  create_info.pNext = &layer_settings;

  return create_instance_wrapper(opaque, &create_info);
}
#endif

bool CreateDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                         VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         const char** required_device_extensions,
                         unsigned num_required_device_extensions,
                         const char** required_device_layers, unsigned num_required_device_layers,
                         const VkPhysicalDeviceFeatures* required_features)
{
  assert(g_video_backend->GetName() == "Vulkan");

  Vulkan::LoadVulkanLibrary();

  Init(instance, gpu, surface, get_instance_proc_addr, required_device_extensions,
       num_required_device_extensions, required_device_layers, num_required_device_layers,
       required_features);

  if (!Vulkan::LoadVulkanInstanceFunctions(instance))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load Vulkan instance functions.");
    Vulkan::UnloadVulkanLibrary();
    return false;
  }

  Vulkan::VulkanContext::GPUList gpu_list = Vulkan::VulkanContext::EnumerateGPUs(instance);
  if (gpu_list.empty())
  {
    ERROR_LOG_FMT(VIDEO, "No Vulkan physical devices available.");
    Vulkan::UnloadVulkanLibrary();
    return false;
  }

  Vulkan::VulkanContext::PopulateBackendInfo(&g_backend_info);
  Vulkan::VulkanContext::PopulateBackendInfoAdapters(&g_backend_info, gpu_list);

  if (gpu == VK_NULL_HANDLE)
    gpu = gpu_list[0];
  Vulkan::g_vulkan_context = Vulkan::VulkanContext::Create(instance, gpu, surface, false, false, VK_API_VERSION_1_0);
  if (!Vulkan::g_vulkan_context)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create Vulkan device");
    Vulkan::UnloadVulkanLibrary();
    return false;
  }

  context->gpu = Vulkan::g_vulkan_context->GetPhysicalDevice();
  context->device = Vulkan::g_vulkan_context->GetDevice();
  context->queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
  context->queue_family_index = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();
  context->presentation_queue = context->queue;
  context->presentation_queue_family_index = context->queue_family_index;

  return true;
}
}  // namespace Vk
#endif

}  // namespace Video
}  // namespace Libretro

void retro_set_video_refresh(retro_video_refresh_t cb)
{
  Libretro::Video::video_cb = cb;
}
