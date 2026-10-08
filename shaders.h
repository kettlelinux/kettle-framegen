// SPDX-License-Identifier: BSD-3-Clause
// The shaders' host-side contract, shared by the layer (framegen.c) and the offline test harness
// (test/fgtest.c): descriptor bindings, the sampler, push constants and the motion geometry.
// Anything a shader reads from the host belongs here, so the harness runs exactly what the
// layer runs.
#include "luma0.spv.h"
#include "down.spv.h"
#include "motion.spv.h"
#include "filter.spv.h"
#include "still.spv.h"
#include "synth.spv.h"

#define MAX_LEVELS 7
#define BLOCK 8  // motion block size, pixels (motion.comp B)

enum { P_LUMA, P_DOWN, P_MOTION, P_FILTER, P_STILL, P_SYNTH, NPIPE };
#define PUSH_SIZE 32

#define S VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define W VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
#define B VK_DESCRIPTOR_TYPE_STORAGE_BUFFER  // always the scene-cut counter
static const struct pipe_spec {
    const uint32_t *code;
    size_t size;
    uint32_t n;
    VkDescriptorType types[6];
    bool wide;  // workgroup size from specialization constants 0 and 1 (see pipe_group())
} pipe_specs[NPIPE] = {
    [P_LUMA] = { spv_luma0, sizeof(spv_luma0), 2, { S, W }, true },
    [P_DOWN] = { spv_down, sizeof(spv_down), 2, { S, W }, true },
    [P_MOTION] = { spv_motion, sizeof(spv_motion), 5, { S, S, S, S, W }, false },
    [P_FILTER] = { spv_filter, sizeof(spv_filter), 3, { S, W, B }, false },
    [P_STILL] = { spv_still, sizeof(spv_still), 3, { S, S, W }, true },
    [P_SYNTH] = { spv_synth, sizeof(spv_synth), 6, { S, S, S, W, B, S }, false },
};
#undef S
#undef W
#undef B

// Workgroup size of pipeline p. The light per-pixel passes run in groups as large as the
// device allows up to 256 (Vulkan only promises 128): on the Adreno 740 every group costs about
// 30 ns whatever it does, and in 8x8 groups the still mask at 1080p took 1.04 ms, in 32x8 0.39.
// The rest stay 8x8: their work is per block (motion, filter), or heavy enough that larger groups
// ran slower (synth). wide_x: wide_group_x() of the device.
static inline uint32_t wide_group_x(const VkPhysicalDeviceLimits *l)
{
    return l->maxComputeWorkGroupInvocations >= 256 && l->maxComputeWorkGroupSize[0] >= 32 ? 32 : 16;
}

static inline VkExtent2D pipe_group(int p, uint32_t wide_x)
{
    return pipe_specs[p].wide ? (VkExtent2D){ wide_x, 8 } : (VkExtent2D){ 8, 8 };
}

static const VkSpecializationMapEntry group_spec_map[2] = { { 0, 0, 4 }, { 1, 4, 4 } };

// The one sampler every sampled binding uses
static const VkSamplerCreateInfo sampler_info = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .magFilter = VK_FILTER_LINEAR,
    .minFilter = VK_FILTER_LINEAR,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .maxLod = VK_LOD_CLAMP_NONE,
};

// Luma pyramid: level 0 at flow_scale, halving down to ~24 pixels on the short side. mv[l] is
// level l's block grid.
static inline uint32_t flow_geometry(VkExtent2D full, float flow_scale, VkExtent2D luma[MAX_LEVELS],
                                     VkExtent2D mv[MAX_LEVELS])
{
    VkExtent2D l0 = {
        fmaxf(16.0f, roundf(full.width * flow_scale)),
        fmaxf(16.0f, roundf(full.height * flow_scale)),
    };
    uint32_t levels = 1;
    while (levels < MAX_LEVELS && (l0.width >> levels) >= 24 && (l0.height >> levels) >= 24)
        levels++;
    for (uint32_t l = 0; l < levels; l++) {
        luma[l] = (VkExtent2D){ l0.width >> l, l0.height >> l };
        mv[l] = (VkExtent2D){ (luma[l].width + BLOCK - 1) / BLOCK, (luma[l].height + BLOCK - 1) / BLOCK };
    }
    return levels;
}

// luma0.comp takes an int32 (bgr), down.comp an int32 (source level), filter.comp nothing

struct motion_pc {
    float luma_size[2], mv0_size[2];
    int32_t level, coarsest, use_temporal;
    float lambda;
};

static inline struct motion_pc motion_push(const VkExtent2D *luma, const VkExtent2D *mv, uint32_t levels,
                                           uint32_t l, bool use_temporal)
{
    return (struct motion_pc){
        { luma[l].width, luma[l].height },
        { mv[0].width, mv[0].height },
        l, l == levels - 1, use_temporal, 0.01f,
    };
}

struct synth_pc {
    float size[2], mv_uv[2], mv_to_px[2], t;
    int32_t flags;
};

// Generated frame at time t (0: previous frame, 1: current). ten: 10:10:10:2 pixels.
static inline struct synth_pc synth_push(VkExtent2D full, const VkExtent2D *luma, const VkExtent2D *mv, float t,
                                         bool flow, bool ten)
{
    return (struct synth_pc){
        { full.width, full.height },
        { (float)luma[0].width / (full.width * BLOCK * mv[0].width),
          (float)luma[0].height / (full.height * BLOCK * mv[0].height) },
        { (float)full.width / luma[0].width, (float)full.height / luma[0].height },
        t,
        (flow ? 1 : 0) | (ten ? 2 : 0),
    };
}

_Static_assert(sizeof(struct motion_pc) <= PUSH_SIZE && sizeof(struct synth_pc) <= PUSH_SIZE, "PUSH_SIZE");
