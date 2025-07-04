/*
 * Copyright © 2009 Corbin Simpson
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMDGPU_WINSYS_H
#define AMDGPU_WINSYS_H

#include "pipebuffer/pb_cache.h"
#include "pipebuffer/pb_slab.h"
#include "winsys/radeon_winsys.h"
#include "util/simple_mtx.h"
#include "util/u_queue.h"
#include "ac_linux_drm.h"
#include <amdgpu.h>
#include "amdgpu_userq.h"

struct amdgpu_cs;

/* DRM file descriptors, file descriptions and buffer sharing.
 *
 * amdgpu_device_initialize() creates one amdgpu_device_handle for one
 * gpu. It does this by getting sysfs path(eg /dev/dri/cardxx) for the fd.
 * It uses the sysfs path to return the amdgpu_device_handle if already created
 * or to create new one.
 *
 * Thus amdgpu_device_handle's fd will be from the first time the gpu
 * was initialized by amdgpu_device_initialize().
 *
 * KMS/GEM buffer handles are specific to a DRM file description. i.e. the
 * same handle value may refer to different underlying BOs in different
 * DRM file descriptions even for the same gpu. The
 * https://en.wikipedia.org/wiki/File:File_table_and_inode_table.svg diagram shows
 * the file descriptors and its relation to file descriptions in the file table.
 *
 * The fd's are considered different if the fd's are obtained using open()
 * function. The fd's that are duplicates(using dup() or fcntl F_DUPFD) of
 * open fd, all will be same when compared with os_same_file_description()
 * function which uses kcmp system call.
 *
 * amdgpu_screen_winsys's fd tracks the file description which was
 * given to amdgpu_winsys_create(). This is the fd used by the application
 * using the driver and may be used in other ioctl (eg: drmModeAddFB)
 *
 * amdgpu_winsys's fd is the file description used to initialize the
 * device handle in libdrm_amdgpu.
 *
 * The 2 fds can be different, even in systems with a single GPU, eg: if
 * radv is initialized before radeonsi.
 *
 * This fd tracking is useful for buffer sharing. As an example, if an app
 * wants to use drmModeAddFB it'll need a KMS handle valid for its
 * fd (== amdgpu_screen_winsys::fd). If both fds are identical, there's
 * nothing to do: bo->u.real.kms_handle can be used directly
 * (see amdgpu_bo_get_handle). If they're different, the BO has to be exported
 * from the device fd as a dma-buf, then imported to the app fd to get the
 * KMS handle of the buffer for that app fd.
 *
 * Examples:
 * 1) OpenGL, then VAAPI:
 *    OpenGL                             | VAAPI (same device, != file description)
 *    -----------------------------------│-----------------------------------------
 *    fd = 5 (/dev/dri/renderD128)       │fd = 9 (/dev/dri/renderD128')
 *          │                            │       │
 *     device_handle = 0xffff0250        │ device_handle = 0xffff0250 (fd=5, re-used)
 *          │                            │       │
 *    amdgpu_screen_winsys = 0xffff0120  │amdgpu_winsys = 0xffff0470  ◄─────────────┐
 *          │   ├─ fd = dup(5) = 6       │       │   └─ sws_list = 0xffff0120       │
 *          │   └─ aws = 0xffff0470 ◄──┐ │       │                 0xffff0640 ◄───┐ │
 *          │                          │ │amdgpu_screen_winsys = 0xffff0640 ──────┘ │
 *    amdgpu_winsys = 0xffff0470    ───┘ │           └─ fd = dup(9) = 10            │
 *          │   ├─ dev = 0xffff0250      │                                          │
 *          │   ├─ sws_list = 0xffff0120 │                                          │
 *          │   └─ fd = 6                │                                          │
 *    dev_tab(0xffff0250) = 0xffff0470 ──│──────────────────────────────────────────┘
 *
 * 2) Vulkan (fd=5) then OpenGL (same device, != file description):
 *    -----------------------------
 *    fd = 9 (/dev/dri/renderD128)
 *           │
 *     device_handle = 0xffff0250 (fd=5, re-used)
 *           │
 *    amdgpu_screen_winsys = 0xffff0740
 *           │   ├─ fd = dup(9) = 10
 *           │   └─ aws = 0xffff0940 ◄───┐
 *    amdgpu_winsys = 0xffff0940 ────────┘
 *           │   ├─ dev = 0xffff0250
 *           │   ├─ sws_list = 0xffff0740
 *           │   └─ fd = 5
 *    dev_tab(0xffff0250) = 0xffff0940
 */

