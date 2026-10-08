// SPDX-License-Identifier: BSD-3-Clause
// Offline test harness for the frame generation shaders. Runs a case's frames through
// luma0 -> down -> motion -> filter -> synth on a headless Vulkan device, as the layer does on
// each present, and compares the generated frames with stored references.
//
//   fgtest [-d device] [-o outdir] [-u] [-t reps] case...
//
// A case is a directory holding
//   0.ppm, 1.ppm, ...  the rendered frames, in order (binary PPM, 8 bits); at least two
//   case.conf          optional `key = value` lines: multiplier (2), flow_scale (0.5),
//                      mode (motion), min_psnr (35), max_bad (0.01): pass/fail tolerances
//   ref/I-K.ppm        reference for generated frame K (1 to multiplier - 1) between frames
//                      I - 1 and I
//   truth/I-K.ppm      optional: what that frame should look like; only reported, as a
//                      quality measure independent of the references
// Like the layer, each frame after the first starts its motion search from the previous
// pair's vectors, so longer sequences test that too.
//
// Drivers sample and round differently (and synth.comp's colour maths is mediump), which can
// tip a block to another vector, so a frame passes when its PSNR against the reference is at
// least min_psnr and at most max_bad of its pixels differ by more than BAD_DIFF in some
// channel. The defaults hold between RADV and lavapipe with a margin; a broken shader is far
// outside them.
//
// -u writes the outputs as the new references. -o writes the outputs, and a 4x amplified
// difference image beside each one that fails, to outdir/<case>/. -d picks the physical device
// by index (default: the first; FGTEST_DEVICE works too). -t runs each frame reps times and
// prints the GPU time of each stage per generated frame, the fastest of the runs, for comparing
// shader changes on the same frames.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

#include "shaders.h"

#define MAX_GEN 2  // as framegen.c
#define BAD_DIFF 24

static VkInstance instance;
static VkPhysicalDevice phys;
static VkPhysicalDeviceMemoryProperties memprops;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkSampler sampler;
static VkDescriptorSetLayout dsl[NPIPE];
static VkPipelineLayout layout[NPIPE];
static VkPipeline pipeline[NPIPE];
static VkCommandPool pool;
static float ts_period;  // ns per timestamp tick, 0: the queue has no timestamps
static int reps;         // -t: runs of each frame, 0: not timed

#define CHECK(x)                                                                         \
    do {                                                                                 \
        VkResult r_ = (x);                                                               \
        if (r_ != VK_SUCCESS) {                                                          \
            fprintf(stderr, "fgtest: %s failed (%d) at line %d\n", #x, r_, __LINE__);  \
            exit(2);                                                                     \
        }                                                                                \
    } while (0)

// ---------- images on disk ----------

struct pic {
    uint32_t w, h;
    uint8_t *rgb;
};

static bool ppm_read(const char *path, struct pic *p)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned w, h, max;
    int c;
    bool ok = fscanf(f, "P6 %u %u %u", &w, &h, &max) == 3 && max == 255 && (c = fgetc(f)) != EOF &&
              (c == ' ' || c == '\n' || c == '\r' || c == '\t') && w && h && w <= 16384 && h <= 16384;
    if (ok) {
        p->w = w;
        p->h = h;
        p->rgb = malloc((size_t)w * h * 3);
        ok = p->rgb && fread(p->rgb, 3, (size_t)w * h, f) == (size_t)w * h;
        if (!ok) {
            free(p->rgb);
            p->rgb = NULL;
        }
    }
    fclose(f);
    if (!ok)
        fprintf(stderr, "fgtest: %s: not an 8-bit binary PPM\n", path);
    return ok;
}

static bool ppm_write(const char *path, const struct pic *p)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "fgtest: %s: %s\n", path, strerror(errno));
        return false;
    }
    fprintf(f, "P6\n%u %u\n255\n", p->w, p->h);
    bool ok = fwrite(p->rgb, 3, (size_t)p->w * p->h, f) == (size_t)p->w * p->h;
    return fclose(f) == 0 && ok;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void mkdirs(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *s = buf + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(buf, 0755);
            *s = '/';
        }
    mkdir(buf, 0755);
}

// ---------- device ----------

