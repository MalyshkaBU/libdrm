/*
 * Copyright © 2011 Red Hat All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS, AUTHORS
 * AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 */
/*
 * Authors:
 *      Jérôme Glisse <jglisse@redhat.com>
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "drm.h"
#include "xf86drm.h"
#include "radeon_drm.h"
#include "radeon_surface.h"

#define ALIGN(value, alignment) (((value) + alignment - 1) & ~(alignment - 1))
#define MAX2(A, B)              ((A) > (B) ? (A) : (B))
#define MIN2(A, B)              ((A) < (B) ? (A) : (B))

/* keep this private */
enum radeon_family {
    CHIP_UNKNOWN,
    CHIP_R600,
    CHIP_RV610,
    CHIP_RV630,
    CHIP_RV670,
    CHIP_RV620,
    CHIP_RV635,
    CHIP_RS780,
    CHIP_RS880,
    CHIP_RV770,
    CHIP_RV730,
    CHIP_RV710,
    CHIP_RV740,
    CHIP_CEDAR,
    CHIP_REDWOOD,
    CHIP_JUNIPER,
    CHIP_CYPRESS,
    CHIP_HEMLOCK,
    CHIP_PALM,
    CHIP_SUMO,
    CHIP_SUMO2,
    CHIP_BARTS,
    CHIP_TURKS,
    CHIP_CAICOS,
    CHIP_CAYMAN,
    CHIP_ARUBA,
    CHIP_TAHITI,
    CHIP_PITCAIRN,
    CHIP_VERDE,
    CHIP_OLAND,
    CHIP_LAST,
};

typedef int (*hw_init_surface_t)(struct radeon_surface_manager *surf_man,
                                 struct radeon_surface *surf);
typedef int (*hw_best_surface_t)(struct radeon_surface_manager *surf_man,
                                 struct radeon_surface *surf);

struct radeon_hw_info {
    /* apply to r6, eg */
    uint32_t                    group_bytes;
    uint32_t                    num_banks;
    uint32_t                    num_pipes;
    /* apply to eg */
    uint32_t                    row_size;
    unsigned                    allow_2d;
};

struct radeon_surface_manager {
    int                         fd;
    uint32_t                    device_id;
    struct radeon_hw_info       hw_info;
    unsigned                    family;
    hw_init_surface_t           surface_init;
    hw_best_surface_t           surface_best;
};

/* helper */
static int radeon_get_value(int fd, unsigned req, uint32_t *value)
{
    struct drm_radeon_info info = {};
    int r;

    *value = 0;
    info.request = req;
    info.value = (uintptr_t)value;
    r = drmCommandWriteRead(fd, DRM_RADEON_INFO, &info,
                            sizeof(struct drm_radeon_info));
    return r;
}

static int radeon_get_family(struct radeon_surface_manager *surf_man)
{
    switch (surf_man->device_id) {
#define CHIPSET(pci_id, name, fam) case pci_id: surf_man->family = CHIP_##fam; break;
#include "r600_pci_ids.h"
#undef CHIPSET
    default:
        return -EINVAL;
    }
    return 0;
}

static unsigned next_power_of_two(unsigned x)
{
   if (x <= 1)
       return 1;

   return (1 << ((sizeof(unsigned) * 8) - __builtin_clz(x - 1)));
}

static unsigned mip_minify(unsigned size, unsigned level)
{
    unsigned val;

    val = MAX2(1, size >> level);
    if (level > 0)
        val = next_power_of_two(val);
    return val;
}

static void surf_minify(struct radeon_surface *surf,
                        struct radeon_surface_level *surflevel,
                        unsigned bpe, unsigned level,
                        uint32_t xalign, uint32_t yalign, uint32_t zalign,
                        unsigned offset)
{
    surflevel->npix_x = mip_minify(surf->npix_x, level);
    surflevel->npix_y = mip_minify(surf->npix_y, level);
    surflevel->npix_z = mip_minify(surf->npix_z, level);
    surflevel->nblk_x = (surflevel->npix_x + surf->blk_w - 1) / surf->blk_w;
    surflevel->nblk_y = (surflevel->npix_y + surf->blk_h - 1) / surf->blk_h;
    surflevel->nblk_z = (surflevel->npix_z + surf->blk_d - 1) / surf->blk_d;
    if (surf->nsamples == 1 && surflevel->mode == RADEON_SURF_MODE_2D) {
        if (surflevel->nblk_x < xalign || surflevel->nblk_y < yalign) {
            surflevel->mode = RADEON_SURF_MODE_1D;
            return;
        }
    }
    surflevel->nblk_x  = ALIGN(surflevel->nblk_x, xalign);
    surflevel->nblk_y  = ALIGN(surflevel->nblk_y, yalign);
    surflevel->nblk_z  = ALIGN(surflevel->nblk_z, zalign);

    surflevel->offset = offset;
    surflevel->pitch_bytes = surflevel->nblk_x * bpe * surf->nsamples;
    surflevel->slice_size = surflevel->pitch_bytes * surflevel->nblk_y;

    surf->bo_size = offset + surflevel->slice_size * surflevel->nblk_z * surf->array_size;
}

/* ===========================================================================
 * r600/r700 family
 */
