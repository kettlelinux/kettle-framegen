// SPDX-License-Identifier: BSD-3-Clause
// VK_LAYER_KETTLE_framegen: frame generation for Vulkan games (Proton's DXVK and vkd3d-proton
// included), Kettle Linux's own replacement for lsfg-vk.
//
// Everything runs on the game's own VkDevice and present queue: no second device and no
// images shared between devices. On each vkQueuePresentKHR the layer
//   1. copies the presented image into its history (two frames, ping-pong),
//   2. builds that frame's luma pyramid,
//   3. estimates motion between the previous and this frame, coarse to fine (motion.comp),
//   4. acquires spare swapchain images and fills them with frames interpolated along that
//      motion (synth.comp),
//   5. presents them, then the game's frame. FIFO paces the lot.
// Swapchains get MAX_GEN extra images for the generated frames and TRANSFER_SRC/DST usage.
//
// Loaded only with KETTLE_FG=1 (set per game by the Frame Generation plugin). Settings come
// from a per-game file, reread while the game runs, and KETTLE_FG_* variables; see config_load.
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "luma0.spv.h"
#include "down.spv.h"
#include "motion.spv.h"
#include "filter.spv.h"
#include "synth.spv.h"

#define EXPORT __attribute__((visibility("default")))
#define LAYER "VK_LAYER_KETTLE_framegen"
#define KEY(h) (*(void **)(h))
#define MAX_GEN 3          // generated frames per rendered one (4x)
#define RING 3             // presents in flight
#define MAX_LEVELS 7
#define MAX_QUEUES 64
#define BLOCK 8            // motion block size, pixels (motion.comp B)
#define ACQUIRE_TIMEOUT 100000000ull  // ns

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(LAYER ": ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// ---------- settings ----------

struct config {
    int multiplier;        // frames shown per rendered one; 1 = off
    float flow_scale;      // motion estimation resolution, fraction of the frame
    bool flow;             // false: plain blend without motion ("mode = blend")
    bool fifo;             // force FIFO presentation (read at swapchain creation)
    bool preserve_images;  // no extra swapchain images (read at swapchain creation)
    bool stats;            // log GPU time every 2 s
};

static pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct config cfg;
static bool cfg_loaded;
static char cfg_path[1024];
static struct timespec cfg_mtime;
static double cfg_checked;

static bool parse_bool(const char *v)
{
    return !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcasecmp(v, "yes");
}

static void config_set(struct config *c, const char *k, const char *v)
{
    if (!strcmp(k, "multiplier"))
        c->multiplier = atoi(v);
    else if (!strcmp(k, "flow_scale"))
        c->flow_scale = strtof(v, NULL);
    else if (!strcmp(k, "mode"))
        c->flow = strcmp(v, "blend") != 0;
    else if (!strcmp(k, "fifo"))
        c->fifo = parse_bool(v);
    else if (!strcmp(k, "preserve_images"))
        c->preserve_images = parse_bool(v);
    else if (!strcmp(k, "stats"))
        c->stats = parse_bool(v);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s) || *s == '"')
        s++;
    char *e = s + strlen(s);
    while (e > s && (isspace((unsigned char)e[-1]) || e[-1] == '"'))
        *--e = 0;
    return s;
}

// Defaults, then the per-game file (`key = value` lines), then KETTLE_FG_<KEY> variables.
static void config_load(void)
{
    struct config c = { .multiplier = 2, .flow_scale = 0.5f, .flow = true, .fifo = true };
    FILE *f = fopen(cfg_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq)
                continue;
            *eq = 0;
            config_set(&c, trim(line), trim(eq + 1));
        }
        fclose(f);
    }
    static const char *const keys[] = { "multiplier", "flow_scale", "mode", "fifo", "preserve_images", "stats" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        char env[64];
        snprintf(env, sizeof(env), "KETTLE_FG_%s", keys[i]);
        for (char *p = env; *p; p++)
            *p = toupper((unsigned char)*p);
        const char *v = getenv(env);
        if (v && *v)
            config_set(&c, keys[i], v);
    }
    if (c.multiplier < 1)
        c.multiplier = 1;
    if (c.multiplier > MAX_GEN + 1)
        c.multiplier = MAX_GEN + 1;
    if (!(c.flow_scale >= 0.1f))  // NaN too
        c.flow_scale = 0.1f;
    if (c.flow_scale > 1.0f)
        c.flow_scale = 1.0f;
    cfg = c;
}

// Current settings; checks the file for changes at most twice a second.
static struct config config_get(void)
{
    pthread_mutex_lock(&cfg_lock);
    double t = now_s();
    bool changed = false;
    if (!cfg_loaded) {
        const char *p = getenv("KETTLE_FG_CONFIG"), *app = getenv("SteamAppId");
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (!app || !*app || !strcmp(app, "0"))
            app = "default";
        if (p && *p)
            snprintf(cfg_path, sizeof(cfg_path), "%s", p);
        else if (xdg && *xdg)
            snprintf(cfg_path, sizeof(cfg_path), "%s/kettle-framegen/%s.conf", xdg, app);
        else
            snprintf(cfg_path, sizeof(cfg_path), "%s/.config/kettle-framegen/%s.conf", home ? home : "", app);
        cfg_loaded = changed = true;
    } else if (t - cfg_checked >= 0.5) {
        struct stat st;
        struct timespec m = { 0 };
        if (!stat(cfg_path, &st))
            m = st.st_mtim;
        changed = m.tv_sec != cfg_mtime.tv_sec || m.tv_nsec != cfg_mtime.tv_nsec;
    }
    if (changed) {
        struct stat st;
        cfg_mtime = (struct timespec){ 0 };
        if (!stat(cfg_path, &st))
            cfg_mtime = st.st_mtim;
        config_load();
        say("%s: %dx, %s, flow scale %.2f%s%s", cfg_path, cfg.multiplier, cfg.flow ? "motion" : "blend",
            cfg.flow_scale, cfg.fifo ? ", fifo" : "", cfg.preserve_images ? ", preserve images" : "");
    }
    if (changed || t - cfg_checked >= 0.5)
        cfg_checked = t;
    struct config c = cfg;
    pthread_mutex_unlock(&cfg_lock);
    return c;
}

// ---------- dispatch bookkeeping ----------

struct inst {
    struct inst *next;
    void *key;
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
};