static uint32_t mem_type(uint32_t allowed, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((allowed & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkDeviceMemory alloc(VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
    uint32_t type = mem_type(req.memoryTypeBits, want);
    if (type == UINT32_MAX)
        type = mem_type(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = type };
    VkDeviceMemory m;
    CHECK(vkAllocateMemory(dev, &ai, NULL, &m));
    return m;
}

static void device_init(int index)
{
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "fgtest",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, NULL));
    VkPhysicalDevice all[16];
    if (n > 16)
        n = 16;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, all));
    if (index < 0 || (uint32_t)index >= n) {
        fprintf(stderr, "fgtest: no Vulkan device %d (%u found)\n", index, n);
        exit(2);
    }
    phys = all[index];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    printf("device: %s\n", props.deviceName);
    ts_period = props.limits.timestampPeriod;

    VkQueueFamilyProperties fams[32];
    uint32_t nf = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nf, fams);
    for (family = 0; family < nf && !(fams[family].queueFlags & VK_QUEUE_COMPUTE_BIT); family++)
        ;
    if (family == nf) {
        fprintf(stderr, "fgtest: the device has no compute queue\n");
        exit(2);
    }
    if (!fams[family].timestampValidBits)
        ts_period = 0;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci };
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = family };
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    // as framegen.c's pipelines_create
    CHECK(vkCreateSampler(dev, &sampler_info, NULL, &sampler));
    for (int p = 0; p < NPIPE; p++) {
        const struct pipe_spec *s = &pipe_specs[p];
        VkDescriptorSetLayoutBinding b[sizeof(s->types) / sizeof(*s->types)];
        for (uint32_t i = 0; i < s->n; i++)
            b[i] = (VkDescriptorSetLayoutBinding){ i, s->types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = s->n, .pBindings = b };
        CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl[p]));
        VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, PUSH_SIZE };
        VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = 1, .pSetLayouts = &dsl[p],
                                            .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
        CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout[p]));
        VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                         .codeSize = s->size, .pCode = s->code };
        VkShaderModule mod;
        CHECK(vkCreateShaderModule(dev, &mci, NULL, &mod));
        VkComputePipelineCreateInfo cpci = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                       .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" },
            .layout = layout[p],
        };
        CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline[p]));
        vkDestroyShaderModule(dev, mod, NULL);
    }
}

static void device_free(void)
{
    for (int p = 0; p < NPIPE; p++) {
        vkDestroyPipeline(dev, pipeline[p], NULL);
        vkDestroyPipelineLayout(dev, layout[p], NULL);
        vkDestroyDescriptorSetLayout(dev, dsl[p], NULL);
    }
    vkDestroySampler(dev, sampler, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
}

// ---------- one case's resources, as framegen.c's flow_build ----------

// GPU timestamps in frame(), for -t: before the pyramid, then after each stage
enum { ST_START, ST_PYRAMID, ST_MOTION, ST_FILTER, ST_SYNTH, NSTAMP };
static const char *const stage_names[NSTAMP] = { NULL, "pyramid", "motion", "filter+still", "synth" };

struct img {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
};

struct buf {
    VkBuffer buffer;
    VkDeviceMemory mem;
    void *map;
};

struct run {
    VkExtent2D full;
    uint32_t levels;
    VkExtent2D luma[MAX_LEVELS], mv[MAX_LEVELS];
    struct img hist[2], pyr[2], mvl[MAX_LEVELS], mvf[2], out[MAX_GEN], still;
    VkImageView pyr_level[2][MAX_LEVELS];
    struct buf cut, up, down;
    VkDescriptorPool dpool;
    VkDescriptorSet ds_luma[2], ds_down[2][MAX_LEVELS], ds_motion[2][MAX_LEVELS], ds_filter[2], ds_still[2];
    VkDescriptorSet ds_synth[2][MAX_GEN];
    VkCommandBuffer cmd;
    VkQueryPool queries;
    VkFence fence;
};

static VkImageView view_create(VkImage image, VkFormat fmt, uint32_t level, uint32_t count)
{
    VkImageViewCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1 },
    };
    VkImageView v;
    CHECK(vkCreateImageView(dev, &ci, NULL, &v));
    return v;
}

static void img_create(struct img *im, VkFormat fmt, VkExtent2D size, uint32_t mips, VkImageUsageFlags usage)
{
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { size.width, size.height, 1 },
        .mipLevels = mips,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(dev, &ci, NULL, &im->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, im->image, &req);
    im->mem = alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkBindImageMemory(dev, im->image, im->mem, 0));
    im->view = view_create(im->image, fmt, 0, mips);
}

