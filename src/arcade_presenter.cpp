#include "arcade_presenter.h"

#include "arcade_config.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
std::array<int, 2> bounded_window_size(SDL_Window* window,
                                       int requested_width) {
    SDL_Rect usable{0, 0, 1920, 1080};
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) :
                                     SDL_GetPrimaryDisplay();
    if (!display || !SDL_GetDisplayUsableBounds(display, &usable)) {
        display = SDL_GetPrimaryDisplay();
        if (display) SDL_GetDisplayBounds(display, &usable);
    }
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
    const int maximum_width = std::max(320, std::min(
        usable.w - left - right, (usable.h - top - bottom) * 4 / 3));
    const int width = std::clamp(requested_width, 320, maximum_width);
    return {width, width * 3 / 4};
}

void set_fixed_window_size(SDL_Window* window, int requested_width) {
    const auto size = bounded_window_size(window, requested_width);
    const int width = size[0];
    const int height = size[1];
    SDL_SetWindowMinimumSize(window, width, height);
    SDL_SetWindowMaximumSize(window, width, height);
    SDL_SetWindowSize(window, width, height);
    SDL_RaiseWindow(window);
}

template <typename T>
T vk_structure(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

bool vk_ok(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "%s failed (Vulkan result %d)\n", operation,
                 static_cast<int>(result));
    return false;
}

// Composite a top-left, straight-alpha RGBA status panel after the arcade
// image has been scaled to the host drawable. The Vulkan swapchain can be
// RGBA or BGRA, so destination channel order is selected explicitly.
void composite_status_overlay(uint8_t* destination, int destination_width,
                              int destination_height,
                              std::size_t destination_pitch, bool bgra,
                              const uint8_t* overlay, int overlay_width,
                              int overlay_height) {
    if (!destination || !overlay || overlay_width <= 0 ||
        overlay_height <= 0)
        return;
    constexpr int margin = 12;
    const int copy_width = std::min(
        overlay_width, std::max(destination_width - margin, 0));
    const int copy_height = std::min(
        overlay_height, std::max(destination_height - margin, 0));
    for (int y = 0; y < copy_height; ++y) {
        uint8_t* output = destination +
            static_cast<std::size_t>(y + margin) * destination_pitch +
            static_cast<std::size_t>(margin) * 4;
        const uint8_t* input = overlay +
            static_cast<std::size_t>(y) * overlay_width * 4;
        for (int x = 0; x < copy_width; ++x) {
            const uint8_t alpha = input[x * 4 + 3];
            if (alpha == 0) continue;
            const uint8_t red = input[x * 4 + 0];
            const uint8_t green = input[x * 4 + 1];
            const uint8_t blue = input[x * 4 + 2];
            const uint8_t source[3]{bgra ? blue : red, green,
                                    bgra ? red : blue};
            for (int channel = 0; channel < 3; ++channel) {
                output[x * 4 + channel] = static_cast<uint8_t>(
                    (static_cast<unsigned>(source[channel]) * alpha +
                     static_cast<unsigned>(output[x * 4 + channel]) *
                         (255u - alpha) +
                     127u) /
                    255u);
            }
            output[x * 4 + 3] = 255;
        }
    }
}

VkSurfaceFormatKHR select_surface_format(
    const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const VkSurfaceFormatKHR& format : formats) {
        if ((format.format == VK_FORMAT_B8G8R8A8_UNORM ||
             format.format == VK_FORMAT_B8G8R8A8_SRGB) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    }
    return formats.front();
}

VkCompositeAlphaFlagBitsKHR select_composite_alpha(VkCompositeAlphaFlagsKHR flags) {
    constexpr VkCompositeAlphaFlagBitsKHR choices[]{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR choice : choices)
        if (flags & choice) return choice;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
} // namespace

struct alternate_presenter::implementation {
    renderer_backend backend{renderer_backend::software};
    emulator_settings settings{};
    SDL_Window* window{};
    SDL_Renderer* software_renderer{};
    SDL_Texture* software_texture{};
    SDL_Texture* software_overlay_texture{};
    int texture_width{};
    int texture_height{};
    int overlay_width{};
    int overlay_height{};
    std::vector<uint8_t> flipped_pixels;

