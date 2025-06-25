#include "wrapper_private.h"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common.h"
#include "util/os_misc.h"
#include <fcntl.h>

static VkResult
wrapper_setup_device_extensions(struct wrapper_physical_device *pdevice) {
   struct vk_device_extension_table *exts = &pdevice->vk.supported_extensions;
   VkExtensionProperties pdevice_extensions[VK_DEVICE_EXTENSION_COUNT];
   uint32_t pdevice_extension_count = VK_DEVICE_EXTENSION_COUNT;
   VkResult result;

   result = pdevice->dispatch_table.EnumerateDeviceExtensionProperties(
      pdevice->dispatch_handle, NULL, &pdevice_extension_count, pdevice_extensions);

   if (result != VK_SUCCESS)
      return result;

   *exts = wrapper_device_extensions;

   for (int i = 0; i < pdevice_extension_count; i++) {
      int idx;
      for (idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
         if (strcmp(vk_device_extensions[idx].extensionName,
                     pdevice_extensions[i].extensionName) == 0)
            break;
      }

      if (idx >= VK_DEVICE_EXTENSION_COUNT)
         continue;

      if (wrapper_filter_extensions.extensions[idx])
         continue;

      pdevice->base_supported_extensions.extensions[idx] =
         exts->extensions[idx] = true;
   }

   exts->KHR_present_wait = exts->KHR_timeline_semaphore;

   return VK_SUCCESS;
}

static void
wrapper_apply_device_extension_blacklist(struct wrapper_physical_device *physical_device) {
   const char *blacklist = getenv("WRAPPER_EXTENSION_BLACKLIST");
   if (!blacklist)
      return;

   for (int i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
      if (strstr(blacklist, vk_device_extensions[i].extensionName)) {
         physical_device->vk.supported_extensions.extensions[i] = false;
      }
   }
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->instance, pName);
}

VkResult enumerate_physical_device(struct vk_instance *_instance)
{
   struct wrapper_instance *instance = (struct wrapper_instance *)_instance;
   VkPhysicalDevice physical_devices[16];
   uint32_t physical_device_count = 16;
   VkResult result;

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, physical_devices);

   if (result != VK_SUCCESS)
      return result;

   for (int i = 0; i < physical_device_count; i++) {
      PFN_vkGetInstanceProcAddr get_instance_proc_addr;
      struct wrapper_physical_device *pdevice;

      pdevice = vk_zalloc(&_instance->alloc, sizeof(*pdevice), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      if (!pdevice)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      struct vk_physical_device_dispatch_table dispatch_table;
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_entrypoints, true);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wsi_physical_device_entrypoints, false);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_trampolines, false);

      result = vk_physical_device_init(&pdevice->vk,
                                       &instance->vk,
                                       NULL, NULL, NULL,
                                       &dispatch_table);
      if (result != VK_SUCCESS) {
         vk_free(&_instance->alloc, pdevice);
         return result;
      }

      pdevice->instance = instance;
      pdevice->dispatch_handle = physical_devices[i];
      get_instance_proc_addr = instance->dispatch_table.GetInstanceProcAddr;

      vk_physical_device_dispatch_table_load(&pdevice->dispatch_table,
                                             get_instance_proc_addr,
                                             instance->dispatch_handle);

      wrapper_setup_device_extensions(pdevice);
      wrapper_apply_device_extension_blacklist(pdevice);
      wrapper_setup_device_features(pdevice);

      struct vk_features *supported_features = &pdevice->vk.supported_features;
      pdevice->base_supported_features = *supported_features;

      #define ENABLE_FEAT(feature) \
         if (!supported_features->feature) { \
            pdevice->fake_##feature = true; \
            supported_features->feature = true; \
         }

      ENABLE_FEAT(geometryShader);
      ENABLE_FEAT(robustBufferAccess);
      ENABLE_FEAT(shaderStorageImageExtendedFormats);
      ENABLE_FEAT(shaderStorageImageWriteWithoutFormat);
      ENABLE_FEAT(depthClamp);
      ENABLE_FEAT(depthBiasClamp);
      ENABLE_FEAT(fillModeNonSolid);
      ENABLE_FEAT(sampleRateShading);
      ENABLE_FEAT(samplerAnisotropy);
      ENABLE_FEAT(shaderClipDistance);
      ENABLE_FEAT(shaderCullDistance);
      ENABLE_FEAT(textureCompressionBC);
      ENABLE_FEAT(occlusionQueryPrecise);
      ENABLE_FEAT(independentBlend);
      ENABLE_FEAT(multiViewport);
      ENABLE_FEAT(fullDrawIndexUint32);
      ENABLE_FEAT(shaderImageGatherExtended);
      ENABLE_FEAT(dualSrcBlend);
      ENABLE_FEAT(imageCubeArray);
      ENABLE_FEAT(drawIndirectFirstInstance);
      ENABLE_FEAT(fragmentStoresAndAtomics);
      ENABLE_FEAT(multiDrawIndirect);
      ENABLE_FEAT(tessellationShader);
      ENABLE_FEAT(logicOp);
      ENABLE_FEAT(variableMultisampleRate);
      ENABLE_FEAT(vertexPipelineStoresAndAtomics);
      ENABLE_FEAT(shaderImageGatherExtended);
      ENABLE_FEAT(shaderImageGatherExtended);
      ENABLE_FEAT(memoryMapPlaced);

      #define ENABLE_EXT(extension) \
         if (!pdevice->vk.supported_extensions.extension) { \
            pdevice->fake_##extension = true; \
            pdevice->vk.supported_extensions.extension = true; \
         }

      ENABLE_EXT(EXT_map_memory_placed);
      ENABLE_EXT(EXT_transform_feedback);
      ENABLE_EXT(EXT_depth_clip_enable);
      ENABLE_EXT(EXT_custom_border_color);

      supported_features->presentId = true;
      supported_features->presentWait = supported_features->timelineSemaphore;
      supported_features->swapchainMaintenance1 = true;
      supported_features->imageCompressionControlSwapchain = false;

      pdevice->vk.supported_extensions.KHR_map_memory2 = true;
      supported_features->memoryUnmapReserve = true;

      result = wsi_device_init(&pdevice->wsi_device,
                               wrapper_physical_device_to_handle(pdevice),
                               wrapper_wsi_proc_addr, &_instance->alloc, -1,
                               NULL, &(struct wsi_device_options){});
      if (result != VK_SUCCESS) {
         vk_physical_device_finish(&pdevice->vk);
         vk_free(&_instance->alloc, pdevice);
         return result;
      }
      pdevice->vk.wsi_device = &pdevice->wsi_device;
      pdevice->wsi_device.force_bgra8_unorm_first = true;