static int r6_init_hw_info(struct radeon_surface_manager *surf_man)
{
    uint32_t tiling_config;
    drmVersionPtr version;
    int r;

    r = radeon_get_value(surf_man->fd, RADEON_INFO_TILING_CONFIG,
                         &tiling_config);
    if (r) {
        return r;
    }

    surf_man->hw_info.allow_2d = 0;
    version = drmGetVersion(surf_man->fd);
    if (version && version->version_minor >= 14) {
        surf_man->hw_info.allow_2d = 1;
    }
    drmFreeVersion(version);

    switch ((tiling_config & 0xe) >> 1) {
    case 0:
        surf_man->hw_info.num_pipes = 1;
        break;
    case 1:
        surf_man->hw_info.num_pipes = 2;
        break;
    case 2:
        surf_man->hw_info.num_pipes = 4;
        break;
    case 3:
        surf_man->hw_info.num_pipes = 8;
        break;
    default:
        surf_man->hw_info.num_pipes = 8;
        surf_man->hw_info.allow_2d = 0;
        break;
    }

    switch ((tiling_config & 0x30) >> 4) {
    case 0:
        surf_man->hw_info.num_banks = 4;
        break;
    case 1:
        surf_man->hw_info.num_banks = 8;
        break;
    default:
        surf_man->hw_info.num_banks = 8;
        surf_man->hw_info.allow_2d = 0;
        break;
    }

    switch ((tiling_config & 0xc0) >> 6) {
    case 0:
        surf_man->hw_info.group_bytes = 256;
        break;
    case 1:
        surf_man->hw_info.group_bytes = 512;
        break;
    default:
        surf_man->hw_info.group_bytes = 256;
        surf_man->hw_info.allow_2d = 0;
        break;
    }
    return 0;
}

static int r6_surface_init_linear(struct radeon_surface_manager *surf_man,
                                  struct radeon_surface *surf,
                                  uint64_t offset, unsigned start_level)
{
    uint32_t xalign, yalign, zalign;
    unsigned i;

    /* compute alignment */
    if (!start_level) {
        surf->bo_alignment = MAX2(256, surf_man->hw_info.group_bytes);
    }
    /* the 32 alignment is for scanout, cb or db but to allow texture to be
     * easily bound as such we force this alignment to all surface
     */
    xalign = MAX2(1, surf_man->hw_info.group_bytes / surf->bpe);
    yalign = 1;
    zalign = 1;
    if (surf->flags & RADEON_SURF_SCANOUT) {
        xalign = MAX2((surf->bpe == 1) ? 64 : 32, xalign);
    }

    /* build mipmap tree */
    for (i = start_level; i <= surf->last_level; i++) {
        surf->level[i].mode = RADEON_SURF_MODE_LINEAR;
        surf_minify(surf, surf->level+i, surf->bpe, i, xalign, yalign, zalign, offset);
        /* level0 and first mipmap need to have alignment */
        offset = surf->bo_size;
        if ((i == 0)) {
            offset = ALIGN(offset, surf->bo_alignment);
        }
    }
    return 0;
}

static int r6_surface_init_linear_aligned(struct radeon_surface_manager *surf_man,
                                          struct radeon_surface *surf,
                                          uint64_t offset, unsigned start_level)
{
    uint32_t xalign, yalign, zalign;
    unsigned i;

    /* compute alignment */
    if (!start_level) {
        surf->bo_alignment = MAX2(256, surf_man->hw_info.group_bytes);
    }
    xalign = MAX2(64, surf_man->hw_info.group_bytes / surf->bpe);
    yalign = 1;
    zalign = 1;

    /* build mipmap tree */
    for (i = start_level; i <= surf->last_level; i++) {
        surf->level[i].mode = RADEON_SURF_MODE_LINEAR_ALIGNED;
        surf_minify(surf, surf->level+i, surf->bpe, i, xalign, yalign, zalign, offset);
        /* level0 and first mipmap need to have alignment */
        offset = surf->bo_size;
        if ((i == 0)) {
            offset = ALIGN(offset, surf->bo_alignment);
        }
    }
    return 0;
}

static int r6_surface_init_1d(struct radeon_surface_manager *surf_man,
                              struct radeon_surface *surf,
                              uint64_t offset, unsigned start_level)
{
    uint32_t xalign, yalign, zalign, tilew;
    unsigned i;

    /* compute alignment */
    tilew = 8;
    xalign = surf_man->hw_info.group_bytes / (tilew * surf->bpe * surf->nsamples);
    xalign = MAX2(tilew, xalign);
    yalign = tilew;
    zalign = 1;
    if (surf->flags & RADEON_SURF_SCANOUT) {
        xalign = MAX2((surf->bpe == 1) ? 64 : 32, xalign);
    }
    if (!start_level) {
        surf->bo_alignment = MAX2(256, surf_man->hw_info.group_bytes);
    }

    /* build mipmap tree */
    for (i = start_level; i <= surf->last_level; i++) {
        surf->level[i].mode = RADEON_SURF_MODE_1D;
        surf_minify(surf, surf->level+i, surf->bpe, i, xalign, yalign, zalign, offset);
        /* level0 and first mipmap need to have alignment */
        offset = surf->bo_size;
        if ((i == 0)) {
            offset = ALIGN(offset, surf->bo_alignment);
        }
    }
    return 0;
}

