#include <switch.h>
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "dfx_fxaa_frag_bin.h"
#include "dfx_fxaa_hq_frag_bin.h"
#include "dfx_fxaa_hq_vert_bin.h"
#include "dfx_fxaa_luma_frag_bin.h"
#include "dfx_fxaa_luma_vert_bin.h"
#include "dfx_fxaa_vert_bin.h"
#include "dfx_hq2x_frag_bin.h"
#include "dfx_hq2x_vert_bin.h"
#include "dfx_scale2x_frag_bin.h"
#include "dfx_scale2x_vert_bin.h"
#include "dfx_smaa_blend_frag_bin.h"
#include "dfx_smaa_blend_vert_bin.h"
#include "dfx_smaa_edge_frag_bin.h"
#include "dfx_smaa_edge_vert_bin.h"
#include "dfx_smaa_weight_frag_bin.h"
#include "dfx_smaa_weight_vert_bin.h"
#include "drastic_dfx.h"
#include "drastic_custom_shader.h"

#define drastic_renderer_init drastic_vk_renderer_init
#define drastic_renderer_present drastic_vk_renderer_present
#define drastic_renderer_suspend drastic_vk_renderer_suspend
#define drastic_renderer_resume drastic_vk_renderer_resume
#define drastic_renderer_shutdown drastic_vk_renderer_shutdown
#define drastic_renderer_frame_count drastic_vk_renderer_frame_count
#define drastic_renderer_lsfg_available drastic_vk_renderer_lsfg_available
#define drastic_renderer_lsfg_enabled drastic_vk_renderer_lsfg_enabled
#define drastic_renderer_lsfg_request_enabled \
  drastic_vk_renderer_lsfg_request_enabled
#define drastic_renderer_set_custom_shader \
  drastic_vk_renderer_set_custom_shader
#define drastic_renderer_last_error drastic_vk_renderer_last_error
#include "drastic_renderer.h"
#include "drastic_rotation.h"
#include "drastic_vk_capture.h"
#include "drastic_smaa_area_rgb_bin.h"
#include "drastic_smaa_search_rgb_bin.h"
#include "drastic_vk_frag_bin.h"
#include "drastic_vk_vert_bin.h"
#include "lsfg_bridge.h"
#include "prefs.h"

#define MAX_SWAP_IMAGES 8
#define RENDER_FRAME_SLOTS 3
#define TEXTURE_COUNT 12
#define TEXTURE_TOP 0
#define TEXTURE_BOTTOM 1
#define TEXTURE_OVERLAY 2
#define TEXTURE_OVERLAY_PORTRAIT 3
#define TEXTURE_AREA 4
#define TEXTURE_SEARCH 5
#define TEXTURE_TOP_A 6
#define TEXTURE_TOP_B 7
#define TEXTURE_TOP_2X 8
#define TEXTURE_BOTTOM_A 9
#define TEXTURE_BOTTOM_B 10
#define TEXTURE_BOTTOM_2X 11
#define STANDARD_DESCRIPTOR_COUNT (TEXTURE_COUNT * 2)
#define CUSTOM_DESCRIPTOR_COUNT 4
#define DESCRIPTOR_SET_COUNT \
  (STANDARD_DESCRIPTOR_COUNT + CUSTOM_DESCRIPTOR_COUNT)
#define MAX_DRAWS 8
#define MAX_VERTICES (MAX_DRAWS * 6)

typedef struct {
  float x, y;
  float u, v;
} Vertex;

typedef struct {
  int32_t effect;
  int32_t mode; /* 0=alpha texture, 1=solid, 2=opaque texture */
  float texture_width;
  float texture_height;
  float target_width;
  float target_height;
  float padding[2];
  float color[4];
} DrawParameters;

_Static_assert(sizeof(DrawParameters) == 48, "Vulkan push constant layout");

typedef struct {
  float texture_size[4];
  float target_size[2];
  float time;
  float padding;
} DfxParameters;

_Static_assert(sizeof(DfxParameters) == 32, "Drastic DFX push constants");

typedef struct {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
  VkFramebuffer framebuffer;
  VkDescriptorSet descriptors[2];
  VkFormat format;
  uint32_t width;
  uint32_t height;
  int initialized;
} SampledTexture;

typedef struct {
  uint32_t first_vertex;
  uint32_t vertex_count;
  int texture;
  int sampler;
  DrawParameters parameters;
} DrawBatch;

typedef struct {
  DrasticCustomShader shader;
  SampledTexture textures[2][DRASTIC_CUSTOM_SHADER_MAX_TEXTURES];
  VkSampler samplers[DRASTIC_CUSTOM_SHADER_MAX_TEXTURES];
  VkDescriptorSetLayout descriptor_layout;
  VkDescriptorPool descriptor_pool;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipelines[DRASTIC_CUSTOM_SHADER_MAX_PASSES];
  VkDescriptorSet descriptors[2][DRASTIC_CUSTOM_SHADER_MAX_PASSES];
  int valid;
} VkCustomState;

typedef struct {
  uint32_t first_vertex;
  int screen;
  float target_width;
  float target_height;
} CustomFinalDraw;

typedef struct {
  VkCommandBuffer command;
  VkSemaphore acquired;
  VkSemaphore rendered;
  VkFence fence;
  int pending;
} RenderFrameSlot;

static VkInstance g_instance;
static VkPhysicalDevice g_physical;
static VkDevice g_device;
static VkSurfaceKHR g_surface;
static VkSwapchainKHR g_swapchain;
static VkQueue g_queue;
static uint32_t g_queue_family;
static VkFormat g_format;
static VkColorSpaceKHR g_color_space;
static VkExtent2D g_extent;
static VkImage g_swap_images[MAX_SWAP_IMAGES];
static VkImageView g_swap_views[MAX_SWAP_IMAGES];
static VkFramebuffer g_framebuffers[MAX_SWAP_IMAGES];
static uint32_t g_image_count;
static RenderFrameSlot g_frame_slots[RENDER_FRAME_SLOTS];
static uint32_t g_frame_slot_count = 1;
static uint32_t g_frame_slot_cursor;
static VkCommandPool g_command_pool;
static VkRenderPass g_render_pass;
static VkRenderPass g_filter_render_pass;
static VkDescriptorSetLayout g_descriptor_layout;
static VkDescriptorPool g_descriptor_pool;
static VkPipelineLayout g_pipeline_layout;
static VkPipeline g_pipeline;
static VkSampler g_samplers[2];

static SampledTexture g_textures[TEXTURE_COUNT];
static VkBuffer g_staging_buffer;
static VkDeviceMemory g_staging_memory;
static uint8_t *g_staging_mapped;
static VkDeviceSize g_staging_size;
static VkDeviceSize g_staging_stride;
static VkDeviceSize g_staging_base;
static VkDeviceSize g_texture_offsets[TEXTURE_COUNT];
static VkDeviceSize g_vertex_offset;
static VkDeviceSize g_filter_vertex_offset;
static int g_staging_coherent;

static DrawBatch g_draws[MAX_DRAWS];
static uint32_t g_draw_count;
static uint32_t g_vertex_count;
static uint64_t g_overlay_generation = UINT64_MAX;
static unsigned g_frames;
static uint32_t g_ds_width = 256;
static uint32_t g_ds_height = 192;
static uint32_t *g_core_pixels;
static VkPipeline g_filter_pipelines[DRASTIC_DFX_SHADER_COUNT];
static VkDescriptorSet g_filter_descriptors[2][2];
static DrasticVideoFilter g_filtered_filter = DRASTIC_FILTER_COUNT;
static int g_filter_valid[2];
static VkCustomState g_custom;
static CustomFinalDraw g_custom_draws[3];
static uint32_t g_custom_draw_count;
static char g_renderer_error[512];

static LsfgNxRuntime *g_lsfg_runtime;
static int g_lsfg_init_attempted;
static int g_lsfg_device_capable;
static int g_lsfg_swapchain_compatible;
static int g_lsfg_pipeline_prepared;
static int g_lsfg_enabled_requested;
static int g_lsfg_runtime_available;
static int g_low_latency;

static int vk_ok(VkResult result) { return result >= 0; }

static void set_renderer_error(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(g_renderer_error, sizeof(g_renderer_error), format, arguments);
  va_end(arguments);
  g_renderer_error[sizeof(g_renderer_error) - 1] = '\0';
}

static int lsfg_requested(void) {
  return __atomic_load_n(&g_lsfg_enabled_requested, __ATOMIC_ACQUIRE);
}

static const char *lsfg_dll_path(void) {
  return DATA_ROOT "/lsfg/Lossless.dll";
}

static int file_readable(const char *path) {
  if (!path || !path[0]) return 0;
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  fclose(file);
  return 1;
}

static int texture_for_role(int screen, DrasticDfxTextureRole role) {
  switch (role) {
    case DRASTIC_DFX_WORK_A:
      return screen ? TEXTURE_BOTTOM_A : TEXTURE_TOP_A;
    case DRASTIC_DFX_WORK_B:
      return screen ? TEXTURE_BOTTOM_B : TEXTURE_TOP_B;
    case DRASTIC_DFX_WORK_2X:
      return screen ? TEXTURE_BOTTOM_2X : TEXTURE_TOP_2X;
    case DRASTIC_DFX_AREA: return TEXTURE_AREA;
    case DRASTIC_DFX_SEARCH: return TEXTURE_SEARCH;
    default: return screen ? TEXTURE_BOTTOM : TEXTURE_TOP;
  }
}

static int final_effect(const DrasticDfxChain *chain) {
  if (chain->final_shader == DRASTIC_DFX_QUILEZ) return 2;
  if (chain->final_shader == DRASTIC_DFX_SCANLINE) return 3;
  return chain->final_sampler == DRASTIC_DFX_LINEAR ? 1 : 0;
}

/* LSFG's extracted compute shaders require NVK's mapped descriptor/UBO
 * constant-buffer path to be disabled. NVK snapshots this before instance
 * creation, so prepare it only for launches that explicitly enable LSFG. */
static int enable_nvk_no_cbuf(void) {
  const char *current = getenv("NVK_DEBUG");
  if (current && strstr(current, "no_cbuf")) return 1;
  if (!current || !current[0])
    return setenv("NVK_DEBUG", "no_cbuf", 1) == 0;

  const size_t length = strlen(current);
  char *combined = malloc(length + sizeof(",no_cbuf"));
  if (!combined) return 0;
  snprintf(combined, length + sizeof(",no_cbuf"), "%s,no_cbuf", current);
  const int result = setenv("NVK_DEBUG", combined, 1) == 0;
  free(combined);
  return result;
}

static int has_device_extension(const char *wanted) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(g_physical, NULL, &count, NULL) !=
      VK_SUCCESS)
    return 0;
  VkExtensionProperties *properties =
      count ? malloc(sizeof(*properties) * count) : NULL;
  if (count && !properties) return 0;
  const VkResult result = vkEnumerateDeviceExtensionProperties(
      g_physical, NULL, &count, properties);
  int found = 0;
  if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
    for (uint32_t index = 0; index < count; index++) {
      if (!strcmp(properties[index].extensionName, wanted)) {
        found = 1;
        break;
      }
    }
  }
  free(properties);
  return found;
}

static void lsfg_destroy_runtime(void) {
  if (!g_lsfg_runtime) return;
  lsfg_nx_destroy(g_lsfg_runtime);
  g_lsfg_runtime = NULL;
}

static VkDeviceSize align_device_size(VkDeviceSize value,
                                      VkDeviceSize alignment) {
  if (alignment < 1) alignment = 1;
  return (value + alignment - 1) & ~(alignment - 1);
}

static uint32_t find_memory_type(uint32_t mask, VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred,
                                 int *coherent) {
  VkPhysicalDeviceMemoryProperties properties;
  vkGetPhysicalDeviceMemoryProperties(g_physical, &properties);
  uint32_t fallback = UINT32_MAX;
  for (uint32_t index = 0; index < properties.memoryTypeCount; index++) {
    if (!(mask & (1u << index))) continue;
    const VkMemoryPropertyFlags flags =
        properties.memoryTypes[index].propertyFlags;
    if ((flags & required) != required) continue;
    if (fallback == UINT32_MAX) fallback = index;
    if ((flags & preferred) == preferred) {
      if (coherent)
        *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      return index;
    }
  }
  if (fallback != UINT32_MAX && coherent) {
    const VkMemoryPropertyFlags flags =
        properties.memoryTypes[fallback].propertyFlags;
    *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  }
  return fallback;
}

