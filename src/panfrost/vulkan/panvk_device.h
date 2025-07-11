/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_DEVICE_H
#define PANVK_DEVICE_H

#include <stdint.h>

#include "vk_debug_utils.h"
#include "vk_device.h"
#include "vk_meta.h"

#include "panvk_blend.h"
#include "panvk_instance.h"
#include "panvk_macros.h"
#include "panvk_mempool.h"
#include "panvk_meta.h"
#include "panvk_physical_device.h"
#include "panvk_utrace_perfetto.h"

#include "kmod/pan_kmod.h"
#include "util/pan_ir.h"
#include "util/perf/u_trace.h"

#include "util/u_printf.h"
#include "util/vma.h"

#define PANVK_MAX_QUEUE_FAMILIES 1

struct panvk_precomp_cache;
struct panvk_device_draw_context;

struct panvk_device {
   struct vk_device vk;

   struct {
      simple_mtx_t lock;
      struct util_vma_heap heap;
   } as;

   struct {
      struct pan_kmod_vm *vm;
      struct pan_kmod_dev *dev;
      struct pan_kmod_allocator allocator;
   } kmod;

   struct panvk_priv_bo *tiler_heap;
   struct panvk_priv_bo *sample_positions;

   struct {
      struct panvk_priv_bo *handlers_bo;
      uint32_t handler_stride;
   } tiler_oom;

   struct vk_meta_device meta;

   struct {
      struct panvk_pool rw;
      struct panvk_pool rw_nc;
      struct panvk_pool exec;
   } mempools;

   /* For each subqueue, maximum size of the register dump region needed by
    * exception handlers or functions */
   uint32_t *dump_region_size;

   struct vk_device_dispatch_table cmd_dispatch;

   struct panvk_queue *queues[PANVK_MAX_QUEUE_FAMILIES];
   int queue_count[PANVK_MAX_QUEUE_FAMILIES];

   struct panvk_precomp_cache *precomp_cache;

   struct {
      struct u_trace_context utctx;
#ifdef HAVE_PERFETTO
      struct panvk_utrace_perfetto utp;
#endif
   } utrace;

   struct panvk_device_draw_context* draw_ctx;

   struct {
      struct pandecode_context *decode_ctx;
   } debug;

   struct {
      struct u_printf_ctx ctx;
      struct panvk_priv_bo *bo;
   } printf;

   int drm_fd;
};

VK_DEFINE_HANDLE_CASTS(panvk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

static inline struct panvk_device *
to_panvk_device(struct vk_device *dev)
{
   return container_of(dev, struct panvk_device, vk);
}

static inline uint32_t
panvk_device_adjust_bo_flags(const struct panvk_device *device,
                             uint32_t bo_flags)
{
   struct panvk_instance *instance =
      to_panvk_instance(device->vk.physical->instance);

   if (instance->debug_flags & (PANVK_DEBUG_DUMP | PANVK_DEBUG_TRACE))
      bo_flags &= ~PAN_KMOD_BO_FLAG_NO_MMAP;

   return bo_flags;
}

#if PAN_ARCH
VkResult
panvk_per_arch(create_device)(struct panvk_physical_device *physical_device,
                              const VkDeviceCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDevice *pDevice);

void panvk_per_arch(destroy_device)(struct panvk_device *device,
                                    const VkAllocationCallbacks *pAllocator);

static inline VkResult
panvk_common_check_status(struct panvk_device *dev)
{
   return vk_check_printf_status(&dev->vk, &dev->printf.ctx);
}

VkResult panvk_per_arch(device_check_status)(struct vk_device *vk_dev);

#if PAN_ARCH >= 10
VkResult panvk_per_arch(init_tiler_oom)(struct panvk_device *device);
#endif
#endif

#endif