static int r6_surface_init_2d(struct radeon_surface_manager *surf_man,
                              struct radeon_surface *surf,
                              uint64_t offset, unsigned start_level)
{
    uint32_t xalign, yalign, zalign, tilew;
    unsigned i;

    /* compute alignment */
    tilew = 8;
    zalign = 1;
    xalign = (surf_man->hw_info.group_bytes * surf_man->hw_info.num_banks) /
             (tilew * surf->bpe * surf->nsamples);
    xalign = MAX2(tilew * surf_man->hw_info.num_banks, xalign);
    yalign = tilew * surf_man->hw_info.num_pipes;
    if (surf->flags & RADEON_SURF_SCANOUT) {
        xalign = MAX2((surf->bpe == 1) ? 64 : 32, xalign);
    }
    if (!start_level) {
        surf->bo_alignment =
            MAX2(surf_man->hw_info.num_pipes *
                 surf_man->hw_info.num_banks *
                 surf->nsamples * surf->bpe * 64,
                 xalign * yalign * surf->nsamples * surf->bpe);
    }

    /* build mipmap tree */
    for (i = start_level; i <= surf->last_level; i++) {
        surf->level[i].mode = RADEON_SURF_MODE_2D;
        surf_minify(surf, surf->level+i, surf->bpe, i, xalign, yalign, zalign, offset);
        if (surf->level[i].mode == RADEON_SURF_MODE_1D) {
            return r6_surface_init_1d(surf_man, surf, offset, i);
        }
        /* level0 and first mipmap need to have alignment */
        offset = surf->bo_size;
        if ((i == 0)) {
            offset = ALIGN(offset, surf->bo_alignment);
        }
    }
    return 0;
}

static int r6_surface_init(struct radeon_surface_manager *surf_man,
                           struct radeon_surface *surf)
{
    unsigned mode;
    int r;

    /* MSAA surfaces support the 2D mode only. */
    if (surf->nsamples > 1) {
        surf->flags = RADEON_SURF_CLR(surf->flags, MODE);
        surf->flags |= RADEON_SURF_SET(RADEON_SURF_MODE_2D, MODE);
    }

    /* tiling mode */
    mode = (surf->flags >> RADEON_SURF_MODE_SHIFT) & RADEON_SURF_MODE_MASK;

    if (surf->flags & (RADEON_SURF_ZBUFFER | RADEON_SURF_SBUFFER)) {
        /* zbuffer only support 1D or 2D tiled surface */
        switch (mode) {
        case RADEON_SURF_MODE_1D:
        case RADEON_SURF_MODE_2D:
            break;
        default:
            mode = RADEON_SURF_MODE_1D;
            surf->flags = RADEON_SURF_CLR(surf->flags, MODE);
            surf->flags |= RADEON_SURF_SET(RADEON_SURF_MODE_1D, MODE);
            break;
        }
    }

    /* force 1d on kernel that can't do 2d */
    if (!surf_man->hw_info.allow_2d && mode > RADEON_SURF_MODE_1D) {
        if (surf->nsamples > 1) {
            fprintf(stderr, "radeon: Cannot use 2D tiling for an MSAA surface (%i).\n", __LINE__);
            return -EFAULT;
        }
        mode = RADEON_SURF_MODE_1D;
        surf->flags = RADEON_SURF_CLR(surf->flags, MODE);
        surf->flags |= RADEON_SURF_SET(mode, MODE);
    }

    /* check surface dimension */
    if (surf->npix_x > 8192 || surf->npix_y > 8192 || surf->npix_z > 8192) {
        return -EINVAL;
    }

    /* check mipmap last_level */
    if (surf->last_level > 14) {
        return -EINVAL;
    }

    /* check tiling mode */
    switch (mode) {
    case RADEON_SURF_MODE_LINEAR:
        r = r6_surface_init_linear(surf_man, surf, 0, 0);
        break;
    case RADEON_SURF_MODE_LINEAR_ALIGNED:
        r = r6_surface_init_linear_aligned(surf_man, surf, 0, 0);
        break;
    case RADEON_SURF_MODE_1D:
        r = r6_surface_init_1d(surf_man, surf, 0, 0);
        break;
    case RADEON_SURF_MODE_2D:
        r = r6_surface_init_2d(surf_man, surf, 0, 0);
        break;
    default:
        return -EINVAL;
    }
    return r;
}

static int r6_surface_best(struct radeon_surface_manager *surf_man,
                           struct radeon_surface *surf)
{
    /* no value to optimize for r6xx/r7xx */
    return 0;
}


/* ===========================================================================
 * evergreen family
 */
static int eg_init_hw_info(struct radeon_surface_manager *surf_man)
{
    uint32_t tiling_config;
    drmVersionPtr version;
    int r;

    r = radeon_get_value(surf_man->fd, RADEON_INFO_TILING_CONFIG,
                         &tiling_config);
    if (r) {
        return r;
    }

    surf_man->hw_info.allow_2d = 0;
    version = drmGetVersion(surf_man->fd);
    if (version && version->version_minor >= 16) {
        surf_man->hw_info.allow_2d = 1;
    }
    drmFreeVersion(version);

    switch (tiling_config & 0xf) {
    case 0:
        surf_man->hw_info.num_pipes = 1;
        break;
    case 1:
        surf_man->hw_info.num_pipes = 2;
        break;
    case 2:
        surf_man->hw_info.num_pipes = 4;
        break;
    case 3:
        surf_man->hw_info.num_pipes = 8;
        break;
    default:
        surf_man->hw_info.num_pipes = 8;
        surf_man->hw_info.allow_2d = 0;
        break;
    }

    switch ((tiling_config & 0xf0) >> 4) {
    case 0:
        surf_man->hw_info.num_banks = 4;
        break;
    case 1:
        surf_man->hw_info.num_banks = 8;
        break;
    case 2:
        surf_man->hw_info.num_banks = 16;
        break;
    default:
        surf_man->hw_info.num_banks = 8;
        surf_man->hw_info.allow_2d = 0;
        break;
    }

    switch ((tiling_config & 0xf00) >> 8) {
    case 0:
        surf_man->hw_info.group_bytes = 256;
        break;
    case 1:
        surf_man->hw_info.group_bytes = 512;
        break;
    default:
        surf_man->hw_info.group_bytes = 256;
        surf_man->hw_info.allow_2d = 0;
        break;
    }

    switch ((tiling_config & 0xf000) >> 12) {
    case 0:
        surf_man->hw_info.row_size = 1024;
        break;
    case 1:
        surf_man->hw_info.row_size = 2048;
        break;
    case 2:
        surf_man->hw_info.row_size = 4096;
        break;
    default:
        surf_man->hw_info.row_size = 4096;
        surf_man->hw_info.allow_2d = 0;
        break;
    }
    return 0;
}