static int choose_physical_device(void) {
  uint32_t device_count = 0;
  if (!vk_ok(vkEnumeratePhysicalDevices(g_instance, &device_count, NULL)) ||
      !device_count)
    return 0;
  VkPhysicalDevice devices[8];
  if (device_count > 8) device_count = 8;
  if (!vk_ok(vkEnumeratePhysicalDevices(g_instance, &device_count, devices)))
    return 0;
  for (uint32_t device_index = 0; device_index < device_count; device_index++) {
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[device_index],
                                             &family_count, NULL);
    VkQueueFamilyProperties families[16];
    if (family_count > 16) family_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[device_index],
                                             &family_count, families);
    for (uint32_t family = 0; family < family_count; family++) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(devices[device_index], family,
                                           g_surface, &present);
      if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
        g_physical = devices[device_index];
        g_queue_family = family;
        return 1;
      }
    }
  }
  return 0;
}

static int create_device(void) {
  const float priority = 1.0f;
  const VkDeviceQueueCreateInfo queue_info = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = g_queue_family,
    .queueCount = 1,
    .pQueuePriorities = &priority,
  };

  const char *extensions[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  uint32_t extension_count = 1;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
    .timelineSemaphore = VK_TRUE,
  };
  if (g_lsfg_pipeline_prepared) {
    /* The renderer requests Vulkan 1.1, matching NetherSX2's compatibility
     * path, so timeline semaphores are enabled through their KHR extension. */
    if (!has_device_extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
      return 0;
    extensions[extension_count++] =
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
  }
  const VkDeviceCreateInfo create_info = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext = g_lsfg_pipeline_prepared ? &timeline_feature : NULL,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &queue_info,
    .enabledExtensionCount = extension_count,
    .ppEnabledExtensionNames = extensions,
  };
  if (!vk_ok(vkCreateDevice(g_physical, &create_info, NULL, &g_device)))
    return 0;
  vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);
  if (!g_queue) return 0;
  g_lsfg_device_capable = g_lsfg_pipeline_prepared;
  return 1;
}

static int choose_surface_format(void) {
  uint32_t count = 0;
  if (!vk_ok(vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface,
                                                   &count, NULL)) || !count)
    return 0;
  VkSurfaceFormatKHR formats[16];
  if (count > 16) count = 16;
  if (!vk_ok(vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface,
                                                   &count, formats)))
    return 0;
  g_format = VK_FORMAT_UNDEFINED;
  g_color_space = formats[0].colorSpace;
  VkFormat rgba_fallback = VK_FORMAT_UNDEFINED;
  for (uint32_t index = 0; index < count; index++) {
    if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM ||
        formats[index].format == VK_FORMAT_B8G8R8A8_SRGB) {
      g_format = formats[index].format;
      g_color_space = formats[index].colorSpace;
      break;
    }
    if (formats[index].format == VK_FORMAT_R8G8B8A8_UNORM ||
        formats[index].format == VK_FORMAT_R8G8B8A8_SRGB) {
      rgba_fallback = formats[index].format;
      g_color_space = formats[index].colorSpace;
    }
  }
  if (g_format == VK_FORMAT_UNDEFINED) g_format = rgba_fallback;
  if (g_format == VK_FORMAT_UNDEFINED && count == 1 &&
      formats[0].format == VK_FORMAT_UNDEFINED)
    g_format = VK_FORMAT_B8G8R8A8_UNORM;
  return g_format != VK_FORMAT_UNDEFINED;
}

static int create_swapchain(void) {
  VkSurfaceCapabilitiesKHR capabilities;
  if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
          g_physical, g_surface, &capabilities)))
    return 0;
  if (!(capabilities.supportedUsageFlags &
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    return 0;
  g_extent = capabilities.currentExtent;
  if (g_extent.width == UINT32_MAX) {
    g_extent.width = (uint32_t)panel_width;
    g_extent.height = (uint32_t)panel_height;
  }
  panel_width = screen_width = (int)g_extent.width;
  panel_height = screen_height = (int)g_extent.height;
  /* The default path keeps one image of CPU/GPU overlap. Low-latency mode
   * deliberately requests the surface minimum so FIFO cannot accumulate an
   * additional completed game frame ahead of the display. */
  uint32_t requested = capabilities.minImageCount + (g_low_latency ? 0u : 1u);
  VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  g_lsfg_swapchain_compatible = 0;
  __atomic_store_n(&g_lsfg_runtime_available, 0, __ATOMIC_RELEASE);
  if (g_lsfg_pipeline_prepared) {
    const VkImageUsageFlags transfer_usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags transfer_features =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkFormatProperties swapchain_properties;
    VkFormatProperties rgba_properties;
    vkGetPhysicalDeviceFormatProperties(g_physical, g_format,
                                        &swapchain_properties);
    vkGetPhysicalDeviceFormatProperties(g_physical,
                                        VK_FORMAT_R8G8B8A8_UNORM,
                                        &rgba_properties);
    if (!g_lsfg_device_capable ||
        (capabilities.supportedUsageFlags & transfer_usage) !=
            transfer_usage ||
        (swapchain_properties.optimalTilingFeatures & transfer_features) !=
            transfer_features ||
        (rgba_properties.optimalTilingFeatures & transfer_features) !=
            transfer_features)
      return 0;

    image_usage |= transfer_usage;
    /* Preserve NetherSX2's no-pacing headroom request. The surface maximum
     * still wins, and LSFG validates that at least three images were exposed. */
    requested += 2;
    g_lsfg_swapchain_compatible = 1;
  }
  if (capabilities.maxImageCount && requested > capabilities.maxImageCount)
    requested = capabilities.maxImageCount;
  const VkSwapchainCreateInfoKHR create_info = {
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    .surface = g_surface,
    .minImageCount = requested,
    .imageFormat = g_format,
    .imageColorSpace = g_color_space,
    .imageExtent = g_extent,
    .imageArrayLayers = 1,
    .imageUsage = image_usage,
    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = 0,
    .pQueueFamilyIndices = NULL,
    .preTransform = capabilities.currentTransform,
    .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    .presentMode = VK_PRESENT_MODE_FIFO_KHR,
    .clipped = VK_TRUE,
  };
  if (!vk_ok(vkCreateSwapchainKHR(g_device, &create_info, NULL,
                                   &g_swapchain)))
    return 0;
  uint32_t count = 0;
  if (!vk_ok(vkGetSwapchainImagesKHR(g_device, g_swapchain, &count, NULL)) ||
      !count || count > MAX_SWAP_IMAGES)
    return 0;
  g_image_count = count;
  if (!vk_ok(vkGetSwapchainImagesKHR(g_device, g_swapchain, &g_image_count,
                                     g_swap_images)))
    return 0;
  if (g_lsfg_pipeline_prepared && g_image_count < 3) return 0;
  if (g_lsfg_swapchain_compatible)
    __atomic_store_n(&g_lsfg_runtime_available, 1, __ATOMIC_RELEASE);
  return 1;
}

static int create_command_resources(void) {
  const VkCommandPoolCreateInfo pool_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = g_queue_family,
  };
  if (!vk_ok(vkCreateCommandPool(g_device, &pool_info, NULL,
                                  &g_command_pool)))
    return 0;
  const VkCommandBufferAllocateInfo command_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = g_command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = g_frame_slot_count,
  };
  VkCommandBuffer commands[RENDER_FRAME_SLOTS] = {0};
  if (!vk_ok(vkAllocateCommandBuffers(g_device, &command_info, commands)))
    return 0;
  for (uint32_t index = 0; index < g_frame_slot_count; index++)
    g_frame_slots[index].command = commands[index];
  return 1;
}

static int create_render_targets(void) {
  const VkAttachmentDescription attachment = {
    .format = g_format,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
  };
  const VkAttachmentReference color_reference = {
    .attachment = 0,
    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };
  const VkSubpassDescription subpass = {
    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .colorAttachmentCount = 1,
    .pColorAttachments = &color_reference,
  };
  const VkSubpassDependency dependency = {
    .srcSubpass = VK_SUBPASS_EXTERNAL,
    .dstSubpass = 0,
    .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
  };
  const VkRenderPassCreateInfo render_pass_info = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &attachment,
    .subpassCount = 1,
    .pSubpasses = &subpass,
    .dependencyCount = 1,
    .pDependencies = &dependency,
  };
  if (!vk_ok(vkCreateRenderPass(g_device, &render_pass_info, NULL,
                                 &g_render_pass)))
    return 0;
  const VkAttachmentDescription filter_attachment = {
    .format = VK_FORMAT_B8G8R8A8_UNORM,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  const VkSubpassDependency filter_dependencies[2] = {
    {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    },
    {
      .srcSubpass = 0,
      .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    },
  };
  const VkRenderPassCreateInfo filter_render_pass_info = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &filter_attachment,
    .subpassCount = 1,
    .pSubpasses = &subpass,
    .dependencyCount = 2,
    .pDependencies = filter_dependencies,
  };
  if (!vk_ok(vkCreateRenderPass(g_device, &filter_render_pass_info, NULL,
                                 &g_filter_render_pass)))
    return 0;
  for (uint32_t index = 0; index < g_image_count; index++) {
    const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = g_swap_images[index],
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = g_format,
      .components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      },
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (!vk_ok(vkCreateImageView(g_device, &view_info, NULL,
                                  &g_swap_views[index])))
      return 0;
    const VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = g_render_pass,
      .attachmentCount = 1,
      .pAttachments = &g_swap_views[index],
      .width = g_extent.width,
      .height = g_extent.height,
      .layers = 1,
    };
    if (!vk_ok(vkCreateFramebuffer(g_device, &framebuffer_info, NULL,
                                    &g_framebuffers[index])))
      return 0;
  }
  return 1;
}

static int create_sampled_texture(SampledTexture *texture, uint32_t width,
                                  uint32_t height, VkFormat format,
                                  VkImageUsageFlags usage) {
  texture->width = width;
  texture->height = height;
  texture->format = format;
  const VkImageCreateInfo image_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = format,
    .extent = {width, height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (!vk_ok(vkCreateImage(g_device, &image_info, NULL, &texture->image)))
    return 0;
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(g_device, texture->image, &requirements);
  const uint32_t memory_type = find_memory_type(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
  if (memory_type == UINT32_MAX) return 0;
  const VkMemoryAllocateInfo allocation = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = requirements.size,
    .memoryTypeIndex = memory_type,
  };
  if (!vk_ok(vkAllocateMemory(g_device, &allocation, NULL,
                               &texture->memory)) ||
      !vk_ok(vkBindImageMemory(g_device, texture->image, texture->memory, 0)))
    return 0;
  const VkImageViewCreateInfo view_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .image = texture->image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = format,
    .components = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
    },
    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  return vk_ok(vkCreateImageView(g_device, &view_info, NULL, &texture->view));
}