#define DEV_FUNCS(X)                                                                           \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(QueueSubmit)                       \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR)                      \
    X(AcquireNextImageKHR) X(QueuePresentKHR)                                                  \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory)            \
    X(FreeMemory) X(BindImageMemory) X(CreateImageView) X(DestroyImageView)                    \
    X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule)            \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout)         \
    X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline)                      \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets)                 \
    X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool)                         \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)                        \
    X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences)                             \
    X(CreateSemaphore) X(DestroySemaphore)                                                     \
    X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults)                              \
    X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBindPipeline) X(CmdBindDescriptorSets)         \
    X(CmdPushConstants) X(CmdDispatch) X(CmdResetQueryPool) X(CmdWriteTimestamp)            \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)       \
    X(MapMemory) X(CmdCopyImageToBuffer) X(FreeCommandBuffers)

enum { P_LUMA, P_DOWN, P_MOTION, P_FILTER, P_SYNTH, NPIPE };
#define PUSH_SIZE 32

#define S VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define W VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
static const struct pipe_spec {
    const uint32_t *code;
    size_t size;
    uint32_t n;
    VkDescriptorType types[5];
} pipe_specs[NPIPE] = {
    [P_LUMA] = { spv_luma0, sizeof(spv_luma0), 2, { S, W } },
    [P_DOWN] = { spv_down, sizeof(spv_down), 2, { S, W } },
    [P_MOTION] = { spv_motion, sizeof(spv_motion), 5, { S, S, S, S, W } },
    [P_FILTER] = { spv_filter, sizeof(spv_filter), 2, { S, W } },
    [P_SYNTH] = { spv_synth, sizeof(spv_synth), 4, { S, S, S, W } },
};
#undef S
#undef W

struct dev {
    struct dev *next;
    void *key;
    VkDevice handle;
    VkPhysicalDevice phys;
    struct inst *inst;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData set_loader_data;
#define X(n) PFN_vk##n n;
    DEV_FUNCS(X)
#undef X
    VkPhysicalDeviceMemoryProperties mem;
    VkQueueFamilyProperties *families;
    uint32_t nfamilies;
    float ts_period;
    struct { VkQueue queue; uint32_t family; } queues[MAX_QUEUES];
    uint32_t nqueues;
    // shared by all swapchains, created with the first one that needs them
    bool pipes_ready;
    VkSampler sampler;
    VkDescriptorSetLayout dsl[NPIPE];
    VkPipelineLayout layout[NPIPE];
    VkPipeline pipe[NPIPE];
};

struct img {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
};

struct frame {  // one present in flight
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired[MAX_GEN];
    bool pending;
};

struct swapchain {
    struct swapchain *next;
    VkSwapchainKHR handle;
    struct dev *dev;
    VkExtent2D extent;
    VkFormat hist_format;  // swapchain pixels copied raw: RGBA8 or A2B10G10R10
    bool bgr;              // ...holding BGR(A)
    VkImage *images;
    uint32_t nimages;
    // Waited on by the present of each image. Per image, not per frame: a present's semaphore
    // is only free again once its image has been acquired again.
    VkSemaphore *present_sems;
    // Generated frames per present that found a spare image; after a timeout, one more is
    // tried (without waiting) every 10 s from probe_at
    uint32_t gen_limit;
    double probe_at;
    // Created on the first present (they need the present queue's family): the frame ring,
    // kept for the swapchain's life since presents wait on its semaphores, and the flow
    // resources, rebuilt when flow_scale changes. broken: creating them failed, frames pass
    // through untouched.
    bool ring_built, flow_built, broken;
    uint32_t family;
    float flow_scale;
    VkCommandPool pool;
    struct frame frames[RING];
    uint64_t count;
    VkQueryPool queries;
    uint32_t levels;
    VkExtent2D luma[MAX_LEVELS], mv[MAX_LEVELS];
    struct img hist[2], pyr[2], mvl[MAX_LEVELS], mvf[2], out[MAX_GEN];
    VkImageView pyr_level[2][MAX_LEVELS];
    VkDescriptorPool dpool;
    VkDescriptorSet ds_luma[2], ds_down[2][MAX_LEVELS], ds_motion[2][MAX_LEVELS], ds_filter[2];
    VkDescriptorSet ds_synth[2][MAX_GEN];
    int cur;             // history slot the next presented frame goes to
    bool have_prev;      // the other slot holds the previous frame and its pyramid
    bool have_mv;        // mvf of the other slot holds the previous frame's vectors
    bool fresh;          // images still in UNDEFINED layout
    double gpu_ms, stat_t;
    uint32_t gpu_n, shown;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct inst *insts;
static struct dev *devs;
static struct swapchain *swapchains;

static void *find_key(void *list, void *key)
{
    // inst and dev both start with {next, key}
    struct node { struct node *next; void *key; };
    pthread_mutex_lock(&lock);
    struct node *n = list;
    while (n && n->key != key)
        n = n->next;
    pthread_mutex_unlock(&lock);
    return n;
}

static void *unlink_key(void **list, void *key)
{
    struct node { struct node *next; void *key; };
    pthread_mutex_lock(&lock);
    struct node **p = (struct node **)list, *n;
    while ((n = *p) && n->key != key)
        p = &n->next;
    if (n)
        *p = n->next;
    pthread_mutex_unlock(&lock);
    return n;
}

#define find_inst(k) ((struct inst *)find_key(insts, (k)))
#define find_dev(k) ((struct dev *)find_key(devs, (k)))

static struct swapchain *find_sc(VkSwapchainKHR h, bool unlink)
{
    pthread_mutex_lock(&lock);
    struct swapchain **p = &swapchains, *s;
    while ((s = *p) && s->handle != h)
        p = &s->next;
    if (s && unlink)
        *p = s->next;
    pthread_mutex_unlock(&lock);
    return s;
}

static uint32_t queue_family(struct dev *d, VkQueue q)
{
    uint32_t f = UINT32_MAX;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < d->nqueues; i++)
        if (d->queues[i].queue == q)
            f = d->queues[i].family;
    pthread_mutex_unlock(&lock);
    return f;
}

