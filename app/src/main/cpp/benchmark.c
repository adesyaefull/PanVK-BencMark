/* In-process Vulkan scene benchmark. */
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "scene_spv.h"
#include "bc_emulation.h"

#define FRAMES 2
#define MAX_IMG 16
#define WAIT_NS 2000000000ull
#define LOG_TAG "PanVKBench"

/* Phase tracking and cancellation are the only process-global benchmark state. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_phase[64] = "idle";
static volatile int g_stop;
static pthread_mutex_t g_preview_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_preview_thread;
static int g_preview_active;
static volatile uint32_t g_bc1_checksum;
static volatile int g_debug_logging;
static volatile int g_scene_rotation_millidegrees;

struct LoadedLibrary {
    char *path;
    int flags;
    void *handle;
    struct LoadedLibrary *next;
};

static pthread_mutex_t g_library_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct LoadedLibrary *g_loaded_libraries;

struct PreviewTask {
    ANativeWindow *window;
    char *driver;
    char *dependencies;
    char *bundled_libraries;
    char *log_path;
    uint32_t test_mode;
    uint32_t objects;
};

static void *
load_library_for_process_lifetime(const char *path, int flags)
{
    struct LoadedLibrary *library;
    void *handle;

    pthread_mutex_lock(&g_library_mutex);
    for (library = g_loaded_libraries; library; library = library->next) {
        if (library->flags == flags && strcmp(library->path, path) == 0) {
            pthread_mutex_unlock(&g_library_mutex);
            return library->handle;
        }
    }

    library = calloc(1, sizeof *library);
    if (!library) {
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }
    library->path = strdup(path);
    if (!library->path) {
        free(library);
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    handle = dlopen(path, flags);
    if (!handle) {
        free(library->path);
        free(library);
        pthread_mutex_unlock(&g_library_mutex);
        return NULL;
    }

    library->flags = flags;
    library->handle = handle;
    library->next = g_loaded_libraries;
    g_loaded_libraries = library;
    pthread_mutex_unlock(&g_library_mutex);
    return handle;
}

static void
set_phase(const char *name)
{
    pthread_mutex_lock(&g_mu);
    snprintf(g_phase, sizeof g_phase, "%s", name ? name : "idle");
    pthread_mutex_unlock(&g_mu);
}

struct Images {
    VkSwapchainKHR swap;
    VkExtent2D extent;
    uint32_t n;
    VkImage img[MAX_IMG];
    VkImageView view[MAX_IMG];
    VkImage depth_img[MAX_IMG];
    VkDeviceMemory depth_mem[MAX_IMG];
    VkImageView depth_view[MAX_IMG];
    VkFramebuffer fb[MAX_IMG];
    VkSemaphore rend[MAX_IMG];
};

struct Ctx {
    FILE *log;
    ANativeWindow *win;
    int failed;
    int timed_out;
    int slot;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;
    uint32_t qf;
    int subpass_merge_feedback_enabled;
    int astc_decode_shared_exponent_enabled;
    int tessellation_supported;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkFormat depth_format;
    VkQueue queue;
    VkSurfaceKHR surf;
    VkRenderPass rp;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkCommandPool pool;
    VkCommandBuffer cmd[FRAMES];
    VkSemaphore acq[FRAMES];
    VkFence fence[FRAMES];
    VkFence img_fence[MAX_IMG];
    struct Images sc;
    int frame_boundary_enabled;
    int swapchain_compression_enabled;
    uint32_t instance_extension_count;
    uint32_t instance_layer_count;
    uint32_t device_extension_count;
    uint32_t core_feature_supported_count;
    uint32_t feature_extension_supported_count;
    void *icd;
    void *dependency_handles[2];
    uint32_t dependency_count;
    uint32_t duration_seconds;
    uint32_t objects;
    uint32_t test_mode;
    uint32_t scene_mode;
    struct timespec animation_start;
    double *frame_ms;
    size_t frame_count;
    size_t frame_capacity;

    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
    PFN_vkDestroyInstance vkDestroyInstance;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
    PFN_vkCreateDevice vkCreateDevice;
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkGetDeviceQueue vkGetDeviceQueue;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR vkQueuePresentKHR;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCreateRenderPass vkCreateRenderPass;
    PFN_vkCreateRenderPass2KHR vkCreateRenderPass2KHR;
    PFN_vkDestroyRenderPass vkDestroyRenderPass;
    PFN_vkCreateFramebuffer vkCreateFramebuffer;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline vkDestroyPipeline;
    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkResetCommandBuffer vkResetCommandBuffer;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdDraw vkCmdDraw;
    PFN_vkCmdPushConstants vkCmdPushConstants;
    PFN_vkCmdSetViewport vkCmdSetViewport;
    PFN_vkCmdSetScissor vkCmdSetScissor;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkCreateFence vkCreateFence;
    PFN_vkDestroyFence vkDestroyFence;
    PFN_vkWaitForFences vkWaitForFences;
    PFN_vkResetFences vkResetFences;
    PFN_vkCreateSemaphore vkCreateSemaphore;
    PFN_vkDestroySemaphore vkDestroySemaphore;
    PFN_vkQueueSubmit vkQueueSubmit;
};

static void
lg(struct Ctx *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (c && c->log) {
        fputs(buf, c->log);
        fputc('\n', c->log);
        fflush(c->log);
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", buf);
}

static void
lg_debug(struct Ctx *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!__atomic_load_n(&g_debug_logging, __ATOMIC_RELAXED))
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    lg(c, "%s", buf);
}

static int
fail(struct Ctx *c, const char *what, VkResult res)
{
    char phase[64];
    if (res == VK_TIMEOUT || res == VK_NOT_READY)
        c->timed_out = 1;
    if (c->failed)
        return 0;
    c->failed = 1;
    pthread_mutex_lock(&g_mu);
    snprintf(phase, sizeof phase, "%s", g_phase);
    pthread_mutex_unlock(&g_mu);
    if (res == VK_TIMEOUT || res == VK_NOT_READY)
        lg(c, "FAIL hang in %s", phase);
    lg(c, "FAIL %s res=%d phase=%s", what, (int)res, phase);
    return 0;
}

static int
chk(struct Ctx *c, const char *what, VkResult res)
{
    return res == VK_SUCCESS ? 1 : fail(c, what, res);
}

/* Create outputs are undefined on failure. Zero the caller's 8-byte handle. */
static int
chk0(struct Ctx *c, const char *what, VkResult res, void *out)
{
    if (res == VK_SUCCESS)
        return 1;
    if (out) {
        uint64_t z = 0;
        memcpy(out, &z, sizeof z);
    }
    return fail(c, what, res);
}

static double
ms_exact(const struct timespec *a)
{
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a->tv_sec) * 1e3 + (b.tv_nsec - a->tv_nsec) / 1e6;
}

static double
ms_between(const struct timespec *start, const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1e3 +
           (end->tv_nsec - start->tv_nsec) / 1e6;
}

static const char *
present_mode_name(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "FIFO_RELAXED";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    default:
        return "UNKNOWN";
    }
}