static void update_descriptor(VkDescriptorSet set, const int textures[3],
                              const int samplers[3]) {
  VkDescriptorImageInfo images[3];
  VkWriteDescriptorSet writes[3];
  for (int binding = 0; binding < 3; binding++) {
    images[binding] = (VkDescriptorImageInfo){
      .sampler = g_samplers[samplers[binding]],
      .imageView = g_textures[textures[binding]].view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    writes[binding] = (VkWriteDescriptorSet){
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = (uint32_t)binding,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &images[binding],
    };
  }
  vkUpdateDescriptorSets(g_device, 3, writes, 0, NULL);
}

static int create_texture_resources(void) {
  VkFormatProperties bgra_properties;
  VkFormatProperties rgba_properties;
  vkGetPhysicalDeviceFormatProperties(g_physical, VK_FORMAT_B8G8R8A8_UNORM,
                                      &bgra_properties);
  vkGetPhysicalDeviceFormatProperties(g_physical, VK_FORMAT_R8G8B8A8_UNORM,
                                      &rgba_properties);
  const VkFormatFeatureFlags sampled =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((bgra_properties.optimalTilingFeatures &
       (sampled | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) !=
      (sampled | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
      (rgba_properties.optimalTilingFeatures & sampled) != sampled)
    return 0;

  const VkImageUsageFlags uploaded =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  const VkImageUsageFlags filtered =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!create_sampled_texture(&g_textures[TEXTURE_TOP], g_ds_width, g_ds_height,
                              VK_FORMAT_B8G8R8A8_UNORM, uploaded) ||
      !create_sampled_texture(&g_textures[TEXTURE_BOTTOM], g_ds_width,
                              g_ds_height,
                              VK_FORMAT_B8G8R8A8_UNORM, uploaded) ||
      !create_sampled_texture(&g_textures[TEXTURE_OVERLAY],
                              DRASTIC_OVERLAY_WIDTH, DRASTIC_OVERLAY_HEIGHT,
                              VK_FORMAT_B8G8R8A8_UNORM, uploaded) ||
      !create_sampled_texture(&g_textures[TEXTURE_OVERLAY_PORTRAIT],
                              DRASTIC_OVERLAY_HEIGHT, DRASTIC_OVERLAY_WIDTH,
                              VK_FORMAT_B8G8R8A8_UNORM, uploaded) ||
      !create_sampled_texture(&g_textures[TEXTURE_AREA], 160, 560,
                              VK_FORMAT_R8G8B8A8_UNORM, uploaded) ||
      !create_sampled_texture(&g_textures[TEXTURE_SEARCH], 64, 16,
                              VK_FORMAT_R8G8B8A8_UNORM, uploaded) ||
      !create_sampled_texture(&g_textures[TEXTURE_TOP_A], g_ds_width,
                              g_ds_height,
                              VK_FORMAT_B8G8R8A8_UNORM, filtered) ||
      !create_sampled_texture(&g_textures[TEXTURE_TOP_B], g_ds_width,
                              g_ds_height,
                              VK_FORMAT_B8G8R8A8_UNORM, filtered) ||
      !create_sampled_texture(&g_textures[TEXTURE_TOP_2X], g_ds_width * 2,
                              g_ds_height * 2, VK_FORMAT_B8G8R8A8_UNORM,
                              filtered) ||
      !create_sampled_texture(&g_textures[TEXTURE_BOTTOM_A], g_ds_width,
                              g_ds_height, VK_FORMAT_B8G8R8A8_UNORM,
                              filtered) ||
      !create_sampled_texture(&g_textures[TEXTURE_BOTTOM_B], g_ds_width,
                              g_ds_height, VK_FORMAT_B8G8R8A8_UNORM,
                              filtered) ||
      !create_sampled_texture(&g_textures[TEXTURE_BOTTOM_2X], g_ds_width * 2,
                              g_ds_height * 2, VK_FORMAT_B8G8R8A8_UNORM,
                              filtered))
    return 0;

  for (int index = TEXTURE_TOP_A; index < TEXTURE_COUNT; index++) {
    const VkImageView view = g_textures[index].view;
    const VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = g_filter_render_pass,
      .attachmentCount = 1,
      .pAttachments = &view,
      .width = g_textures[index].width,
      .height = g_textures[index].height,
      .layers = 1,
    };
    if (!vk_ok(vkCreateFramebuffer(g_device, &framebuffer_info, NULL,
                                    &g_textures[index].framebuffer)))
      return 0;
  }

  for (int sampler = 0; sampler < 2; sampler++) {
    const VkFilter filter = sampler == DRASTIC_DFX_LINEAR
        ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = filter,
      .minFilter = filter,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
    };
    if (!vk_ok(vkCreateSampler(g_device, &sampler_info, NULL,
                                &g_samplers[sampler]))) return 0;
  }

  VkDescriptorSetLayoutBinding bindings[3];
  for (int binding = 0; binding < 3; binding++) {
    bindings[binding] = (VkDescriptorSetLayoutBinding){
      .binding = (uint32_t)binding,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
  }
  const VkDescriptorSetLayoutCreateInfo layout_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 3,
    .pBindings = bindings,
  };
  if (!vk_ok(vkCreateDescriptorSetLayout(g_device, &layout_info, NULL,
                                          &g_descriptor_layout))) return 0;
  const VkDescriptorPoolSize pool_size = {
    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .descriptorCount = DESCRIPTOR_SET_COUNT * 3,
  };
  const VkDescriptorPoolCreateInfo pool_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = DESCRIPTOR_SET_COUNT,
    .poolSizeCount = 1,
    .pPoolSizes = &pool_size,
  };
  if (!vk_ok(vkCreateDescriptorPool(g_device, &pool_info, NULL,
                                     &g_descriptor_pool))) return 0;
  VkDescriptorSetLayout layouts[DESCRIPTOR_SET_COUNT];
  VkDescriptorSet sets[DESCRIPTOR_SET_COUNT];
  for (int index = 0; index < DESCRIPTOR_SET_COUNT; index++)
    layouts[index] = g_descriptor_layout;
  const VkDescriptorSetAllocateInfo descriptor_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = g_descriptor_pool,
    .descriptorSetCount = DESCRIPTOR_SET_COUNT,
    .pSetLayouts = layouts,
  };
  if (!vk_ok(vkAllocateDescriptorSets(g_device, &descriptor_info, sets)))
    return 0;

  for (int texture = 0; texture < TEXTURE_COUNT; texture++) {
    for (int sampler = 0; sampler < 2; sampler++) {
      const int descriptor_index = texture * 2 + sampler;
      g_textures[texture].descriptors[sampler] = sets[descriptor_index];
      const int textures[3] = {texture, texture, texture};
      const int samplers[3] = {sampler, sampler, sampler};
      update_descriptor(sets[descriptor_index], textures, samplers);
    }
  }
  int next_set = STANDARD_DESCRIPTOR_COUNT;
  for (int screen = 0; screen < 2; screen++) {
    const int source = screen ? TEXTURE_BOTTOM : TEXTURE_TOP;
    const int work_a = screen ? TEXTURE_BOTTOM_A : TEXTURE_TOP_A;
    const int work_b = screen ? TEXTURE_BOTTOM_B : TEXTURE_TOP_B;
    g_filter_descriptors[screen][0] = sets[next_set++];
    const int weight_textures[3] = {
      work_a, TEXTURE_AREA, TEXTURE_SEARCH
    };
    const int linear_samplers[3] = {
      DRASTIC_DFX_LINEAR, DRASTIC_DFX_LINEAR, DRASTIC_DFX_LINEAR
    };
    update_descriptor(g_filter_descriptors[screen][0], weight_textures,
                      linear_samplers);
    g_filter_descriptors[screen][1] = sets[next_set++];
    const int blend_textures[3] = {source, work_b, source};
    update_descriptor(g_filter_descriptors[screen][1], blend_textures,
                      linear_samplers);
  }
  return 1;
}

static int create_staging_buffer(void) {
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(g_physical, &properties);
  const VkDeviceSize copy_alignment =
      properties.limits.optimalBufferCopyOffsetAlignment;
  const VkDeviceSize ds_size = (VkDeviceSize)g_ds_width * g_ds_height * 4;
  const VkDeviceSize overlay_size =
      (VkDeviceSize)DRASTIC_OVERLAY_WIDTH * DRASTIC_OVERLAY_HEIGHT * 4;
  const VkDeviceSize area_size = (VkDeviceSize)160 * 560 * 4;
  const VkDeviceSize search_size = (VkDeviceSize)64 * 16 * 4;
  g_texture_offsets[TEXTURE_TOP] = 0;
  g_texture_offsets[TEXTURE_BOTTOM] = align_device_size(ds_size,
                                                        copy_alignment);
  g_texture_offsets[TEXTURE_OVERLAY] = align_device_size(
      g_texture_offsets[TEXTURE_BOTTOM] + ds_size, copy_alignment);
  g_texture_offsets[TEXTURE_OVERLAY_PORTRAIT] =
      g_texture_offsets[TEXTURE_OVERLAY];
  g_texture_offsets[TEXTURE_AREA] = align_device_size(
      g_texture_offsets[TEXTURE_OVERLAY] + overlay_size, copy_alignment);
  g_texture_offsets[TEXTURE_SEARCH] = align_device_size(
      g_texture_offsets[TEXTURE_AREA] + area_size, copy_alignment);
  g_vertex_offset = align_device_size(
      g_texture_offsets[TEXTURE_SEARCH] + search_size, 16);
  g_filter_vertex_offset = align_device_size(
      g_vertex_offset + sizeof(Vertex) * MAX_VERTICES, 16);
  const VkDeviceSize frame_size =
      g_filter_vertex_offset + sizeof(Vertex) * 6;
  VkDeviceSize stride_alignment = copy_alignment;
  if (properties.limits.nonCoherentAtomSize > stride_alignment)
    stride_alignment = properties.limits.nonCoherentAtomSize;
  g_staging_stride = align_device_size(frame_size, stride_alignment);
  g_staging_size = g_staging_stride * g_frame_slot_count;
  const VkBufferCreateInfo buffer_info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = g_staging_size,
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (!vk_ok(vkCreateBuffer(g_device, &buffer_info, NULL,
                             &g_staging_buffer)))
    return 0;
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(g_device, g_staging_buffer, &requirements);
  const uint32_t memory_type = find_memory_type(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      &g_staging_coherent);
  if (memory_type == UINT32_MAX) return 0;
  const VkMemoryAllocateInfo allocation = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = requirements.size,
    .memoryTypeIndex = memory_type,
  };
  if (!vk_ok(vkAllocateMemory(g_device, &allocation, NULL,
                               &g_staging_memory)) ||
      !vk_ok(vkBindBufferMemory(g_device, g_staging_buffer,
                                g_staging_memory, 0)) ||
      !vk_ok(vkMapMemory(g_device, g_staging_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&g_staging_mapped)))
    return 0;
  const Vertex filter_vertices[6] = {
    {-1.0f, -1.0f, 0.0f, 0.0f}, {-1.0f,  1.0f, 0.0f, 1.0f},
    { 1.0f,  1.0f, 1.0f, 1.0f}, {-1.0f, -1.0f, 0.0f, 0.0f},
    { 1.0f,  1.0f, 1.0f, 1.0f}, { 1.0f, -1.0f, 1.0f, 0.0f},
  };
  for (uint32_t slot = 0; slot < g_frame_slot_count; slot++) {
    const VkDeviceSize base = (VkDeviceSize)slot * g_staging_stride;
    uint8_t *area = g_staging_mapped + base +
                    g_texture_offsets[TEXTURE_AREA];
    for (size_t pixel = 0; pixel < (size_t)160 * 560; pixel++) {
      area[pixel * 4 + 0] = drastic_smaa_area_rgb_bin[pixel * 3 + 0];
      area[pixel * 4 + 1] = drastic_smaa_area_rgb_bin[pixel * 3 + 1];
      area[pixel * 4 + 2] = drastic_smaa_area_rgb_bin[pixel * 3 + 2];
      area[pixel * 4 + 3] = 255;
    }
    uint8_t *search = g_staging_mapped + base +
                      g_texture_offsets[TEXTURE_SEARCH];
    for (size_t pixel = 0; pixel < (size_t)64 * 16; pixel++) {
      search[pixel * 4 + 0] = drastic_smaa_search_rgb_bin[pixel * 3 + 0];
      search[pixel * 4 + 1] = drastic_smaa_search_rgb_bin[pixel * 3 + 1];
      search[pixel * 4 + 2] = drastic_smaa_search_rgb_bin[pixel * 3 + 2];
      search[pixel * 4 + 3] = 255;
    }
    memcpy(g_staging_mapped + base + g_filter_vertex_offset,
           filter_vertices, sizeof(filter_vertices));
  }
  return 1;
}

static VkShaderModule create_shader_module(const uint8_t *code,
                                           uint32_t size) {
  if (!code || !size || (size & 3)) return VK_NULL_HANDLE;
  const VkShaderModuleCreateInfo create_info = {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = size,
    .pCode = (const uint32_t *)code,
  };
  VkShaderModule module = VK_NULL_HANDLE;
  if (!vk_ok(vkCreateShaderModule(g_device, &create_info, NULL, &module)))
    return VK_NULL_HANDLE;
  return module;
}