static void note_queue(struct dev *d, VkQueue q, uint32_t family)
{
    if (!q || queue_family(d, q) != UINT32_MAX)
        return;
    pthread_mutex_lock(&lock);
    if (d->nqueues < MAX_QUEUES) {
        d->queues[d->nqueues].queue = q;
        d->queues[d->nqueues++].family = family;
    }
    pthread_mutex_unlock(&lock);
}

static const void *find_struct(const void *chain, VkStructureType type)
{
    for (const VkBaseInStructure *s = chain; s; s = s->pNext)
        if (s->sType == type)
            return s;
    return NULL;
}

// ---------- GPU resources ----------

static bool dev_pipelines(struct dev *d)
{
    if (d->pipes_ready)
        return true;
    VkSamplerCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    if (!d->sampler && d->CreateSampler(d->handle, &si, NULL, &d->sampler) != VK_SUCCESS)
        return false;
    for (int p = 0; p < NPIPE; p++) {
        if (d->pipe[p])
            continue;
        const struct pipe_spec *s = &pipe_specs[p];
        VkDescriptorSetLayoutBinding b[5];
        for (uint32_t i = 0; i < s->n; i++)
            b[i] = (VkDescriptorSetLayoutBinding){ i, s->types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = s->n,
            .pBindings = b,
        };
        if (!d->dsl[p] && d->CreateDescriptorSetLayout(d->handle, &dci, NULL, &d->dsl[p]) != VK_SUCCESS)
            return false;
        VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, PUSH_SIZE };
        VkPipelineLayoutCreateInfo lci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &d->dsl[p],
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pr,
        };
        if (!d->layout[p] && d->CreatePipelineLayout(d->handle, &lci, NULL, &d->layout[p]) != VK_SUCCESS)
            return false;
        VkShaderModuleCreateInfo mci = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = s->size,
            .pCode = s->code,
        };
        VkShaderModule mod;
        if (d->CreateShaderModule(d->handle, &mci, NULL, &mod) != VK_SUCCESS)
            return false;
        VkComputePipelineCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = mod,
                .pName = "main",
            },
            .layout = d->layout[p],
        };
        VkResult r = d->CreateComputePipelines(d->handle, VK_NULL_HANDLE, 1, &pci, NULL, &d->pipe[p]);
        d->DestroyShaderModule(d->handle, mod, NULL);
        if (r != VK_SUCCESS)
            return false;
    }
    d->pipes_ready = true;
    return true;
}

static void dev_free_pipelines(struct dev *d)
{
    for (int p = 0; p < NPIPE; p++) {
        if (d->pipe[p])
            d->DestroyPipeline(d->handle, d->pipe[p], NULL);
        if (d->layout[p])
            d->DestroyPipelineLayout(d->handle, d->layout[p], NULL);
        if (d->dsl[p])
            d->DestroyDescriptorSetLayout(d->handle, d->dsl[p], NULL);
    }
    if (d->sampler)
        d->DestroySampler(d->handle, d->sampler, NULL);
}

static VkImageView view_create(struct dev *d, VkImage image, VkFormat fmt, uint32_t level, uint32_t count)
{
    VkImageViewCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1 },
    };
    VkImageView v = VK_NULL_HANDLE;
    d->CreateImageView(d->handle, &ci, NULL, &v);
    return v;
}