static void img_free(struct img *im)
{
    if (!im->image)
        return;
    vkDestroyImageView(dev, im->view, NULL);
    vkDestroyImage(dev, im->image, NULL);
    vkFreeMemory(dev, im->mem, NULL);
}

static void buf_create(struct buf *b, VkDeviceSize size, VkBufferUsageFlags usage, bool host)
{
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    CHECK(vkCreateBuffer(dev, &ci, NULL, &b->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b->buffer, &req);
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (host && mem_type(req.memoryTypeBits, hv) == UINT32_MAX) {
        fprintf(stderr, "fgtest: no host-visible coherent memory\n");
        exit(2);
    }
    b->mem = alloc(req, host ? hv : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkBindBufferMemory(dev, b->buffer, b->mem, 0));
    b->map = NULL;
    if (host)
        CHECK(vkMapMemory(dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map));
}

static void buf_free(struct buf *b)
{
    vkDestroyBuffer(dev, b->buffer, NULL);
    vkFreeMemory(dev, b->mem, NULL);  // unmaps
}

static VkDescriptorSet ds_make(struct run *R, int p, const VkImageView *views)
{
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = R->dpool, .descriptorSetCount = 1,
                                       .pSetLayouts = &dsl[p] };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(dev, &ai, &set));
    const struct pipe_spec *s = &pipe_specs[p];
    VkDescriptorImageInfo ii[6];
    VkDescriptorBufferInfo bi = { R->cut.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[6];
    for (uint32_t i = 0; i < s->n; i++) {
        bool isbuf = s->types[i] == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (!isbuf)
            ii[i] = (VkDescriptorImageInfo){ sampler, views[i], VK_IMAGE_LAYOUT_GENERAL };
        w[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = s->types[i],
            .pImageInfo = isbuf ? NULL : &ii[i],
            .pBufferInfo = isbuf ? &bi : NULL,
        };
    }
    vkUpdateDescriptorSets(dev, s->n, w, 0, NULL);
    return set;
}

static void run_create(struct run *R, VkExtent2D full, float flow_scale)
{
    memset(R, 0, sizeof(*R));
    R->full = full;
    R->levels = flow_geometry(full, flow_scale, R->luma, R->mv);
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM, vec = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    for (int s = 0; s < 2; s++) {
        img_create(&R->hist[s], color, full, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        img_create(&R->pyr[s], color, R->luma[0], R->levels, rw);
        img_create(&R->mvf[s], vec, R->mv[0], 1, rw);
        for (uint32_t l = 0; l < R->levels; l++)
            R->pyr_level[s][l] = view_create(R->pyr[s].image, color, l, 1);
    }
    for (uint32_t l = 0; l < R->levels; l++)
        img_create(&R->mvl[l], vec, R->mv[l], 1, rw);
    for (int k = 0; k < MAX_GEN; k++)
        img_create(&R->out[k], VK_FORMAT_R32_UINT, full, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    img_create(&R->still, color, full, 1, rw);
    VkDeviceSize one = (VkDeviceSize)full.width * full.height * 4;
    buf_create(&R->cut, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    buf_create(&R->up, one, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    buf_create(&R->down, one * MAX_GEN + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (1 + MAX_LEVELS + 4 * MAX_LEVELS + 1 + 2 + 4 * MAX_GEN) },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * (1 + MAX_LEVELS + MAX_LEVELS + 1 + 1 + MAX_GEN) },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * (1 + MAX_GEN) },
    };
    VkDescriptorPoolCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 2 * (1 + MAX_LEVELS + MAX_LEVELS + 1 + 1 + MAX_GEN),
                                       .poolSizeCount = 3, .pPoolSizes = sizes };
    CHECK(vkCreateDescriptorPool(dev, &dci, NULL, &R->dpool));
    for (int c = 0; c < 2; c++) {
        int p = c ^ 1;
        R->ds_luma[c] = ds_make(R, P_LUMA, (VkImageView[]){ R->hist[c].view, R->pyr_level[c][0] });
        for (uint32_t l = 1; l < R->levels; l++)
            R->ds_down[c][l] = ds_make(R, P_DOWN, (VkImageView[]){ R->pyr[c].view, R->pyr_level[c][l] });
        for (uint32_t l = 0; l < R->levels; l++) {
            VkImageView coarse = l + 1 < R->levels ? R->mvl[l + 1].view : R->mvf[p].view;
            R->ds_motion[c][l] = ds_make(R, P_MOTION, (VkImageView[]){ R->pyr[p].view, R->pyr[c].view, coarse,
                                                                       R->mvf[p].view, R->mvl[l].view });
        }
        R->ds_filter[c] = ds_make(R, P_FILTER, (VkImageView[]){ R->mvl[0].view, R->mvf[c].view });
        R->ds_still[c] = ds_make(R, P_STILL, (VkImageView[]){ R->hist[p].view, R->hist[c].view, R->still.view });
        for (int k = 0; k < MAX_GEN; k++)
            R->ds_synth[c][k] = ds_make(R, P_SYNTH, (VkImageView[]){ R->hist[p].view, R->hist[c].view,
                                                                     R->mvf[c].view, R->out[k].view, VK_NULL_HANDLE,
                                                                     R->still.view });
    }
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(dev, &cai, &R->cmd));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(dev, &fi, NULL, &R->fence));
    VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NSTAMP };
    if (reps > 0 && ts_period)
        CHECK(vkCreateQueryPool(dev, &qi, NULL, &R->queries));
}