static void eg_surf_minify(struct radeon_surface *surf,
                           struct radeon_surface_level *surflevel,
                           unsigned bpe,
                           unsigned level,
                           unsigned slice_pt,
                           unsigned mtilew,
                           unsigned mtileh,
                           unsigned mtileb,
                           unsigned offset)
{
    unsigned mtile_pr, mtile_ps;

    surflevel->npix_x = mip_minify(surf->npix_x, level);
    surflevel->npix_y = mip_minify(surf->npix_y, level);
    surflevel->npix_z = mip_minify(surf->npix_z, level);
    surflevel->nblk_x = (surflevel->npix_x + surf->blk_w - 1) / surf->blk_w;
    surflevel->nblk_y = (surflevel->npix_y + surf->blk_h - 1) / surf->blk_h;
    surflevel->nblk_z = (surflevel->npix_z + surf->blk_d - 1) / surf->blk_d;
    if (surf->nsamples == 1 && surflevel->mode == RADEON_SURF_MODE_2D) {
        if (surflevel->nblk_x < mtilew || surflevel->nblk_y < mtileh) {
            surflevel->mode = RADEON_SURF_MODE_1D;
            return;
        }
    }
    surflevel->nblk_x  = ALIGN(surflevel->nblk_x, mtilew);
    surflevel->nblk_y  = ALIGN(surflevel->nblk_y, mtileh);
    surflevel->nblk_z  = ALIGN(surflevel->nblk_z, 1);

    /* macro tile per row */
    mtile_pr = surflevel->nblk_x / mtilew;
    /* macro tile per slice */
    mtile_ps = (mtile_pr * surflevel->nblk_y) / mtileh;

    surflevel->offset = offset;
    surflevel->pitch_bytes = surflevel->nblk_x * bpe * slice_pt;
    surflevel->slice_size = mtile_ps * mtileb * slice_pt;

    surf->bo_size = offset + surflevel->slice_size * surflevel->nblk_z * surf->array_size;
}

static int eg_surface_init_1d(struct radeon_surface_manager *surf_man,
                              struct radeon_surface *surf,
                              struct radeon_surface_level *level,
                              unsigned bpe,
                              uint64_t offset, unsigned start_level)
{
    uint32_t xalign, yalign, zalign, tilew;
    unsigned i;

    /* compute alignment */
    tilew = 8;
    xalign = surf_man->hw_info.group_bytes / (tilew * bpe * surf->nsamples);
    xalign = MAX2(tilew, xalign);
    yalign = tilew;
    zalign = 1;
    if (surf->flags & RADEON_SURF_SCANOUT) {
        xalign = MAX2((bpe == 1) ? 64 : 32, xalign);
    }

    if (!start_level) {
        unsigned alignment = MAX2(256, surf_man->hw_info.group_bytes);
        surf->bo_alignment = MAX2(surf->bo_alignment, alignment);

        if (offset) {
            offset = ALIGN(offset, alignment);
        }
    }

    /* build mipmap tree */
    for (i = start_level; i <= surf->last_level; i++) {
        level[i].mode = RADEON_SURF_MODE_1D;
        surf_minify(surf, level+i, bpe, i, xalign, yalign, zalign, offset);
        /* level0 and first mipmap need to have alignment */
        offset = surf->bo_size;
        if ((i == 0)) {
            offset = ALIGN(offset, surf->bo_alignment);
        }
    }
    return 0;
}

static int eg_surface_init_2d(struct radeon_surface_manager *surf_man,
                              struct radeon_surface *surf,
                              struct radeon_surface_level *level,
                              unsigned bpe, unsigned tile_split,
                              uint64_t offset, unsigned start_level)
{
    unsigned tilew, tileh, tileb;
    unsigned mtilew, mtileh, mtileb;
    unsigned slice_pt;
    unsigned i;

    /* compute tile values */
    tilew = 8;
    tileh = 8;
    tileb = tilew * tileh * bpe * surf->nsamples;
    /* slices per tile */
    slice_pt = 1;
    if (tileb > tile_split) {
        slice_pt = tileb / tile_split;
    }
    tileb = tileb / slice_pt;

    /* macro tile width & height */
    mtilew = (tilew * surf->bankw * surf_man->hw_info.num_pipes) * surf->mtilea;
    mtileh = (tileh * surf->bankh * surf_man->hw_info.num_banks) / surf->mtilea;
    /* macro tile bytes */
    mtileb = (mtilew / tilew) * (mtileh / tileh) * tileb;

    if (!start_level) {
        unsigned alignment = MAX2(256, mtileb);
        surf->bo_alignment = MAX2(surf->bo_alignment, alignment);

        if (offset) {
            offset = ALIGN(offset, alignment);
        }
    }

    /* build mipmap tree */
    for (i = start_level; i <= surf->last_level; i++) {
        level[i].mode = RADEON_SURF_MODE_2D;
        eg_surf_minify(surf, level+i, bpe, i, slice_pt, mtilew, mtileh, mtileb, offset);
        if (level[i].mode == RADEON_SURF_MODE_1D) {
            return eg_surface_init_1d(surf_man, surf, level, bpe, offset, i);
        }
        /* level0 and first mipmap need to have alignment */
        offset = surf->bo_size;
        if ((i == 0)) {
            offset = ALIGN(offset, surf->bo_alignment);
        }
    }
    return 0;
}