#ifdef __ANDROID__
      pdevice->wsi_device.wants_ahardware_buffer = true;
#endif

      pdevice->driver_properties = (VkPhysicalDeviceDriverProperties) {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };
      pdevice->properties2 = (VkPhysicalDeviceProperties2) {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &pdevice->driver_properties,
      };
      pdevice->dispatch_table.GetPhysicalDeviceProperties2(
         pdevice->dispatch_handle, &pdevice->properties2);

      pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
         pdevice->dispatch_handle, &pdevice->memory_properties);

      const char *app_name = instance->vk.app_info.app_name
         ? instance->vk.app_info.app_name : "wrapper";

      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
          pdevice->properties2.properties.driverVersion > VK_MAKE_VERSION(512, 744, 0) &&
          strstr(app_name, "clvk")) {
         /* HACK: Fixed clvk not working on qualcomm proprietary driver. */
         supported_features->globalPriorityQuery = false;
      }

      pdevice->dma_heap_fd = open("/dev/dma_heap/system", O_RDONLY);
      if (pdevice->dma_heap_fd < 0)
         pdevice->dma_heap_fd = open("/dev/ion", O_RDONLY);

      list_addtail(&pdevice->vk.link, &_instance->physical_devices.list);
   }

   return VK_SUCCESS;
}

void destroy_physical_device(struct vk_physical_device *pdevice) {
   VK_FROM_HANDLE(wrapper_physical_device, wpdevice,
                  vk_physical_device_to_handle(pdevice));
   if (wpdevice->dma_heap_fd != -1)
      close(wpdevice->dma_heap_fd);
   wsi_device_finish(pdevice->wsi_device, &pdevice->instance->alloc);
   vk_physical_device_finish(pdevice);
   vk_free(&pdevice->instance->alloc, pdevice);
}