static void run_free(struct run *R)
{
    vkDestroyFence(dev, R->fence, NULL);
    if (R->queries)
        vkDestroyQueryPool(dev, R->queries, NULL);
    vkFreeCommandBuffers(dev, pool, 1, &R->cmd);
    vkDestroyDescriptorPool(dev, R->dpool, NULL);
    for (int s = 0; s < 2; s++) {
        for (uint32_t l = 0; l < R->levels; l++)
            vkDestroyImageView(dev, R->pyr_level[s][l], NULL);
        img_free(&R->hist[s]);
        img_free(&R->pyr[s]);
        img_free(&R->mvf[s]);
    }
    for (uint32_t l = 0; l < R->levels; l++)
        img_free(&R->mvl[l]);
    for (int k = 0; k < MAX_GEN; k++)
        img_free(&R->out[k]);
    img_free(&R->still);
    buf_free(&R->cut);
    buf_free(&R->up);
    buf_free(&R->down);
}

// ---------- per frame, as framegen.c's record ----------

static void stamp(struct run *R, VkCommandBuffer cmd, int i)
{
    if (R->queries)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, R->queries, i);
}

static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst,
                    VkAccessFlags da)
{
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da };
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}

static void compute_to_compute(VkCommandBuffer cmd)
{
    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static void dispatch(VkCommandBuffer cmd, int p, VkDescriptorSet set, const void *push, uint32_t push_size,
                     VkExtent2D size)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline[p]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout[p], 0, 1, &set, 0, NULL);
    if (push_size)
        vkCmdPushConstants(cmd, layout[p], VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    vkCmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
}