static int eg_surface_sanity(struct radeon_surface_manager *surf_man,
                             struct radeon_surface *surf,
                             unsigned mode)
{
    unsigned tileb;

    /* check surface dimension */
    if (surf->npix_x > 16384 || surf->npix_y > 16384 || surf->npix_z > 16384) {
        return -EINVAL;
    }

    /* check mipmap last_level */
    if (surf->last_level > 15) {
        return -EINVAL;
    }

    /* force 1d on kernel that can't do 2d */
    if (!surf_man->hw_info.allow_2d && mode > RADEON_SURF_MODE_1D) {
        if (surf->nsamples > 1) {
            fprintf(stderr, "radeon: Cannot use 2D tiling for an MSAA surface (%i).\n", __LINE__);
            return -EFAULT;
        }
        mode = RADEON_SURF_MODE_1D;
        surf->flags = RADEON_SURF_CLR(surf->flags, MODE);
        surf->flags |= RADEON_SURF_SET(mode, MODE);
    }

    /* check tile split */
    if (mode == RADEON_SURF_MODE_2D) {
        switch (surf->tile_split) {
        case 64:
        case 128:
        case 256:
        case 512:
        case 1024:
        case 2048:
        case 4096:
            break;
        default:
            return -EINVAL;
        }
        switch (surf->mtilea) {
        case 1:
        case 2:
        case 4:
        case 8:
            break;
        default:
            return -EINVAL;
        }
        /* check aspect ratio */
        if (surf_man->hw_info.num_banks < surf->mtilea) {
            return -EINVAL;
        }
        /* check bank width */
        switch (surf->bankw) {
        case 1:
        case 2:
        case 4:
        case 8:
            break;
        default:
            return -EINVAL;
        }
        /* check bank height */
        switch (surf->bankh) {
        case 1:
        case 2:
        case 4:
        case 8:
            break;
        default:
            return -EINVAL;
        }
        tileb = MIN2(surf->tile_split, 64 * surf->bpe * surf->nsamples);
        if ((tileb * surf->bankh * surf->bankw) < surf_man->hw_info.group_bytes) {
            return -EINVAL;
        }
    }

    return 0;
}

static int eg_surface_init_1d_miptrees(struct radeon_surface_manager *surf_man,
                                       struct radeon_surface *surf)
{
    unsigned zs_flags = RADEON_SURF_ZBUFFER | RADEON_SURF_SBUFFER;
    int r, is_depth_stencil = (surf->flags & zs_flags) == zs_flags;
    /* Old libdrm headers didn't have stencil_level in it. This prevents crashes. */
    struct radeon_surface_level tmp[RADEON_SURF_MAX_LEVEL];
    struct radeon_surface_level *stencil_level =
        (surf->flags & RADEON_SURF_HAS_SBUFFER_MIPTREE) ? surf->stencil_level : tmp;

    r = eg_surface_init_1d(surf_man, surf, surf->level, surf->bpe, 0, 0);
    if (r)
        return r;

    if (is_depth_stencil) {
        r = eg_surface_init_1d(surf_man, surf, stencil_level, 1,
                               surf->bo_size, 0);
        surf->stencil_offset = stencil_level[0].offset;
    }
    return r;
}

static int eg_surface_init_2d_miptrees(struct radeon_surface_manager *surf_man,
                                       struct radeon_surface *surf)
{
    unsigned zs_flags = RADEON_SURF_ZBUFFER | RADEON_SURF_SBUFFER;
    int r, is_depth_stencil = (surf->flags & zs_flags) == zs_flags;
    /* Old libdrm headers didn't have stencil_level in it. This prevents crashes. */
    struct radeon_surface_level tmp[RADEON_SURF_MAX_LEVEL];
    struct radeon_surface_level *stencil_level =
        (surf->flags & RADEON_SURF_HAS_SBUFFER_MIPTREE) ? surf->stencil_level : tmp;

    r = eg_surface_init_2d(surf_man, surf, surf->level, surf->bpe,
                           surf->tile_split, 0, 0);
    if (r)
        return r;

    if (is_depth_stencil) {
        r = eg_surface_init_2d(surf_man, surf, stencil_level, 1,
                               surf->stencil_tile_split, surf->bo_size, 0);
        surf->stencil_offset = stencil_level[0].offset;
    }
    return r;
}

static int eg_surface_init(struct radeon_surface_manager *surf_man,
                           struct radeon_surface *surf)
{
    unsigned mode;
    int r;

    /* MSAA surfaces support the 2D mode only. */
    if (surf->nsamples > 1) {
        surf->flags = RADEON_SURF_CLR(surf->flags, MODE);
        surf->flags |= RADEON_SURF_SET(RADEON_SURF_MODE_2D, MODE);
    }

    /* tiling mode */
    mode = (surf->flags >> RADEON_SURF_MODE_SHIFT) & RADEON_SURF_MODE_MASK;

    if (surf->flags & (RADEON_SURF_ZBUFFER | RADEON_SURF_SBUFFER)) {
        /* zbuffer only support 1D or 2D tiled surface */
        switch (mode) {
        case RADEON_SURF_MODE_1D:
        case RADEON_SURF_MODE_2D:
            break;
        default:
            mode = RADEON_SURF_MODE_1D;
            surf->flags = RADEON_SURF_CLR(surf->flags, MODE);
            surf->flags |= RADEON_SURF_SET(RADEON_SURF_MODE_1D, MODE);
            break;
        }
    }

    r = eg_surface_sanity(surf_man, surf, mode);
    if (r) {
        return r;
    }

    surf->stencil_offset = 0;
    surf->bo_alignment = 0;