#define LOAD_I(c, gipa, n)                                                     \
    do {                                                                       \
        (c)->vk##n = (PFN_vk##n)(gipa)((c)->inst, "vk" #n);                    \
        if (!(c)->vk##n)                                                       \
            return fail((c), "vk" #n, VK_ERROR_EXTENSION_NOT_PRESENT);         \
    } while (0)

#define LOAD_D(c, n)                                                           \
    do {                                                                       \
        (c)->vk##n = (PFN_vk##n)(c)->vkGetDeviceProcAddr((c)->dev, "vk" #n);   \
        if (!(c)->vk##n)                                                       \
            return fail((c), "vk" #n, VK_ERROR_EXTENSION_NOT_PRESENT);         \
    } while (0)

static int
load_instance(struct Ctx *c, PFN_vkGetInstanceProcAddr gipa)
{
    LOAD_I(c, gipa, DestroyInstance);
    LOAD_I(c, gipa, EnumeratePhysicalDevices);
    LOAD_I(c, gipa, EnumerateDeviceExtensionProperties);
    LOAD_I(c, gipa, GetPhysicalDeviceFeatures2);
    LOAD_I(c, gipa, GetPhysicalDeviceProperties);
    LOAD_I(c, gipa, GetPhysicalDeviceFormatProperties);
    LOAD_I(c, gipa, GetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(c, gipa, GetPhysicalDeviceMemoryProperties);
    LOAD_I(c, gipa, GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_I(c, gipa, GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_I(c, gipa, GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_I(c, gipa, GetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_I(c, gipa, CreateAndroidSurfaceKHR);
    LOAD_I(c, gipa, DestroySurfaceKHR);
    LOAD_I(c, gipa, CreateDevice);
    LOAD_I(c, gipa, GetDeviceProcAddr);
    return 1;
}

static int
log_instance_capabilities(struct Ctx *c, PFN_vkGetInstanceProcAddr gipa)
{
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkEnumerateInstanceLayerProperties enumerate_layers =
        (PFN_vkEnumerateInstanceLayerProperties)gipa(
            VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    VkExtensionProperties *extensions = NULL;
    VkLayerProperties *layers = NULL;
    uint32_t count = 0;
    VkResult result;

    if (!enumerate_extensions || !enumerate_layers)
        return fail(c, "Vulkan instance enumeration entry points",
                    VK_ERROR_EXTENSION_NOT_PRESENT);
    result = enumerate_extensions(NULL, &count, NULL);
    if (result != VK_SUCCESS)
        return fail(c, "vkEnumerateInstanceExtensionProperties", result);
    extensions = calloc(count ? count : 1, sizeof *extensions);
    if (!extensions)
        return fail(c, "calloc instance extensions", VK_ERROR_OUT_OF_HOST_MEMORY);
    result = enumerate_extensions(NULL, &count, extensions);
    if (result != VK_SUCCESS) {
        free(extensions);
        return fail(c, "vkEnumerateInstanceExtensionProperties", result);
    }
    c->instance_extension_count = count;
    {
        int surface_available = 0, android_surface_available = 0;
        for (uint32_t i = 0; i < count; i++) {
            surface_available |=
                strcmp(extensions[i].extensionName,
                       VK_KHR_SURFACE_EXTENSION_NAME) == 0;
            android_surface_available |=
                strcmp(extensions[i].extensionName,
                       VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) == 0;
            lg_debug(c, "VULKAN_INSTANCE_EXTENSION name=%s spec=%u",
               extensions[i].extensionName, extensions[i].specVersion);
        }
        lg(c, "VULKAN_INSTANCE_REQUIRED_EXTENSION name=%s available=%u",
           VK_KHR_SURFACE_EXTENSION_NAME, surface_available);
        lg(c, "VULKAN_INSTANCE_REQUIRED_EXTENSION name=%s available=%u",
           VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, android_surface_available);
    }
    free(extensions);

    count = 0;
    result = enumerate_layers(&count, NULL);
    if (result != VK_SUCCESS)
        return fail(c, "vkEnumerateInstanceLayerProperties", result);
    layers = calloc(count ? count : 1, sizeof *layers);
    if (!layers)
        return fail(c, "calloc instance layers", VK_ERROR_OUT_OF_HOST_MEMORY);
    result = enumerate_layers(&count, layers);
    if (result != VK_SUCCESS) {
        free(layers);
        return fail(c, "vkEnumerateInstanceLayerProperties", result);
    }
    c->instance_layer_count = count;
    for (uint32_t i = 0; i < count; i++)
        lg_debug(c, "VULKAN_INSTANCE_LAYER name=%s spec=%u implementation=%u",
           layers[i].layerName, layers[i].specVersion,
           layers[i].implementationVersion);
    free(layers);
    lg(c, "VULKAN_INSTANCE_SUMMARY extensions=%u layers=%u requested_extensions=2",
       c->instance_extension_count, c->instance_layer_count);
    return 1;
}

static int
load_device(struct Ctx *c)
{
    LOAD_D(c, DestroyDevice);
    LOAD_D(c, GetDeviceQueue);
    LOAD_D(c, DeviceWaitIdle);
    LOAD_D(c, CreateSwapchainKHR);
    LOAD_D(c, DestroySwapchainKHR);
    LOAD_D(c, GetSwapchainImagesKHR);
    LOAD_D(c, AcquireNextImageKHR);
    LOAD_D(c, QueuePresentKHR);
    LOAD_D(c, CreateImageView);
    LOAD_D(c, DestroyImageView);
    LOAD_D(c, CreateImage);
    LOAD_D(c, DestroyImage);
    LOAD_D(c, GetImageMemoryRequirements);
    LOAD_D(c, BindImageMemory);
    LOAD_D(c, CreateRenderPass);
    LOAD_D(c, DestroyRenderPass);
    LOAD_D(c, CreateFramebuffer);
    LOAD_D(c, DestroyFramebuffer);
    LOAD_D(c, CreateShaderModule);
    LOAD_D(c, DestroyShaderModule);
    LOAD_D(c, CreatePipelineLayout);
    LOAD_D(c, DestroyPipelineLayout);
    LOAD_D(c, CreateGraphicsPipelines);
    LOAD_D(c, DestroyPipeline);
    LOAD_D(c, CreateCommandPool);
    LOAD_D(c, DestroyCommandPool);
    LOAD_D(c, AllocateCommandBuffers);
    LOAD_D(c, ResetCommandBuffer);
    LOAD_D(c, BeginCommandBuffer);
    LOAD_D(c, EndCommandBuffer);
    LOAD_D(c, CmdBeginRenderPass);
    LOAD_D(c, CmdEndRenderPass);
    LOAD_D(c, CmdBindPipeline);
    LOAD_D(c, CmdDraw);
    LOAD_D(c, CmdPushConstants);
    LOAD_D(c, CmdSetViewport);
    LOAD_D(c, CmdSetScissor);
    LOAD_D(c, AllocateMemory);
    LOAD_D(c, FreeMemory);
    LOAD_D(c, CreateFence);
    LOAD_D(c, DestroyFence);
    LOAD_D(c, WaitForFences);
    LOAD_D(c, ResetFences);
    LOAD_D(c, CreateSemaphore);
    LOAD_D(c, DestroySemaphore);
    LOAD_D(c, QueueSubmit);
    if (c->subpass_merge_feedback_enabled) {
        c->vkCreateRenderPass2KHR =
            (PFN_vkCreateRenderPass2KHR)c->vkGetDeviceProcAddr(
                c->dev, "vkCreateRenderPass2KHR");
        if (!c->vkCreateRenderPass2KHR)
            return fail(c, "vkCreateRenderPass2KHR",
                        VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    return 1;
}

static void
images_destroy(struct Ctx *c, struct Images *im)
{
    uint32_t i;
    if (c->dev) {
        for (i = 0; i < im->n && i < MAX_IMG; i++) {
            if (im->fb[i] && c->vkDestroyFramebuffer)
                c->vkDestroyFramebuffer(c->dev, im->fb[i], NULL);
            if (im->view[i] && c->vkDestroyImageView)
                c->vkDestroyImageView(c->dev, im->view[i], NULL);
            if (im->depth_view[i] && c->vkDestroyImageView)
                c->vkDestroyImageView(c->dev, im->depth_view[i], NULL);
            if (im->depth_img[i] && c->vkDestroyImage)
                c->vkDestroyImage(c->dev, im->depth_img[i], NULL);
            if (im->depth_mem[i] && c->vkFreeMemory)
                c->vkFreeMemory(c->dev, im->depth_mem[i], NULL);
            if (im->rend[i] && c->vkDestroySemaphore)
                c->vkDestroySemaphore(c->dev, im->rend[i], NULL);
        }
        if (im->swap && c->vkDestroySwapchainKHR)
            c->vkDestroySwapchainKHR(c->dev, im->swap, NULL);
    }
    memset(im, 0, sizeof *im);
}

static int
shutdown(struct Ctx *c)
{
    uint32_t i;
    if (c->dev && c->vkDeviceWaitIdle && !c->timed_out) {
        VkResult r = c->vkDeviceWaitIdle(c->dev);
        if (r != VK_SUCCESS)
            fail(c, "vkDeviceWaitIdle", r);
    }
    images_destroy(c, &c->sc);
    memset(c->img_fence, 0, sizeof c->img_fence);
    if (c->dev) {
        for (i = 0; i < FRAMES; i++) {
            if (c->fence[i] && c->vkDestroyFence)
                c->vkDestroyFence(c->dev, c->fence[i], NULL);
            if (c->acq[i] && c->vkDestroySemaphore)
                c->vkDestroySemaphore(c->dev, c->acq[i], NULL);
            c->fence[i] = VK_NULL_HANDLE;
            c->acq[i] = VK_NULL_HANDLE;
        }
        if (c->pool && c->vkDestroyCommandPool)
            c->vkDestroyCommandPool(c->dev, c->pool, NULL);
        if (c->pipe && c->vkDestroyPipeline)
            c->vkDestroyPipeline(c->dev, c->pipe, NULL);
        if (c->layout && c->vkDestroyPipelineLayout)
            c->vkDestroyPipelineLayout(c->dev, c->layout, NULL);
        if (c->rp && c->vkDestroyRenderPass)
            c->vkDestroyRenderPass(c->dev, c->rp, NULL);
        c->pool = VK_NULL_HANDLE;
        c->pipe = VK_NULL_HANDLE;
        c->layout = VK_NULL_HANDLE;
        c->rp = VK_NULL_HANDLE;
        if (c->vkDestroyDevice)
            c->vkDestroyDevice(c->dev, NULL);
        c->dev = VK_NULL_HANDLE;
    }
    if (c->surf && c->inst && c->vkDestroySurfaceKHR)
        c->vkDestroySurfaceKHR(c->inst, c->surf, NULL);
    c->surf = VK_NULL_HANDLE;
    if (c->inst && c->vkDestroyInstance)
        c->vkDestroyInstance(c->inst, NULL);
    c->inst = VK_NULL_HANDLE;
    /* ICD TLS destructors may run after this context is gone; keep code mapped. */
    c->icd = NULL;
    c->dependency_count = 0;
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }
    return c->failed ? 0 : 1;
}

static int
preload_dependency(struct Ctx *c, const char *directory, const char *name)
{
    char path[1024];
    void *handle;
    int length;
    if (!directory || !directory[0] || c->dependency_count >= 2)
        return 0;
    length = snprintf(path, sizeof path, "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof path) {
        fail(c, "dependency path too long", VK_ERROR_INITIALIZATION_FAILED);
        return -1;
    }
    if (access(path, R_OK) != 0)
        return 0;
    handle = load_library_for_process_lifetime(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char *error = dlerror();
        lg(c, "DEPENDENCY_LOAD name=%s path=%s error=%s",
           name, path, error ? error : "unknown");
        return 0;
    }
    c->dependency_handles[c->dependency_count++] = handle;
    lg(c, "DEPENDENCY_LOADED name=%s path=%s", name, path);
    return 1;
}

static int
preload_dependency_from_dirs(struct Ctx *c, const char *import_directory,
                             const char *bundled_directory, const char *name)
{
    int loaded = preload_dependency(c, import_directory, name);
    if (loaded < 0)
        return 0;
    if (loaded)
        return 1;
    loaded = preload_dependency(c, bundled_directory, name);
    if (loaded < 0)
        return 0;
    if (!loaded)
        lg(c, "DEPENDENCY_NOT_FOUND name=%s", name);
    return 1;
}

static int
open_icd(struct Ctx *c, const char *path, const char *dependency_directory,
         const char *bundled_library_directory)
{
    PFN_vkGetInstanceProcAddr gipa;
    if (strcmp(path, "libvulkan.so") != 0 &&
        (!preload_dependency_from_dirs(c, dependency_directory,
                                       bundled_library_directory, "libc++.so") ||
         !preload_dependency_from_dirs(c, dependency_directory,
                                       bundled_library_directory, "libdrm.so")))
        return 0;
    c->icd = load_library_for_process_lifetime(path, RTLD_NOW | RTLD_LOCAL);
    if (!c->icd) {
        const char *error = dlerror();
        lg(c, "ICD_LOAD path=%s error=%s", path, error ? error : "unknown");
        return fail(c, "dlopen", -1);
    }
    lg(c, "ICD_LOAD path=%s status=loaded", path);
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(c->icd, "vk_icdGetInstanceProcAddr");
    if (!gipa)
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(c->icd, "vkGetInstanceProcAddr");
    if (!gipa) {
        const char *error = dlerror();
        lg(c, "ICD_SYMBOL error=%s", error ? error : "missing Vulkan entry point");
        return fail(c, "dlsym", -1);
    }
    if (!log_instance_capabilities(c, gipa))
        return 0;
    {
        PFN_vkCreateInstance create =
            (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
        const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                              VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                 .pApplicationName = "PanVK Bench",
                                 .apiVersion = VK_API_VERSION_1_1};
        VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                    .pApplicationInfo = &app,
                                    .enabledExtensionCount = 2,
                                    .ppEnabledExtensionNames = exts};
        if (!create)
            return fail(c, "vkCreateInstance", VK_ERROR_EXTENSION_NOT_PRESENT);
        if (!chk0(c, "vkCreateInstance", create(&ici, NULL, &c->inst), &c->inst))
            return 0;
        if (!load_instance(c, gipa))
            return 0;
    }
    return 1;
}

static int
pick_gpu(struct Ctx *c)
{
    uint32_t nd = 0, d, selected_gpu = 0;
    VkPhysicalDevice *devs;
    int found = 0;
    if (!chk(c, "vkEnumeratePhysicalDevices",
             c->vkEnumeratePhysicalDevices(c->inst, &nd, NULL)))
        return 0;
    if (nd == 0)
        return fail(c, "vkEnumeratePhysicalDevices", VK_ERROR_INITIALIZATION_FAILED);
    devs = calloc(nd, sizeof *devs);
    if (!devs)
        return fail(c, "calloc", -12);
    if (!chk(c, "vkEnumeratePhysicalDevices",
             c->vkEnumeratePhysicalDevices(c->inst, &nd, devs))) {
        free(devs);
        return 0;
    }
    for (d = 0; d < nd && !found; d++) {
        uint32_t nq = 0, i;
        VkQueueFamilyProperties *qp;
        c->vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, NULL);
        if (nq == 0)
            continue;
        qp = calloc(nq, sizeof *qp);
        if (!qp) {
            free(devs);
            return fail(c, "calloc", -12);
        }
        c->vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qp);
        for (i = 0; i < nq; i++) {
            VkBool32 sup = VK_FALSE;
            VkResult r;
            if (!(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || qp[i].queueCount == 0)
                continue;
            r = c->vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], i, c->surf, &sup);
            if (r != VK_SUCCESS) {
                free(qp);
                free(devs);
                return fail(c, "vkGetPhysicalDeviceSurfaceSupportKHR", r);
            }
            if (sup) {
                c->phys = devs[d];
                c->qf = i;
                selected_gpu = d;
                found = 1;
                break;
            }
        }
        free(qp);
    }
    free(devs);
    if (!found)
        return fail(c, "queue", VK_ERROR_INITIALIZATION_FAILED);
    lg(c, "VULKAN_PHYSICAL_DEVICE_SELECTION total=%u selected_index=%u queue_family_index=%u",
       nd, selected_gpu, c->qf);
    {
        VkPhysicalDeviceProperties properties;
        uint32_t extension_count = 0, i;
        VkExtensionProperties *extensions = NULL;
        c->vkGetPhysicalDeviceProperties(c->phys, &properties);
        lg(c, "DEVICE name=%s vendor=0x%04x device=0x%04x api=%u.%u.%u driver_version=%u type=%u",
           properties.deviceName, properties.vendorID, properties.deviceID,
           VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
           VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion,
           (unsigned)properties.deviceType);
        lg(c, "VULKAN_LIMITS max_image_2d=%u max_image_array_layers=%u max_color_attachments=%u max_bound_descriptor_sets=%u max_push_constants=%u max_vertex_attributes=%u max_compute_shared_memory=%u max_sampler_anisotropy=%.1f max_tessellation_level=%u",
           properties.limits.maxImageDimension2D,
           properties.limits.maxImageArrayLayers,
           properties.limits.maxColorAttachments,
           properties.limits.maxBoundDescriptorSets,
           properties.limits.maxPushConstantsSize,
           properties.limits.maxVertexInputAttributes,
           properties.limits.maxComputeSharedMemorySize,
           properties.limits.maxSamplerAnisotropy,
           properties.limits.maxTessellationGenerationLevel);
        {
            VkPhysicalDeviceMemoryProperties memory;
            uint64_t local_heap_bytes = 0;
            c->vkGetPhysicalDeviceMemoryProperties(c->phys, &memory);
            for (uint32_t heap = 0; heap < memory.memoryHeapCount; heap++) {
                lg_debug(c, "VULKAN_MEMORY_HEAP index=%u size_bytes=%llu flags=0x%x",
                   heap, (unsigned long long)memory.memoryHeaps[heap].size,
                   memory.memoryHeaps[heap].flags);
                if (memory.memoryHeaps[heap].flags &
                    VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    local_heap_bytes += memory.memoryHeaps[heap].size;
            }
            lg(c, "VULKAN_MEMORY_SUMMARY heaps=%u memory_types=%u device_local_heap_bytes=%llu",
               memory.memoryHeapCount, memory.memoryTypeCount,
               (unsigned long long)local_heap_bytes);
        }
        if (!chk(c, "vkEnumerateDeviceExtensionProperties",
                 c->vkEnumerateDeviceExtensionProperties(c->phys, NULL,
                                                         &extension_count, NULL)))
            return 0;
        extensions = calloc(extension_count ? extension_count : 1,
                            sizeof *extensions);
        if (!extensions)
            return fail(c, "calloc device extensions", VK_ERROR_OUT_OF_HOST_MEMORY);
        if (!chk(c, "vkEnumerateDeviceExtensionProperties",
                 c->vkEnumerateDeviceExtensionProperties(c->phys, NULL,
                                                         &extension_count, extensions))) {
            free(extensions);
            return 0;
        }
        c->device_extension_count = extension_count;
        for (i = 0; i < extension_count; i++)
            lg_debug(c, "VULKAN_DEVICE_EXTENSION name=%s spec=%u",
               extensions[i].extensionName, extensions[i].specVersion);
        free(extensions);
    }
    return 1;
}

static int
pick_format(struct Ctx *c)
{
    uint32_t n = 0, i;
    VkSurfaceFormatKHR *fmts;
    int unorm = -1, srgb = -1;
    if (!chk(c, "vkGetPhysicalDeviceSurfaceFormatsKHR",
             c->vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, c->surf, &n, NULL)))
        return 0;
    if (n == 0)
        return fail(c, "vkGetPhysicalDeviceSurfaceFormatsKHR",
                    VK_ERROR_FORMAT_NOT_SUPPORTED);
    fmts = calloc(n, sizeof *fmts);
    if (!fmts)
        return fail(c, "calloc", -12);
    if (!chk(c, "vkGetPhysicalDeviceSurfaceFormatsKHR",
             c->vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, c->surf, &n, fmts))) {
        free(fmts);
        return 0;
    }
    if (n == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        c->format = VK_FORMAT_R8G8B8A8_UNORM;
        c->color_space = fmts[0].colorSpace;
        free(fmts);
        return 1;
    }
    for (i = 0; i < n; i++) {
        if (unorm < 0 && fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM)
            unorm = (int)i;
        if (srgb < 0 && fmts[i].format == VK_FORMAT_R8G8B8A8_SRGB)
            srgb = (int)i;
    }
    i = unorm >= 0 ? (uint32_t)unorm : srgb >= 0 ? (uint32_t)srgb : 0;
    c->format = fmts[i].format;
    c->color_space = fmts[i].colorSpace;
    free(fmts);
    return 1;
}

static int
pick_depth_format(struct Ctx *c)
{
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM,
    };
    uint32_t i;
    for (i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties properties;
        c->vkGetPhysicalDeviceFormatProperties(c->phys, candidates[i], &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            c->depth_format = candidates[i];
            lg(c, "DEPTH_FORMAT format=%u", (unsigned)c->depth_format);
            return 1;
        }
    }
    return fail(c, "depth attachment format", VK_ERROR_FORMAT_NOT_SUPPORTED);
}

static int
make_rp(struct Ctx *c)
{
    VkAttachmentDescription attachments[2] = {
        {
            .format = c->format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        {
            .format = c->depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &ref,
                                .pDepthStencilAttachment = &depth_ref};
    VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL,
                               .dstSubpass = 0,
                               .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                               .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 2,
                                 .pAttachments = attachments,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub,
                                 .dependencyCount = 1,
                                 .pDependencies = &dep};
    return chk0(c, "vkCreateRenderPass", c->vkCreateRenderPass(c->dev, &rp, NULL, &c->rp), &c->rp);
}

static int
make_pipeline(struct Ctx *c)
{
    VkShaderModule modules[4] = {VK_NULL_HANDLE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkPipelineShaderStageCreateInfo st[4] = {0};
    VkPipelineTessellationStateCreateInfo tess = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints = 3,
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = c->test_mode == 5 ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                                      : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, 1, 1, 0, 1};
    VkRect2D sc = {{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &vp,
        .scissorCount = 1,
        .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.f};
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds_depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = c->test_mode == 9 || c->test_mode == 12
                                ? VK_FALSE : VK_TRUE,
        .depthWriteEnable = c->test_mode == 8 || c->test_mode == 9 ||
                                    c->test_mode == 12 || c->test_mode == 13
                                ? VK_FALSE : VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .maxDepthBounds = 1.f,
    };
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = c->test_mode == 5 || c->test_mode == 8 ||
                               c->test_mode == 13
                           ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xf,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn};
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                      (c->test_mode == 5
                           ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                           : 0),
        .offset = 0,
        .size = sizeof(float) * 5,
    };
    VkPipelineLayoutCreateInfo pl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range};
    VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = c->test_mode == 5 ? 4u : 2u,
                                       .pStages = st,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vps,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds_depth,
                                       .pColorBlendState = &cb,
                                       .pDynamicState = &ds,
                                       .renderPass = c->rp};
    int ok = 0;
    uint32_t stage_count = c->test_mode == 5 ? 4u : 2u;

    if (c->test_mode == 5 && !c->tessellation_supported)
        return fail(c, "tessellationShader feature unavailable",
                    VK_ERROR_FEATURE_NOT_PRESENT);

    mi.codeSize = sizeof bench_vs_spv;
    mi.pCode = bench_vs_spv;
    if (!chk0(c, "vkCreateShaderModule(vertex)",
              c->vkCreateShaderModule(c->dev, &mi, NULL, &modules[0]), &modules[0]))
        goto out;
    if (c->test_mode == 5) {
        mi.codeSize = sizeof bench_tesc_spv;
        mi.pCode = bench_tesc_spv;
        if (!chk0(c, "vkCreateShaderModule(tess-control)",
                  c->vkCreateShaderModule(c->dev, &mi, NULL, &modules[1]), &modules[1]))
            goto out;
        mi.codeSize = sizeof bench_tese_spv;
        mi.pCode = bench_tese_spv;
        if (!chk0(c, "vkCreateShaderModule(tess-evaluation)",
                  c->vkCreateShaderModule(c->dev, &mi, NULL, &modules[2]), &modules[2]))
            goto out;
    }
    mi.codeSize = sizeof bench_fs_spv;
    mi.pCode = bench_fs_spv;
    if (!chk0(c, "vkCreateShaderModule(fragment)",
              c->vkCreateShaderModule(c->dev, &mi, NULL, &modules[3]), &modules[3]))
        goto out;

    for (uint32_t i = 0; i < stage_count; i++)
        st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = modules[0];
    st[0].pName = "main";
    if (c->test_mode == 5) {
        st[1].stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        st[1].module = modules[1];
        st[1].pName = "main";
        st[2].stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        st[2].module = modules[2];
        st[2].pName = "main";
        st[3].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[3].module = modules[3];
        st[3].pName = "main";
        gp.pTessellationState = &tess;
    } else {
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = modules[3];
        st[1].pName = "main";
    }
    if (!chk0(c, "vkCreatePipelineLayout",
              c->vkCreatePipelineLayout(c->dev, &pl, NULL, &c->layout), &c->layout))
        goto out;
    gp.layout = c->layout;
    ok = chk0(c, "vkCreateGraphicsPipelines",
              c->vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &gp, NULL, &c->pipe),
              &c->pipe);
out:
    for (uint32_t i = 0; i < sizeof modules / sizeof modules[0]; i++)
        if (modules[i])
            c->vkDestroyShaderModule(c->dev, modules[i], NULL);
    return ok;
}

static int
make_sync(struct Ctx *c)
{
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = c->qf};
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = FRAMES};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                            .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    int i;
    if (!chk0(c, "vkCreateCommandPool", c->vkCreateCommandPool(c->dev, &pi, NULL, &c->pool),
              &c->pool))
        return 0;
    ai.commandPool = c->pool;
    if (!chk(c, "vkAllocateCommandBuffers",
             c->vkAllocateCommandBuffers(c->dev, &ai, c->cmd)))
        return 0;
    for (i = 0; i < FRAMES; i++) {
        if (!chk0(c, "vkCreateFence", c->vkCreateFence(c->dev, &fi, NULL, &c->fence[i]),
                  &c->fence[i]))
            return 0;
        if (!chk0(c, "vkCreateSemaphore", c->vkCreateSemaphore(c->dev, &si, NULL, &c->acq[i]),
                  &c->acq[i]))
            return 0;
    }
    return 1;
}