// Frame `pic` goes to history slot c; with ngen > 0, the frames between the other slot's and
// it are generated into R->down (ngen frames of packed RGBA8, then the scene-cut counter).
static void frame(struct run *R, const struct pic *pic, int c, bool first, uint32_t ngen, int multiplier,
                  bool flow, bool have_mv)
{
    VkExtent2D full = R->full;
    uint8_t *up = R->up.map;
    for (size_t i = 0; i < (size_t)full.width * full.height; i++) {
        memcpy(up + 4 * i, pic->rgb + 3 * i, 3);
        up[4 * i + 3] = 255;
    }
    VkCommandBuffer cmd = R->cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    if (first) {
        // all images live in GENERAL
        VkImageMemoryBarrier b[2 * 3 + MAX_GEN + 1 + MAX_LEVELS];
        uint32_t n = 0;
        struct img *all[] = { &R->hist[0], &R->hist[1], &R->pyr[0], &R->pyr[1], &R->mvf[0], &R->mvf[1],
                              &R->out[0], &R->out[1], &R->still };
        _Static_assert(MAX_GEN == 2, "out[] list above");
        for (uint32_t i = 0; i < sizeof(all) / sizeof(*all) + R->levels; i++)
            b[n++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = i < sizeof(all) / sizeof(*all) ? all[i]->image : R->mvl[i - sizeof(all) / sizeof(*all)].image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 },
            };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, n, b);
    }

    // 1. the frame into history
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { full.width, full.height, 1 } };
    vkCmdCopyBufferToImage(cmd, R->up.buffer, R->hist[c].image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    if (ngen && flow)
        vkCmdFillBuffer(cmd, R->cut.buffer, 0, VK_WHOLE_SIZE, 0);
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (R->queries)
        vkCmdResetQueryPool(cmd, R->queries, 0, NSTAMP);
    stamp(R, cmd, ST_START);

    // 2. its luma pyramid
    int32_t bgr = 0;
    dispatch(cmd, P_LUMA, R->ds_luma[c], &bgr, sizeof(bgr), R->luma[0]);
    for (uint32_t l = 1; l < R->levels; l++) {
        compute_to_compute(cmd);
        int32_t src = l - 1;
        dispatch(cmd, P_DOWN, R->ds_down[c][l], &src, sizeof(src), R->luma[l]);
    }
    compute_to_compute(cmd);
    stamp(R, cmd, ST_PYRAMID);

    if (ngen) {
        // 3. motion, coarse to fine, then the median filter into mvf[c]
        if (flow) {
            for (int l = R->levels - 1; l >= 0; l--) {
                struct motion_pc pc = motion_push(R->luma, R->mv, R->levels, l, have_mv);
                dispatch(cmd, P_MOTION, R->ds_motion[c][l], &pc, sizeof(pc), R->mv[l]);
                compute_to_compute(cmd);
            }
            stamp(R, cmd, ST_MOTION);
            dispatch(cmd, P_FILTER, R->ds_filter[c], NULL, 0, R->mv[0]);
            dispatch(cmd, P_STILL, R->ds_still[c], NULL, 0, full);
            compute_to_compute(cmd);
        } else {
            stamp(R, cmd, ST_MOTION);
        }
        stamp(R, cmd, ST_FILTER);
        // 4. the in-between frames
        for (uint32_t k = 0; k < ngen; k++) {
            struct synth_pc pc = synth_push(full, R->luma, R->mv, (k + 1.0f) / multiplier, flow, false);
            dispatch(cmd, P_SYNTH, R->ds_synth[c][k], &pc, sizeof(pc), full);
        }
        stamp(R, cmd, ST_SYNTH);
        barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkDeviceSize one = (VkDeviceSize)full.width * full.height * 4;
        for (uint32_t k = 0; k < ngen; k++) {
            region.bufferOffset = one * k;
            vkCmdCopyImageToBuffer(cmd, R->out[k].image, VK_IMAGE_LAYOUT_GENERAL, R->down.buffer, 1, &region);
        }
        VkBufferCopy cr = { 0, one * MAX_GEN, 4 };
        vkCmdCopyBuffer(cmd, R->cut.buffer, R->down.buffer, 1, &cr);
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
    }
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CHECK(vkResetFences(dev, 1, &R->fence));
    CHECK(vkQueueSubmit(queue, 1, &si, R->fence));
    CHECK(vkWaitForFences(dev, 1, &R->fence, VK_TRUE, UINT64_MAX));
}