    /* check tiling mode */
    switch (mode) {
    case RADEON_SURF_MODE_LINEAR:
        r = r6_surface_init_linear(surf_man, surf, 0, 0);
        break;
    case RADEON_SURF_MODE_LINEAR_ALIGNED:
        r = r6_surface_init_linear_aligned(surf_man, surf, 0, 0);
        break;
    case RADEON_SURF_MODE_1D:
        r = eg_surface_init_1d_miptrees(surf_man, surf);
        break;
    case RADEON_SURF_MODE_2D:
        r = eg_surface_init_2d_miptrees(surf_man, surf);
        break;
    default:
        return -EINVAL;
    }
    return r;
}

static unsigned log2_int(unsigned x)
{
    unsigned l;

    if (x < 2) {
        return 0;
    }
    for (l = 2; ; l++) {
        if ((unsigned)(1 << l) > x) {
            return l - 1;
        }
    }
    return 0;
}

/* compute best tile_split, bankw, bankh, mtilea
 * depending on surface
 */
static int eg_surface_best(struct radeon_surface_manager *surf_man,
                           struct radeon_surface *surf)
{
    unsigned mode, tileb, h_over_w;
    int r;

    /* tiling mode */
    mode = (surf->flags >> RADEON_SURF_MODE_SHIFT) & RADEON_SURF_MODE_MASK;

    /* set some default value to avoid sanity check choking on them */
    surf->tile_split = 1024;
    surf->bankw = 1;
    surf->bankh = 1;
    surf->mtilea = surf_man->hw_info.num_banks;
    tileb = MIN2(surf->tile_split, 64 * surf->bpe * surf->nsamples);
    for (; surf->bankh <= 8; surf->bankh *= 2) {
        if ((tileb * surf->bankh * surf->bankw) >= surf_man->hw_info.group_bytes) {
            break;
        }
    }
    if (surf->mtilea > 8) {
        surf->mtilea = 8;
    }

    r = eg_surface_sanity(surf_man, surf, mode);
    if (r) {
        return r;
    }

    if (mode != RADEON_SURF_MODE_2D) {
        /* nothing to do for non 2D tiled surface */
        return 0;
    }

    /* Tweak TILE_SPLIT for performance here. */
    if (surf->nsamples > 1) {
        if (surf->flags & (RADEON_SURF_ZBUFFER | RADEON_SURF_SBUFFER)) {
            switch (surf->nsamples) {
            case 2:
                surf->tile_split = 128;
                break;
            case 4:
                surf->tile_split = 128;
                break;
            case 8:
                surf->tile_split = 256;
                break;
            case 16: /* cayman only */
                surf->tile_split = 512;
                break;
            default:
                fprintf(stderr, "radeon: Wrong number of samples %i (%i)\n",
                        surf->nsamples, __LINE__);
                return -EINVAL;
            }
            surf->stencil_tile_split = 64;
        } else {
            /* tile split must be >= 256 for colorbuffer surfaces */
            surf->tile_split = MAX2(surf->nsamples * surf->bpe * 64, 256);
            if (surf->tile_split > 4096)
                surf->tile_split = 4096;
        }
    } else {
        /* set tile split to row size */
        surf->tile_split = surf_man->hw_info.row_size;
        surf->stencil_tile_split = surf_man->hw_info.row_size / 2;
    }

    /* bankw or bankh greater than 1 increase alignment requirement, not
     * sure if it's worth using smaller bankw & bankh to stick with 2D
     * tiling on small surface rather than falling back to 1D tiling.
     * Use recommanded value based on tile size for now.
     *
     * fmask buffer has different optimal value figure them out once we
     * use it.
     */
    if (surf->flags & RADEON_SURF_SBUFFER) {
        /* assume 1 bytes for stencil, we optimize for stencil as stencil
         * and depth shares surface values
         */
        tileb = MIN2(surf->tile_split, 64 * surf->nsamples);
    } else {
        tileb = MIN2(surf->tile_split, 64 * surf->bpe * surf->nsamples);
    }

    /* use bankw of 1 to minimize width alignment, might be interesting to
     * increase it for large surface
     */
    surf->bankw = 1;
    switch (tileb) {
    case 64:
        surf->bankh = 4;
        break;
    case 128:
    case 256:
        surf->bankh = 2;
        break;
    default:
        surf->bankh = 1;
        break;
    }
    /* double check the constraint */
    for (; surf->bankh <= 8; surf->bankh *= 2) {
        if ((tileb * surf->bankh * surf->bankw) >= surf_man->hw_info.group_bytes) {
            break;
        }
    }

    h_over_w = (((surf->bankh * surf_man->hw_info.num_banks) << 16) /
                (surf->bankw * surf_man->hw_info.num_pipes)) >> 16;
    surf->mtilea = 1 << (log2_int(h_over_w) >> 1);

    return 0;
}


/* ===========================================================================
 * Southern Islands family
 */