static bool img_create(struct dev *d, struct img *im, VkFormat fmt, VkExtent2D size, uint32_t mips,
                       VkImageUsageFlags usage)
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
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (d->CreateImage(d->handle, &ci, NULL, &im->image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    d->GetImageMemoryRequirements(d->handle, im->image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < d->mem.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (d->mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    for (uint32_t i = 0; i < d->mem.memoryTypeCount && type == UINT32_MAX; i++)
        if (req.memoryTypeBits & (1u << i))
            type = i;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (type == UINT32_MAX || d->AllocateMemory(d->handle, &ai, NULL, &im->mem) != VK_SUCCESS ||
        d->BindImageMemory(d->handle, im->image, im->mem, 0) != VK_SUCCESS)
        return false;
    im->view = view_create(d, im->image, fmt, 0, mips);
    return im->view != VK_NULL_HANDLE;
}

static void img_free(struct dev *d, struct img *im)
{
    if (im->view)
        d->DestroyImageView(d->handle, im->view, NULL);
    if (im->image)
        d->DestroyImage(d->handle, im->image, NULL);
    if (im->mem)
        d->FreeMemory(d->handle, im->mem, NULL);
    *im = (struct img){ 0 };
}

// A descriptor set for pipeline `pipe`, binding views[i] to binding i.
static VkDescriptorSet ds_make(struct swapchain *sc, int pipe, const VkImageView *views)
{
    struct dev *d = sc->dev;
    const struct pipe_spec *p = &pipe_specs[pipe];
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = sc->dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &d->dsl[pipe],
    };
    if (d->AllocateDescriptorSets(d->handle, &ai, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[5];
    VkWriteDescriptorSet w[5];
    for (uint32_t i = 0; i < p->n; i++) {
        ii[i] = (VkDescriptorImageInfo){ d->sampler, views[i], VK_IMAGE_LAYOUT_GENERAL };
        w[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = p->types[i],
            .pImageInfo = &ii[i],
        };
    }
    d->UpdateDescriptorSets(d->handle, p->n, w, 0, NULL);
    return set;
}

static void sc_idle(struct swapchain *sc)
{
    struct dev *d = sc->dev;
    for (int i = 0; i < RING; i++) {
        struct frame *f = &sc->frames[i];
        if (f->pending)
            d->WaitForFences(d->handle, 1, &f->fence, VK_TRUE, UINT64_MAX);
        f->pending = false;
    }
}

static void ring_free(struct swapchain *sc)
{
    struct dev *d = sc->dev;
    VkDevice dev = d->handle;
    sc_idle(sc);
    for (int i = 0; i < RING; i++) {
        struct frame *f = &sc->frames[i];
        if (f->fence)
            d->DestroyFence(dev, f->fence, NULL);
        for (int k = 0; k < MAX_GEN; k++) {
            if (f->acquired[k])
                d->DestroySemaphore(dev, f->acquired[k], NULL);
        }
        *f = (struct frame){ 0 };
    }
    for (uint32_t i = 0; i < sc->nimages; i++)
        if (sc->present_sems[i])
            d->DestroySemaphore(dev, sc->present_sems[i], NULL);
    memset(sc->present_sems, 0, sc->nimages * sizeof(*sc->present_sems));
    if (sc->pool)
        d->DestroyCommandPool(dev, sc->pool, NULL);
    if (sc->queries)
        d->DestroyQueryPool(dev, sc->queries, NULL);
    sc->pool = VK_NULL_HANDLE;
    sc->queries = VK_NULL_HANDLE;
    sc->ring_built = false;
}

static void flow_free(struct swapchain *sc)
{
    struct dev *d = sc->dev;
    VkDevice dev = d->handle;
    sc_idle(sc);
    if (sc->dpool)
        d->DestroyDescriptorPool(dev, sc->dpool, NULL);
    for (int s = 0; s < 2; s++)
        for (int l = 0; l < MAX_LEVELS; l++)
            if (sc->pyr_level[s][l])
                d->DestroyImageView(dev, sc->pyr_level[s][l], NULL);
    for (int s = 0; s < 2; s++) {
        img_free(d, &sc->hist[s]);
        img_free(d, &sc->pyr[s]);
        img_free(d, &sc->mvf[s]);
    }
    for (int l = 0; l < MAX_LEVELS; l++)
        img_free(d, &sc->mvl[l]);
    for (int k = 0; k < MAX_GEN; k++)
        img_free(d, &sc->out[k]);
    sc->dpool = VK_NULL_HANDLE;
    memset(sc->pyr_level, 0, sizeof(sc->pyr_level));
    sc->flow_built = false;
}

static void sc_free(struct swapchain *sc)
{
    flow_free(sc);
    ring_free(sc);
}

static bool ring_build(struct swapchain *sc, uint32_t family)
{
    struct dev *d = sc->dev;
    VkDevice dev = d->handle;
    sc->ring_built = true;  // from here on ring_free cleans up whatever got created
    sc->family = family;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    if (d->CreateCommandPool(dev, &pci, NULL, &sc->pool) != VK_SUCCESS)
        return false;
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (int i = 0; i < RING; i++) {
        struct frame *f = &sc->frames[i];
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = sc->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (d->AllocateCommandBuffers(dev, &ai, &f->cmd) != VK_SUCCESS ||
            d->set_loader_data(dev, f->cmd) != VK_SUCCESS ||
            d->CreateFence(dev, &fi, NULL, &f->fence) != VK_SUCCESS)
            return false;
        for (int k = 0; k < MAX_GEN; k++)
            if (d->CreateSemaphore(dev, &semi, NULL, &f->acquired[k]) != VK_SUCCESS)
                return false;
    }
    for (uint32_t i = 0; i < sc->nimages; i++)
        if (d->CreateSemaphore(dev, &semi, NULL, &sc->present_sems[i]) != VK_SUCCESS)
            return false;
    if (d->families[family].timestampValidBits) {
        VkQueryPoolCreateInfo qci = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 2 * RING,
        };
        d->CreateQueryPool(dev, &qci, NULL, &sc->queries);
    }
    return true;
}

static bool flow_build(struct swapchain *sc, float flow_scale)
{
    struct dev *d = sc->dev;
    VkDevice dev = d->handle;
    sc->flow_built = true;  // from here on flow_free cleans up whatever got created
    sc->flow_scale = flow_scale;
    if (!dev_pipelines(d))
        return false;

    // luma pyramid: level 0 at flow_scale, halving down to ~24 pixels on the short side
    VkExtent2D full = sc->extent;
    VkExtent2D l0 = {
        fmaxf(16.0f, roundf(full.width * flow_scale)),
        fmaxf(16.0f, roundf(full.height * flow_scale)),
    };
    sc->levels = 1;
    while (sc->levels < MAX_LEVELS && (l0.width >> sc->levels) >= 24 && (l0.height >> sc->levels) >= 24)
        sc->levels++;
    for (uint32_t l = 0; l < sc->levels; l++) {
        sc->luma[l] = (VkExtent2D){ l0.width >> l, l0.height >> l };
        sc->mv[l] = (VkExtent2D){ (sc->luma[l].width + BLOCK - 1) / BLOCK, (sc->luma[l].height + BLOCK - 1) / BLOCK };
    }

    // History holds raw copies of the swapchain's 32-bit pixels, whatever their channel order
    // or encoding. Generated frames are R32_UINT that synth.comp packs in the swapchain's
    // layout: a raw copy back, and no optional storage format needed.
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM, vec = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    for (int s = 0; s < 2; s++) {
        if (!img_create(d, &sc->hist[s], sc->hist_format, full, 1,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !img_create(d, &sc->pyr[s], color, l0, sc->levels, rw) ||
            !img_create(d, &sc->mvf[s], vec, sc->mv[0], 1, rw))
            return false;
        for (uint32_t l = 0; l < sc->levels; l++)
            if (!(sc->pyr_level[s][l] = view_create(d, sc->pyr[s].image, color, l, 1)))
                return false;
    }
    for (uint32_t l = 0; l < sc->levels; l++)
        if (!img_create(d, &sc->mvl[l], vec, sc->mv[l], 1, rw))
            return false;
    for (int k = 0; k < MAX_GEN; k++)
        if (!img_create(d, &sc->out[k], VK_FORMAT_R32_UINT, full, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            return false;

    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (1 + MAX_LEVELS + 4 * MAX_LEVELS + 1 + 3 * MAX_GEN) },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * (1 + MAX_LEVELS + MAX_LEVELS + 1 + MAX_GEN) },
    };
    VkDescriptorPoolCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2 * (1 + MAX_LEVELS + MAX_LEVELS + 1 + MAX_GEN),
        .poolSizeCount = 2,
        .pPoolSizes = sizes,
    };
    if (d->CreateDescriptorPool(dev, &dci, NULL, &sc->dpool) != VK_SUCCESS)
        return false;
    // Sets per history slot c (the slot the current frame is in; c ^ 1 holds the previous)
    for (int c = 0; c < 2; c++) {
        int p = c ^ 1;
        bool ok = (sc->ds_luma[c] = ds_make(sc, P_LUMA, (VkImageView[]){ sc->hist[c].view, sc->pyr_level[c][0] }));
        for (uint32_t l = 1; l < sc->levels; l++)
            ok = ok && (sc->ds_down[c][l] = ds_make(sc, P_DOWN, (VkImageView[]){ sc->pyr[c].view, sc->pyr_level[c][l] }));
        for (uint32_t l = 0; l < sc->levels; l++) {
            // the coarsest level has no level below; bind something valid it won't read
            VkImageView coarse = l + 1 < sc->levels ? sc->mvl[l + 1].view : sc->mvf[p].view;
            ok = ok && (sc->ds_motion[c][l] = ds_make(sc, P_MOTION, (VkImageView[]){
                            sc->pyr[p].view, sc->pyr[c].view, coarse, sc->mvf[p].view, sc->mvl[l].view }));
        }
        ok = ok && (sc->ds_filter[c] = ds_make(sc, P_FILTER, (VkImageView[]){ sc->mvl[0].view, sc->mvf[c].view }));
        for (int k = 0; k < MAX_GEN; k++)
            ok = ok && (sc->ds_synth[c][k] = ds_make(sc, P_SYNTH, (VkImageView[]){
                            sc->hist[p].view, sc->hist[c].view, sc->mvf[c].view, sc->out[k].view }));
        if (!ok)
            return false;
    }
    sc->cur = 0;
    sc->have_prev = sc->have_mv = false;
    sc->fresh = true;
    say("%ux%u: motion at %ux%u, %u levels", full.width, full.height, l0.width, l0.height, sc->levels);
    return true;
}

// ---------- per-present work ----------

static void barrier(struct dev *d, VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags sa,
                    VkPipelineStageFlags dst, VkAccessFlags da)
{
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da };
    d->CmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}

static VkImageMemoryBarrier layout_change(VkImage image, VkImageLayout from, VkImageLayout to,
                                          VkAccessFlags sa, VkAccessFlags da)
{
    return (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = sa,
        .dstAccessMask = da,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 },
    };
}