// For -t: each stage's GPU time in the frame just run, ms
static bool stage_ms(struct run *R, double ms[NSTAMP])
{
    uint64_t t[NSTAMP];
    if (!R->queries || vkGetQueryPoolResults(dev, R->queries, 0, NSTAMP, sizeof(t), t, sizeof(*t),
                                             VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return false;
    for (int i = 1; i < NSTAMP; i++)
        ms[i] = (t[i] - t[i - 1]) * ts_period * 1e-6;
    return true;
}

// ---------- comparison ----------

struct diff {
    double psnr;  // INFINITY when identical
    double bad;   // fraction of pixels off by more than BAD_DIFF in some channel
};

static struct diff compare(const struct pic *a, const struct pic *b, struct pic *amp)
{
    size_t n = (size_t)a->w * a->h, bad = 0;
    double se = 0.0;
    for (size_t i = 0; i < n; i++) {
        int worst = 0;
        for (int ch = 0; ch < 3; ch++) {
            int d = abs(a->rgb[3 * i + ch] - b->rgb[3 * i + ch]);
            se += (double)d * d;
            if (d > worst)
                worst = d;
            if (amp)
                amp->rgb[3 * i + ch] = d * 4 > 255 ? 255 : d * 4;
        }
        bad += worst > BAD_DIFF;
    }
    double mse = se / (3.0 * n);
    return (struct diff){ mse > 0.0 ? 10.0 * log10(255.0 * 255.0 / mse) : INFINITY, (double)bad / n };
}

// ---------- cases ----------

struct conf {
    int multiplier;
    float flow_scale;
    bool flow;
    double min_psnr, max_bad;
};

static bool conf_read(const char *dir, struct conf *c)
{
    *c = (struct conf){ .multiplier = 2, .flow_scale = 0.5f, .flow = true, .min_psnr = 35.0, .max_bad = 0.01 };
    char path[1024], line[256];
    snprintf(path, sizeof(path), "%s/case.conf", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return true;
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        char k[64], v[64];
        if (line[strspn(line, " \t")] == '#' || sscanf(line, " %63[^= \t] = %63s", k, v) != 2)
            continue;
        if (!strcmp(k, "multiplier"))
            c->multiplier = atoi(v);
        else if (!strcmp(k, "flow_scale"))
            c->flow_scale = strtof(v, NULL);
        else if (!strcmp(k, "mode"))
            c->flow = strcmp(v, "blend") != 0;
        else if (!strcmp(k, "min_psnr"))
            c->min_psnr = strtod(v, NULL);
        else if (!strcmp(k, "max_bad"))
            c->max_bad = strtod(v, NULL);
        else {
            fprintf(stderr, "fgtest: %s: unknown key %s\n", path, k);
            ok = false;
        }
    }
    fclose(f);
    if (c->multiplier < 2 || c->multiplier > MAX_GEN + 1 || !(c->flow_scale >= 0.1f && c->flow_scale <= 1.0f)) {
        fprintf(stderr, "fgtest: %s: multiplier must be 2 to %d, flow_scale 0.1 to 1\n", path, MAX_GEN + 1);
        ok = false;
    }
    return ok;
}

// 0: passed, 1: failed, 2: couldn't run
static int run_case(const char *dir, const char *outdir, bool update)
{
    const char *name = strrchr(dir, '/') && strrchr(dir, '/')[1] ? strrchr(dir, '/') + 1 : dir;
    struct conf cf;
    if (!conf_read(dir, &cf))
        return 2;
    char path[1400], out[1024];
    if (outdir) {
        snprintf(out, sizeof(out), "%s/%s", outdir, name);
        mkdirs(out);
    }
    if (update) {
        snprintf(path, sizeof(path), "%s/ref", dir);
        mkdirs(path);
    }

    struct run R;
    struct pic pic = { 0 }, gen = { 0 }, ref = { 0 }, amp = { 0 };
    int result = 0, nframes = 0, timed = 0;
    double total[NSTAMP] = { 0 };  // -t: summed over the generated frames, the fastest run of each
    for (;; nframes++) {
        snprintf(path, sizeof(path), "%s/%d.ppm", dir, nframes);
        if (!exists(path))
            break;
        free(pic.rgb);
        if (!ppm_read(path, &pic)) {
            result = 2;
            break;
        }
        VkExtent2D full = { pic.w, pic.h };
        if (nframes == 0) {
            run_create(&R, full, cf.flow_scale);
            gen = (struct pic){ pic.w, pic.h, malloc((size_t)pic.w * pic.h * 3) };
            amp = (struct pic){ pic.w, pic.h, malloc((size_t)pic.w * pic.h * 3) };
        } else if (full.width != R.full.width || full.height != R.full.height) {
            fprintf(stderr, "fgtest: %s: %ux%u, the frames before it are %ux%u\n", path, pic.w, pic.h,
                    R.full.width, R.full.height);
            result = 2;
            break;
        }
        uint32_t ngen = nframes ? cf.multiplier - 1 : 0;
        // like the layer: the current frame goes to slot cur, its vectors to mvf[cur], and the
        // previous pair's vectors seed this one's search
        int c = nframes & 1;
        frame(&R, &pic, c, nframes == 0, ngen, cf.multiplier, cf.flow, cf.flow && nframes >= 2);
        if (ngen && reps > 0) {
            // the same frame again (the other slot, the previous frame and its vectors are as they
            // were), keeping each stage's fastest
            double best[NSTAMP], ms[NSTAMP];
            bool ok = stage_ms(&R, best);
            for (int r = 1; ok && r < reps; r++) {
                frame(&R, &pic, c, false, ngen, cf.multiplier, cf.flow, cf.flow && nframes >= 2);
                ok = stage_ms(&R, ms);
                for (int i = 1; i < NSTAMP; i++)
                    best[i] = fmin(best[i], ms[i]);
            }
            for (int i = 1; ok && i < NSTAMP; i++)
                total[i] += best[i];
            timed += ok;
        }
        uint32_t cut = *(uint32_t *)((uint8_t *)R.down.map + (size_t)pic.w * pic.h * 4 * MAX_GEN);

        for (uint32_t k = 0; k < ngen; k++) {
            const uint8_t *px = (uint8_t *)R.down.map + (size_t)pic.w * pic.h * 4 * k;
            for (size_t i = 0; i < (size_t)pic.w * pic.h; i++)
                memcpy(gen.rgb + 3 * i, px + 4 * i, 3);
            char id[32];
            snprintf(id, sizeof(id), "%d-%u", nframes, k + 1);
            printf("%-15s %-5s", name, id);
            if (cf.flow)
                printf(" cut %4.1f%%", 100.0 * cut / (R.mv[0].width * R.mv[0].height));
            snprintf(path, sizeof(path), "%s/truth/%s.ppm", dir, id);
            if (exists(path)) {
                free(ref.rgb);
                if (!ppm_read(path, &ref) || ref.w != pic.w || ref.h != pic.h) {
                    result = 2;
                    break;
                }
                printf("  truth %6.2f dB", compare(&gen, &ref, NULL).psnr);
            }
            if (outdir) {
                snprintf(path, sizeof(path), "%s/%s.ppm", out, id);
                if (!ppm_write(path, &gen))
                    result = 2;
            }
            snprintf(path, sizeof(path), "%s/ref/%s.ppm", dir, id);
            if (update) {
                printf("  updated\n");
                if (!ppm_write(path, &gen))
                    result = 2;
                continue;
            }
            free(ref.rgb);
            ref.rgb = NULL;
            if (!exists(path)) {
                printf("  no reference (run with -u)\n");
                result = result ? result : 1;
                continue;
            }
            if (!ppm_read(path, &ref) || ref.w != pic.w || ref.h != pic.h) {
                printf("\n");
                fprintf(stderr, "fgtest: %s: unreadable or the wrong size\n", path);
                result = 2;
                continue;
            }
            struct diff d = compare(&gen, &ref, &amp);
            bool pass = d.psnr >= cf.min_psnr && d.bad <= cf.max_bad;
            printf("  ref %6.2f dB  %5.2f%% off  %s\n", d.psnr, 100.0 * d.bad, pass ? "ok" : "FAIL");
            if (!pass) {
                result = result ? result : 1;
                if (outdir) {
                    snprintf(path, sizeof(path), "%s/%s-diff.ppm", out, id);
                    ppm_write(path, &amp);
                }
            }
        }
        if (result == 2)
            break;
    }
    if (timed) {
        double all = 0;
        printf("%-15s GPU ms per pair:", name);
        for (int i = 1; i < NSTAMP; i++) {
            printf(" %s %.3f", stage_names[i], total[i] / timed);
            all += total[i] / timed;
        }
        printf(", all %.3f\n", all);
    } else if (reps > 0) {
        printf("%-15s no GPU timestamps on this device\n", name);
    }
    if (nframes > 0)
        run_free(&R);
    if (nframes < 2 && result == 0) {
        fprintf(stderr, "fgtest: %s: needs at least 0.ppm and 1.ppm\n", dir);
        result = 2;
    }
    free(pic.rgb);
    free(gen.rgb);
    free(ref.rgb);
    free(amp.rgb);
    return result;
}

int main(int argc, char **argv)
{
    const char *outdir = NULL, *env = getenv("FGTEST_DEVICE");
    int device = env ? atoi(env) : 0, opt;
    bool update = false;
    while ((opt = getopt(argc, argv, "d:o:ut:")) != -1) {
        if (opt == 'd')
            device = atoi(optarg);
        else if (opt == 'o')
            outdir = optarg;
        else if (opt == 'u')
            update = true;
        else if (opt == 't')
            reps = atoi(optarg);
        else
            goto usage;
    }
    if (optind == argc)
        goto usage;
    setvbuf(stdout, NULL, _IOLBF, 0);
    device_init(device);
    int worst = 0, failed = 0;
    for (int i = optind; i < argc; i++) {
        int r = run_case(argv[i], outdir, update);
        failed += r != 0;
        if (r > worst)
            worst = r;
    }
    device_free();
    if (!update)
        printf("%d of %d cases passed\n", argc - optind - failed, argc - optind);
    return worst;
usage:
    fprintf(stderr, "usage: %s [-d device] [-o outdir] [-u] [-t reps] case...\n", argv[0]);
    return 2;
}