    VkInstance instance{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    uint32_t queue_family{std::numeric_limits<uint32_t>::max()};
    VkQueue queue{};
    VkSwapchainKHR swapchain{};
    VkFormat swapchain_format{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> swapchain_images;
    VkCommandPool command_pool{};
    VkCommandBuffer command_buffer{};
    VkSemaphore image_available{};
    VkSemaphore transfer_complete{};
    VkFence frame_fence{};
    VkBuffer staging_buffer{};
    VkDeviceMemory staging_memory{};
    VkDeviceSize staging_size{};
    bool swapchain_dirty{true};
    bool first_present{true};

    uint32_t memory_type(uint32_t allowed, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
        for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
            if ((allowed & (1u << index)) &&
                (memory.memoryTypes[index].propertyFlags & properties) == properties)
                return index;
        }
        return std::numeric_limits<uint32_t>::max();
    }

    void destroy_staging() {
        if (staging_buffer) vkDestroyBuffer(device, staging_buffer, nullptr);
        if (staging_memory) vkFreeMemory(device, staging_memory, nullptr);
        staging_buffer = VK_NULL_HANDLE;
        staging_memory = VK_NULL_HANDLE;
        staging_size = 0;
    }

    bool create_staging(VkDeviceSize bytes) {
        if (staging_size >= bytes) return true;
        if (device) vkDeviceWaitIdle(device);
        destroy_staging();
        auto buffer_info = vk_structure<VkBufferCreateInfo>(
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        buffer_info.size = bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vk_ok(vkCreateBuffer(device, &buffer_info, nullptr, &staging_buffer),
                   "vkCreateBuffer"))
            return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, staging_buffer, &requirements);
        const uint32_t type = memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == std::numeric_limits<uint32_t>::max()) return false;
        auto allocation = vk_structure<VkMemoryAllocateInfo>(
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (!vk_ok(vkAllocateMemory(device, &allocation, nullptr, &staging_memory),
                   "vkAllocateMemory") ||
            !vk_ok(vkBindBufferMemory(device, staging_buffer, staging_memory, 0),
                   "vkBindBufferMemory"))
            return false;
        staging_size = bytes;
        return true;
    }