static void si_surf_minify(struct radeon_surface *surf,
			   struct radeon_surface_level *surflevel,
			   unsigned bpe, unsigned level,
			   uint32_t xalign, uint32_t yalign, uint32_t zalign, uint32_t slice_align,
			   unsigned offset)
{
    surflevel->npix_x = mip_minify(surf->npix_x, level);
    surflevel->npix_y = mip_minify(surf->npix_y, level);
    surflevel->npix_z = mip_minify(surf->npix_z, level);

    if (level == 0 && surf->last_level > 0) {
        surflevel->nblk_x = (next_power_of_two(surflevel->npix_x) + surf->blk_w - 1) / surf->blk_w;
        surflevel->nblk_y = (next_power_of_two(surflevel->npix_y) + surf->blk_h - 1) / surf->blk_h;
        surflevel->nblk_z = (next_power_of_two(surflevel->npix_z) + surf->blk_d - 1) / surf->blk_d;
    } else {
        surflevel->nblk_x = (surflevel->npix_x + surf->blk_w - 1) / surf->blk_w;
        surflevel->nblk_y = (surflevel->npix_y + surf->blk_h - 1) / surf->blk_h;
        surflevel->nblk_z = (surflevel->npix_z + surf->blk_d - 1) / surf->blk_d;
    }

    surflevel->nblk_y  = ALIGN(surflevel->nblk_y, yalign);

    /* XXX: Texture sampling uses unexpectedly large pitches in some cases,
     * these are just guesses for the rules behind those
     */
    if (level == 0 && surf->last_level == 0)
        /* Non-mipmap pitch padded to slice alignment */
        xalign = MAX2(xalign, slice_align / surf->bpe);
    else
        /* Small rows evenly distributed across slice */
        xalign = MAX2(xalign, slice_align / surf->bpe / surflevel->nblk_y);

    surflevel->nblk_x  = ALIGN(surflevel->nblk_x, xalign);
    surflevel->nblk_z  = ALIGN(surflevel->nblk_z, zalign);

    surflevel->offset = offset;
    surflevel->pitch_bytes = surflevel->nblk_x * surf->bpe * surf->nsamples;
    surflevel->slice_size = ALIGN(surflevel->pitch_bytes * surflevel->nblk_y, slice_align);

    surf->bo_size = offset + surflevel->slice_size * surflevel->nblk_z * surf->array_size;
}

static int si_surface_init_linear_aligned(struct radeon_surface_manager *surf_man,
                                          struct radeon_surface *surf,
                                          uint64_t offset, unsigned start_level)
{
    uint32_t xalign, yalign, zalign, slice_align;
    unsigned i;

    /* compute alignment */
    if (!start_level) {
        surf->bo_alignment = MAX2(256, surf_man->hw_info.group_bytes);
    }
    xalign = MAX2(8, 64 / surf->bpe);
    yalign = 1;
    zalign = 1;
    slice_align = MAX2(64 * surf->bpe, surf_man->hw_info.group_bytes);

    /* build mipmap tree */
    for (i = start_level; i <= surf->last_level; i++) {
        surf->level[i].mode = RADEON_SURF_MODE_LINEAR_ALIGNED;
        si_surf_minify(surf, surf->level+i, surf->bpe, i, xalign, yalign,
                       zalign, slice_align, offset);
        /* level0 and first mipmap need to have alignment */
        offset = surf->bo_size;
        if ((i == 0)) {
            offset = ALIGN(offset, surf->bo_alignment);
        }
    }
    return 0;
}

static int si_surface_init_1d(struct radeon_surface_manager *surf_man,
                              struct radeon_surface *surf,
                              struct radeon_surface_level *level,
                              unsigned bpe,
                              uint64_t offset, unsigned start_level)
{
    uint32_t xalign, yalign, zalign, slice_align;
    unsigned i;

    /* compute alignment */
    xalign = 8;
    yalign = 8;
    zalign = 1;
    slice_align = surf_man->hw_info.group_bytes;
    if (surf->flags & RADEON_SURF_SCANOUT) {
        xalign = MAX2((bpe == 1) ? 64 : 32, xalign);
    }

    if (!start_level) {
        unsigned alignment = MAX2(256, surf_man->hw_info.group_bytes);
        surf->bo_alignment = MAX2(surf->bo_alignment, alignment);

        if (offset) {
            offset = ALIGN(offset, alignment);
        }
    }

    /* build mipmap tree */
    for (i = start_level; i <= surf->last_level; i++) {
        level[i].mode = RADEON_SURF_MODE_1D;
        si_surf_minify(surf, level+i, bpe, i, xalign, yalign, zalign, slice_align, offset);
        /* level0 and first mipmap need to have alignment */
        offset = surf->bo_size;
        if ((i == 0)) {
            offset = ALIGN(offset, surf->bo_alignment);
        }
    }
    return 0;
}

static int si_surface_init_1d_miptrees(struct radeon_surface_manager *surf_man,
                                       struct radeon_surface *surf)
{
    unsigned zs_flags = RADEON_SURF_ZBUFFER | RADEON_SURF_SBUFFER;
    int r, is_depth_stencil = (surf->flags & zs_flags) == zs_flags;
    /* Old libdrm headers didn't have stencil_level in it. This prevents crashes. */
    struct radeon_surface_level tmp[RADEON_SURF_MAX_LEVEL];
    struct radeon_surface_level *stencil_level =
        (surf->flags & RADEON_SURF_HAS_SBUFFER_MIPTREE) ? surf->stencil_level : tmp;

    r = si_surface_init_1d(surf_man, surf, surf->level, surf->bpe, 0, 0);
    if (r)
        return r;

    if (is_depth_stencil) {
        r = si_surface_init_1d(surf_man, surf, stencil_level, 1,
                               surf->bo_size, 0);
        surf->stencil_offset = stencil_level[0].offset;
    }
    return r;
}

static int si_surface_init(struct radeon_surface_manager *surf_man,
                           struct radeon_surface *surf)
{
    unsigned mode;
    int r;

    /* MSAA surfaces support the 2D mode only. */
    if (surf->nsamples > 1) {
        surf->flags = RADEON_SURF_CLR(surf->flags, MODE);
        surf->flags |= RADEON_SURF_SET(RADEON_SURF_MODE_2D, MODE);
    }

    /* tiling mode */
    mode = (surf->flags >> RADEON_SURF_MODE_SHIFT) & RADEON_SURF_MODE_MASK;