static int create_pipeline(void) {
  const VkPushConstantRange push_range = {
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    .offset = 0,
    .size = sizeof(DrawParameters),
  };
  const VkPipelineLayoutCreateInfo layout_info = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &g_descriptor_layout,
    .pushConstantRangeCount = 1,
    .pPushConstantRanges = &push_range,
  };
  if (!vk_ok(vkCreatePipelineLayout(g_device, &layout_info, NULL,
                                     &g_pipeline_layout)))
    return 0;
  const VkShaderModule vertex = create_shader_module(
      drastic_vk_vert_bin, drastic_vk_vert_bin_size);
  const VkShaderModule fragment = create_shader_module(
      drastic_vk_frag_bin, drastic_vk_frag_bin_size);
  if (!vertex || !fragment) {
    if (vertex) vkDestroyShaderModule(g_device, vertex, NULL);
    if (fragment) vkDestroyShaderModule(g_device, fragment, NULL);
    return 0;
  }
  const VkPipelineShaderStageCreateInfo stages[2] = {
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = vertex,
      .pName = "main",
    },
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = fragment,
      .pName = "main",
    },
  };
  const VkVertexInputBindingDescription vertex_binding = {
    .binding = 0,
    .stride = sizeof(Vertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
  };
  const VkVertexInputAttributeDescription attributes[2] = {
    {
      .location = 0,
      .binding = 0,
      .format = VK_FORMAT_R32G32_SFLOAT,
      .offset = offsetof(Vertex, x),
    },
    {
      .location = 1,
      .binding = 0,
      .format = VK_FORMAT_R32G32_SFLOAT,
      .offset = offsetof(Vertex, u),
    },
  };
  const VkPipelineVertexInputStateCreateInfo vertex_input = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = 1,
    .pVertexBindingDescriptions = &vertex_binding,
    .vertexAttributeDescriptionCount = 2,
    .pVertexAttributeDescriptions = attributes,
  };
  const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkViewport viewport = {
    .x = 0.0f,
    .y = 0.0f,
    .width = (float)g_extent.width,
    .height = (float)g_extent.height,
    .minDepth = 0.0f,
    .maxDepth = 1.0f,
  };
  const VkRect2D scissor = {{0, 0}, g_extent};
  const VkPipelineViewportStateCreateInfo viewport_state = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .pViewports = &viewport,
    .scissorCount = 1,
    .pScissors = &scissor,
  };
  const VkPipelineRasterizationStateCreateInfo rasterization = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = VK_CULL_MODE_NONE,
    .frontFace = VK_FRONT_FACE_CLOCKWISE,
    .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineColorBlendAttachmentState blend_attachment = {
    .blendEnable = VK_TRUE,
    .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .colorBlendOp = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alphaBlendOp = VK_BLEND_OP_ADD,
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  const VkPipelineColorBlendStateCreateInfo blend = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &blend_attachment,
  };
  const VkGraphicsPipelineCreateInfo pipeline_info = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .stageCount = 2,
    .pStages = stages,
    .pVertexInputState = &vertex_input,
    .pInputAssemblyState = &input_assembly,
    .pViewportState = &viewport_state,
    .pRasterizationState = &rasterization,
    .pMultisampleState = &multisample,
    .pColorBlendState = &blend,
    .layout = g_pipeline_layout,
    .renderPass = g_render_pass,
    .subpass = 0,
  };
  const VkResult result = vkCreateGraphicsPipelines(
      g_device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &g_pipeline);
  vkDestroyShaderModule(g_device, vertex, NULL);
  vkDestroyShaderModule(g_device, fragment, NULL);
  return vk_ok(result);
}

static int create_filter_pipeline(DrasticDfxShader shader,
                                  const uint8_t *vertex_code,
                                  uint32_t vertex_size,
                                  const uint8_t *fragment_code,
                                  uint32_t fragment_size) {
  VkShaderModule vertex = create_shader_module(vertex_code, vertex_size);
  VkShaderModule fragment = create_shader_module(fragment_code, fragment_size);
  if (!vertex || !fragment) {
    if (vertex) vkDestroyShaderModule(g_device, vertex, NULL);
    if (fragment) vkDestroyShaderModule(g_device, fragment, NULL);
    return 0;
  }
  const VkPipelineShaderStageCreateInfo stages[2] = {
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = vertex,
      .pName = "main",
    },
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = fragment,
      .pName = "main",
    },
  };
  const VkVertexInputBindingDescription vertex_binding = {
    .binding = 0,
    .stride = sizeof(Vertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
  };
  const VkVertexInputAttributeDescription attributes[2] = {
    {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)},
    {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
  };
  const VkPipelineVertexInputStateCreateInfo vertex_input = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = 1,
    .pVertexBindingDescriptions = &vertex_binding,
    .vertexAttributeDescriptionCount = 2,
    .pVertexAttributeDescriptions = attributes,
  };
  const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo viewport_state = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .scissorCount = 1,
  };
  const VkPipelineRasterizationStateCreateInfo rasterization = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = VK_CULL_MODE_NONE,
    .frontFace = VK_FRONT_FACE_CLOCKWISE,
    .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineColorBlendAttachmentState blend_attachment = {
    .blendEnable = VK_FALSE,
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  const VkPipelineColorBlendStateCreateInfo blend = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &blend_attachment,
  };
  const VkDynamicState dynamic_states[2] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
  };
  const VkPipelineDynamicStateCreateInfo dynamic_state = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .dynamicStateCount = 2,
    .pDynamicStates = dynamic_states,
  };
  const VkGraphicsPipelineCreateInfo pipeline_info = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .stageCount = 2,
    .pStages = stages,
    .pVertexInputState = &vertex_input,
    .pInputAssemblyState = &input_assembly,
    .pViewportState = &viewport_state,
    .pRasterizationState = &rasterization,
    .pMultisampleState = &multisample,
    .pColorBlendState = &blend,
    .pDynamicState = &dynamic_state,
    .layout = g_pipeline_layout,
    .renderPass = g_filter_render_pass,
    .subpass = 0,
  };
  const VkResult result = vkCreateGraphicsPipelines(
      g_device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
      &g_filter_pipelines[shader]);
  vkDestroyShaderModule(g_device, vertex, NULL);
  vkDestroyShaderModule(g_device, fragment, NULL);
  return vk_ok(result);
}

#define CREATE_DFX_PIPELINE(name, shader) \
  create_filter_pipeline((shader), dfx_##name##_vert_bin, \
      dfx_##name##_vert_bin_size, dfx_##name##_frag_bin, \
      dfx_##name##_frag_bin_size)

static int create_filter_pipelines(void) {
  return CREATE_DFX_PIPELINE(scale2x, DRASTIC_DFX_SCALE2X) &&
         CREATE_DFX_PIPELINE(hq2x, DRASTIC_DFX_HQ2X) &&
         CREATE_DFX_PIPELINE(fxaa, DRASTIC_DFX_FXAA) &&
         CREATE_DFX_PIPELINE(fxaa_luma, DRASTIC_DFX_FXAA_LUMA) &&
         CREATE_DFX_PIPELINE(fxaa_hq, DRASTIC_DFX_FXAA_HQ) &&
         CREATE_DFX_PIPELINE(smaa_edge, DRASTIC_DFX_SMAA_EDGE) &&
         CREATE_DFX_PIPELINE(smaa_weight, DRASTIC_DFX_SMAA_WEIGHT) &&
         CREATE_DFX_PIPELINE(smaa_blend, DRASTIC_DFX_SMAA_BLEND);
}

#undef CREATE_DFX_PIPELINE

static SampledTexture *custom_sampled_texture(VkCustomState *state,
                                              int screen,
                                              int texture_index) {
  const DrasticCustomTexture *texture =
      &state->shader.textures[texture_index];
  if (texture->kind == DRASTIC_CUSTOM_TEXTURE_FRAMEBUFFER)
    return &g_textures[screen ? TEXTURE_BOTTOM : TEXTURE_TOP];
  if (texture->kind == DRASTIC_CUSTOM_TEXTURE_RAW)
    return &state->textures[0][texture_index];
  return &state->textures[screen][texture_index];
}

static void destroy_sampled_texture(SampledTexture *texture) {
  if (!texture || !g_device) return;
  if (texture->framebuffer)
    vkDestroyFramebuffer(g_device, texture->framebuffer, NULL);
  if (texture->view) vkDestroyImageView(g_device, texture->view, NULL);
  if (texture->image) vkDestroyImage(g_device, texture->image, NULL);
  if (texture->memory) vkFreeMemory(g_device, texture->memory, NULL);
  memset(texture, 0, sizeof(*texture));
}

static void destroy_custom_state(VkCustomState *state) {
  if (!state) return;
  if (g_device) {
    for (int pass = 0; pass < DRASTIC_CUSTOM_SHADER_MAX_PASSES; pass++)
      if (state->pipelines[pass])
        vkDestroyPipeline(g_device, state->pipelines[pass], NULL);
    if (state->pipeline_layout)
      vkDestroyPipelineLayout(g_device, state->pipeline_layout, NULL);
    if (state->descriptor_pool)
      vkDestroyDescriptorPool(g_device, state->descriptor_pool, NULL);
    if (state->descriptor_layout)
      vkDestroyDescriptorSetLayout(g_device, state->descriptor_layout, NULL);
    for (int index = 0; index < DRASTIC_CUSTOM_SHADER_MAX_TEXTURES; index++)
      if (state->samplers[index])
        vkDestroySampler(g_device, state->samplers[index], NULL);
    for (int screen = 0; screen < 2; screen++)
      for (int index = 0; index < DRASTIC_CUSTOM_SHADER_MAX_TEXTURES; index++)
        destroy_sampled_texture(&state->textures[screen][index]);
  }
  drastic_custom_shader_destroy(&state->shader);
  memset(state, 0, sizeof(*state));
}

static uint8_t *custom_rgba_pixels(const DrasticCustomTexture *texture) {
  const size_t count = (size_t)texture->width * texture->height;
  uint8_t *rgba = malloc(count * 4);
  if (!rgba) return NULL;
  for (size_t pixel = 0; pixel < count; pixel++) {
    const uint8_t *source = texture->pixels + pixel * texture->channels;
    uint8_t *target = rgba + pixel * 4;
    switch (texture->format) {
      case DRASTIC_CUSTOM_FORMAT_ALPHA:
        target[0] = target[1] = target[2] = 255; target[3] = source[0]; break;
      case DRASTIC_CUSTOM_FORMAT_LUMINANCE:
        target[0] = target[1] = target[2] = source[0]; target[3] = 255; break;
      case DRASTIC_CUSTOM_FORMAT_LUMINANCE_ALPHA:
        target[0] = target[1] = target[2] = source[0]; target[3] = source[1]; break;
      case DRASTIC_CUSTOM_FORMAT_RGB:
        target[0] = source[0]; target[1] = source[1];
        target[2] = source[2]; target[3] = 255; break;
      case DRASTIC_CUSTOM_FORMAT_RGBA:
        memcpy(target, source, 4); break;
      case DRASTIC_CUSTOM_FORMAT_RED:
        target[0] = source[0]; target[1] = target[2] = 0; target[3] = 255; break;
      case DRASTIC_CUSTOM_FORMAT_RG:
        target[0] = source[0]; target[1] = source[1];
        target[2] = 0; target[3] = 255; break;
    }
  }
  return rgba;
}

static int upload_custom_texture(SampledTexture *texture,
                                 const uint8_t *pixels, size_t size) {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  int coherent = 0;
  const VkBufferCreateInfo buffer_info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (!vk_ok(vkCreateBuffer(g_device, &buffer_info, NULL, &buffer)))
    goto failure;
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(g_device, buffer, &requirements);
  const uint32_t type = find_memory_type(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &coherent);
  if (type == UINT32_MAX) goto failure;
  const VkMemoryAllocateInfo allocation = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = requirements.size,
    .memoryTypeIndex = type,
  };
  if (!vk_ok(vkAllocateMemory(g_device, &allocation, NULL, &memory)) ||
      !vk_ok(vkBindBufferMemory(g_device, buffer, memory, 0))) goto failure;
  void *mapped = NULL;
  if (!vk_ok(vkMapMemory(g_device, memory, 0, size, 0, &mapped)))
    goto failure;
  memcpy(mapped, pixels, size);
  if (!coherent) {
    const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = memory,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
    };
    if (!vk_ok(vkFlushMappedMemoryRanges(g_device, 1, &range))) {
      vkUnmapMemory(g_device, memory);
      goto failure;
    }
  }
  vkUnmapMemory(g_device, memory);
  const VkCommandBufferAllocateInfo command_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = g_command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
  };
  if (!vk_ok(vkAllocateCommandBuffers(g_device, &command_info, &command)))
    goto failure;
  const VkCommandBufferBeginInfo begin = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (!vk_ok(vkBeginCommandBuffer(command, &begin))) goto failure;
  const VkImageMemoryBarrier to_transfer = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = texture->image,
    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, NULL, 0, NULL, 1, &to_transfer);
  const VkBufferImageCopy copy = {
    .bufferOffset = 0,
    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
    .imageExtent = {texture->width, texture->height, 1},
  };
  vkCmdCopyBufferToImage(command, buffer, texture->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  VkImageMemoryBarrier to_shader = to_transfer;
  to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, NULL, 0, NULL, 1, &to_shader);
  if (!vk_ok(vkEndCommandBuffer(command))) goto failure;
  const VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers = &command,
  };
  if (!vk_ok(vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE)) ||
      !vk_ok(vkQueueWaitIdle(g_queue))) goto failure;
  texture->initialized = 1;
  vkFreeCommandBuffers(g_device, g_command_pool, 1, &command);
  vkDestroyBuffer(g_device, buffer, NULL);
  vkFreeMemory(g_device, memory, NULL);
  return 1;