static void image_barrier(struct dev *d, VkCommandBuffer cmd, VkPipelineStageFlags src,
                          VkPipelineStageFlags dst, VkImageMemoryBarrier b)
{
    d->CmdPipelineBarrier(cmd, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

static void compute_to_compute(struct dev *d, VkCommandBuffer cmd)
{
    barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static void run(struct dev *d, VkCommandBuffer cmd, int pipe, VkDescriptorSet set, const void *push,
                uint32_t push_size, VkExtent2D size)
{
    d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipe[pipe]);
    d->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->layout[pipe], 0, 1, &set, 0, NULL);
    if (push_size)
        d->CmdPushConstants(cmd, d->layout[pipe], VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    d->CmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
}

// Record one present's work: history copy and pyramid always (the next frame needs them),
// motion and ngen generated frames into the acquired images acq[] when there are any.
static void record(struct swapchain *sc, VkCommandBuffer cmd, int slot, uint32_t idx,
                   const uint32_t *acq, uint32_t ngen, int multiplier, bool flow)
{
    struct dev *d = sc->dev;
    int c = sc->cur;
    VkExtent2D full = sc->extent;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    d->BeginCommandBuffer(cmd, &bi);
    if (sc->queries) {
        d->CmdResetQueryPool(cmd, sc->queries, 2 * slot, 2);
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sc->queries, 2 * slot);
    }

    if (sc->fresh) {
        // all our images live in GENERAL
        struct img *all[] = { &sc->hist[0], &sc->hist[1], &sc->pyr[0], &sc->pyr[1], &sc->mvf[0], &sc->mvf[1],
                              &sc->out[0], &sc->out[1], &sc->out[2] };
        _Static_assert(MAX_GEN == 3, "out[] list above");
        VkImageMemoryBarrier b[sizeof(all) / sizeof(*all) + MAX_LEVELS];
        uint32_t n = 0;
        for (size_t i = 0; i < sizeof(all) / sizeof(*all); i++)
            b[n++] = layout_change(all[i]->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        for (uint32_t l = 0; l < sc->levels; l++)
            b[n++] = layout_change(sc->mvl[l].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                              0, NULL, n, b);
        sc->fresh = false;
    } else {
        // earlier presents' reads and writes of the slot and images reused now
        barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                    VK_ACCESS_SHADER_WRITE_BIT);
    }

    // 1. the game's frame into history
    VkImageCopy region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { full.width, full.height, 1 },
    };
    image_barrier(d, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  layout_change(sc->images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT));
    d->CmdCopyImage(cmd, sc->images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->hist[c].image,
                    VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    image_barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                  layout_change(sc->images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0));
    barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    // 2. its luma pyramid
    int32_t bgr = sc->bgr;
    run(d, cmd, P_LUMA, sc->ds_luma[c], &bgr, sizeof(bgr), sc->luma[0]);
    for (uint32_t l = 1; l < sc->levels; l++) {
        compute_to_compute(d, cmd);
        int32_t src = l - 1;
        run(d, cmd, P_DOWN, sc->ds_down[c][l], &src, sizeof(src), sc->luma[l]);
    }
    compute_to_compute(d, cmd);

    if (ngen) {
        // 3. motion, coarse to fine, then the median filter into mvf[c]
        if (flow) {
            for (int l = sc->levels - 1; l >= 0; l--) {
                struct {
                    float luma_size[2], mv0_size[2];
                    int32_t level, coarsest, use_temporal;
                    float lambda;
                } pc = {
                    { sc->luma[l].width, sc->luma[l].height },
                    { sc->mv[0].width, sc->mv[0].height },
                    l, l == (int)sc->levels - 1, sc->have_mv, 0.01f,
                };
                run(d, cmd, P_MOTION, sc->ds_motion[c][l], &pc, sizeof(pc), sc->mv[l]);
                compute_to_compute(d, cmd);
            }
            run(d, cmd, P_FILTER, sc->ds_filter[c], NULL, 0, sc->mv[0]);
            compute_to_compute(d, cmd);
        }

        // 4. the in-between frames
        for (uint32_t k = 0; k < ngen; k++) {
            struct {
                float size[2], mv_uv[2], mv_to_px[2], t;
                int32_t flags;
            } pc = {
                { full.width, full.height },
                { (float)sc->luma[0].width / (full.width * BLOCK * sc->mv[0].width),
                  (float)sc->luma[0].height / (full.height * BLOCK * sc->mv[0].height) },
                { (float)full.width / sc->luma[0].width, (float)full.height / sc->luma[0].height },
                (k + 1.0f) / multiplier,
                (flow ? 1 : 0) | (sc->hist_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 2 : 0),
            };
            run(d, cmd, P_SYNTH, sc->ds_synth[c][k], &pc, sizeof(pc), full);
        }
        barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (uint32_t k = 0; k < ngen; k++) {
            VkImage dst = sc->images[acq[k]];
            image_barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          layout_change(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                        VK_ACCESS_TRANSFER_WRITE_BIT));
            d->CmdCopyImage(cmd, sc->out[k].image, VK_IMAGE_LAYOUT_GENERAL, dst,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            image_barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          layout_change(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                        VK_ACCESS_TRANSFER_WRITE_BIT, 0));
        }
    }

    if (sc->queries)
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sc->queries, 2 * slot + 1);
    d->EndCommandBuffer(cmd);
}