static int
device_has_extension(struct Ctx *c, const char *name)
{
    uint32_t count = 0;
    VkExtensionProperties *extensions;
    int found = 0;
    if (!chk(c, "vkEnumerateDeviceExtensionProperties",
             c->vkEnumerateDeviceExtensionProperties(c->phys, NULL, &count, NULL)))
        return -1;
    extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) {
        fail(c, "calloc device extensions", VK_ERROR_OUT_OF_HOST_MEMORY);
        return -1;
    }
    if (!chk(c, "vkEnumerateDeviceExtensionProperties",
             c->vkEnumerateDeviceExtensionProperties(c->phys, NULL, &count, extensions))) {
        free(extensions);
        return -1;
    }
    for (uint32_t i = 0; i < count; i++)
        found |= strcmp(extensions[i].extensionName, name) == 0;
    free(extensions);
    return found;
}

static uint32_t
log_core_features(struct Ctx *c, const VkPhysicalDeviceFeatures *features)
{
    uint32_t supported = 0;
#define LOG_FEATURE(name)                                                     \
    do {                                                                      \
        uint32_t value = !!features->name;                                    \
        supported += value;                                                   \
        lg_debug(c, "CORE_FEATURE name=%s supported=%u", #name, value);       \
    } while (0)
    LOG_FEATURE(robustBufferAccess);
    LOG_FEATURE(fullDrawIndexUint32);
    LOG_FEATURE(imageCubeArray);
    LOG_FEATURE(independentBlend);
    LOG_FEATURE(geometryShader);
    LOG_FEATURE(tessellationShader);
    LOG_FEATURE(sampleRateShading);
    LOG_FEATURE(dualSrcBlend);
    LOG_FEATURE(logicOp);
    LOG_FEATURE(multiDrawIndirect);
    LOG_FEATURE(drawIndirectFirstInstance);
    LOG_FEATURE(depthClamp);
    LOG_FEATURE(depthBiasClamp);
    LOG_FEATURE(fillModeNonSolid);
    LOG_FEATURE(depthBounds);
    LOG_FEATURE(wideLines);
    LOG_FEATURE(largePoints);
    LOG_FEATURE(alphaToOne);
    LOG_FEATURE(multiViewport);
    LOG_FEATURE(samplerAnisotropy);
    LOG_FEATURE(textureCompressionETC2);
    LOG_FEATURE(textureCompressionASTC_LDR);
    LOG_FEATURE(textureCompressionBC);
    LOG_FEATURE(occlusionQueryPrecise);
    LOG_FEATURE(pipelineStatisticsQuery);
    LOG_FEATURE(vertexPipelineStoresAndAtomics);
    LOG_FEATURE(fragmentStoresAndAtomics);
    LOG_FEATURE(shaderTessellationAndGeometryPointSize);
    LOG_FEATURE(shaderImageGatherExtended);
    LOG_FEATURE(shaderStorageImageExtendedFormats);
    LOG_FEATURE(shaderStorageImageMultisample);
    LOG_FEATURE(shaderStorageImageReadWithoutFormat);
    LOG_FEATURE(shaderStorageImageWriteWithoutFormat);
    LOG_FEATURE(shaderUniformBufferArrayDynamicIndexing);
    LOG_FEATURE(shaderSampledImageArrayDynamicIndexing);
    LOG_FEATURE(shaderStorageBufferArrayDynamicIndexing);
    LOG_FEATURE(shaderStorageImageArrayDynamicIndexing);
    LOG_FEATURE(shaderClipDistance);
    LOG_FEATURE(shaderCullDistance);
    LOG_FEATURE(shaderFloat64);
    LOG_FEATURE(shaderInt64);
    LOG_FEATURE(shaderInt16);
    LOG_FEATURE(shaderResourceResidency);
    LOG_FEATURE(shaderResourceMinLod);
    LOG_FEATURE(sparseBinding);
    LOG_FEATURE(sparseResidencyBuffer);
    LOG_FEATURE(sparseResidencyImage2D);
    LOG_FEATURE(sparseResidencyImage3D);
    LOG_FEATURE(sparseResidency2Samples);
    LOG_FEATURE(sparseResidency4Samples);
    LOG_FEATURE(sparseResidency8Samples);
    LOG_FEATURE(sparseResidency16Samples);
    LOG_FEATURE(sparseResidencyAliased);
    LOG_FEATURE(variableMultisampleRate);
    LOG_FEATURE(inheritedQueries);
#undef LOG_FEATURE
    c->core_feature_supported_count = supported;
    lg(c, "VULKAN_CORE_FEATURE_SUMMARY supported=%u total=55",
       supported);
    return supported;
}

static int
log_texture_formats(struct Ctx *c)
{
    static const struct {
        const char *name;
        VkFormat format;
    } formats[] = {
        {"R8_UNORM", VK_FORMAT_R8_UNORM},
        {"RG8_UNORM", VK_FORMAT_R8G8_UNORM},
        {"RGBA8_UNORM", VK_FORMAT_R8G8B8A8_UNORM},
        {"RGBA8_SRGB", VK_FORMAT_R8G8B8A8_SRGB},
        {"BGRA8_UNORM", VK_FORMAT_B8G8R8A8_UNORM},
        {"RGBA16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT},
        {"RGBA32_SFLOAT", VK_FORMAT_R32G32B32A32_SFLOAT},
        {"D16_UNORM", VK_FORMAT_D16_UNORM},
        {"D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT},
        {"D32_SFLOAT", VK_FORMAT_D32_SFLOAT},
        {"ETC2_RGB8", VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK},
        {"ASTC_4X4_UNORM", VK_FORMAT_ASTC_4x4_UNORM_BLOCK},
        {"ASTC_4X4_SRGB", VK_FORMAT_ASTC_4x4_SRGB_BLOCK},
        {"BC1_RGB_UNORM", VK_FORMAT_BC1_RGB_UNORM_BLOCK},
        {"BC1_RGB_SRGB", VK_FORMAT_BC1_RGB_SRGB_BLOCK},
        {"BC2_UNORM", VK_FORMAT_BC2_UNORM_BLOCK},
        {"BC2_SRGB", VK_FORMAT_BC2_SRGB_BLOCK},
        {"BC3_UNORM", VK_FORMAT_BC3_UNORM_BLOCK},
        {"BC3_SRGB", VK_FORMAT_BC3_SRGB_BLOCK},
        {"BC4_UNORM", VK_FORMAT_BC4_UNORM_BLOCK},
        {"BC4_SNORM", VK_FORMAT_BC4_SNORM_BLOCK},
        {"BC5_UNORM", VK_FORMAT_BC5_UNORM_BLOCK},
        {"BC5_SNORM", VK_FORMAT_BC5_SNORM_BLOCK},
        {"BC6H_UFLOAT", VK_FORMAT_BC6H_UFLOAT_BLOCK},
        {"BC6H_SFLOAT", VK_FORMAT_BC6H_SFLOAT_BLOCK},
        {"BC7_UNORM", VK_FORMAT_BC7_UNORM_BLOCK},
        {"BC7_SRGB", VK_FORMAT_BC7_SRGB_BLOCK},
    };

    for (uint32_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        VkFormatProperties properties;
        c->vkGetPhysicalDeviceFormatProperties(c->phys, formats[i].format,
                                               &properties);
        lg_debug(c, "FORMAT_INFO name=%s format=%u optimal_features=0x%08x linear_features=0x%08x buffer_features=0x%08x sampled=%u linear_filter=%u storage=%u color_attachment=%u",
           formats[i].name,
           (unsigned)formats[i].format,
           properties.optimalTilingFeatures,
           properties.linearTilingFeatures,
           properties.bufferFeatures,
           !!(properties.optimalTilingFeatures &
              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
           !!(properties.optimalTilingFeatures &
              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
           !!(properties.optimalTilingFeatures &
              VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),
           !!(properties.optimalTilingFeatures &
              VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    }
    lg(c, "VULKAN_TEXTURE_SUMMARY queried_formats=%u bc_variants=14 detailed=%u",
       (unsigned)(sizeof formats / sizeof formats[0]),
       __atomic_load_n(&g_debug_logging, __ATOMIC_RELAXED));
    return 1;
}

static int
test_subpass_merge_feedback(struct Ctx *c)
{
    if (!c->subpass_merge_feedback_enabled)
        return 1;

    VkRenderPassCreationFeedbackInfoEXT pass_feedback = {0};
    VkRenderPassSubpassFeedbackInfoEXT subpass_feedback = {0};
    VkRenderPassSubpassFeedbackCreateInfoEXT subpass_feedback_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_SUBPASS_FEEDBACK_CREATE_INFO_EXT,
        .pSubpassFeedback = &subpass_feedback,
    };
    VkRenderPassCreationFeedbackCreateInfoEXT pass_feedback_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_FEEDBACK_CREATE_INFO_EXT,
        .pRenderPassFeedback = &pass_feedback,
    };
    VkSubpassDescription2 subpass = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
        .pNext = &subpass_feedback_info,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    };
    VkRenderPassCreateInfo2 create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .pNext = &pass_feedback_info,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };
    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (!chk0(c, "vkCreateRenderPass2KHR(subpass feedback)",
              c->vkCreateRenderPass2KHR(c->dev, &create_info, NULL, &render_pass),
              &render_pass))
        return 0;

    int valid =
        pass_feedback.postMergeSubpassCount == 1 &&
        subpass_feedback.subpassMergeStatus ==
            VK_SUBPASS_MERGE_STATUS_NOT_MERGED_SINGLE_SUBPASS_EXT &&
        subpass_feedback.postMergeIndex == 0 &&
        strcmp(subpass_feedback.description,
               "PanVK does not merge render pass subpasses") == 0;
    c->vkDestroyRenderPass(c->dev, render_pass, NULL);
    if (!valid)
        return fail(c, "subpass merge feedback values", VK_ERROR_UNKNOWN);

    lg(c, "PASS VK_EXT_subpass_merge_feedback status=%d postMergeIndex=%u",
       subpass_feedback.subpassMergeStatus, subpass_feedback.postMergeIndex);
    return 1;
}

static int
test_astc_shared_exponent(struct Ctx *c)
{
    if (!c->astc_decode_shared_exponent_enabled)
        return 1;

    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t memory_type_index = ~0u;
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
        .extent = {4, 4, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImageViewASTCDecodeModeEXT decode_mode = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT,
        .decodeMode = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
    };
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &decode_mode,
        .image = VK_NULL_HANDLE,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    if (!chk0(c, "vkCreateImage(ASTC shared exponent)",
              c->vkCreateImage(c->dev, &image_info, NULL, &image), &image))
        goto fail;

    c->vkGetImageMemoryRequirements(c->dev, image, &requirements);
    c->vkGetPhysicalDeviceMemoryProperties(c->phys, &memory_properties);
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memory_type_index = i;
            break;
        }
    }
    if (memory_type_index == ~0u) {
        fail(c, "ASTC image memory type", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        goto fail;
    }

    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type_index,
    };
    if (!chk0(c, "vkAllocateMemory(ASTC image)",
              c->vkAllocateMemory(c->dev, &allocate_info, NULL, &memory),
              &memory) ||
        !chk(c, "vkBindImageMemory(ASTC image)",
             c->vkBindImageMemory(c->dev, image, memory, 0)))
        goto fail;

    view_info.image = image;
    if (!chk0(c, "vkCreateImageView(ASTC shared exponent)",
              c->vkCreateImageView(c->dev, &view_info, NULL, &view), &view))
        goto fail;

    c->vkDestroyImageView(c->dev, view, NULL);
    c->vkDestroyImage(c->dev, image, NULL);
    c->vkFreeMemory(c->dev, memory, NULL);
    lg(c, "PASS VK_EXT_astc_decode_mode decodeMode=E5B9G9R9");
    return 1;

fail:
    if (view)
        c->vkDestroyImageView(c->dev, view, NULL);
    if (image)
        c->vkDestroyImage(c->dev, image, NULL);
    if (memory)
        c->vkFreeMemory(c->dev, memory, NULL);
    return 0;
}

static int
init_all(struct Ctx *c, const char *driver, const char *dependency_directory,
         const char *bundled_library_directory)
{
    float prio = 1.f;
    const char *exts[8] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t ext_count = 1;
    VkPhysicalDeviceFrameBoundaryFeaturesEXT frame_boundary_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT,
    };
    VkPhysicalDeviceImageCompressionControlFeaturesEXT compression_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT,
    };
    VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT swapchain_compression_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_FEATURES_EXT,
    };
    VkPhysicalDeviceFrameBoundaryFeaturesEXT frame_boundary_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT,
    };
    VkPhysicalDeviceImageCompressionControlFeaturesEXT compression_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT,
    };
    VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT swapchain_compression_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_FEATURES_EXT,
    };
    VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT subpass_merge_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_MERGE_FEEDBACK_FEATURES_EXT,
    };
    VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT subpass_merge_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_MERGE_FEEDBACK_FEATURES_EXT,
    };
    VkPhysicalDeviceASTCDecodeFeaturesEXT astc_decode_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT,
    };
    VkPhysicalDeviceASTCDecodeFeaturesEXT astc_decode_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT,
    };
    VkPhysicalDeviceFeatures2 enabled_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    VkAndroidSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                         .window = c->win};
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueCount = 1,
                                 .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &q,
                             .enabledExtensionCount = ext_count,
                             .ppEnabledExtensionNames = exts};
    set_phase("init");
    if (!open_icd(c, driver, dependency_directory, bundled_library_directory))
        return 0;
    if (!chk0(c, "vkCreateAndroidSurfaceKHR",
              c->vkCreateAndroidSurfaceKHR(c->inst, &sci, NULL, &c->surf), &c->surf))
        return 0;
    if (!pick_gpu(c))
        return 0;
    {
        VkPhysicalDeviceFeatures2 core_features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        };
        c->vkGetPhysicalDeviceFeatures2(c->phys, &core_features);
        c->tessellation_supported = core_features.features.tessellationShader;
        log_core_features(c, &core_features.features);
        lg(c, "VULKAN_FEATURES tessellation_shader=%u geometry_shader=%u texture_compression_bc=%u",
           core_features.features.tessellationShader,
           core_features.features.geometryShader,
           core_features.features.textureCompressionBC);
    }
    if (!log_texture_formats(c))
        return 0;

    int has_frame_boundary =
        device_has_extension(c, VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME);
    int has_compression =
        device_has_extension(c, VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME);
    int has_swapchain_compression =
        device_has_extension(c, VK_EXT_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_EXTENSION_NAME);
    int has_subpass_merge_feedback =
        device_has_extension(c, VK_EXT_SUBPASS_MERGE_FEEDBACK_EXTENSION_NAME);
    int has_astc_decode =
        device_has_extension(c, VK_EXT_ASTC_DECODE_MODE_EXTENSION_NAME);
    if (has_frame_boundary < 0 || has_compression < 0 ||
        has_swapchain_compression < 0 || has_subpass_merge_feedback < 0 ||
        has_astc_decode < 0)
        return 0;

    if (has_frame_boundary) {
        VkPhysicalDeviceFeatures2 features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &frame_boundary_query,
        };
        c->vkGetPhysicalDeviceFeatures2(c->phys, &features);
        c->frame_boundary_enabled = frame_boundary_query.frameBoundary;
        lg(c, "FEATURE_EXTENSION name=VK_EXT_frame_boundary extension_available=1 feature=frameBoundary supported=%u enabled=%u",
           c->frame_boundary_enabled, c->frame_boundary_enabled);
        if (c->frame_boundary_enabled) {
            c->feature_extension_supported_count++;
            exts[ext_count++] = VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME;
            frame_boundary_enable.frameBoundary = VK_TRUE;
            lg(c, "EXT VK_EXT_frame_boundary runtime test enabled");
        } else {
            lg(c, "SKIP VK_EXT_frame_boundary: feature bit is false");
        }
    } else {
        lg(c, "FEATURE_EXTENSION name=VK_EXT_frame_boundary extension_available=0 feature=frameBoundary supported=0 enabled=0");
        lg(c, "SKIP VK_EXT_frame_boundary: extension not exposed");
    }

    if (has_compression) {
        VkPhysicalDeviceFeatures2 features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &compression_query,
        };
        c->vkGetPhysicalDeviceFeatures2(c->phys, &features);
        if (compression_query.imageCompressionControl)
            c->feature_extension_supported_count++;
    }
    if (has_swapchain_compression) {
        VkPhysicalDeviceFeatures2 features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &swapchain_compression_query,
        };
        c->vkGetPhysicalDeviceFeatures2(c->phys, &features);
        if (swapchain_compression_query.imageCompressionControlSwapchain)
            c->feature_extension_supported_count++;
    }
    c->swapchain_compression_enabled =
        has_compression && has_swapchain_compression &&
        compression_query.imageCompressionControl &&
        swapchain_compression_query.imageCompressionControlSwapchain;
    lg(c, "FEATURE_EXTENSION name=VK_EXT_image_compression_control extension_available=%u feature=imageCompressionControl supported=%u enabled=%u",
       has_compression, compression_query.imageCompressionControl,
       c->swapchain_compression_enabled);
    lg(c, "FEATURE_EXTENSION name=VK_EXT_image_compression_control_swapchain extension_available=%u feature=imageCompressionControlSwapchain supported=%u enabled=%u",
       has_swapchain_compression,
       swapchain_compression_query.imageCompressionControlSwapchain,
       c->swapchain_compression_enabled);
    if (c->swapchain_compression_enabled) {
        exts[ext_count++] = VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME;
        exts[ext_count++] =
            VK_EXT_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_EXTENSION_NAME;
        compression_enable.imageCompressionControl = VK_TRUE;
        compression_enable.pNext = &swapchain_compression_enable;
        swapchain_compression_enable.imageCompressionControlSwapchain = VK_TRUE;
        lg(c, "EXT VK_EXT_image_compression_control_swapchain runtime test enabled");
    } else {
        lg(c, "SKIP swapchain compression: both extensions and feature bits are required");
    }

    if (has_subpass_merge_feedback) {
        VkPhysicalDeviceFeatures2 features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &subpass_merge_query,
        };
        c->vkGetPhysicalDeviceFeatures2(c->phys, &features);
        c->subpass_merge_feedback_enabled = subpass_merge_query.subpassMergeFeedback;
        if (c->subpass_merge_feedback_enabled)
            c->feature_extension_supported_count++;
        lg(c, "FEATURE_EXTENSION name=VK_EXT_subpass_merge_feedback extension_available=1 feature=subpassMergeFeedback supported=%u enabled_candidate=%u",
           c->subpass_merge_feedback_enabled,
           c->subpass_merge_feedback_enabled);
        if (c->subpass_merge_feedback_enabled) {
            int has_renderpass2 =
                device_has_extension(c, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
            if (has_renderpass2 < 0)
                return 0;
            if (!has_renderpass2) {
                c->subpass_merge_feedback_enabled = 0;
                lg(c, "SKIP subpass merge feedback: VK_KHR_create_renderpass2 unavailable");
            } else {
                exts[ext_count++] = VK_EXT_SUBPASS_MERGE_FEEDBACK_EXTENSION_NAME;
                exts[ext_count++] = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
                subpass_merge_enable.subpassMergeFeedback = VK_TRUE;
                lg(c, "EXT VK_EXT_subpass_merge_feedback runtime test enabled");
            }
        } else {
            lg(c, "SKIP VK_EXT_subpass_merge_feedback: feature bit is false");
        }
    } else {
        lg(c, "FEATURE_EXTENSION name=VK_EXT_subpass_merge_feedback extension_available=0 feature=subpassMergeFeedback supported=0 enabled_candidate=0");
        lg(c, "SKIP VK_EXT_subpass_merge_feedback: extension not exposed");
    }

    if (has_astc_decode) {
        VkPhysicalDeviceFeatures2 features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &astc_decode_query,
        };
        c->vkGetPhysicalDeviceFeatures2(c->phys, &features);
        c->astc_decode_shared_exponent_enabled =
            astc_decode_query.decodeModeSharedExponent;
        lg(c, "FEATURE_EXTENSION name=VK_EXT_astc_decode_mode extension_available=1 feature=decodeModeSharedExponent supported=%u enabled=%u",
           c->astc_decode_shared_exponent_enabled,
           c->astc_decode_shared_exponent_enabled);
        if (c->astc_decode_shared_exponent_enabled) {
            c->feature_extension_supported_count++;
            exts[ext_count++] = VK_EXT_ASTC_DECODE_MODE_EXTENSION_NAME;
            astc_decode_enable.decodeModeSharedExponent = VK_TRUE;
            lg(c, "EXT VK_EXT_astc_decode_mode shared-exponent runtime test enabled");
        } else {
            lg(c, "SKIP VK_EXT_astc_decode_mode: shared-exponent feature bit is false");
        }
    } else {
        lg(c, "FEATURE_EXTENSION name=VK_EXT_astc_decode_mode extension_available=0 feature=decodeModeSharedExponent supported=0 enabled=0");
        lg(c, "SKIP VK_EXT_astc_decode_mode: extension not exposed");
    }

    lg(c, "VULKAN_CAPABILITY_SUMMARY instance_extensions=%u instance_layers=%u device_extensions=%u core_features_supported=%u core_features_total=55 queried_feature_extension_count=5 queried_feature_extensions_supported=%u",
       c->instance_extension_count, c->instance_layer_count,
       c->device_extension_count, c->core_feature_supported_count,
       c->feature_extension_supported_count);

    if (c->frame_boundary_enabled) {
        frame_boundary_enable.pNext =
            c->swapchain_compression_enabled
               ? (void *)&compression_enable
               : (c->subpass_merge_feedback_enabled
                      ? (void *)&subpass_merge_enable
                      : NULL);
        enabled_features.pNext = &frame_boundary_enable;
    } else if (c->swapchain_compression_enabled) {
        enabled_features.pNext = &compression_enable;
        swapchain_compression_enable.pNext =
            c->subpass_merge_feedback_enabled ? &subpass_merge_enable : NULL;
    } else if (c->subpass_merge_feedback_enabled) {
        enabled_features.pNext = &subpass_merge_enable;
    }
    if (c->swapchain_compression_enabled && c->subpass_merge_feedback_enabled) {
        swapchain_compression_enable.pNext = &subpass_merge_enable;
    }
    if (c->frame_boundary_enabled && c->swapchain_compression_enabled &&
        c->subpass_merge_feedback_enabled) {
        swapchain_compression_enable.pNext = &subpass_merge_enable;
    }
    if (c->astc_decode_shared_exponent_enabled) {
        if (c->subpass_merge_feedback_enabled) {
            subpass_merge_enable.pNext = &astc_decode_enable;
        } else if (c->swapchain_compression_enabled) {
            swapchain_compression_enable.pNext = &astc_decode_enable;
        } else if (c->frame_boundary_enabled) {
            frame_boundary_enable.pNext = &astc_decode_enable;
        } else {
            enabled_features.pNext = &astc_decode_enable;
        }
    }
    if (c->test_mode == 5) {
        if (!c->tessellation_supported)
            return fail(c, "tessellationShader feature unavailable",
                        VK_ERROR_FEATURE_NOT_PRESENT);
        enabled_features.features.tessellationShader = VK_TRUE;
    }
    if (enabled_features.pNext || c->test_mode == 5)
        di.pNext = &enabled_features;
    di.enabledExtensionCount = ext_count;
    di.ppEnabledExtensionNames = exts;

    q.queueFamilyIndex = c->qf;
    if (!chk0(c, "vkCreateDevice", c->vkCreateDevice(c->phys, &di, NULL, &c->dev), &c->dev))
        return 0;
    if (!load_device(c))
        return 0;
    if (!test_subpass_merge_feedback(c))
        return 0;
    if (!test_astc_shared_exponent(c))
        return 0;
    c->vkGetDeviceQueue(c->dev, c->qf, 0, &c->queue);
    if (!pick_format(c) || !pick_depth_format(c) || !make_rp(c) ||
        !make_pipeline(c) ||
        !make_sync(c))
        return 0;
    if (c->test_mode == 5)
        lg(c, "TESSELLATION_PIPELINE patch_points=3 factor=%u shadow=projected_geometry",
           c->objects);
    return 1;
}