/* One struct amdgpu_screen_winsys is created in amdgpu_winsys_create() for one
 * fd. For fd's that are same (read above description for same if condition),
 * already created amdgpu_screen_winsys will be returned.
 */
struct amdgpu_screen_winsys {
   struct radeon_winsys base;
   struct amdgpu_winsys *aws;
   /* See comment above */
   int fd;
   struct pipe_reference reference;
   struct amdgpu_screen_winsys *next;

   /* Maps a BO to its KMS handle valid for this DRM file descriptor
    * Protected by amdgpu_winsys::sws_list_lock
    */
   struct hash_table *kms_handles;
};

/* Maximum this number of IBs can be busy per queue. When submitting a new IB and the oldest IB
 * ("AMDGPU_FENCE_RING_SIZE" IBs ago) is still busy, the CS thread will wait for it and will
 * also block all queues from submitting new IBs.
 */
#define AMDGPU_FENCE_RING_SIZE 32

/* The maximum number of queues that can be present. */
#define AMDGPU_MAX_QUEUES 6

/* This can use any integer type because the logic handles integer wraparounds robustly, but
 * uint8_t wraps around so quickly that some BOs might never become idle because we don't
 * remove idle fences from BOs, so they become "busy" again after a queue sequence number wraps
 * around and they may stay "busy" in pb_cache long enough that we run out of memory.
 *
 * High FPS applications also wrap around uint16_t so quickly that 32-bit address space allocations
 * aren't deallocated soon enough and we run out.
 */
typedef uint32_t uint_seq_no;

struct amdgpu_queue {
   /* Ring buffer of fences.
    *
    * We only remember a certain number of the most recent fences per queue. When we add a new
    * fence, we wait for the oldest one, which implies that all older fences not present
    * in the ring are idle. This way we don't have to keep track of a million fence references
    * for a million BOs.
    *
    * We only support 1 queue per IP. If an IP has multiple queues, we always add a fence
    * dependency on the previous fence to make it behave like there is only 1 queue.
    *
    * amdgpu_winsys_bo doesn't have a list of fences. It only remembers the last sequence number
    * for every queue where it was used. We then use the BO's sequence number to look up a fence
    * in this ring.
    */
   struct pipe_fence_handle *fences[AMDGPU_FENCE_RING_SIZE];

   /* The sequence number of the latest fence.
    *
    * This sequence number is global per queue per device, shared by all contexts, and generated
    * by the winsys, not the kernel.
    *
    * The latest fence is: fences[latest_seq_no % AMDGPU_FENCE_RING_SIZE]
    * The oldest fence is: fences([latest_seq_no + 1) % AMDGPU_FENCE_RING_SIZE]
    * The oldest sequence number in the ring: latest_seq_no - AMDGPU_FENCE_RING_SIZE + 1
    *
    * The sequence number is in the ring if:
    *    latest_seq_no - buffer_seq_no < AMDGPU_FENCE_RING_SIZE
    * If the sequence number is not in the ring, it's idle.
    *
    * Integer wraparounds of the sequence number behave as follows:
    *
    * The comparison above gives the correct answer if buffer_seq_no isn't older than UINT*_MAX.
    * If it's older than UINT*_MAX but not older than UINT*_MAX + AMDGPU_FENCE_RING_SIZE, we
    * incorrectly pick and wait for one of the fences in the ring. That's only a problem when
    * the type is so small (uint8_t) that seq_no wraps around very frequently, causing BOs to
    * never become idle in certain very unlucky scenarios and running out of memory.
    */
   uint_seq_no latest_seq_no;