// Debugging: with KETTLE_FG_DUMP=<dir>, the previous frame, the generated ones and the current
// frame of present number KETTLE_FG_DUMP_FRAME (default 300) go to <dir>/kettle-fg-<n>-<i>.ppm.
static void dump(struct swapchain *sc, struct frame *f, uint32_t ngen)
{
    struct dev *d = sc->dev;
    VkDevice dev = d->handle;
    const char *dir = getenv("KETTLE_FG_DUMP");
    uint32_t w = sc->extent.width, h = sc->extent.height, n = ngen + 2;
    VkImage src[MAX_GEN + 2];
    src[0] = sc->hist[sc->cur ^ 1].image;
    for (uint32_t k = 0; k < ngen; k++)
        src[k + 1] = sc->out[k].image;
    src[ngen + 1] = sc->hist[sc->cur].image;

    VkDeviceSize one = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = one * n,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t *px = NULL;
    VkMemoryRequirements req;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (d->CreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS)
        goto out;
    d->GetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = UINT32_MAX };
    for (uint32_t i = 0; i < d->mem.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (d->mem.memoryTypes[i].propertyFlags & host) == host)
            ai.memoryTypeIndex = i;
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = sc->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (ai.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(dev, &ai, NULL, &mem) != VK_SUCCESS ||
        d->BindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS ||
        d->MapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&px) != VK_SUCCESS ||
        d->AllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS || d->set_loader_data(dev, cmd) != VK_SUCCESS ||
        d->CreateFence(dev, &fi, NULL, &fence) != VK_SUCCESS)
        goto out;

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    d->BeginCommandBuffer(cmd, &bi);
    barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    for (uint32_t i = 0; i < n; i++) {
        VkBufferImageCopy r = {
            .bufferOffset = one * i,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { w, h, 1 },
        };
        d->CmdCopyImageToBuffer(cmd, src[i], VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
    }
    barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
    d->EndCommandBuffer(cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VkQueue q = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < d->nqueues && !q; i++)
        if (d->queues[i].family == sc->family)
            q = d->queues[i].queue;
    d->WaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    if (!q || d->QueueSubmit(q, 1, &si, fence) != VK_SUCCESS)
        goto out;
    d->WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);

    bool ten = sc->hist_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    for (uint32_t i = 0; i < n; i++) {
        char path[1100];
        snprintf(path, sizeof(path), "%s/kettle-fg-%llu-%u.ppm", dir, (unsigned long long)sc->count, i);
        FILE *o = fopen(path, "wb");
        if (!o)
            continue;
        fprintf(o, "P6\n%u %u\n255\n", w, h);
        const uint32_t *p = px + (size_t)w * h * i;
        for (size_t j = 0; j < (size_t)w * h; j++) {
            uint32_t v = p[j];
            uint8_t c[3] = { v, v >> 8, v >> 16 };
            if (ten)
                c[0] = (v & 1023) >> 2, c[1] = ((v >> 10) & 1023) >> 2, c[2] = ((v >> 20) & 1023) >> 2;
            if (sc->bgr) {
                uint8_t t = c[0];
                c[0] = c[2];
                c[2] = t;
            }
            fwrite(c, 1, 3, o);
        }
        fclose(o);
    }
    say("dumped %u frames of present %llu to %s", n, (unsigned long long)sc->count, dir);
out:
    if (fence)
        d->DestroyFence(dev, fence, NULL);
    if (cmd)
        d->FreeCommandBuffers(dev, sc->pool, 1, &cmd);
    if (buf)
        d->DestroyBuffer(dev, buf, NULL);
    if (mem)
        d->FreeMemory(dev, mem, NULL);  // unmaps
}