static int
pick_extent(struct Ctx *c, const VkSurfaceCapabilitiesKHR *caps, VkExtent2D *out)
{
    if (caps->currentExtent.width != 0xffffffffu) {
        *out = caps->currentExtent;
    } else {
        int32_t w = ANativeWindow_getWidth(c->win);
        int32_t h = ANativeWindow_getHeight(c->win);
        uint32_t uw, uh;
        if (w <= 0 || h <= 0)
            return fail(c, "ANativeWindow_getWidth", w < 0 ? w : h);
        uw = (uint32_t)w;
        uh = (uint32_t)h;
        if (uw < caps->minImageExtent.width)
            uw = caps->minImageExtent.width;
        if (uh < caps->minImageExtent.height)
            uh = caps->minImageExtent.height;
        if (uw > caps->maxImageExtent.width)
            uw = caps->maxImageExtent.width;
        if (uh > caps->maxImageExtent.height)
            uh = caps->maxImageExtent.height;
        out->width = uw;
        out->height = uh;
    }
    if (out->width == 0 || out->height == 0)
        return fail(c, "extent", VK_ERROR_INITIALIZATION_FAILED);
    return 1;
}

static int
create_depth_attachment(struct Ctx *c, struct Images *im, uint32_t index)
{
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = c->depth_format,
        .extent = {im->extent.width, im->extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = c->depth_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    };
    uint32_t i, memory_type = ~0u;

    if (!chk0(c, "vkCreateImage(depth)",
              c->vkCreateImage(c->dev, &image_info, NULL, &im->depth_img[index]),
              &im->depth_img[index]))
        return 0;
    c->vkGetImageMemoryRequirements(c->dev, im->depth_img[index], &requirements);
    c->vkGetPhysicalDeviceMemoryProperties(c->phys, &memory_properties);
    for (i = 0; i < memory_properties.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags;
        if (!(requirements.memoryTypeBits & (1u << i)))
            continue;
        flags = memory_properties.memoryTypes[i].propertyFlags;
        if (memory_type == ~0u)
            memory_type = i;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == ~0u)
        return fail(c, "depth memory type", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    if (!chk0(c, "vkAllocateMemory(depth)",
              c->vkAllocateMemory(c->dev, &allocate_info, NULL, &im->depth_mem[index]),
              &im->depth_mem[index]) ||
        !chk(c, "vkBindImageMemory(depth)",
             c->vkBindImageMemory(c->dev, im->depth_img[index], im->depth_mem[index], 0)))
        return 0;
    view_info.image = im->depth_img[index];
    return chk0(c, "vkCreateImageView(depth)",
                c->vkCreateImageView(c->dev, &view_info, NULL, &im->depth_view[index]),
                &im->depth_view[index]);
}

static int
sc_fill(struct Ctx *c, VkSwapchainKHR old, struct Images *out, int banner)
{
    VkSurfaceCapabilitiesKHR caps;
    VkExtent2D ext;
    VkPresentModeKHR stack[16], *modes = stack, *heap = NULL;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VkSurfaceTransformFlagBitsKHR pre;
    VkSwapchainCreateInfoKHR sci;
    VkImageCompressionControlEXT compression = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT,
        .flags = VK_IMAGE_COMPRESSION_DEFAULT_EXT,
    };
    struct Images im;
    uint32_t npm = 0, want, i;
    int fifo = 0, immediate = 0, mailbox = 0;
    static const VkCompositeAlphaFlagBitsKHR order[] = {
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
    memset(&im, 0, sizeof im);
    if (!chk(c, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
             c->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->phys, c->surf, &caps)))
        return 0;
    if (!pick_extent(c, &caps, &ext))
        return 0;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return fail(c, "imageUsage", VK_ERROR_FORMAT_NOT_SUPPORTED);
    if (!chk(c, "vkGetPhysicalDeviceSurfacePresentModesKHR",
             c->vkGetPhysicalDeviceSurfacePresentModesKHR(c->phys, c->surf, &npm, NULL)))
        return 0;
    if (npm == 0)
        return fail(c, "vkGetPhysicalDeviceSurfacePresentModesKHR",
                    VK_ERROR_FORMAT_NOT_SUPPORTED);
    if (npm > 16) {
        heap = calloc(npm, sizeof *heap);
        if (!heap)
            return fail(c, "calloc", -12);
        modes = heap;
    }
    if (!chk(c, "vkGetPhysicalDeviceSurfacePresentModesKHR",
             c->vkGetPhysicalDeviceSurfacePresentModesKHR(c->phys, c->surf, &npm, modes))) {
        free(heap);
        return 0;
    }
    for (i = 0; i < npm; i++) {
        if (modes[i] == VK_PRESENT_MODE_FIFO_KHR)
            fifo = 1;
        else if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            immediate = 1;
        else if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            mailbox = 1;
    }
    free(heap);
    if (!fifo)
        return fail(c, "FIFO", VK_ERROR_FORMAT_NOT_SUPPORTED);
    c->present_mode = immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR :
                      mailbox ? VK_PRESENT_MODE_MAILBOX_KHR :
                                VK_PRESENT_MODE_FIFO_KHR;
    for (i = 0; i < 4; i++)
        if (caps.supportedCompositeAlpha & order[i]) {
            alpha = order[i];
            break;
        }
    if (i == 4)
        return fail(c, "compositeAlpha", VK_ERROR_FORMAT_NOT_SUPPORTED);
    pre = caps.currentTransform;
    if (!pre) {
        if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
            pre = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        else
            return fail(c, "preTransform", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    want = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && want > caps.maxImageCount)
        want = caps.maxImageCount;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = c->surf;
    sci.minImageCount = want;
    sci.imageFormat = c->format;
    sci.imageColorSpace = c->color_space;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = pre;
    sci.compositeAlpha = alpha;
    sci.presentMode = c->present_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    if (c->swapchain_compression_enabled) {
        sci.pNext = &compression;
        lg(c, "swapchain compression request: DEFAULT");
    }
    if (!chk0(c, "vkCreateSwapchainKHR",
              c->vkCreateSwapchainKHR(c->dev, &sci, NULL, &im.swap), &im.swap))
        return 0;
    if (!chk(c, "vkGetSwapchainImagesKHR",
             c->vkGetSwapchainImagesKHR(c->dev, im.swap, &im.n, NULL)) ||
        im.n == 0 || im.n > MAX_IMG) {
        if (im.n == 0 || im.n > MAX_IMG)
            fail(c, "vkGetSwapchainImagesKHR", VK_ERROR_TOO_MANY_OBJECTS);
        images_destroy(c, &im);
        return 0;
    }
    if (!chk(c, "vkGetSwapchainImagesKHR",
             c->vkGetSwapchainImagesKHR(c->dev, im.swap, &im.n, im.img))) {
        images_destroy(c, &im);
        return 0;
    }
    im.extent = ext;
    for (i = 0; i < im.n; i++) {
        VkImageView attachments[2];
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = im.img[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = c->format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                      .renderPass = c->rp,
                                      .attachmentCount = 2,
                                      .pAttachments = attachments,
                                      .width = ext.width,
                                      .height = ext.height,
                                      .layers = 1};
        VkSemaphoreCreateInfo se = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!chk0(c, "vkCreateImageView",
                  c->vkCreateImageView(c->dev, &vi, NULL, &im.view[i]), &im.view[i]) ||
            !create_depth_attachment(c, &im, i)) {
            images_destroy(c, &im);
            return 0;
        }
        attachments[0] = im.view[i];
        attachments[1] = im.depth_view[i];
        if (!chk0(c, "vkCreateFramebuffer",
                  c->vkCreateFramebuffer(c->dev, &fi, NULL, &im.fb[i]), &im.fb[i]) ||
            !chk0(c, "vkCreateSemaphore",
                  c->vkCreateSemaphore(c->dev, &se, NULL, &im.rend[i]), &im.rend[i])) {
            images_destroy(c, &im);
            return 0;
        }
    }
    if (banner)
        lg(c, "SWAPCHAIN %ux%u images=%u format=%u present_mode=%s uncapped=%s",
           ext.width, ext.height, im.n, (unsigned)c->format,
           present_mode_name(c->present_mode),
           c->present_mode == VK_PRESENT_MODE_FIFO_KHR ? "no" : "yes");
    *out = im;
    return 1;
}