    bool create_swapchain() {
        int drawable_width = 0;
        int drawable_height = 0;
        SDL_GetWindowSizeInPixels(window, &drawable_width, &drawable_height);
        if (drawable_width <= 0 || drawable_height <= 0) return false;

        VkSurfaceCapabilitiesKHR capabilities{};
        if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                       physical_device, surface, &capabilities),
                   "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
            return false;
        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface,
                                             &format_count, nullptr);
        if (!format_count) return false;
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface,
                                             &format_count, formats.data());
        const VkSurfaceFormatKHR selected = select_surface_format(formats);

        uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface,
                                                  &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface,
                                                  &mode_count, modes.data());
        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!settings.vsync) {
            for (VkPresentModeKHR mode : modes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                    present_mode = mode;
                    break;
                }
                if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
                    present_mode = mode;
            }
        }

        VkExtent2D extent{};
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = std::clamp(static_cast<uint32_t>(drawable_width),
                                      capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(drawable_height),
                                       capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
            image_count = capabilities.maxImageCount;
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            std::fprintf(stderr, "Vulkan surface cannot accept transfer output\n");
            return false;
        }

        auto create = vk_structure<VkSwapchainCreateInfoKHR>(
            VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
        create.surface = surface;
        create.minImageCount = image_count;
        create.imageFormat = selected.format;
        create.imageColorSpace = selected.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = capabilities.currentTransform;
        create.compositeAlpha = select_composite_alpha(capabilities.supportedCompositeAlpha);
        create.presentMode = present_mode;
        create.clipped = VK_TRUE;
        create.oldSwapchain = swapchain;
        VkSwapchainKHR replacement{};
        if (!vk_ok(vkCreateSwapchainKHR(device, &create, nullptr, &replacement),
                   "vkCreateSwapchainKHR"))
            return false;
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = replacement;
        swapchain_format = selected.format;
        swapchain_extent = extent;
        vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
        swapchain_images.resize(image_count);
        vkGetSwapchainImagesKHR(device, swapchain, &image_count,
                                swapchain_images.data());
        swapchain_dirty = false;
        return create_staging(static_cast<VkDeviceSize>(extent.width) *
                              extent.height * 4);
    }

    bool initialize_vulkan() {
        Uint32 extension_count = 0;
        const char* const* sdl_extensions =
            SDL_Vulkan_GetInstanceExtensions(&extension_count);
        if (!sdl_extensions)
            return false;
        std::vector<const char*> extensions(
            sdl_extensions, sdl_extensions + extension_count);
        auto app = vk_structure<VkApplicationInfo>(
            VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "WhittyArcade";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "WhittyArcade Output";
        app.apiVersion = VK_API_VERSION_1_0;
        auto create = vk_structure<VkInstanceCreateInfo>(
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        create.pApplicationInfo = &app;
        create.enabledExtensionCount = extension_count;
        create.ppEnabledExtensionNames = extensions.data();
        if (!vk_ok(vkCreateInstance(&create, nullptr, &instance),
                   "vkCreateInstance") ||
            !SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
            return false;

        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
                                                     nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
                                                     families.data());
            for (uint32_t family = 0; family < family_count; ++family) {
                VkBool32 presents = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface,
                                                     &presents);
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) {
                    physical_device = candidate;
                    queue_family = family;
                    break;
                }
            }
            if (physical_device) break;
        }
        if (!physical_device) {
            std::fprintf(stderr, "No Vulkan presentation device found\n");
            return false;
        }
        const float priority = 1.0f;
        auto queue_info = vk_structure<VkDeviceQueueCreateInfo>(
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const char* device_extensions[]{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        auto device_info = vk_structure<VkDeviceCreateInfo>(
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = device_extensions;
        if (!vk_ok(vkCreateDevice(physical_device, &device_info, nullptr, &device),
                   "vkCreateDevice"))
            return false;
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        auto pool = vk_structure<VkCommandPoolCreateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool.queueFamilyIndex = queue_family;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (!vk_ok(vkCreateCommandPool(device, &pool, nullptr, &command_pool),
                   "vkCreateCommandPool"))
            return false;
        auto command = vk_structure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        command.commandPool = command_pool;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        if (!vk_ok(vkAllocateCommandBuffers(device, &command, &command_buffer),
                   "vkAllocateCommandBuffers"))
            return false;
        auto semaphore = vk_structure<VkSemaphoreCreateInfo>(
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        auto fence = vk_structure<VkFenceCreateInfo>(
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        return vk_ok(vkCreateSemaphore(device, &semaphore, nullptr,
                                       &image_available), "vkCreateSemaphore") &&
               vk_ok(vkCreateSemaphore(device, &semaphore, nullptr,
                                       &transfer_complete), "vkCreateSemaphore") &&
               vk_ok(vkCreateFence(device, &fence, nullptr, &frame_fence),
                     "vkCreateFence") && create_swapchain();
    }

    void shutdown_vulkan() {
        if (device) vkDeviceWaitIdle(device);
        destroy_staging();
        if (frame_fence) vkDestroyFence(device, frame_fence, nullptr);
        if (transfer_complete) vkDestroySemaphore(device, transfer_complete, nullptr);
        if (image_available) vkDestroySemaphore(device, image_available, nullptr);
        if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        surface = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
    }

    bool present_vulkan(const uint8_t* pixels, int width, int height,
                        const uint8_t* overlay, int overlay_width,
                        int overlay_height) {
        if (swapchain_dirty && !create_swapchain()) return true;
        vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX);
        uint32_t image_index = 0;
        VkResult acquired = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX, image_available, VK_NULL_HANDLE,
            &image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain_dirty = true;
            return true;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return vk_ok(acquired, "vkAcquireNextImageKHR");
        vkResetFences(device, 1, &frame_fence);

        void* mapped = nullptr;
        const std::size_t destination_pitch =
            static_cast<std::size_t>(swapchain_extent.width) * 4;
        if (!vk_ok(vkMapMemory(device, staging_memory, 0, staging_size, 0,
                               &mapped), "vkMapMemory"))
            return false;
        auto* destination = static_cast<uint8_t*>(mapped);
        std::memset(destination, 0,
                    destination_pitch * swapchain_extent.height);
        const bool bgra = swapchain_format == VK_FORMAT_B8G8R8A8_UNORM ||
                          swapchain_format == VK_FORMAT_B8G8R8A8_SRGB;
        const int surface_w = static_cast<int>(swapchain_extent.width);
        const int surface_h = static_cast<int>(swapchain_extent.height);
        // One pane, or two side-by-side panes for fullscreen dual output; each
        // letterboxes the arcade frame within its own half of the swapchain.
        struct present_pane { int x; int w; };
        std::array<present_pane, 2> panes{{{0, surface_w}, {0, 0}}};
        int pane_count = 1;
        if (settings.output == output_mode::dual && settings.fullscreen) {
            const int gap = std::max(surface_w / 40, 24);
            const int left_w = std::clamp((surface_w - gap) / 2, 1,
                                          surface_w - 1);
            panes[0] = {0, left_w};
            panes[1] = {left_w + gap, surface_w - left_w - gap};
            pane_count = 2;
        }
        for (int pane = 0; pane < pane_count; ++pane) {
            int copy_width = panes[pane].w;
            int copy_height = surface_h;
            if (settings.integer_scaling) {
                const int scale = std::min(copy_width / width,
                                           copy_height / height);
                if (scale >= 1) {
                    copy_width = width * scale;
                    copy_height = height * scale;
                }
            } else if (copy_width * height > copy_height * width) {
                copy_width = copy_height * width / height;
            } else {
                copy_height = copy_width * height / width;
            }
            const int x_offset =
                panes[pane].x + (panes[pane].w - copy_width) / 2;
            const int y_offset = (surface_h - copy_height) / 2;
            for (int y = 0; y < copy_height; ++y) {
                const int source_y = height - 1 -
                    (y * height / std::max(copy_height, 1));
                const uint8_t* source = pixels +
                    (static_cast<std::size_t>(source_y) * width) * 4;
                uint8_t* row = destination +
                    (static_cast<std::size_t>(y + y_offset) *
                     swapchain_extent.width + x_offset) * 4;
                for (int x = 0; x < copy_width; ++x) {
                    const uint8_t* pixel = source +
                        (x * width / std::max(copy_width, 1)) * 4;
                    row[x * 4 + 0] = bgra ? pixel[2] : pixel[0];
                    row[x * 4 + 1] = pixel[1];
                    row[x * 4 + 2] = bgra ? pixel[0] : pixel[2];
                    row[x * 4 + 3] = pixel[3];
                }
            }
        }
        composite_status_overlay(
            destination, static_cast<int>(swapchain_extent.width),
            static_cast<int>(swapchain_extent.height), destination_pitch,
            bgra, overlay, overlay_width, overlay_height);
        vkUnmapMemory(device, staging_memory);

        vkResetCommandBuffer(command_buffer, 0);
        auto begin = vk_structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        vkBeginCommandBuffer(command_buffer, &begin);
        auto to_transfer = vk_structure<VkImageMemoryBarrier>(
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        to_transfer.srcAccessMask = 0;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = swapchain_images[image_index];
        to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_transfer.subresourceRange.levelCount = 1;
        to_transfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_transfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {swapchain_extent.width, swapchain_extent.height, 1};
        vkCmdCopyBufferToImage(command_buffer, staging_buffer,
                               swapchain_images[image_index],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier to_present = to_transfer;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &to_present);
        vkEndCommandBuffer(command_buffer);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        auto submit = vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &transfer_complete;
        if (!vk_ok(vkQueueSubmit(queue, 1, &submit, frame_fence),
                   "vkQueueSubmit"))
            return false;
        auto present = vk_structure<VkPresentInfoKHR>(
            VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &transfer_complete;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        const VkResult result = vkQueuePresentKHR(queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty = true;
            return true;
        }
        const bool presented = vk_ok(result, "vkQueuePresentKHR");
        if (presented && first_present) {
            SDL_ShowWindow(window);
            first_present = false;
        }
        return presented;
    }
};

alternate_presenter::alternate_presenter() = default;
alternate_presenter::~alternate_presenter() { shutdown(); }

bool alternate_presenter::initialize(renderer_backend backend, int width,
                                     int height,
                                     const emulator_settings& settings) {
    shutdown();
    if (backend == renderer_backend::opengl) return false;
    m_impl = std::make_unique<implementation>();
    m_impl->backend = backend;
    m_impl->settings = settings;
    // X11 compositors only see a fullscreen request once the window is
    // mapped.  Setting SDL_WINDOW_FULLSCREEN_DESKTOP on a hidden window and
    // showing it after the first present leaves SDL believing it is
    // fullscreen while the compositor tiles it as an ordinary window.  That
    // made the live drawable jump between arbitrary tile sizes.  Create an
    // initially-fullscreen output in the mapped state so SDL can publish the
    // WM state as part of the initial map.
    const SDL_WindowFlags visibility_flags = settings.fullscreen ?
        SDL_WINDOW_FULLSCREEN :
        SDL_WINDOW_HIDDEN;
    const SDL_WindowFlags flags = visibility_flags | SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIGH_PIXEL_DENSITY |
        (backend == renderer_backend::vulkan ? SDL_WINDOW_VULKAN : 0);
    m_impl->window = SDL_CreateWindow(
        backend == renderer_backend::vulkan ? "WhittyArcade - Vulkan" :
                                              "WhittyArcade - Software",
        width, height, flags);
    if (!m_impl->window) {
        std::fprintf(stderr, "Output window creation failed: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    m_impl->first_present = !settings.fullscreen;
    if (backend == renderer_backend::vulkan) {
        if (!m_impl->initialize_vulkan()) {
            shutdown();
            return false;
        }
    } else {
        m_impl->software_renderer = SDL_CreateRenderer(
            m_impl->window, "software");
        if (!m_impl->software_renderer) {
            std::fprintf(stderr, "Software output creation failed: %s\n",
                         SDL_GetError());
            shutdown();
            return false;
        }
    }
    apply_settings(settings);
    std::printf("%s presentation initialized\n", renderer_backend_name(backend));
    return true;
}

void alternate_presenter::shutdown() {
    if (!m_impl) return;
    if (m_impl->backend == renderer_backend::vulkan)
        m_impl->shutdown_vulkan();
    if (m_impl->software_overlay_texture)
        SDL_DestroyTexture(m_impl->software_overlay_texture);
    if (m_impl->software_texture)
        SDL_DestroyTexture(m_impl->software_texture);
    if (m_impl->software_renderer)
        SDL_DestroyRenderer(m_impl->software_renderer);
    if (m_impl->window) SDL_DestroyWindow(m_impl->window);
    m_impl.reset();
}

void alternate_presenter::apply_settings(const emulator_settings& settings) {
    if (!m_impl || !m_impl->window) return;
    const bool vsync_changed = m_impl->settings.vsync != settings.vsync;
    const bool fullscreen_changed =
        m_impl->settings.fullscreen != settings.fullscreen;
    m_impl->settings = settings;
    if (settings.fullscreen) {
        SDL_SetWindowMinimumSize(m_impl->window, 1, 1);
        SDL_SetWindowMaximumSize(m_impl->window, 16384, 16384);
        SDL_ShowWindow(m_impl->window);
        if (!SDL_SetWindowFullscreen(m_impl->window, true))
            std::fprintf(stderr, "Fullscreen request failed: %s\n",
                         SDL_GetError());
        m_impl->first_present = false;
    } else {
        SDL_SetWindowFullscreen(m_impl->window, false);
        set_fixed_window_size(m_impl->window, settings.window_width);
        SDL_SetWindowPosition(m_impl->window, SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED);
    }
    if ((vsync_changed || fullscreen_changed) &&
        m_impl->backend == renderer_backend::vulkan)
        m_impl->swapchain_dirty = true;
}

void alternate_presenter::drawable_size(int& width, int& height) const {
    width = height = 1;
    if (!m_impl || !m_impl->window) return;
    if (m_impl->backend == renderer_backend::vulkan) {
        SDL_GetWindowSizeInPixels(m_impl->window, &width, &height);
    } else if (m_impl->software_renderer) {
        SDL_GetCurrentRenderOutputSize(m_impl->software_renderer, &width, &height);
    }
    width = std::max(width, 1);
    height = std::max(height, 1);
}

void* alternate_presenter::window() const {
    return m_impl ? m_impl->window : nullptr;
}

renderer_backend alternate_presenter::backend() const {
    return m_impl ? m_impl->backend : renderer_backend::opengl;
}

bool alternate_presenter::present_rgba_bottom_up(const uint8_t* pixels,
                                                  int width, int height,
                                                  const uint8_t* overlay_pixels,
                                                  int overlay_width,
                                                  int overlay_height) {
    if (!m_impl || !pixels || width <= 0 || height <= 0) return false;
    if (m_impl->backend == renderer_backend::vulkan)
        return m_impl->present_vulkan(pixels, width, height, overlay_pixels,
                                      overlay_width, overlay_height);
    if (!m_impl->software_texture || width != m_impl->texture_width ||
        height != m_impl->texture_height) {
        if (m_impl->software_texture)
            SDL_DestroyTexture(m_impl->software_texture);
        m_impl->software_texture = SDL_CreateTexture(
            m_impl->software_renderer, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING, width, height);
        m_impl->texture_width = width;
        m_impl->texture_height = height;
    }
    if (!m_impl->software_texture) return false;
    m_impl->flipped_pixels.resize(static_cast<std::size_t>(width) * height * 4);
    const std::size_t pitch = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y)
        std::memcpy(m_impl->flipped_pixels.data() + y * pitch,
                    pixels + (height - 1 - y) * pitch, pitch);
    SDL_UpdateTexture(m_impl->software_texture, nullptr,
                      m_impl->flipped_pixels.data(), static_cast<int>(pitch));
    SDL_SetRenderDrawColor(m_impl->software_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_impl->software_renderer);
    int output_width = 1;
    int output_height = 1;
    drawable_size(output_width, output_height);
    // One pane, or two side-by-side panes for fullscreen dual output. Each pane
    // keeps the arcade framebuffer's aspect and letterboxes within its half
    // instead of stretching to the (possibly ultrawide) drawable.
    struct present_pane { int x; int w; };
    std::array<present_pane, 2> panes{{{0, output_width}, {0, 0}}};
    int pane_count = 1;
    if (m_impl->settings.output == output_mode::dual &&
        m_impl->settings.fullscreen) {
        const int gap = std::max(output_width / 40, 24);
        const int left_w = std::clamp((output_width - gap) / 2, 1,
                                      output_width - 1);
        panes[0] = {0, left_w};
        panes[1] = {left_w + gap, output_width - left_w - gap};
        pane_count = 2;
    }
    for (int pane = 0; pane < pane_count; ++pane) {
        int copy_width = panes[pane].w;
        int copy_height = output_height;
        if (m_impl->settings.integer_scaling) {
            const int scale = std::min(copy_width / width, copy_height / height);
            if (scale >= 1) {
                copy_width = width * scale;
                copy_height = height * scale;
            }
        } else if (copy_width * height > copy_height * width) {
            copy_width = copy_height * width / height;
        } else {
            copy_height = copy_width * height / width;
        }
        const SDL_FRect destination{
            static_cast<float>(panes[pane].x +
                               (panes[pane].w - copy_width) / 2),
            static_cast<float>((output_height - copy_height) / 2),
            static_cast<float>(copy_width), static_cast<float>(copy_height)};
        SDL_RenderTexture(m_impl->software_renderer, m_impl->software_texture,
                          nullptr, &destination);
    }
    if (overlay_pixels && overlay_width > 0 && overlay_height > 0) {
        if (!m_impl->software_overlay_texture ||
            overlay_width != m_impl->overlay_width ||
            overlay_height != m_impl->overlay_height) {
            if (m_impl->software_overlay_texture)
                SDL_DestroyTexture(m_impl->software_overlay_texture);
            m_impl->software_overlay_texture = SDL_CreateTexture(
                m_impl->software_renderer, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STREAMING, overlay_width, overlay_height);
            m_impl->overlay_width = overlay_width;
            m_impl->overlay_height = overlay_height;
            if (m_impl->software_overlay_texture)
                SDL_SetTextureBlendMode(m_impl->software_overlay_texture,
                                        SDL_BLENDMODE_BLEND);
        }
        if (m_impl->software_overlay_texture) {
            SDL_UpdateTexture(m_impl->software_overlay_texture, nullptr,
                              overlay_pixels, overlay_width * 4);
            const SDL_FRect overlay_destination{
                12.0f, 12.0f, static_cast<float>(overlay_width),
                static_cast<float>(overlay_height)};
            SDL_RenderTexture(m_impl->software_renderer,
                              m_impl->software_overlay_texture, nullptr,
                              &overlay_destination);
        }
    }
    SDL_RenderPresent(m_impl->software_renderer);
    if (m_impl->first_present) {
        SDL_ShowWindow(m_impl->window);
        m_impl->first_present = false;
    }
    return true;
}