static void stats(struct swapchain *sc, int slot, int multiplier)
{
    struct dev *d = sc->dev;
    uint64_t ts[2];
    if (!sc->queries || d->GetQueryPoolResults(d->handle, sc->queries, 2 * slot, 2, sizeof(ts), ts, sizeof(ts[0]),
                                               VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    uint32_t bits = d->families[sc->family].timestampValidBits;
    uint64_t mask = bits >= 64 ? UINT64_MAX : (1ull << bits) - 1;
    sc->gpu_ms += ((ts[1] - ts[0]) & mask) * d->ts_period * 1e-6;
    sc->gpu_n++;
    double t = now_s();
    if (t - sc->stat_t >= 2.0) {
        if (sc->stat_t > 0)
            say("%.1f rendered fps, %.1f shown, %.2f ms GPU per rendered frame (%dx)", sc->gpu_n / (t - sc->stat_t),
                sc->shown / (t - sc->stat_t), sc->gpu_ms / sc->gpu_n, multiplier);
        sc->stat_t = t;
        sc->gpu_ms = 0;
        sc->gpu_n = sc->shown = 0;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    struct dev *d = find_dev(KEY(queue));
    struct swapchain *sc = pi->swapchainCount == 1 ? find_sc(pi->pSwapchains[0], false) : NULL;
    if (!sc || sc->broken)
        return d->QueuePresentKHR(queue, pi);
    VkDevice dev = d->handle;
    struct config c = config_get();
    uint32_t family = queue_family(d, queue);
    if (c.multiplier < 2 || family >= d->nfamilies || !(d->families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        sc->have_prev = sc->have_mv = false;
        return d->QueuePresentKHR(queue, pi);
    }
    if (sc->ring_built && sc->family != family) {
        // presented from another queue family now (rare): start over there
        sc_free(sc);
    }
    bool ok = sc->ring_built || ring_build(sc, family);
    if (ok && sc->flow_built && sc->flow_scale != c.flow_scale)
        flow_free(sc);
    ok = ok && (sc->flow_built || flow_build(sc, c.flow_scale));
    if (!ok) {
        say("couldn't create its resources, frame generation off for this swapchain");
        sc_free(sc);
        sc->broken = true;
        return d->QueuePresentKHR(queue, pi);
    }

    int slot = sc->count % RING;
    struct frame *f = &sc->frames[slot];
    if (f->pending) {
        d->WaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        f->pending = false;
        if (c.stats)
            stats(sc, slot, c.multiplier);
    }
    d->ResetFences(dev, 1, &f->fence);

    // Spare images for the generated frames. Waiting here is FIFO pacing at work; a timeout
    // means the images ran out (preserve_images) or presentation stalled (hidden window).
    uint32_t idx = pi->pImageIndices[0], acq[MAX_GEN], ngen = 0;
    while (sc->have_prev && ngen < (uint32_t)c.multiplier - 1) {
        bool probe = ngen >= sc->gen_limit;
        if (probe && now_s() < sc->probe_at)
            break;
        VkResult r = d->AcquireNextImageKHR(dev, sc->handle, probe ? 0 : ACQUIRE_TIMEOUT, f->acquired[ngen],
                                            VK_NULL_HANDLE, &acq[ngen]);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) {
            if (!probe) {
                say("no spare swapchain image after %u generated frames, generating at most that many", ngen);
                sc->gen_limit = ngen;
            }
            sc->probe_at = now_s() + 10.0;
            break;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
            break;
        if (probe)
            sc->gen_limit = ngen + 1;
        ngen++;
    }

    record(sc, f->cmd, slot, idx, acq, ngen, c.multiplier, c.flow);

    // The game's wait semaphores gate our copy of its frame; its present then waits for us.
    uint32_t nwait = pi->waitSemaphoreCount + ngen;
    VkSemaphore *waits = malloc(nwait * sizeof(*waits));
    VkPipelineStageFlags *stages = malloc(nwait * sizeof(*stages));
    if (!waits || !stages) {
        free(waits);
        free(stages);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++)
        waits[i] = pi->pWaitSemaphores[i];
    for (uint32_t k = 0; k < ngen; k++)
        waits[pi->waitSemaphoreCount + k] = f->acquired[k];
    for (uint32_t i = 0; i < nwait; i++)
        stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore signals[MAX_GEN + 1];
    for (uint32_t k = 0; k < ngen; k++)
        signals[k] = sc->present_sems[acq[k]];
    signals[ngen] = sc->present_sems[idx];
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = nwait,
        .pWaitSemaphores = waits,
        .pWaitDstStageMask = stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &f->cmd,
        .signalSemaphoreCount = ngen + 1,
        .pSignalSemaphores = signals,
    };
    VkResult r = d->QueueSubmit(queue, 1, &si, f->fence);
    free(waits);
    free(stages);
    if (r != VK_SUCCESS)
        return r;
    f->pending = true;
    if (ngen && getenv("KETTLE_FG_DUMP")) {
        const char *at = getenv("KETTLE_FG_DUMP_FRAME");
        if (sc->count == (uint64_t)(at ? atoll(at) : 300))
            dump(sc, f, ngen);
    }

    for (uint32_t k = 0; k < ngen; k++) {
        VkPresentInfoKHR gen = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sc->present_sems[acq[k]],
            .swapchainCount = 1,
            .pSwapchains = &sc->handle,
            .pImageIndices = &acq[k],
        };
        d->QueuePresentKHR(queue, &gen);
    }
    VkPresentInfoKHR real = *pi;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &sc->present_sems[idx];
    r = d->QueuePresentKHR(queue, &real);

    sc->shown += ngen + 1;
    sc->count++;
    sc->cur ^= 1;
    sc->have_prev = true;
    sc->have_mv = ngen && c.flow;
    return r;
}

// ---------- swapchains ----------

// Swapchain formats handled, as the history format their raw bits are copied into
static VkFormat hist_format(VkFormat f, bool *bgr)
{
    *bgr = false;
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        *bgr = true;
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        *bgr = true;
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                         const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    struct dev *d = find_dev(KEY(device));
    struct config c = config_get();
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSurfaceCapabilitiesKHR caps;
    bool bgr;
    VkFormat hist = hist_format(ci->imageFormat, &bgr);
    const char *why = NULL;
    if (hist == VK_FORMAT_UNDEFINED)
        why = "unsupported format";
    else if (ci->imageArrayLayers != 1)
        why = "layered images";
    else if (!d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR ||
             d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, ci->surface, &caps) != VK_SUCCESS)
        why = "no surface capabilities";
    else if ((caps.supportedUsageFlags & usage) != usage)
        why = "surface lacks transfer usage";
    if (why) {
        say("swapchain %ux%u format %d: %s, frame generation off", ci->imageExtent.width, ci->imageExtent.height,
            ci->imageFormat, why);
        return d->CreateSwapchainKHR(device, ci, alloc, out);
    }

    VkSwapchainCreateInfoKHR ci2 = *ci;
    ci2.imageUsage |= usage;
    uint32_t extra = 0;
    if (!c.preserve_images) {
        ci2.minImageCount += MAX_GEN;
        if (caps.maxImageCount && ci2.minImageCount > caps.maxImageCount)
            ci2.minImageCount = caps.maxImageCount;
        extra = ci2.minImageCount - ci->minImageCount;
    }
    // generated frames are queued back to back; any mode that replaces queued frames drops them
    if (c.fifo && !find_struct(ci->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT))
        ci2.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkResult r = d->CreateSwapchainKHR(device, &ci2, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    struct swapchain *sc = calloc(1, sizeof(*sc));
    if (sc && d->GetSwapchainImagesKHR(device, *out, &sc->nimages, NULL) == VK_SUCCESS &&
        (sc->images = calloc(sc->nimages, sizeof(VkImage))) &&
        (sc->present_sems = calloc(sc->nimages, sizeof(VkSemaphore))) &&
        d->GetSwapchainImagesKHR(device, *out, &sc->nimages, sc->images) == VK_SUCCESS) {
        sc->handle = *out;
        sc->dev = d;
        sc->extent = ci->imageExtent;
        sc->hist_format = hist;
        sc->gen_limit = MAX_GEN;
        sc->bgr = bgr;
        pthread_mutex_lock(&lock);
        sc->next = swapchains;
        swapchains = sc;
        pthread_mutex_unlock(&lock);
        say("swapchain %ux%u, %u images (%u extra), present mode %d", ci->imageExtent.width,
            ci->imageExtent.height, sc->nimages, extra, ci2.presentMode);
    } else if (sc) {
        free(sc->images);
        free(sc->present_sems);
        free(sc);
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                      const VkAllocationCallbacks *alloc)
{
    struct dev *d = find_dev(KEY(device));
    struct swapchain *sc = swapchain ? find_sc(swapchain, true) : NULL;
    if (sc) {
        sc_free(sc);
        free(sc->images);
        free(sc->present_sems);
        free(sc);
    }
    d->DestroySwapchainKHR(device, swapchain, alloc);
}

// ---------- instance, device, queues ----------

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *ci,
                                                     const VkAllocationCallbacks *alloc, VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    struct inst *i = calloc(1, sizeof(*i));
    if (!i) {
        ((PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance"))(*out, alloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    i->key = KEY(*out);
    i->handle = *out;
    i->gipa = gipa;
#define LOAD(n) i->n = (PFN_vk##n)gipa(*out, "vk" #n)
    LOAD(DestroyInstance);
    LOAD(GetPhysicalDeviceQueueFamilyProperties);
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(GetPhysicalDeviceProperties);
    LOAD(GetPhysicalDeviceSurfaceCapabilitiesKHR);
#undef LOAD
    pthread_mutex_lock(&lock);
    i->next = insts;
    insts = i;
    pthread_mutex_unlock(&lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    if (!instance)
        return;
    struct inst *i = unlink_key((void **)&insts, KEY(instance));
    if (i) {
        i->DestroyInstance(instance, alloc);
        free(i);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc, VkDevice *out)
{
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext, *cb = link;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    while (cb && !(cb->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                   cb->function == VK_LOADER_DATA_CALLBACK))
        cb = (VkLayerDeviceCreateInfo *)cb->pNext;
    struct inst *i = find_inst(KEY(phys));
    if (!link || !cb || !i)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(i->handle, "vkCreateDevice");
    VkResult r = create(phys, ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    struct dev *d = calloc(1, sizeof(*d));
    if (d) {
        i->GetPhysicalDeviceQueueFamilyProperties(phys, &d->nfamilies, NULL);
        d->families = calloc(d->nfamilies ? d->nfamilies : 1, sizeof(*d->families));
    }
    if (!d || !d->families) {
        ((PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice"))(*out, alloc);
        free(d);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    i->GetPhysicalDeviceQueueFamilyProperties(phys, &d->nfamilies, d->families);
    i->GetPhysicalDeviceMemoryProperties(phys, &d->mem);
    VkPhysicalDeviceProperties props;
    i->GetPhysicalDeviceProperties(phys, &props);
    d->ts_period = props.limits.timestampPeriod;
    d->key = KEY(*out);
    d->handle = *out;
    d->phys = phys;
    d->inst = i;
    d->gdpa = gdpa;
    d->set_loader_data = cb->u.pfnSetDeviceLoaderData;
#define X(n) d->n = (PFN_vk##n)gdpa(*out, "vk" #n);
    DEV_FUNCS(X)
#undef X
    pthread_mutex_lock(&lock);
    d->next = devs;
    devs = d;
    pthread_mutex_unlock(&lock);
    say("on %s", props.deviceName);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    if (!device)
        return;
    struct dev *d = unlink_key((void **)&devs, KEY(device));
    if (!d)
        return;
    // swapchains the game left behind
    pthread_mutex_lock(&lock);
    struct swapchain **p = &swapchains, *s, *mine = NULL;
    while ((s = *p)) {
        if (s->dev == d) {
            *p = s->next;
            s->next = mine;
            mine = s;
        } else {
            p = &s->next;
        }
    }
    pthread_mutex_unlock(&lock);
    while ((s = mine)) {
        mine = s->next;
        sc_free(s);
        free(s->images);
        free(s->present_sems);
        free(s);
    }
    dev_free_pipelines(d);
    d->DestroyDevice(device, alloc);
    free(d->families);
    free(d);
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out)
{
    struct dev *d = find_dev(KEY(device));
    d->GetDeviceQueue(device, family, index, out);
    note_queue(d, *out, family);
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *out)
{
    struct dev *d = find_dev(KEY(device));
    d->GetDeviceQueue2(device, info, out);
    note_queue(d, *out, info->queueFamilyIndex);
}

static PFN_vkVoidFunction intercept(const char *name);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name)
{
    struct dev *d = find_dev(KEY(device));
    PFN_vkVoidFunction next = d ? d->gdpa(device, name) : NULL;
    if (!next)
        return NULL;  // not enabled on this device (swapchain functions without the extension)
    PFN_vkVoidFunction f = intercept(name);
    return f ? f : next;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)CreateInstance;
    PFN_vkVoidFunction f = intercept(name);
    if (f || !instance)
        return f;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)DestroyInstance;
    if (!strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)CreateDevice;
    struct inst *i = find_inst(KEY(instance));
    return i ? i->gipa(instance, name) : NULL;
}

static PFN_vkVoidFunction intercept(const char *name)
{
    static const struct { const char *name; PFN_vkVoidFunction f; } funcs[] = {
        { "vkGetInstanceProcAddr", (PFN_vkVoidFunction)GetInstanceProcAddr },
        { "vkGetDeviceProcAddr", (PFN_vkVoidFunction)GetDeviceProcAddr },
        { "vkDestroyDevice", (PFN_vkVoidFunction)DestroyDevice },
        { "vkGetDeviceQueue", (PFN_vkVoidFunction)GetDeviceQueue },
        { "vkGetDeviceQueue2", (PFN_vkVoidFunction)GetDeviceQueue2 },
        { "vkCreateSwapchainKHR", (PFN_vkVoidFunction)CreateSwapchainKHR },
        { "vkDestroySwapchainKHR", (PFN_vkVoidFunction)DestroySwapchainKHR },
        { "vkQueuePresentKHR", (PFN_vkVoidFunction)QueuePresentKHR },
    };
    for (size_t i = 0; i < sizeof(funcs) / sizeof(*funcs); i++)
        if (!strcmp(name, funcs[i].name))
            return funcs[i].f;
    return NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