static int
wait_frame_fences(struct Ctx *c)
{
    int i;
    for (i = 0; i < FRAMES; i++) {
        VkResult r;
        if (!c->fence[i])
            continue;
        r = c->vkWaitForFences(c->dev, 1, &c->fence[i], VK_TRUE, WAIT_NS);
        if (r != VK_SUCCESS)
            return fail(c, "vkWaitForFences", r);
    }
    return 1;
}

static int
sc_recreate(struct Ctx *c)
{
    struct Images neu;
    VkExtent2D prev = c->sc.extent;
    memset(&neu, 0, sizeof neu);
    if (!wait_frame_fences(c))
        return 0;
    if (!sc_fill(c, c->sc.swap, &neu, 0))
        return 0;
    if (!c->timed_out && c->vkDeviceWaitIdle) {
        VkResult r = c->vkDeviceWaitIdle(c->dev);
        if (r != VK_SUCCESS) {
            images_destroy(c, &neu);
            return fail(c, "vkDeviceWaitIdle", r);
        }
    }
    images_destroy(c, &c->sc);
    c->sc = neu;
    memset(c->img_fence, 0, sizeof c->img_fence);
    if (neu.extent.width == prev.width && neu.extent.height == prev.height)
        lg(c, "RESIZE extent unchanged, recreated with oldSwapchain");
    return 1;
}