failure:
  if (command)
    vkFreeCommandBuffers(g_device, g_command_pool, 1, &command);
  if (buffer) vkDestroyBuffer(g_device, buffer, NULL);
  if (memory) vkFreeMemory(g_device, memory, NULL);
  return 0;
}

static int create_custom_pipeline(VkCustomState *state, int pass_index) {
  const DrasticCustomPass *pass = &state->shader.passes[pass_index];
  VkShaderModule vertex = create_shader_module(
      pass->vertex_spirv, (uint32_t)pass->vertex_spirv_size);
  VkShaderModule fragment = create_shader_module(
      pass->fragment_spirv, (uint32_t)pass->fragment_spirv_size);
  if (!vertex || !fragment) {
    if (vertex) vkDestroyShaderModule(g_device, vertex, NULL);
    if (fragment) vkDestroyShaderModule(g_device, fragment, NULL);
    set_renderer_error("%s pass %d has invalid Vulkan modules",
                       state->shader.relative_path, pass_index + 1);
    return 0;
  }
  const VkPipelineShaderStageCreateInfo stages[2] = {
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
     VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", NULL},
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
     VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", NULL},
  };
  const VkVertexInputBindingDescription vertex_binding = {
    .binding = 0, .stride = sizeof(Vertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
  };
  const VkVertexInputAttributeDescription attributes[2] = {
    {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)},
    {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
  };
  const VkPipelineVertexInputStateCreateInfo vertex_input = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = 1,
    .pVertexBindingDescriptions = &vertex_binding,
    .vertexAttributeDescriptionCount = 2,
    .pVertexAttributeDescriptions = attributes,
  };
  const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo viewport_state = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1, .scissorCount = 1,
  };
  const VkPipelineRasterizationStateCreateInfo rasterization = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = VK_CULL_MODE_NONE,
    .frontFace = VK_FRONT_FACE_CLOCKWISE,
    .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineColorBlendAttachmentState blend_attachment = {
    .blendEnable = VK_FALSE,
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  const VkPipelineColorBlendStateCreateInfo blend = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .attachmentCount = 1, .pAttachments = &blend_attachment,
  };
  const VkDynamicState dynamic_states[2] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
  };
  const VkPipelineDynamicStateCreateInfo dynamic_state = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .dynamicStateCount = 2, .pDynamicStates = dynamic_states,
  };
  const VkGraphicsPipelineCreateInfo pipeline_info = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .stageCount = 2, .pStages = stages,
    .pVertexInputState = &vertex_input,
    .pInputAssemblyState = &input_assembly,
    .pViewportState = &viewport_state,
    .pRasterizationState = &rasterization,
    .pMultisampleState = &multisample,
    .pColorBlendState = &blend,
    .pDynamicState = &dynamic_state,
    .layout = state->pipeline_layout,
    .renderPass = pass_index + 1 == state->shader.pass_count
        ? g_render_pass : g_filter_render_pass,
    .subpass = 0,
  };
  const VkResult result = vkCreateGraphicsPipelines(
      g_device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
      &state->pipelines[pass_index]);
  vkDestroyShaderModule(g_device, vertex, NULL);
  vkDestroyShaderModule(g_device, fragment, NULL);
  if (!vk_ok(result))
    set_renderer_error("%s pass %d pipeline creation failed (%d)",
                       state->shader.relative_path, pass_index + 1,
                       (int)result);
  return vk_ok(result);
}

static int create_custom_descriptors(VkCustomState *state) {
  VkDescriptorSetLayoutBinding bindings[DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS];
  for (int binding = 0; binding < DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS;
       binding++) {
    bindings[binding] = (VkDescriptorSetLayoutBinding){
      .binding = (uint32_t)binding,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    };
  }
  const VkDescriptorSetLayoutCreateInfo layout_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS,
    .pBindings = bindings,
  };
  if (!vk_ok(vkCreateDescriptorSetLayout(
          g_device, &layout_info, NULL, &state->descriptor_layout))) return 0;
  const uint32_t set_count = (uint32_t)state->shader.pass_count * 2;
  const VkDescriptorPoolSize pool_size = {
    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .descriptorCount = set_count * DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS,
  };
  const VkDescriptorPoolCreateInfo pool_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = set_count,
    .poolSizeCount = 1,
    .pPoolSizes = &pool_size,
  };
  if (!vk_ok(vkCreateDescriptorPool(
          g_device, &pool_info, NULL, &state->descriptor_pool))) return 0;
  VkDescriptorSetLayout layouts[DRASTIC_CUSTOM_SHADER_MAX_PASSES * 2];
  VkDescriptorSet sets[DRASTIC_CUSTOM_SHADER_MAX_PASSES * 2];
  for (uint32_t index = 0; index < set_count; index++)
    layouts[index] = state->descriptor_layout;
  const VkDescriptorSetAllocateInfo allocation = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = state->descriptor_pool,
    .descriptorSetCount = set_count,
    .pSetLayouts = layouts,
  };
  if (!vk_ok(vkAllocateDescriptorSets(g_device, &allocation, sets))) return 0;
  for (int screen = 0; screen < 2; screen++) {
    for (int pass_index = 0; pass_index < state->shader.pass_count;
         pass_index++) {
      const DrasticCustomPass *pass = &state->shader.passes[pass_index];
      const VkDescriptorSet set = sets[screen * state->shader.pass_count +
                                       pass_index];
      state->descriptors[screen][pass_index] = set;
      VkDescriptorImageInfo images[DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS];
      VkWriteDescriptorSet writes[DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS];
      for (int binding = 0; binding < DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS;
           binding++) {
        const int sampler_index = binding < pass->sampler_count ? binding : 0;
        const int texture_index = pass->sampler_textures[sampler_index];
        SampledTexture *texture = custom_sampled_texture(
            state, screen, texture_index);
        images[binding] = (VkDescriptorImageInfo){
          .sampler = state->samplers[texture_index],
          .imageView = texture->view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        writes[binding] = (VkWriteDescriptorSet){
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = set,
          .dstBinding = (uint32_t)binding,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &images[binding],
        };
      }
      vkUpdateDescriptorSets(g_device, DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS,
                             writes, 0, NULL);
    }
  }
  const VkPushConstantRange push_range = {
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    .offset = 0, .size = sizeof(DfxParameters),
  };
  const VkPipelineLayoutCreateInfo pipeline_layout = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &state->descriptor_layout,
    .pushConstantRangeCount = 1,
    .pPushConstantRanges = &push_range,
  };
  return vk_ok(vkCreatePipelineLayout(g_device, &pipeline_layout, NULL,
                                       &state->pipeline_layout));
}

static int build_custom_state(VkCustomState *state) {
  const VkImageUsageFlags filtered =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  const VkImageUsageFlags uploaded =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  for (int index = 0; index < state->shader.texture_count; index++) {
    const DrasticCustomTexture *spec = &state->shader.textures[index];
    const VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = spec->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
      .minFilter = spec->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
    };
    if (!vk_ok(vkCreateSampler(g_device, &sampler_info, NULL,
                                &state->samplers[index]))) {
      set_renderer_error("%s texture %d sampler creation failed",
                         state->shader.relative_path, index);
      return 0;
    }
    if (spec->kind == DRASTIC_CUSTOM_TEXTURE_RAW) {
      SampledTexture *texture = &state->textures[0][index];
      if (!create_sampled_texture(texture, (uint32_t)spec->width,
                                  (uint32_t)spec->height,
                                  VK_FORMAT_R8G8B8A8_UNORM, uploaded))
        return 0;
      uint8_t *rgba = custom_rgba_pixels(spec);
      const int uploaded_ok = rgba && upload_custom_texture(
          texture, rgba, (size_t)spec->width * spec->height * 4);
      free(rgba);
      if (!uploaded_ok) {
        set_renderer_error("%s texture %d upload failed",
                           state->shader.relative_path, index);
        return 0;
      }
    } else if (spec->kind == DRASTIC_CUSTOM_TEXTURE_TARGET &&
               spec->output_scale) {
      const uint32_t width = g_ds_width * (uint32_t)spec->output_scale;
      const uint32_t height = g_ds_height * (uint32_t)spec->output_scale;
      if (width > 4096 || height > 4096) {
        set_renderer_error("%s texture %d exceeds the GPU size limit",
                           state->shader.relative_path, index);
        return 0;
      }
      for (int screen = 0; screen < 2; screen++) {
        SampledTexture *texture = &state->textures[screen][index];
        if (!create_sampled_texture(texture, width, height,
                                    VK_FORMAT_B8G8R8A8_UNORM, filtered))
          return 0;
        const VkImageView view = texture->view;
        const VkFramebufferCreateInfo framebuffer_info = {
          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          .renderPass = g_filter_render_pass,
          .attachmentCount = 1,
          .pAttachments = &view,
          .width = width, .height = height, .layers = 1,
        };
        if (!vk_ok(vkCreateFramebuffer(g_device, &framebuffer_info, NULL,
                                        &texture->framebuffer))) return 0;
      }
    }
  }
  if (!create_custom_descriptors(state)) {
    set_renderer_error("%s descriptor creation failed",
                       state->shader.relative_path);
    return 0;
  }
  for (int pass = 0; pass < state->shader.pass_count; pass++)
    if (!create_custom_pipeline(state, pass)) return 0;
  state->valid = 1;
  return 1;
}

bool drastic_renderer_set_custom_shader(const char *relative_path,
                                        char *error, size_t error_size) {
  if (g_custom.valid && relative_path &&
      !strcmp(g_custom.shader.relative_path, relative_path)) {
    if (error && error_size) error[0] = '\0';
    return true;
  }
  VkCustomState next;
  memset(&next, 0, sizeof(next));
  g_renderer_error[0] = '\0';
  if (!drastic_custom_shader_load(
          relative_path, DRASTIC_CUSTOM_SHADER_LOAD_PIXELS |
                             DRASTIC_CUSTOM_SHADER_LOAD_SPIRV,
          &next.shader, g_renderer_error, sizeof(g_renderer_error))) {
    if (error && error_size)
      snprintf(error, error_size, "%s", g_renderer_error);
    return false;
  }
  if (g_device) vkDeviceWaitIdle(g_device);
  if (!build_custom_state(&next)) {
    if (!g_renderer_error[0])
      set_renderer_error("Could not create the custom Vulkan shader");
    destroy_custom_state(&next);
    if (error && error_size)
      snprintf(error, error_size, "%s", g_renderer_error);
    return false;
  }
  destroy_custom_state(&g_custom);
  g_custom = next;
  g_filter_valid[0] = g_filter_valid[1] = 0;
  g_renderer_error[0] = '\0';
  if (error && error_size) error[0] = '\0';
  return true;
}

const char *drastic_renderer_last_error(void) { return g_renderer_error; }

static void uv_for_rotation(int rotation, float uv[8]) {
  drastic_rotation_display_to_source(rotation, 0.0f, 0.0f,
                                     &uv[0], &uv[1]);
  drastic_rotation_display_to_source(rotation, 1.0f, 0.0f,
                                     &uv[2], &uv[3]);
  drastic_rotation_display_to_source(rotation, 0.0f, 1.0f,
                                     &uv[4], &uv[5]);
  drastic_rotation_display_to_source(rotation, 1.0f, 1.0f,
                                     &uv[6], &uv[7]);
}