    /* those are already handled by the kernel tile mode array but we still
     * need value that won't be rejected by kernel set tiling function
     */
    surf->tile_split = 0;
    surf->stencil_tile_split = 0;

    if (surf->flags & (RADEON_SURF_ZBUFFER | RADEON_SURF_SBUFFER)) {
        /* zbuffer only support 1D or 2D tiled surface */
        switch (mode) {
        case RADEON_SURF_MODE_1D:
        case RADEON_SURF_MODE_2D:
            break;
        default:
            mode = RADEON_SURF_MODE_1D;
            surf->flags = RADEON_SURF_CLR(surf->flags, MODE);
            surf->flags |= RADEON_SURF_SET(RADEON_SURF_MODE_1D, MODE);
            break;
        }
    }

    r = eg_surface_sanity(surf_man, surf, mode);
    if (r) {
        return r;
    }

    surf->stencil_offset = 0;
    surf->bo_alignment = 0;

    /* check tiling mode */
    switch (mode) {
    case RADEON_SURF_MODE_LINEAR:
        r = r6_surface_init_linear(surf_man, surf, 0, 0);
        break;
    case RADEON_SURF_MODE_LINEAR_ALIGNED:
        r = si_surface_init_linear_aligned(surf_man, surf, 0, 0);
        break;
    case RADEON_SURF_MODE_1D:
        r = si_surface_init_1d_miptrees(surf_man, surf);
        break;
    case RADEON_SURF_MODE_2D:
        r = eg_surface_init_2d_miptrees(surf_man, surf);
        break;
    default:
        return -EINVAL;
    }
    return r;
}

/* ===========================================================================
 * public API
 */
struct radeon_surface_manager *radeon_surface_manager_new(int fd)
{
    struct radeon_surface_manager *surf_man;

    surf_man = calloc(1, sizeof(struct radeon_surface_manager));
    if (surf_man == NULL) {
        return NULL;
    }
    surf_man->fd = fd;
    if (radeon_get_value(fd, RADEON_INFO_DEVICE_ID, &surf_man->device_id)) {
        goto out_err;
    }
    if (radeon_get_family(surf_man)) {
        goto out_err;
    }

    if (surf_man->family <= CHIP_RV740) {
        if (r6_init_hw_info(surf_man)) {
            goto out_err;
        }
        surf_man->surface_init = &r6_surface_init;
        surf_man->surface_best = &r6_surface_best;
    } else {
        if (eg_init_hw_info(surf_man)) {
            goto out_err;
        }
        if (surf_man->family <= CHIP_ARUBA) {
            surf_man->surface_init = &eg_surface_init;
        } else {
            surf_man->surface_init = &si_surface_init;
        }
        surf_man->surface_best = &eg_surface_best;
    }

    return surf_man;
out_err:
    free(surf_man);
    return NULL;
}

void radeon_surface_manager_free(struct radeon_surface_manager *surf_man)
{
    free(surf_man);
}

static int radeon_surface_sanity(struct radeon_surface_manager *surf_man,
                                 struct radeon_surface *surf,
                                 unsigned type,
                                 unsigned mode)
{
    if (surf_man == NULL || surf_man->surface_init == NULL || surf == NULL) {
        return -EINVAL;
    }

    /* all dimension must be at least 1 ! */
    if (!surf->npix_x || !surf->npix_y || !surf->npix_z) {
        return -EINVAL;
    }
    if (!surf->blk_w || !surf->blk_h || !surf->blk_d) {
        return -EINVAL;
    }
    if (!surf->array_size) {
        return -EINVAL;
    }
    /* array size must be a power of 2 */
    surf->array_size = next_power_of_two(surf->array_size);

    switch (surf->nsamples) {
    case 1:
    case 2:
    case 4:
    case 8:
        break;
    default:
        return -EINVAL;
    }
    /* check type */
    switch (type) {
    case RADEON_SURF_TYPE_1D:
        if (surf->npix_y > 1) {
            return -EINVAL;
        }
    case RADEON_SURF_TYPE_2D:
        if (surf->npix_z > 1) {
            return -EINVAL;
        }
        break;
    case RADEON_SURF_TYPE_CUBEMAP:
        if (surf->npix_z > 1) {
            return -EINVAL;
        }
        /* deal with cubemap as they were texture array */
        if (surf_man->family >= CHIP_RV770) {
            surf->array_size = 8;
        } else {
            surf->array_size = 6;
        }
        break;
    case RADEON_SURF_TYPE_3D:
        break;
    case RADEON_SURF_TYPE_1D_ARRAY:
        if (surf->npix_y > 1) {
            return -EINVAL;
        }
    case RADEON_SURF_TYPE_2D_ARRAY:
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int radeon_surface_init(struct radeon_surface_manager *surf_man,
                        struct radeon_surface *surf)
{
    unsigned mode, type;
    int r;

    type = RADEON_SURF_GET(surf->flags, TYPE);
    mode = RADEON_SURF_GET(surf->flags, MODE);

    r = radeon_surface_sanity(surf_man, surf, type, mode);
    if (r) {
        return r;
    }
    return surf_man->surface_init(surf_man, surf);
}

int radeon_surface_best(struct radeon_surface_manager *surf_man,
                        struct radeon_surface *surf)
{
    unsigned mode, type;
    int r;

    type = RADEON_SURF_GET(surf->flags, TYPE);
    mode = RADEON_SURF_GET(surf->flags, MODE);

    r = radeon_surface_sanity(surf_man, surf, type, mode);
    if (r) {
        return r;
    }
    return surf_man->surface_best(surf_man, surf);
}