static int
record_frame(struct Ctx *c, int slot, uint32_t img, double render_time_s)
{
    VkCommandBuffer cmd = c->cmd[slot];
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkViewport vp = {0, 0, (float)c->sc.extent.width, (float)c->sc.extent.height, 0.f, 1.f};
    VkRect2D sc = {{0, 0}, c->sc.extent};
    float params[5] = {
        (float)render_time_s,
        (float)c->sc.extent.width / (float)c->sc.extent.height,
        (float)(c->test_mode == 6 ? 256 : c->objects),
        (float)c->scene_mode,
        __atomic_load_n(&g_scene_rotation_millidegrees, __ATOMIC_RELAXED) *
            (3.14159265358979323846f / 180000.0f),
    };
    VkClearValue clear[2] = {
        {.color = {{0.025f, 0.045f, 0.075f, 1.f}}},
        {.depthStencil = {1.f, 0}},
    };
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = c->rp,
                                .framebuffer = c->sc.fb[img],
                                .renderArea = sc,
                                .clearValueCount = 2,
                                .pClearValues = clear};
    if (!chk(c, "vkResetCommandBuffer", c->vkResetCommandBuffer(cmd, 0)))
        return 0;
    if (!chk(c, "vkBeginCommandBuffer", c->vkBeginCommandBuffer(cmd, &bi)))
        return 0;
    c->vkCmdSetViewport(cmd, 0, 1, &vp);
    c->vkCmdSetScissor(cmd, 0, 1, &sc);
    c->vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    c->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
    VkShaderStageFlags push_stages = VK_SHADER_STAGE_VERTEX_BIT;
    if (c->test_mode == 5)
        push_stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    c->vkCmdPushConstants(cmd, c->layout, push_stages, 0,
                          sizeof params, params);
    if (c->test_mode == 5) {
        params[3] = 4.f;
        c->vkCmdPushConstants(cmd, c->layout, push_stages, 0,
                              sizeof params, params);
        c->vkCmdDraw(cmd, 3, 1, 0, 0);
        params[3] = 3.f;
        c->vkCmdPushConstants(cmd, c->layout, push_stages, 0,
                              sizeof params, params);
        c->vkCmdDraw(cmd, 3, 1, 0, 0);
    } else if (c->test_mode == 1)
        c->vkCmdDraw(cmd, 6, c->objects, 0, 0);
    else if (c->test_mode == 2)
        c->vkCmdDraw(cmd, 3, c->objects, 0, 0);
    else if (c->test_mode == 6)
        c->vkCmdDraw(cmd, 6, 256, 0, 0);
    else if (c->test_mode == 7 || c->test_mode == 8)
        c->vkCmdDraw(cmd, 3, c->objects, 0, 0);
    else if (c->test_mode == 9)
        c->vkCmdDraw(cmd, 6, c->objects, 0, 0);
    else if (c->test_mode == 10)
        c->vkCmdDraw(cmd, 3, c->objects, 0, 0);
    else if (c->test_mode == 11)
        c->vkCmdDraw(cmd, 3, c->objects, 0, 0);
    else if (c->test_mode == 12 || c->test_mode == 13)
        c->vkCmdDraw(cmd, 6, c->objects, 0, 0);
    else if (c->test_mode == 14)
        c->vkCmdDraw(cmd, 3, c->objects, 0, 0);
    else if (c->test_mode == 15)
        c->vkCmdDraw(cmd, c->objects * 3u, 1, 0, 0);
    else if (c->scene_mode == 1)
        c->vkCmdDraw(cmd, 501, c->objects, 0, 0);
    else if (c->scene_mode == 2)
        c->vkCmdDraw(cmd, 3, 1, 0, 0);
    else
        c->vkCmdDraw(cmd, 36, c->objects, 0, 0);
    c->vkCmdEndRenderPass(cmd);
    return chk(c, "vkEndCommandBuffer", c->vkEndCommandBuffer(cmd));
}