static void set_vertex(Vertex *vertex, float pixel_x, float pixel_y,
                       float u, float v) {
  vertex->x = pixel_x * 2.0f / (float)g_extent.width - 1.0f;
  vertex->y = pixel_y * 2.0f / (float)g_extent.height - 1.0f;
  vertex->u = u;
  vertex->v = v;
}

static DrawBatch *begin_batch(int texture, int sampler,
                              const DrawParameters *parameters) {
  if (g_draw_count >= MAX_DRAWS || g_vertex_count + 6 > MAX_VERTICES)
    return NULL;
  DrawBatch *batch = &g_draws[g_draw_count++];
  batch->first_vertex = g_vertex_count;
  batch->vertex_count = 6;
  batch->texture = texture;
  batch->sampler = sampler;
  batch->parameters = *parameters;
  g_vertex_count += 6;
  return batch;
}

static void add_rectangle(float x, float y, float width, float height,
                          int rotation, int texture, int sampler,
                          const DrawParameters *parameters) {
  if (width <= 0.0f || height <= 0.0f) return;
  DrawBatch *batch = begin_batch(texture, sampler, parameters);
  if (!batch) return;
  Vertex *vertices = (Vertex *)(g_staging_mapped + g_staging_base +
                                g_vertex_offset) +
                     batch->first_vertex;
  float uv[8];
  uv_for_rotation(rotation, uv);
  set_vertex(&vertices[0], x, y, uv[0], uv[1]);
  set_vertex(&vertices[1], x, y + height, uv[4], uv[5]);
  set_vertex(&vertices[2], x + width, y + height, uv[6], uv[7]);
  set_vertex(&vertices[3], x, y, uv[0], uv[1]);
  set_vertex(&vertices[4], x + width, y + height, uv[6], uv[7]);
  set_vertex(&vertices[5], x + width, y, uv[2], uv[3]);
}

static void add_custom_rectangle(const DrasticScreenRect *rectangle,
                                 int rotation) {
  if (g_custom_draw_count >= 3 || g_vertex_count + 6 > MAX_VERTICES ||
      rectangle->width <= 0.0f || rectangle->height <= 0.0f) return;
  CustomFinalDraw *draw = &g_custom_draws[g_custom_draw_count++];
  draw->first_vertex = g_vertex_count;
  draw->screen = rectangle->screen ? 1 : 0;
  /* Final filters run in rotated UV space. Keep DraStic's target-size
   * uniform aligned with those texture axes rather than the physical output
   * rectangle, whose width/height are exchanged by a quarter-turn. */
  draw->target_width = (rotation & 1) ? rectangle->height : rectangle->width;
  draw->target_height = (rotation & 1) ? rectangle->width : rectangle->height;
  Vertex *vertices = (Vertex *)(g_staging_mapped + g_staging_base +
                                g_vertex_offset) +
                     g_vertex_count;
  g_vertex_count += 6;
  float uv[8];
  uv_for_rotation(rotation, uv);
  const float x = rectangle->x;
  const float y = rectangle->y;
  const float width = rectangle->width;
  const float height = rectangle->height;
  set_vertex(&vertices[0], x, y, uv[0], uv[1]);
  set_vertex(&vertices[1], x, y + height, uv[4], uv[5]);
  set_vertex(&vertices[2], x + width, y + height, uv[6], uv[7]);
  set_vertex(&vertices[3], x, y, uv[0], uv[1]);
  set_vertex(&vertices[4], x + width, y + height, uv[6], uv[7]);
  set_vertex(&vertices[5], x + width, y, uv[2], uv[3]);
}

static DrawParameters texture_parameters(int effect, int mode, float width,
                                         float height, float target_width,
                                         float target_height) {
  return (DrawParameters){
    .effect = effect,
    .mode = mode,
    .texture_width = width,
    .texture_height = height,
    .target_width = target_width,
    .target_height = target_height,
    .color = {1.0f, 1.0f, 1.0f, 1.0f},
  };
}

static int overlay_texture_index(const DrasticOverlayFrame *overlay) {
  return overlay && overlay->height > overlay->width
      ? TEXTURE_OVERLAY_PORTRAIT : TEXTURE_OVERLAY;
}

static void add_solid_rectangle(float x, float y, float width, float height,
                                float red, float green, float blue) {
  DrawParameters parameters = texture_parameters(
      0, 1, 1.0f, 1.0f, width, height);
  parameters.color[0] = red;
  parameters.color[1] = green;
  parameters.color[2] = blue;
  add_rectangle(x, y, width, height, 0, TEXTURE_TOP,
                DRASTIC_DFX_NEAREST, &parameters);
}

static void build_draws(const DrasticRuntimeConfig *config,
                         const DrasticOverlayFrame *overlay) {
  g_draw_count = 0;
  g_vertex_count = 0;
  g_custom_draw_count = 0;
  if (config->video_filter == DRASTIC_FILTER_CUSTOM) {
    for (int index = 0; index < config->screen_count; index++)
      add_custom_rectangle(&config->screens[index], config->rotation);
  } else {
    const DrasticDfxChain *chain = drastic_dfx_chain(config->video_filter);
    for (int index = 0; index < config->screen_count; index++) {
      const DrasticScreenRect *rectangle = &config->screens[index];
      const int screen_index = rectangle->screen ? 1 : 0;
      const int texture = texture_for_role(screen_index,
                                           chain->final_texture);
      const float target_width = (config->rotation & 1)
          ? rectangle->height : rectangle->width;
      const float target_height = (config->rotation & 1)
          ? rectangle->width : rectangle->height;
      const DrawParameters screen = texture_parameters(
          final_effect(chain), 2, (float)g_textures[texture].width,
          (float)g_textures[texture].height, target_width,
          target_height);
      add_rectangle(rectangle->x, rectangle->y, rectangle->width,
                    rectangle->height, config->rotation,
                    texture, chain->final_sampler,
                    &screen);
    }
  }

  float cursor_x, cursor_y;
  if (config->stylus_visible &&
      drastic_config_map_stylus(config, config->stylus_x, config->stylus_y,
                                 &cursor_x, &cursor_y)) {
    const float radius = g_extent.width >= 1600 ? 10.0f : 7.0f;
    add_solid_rectangle(cursor_x - radius, cursor_y - 2.0f,
                        radius * 2.0f + 1.0f, 5.0f, 0.0f, 0.0f, 0.0f);
    add_solid_rectangle(cursor_x - 2.0f, cursor_y - radius, 5.0f,
                        radius * 2.0f + 1.0f, 0.0f, 0.0f, 0.0f);
    add_solid_rectangle(cursor_x - radius + 1.0f, cursor_y - 1.0f,
                        radius * 2.0f - 1.0f, 3.0f,
                        0.25f, 0.95f, 1.0f);
    add_solid_rectangle(cursor_x - 1.0f, cursor_y - radius + 1.0f, 3.0f,
                        radius * 2.0f - 1.0f,
                        0.25f, 0.95f, 1.0f);
  }

  if (overlay && overlay->visible && overlay->pixels) {
    const int texture = overlay_texture_index(overlay);
    const DrawParameters overlay_parameters = texture_parameters(
        1, 0, (float)overlay->width, (float)overlay->height,
        (float)g_extent.width, (float)g_extent.height);
    add_rectangle(0.0f, 0.0f, (float)g_extent.width,
                  (float)g_extent.height, config->rotation, texture,
                  DRASTIC_DFX_LINEAR,
                  &overlay_parameters);
  }
}

static void record_image_upload(VkCommandBuffer command,
                                SampledTexture *texture,
                                VkDeviceSize buffer_offset) {
  const VkImageMemoryBarrier to_transfer = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = texture->initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .oldLayout = texture->initialized
                     ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                     : VK_IMAGE_LAYOUT_UNDEFINED,
    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = texture->image,
    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(command,
                       texture->initialized
                           ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, NULL, 0, NULL, 1, &to_transfer);
  const VkBufferImageCopy copy = {
    .bufferOffset = buffer_offset,
    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
    .imageExtent = {texture->width, texture->height, 1},
  };
  vkCmdCopyBufferToImage(command, g_staging_buffer, texture->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  VkImageMemoryBarrier to_shader = to_transfer;
  to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, NULL, 0, NULL, 1, &to_shader);
  texture->initialized = 1;
}

static int record_custom_filter_chains(
    VkCommandBuffer command, const DrasticRuntimeConfig *config) {
  if (!g_custom.valid) return 0;
  if (g_custom.shader.pass_count <= 1) return 1;
  const VkDeviceSize vertex_offset =
      g_staging_base + g_filter_vertex_offset;
  vkCmdBindVertexBuffers(command, 0, 1, &g_staging_buffer, &vertex_offset);
  const VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}};
  int needed[2] = {0, 0};
  for (int index = 0; index < config->screen_count; index++)
    needed[config->screens[index].screen ? 1 : 0] = 1;
  for (int screen = 0; screen < 2; screen++) {
    if (!needed[screen] || g_filter_valid[screen]) continue;
    for (int pass_index = 0;
         pass_index + 1 < g_custom.shader.pass_count; pass_index++) {
      const DrasticCustomPass *pass = &g_custom.shader.passes[pass_index];
      SampledTexture *output = custom_sampled_texture(
          &g_custom, screen, pass->output_texture);
      SampledTexture *input = custom_sampled_texture(
          &g_custom, screen, pass->sampler_textures[0]);
      if (!output || !output->framebuffer || !input ||
          !g_custom.pipelines[pass_index]) return 0;
      const VkExtent2D extent = {output->width, output->height};
      const VkRenderPassBeginInfo render_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_filter_render_pass,
        .framebuffer = output->framebuffer,
        .renderArea = {{0, 0}, extent},
        .clearValueCount = 1,
        .pClearValues = &clear,
      };
      vkCmdBeginRenderPass(command, &render_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        g_custom.pipelines[pass_index]);
      const VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)extent.width, .height = (float)extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
      };
      const VkRect2D scissor = {{0, 0}, extent};
      vkCmdSetViewport(command, 0, 1, &viewport);
      vkCmdSetScissor(command, 0, 1, &scissor);
      const VkDescriptorSet descriptor =
          g_custom.descriptors[screen][pass_index];
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              g_custom.pipeline_layout, 0, 1, &descriptor,
                              0, NULL);
      const DfxParameters parameters = {
        .texture_size = {
          1.0f / (float)input->width, 1.0f / (float)input->height,
          (float)input->width, (float)input->height,
        },
        .target_size = {(float)output->width, (float)output->height},
        .time = (float)g_frames / 60.0f,
      };
      vkCmdPushConstants(command, g_custom.pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(parameters), &parameters);
      vkCmdDraw(command, 6, 1, 0, 0);
      vkCmdEndRenderPass(command);
      output->initialized = 1;
    }
    g_filter_valid[screen] = 1;
  }
  return 1;
}

static int record_filter_chains(VkCommandBuffer command,
                                 const DrasticRuntimeConfig *config) {
  if (config->video_filter == DRASTIC_FILTER_CUSTOM)
    return record_custom_filter_chains(command, config);
  const DrasticDfxChain *chain = drastic_dfx_chain(config->video_filter);
  if (!chain->pass_count) return 1;
  const VkDeviceSize vertex_offset =
      g_staging_base + g_filter_vertex_offset;
  vkCmdBindVertexBuffers(command, 0, 1, &g_staging_buffer, &vertex_offset);
  const VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}};
  int needed[2] = {0, 0};
  for (int index = 0; index < config->screen_count; index++)
    needed[config->screens[index].screen ? 1 : 0] = 1;
  for (int screen = 0; screen < 2; screen++) {
    if (!needed[screen] || g_filter_valid[screen]) continue;
    for (int pass_index = 0; pass_index < chain->pass_count; pass_index++) {
      const DrasticDfxPass *pass = &chain->passes[pass_index];
      const int output_index = texture_for_role(screen, pass->output);
      SampledTexture *output = &g_textures[output_index];
      if (!output->framebuffer || !g_filter_pipelines[pass->shader]) return 0;
      const int input_index = texture_for_role(
          screen, pass->inputs[0].texture);
      const SampledTexture *input = &g_textures[input_index];
      const VkExtent2D extent = {output->width, output->height};
      const VkRenderPassBeginInfo render_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_filter_render_pass,
        .framebuffer = output->framebuffer,
        .renderArea = {{0, 0}, extent},
        .clearValueCount = 1,
        .pClearValues = &clear,
      };
      vkCmdBeginRenderPass(command, &render_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        g_filter_pipelines[pass->shader]);
      const VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)extent.width, .height = (float)extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
      };
      const VkRect2D scissor = {{0, 0}, extent};
      vkCmdSetViewport(command, 0, 1, &viewport);
      vkCmdSetScissor(command, 0, 1, &scissor);
      VkDescriptorSet descriptor;
      if (pass->shader == DRASTIC_DFX_SMAA_WEIGHT)
        descriptor = g_filter_descriptors[screen][0];
      else if (pass->shader == DRASTIC_DFX_SMAA_BLEND)
        descriptor = g_filter_descriptors[screen][1];
      else
        descriptor = g_textures[input_index].descriptors[
            pass->inputs[0].sampler];
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              g_pipeline_layout, 0, 1, &descriptor,
                              0, NULL);
      const DfxParameters parameters = {
        .texture_size = {
          1.0f / (float)input->width, 1.0f / (float)input->height,
          (float)input->width, (float)input->height,
        },
        .target_size = {(float)output->width, (float)output->height},
        .time = (float)g_frames / 60.0f,
      };
      vkCmdPushConstants(command, g_pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(parameters), &parameters);
      vkCmdDraw(command, 6, 1, 0, 0);
      vkCmdEndRenderPass(command);
      output->initialized = 1;
    }
    g_filter_valid[screen] = 1;
  }
  return 1;
}

