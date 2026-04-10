// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/*
 * GDRcopy stub library — provides no-op implementations of GDR functions
 * for systems where gdrdrv kernel module is not available.
 *
 * Usage: LD_PRELOAD=/path/to/libgdr_stub.so ./cuphycontroller_scf
 *
 * This stubs out gdr_open, gdr_pin_buffer, gdr_map, gdr_unmap, gdr_unpin_buffer
 * so the cuphydriver initializes without the gdrdrv kernel module.
 * GPU-direct RDMA won't work but regular CUDA memcpy paths will be used.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>

extern "C" {

/* Minimal GDR types matching libgdrapi */
typedef void* gdr_t;
typedef unsigned long gdr_mh_t;

/* gdr_open: return a non-null dummy handle */
gdr_t gdr_open(void)
{
    static int warned = 0;
    if (!warned) {
        fprintf(stderr, "GDR_STUB: gdr_open() — returning stub handle (no GPU-direct RDMA)\n");
        warned = 1;
    }
    /* Return a non-null sentinel value so callers don't think it failed */
    return (gdr_t)(uintptr_t)0xDEADBEEF;
}

/* gdr_close: no-op */
int gdr_close(gdr_t g)
{
    (void)g;
    return 0;
}

/* gdr_pin_buffer: pretend to pin — just return success and a dummy handle */
int gdr_pin_buffer(gdr_t g, unsigned long addr, unsigned long size,
                   unsigned long p2p_token, unsigned int va_space,
                   gdr_mh_t *handle)
{
    (void)g; (void)addr; (void)size; (void)p2p_token; (void)va_space;
    if (handle) *handle = 0x1;
    return 0;
}

/* gdr_unpin_buffer: no-op */
int gdr_unpin_buffer(gdr_t g, gdr_mh_t handle)
{
    (void)g; (void)handle;
    return 0;
}

/* gdr_map: allocate raw anonymous mmap memory.
   gpinned_buffer will then cudaHostRegister this memory.
   We use MAP_ANONYMOUS (not CUDA alloc) so cudaHostRegister succeeds. */
int gdr_map(gdr_t g, gdr_mh_t handle, void **va, unsigned long size)
{
    (void)g; (void)handle;
    if (va) {
        size_t alloc_size = size > 0 ? size : 4096;
        /* Page-align the allocation */
        alloc_size = (alloc_size + 4095) & ~4095UL;
        *va = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (*va == MAP_FAILED) {
            *va = NULL;
            return -1;
        }
        memset(*va, 0, alloc_size);
    }
    return 0;
}

/* gdr_unmap: unmap the anonymous memory */
int gdr_unmap(gdr_t g, gdr_mh_t handle, void *va, unsigned long size)
{
    (void)g; (void)handle;
    if (va) {
        munmap(va, size > 0 ? size : 4096);
    }
    return 0;
}

/* gdr_copy_to_mapping / gdr_copy_from_mapping: use regular memcpy */
int gdr_copy_to_mapping(gdr_mh_t handle, void *map_d_ptr,
                        const void *h_ptr, unsigned long size)
{
    (void)handle;
    memcpy(map_d_ptr, h_ptr, size);
    return 0;
}

int gdr_copy_from_mapping(gdr_mh_t handle, void *h_ptr,
                          const void *map_d_ptr, unsigned long size)
{
    (void)handle;
    memcpy(h_ptr, map_d_ptr, size);
    return 0;
}

/* gdr_get_info: return dummy info */
int gdr_get_info(gdr_t g, gdr_mh_t handle, void *info)
{
    (void)g; (void)handle;
    if (info) memset(info, 0, 64);
    return 0;
}

/* gdr_get_info_v2: same as gdr_get_info */
int gdr_get_info_v2(gdr_t g, gdr_mh_t handle, void *info)
{
    (void)g; (void)handle;
    if (info) memset(info, 0, 128);
    return 0;
}

} /* extern "C" */