static int
draw_frame(struct Ctx *c, int index, double *render_time_s,
           struct timespec *present_return_time)
{
    int slot = c->slot;
    uint32_t img = 0;
    VkResult r;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkImage image;
    uint64_t frame_tag = (uint64_t)index;
    VkFrameBoundaryEXT frame_boundary = {
        .sType = VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT,
        .flags = VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT,
        .frameID = (uint64_t)index,
        .imageCount = 1,
        .pImages = &image,
        .tagName = UINT64_C(0x50414e50524f4245),
        .tagSize = sizeof(frame_tag),
        .pTag = &frame_tag,
    };
    r = c->vkWaitForFences(c->dev, 1, &c->fence[slot], VK_TRUE, WAIT_NS);
    if (r != VK_SUCCESS)
        return fail(c, "vkWaitForFences", r);
    r = c->vkAcquireNextImageKHR(c->dev, c->sc.swap, WAIT_NS, c->acq[slot],
                                 VK_NULL_HANDLE, &img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
        return sc_recreate(c) ? 2 : 0;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return fail(c, "vkAcquireNextImageKHR", r);
    if (img >= c->sc.n)
        return fail(c, "vkAcquireNextImageKHR", VK_ERROR_DEVICE_LOST);
    if (c->img_fence[img]) {
        r = c->vkWaitForFences(c->dev, 1, &c->img_fence[img], VK_TRUE, WAIT_NS);
        if (r != VK_SUCCESS)
            return fail(c, "vkWaitForFences", r);
    }
    if (!chk(c, "vkResetFences", c->vkResetFences(c->dev, 1, &c->fence[slot])))
        return 0;
    c->img_fence[img] = c->fence[slot];
    *render_time_s = ms_exact(&c->animation_start) / 1000.0;
    if (!record_frame(c, slot, img, *render_time_s))
        return 0;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &c->acq[slot];
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c->cmd[slot];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &c->sc.rend[img];
    image = c->sc.img[img];
    if (c->frame_boundary_enabled) {
        si.pNext = &frame_boundary;
        if (index == 0)
            lg(c, "frame boundary submitted: frameID=%llu tagSize=%zu",
               (unsigned long long)frame_boundary.frameID, frame_boundary.tagSize);
    }
    if (!chk(c, "vkQueueSubmit", c->vkQueueSubmit(c->queue, 1, &si, c->fence[slot])))
        return 0;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &c->sc.rend[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &c->sc.swap;
    pi.pImageIndices = &img;
    r = c->vkQueuePresentKHR(c->queue, &pi);
    c->slot = (slot + 1) % FRAMES;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!sc_recreate(c))
            return 0;
        return 2;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return fail(c, "vkQueuePresentKHR", r);
    clock_gettime(CLOCK_MONOTONIC, present_return_time);
    return 1;
}

static int
idle_destroy_swap(struct Ctx *c)
{
    if (c->dev && c->vkDeviceWaitIdle && !c->timed_out) {
        VkResult r = c->vkDeviceWaitIdle(c->dev);
        if (r != VK_SUCCESS) {
            images_destroy(c, &c->sc);
            memset(c->img_fence, 0, sizeof c->img_fence);
            return fail(c, "vkDeviceWaitIdle", r);
        }
    }
    images_destroy(c, &c->sc);
    memset(c->img_fence, 0, sizeof c->img_fence);
    return 1;
}

static int
compare_double(const void *a, const void *b)
{
    double left = *(const double *)a;
    double right = *(const double *)b;
    return (left > right) - (left < right);
}

static int
log_phase_summary(struct Ctx *c, const char *name, size_t first, size_t last)
{
    size_t count = last - first, i, p95_index;
    double total_ms = 0.0, *sorted, p95;
    if (!count)
        return 1;
    sorted = malloc(count * sizeof *sorted);
    if (!sorted)
        return fail(c, "phase summary allocation", -12);
    memcpy(sorted, c->frame_ms + first, count * sizeof *sorted);
    for (i = 0; i < count; i++)
        total_ms += sorted[i];
    qsort(sorted, count, sizeof *sorted, compare_double);
    p95_index = (size_t)(count * 0.95);
    if (p95_index >= count)
        p95_index = count - 1;
    p95 = sorted[p95_index];
    lg(c, "PHASE_SUMMARY name=%s present_intervals=%zu avg_fps=%.2f p95_present_interval_ms=%.3f",
       name, count, count * 1000.0 / total_ms, p95);
    free(sorted);
    return 1;
}

static uint32_t
rgb565(uint16_t color)
{
    uint32_t r = (color >> 11) & 31u;
    uint32_t g = (color >> 5) & 63u;
    uint32_t b = color & 31u;
    return ((r * 255u / 31u) << 16) | ((g * 255u / 63u) << 8) |
           (b * 255u / 31u);
}

static double
run_bc1_emulation(uint32_t *checksum_out)
{
    struct timespec start;
    uint64_t checksum = 0;
    uint32_t block;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (block = 0; block < 4096; block++) {
        uint16_t c0 = (uint16_t)(block & 1u
                                     ? 0x07e0u + block % 512u
                                     : 0xf800u - block % 2048u);
        uint16_t c1 = (uint16_t)(block & 1u
                                     ? 0xf800u - block % 1024u
                                     : 0x07e0u + block % 1024u);
        uint32_t colors[4];
        uint32_t selectors = block * 0x9e3779b9u;
        uint32_t pixel;
        colors[0] = rgb565(c0);
        colors[1] = rgb565(c1);
        if (c0 > c1) {
            colors[2] = (((colors[0] >> 16 & 255u) * 2u +
                          (colors[1] >> 16 & 255u)) / 3u << 16) |
                        (((colors[0] >> 8 & 255u) * 2u +
                          (colors[1] >> 8 & 255u)) / 3u << 8) |
                        (((colors[0] & 255u) * 2u + (colors[1] & 255u)) / 3u);
            colors[3] = (((colors[0] >> 16 & 255u) +
                          (colors[1] >> 16 & 255u) * 2u) / 3u << 16) |
                        (((colors[0] >> 8 & 255u) +
                          (colors[1] >> 8 & 255u) * 2u) / 3u << 8) |
                        (((colors[0] & 255u) + (colors[1] & 255u) * 2u) / 3u);
        } else {
            colors[2] = (((colors[0] >> 16 & 255u) +
                          (colors[1] >> 16 & 255u)) / 2u << 16) |
                        (((colors[0] >> 8 & 255u) +
                          (colors[1] >> 8 & 255u)) / 2u << 8) |
                        (((colors[0] & 255u) + (colors[1] & 255u)) / 2u);
            colors[3] = 0;
        }
        for (pixel = 0; pixel < 16; pixel++) {
            uint32_t selector = (selectors >> ((pixel & 15u) * 2u)) & 3u;
            checksum += colors[selector] ^ (pixel * 0x45d9f3bu);
        }
    }
    *checksum_out = (uint32_t)(checksum ^ (checksum >> 32));
    __atomic_store_n(&g_bc1_checksum, *checksum_out, __ATOMIC_RELAXED);
    {
        double elapsed_ms = ms_exact(&start);
        if (elapsed_ms <= 0.0)
            return 0.0;
        return (4096.0 * 16.0 / 1000000.0) * 1000.0 / elapsed_ms;
    }
}

static int
run(struct Ctx *c, const char *driver, const char *dependency_directory,
    const char *bundled_library_directory)
{
    struct timespec start, tick, last_present_at = {0};
    uint64_t frames = 0, tick_frames = 0;
    uint32_t target_objects = c->objects;
    const char *current_phase = NULL;
    const char *workload_name;
    uint32_t scene_objects;
    size_t phase_first = 0;
    double elapsed = 0.0;
    double render_time_s = 0.0, present_interval_ms = 0.0;
    double bc1_mpix_s = 0.0;
    uint32_t bc1_checksum = 0;
    int have_previous_present = 0;
    struct BcEmulationResult bc27_result;
    memset(&bc27_result, 0, sizeof bc27_result);
    if (!init_all(c, driver, dependency_directory, bundled_library_directory))
        return 0;
    set_phase("create_swapchain");
    if (!sc_fill(c, VK_NULL_HANDLE, &c->sc, 1))
        return 0;
    workload_name = c->test_mode == 6 ? "bc_blocks_per_format" :
                    c->test_mode == 5 ? "tessellation_factor" :
                    c->test_mode == 1 ? "bc1_decode_blocks" :
                    c->test_mode == 7 ? "depth_occlusion_layers" :
                    c->test_mode == 8 ? "alpha_blend_overdraw" :
                    c->test_mode == 9 ? "fragment_shader_algebra" :
                    c->test_mode == 10 ? "triangle_raster_throughput" :
                    c->test_mode == 11 ? "vertex_transform_stress" :
                    c->test_mode == 12 ? "fragment_discard_overdraw" :
                    c->test_mode == 13 ? "blend_color_overdraw" :
                    c->test_mode == 14 ? "geometry_triangle_processing" :
                    c->test_mode == 15 ? "large_single_draw" :
                                         "instances";
    scene_objects = c->test_mode == 6 ? 256 :
                    c->test_mode == 5 ? 1 : c->objects;
    lg(c, "CONFIG test_mode=%u duration_s=%u scene_objects=%u workload=%s workload_units=%u driver=%s",
       c->test_mode, c->duration_seconds, scene_objects,
       workload_name, c->objects, driver);
    if (c->test_mode == 7)
        lg(c, "TEST_PROFILE name=depth_occlusion pipeline=depth_test_write visual=rotating_z_layers");
    else if (c->test_mode == 8)
        lg(c, "TEST_PROFILE name=alpha_blend pipeline=src_alpha_over depth_write=off visual=overlapping_translucent_triangles");
    else if (c->test_mode == 9)
        lg(c, "TEST_PROFILE name=fragment_shader_algebra pipeline=depth_test=off shader_ops=20_iterative_trigonometric visual=animated_fullscreen_field");
    else if (c->test_mode == 10)
        lg(c, "TEST_PROFILE name=triangle_raster_throughput pipeline=triangle_list instances=%u visual=rotating_microtriangles",
           c->objects);
    else if (c->test_mode == 11)
        lg(c, "TEST_PROFILE name=vertex_transform_stress pipeline=triangle_list instances=%u shader=animated_multiaxis_transform",
           c->objects);
    else if (c->test_mode == 12)
        lg(c, "TEST_PROFILE name=fragment_discard_overdraw pipeline=depth_test=off instances=%u shader=animated_coverage_discard",
           c->objects);
    else if (c->test_mode == 13)
        lg(c, "TEST_PROFILE name=blend_color_overdraw pipeline=src_alpha_over depth_write=off instances=%u visual=layered_color_fields",
           c->objects);
    else if (c->test_mode == 14)
        lg(c, "TEST_PROFILE name=geometry_triangle_processing pipeline=triangle_list draw_calls=1 triangles=%u vertices=%u visual=animated_geometric_field",
           c->objects, c->objects * 3u);
    else if (c->test_mode == 15)
        lg(c, "TEST_PROFILE name=large_single_draw pipeline=triangle_list draw_calls=1 triangles=%u vertices=%u visual=dense_single_draw_field",
           c->objects, c->objects * 3u);
    lg(c, "PRESENT_TIMING source=vkQueuePresentKHR_host_return physical_scanout_timing=unavailable");
    set_phase("warmup");
    clock_gettime(CLOCK_MONOTONIC, &start);
    c->animation_start = start;
    tick = start;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) &&
           elapsed < c->duration_seconds) {
        struct timespec present_at;
        const char *next_phase;
        uint32_t load;
        int frame_result;
        double tick_ms;

        elapsed = ms_exact(&start) / 1000.0;
        if (elapsed < c->duration_seconds * 0.10) {
            next_phase = "warmup";
            load = target_objects / 4;
        } else if (elapsed < c->duration_seconds * 0.35) {
            next_phase = "scene_ramp_1";
            load = target_objects / 2;
        } else if (elapsed < c->duration_seconds * 0.60) {
            next_phase = "scene_ramp_2";
            load = (target_objects * 3) / 4;
        } else {
            next_phase = "sustained";
            load = target_objects;
        }
        if (c->test_mode == 2 && load < 1024)
            load = 1024;
        if (c->test_mode >= 3)
            load = target_objects;
        if (load < 16 && c->test_mode != 5)
            load = 16;
        c->objects = load;
        if (!current_phase || strcmp(current_phase, next_phase) != 0) {
            if (current_phase &&
                !log_phase_summary(c, current_phase, phase_first, c->frame_count))
                return 0;
            phase_first = c->frame_count;
            current_phase = next_phase;
            set_phase(current_phase);
            lg(c, "PHASE name=%s elapsed_s=%.2f objects=%u",
               current_phase, elapsed, c->objects);
        }

        if (c->test_mode == 1)
            bc1_mpix_s = run_bc1_emulation(&bc1_checksum);
        else if (c->test_mode == 6 &&
                 !bench_bc_emulation(c->objects, &bc27_result))
            return fail(c, "BC2-BC7 software decoder workload", VK_ERROR_UNKNOWN);
        frame_result = draw_frame(c, (int)frames, &render_time_s, &present_at);
        if (!frame_result)
            return 0;
        if (frame_result == 2)
            continue;
        if (have_previous_present) {
            present_interval_ms = ms_between(&last_present_at, &present_at);
            if (present_interval_ms <= 0.0)
                return fail(c, "non-positive present-call interval",
                            VK_ERROR_INITIALIZATION_FAILED);
            if (c->frame_count == c->frame_capacity) {
                size_t new_capacity = c->frame_capacity
                                          ? c->frame_capacity * 2 : 1024;
                double *new_samples =
                    realloc(c->frame_ms, new_capacity * sizeof *new_samples);
                if (!new_samples)
                    return fail(c, "frame sample allocation", -12);
                c->frame_ms = new_samples;
                c->frame_capacity = new_capacity;
            }
            c->frame_ms[c->frame_count++] = present_interval_ms;
        }
        last_present_at = present_at;
        have_previous_present = 1;
        frames++;
        tick_frames++;
        tick_ms = ms_exact(&tick);
        if (tick_ms >= 1000.0) {
            double fps = tick_frames * 1000.0 / tick_ms;
            elapsed = ms_exact(&start) / 1000.0;
            if (c->test_mode == 1)
                lg(c, "LIVE test=bc1_cpu_emulation phase=%s elapsed_s=%.2f render_time_s=%.3f present_call_interval_ms=%.3f fps=%.2f objects=%u frames=%llu bc1_mpix_s=%.2f checksum=%08x",
                   current_phase, elapsed, render_time_s,
                   present_interval_ms, fps, c->objects,
                   (unsigned long long)frames, bc1_mpix_s, bc1_checksum);
            else if (c->test_mode == 6)
                lg(c, "LIVE test=bc2_bc7_cpu_emulation phase=%s elapsed_s=%.2f render_time_s=%.3f present_call_interval_ms=%.3f fps=%.2f objects=%u blocks_per_format=%u frames=%llu BC2=%.2f BC3=%.2f BC4=%.2f BC4_SNORM=%.2f BC5=%.2f BC5_SNORM=%.2f BC6H_UFLOAT=%.2f BC6H_SFLOAT=%.2f BC7_UNORM=%.2f BC7_SRGB=%.2f",
                   current_phase, elapsed, render_time_s,
                   present_interval_ms, fps, c->objects,
                   c->objects,
                   (unsigned long long)frames,
                   bc27_result.mpix_per_second[0],
                   bc27_result.mpix_per_second[1],
                   bc27_result.mpix_per_second[2],
                   bc27_result.mpix_per_second[3],
                   bc27_result.mpix_per_second[4],
                   bc27_result.mpix_per_second[5],
                   bc27_result.mpix_per_second[6],
                   bc27_result.mpix_per_second[7],
                   bc27_result.mpix_per_second[8],
                   bc27_result.mpix_per_second[9]);
            else
                lg(c, "LIVE test=%u phase=%s elapsed_s=%.2f render_time_s=%.3f present_call_interval_ms=%.3f fps=%.2f objects=%u frames=%llu",
                   c->test_mode, current_phase, elapsed, render_time_s,
                   present_interval_ms, fps, c->objects,
                   (unsigned long long)frames);
            tick_frames = 0;
            clock_gettime(CLOCK_MONOTONIC, &tick);
        }
    }

    if (current_phase &&
        !log_phase_summary(c, current_phase, phase_first, c->frame_count))
        return 0;
    set_phase("teardown");
    if (!idle_destroy_swap(c))
        return 0;
    if (!shutdown(c))
        return 0;
    if (c->frame_count > 0) {
        double sum = 0.0, p95, one_percent_low;
        size_t i, slow_count, p95_index;
        for (i = 0; i < c->frame_count; i++)
            sum += c->frame_ms[i];
        qsort(c->frame_ms, c->frame_count, sizeof *c->frame_ms, compare_double);
        p95_index = (size_t)(c->frame_count * 0.95);
        if (p95_index >= c->frame_count)
            p95_index = c->frame_count - 1;
        p95 = c->frame_ms[p95_index];
        slow_count = (c->frame_count + 99) / 100;
        if (slow_count > c->frame_count)
            slow_count = c->frame_count;
        {
            double slow_sum = 0.0;
            for (i = c->frame_count - slow_count; i < c->frame_count; i++)
                slow_sum += c->frame_ms[i];
            one_percent_low = 1000.0 / (slow_sum / slow_count);
        }
        lg(c, "SUMMARY frames=%llu present_intervals=%zu avg_fps=%.2f p95_present_interval_ms=%.3f one_percent_low_fps=%.2f cancelled=%s",
           (unsigned long long)frames, c->frame_count,
           c->frame_count * 1000.0 / sum, p95,
           one_percent_low,
           __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) ? "true" : "false");
    } else {
        lg(c, "SUMMARY frames=0");
    }
    lg(c, "%s",
       __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) ? "STOPPED user_cancelled" :
                                                   "PASS benchmark");
    if (c->test_mode == 1)
        lg(c, "BC1_CPU_EMULATION pixels_per_frame=%u checksum=%08x scope=CPU_reference_decoder",
           4096u * 16u, __atomic_load_n(&g_bc1_checksum, __ATOMIC_RELAXED));
    if (c->test_mode == 6) {
        static const char *const names[] = {
            "BC2", "BC3", "BC4_UNORM", "BC4_SNORM", "BC5_UNORM",
            "BC5_SNORM", "BC6H_UFLOAT", "BC6H_SFLOAT", "BC7_UNORM",
            "BC7_SRGB"
        };
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
            lg(c, "BC_CPU_EMULATION name=%s blocks=%u pixels_per_block=16 mpix_s=%.3f checksum=%08x",
               names[i], c->objects, bc27_result.mpix_per_second[i],
               bc27_result.checksum[i]);
    }
    return 1;
}