   /* The last context using this queue. */
   struct amdgpu_ctx *last_ctx;

   struct amdgpu_userq userq;
};

/* This is part of every BO. */
struct amdgpu_seq_no_fences {
   /* A fence sequence number per queue. This number is used to look up the fence from
    * struct amdgpu_queue.
    *
    * This sequence number is global per queue per device, shared by all contexts, and generated
    * by the winsys, not the kernel.
    */
   uint_seq_no seq_no[AMDGPU_MAX_QUEUES];

   /* The mask of queues where seq_no[i] is valid. */
   uint8_t valid_fence_mask;
};

/* valid_fence_mask should have 1 bit for each queue. */
static_assert(sizeof(((struct amdgpu_seq_no_fences*)NULL)->valid_fence_mask) * 8 >= AMDGPU_MAX_QUEUES, "");

/* One struct amdgpu_winsys is created for one gpu in amdgpu_winsys_create(). */
struct amdgpu_winsys {
   struct pipe_reference reference;
   /* See comment above */
   int fd;

   /* Protected by bo_fence_lock. */
   struct amdgpu_queue queues[AMDGPU_MAX_QUEUES];

   struct pb_cache bo_cache;
   struct pb_slabs bo_slabs;  /* Slab allocator. */

   ac_drm_device *dev;

   simple_mtx_t bo_fence_lock;

   int num_cs; /* The number of command streams created. */
   uint32_t surf_index_color;
   uint32_t surf_index_fmask;
   uint32_t next_bo_unique_id;
   uint64_t allocated_vram;
   uint64_t allocated_gtt;
   uint64_t mapped_vram;
   uint64_t mapped_gtt;
   uint64_t slab_wasted_vram;
   uint64_t slab_wasted_gtt;
   uint64_t buffer_wait_time; /* time spent in buffer_wait in ns */
   uint64_t num_gfx_IBs;
   uint64_t num_sdma_IBs;
   uint64_t num_mapped_buffers;
   uint64_t gfx_bo_list_counter;
   uint64_t gfx_ib_size_counter;

   struct radeon_info info;

   /* multithreaded IB submission */
   struct util_queue cs_queue;

   struct ac_addrlib *addrlib;

   bool check_vm;
   bool noop_cs;
   bool reserve_vmid;
   bool zero_all_vram_allocs;
#if MESA_DEBUG
   bool debug_all_bos;

   /* List of all allocated buffers */
   simple_mtx_t global_bo_list_lock;
   struct list_head global_bo_list;
   unsigned num_buffers;
#endif

   /* Single-linked list of all structs amdgpu_screen_winsys referencing this
    * struct amdgpu_winsys
    */
   simple_mtx_t sws_list_lock;
   struct amdgpu_screen_winsys *sws_list;

   /* For returning the same amdgpu_winsys_bo instance for exported
    * and re-imported buffers. */
   struct hash_table *bo_export_table;
   simple_mtx_t bo_export_table_lock;

   /* Since most winsys functions require struct radeon_winsys *, dummy_sws.base is used
    * for invoking them because sws_list can be NULL.
    */
   struct amdgpu_screen_winsys dummy_sws;

   /*
    * In case of userqueue, mesa should ensure that VM page tables are available
    * when jobs are executed. For this, VM ioctl now outputs timeline syncobj.
    * This timeline syncobj output will be used as one of the dependency
    * fence in userqueue wait ioctl.
    */
   uint32_t vm_timeline_syncobj;
   uint64_t vm_timeline_seq_num;
   simple_mtx_t vm_ioctl_lock;
};

static inline struct amdgpu_screen_winsys *
amdgpu_screen_winsys(struct radeon_winsys *base)
{
   return (struct amdgpu_screen_winsys*)base;
}

static inline struct amdgpu_winsys *
amdgpu_winsys(struct radeon_winsys *base)
{
   return amdgpu_screen_winsys(base)->aws;
}

void amdgpu_surface_init_functions(struct amdgpu_screen_winsys *sws);

#endif