static int record_custom_final_draws(VkCommandBuffer command) {
  if (!g_custom.valid || !g_custom.shader.pass_count) return 0;
  const int pass_index = g_custom.shader.pass_count - 1;
  const DrasticCustomPass *pass = &g_custom.shader.passes[pass_index];
  if (!g_custom.pipelines[pass_index]) return 0;
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    g_custom.pipelines[pass_index]);
  const VkDeviceSize vertex_offset = g_staging_base + g_vertex_offset;
  vkCmdBindVertexBuffers(command, 0, 1, &g_staging_buffer, &vertex_offset);
  const VkViewport viewport = {
    .x = 0.0f, .y = 0.0f,
    .width = (float)g_extent.width, .height = (float)g_extent.height,
    .minDepth = 0.0f, .maxDepth = 1.0f,
  };
  const VkRect2D scissor = {{0, 0}, g_extent};
  vkCmdSetViewport(command, 0, 1, &viewport);
  vkCmdSetScissor(command, 0, 1, &scissor);
  for (uint32_t index = 0; index < g_custom_draw_count; index++) {
    const CustomFinalDraw *draw = &g_custom_draws[index];
    SampledTexture *input = custom_sampled_texture(
        &g_custom, draw->screen, pass->sampler_textures[0]);
    if (!input) return 0;
    const VkDescriptorSet descriptor =
        g_custom.descriptors[draw->screen][pass_index];
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_custom.pipeline_layout, 0, 1, &descriptor,
                            0, NULL);
    const DfxParameters parameters = {
      .texture_size = {
        1.0f / (float)input->width, 1.0f / (float)input->height,
        (float)input->width, (float)input->height,
      },
      .target_size = {draw->target_width, draw->target_height},
      .time = (float)g_frames / 60.0f,
    };
    vkCmdPushConstants(command, g_custom.pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(parameters), &parameters);
    vkCmdDraw(command, 6, 1, draw->first_vertex, 0);
  }
  return 1;
}

static int record_commands(VkCommandBuffer command, uint32_t image_index,
                           int upload_screens,
                           int upload_overlay, int overlay_texture,
                           const DrasticRuntimeConfig *config) {
  vkResetCommandBuffer(command, 0);
  const VkCommandBufferBeginInfo begin = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (!vk_ok(vkBeginCommandBuffer(command, &begin))) return 0;
  if (upload_screens) {
    record_image_upload(command, &g_textures[TEXTURE_TOP],
                        g_staging_base + g_texture_offsets[TEXTURE_TOP]);
    record_image_upload(command, &g_textures[TEXTURE_BOTTOM],
                        g_staging_base + g_texture_offsets[TEXTURE_BOTTOM]);
  }
  if (upload_overlay)
    record_image_upload(command, &g_textures[overlay_texture],
                        g_staging_base + g_texture_offsets[overlay_texture]);
  if (!g_textures[TEXTURE_AREA].initialized) {
    record_image_upload(command, &g_textures[TEXTURE_AREA],
                        g_staging_base + g_texture_offsets[TEXTURE_AREA]);
    record_image_upload(command, &g_textures[TEXTURE_SEARCH],
                        g_staging_base + g_texture_offsets[TEXTURE_SEARCH]);
  }
  if (!record_filter_chains(command, config)) return 0;

  const VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
  const VkRenderPassBeginInfo render_begin = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .renderPass = g_render_pass,
    .framebuffer = g_framebuffers[image_index],
    .renderArea = {{0, 0}, g_extent},
    .clearValueCount = 1,
    .pClearValues = &clear,
  };
  vkCmdBeginRenderPass(command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
  if (config->video_filter == DRASTIC_FILTER_CUSTOM &&
      !record_custom_final_draws(command)) {
    vkCmdEndRenderPass(command);
    return 0;
  }
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);
  const VkDeviceSize vertex_offset = g_staging_base + g_vertex_offset;
  vkCmdBindVertexBuffers(command, 0, 1, &g_staging_buffer, &vertex_offset);
  int bound_texture = -1;
  int bound_sampler = -1;
  for (uint32_t index = 0; index < g_draw_count; index++) {
    const DrawBatch *draw = &g_draws[index];
    if (draw->texture != bound_texture || draw->sampler != bound_sampler) {
      const VkDescriptorSet descriptor =
          g_textures[draw->texture].descriptors[draw->sampler];
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              g_pipeline_layout, 0, 1, &descriptor,
                              0, NULL);
      bound_texture = draw->texture;
      bound_sampler = draw->sampler;
    }
    vkCmdPushConstants(command, g_pipeline_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(draw->parameters), &draw->parameters);
    vkCmdDraw(command, draw->vertex_count, 1, draw->first_vertex, 0);
  }
  vkCmdEndRenderPass(command);
  return vk_ok(vkEndCommandBuffer(command));
}

static int lsfg_try_create(void) {
  if (g_lsfg_runtime) return 1;
  if (g_lsfg_init_attempted || !g_lsfg_swapchain_compatible ||
      !g_lsfg_device_capable || !g_instance || !g_physical || !g_device ||
      !g_queue || !g_swapchain || g_image_count < 3)
    return 0;

  g_lsfg_init_attempted = 1;
  float flow_scale = prefs_get_float("Wrapper/LSFGFlowScale", 0.25f);
  if (flow_scale != 0.25f && flow_scale != 0.5f)
    flow_scale = 0.25f;

  const LsfgNxCreateInfo info = {
    .instance = g_instance,
    .physical_device = g_physical,
    .device = g_device,
    .queue = g_queue,
    .queue_family_index = g_queue_family,
    .get_instance_proc_addr = vkGetInstanceProcAddr,
    .swapchain = g_swapchain,
    .extent = g_extent,
    .swapchain_images = g_swap_images,
    .swapchain_image_count = g_image_count,
    .shader_dll_path = lsfg_dll_path(),
    .flow_scale = flow_scale,
    .performance_mode = prefs_get_bool("Wrapper/LSFGPerformance", true),
  };
  g_lsfg_runtime = lsfg_nx_create(&info);
  return g_lsfg_runtime != NULL;
}

bool drastic_renderer_init(const DrasticRuntimeConfig *config) {
  g_renderer_error[0] = '\0';
  g_ds_width = config &&
      (config->core_config & (UINT64_C(1) << 41)) ? 512 : 256;
  g_ds_height = g_ds_width * 3 / 4;
  free(g_core_pixels);
  const size_t core_pixel_count =
      (size_t)g_ds_width * g_ds_height * 2;
  g_core_pixels = calloc(core_pixel_count, sizeof(*g_core_pixels));
  if (!g_core_pixels) {
    set_renderer_error("Could not allocate Vulkan screen capture buffers.");
    return false;
  }
  int lsfg_launch_enabled =
      prefs_get_bool("Wrapper/LSFGEnabled", false) &&
      file_readable(lsfg_dll_path());
  if (lsfg_launch_enabled && !enable_nvk_no_cbuf())
    lsfg_launch_enabled = 0;
  g_lsfg_pipeline_prepared = lsfg_launch_enabled;
  g_frame_slot_count = lsfg_launch_enabled ? RENDER_FRAME_SLOTS : 1;
  g_frame_slot_cursor = 0;
  memset(g_frame_slots, 0, sizeof(g_frame_slots));
  /* LSFG owns presentation pacing and needs its larger swapchain. Keep the
   * native low-latency path mutually exclusive for the lifetime of this
   * renderer, including when LSFG is toggled from the in-game overlay. */
  g_low_latency = config && config->low_latency &&
                  !lsfg_launch_enabled;
  __atomic_store_n(&g_lsfg_enabled_requested, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_lsfg_runtime_available, 0, __ATOMIC_RELEASE);
  g_lsfg_init_attempted = 0;
  g_lsfg_device_capable = 0;
  g_lsfg_swapchain_compatible = 0;
  g_filtered_filter = DRASTIC_FILTER_COUNT;
  g_filter_valid[0] = g_filter_valid[1] = 0;
  g_frames = 0;
  const char *extensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_NN_VI_SURFACE_EXTENSION_NAME,
  };
  const VkApplicationInfo application = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "DrasticDS_nx",
    .applicationVersion = VK_MAKE_VERSION(1, 1, 0),
    .pEngineName = "Drastic Switch wrapper",
    .engineVersion = VK_MAKE_VERSION(1, 1, 0),
    .apiVersion = VK_API_VERSION_1_1,
  };
  const VkInstanceCreateInfo instance_info = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &application,
    .enabledExtensionCount = 2,
    .ppEnabledExtensionNames = extensions,
  };
  VkResult result = vkCreateInstance(&instance_info, NULL, &g_instance);
  if (!vk_ok(result)) {
    return false;
  }
  const VkViSurfaceCreateInfoNN surface_info = {
    .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
    .window = nwindowGetDefault(),
  };
  result = vkCreateViSurfaceNN(g_instance, &surface_info, NULL, &g_surface);
  if (!vk_ok(result)) {
    return false;
  }
  if (!choose_physical_device()) {
    return false;
  }
  if (!create_device()) {
    return false;
  }
  if (!choose_surface_format()) {
    return false;
  }
  if (!create_swapchain()) {
    return false;
  }
  if (!create_command_resources()) {
    return false;
  }
  if (!create_render_targets()) {
    return false;
  }
  if (!create_texture_resources()) {
    return false;
  }
  if (!create_staging_buffer()) {
    return false;
  }
  if (!create_pipeline()) {
    return false;
  }
  if (!create_filter_pipelines()) {
    return false;
  }
  if (config && config->video_filter == DRASTIC_FILTER_CUSTOM &&
      !drastic_renderer_set_custom_shader(config->custom_shader, NULL, 0)) {
    return false;
  }
  const VkSemaphoreCreateInfo semaphore_info = {
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
  };
  const VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };
  for (uint32_t index = 0; index < g_frame_slot_count; index++) {
    RenderFrameSlot *slot = &g_frame_slots[index];
    if (!vk_ok(vkCreateSemaphore(g_device, &semaphore_info, NULL,
                                  &slot->acquired)) ||
        !vk_ok(vkCreateSemaphore(g_device, &semaphore_info, NULL,
                                  &slot->rendered)) ||
        !vk_ok(vkCreateFence(g_device, &fence_info, NULL,
                             &slot->fence)))
      return false;
  }
  return true;
}

static void stage_screen_pixels(uint8_t *destination,
                                const uint32_t *source) {
  const size_t pixels = (size_t)g_ds_width * g_ds_height;
  memcpy(destination, source, pixels * sizeof(uint32_t));
}

static void capture_core_frame(DrasticCoreRenderFrame core_render,
                               void *env, void *clazz, int *consumed) {
  if (*consumed) return;
  const size_t pixels = (size_t)g_ds_width * g_ds_height;
  drastic_vk_capture_begin(g_core_pixels, g_core_pixels + pixels,
                           g_ds_width, g_ds_height);
  core_render(env, clazz, (int)DRASTIC_VK_CAPTURE_TOP_TEXTURE,
              (int)DRASTIC_VK_CAPTURE_BOTTOM_TEXTURE, 0);
  drastic_vk_capture_end();
  *consumed = 1;
}