static uint32_t
scene_mode_for_test(uint32_t test_mode)
{
    if (test_mode == 1)
        return 6;
    if (test_mode == 2)
        return 7;
    if (test_mode == 3)
        return 1;
    if (test_mode == 4)
        return 2;
    if (test_mode == 5)
        return 3;
    if (test_mode == 6)
        return 8;
    if (test_mode == 7)
        return 9;
    if (test_mode == 8)
        return 10;
    if (test_mode == 9)
        return 11;
    if (test_mode == 10)
        return 12;
    if (test_mode == 11)
        return 13;
    if (test_mode == 12)
        return 14;
    if (test_mode == 13)
        return 15;
    if (test_mode == 14)
        return 16;
    if (test_mode == 15)
        return 17;
    return 0;
}

static void *
preview_thread_main(void *opaque)
{
    struct PreviewTask *task = opaque;
    struct Ctx c;
    int index = 0;
    struct timespec preview_start;
    memset(&c, 0, sizeof c);
    c.win = task->window;
    task->window = NULL;
    c.test_mode = task->test_mode;
    c.scene_mode = scene_mode_for_test(task->test_mode);
    c.objects = task->objects;
    c.duration_seconds = 180;
    c.log = fopen(task->log_path, "w");
    if (c.log)
        setvbuf(c.log, NULL, _IOLBF, 0);
    if (!c.log)
        lg(&c, "PREVIEW_LOG_OPEN_FAILED errno=%d", errno);
    set_phase("preview_init");
    if (init_all(&c, task->driver, task->dependencies, task->bundled_libraries) &&
        sc_fill(&c, VK_NULL_HANDLE, &c.sc, 1)) {
        lg(&c, "PREVIEW_READY test_mode=%u objects=%u scene_mode=%u present_mode=%s uncapped=%s",
           c.test_mode, c.objects, c.scene_mode,
           present_mode_name(c.present_mode),
           c.present_mode == VK_PRESENT_MODE_FIFO_KHR ? "no" : "yes");
        lg(&c, "PRESENT_TIMING source=vkQueuePresentKHR_host_return physical_scanout_timing=unavailable");
        set_phase("preview");
        clock_gettime(CLOCK_MONOTONIC, &preview_start);
        c.animation_start = preview_start;
        while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
            double render_time_s;
            struct timespec present_return_time;
            int frame_result = draw_frame(&c, index, &render_time_s,
                                          &present_return_time);
            if (!frame_result) {
                lg(&c, "PREVIEW_RENDER_FAILED frames=%d debug=%u",
                   index, __atomic_load_n(&g_debug_logging, __ATOMIC_RELAXED));
                break;
            }
            if (frame_result == 1)
                index++;
            if (index % 120 == 0)
                lg_debug(&c, "preview heartbeat frames=%d scene_mode=%u render_time_s=%.3f",
                         index, c.scene_mode, render_time_s);
        }
        lg(&c, "PREVIEW_STOPPED frames=%d reason=%s",
           index, __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)
                      ? "requested" : "render_failure");
    } else {
        lg(&c, "PREVIEW_FAILED initialization");
    }
    shutdown(&c);
    if (c.log)
        fclose(c.log);
    free(task->driver);
    free(task->dependencies);
    free(task->bundled_libraries);
    free(task->log_path);
    if (task->window)
        ANativeWindow_release(task->window);
    free(task);
    return NULL;
}

static void
stop_preview(void)
{
    pthread_t thread;
    int should_join = 0;
    pthread_mutex_lock(&g_preview_mutex);
    if (g_preview_active) {
        __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
        thread = g_preview_thread;
        g_preview_active = 0;
        should_join = 1;
    }
    if (should_join)
        pthread_join(thread, NULL);
    pthread_mutex_unlock(&g_preview_mutex);
}

JNIEXPORT jstring JNICALL
Java_dev_panvk_bench_Native_runBenchmark(JNIEnv *env, jobject self,
                                                    jstring j_driver, jstring j_dependencies,
                                                    jstring j_bundled_libraries,
                                                    jobject surface,
                                                    jint test_mode, jint duration_seconds,
                                                    jint objects,
                                                    jstring j_log)
{
    struct Ctx c;
    const char *driver = NULL, *dependencies = NULL, *bundled_libraries = NULL;
    const char *log_path = NULL;
    int passed = 0;
    (void)self;
    stop_preview();
    memset(&c, 0, sizeof c);
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
    set_phase("init");
    if (j_driver)
        driver = (*env)->GetStringUTFChars(env, j_driver, NULL);
    if (j_dependencies)
        dependencies = (*env)->GetStringUTFChars(env, j_dependencies, NULL);
    if (j_bundled_libraries)
        bundled_libraries = (*env)->GetStringUTFChars(env, j_bundled_libraries, NULL);
    if (j_log)
        log_path = (*env)->GetStringUTFChars(env, j_log, NULL);
    if (!driver || !dependencies || !bundled_libraries || !log_path || !surface) {
        fail(&c, "jni", -1);
        goto out;
    }
    c.log = fopen(log_path, "w");
    if (!c.log) {
        fail(&c, "fopen", errno ? (VkResult)errno : -1);
        goto out;
    }
    setvbuf(c.log, NULL, _IOLBF, 0);
    if (duration_seconds < 20 || duration_seconds > 180) {
        lg(&c, "FAIL invalid configuration duration_s=%d objects=%d",
           duration_seconds, objects);
        goto out;
    }
    c.duration_seconds = (uint32_t)duration_seconds;
    c.objects = (uint32_t)objects;
    if (test_mode < 0 || test_mode > 15 ||
        (test_mode <= 1 && (objects < 64 || objects > 1024)) ||
        (test_mode == 2 && (objects < 1024 || objects > 2048)) ||
        (test_mode == 3 && (objects < 1 || objects > 12)) ||
        (test_mode == 4 && objects != 1) ||
        (test_mode == 5 && (objects < 2 || objects > 32)) ||
        (test_mode == 6 && (objects < 64 || objects > 4096)) ||
        (test_mode == 7 && (objects < 32 || objects > 512)) ||
        (test_mode == 8 && (objects < 16 || objects > 128)) ||
        (test_mode == 9 && (objects < 16 || objects > 128)) ||
        (test_mode == 10 && (objects < 1024 || objects > 4096)) ||
        (test_mode == 11 && (objects < 256 || objects > 2048)) ||
        (test_mode == 12 && (objects < 8 || objects > 64)) ||
        (test_mode == 13 && (objects < 16 || objects > 128)) ||
        (test_mode == 14 && (objects < 128 || objects > 2048)) ||
        (test_mode == 15 && (objects < 4096 || objects > 65536))) {
        lg(&c, "FAIL invalid test configuration test_mode=%d objects=%d",
           test_mode, objects);
        goto out;
    }
    c.test_mode = (uint32_t)test_mode;
    c.scene_mode = scene_mode_for_test(c.test_mode);
    c.win = ANativeWindow_fromSurface(env, surface);
    if (!c.win) {
        fail(&c, "ANativeWindow_fromSurface", -1);
        goto out;
    }
    passed = run(&c, driver, dependencies, bundled_libraries);
out:
    if (!passed)
        shutdown(&c);
    free(c.frame_ms);
    if (c.log) {
        fclose(c.log);
        c.log = NULL;
    }
    if (driver)
        (*env)->ReleaseStringUTFChars(env, j_driver, driver);
    if (dependencies)
        (*env)->ReleaseStringUTFChars(env, j_dependencies, dependencies);
    if (bundled_libraries)
        (*env)->ReleaseStringUTFChars(env, j_bundled_libraries, bundled_libraries);
    if (log_path)
        (*env)->ReleaseStringUTFChars(env, j_log, log_path);
    if (passed && !c.failed)
        return (*env)->NewStringUTF(
            env, __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) ? "exit:stopped" : "exit:0");
    return (*env)->NewStringUTF(env, "exit:1");
}

JNIEXPORT jboolean JNICALL
Java_dev_panvk_bench_Native_startPreview(JNIEnv *env, jobject self,
                                         jstring j_driver, jstring j_dependencies,
                                         jstring j_bundled_libraries,
                                         jobject surface, jint test_mode, jint objects,
                                         jstring j_log)
{
    struct PreviewTask *task;
    const char *driver = NULL, *dependencies = NULL, *bundled = NULL, *log_path = NULL;
    (void)self;
    stop_preview();
    if (test_mode < 0 || test_mode > 15 ||
        (test_mode <= 1 && (objects < 64 || objects > 1024)) ||
        (test_mode == 2 && (objects < 1024 || objects > 2048)) ||
        (test_mode == 3 && (objects < 1 || objects > 12)) ||
        (test_mode == 4 && objects != 1) ||
        (test_mode == 5 && (objects < 2 || objects > 32)) ||
        (test_mode == 6 && (objects < 64 || objects > 4096)) ||
        (test_mode == 7 && (objects < 32 || objects > 512)) ||
        (test_mode == 8 && (objects < 16 || objects > 128)) ||
        (test_mode == 9 && (objects < 16 || objects > 128)) ||
        (test_mode == 10 && (objects < 1024 || objects > 4096)) ||
        (test_mode == 11 && (objects < 256 || objects > 2048)) ||
        (test_mode == 12 && (objects < 8 || objects > 64)) ||
        (test_mode == 13 && (objects < 16 || objects > 128)) ||
        (test_mode == 14 && (objects < 128 || objects > 2048)) ||
        (test_mode == 15 && (objects < 4096 || objects > 65536)))
        return JNI_FALSE;
    if (j_driver)
        driver = (*env)->GetStringUTFChars(env, j_driver, NULL);
    if (j_dependencies)
        dependencies = (*env)->GetStringUTFChars(env, j_dependencies, NULL);
    if (j_bundled_libraries)
        bundled = (*env)->GetStringUTFChars(env, j_bundled_libraries, NULL);
    if (j_log)
        log_path = (*env)->GetStringUTFChars(env, j_log, NULL);
    if (!driver || !dependencies || !bundled || !log_path || !surface)
        goto fail;
    task = calloc(1, sizeof *task);
    if (!task)
        goto fail;
    task->driver = strdup(driver);
    task->dependencies = strdup(dependencies);
    task->bundled_libraries = strdup(bundled);
    task->log_path = strdup(log_path);
    task->window = ANativeWindow_fromSurface(env, surface);
    task->test_mode = (uint32_t)test_mode;
    task->objects = (uint32_t)objects;
    if (!task->driver || !task->dependencies || !task->bundled_libraries ||
        !task->log_path || !task->window) {
        if (task->window)
            ANativeWindow_release(task->window);
        free(task->driver);
        free(task->dependencies);
        free(task->bundled_libraries);
        free(task->log_path);
        free(task);
        goto fail;
    }
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&g_preview_mutex);
    if (pthread_create(&g_preview_thread, NULL, preview_thread_main, task) != 0) {
        pthread_mutex_unlock(&g_preview_mutex);
        __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
        ANativeWindow_release(task->window);
        free(task->driver);
        free(task->dependencies);
        free(task->bundled_libraries);
        free(task->log_path);
        free(task);
        goto fail;
    }
    g_preview_active = 1;
    pthread_mutex_unlock(&g_preview_mutex);
    if (driver)
        (*env)->ReleaseStringUTFChars(env, j_driver, driver);
    if (dependencies)
        (*env)->ReleaseStringUTFChars(env, j_dependencies, dependencies);
    if (bundled)
        (*env)->ReleaseStringUTFChars(env, j_bundled_libraries, bundled);
    if (log_path)
        (*env)->ReleaseStringUTFChars(env, j_log, log_path);
    return JNI_TRUE;

fail:
    if (driver)
        (*env)->ReleaseStringUTFChars(env, j_driver, driver);
    if (dependencies)
        (*env)->ReleaseStringUTFChars(env, j_dependencies, dependencies);
    if (bundled)
        (*env)->ReleaseStringUTFChars(env, j_bundled_libraries, bundled);
    if (log_path)
        (*env)->ReleaseStringUTFChars(env, j_log, log_path);
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_dev_panvk_bench_Native_stopPreview(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    stop_preview();
}

JNIEXPORT void JNICALL
Java_dev_panvk_bench_Native_requestStop(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
}

JNIEXPORT void JNICALL
Java_dev_panvk_bench_Native_setDebugLogging(JNIEnv *env, jobject self,
                                             jboolean enabled)
{
    (void)env;
    (void)self;
    __atomic_store_n(&g_debug_logging, enabled == JNI_TRUE, __ATOMIC_RELEASE);
}

JNIEXPORT void JNICALL
Java_dev_panvk_bench_Native_setSceneRotation(JNIEnv *env, jobject self,
                                              jfloat degrees)
{
    int millidegrees;
    (void)env;
    (void)self;
    if (!(degrees >= -360.0f && degrees <= 360.0f))
        degrees = 0.0f;
    millidegrees = degrees * 1000.0f;
    __atomic_store_n(&g_scene_rotation_millidegrees, millidegrees,
                     __ATOMIC_RELEASE);
}