static bool isBCFormat(VkFormat format)
{
   switch (format) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC2_UNORM_BLOCK:
      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC3_UNORM_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:
      case VK_FORMAT_BC4_UNORM_BLOCK:
      case VK_FORMAT_BC4_SNORM_BLOCK:
      case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:
      case VK_FORMAT_BC6H_UFLOAT_BLOCK:
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      case VK_FORMAT_BC7_UNORM_BLOCK:
      case VK_FORMAT_BC7_SRGB_BLOCK:
         return true;
      default:
         return false;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                           const char* pLayerName,
                                           uint32_t* pPropertyCount,
                                           VkExtensionProperties* pProperties)
{
   return vk_common_EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceFeatures* pFeatures)
{
   return vk_common_GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceFeatures2* pFeatures) {
   vk_common_GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
   vk_foreach_struct(prop, pFeatures->pNext) {
      switch (prop->sType) {
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
         {
            VkPhysicalDeviceTransformFeedbackFeaturesEXT *feedback_prop = (VkPhysicalDeviceTransformFeedbackFeaturesEXT *)prop;
            feedback_prop->transformFeedback = true;
            feedback_prop->geometryStreams = true;
            break;
         }
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
         {
            VkPhysicalDeviceDepthClipEnableFeaturesEXT *depthClip_prop = (VkPhysicalDeviceDepthClipEnableFeaturesEXT *)prop;
            depthClip_prop->depthClipEnable = true;
            break;
         }
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
         {
            VkPhysicalDeviceCustomBorderColorFeaturesEXT *customBorderColor_prop = (VkPhysicalDeviceCustomBorderColorFeaturesEXT *)prop;
            customBorderColor_prop->customBorderColors = true;
            customBorderColor_prop->customBorderColorWithoutFormat = true;
            break;
         }
         default:
         {
            break;
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceProperties2* pProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties2(pdevice->dispatch_handle, pProperties);

   vk_foreach_struct(prop, pProperties->pNext) {
      switch (prop->sType) {
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT:
         {
            VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *placed_prop = (VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *)prop;
            uint64_t os_page_size;
            os_get_page_size(&os_page_size);
            placed_prop->minPlacedMemoryMapAlignment = os_page_size;
            break;
         }
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR:
         {
            VkPhysicalDeviceFloatControlsPropertiesKHR *float_prop = (VkPhysicalDeviceFloatControlsProperties *)prop;
            float_prop->shaderDenormFlushToZeroFloat16 = false;
            float_prop->shaderDenormFlushToZeroFloat32 = false;
            float_prop->shaderRoundingModeRTEFloat16 = false;
            float_prop->shaderRoundingModeRTEFloat32 = false;
            float_prop->shaderSignedZeroInfNanPreserveFloat16 = false;
            float_prop->shaderSignedZeroInfNanPreserveFloat32 = false;
            break;
         }
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT:
         {
            VkPhysicalDeviceTransformFeedbackPropertiesEXT *feedback_prop = (VkPhysicalDeviceTransformFeedbackPropertiesEXT *)prop;
            feedback_prop->maxTransformFeedbackStreams = 4;
            feedback_prop->maxTransformFeedbackBuffers = 4;
            feedback_prop->maxTransformFeedbackBufferSize = 0xffffffff;
            feedback_prop->maxTransformFeedbackStreamDataSize = 512;
            feedback_prop->maxTransformFeedbackBufferDataSize = 512;
            feedback_prop->maxTransformFeedbackBufferDataStride = 512;
            feedback_prop->transformFeedbackQueries = true;
            feedback_prop->transformFeedbackStreamsLinesTriangles = true;
            feedback_prop->transformFeedbackRasterizationStreamSelect = false;
            feedback_prop->transformFeedbackDraw = true;
            break;
         }
         default:
         {
            break;
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                          VkFormat format,
                                          VkFormatProperties* pFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceFormatProperties(pdevice->dispatch_handle, format, pFormatProperties);

   if (pdevice->fake_textureCompressionBC && isBCFormat(format)) {
      pFormatProperties->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                                   VK_FORMAT_FEATURE_BLIT_SRC_BIT;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice,
                                             VkFormat format,
                                             VkImageType type,
                                             VkImageTiling tiling,
                                             VkImageUsageFlags usage,
                                             VkImageCreateFlags flags,
                                             VkImageFormatProperties* pImageFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   VkResult result = pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties(pdevice->dispatch_handle, format, type, tiling, usage, flags, pImageFormatProperties);

   if (pdevice->fake_textureCompressionBC && isBCFormat(format)) {
      pImageFormatProperties->maxExtent.width = 16384;
      pImageFormatProperties->maxExtent.height = 16384;
      pImageFormatProperties->maxExtent.depth = 1;
      pImageFormatProperties->maxMipLevels = 15;
      pImageFormatProperties->maxArrayLayers = 2048;
      pImageFormatProperties->sampleCounts = 1;
      result = VK_SUCCESS;
   }

   return result;
}