static int wait_frame_slot(uint32_t slot_index) {
  RenderFrameSlot *slot = &g_frame_slots[slot_index];
  if (!slot->pending) return 1;

  VkFence completion = slot->fence;
  if (g_frame_slot_count > 1) {
    /* LSFG consumes slot->rendered after this renderer submission. A fence on
     * the following source frame is later in the same queue, so it covers the
     * complete LSFG tail as well as this slot's command/staging resources. */
    RenderFrameSlot *guard =
        &g_frame_slots[(slot_index + 1) % g_frame_slot_count];
    if (guard->pending) {
      completion = guard->fence;
    } else {
      const VkResult idle = vkQueueWaitIdle(g_queue);
      if (!vk_ok(idle)) return 0;
      slot->pending = 0;
      return 1;
    }
  }

  /* Keep the applet message pump reachable while HOME/sleep is taking the VI
   * display away. With an infinite wait the focus notification cannot be
   * processed, leaving the process stuck before it can enter a clean suspend. */
  const VkResult waited = vkWaitForFences(
      g_device, 1, &completion, VK_TRUE, UINT64_C(50000000));
  if (waited == VK_TIMEOUT) return 0;
  if (!vk_ok(waited))
    return 0;
  slot->pending = 0;
  return 1;
}

void drastic_renderer_present(const DrasticRuntimeConfig *config,
                              DrasticCoreRenderFrame core_render,
                              void *env, void *clazz,
                              const DrasticOverlayFrame *overlay,
                              bool consume_core_frame) {
  if (config->video_filter == DRASTIC_FILTER_CUSTOM && !g_custom.valid)
    return;
  const int upload_screens = consume_core_frame;
  const int defer_core_capture = consume_core_frame && g_low_latency;
  int core_frame_consumed = !consume_core_frame;
  /* Use DraStic's own renderFrame selection instead of getScreenBuffers().
   * The latter reads the active producer buffer; renderFrame atomically picks
   * the completed buffer and both screen regions before invoking our Vulkan
   * upload hooks. */
  if (consume_core_frame && !defer_core_capture)
    capture_core_frame(core_render, env, clazz, &core_frame_consumed);

  /* Complete the LSFG -> native-present handoff before acquiring or writing
   * the first ordinary frame. Destroying the backend later, after the native
   * submission was already queued, allowed the tail of LSFG's two-present
   * sequence to overlap that submission and produced a visibly dirty frame. */
  if (!lsfg_requested() && g_lsfg_runtime) {
    lsfg_destroy_runtime();
    g_lsfg_init_attempted = 0;
  }

  const uint32_t slot_index = g_frame_slot_cursor;
  RenderFrameSlot *frame = &g_frame_slots[slot_index];
  if (!wait_frame_slot(slot_index)) {
    capture_core_frame(core_render, env, clazz, &core_frame_consumed);
    return;
  }
  g_staging_base = (VkDeviceSize)slot_index * g_staging_stride;
  VkResult result;

  if (g_filtered_filter != config->video_filter) {
    g_filtered_filter = config->video_filter;
    g_filter_valid[0] = g_filter_valid[1] = 0;
  }
  if (upload_screens) g_filter_valid[0] = g_filter_valid[1] = 0;

  uint32_t image_index = 0;
  const VkResult acquired = vkAcquireNextImageKHR(
      g_device, g_swapchain, UINT64_C(50000000), frame->acquired,
      VK_NULL_HANDLE,
      &image_index);
  if (acquired == VK_TIMEOUT) {
    capture_core_frame(core_render, env, clazz, &core_frame_consumed);
    return;
  }
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
    capture_core_frame(core_render, env, clazz, &core_frame_consumed);
    return;
  }

  if (defer_core_capture)
    capture_core_frame(core_render, env, clazz, &core_frame_consumed);
  if (upload_screens) {
    const size_t pixels = (size_t)g_ds_width * g_ds_height;
    stage_screen_pixels(g_staging_mapped + g_staging_base +
                            g_texture_offsets[TEXTURE_TOP],
                        g_core_pixels);
    stage_screen_pixels(g_staging_mapped + g_staging_base +
                            g_texture_offsets[TEXTURE_BOTTOM],
                        g_core_pixels + pixels);
  }
  const int upload_overlay = overlay && overlay->visible && overlay->pixels &&
      overlay->generation != g_overlay_generation;
  const int overlay_texture = overlay_texture_index(overlay);
  if (upload_overlay) {
    const size_t bytes = (size_t)overlay->width * overlay->height *
                         sizeof(uint32_t);
    if (bytes > (size_t)DRASTIC_OVERLAY_PIXELS * sizeof(uint32_t)) return;
    memcpy(g_staging_mapped + g_staging_base +
               g_texture_offsets[overlay_texture],
           overlay->pixels, bytes);
  }
  build_draws(config, overlay);
  if (!g_staging_coherent) {
    const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = g_staging_memory,
      .offset = g_staging_base,
      .size = g_staging_stride,
    };
    result = vkFlushMappedMemoryRanges(g_device, 1, &range);
    if (!vk_ok(result)) {
      return;
    }
  }
  if (!record_commands(frame->command, image_index, upload_screens,
                       upload_overlay,
                       overlay_texture, config)) {
    return;
  }

  if (!vk_ok(vkResetFences(g_device, 1, &frame->fence))) return;
  const VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  const VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &frame->acquired,
    .pWaitDstStageMask = &wait_stage,
    .commandBufferCount = 1,
    .pCommandBuffers = &frame->command,
    .signalSemaphoreCount = 1,
    .pSignalSemaphores = &frame->rendered,
  };
  result = vkQueueSubmit(g_queue, 1, &submit, frame->fence);
  if (!vk_ok(result)) {
    return;
  }
  frame->pending = 1;
  g_frame_slot_cursor = (slot_index + 1) % g_frame_slot_count;
  if (upload_overlay) g_overlay_generation = overlay->generation;
  const VkPresentInfoKHR present = {
    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &frame->rendered,
    .swapchainCount = 1,
    .pSwapchains = &g_swapchain,
    .pImageIndices = &image_index,
  };
  const int requested = lsfg_requested();
  if (requested) {
    if (!g_lsfg_swapchain_compatible || !lsfg_try_create()) return;
    VkResult present_result = VK_ERROR_INITIALIZATION_FAILED;
    if (!lsfg_nx_present(g_lsfg_runtime, g_queue, &present,
                         &present_result))
      return;
    if (present_result != VK_SUCCESS &&
        present_result != VK_SUBOPTIMAL_KHR)
      return;
  } else {
    const VkResult present_result = vkQueuePresentKHR(g_queue, &present);
    if (present_result != VK_SUCCESS &&
        present_result != VK_SUBOPTIMAL_KHR)
      return;
  }
  g_frames++;
}

void drastic_renderer_suspend(void) {
  if (!g_device) return;
  if (vkDeviceWaitIdle(g_device) != VK_SUCCESS) return;
  for (uint32_t index = 0; index < g_frame_slot_count; index++)
    g_frame_slots[index].pending = 0;
}

void drastic_renderer_resume(void) {
  /* The device and VI swapchain survive Switch application suspension. Force
   * cached post-FX and overlay content to be refreshed on the first frame. */
  g_filter_valid[0] = g_filter_valid[1] = 0;
  g_overlay_generation = UINT64_MAX;
}

void drastic_renderer_shutdown(void) {
  if (g_device) vkDeviceWaitIdle(g_device);
  lsfg_destroy_runtime();
  g_lsfg_init_attempted = 0;
  g_lsfg_device_capable = 0;
  g_lsfg_swapchain_compatible = 0;
  g_lsfg_pipeline_prepared = 0;
  g_low_latency = 0;
  __atomic_store_n(&g_lsfg_runtime_available, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_lsfg_enabled_requested, 0, __ATOMIC_RELEASE);
  if (g_device) {
    destroy_custom_state(&g_custom);
    for (uint32_t index = 0; index < g_frame_slot_count; index++) {
      RenderFrameSlot *slot = &g_frame_slots[index];
      if (slot->fence) vkDestroyFence(g_device, slot->fence, NULL);
      if (slot->acquired)
        vkDestroySemaphore(g_device, slot->acquired, NULL);
      if (slot->rendered)
        vkDestroySemaphore(g_device, slot->rendered, NULL);
    }
    if (g_pipeline) vkDestroyPipeline(g_device, g_pipeline, NULL);
    for (int shader = 0; shader < DRASTIC_DFX_SHADER_COUNT; shader++)
      if (g_filter_pipelines[shader])
        vkDestroyPipeline(g_device, g_filter_pipelines[shader], NULL);
    if (g_pipeline_layout)
      vkDestroyPipelineLayout(g_device, g_pipeline_layout, NULL);
    if (g_descriptor_pool)
      vkDestroyDescriptorPool(g_device, g_descriptor_pool, NULL);
    if (g_descriptor_layout)
      vkDestroyDescriptorSetLayout(g_device, g_descriptor_layout, NULL);
    for (int sampler = 0; sampler < 2; sampler++)
      if (g_samplers[sampler])
        vkDestroySampler(g_device, g_samplers[sampler], NULL);
    for (int index = 0; index < TEXTURE_COUNT; index++) {
      if (g_textures[index].framebuffer)
        vkDestroyFramebuffer(g_device, g_textures[index].framebuffer, NULL);
      if (g_textures[index].view)
        vkDestroyImageView(g_device, g_textures[index].view, NULL);
      if (g_textures[index].image)
        vkDestroyImage(g_device, g_textures[index].image, NULL);
      if (g_textures[index].memory)
        vkFreeMemory(g_device, g_textures[index].memory, NULL);
    }
    if (g_staging_mapped)
      vkUnmapMemory(g_device, g_staging_memory);
    if (g_staging_buffer)
      vkDestroyBuffer(g_device, g_staging_buffer, NULL);
    if (g_staging_memory)
      vkFreeMemory(g_device, g_staging_memory, NULL);
    for (uint32_t index = 0; index < g_image_count; index++) {
      if (g_framebuffers[index])
        vkDestroyFramebuffer(g_device, g_framebuffers[index], NULL);
      if (g_swap_views[index])
        vkDestroyImageView(g_device, g_swap_views[index], NULL);
    }
    if (g_render_pass)
      vkDestroyRenderPass(g_device, g_render_pass, NULL);
    if (g_filter_render_pass)
      vkDestroyRenderPass(g_device, g_filter_render_pass, NULL);
    if (g_command_pool)
      vkDestroyCommandPool(g_device, g_command_pool, NULL);
    if (g_swapchain)
      vkDestroySwapchainKHR(g_device, g_swapchain, NULL);
    vkDestroyDevice(g_device, NULL);
  }
  if (g_surface) vkDestroySurfaceKHR(g_instance, g_surface, NULL);
  if (g_instance) vkDestroyInstance(g_instance, NULL);
  free(g_core_pixels);
  g_core_pixels = NULL;
  memset(g_textures, 0, sizeof(g_textures));
  memset(g_filter_pipelines, 0, sizeof(g_filter_pipelines));
  g_instance = VK_NULL_HANDLE;
  g_device = VK_NULL_HANDLE;
  g_queue = VK_NULL_HANDLE;
  memset(g_frame_slots, 0, sizeof(g_frame_slots));
  g_frame_slot_count = 1;
  g_frame_slot_cursor = 0;
  g_staging_mapped = NULL;
  g_staging_size = 0;
  g_staging_stride = 0;
  g_staging_base = 0;
}

unsigned drastic_renderer_frame_count(void) { return g_frames; }

bool drastic_renderer_lsfg_available(void) {
  return __atomic_load_n(&g_lsfg_runtime_available, __ATOMIC_ACQUIRE) != 0;
}

bool drastic_renderer_lsfg_enabled(void) { return lsfg_requested() != 0; }

bool drastic_renderer_lsfg_request_enabled(bool enabled) {
  if (enabled && !drastic_renderer_lsfg_available()) return false;
  __atomic_store_n(&g_lsfg_enabled_requested, enabled != 0,
                   __ATOMIC_RELEASE);
  return true;
}
