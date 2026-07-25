// Executes the Vulkan presentation shaders offscreen and checks the pixels
// they produce. No window, no swapchain and no emulated board are involved:
// the pipeline state mirrors what arcade_presenter.cpp builds, so the things
// this catches - a flipped image, a scrambled push-constant block, a shader
// whose bindings drifted from the C++ - are exactly the failures that are
// otherwise invisible until a board is on screen.
//
// The test skips (rather than fails) when no Vulkan device is present, so a
// headless CI box without a GPU does not report a false negative.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

// Compiling this header runs its static_asserts, which pin the System 22
// uniform block layouts to the offsets read out of the compiled SPIR-V.
#include "system22_vulkan_uniforms.h"

#include "arcade_post_vert.h"
#include "arcade_post_frag.h"
#include "arcade_overlay_vert.h"
#include "arcade_overlay_frag.h"
#include "system22_polygon_vert.h"
#include "system22_polygon_frag.h"

namespace {

// Value-initialises a Vulkan structure and stamps its type tag, matching the
// helper in src/arcade_presenter.cpp.
template <typename T>
T vk_structure(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

struct rgba {
    uint8_t r, g, b, a;
    bool operator==(const rgba& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

// Same layout as post_push_constants in src/arcade_presenter.cpp.
struct post_constants {
    uint32_t screen_fade[3]{};
    uint32_t super_system22{};
    uint32_t apply_board_color{};
    uint32_t flip_y{};
    uint32_t linear_filtering{};
    float pane_width{};
    float pane_height{};
};

class vulkan_harness {
public:
    bool start() {
        auto app = vk_structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "vulkan_post_pipeline_test";
        app.apiVersion = VK_API_VERSION_1_0;
        auto instance_info =
            vk_structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instance_info.pApplicationInfo = &app;
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
            return false;
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (!count) return false;
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        physical_device = devices[0];
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count,
                                                 nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count,
                                                 families.data());
        for (uint32_t index = 0; index < count; ++index) {
            if (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queue_family = index;
                break;
            }
        }
        if (queue_family == std::numeric_limits<uint32_t>::max()) return false;
        const float priority = 1.0f;
        auto queue_info = vk_structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        // The System 22 polygon shader writes a second blend source, which is
        // an optional feature and must be enabled to use it.
        VkPhysicalDeviceFeatures available{};
        vkGetPhysicalDeviceFeatures(physical_device, &available);
        VkPhysicalDeviceFeatures enabled{};
        enabled.dualSrcBlend = available.dualSrcBlend;
        auto device_info =
            vk_structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.pEnabledFeatures = &enabled;
        if (vkCreateDevice(physical_device, &device_info, nullptr, &device) !=
            VK_SUCCESS)
            return false;
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        auto pool_info =
            vk_structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool_info.queueFamilyIndex = queue_family;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        return vkCreateCommandPool(device, &pool_info, nullptr,
                                   &command_pool) == VK_SUCCESS;
    }

    ~vulkan_harness() {
        if (device) vkDeviceWaitIdle(device);
        if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    uint32_t memory_type(uint32_t allowed, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
        for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
            if ((allowed & (1u << index)) &&
                (memory.memoryTypes[index].propertyFlags & flags) == flags)
                return index;
        }
        return std::numeric_limits<uint32_t>::max();
    }

    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool command_pool{};
    uint32_t queue_family{std::numeric_limits<uint32_t>::max()};
};

struct test_image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
};

test_image make_image(vulkan_harness& vk, int width, int height, int layers,
                      VkFormat format, VkImageUsageFlags usage) {
    test_image target{};
    auto info = vk_structure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {static_cast<uint32_t>(width),
                   static_cast<uint32_t>(height), 1};
    info.mipLevels = 1;
    info.arrayLayers = static_cast<uint32_t>(layers);
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &info, nullptr, &target.image) != VK_SUCCESS)
        return target;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(vk.device, target.image, &requirements);
    auto allocation =
        vk_structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = vk.memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(vk.device, &allocation, nullptr, &target.memory);
    vkBindImageMemory(vk.device, target.image, target.memory, 0);
    auto view_info = vk_structure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view_info.image = target.image;
    view_info.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                                      VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = static_cast<uint32_t>(layers);
    vkCreateImageView(vk.device, &view_info, nullptr, &target.view);
    return target;
}

void destroy_image(vulkan_harness& vk, test_image& target) {
    if (target.view) vkDestroyImageView(vk.device, target.view, nullptr);
    if (target.image) vkDestroyImage(vk.device, target.image, nullptr);
    if (target.memory) vkFreeMemory(vk.device, target.memory, nullptr);
    target = test_image{};
}

struct host_buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
};

host_buffer make_host_buffer(vulkan_harness& vk, VkDeviceSize bytes,
                             VkBufferUsageFlags usage) {
    host_buffer target{};
    auto info = vk_structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = bytes;
    info.usage = usage;
    vkCreateBuffer(vk.device, &info, nullptr, &target.buffer);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(vk.device, target.buffer, &requirements);
    auto allocation =
        vk_structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = vk.memory_type(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk.device, &allocation, nullptr, &target.memory);
    vkBindBufferMemory(vk.device, target.buffer, target.memory, 0);
    vkMapMemory(vk.device, target.memory, 0, requirements.size, 0,
                &target.mapped);
    return target;
}

void destroy_host_buffer(vulkan_harness& vk, host_buffer& target) {
    if (target.mapped) vkUnmapMemory(vk.device, target.memory);
    if (target.buffer) vkDestroyBuffer(vk.device, target.buffer, nullptr);
    if (target.memory) vkFreeMemory(vk.device, target.memory, nullptr);
    target = host_buffer{};
}

VkShaderModule make_shader(vulkan_harness& vk, const uint32_t* code,
                           std::size_t bytes) {
    auto info =
        vk_structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module{};
    vkCreateShaderModule(vk.device, &info, nullptr, &module);
    return module;
}

void barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from,
             VkImageLayout to, VkAccessFlags source_access,
             VkAccessFlags destination_access, VkPipelineStageFlags source,
             VkPipelineStageFlags destination, int layers = 1) {
    auto info = vk_structure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    info.oldLayout = from;
    info.newLayout = to;
    info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    info.image = image;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = static_cast<uint32_t>(layers);
    info.srcAccessMask = source_access;
    info.dstAccessMask = destination_access;
    vkCmdPipelineBarrier(commands, source, destination, 0, 0, nullptr, 0,
                         nullptr, 1, &info);
}

constexpr int source_size = 4;
constexpr int target_size = 8;

// One distinct colour per source row, so a vertical flip is unmistakable in
// the readback.
constexpr std::array<rgba, source_size> row_colors{{
    {255, 0, 0, 255},     // row 0
    {0, 255, 0, 255},     // row 1
    {0, 0, 255, 255},     // row 2
    {255, 255, 0, 255},   // row 3
}};

// Renders the post-process pipeline over a 4-row source into an 8x8 target and
// returns the result, top row first. use_overlay swaps in the overlay shaders,
// which share the quad geometry but sample a plain RGBA texture.
std::vector<rgba> render(vulkan_harness& vk, bool flip_y, bool use_overlay,
                         bool linear_filtering = false) {
    std::vector<rgba> output;

    test_image scene = make_image(
        vk, source_size, source_size, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    test_image gamma = make_image(
        vk, 256, 1, 3, VK_FORMAT_R8_UINT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    test_image target = make_image(
        vk, target_size, target_size, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!scene.view || !gamma.view || !target.view) {
        check(false, "offscreen resources could not be created");
        return output;
    }

    std::vector<uint8_t> scene_pixels(
        static_cast<std::size_t>(source_size) * source_size * 4);
    for (int y = 0; y < source_size; ++y)
        for (int x = 0; x < source_size; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * source_size + x) * 4;
            const rgba color = row_colors[static_cast<std::size_t>(y)];
            scene_pixels[offset + 0] = color.r;
            scene_pixels[offset + 1] = color.g;
            scene_pixels[offset + 2] = color.b;
            scene_pixels[offset + 3] = color.a;
        }
    std::array<uint8_t, 256 * 3> gamma_pixels{};
    for (int channel = 0; channel < 3; ++channel)
        for (int level = 0; level < 256; ++level)
            gamma_pixels[static_cast<std::size_t>(channel) * 256 + level] =
                static_cast<uint8_t>(level);

    host_buffer upload = make_host_buffer(vk, scene_pixels.size() +
                                                  gamma_pixels.size(),
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    host_buffer readback = make_host_buffer(
        vk, static_cast<VkDeviceSize>(target_size) * target_size * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memcpy(upload.mapped, scene_pixels.data(), scene_pixels.size());
    std::memcpy(static_cast<uint8_t*>(upload.mapped) + scene_pixels.size(),
                gamma_pixels.data(), gamma_pixels.size());

    // Render pass matching the presenter's: one colour attachment, cleared.
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference reference{};
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    auto pass_info =
        vk_structure<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    VkRenderPass render_pass{};
    vkCreateRenderPass(vk.device, &pass_info, nullptr, &render_pass);

    auto framebuffer_info =
        vk_structure<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &target.view;
    framebuffer_info.width = target_size;
    framebuffer_info.height = target_size;
    framebuffer_info.layers = 1;
    VkFramebuffer framebuffer{};
    vkCreateFramebuffer(vk.device, &framebuffer_info, nullptr, &framebuffer);

    auto sampler_info = vk_structure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    // The presenter binds a linear sampler whenever linear filtering is on,
    // so the sharp-bilinear path must be exercised through one here too -
    // with a nearest sampler the shader's coordinate maths has no observable
    // effect and the check would pass no matter what the shader did.
    const VkFilter filter =
        linear_filtering ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler_info.magFilter = filter;
    sampler_info.minFilter = filter;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{};
    vkCreateSampler(vk.device, &sampler_info, nullptr, &sampler);

    const uint32_t binding_count = use_overlay ? 1u : 2u;
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t index = 0; index < binding_count; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    auto set_info = vk_structure<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    set_info.bindingCount = binding_count;
    set_info.pBindings = bindings.data();
    VkDescriptorSetLayout set_layout{};
    vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &set_layout);

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(post_constants);
    auto layout_info = vk_structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = use_overlay ? 0u : 1u;
    layout_info.pPushConstantRanges = use_overlay ? nullptr : &range;
    VkPipelineLayout pipeline_layout{};
    vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &pipeline_layout);

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 2;
    auto pool_info = vk_structure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool descriptor_pool{};
    vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &descriptor_pool);
    auto allocate = vk_structure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout;
    VkDescriptorSet descriptor_set{};
    vkAllocateDescriptorSets(vk.device, &allocate, &descriptor_set);

    const VkShaderModule vertex = make_shader(
        vk,
        use_overlay ? arcade_overlay_vert_spirv : arcade_post_vert_spirv,
        use_overlay ? sizeof(arcade_overlay_vert_spirv)
                    : sizeof(arcade_post_vert_spirv));
    const VkShaderModule fragment = make_shader(
        vk,
        use_overlay ? arcade_overlay_frag_spirv : arcade_post_frag_spirv,
        use_overlay ? sizeof(arcade_overlay_frag_spirv)
                    : sizeof(arcade_post_frag_spirv));

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = vk_structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1] = vk_structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    auto vertex_input = vk_structure<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    auto assembly = vk_structure<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(target_size),
                        static_cast<float>(target_size), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {target_size, target_size}};
    auto viewport_state = vk_structure<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    auto raster = vk_structure<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    auto multisample = vk_structure<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    auto blend = vk_structure<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    auto pipeline_info = vk_structure<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    VkPipeline pipeline{};
    const bool built = vkCreateGraphicsPipelines(
        vk.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        &pipeline) == VK_SUCCESS;
    check(built, "graphics pipeline could not be created");

    if (built) {
        std::array<VkDescriptorImageInfo, 2> image_info{};
        image_info[0].sampler = sampler;
        image_info[0].imageView = scene.view;
        image_info[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info[1].sampler = sampler;
        image_info[1].imageView = gamma.view;
        image_info[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t index = 0; index < binding_count; ++index) {
            writes[index] = vk_structure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[index].dstSet = descriptor_set;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index].pImageInfo = &image_info[index];
        }
        vkUpdateDescriptorSets(vk.device, binding_count, writes.data(), 0,
                               nullptr);

        auto command_info = vk_structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        command_info.commandPool = vk.command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        VkCommandBuffer commands{};
        vkAllocateCommandBuffers(vk.device, &command_info, &commands);
        auto begin = vk_structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);

        barrier(commands, scene.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {source_size, source_size, 1};
        vkCmdCopyBufferToImage(commands, upload.buffer, scene.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(commands, scene.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        barrier(commands, gamma.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 3);
        VkBufferImageCopy gamma_copy{};
        gamma_copy.bufferOffset = scene_pixels.size();
        gamma_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        gamma_copy.imageSubresource.layerCount = 3;
        gamma_copy.imageExtent = {256, 1, 1};
        vkCmdCopyBufferToImage(commands, upload.buffer, gamma.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &gamma_copy);
        barrier(commands, gamma.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 3);

        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        auto pass_begin = vk_structure<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass_begin.renderPass = render_pass;
        pass_begin.framebuffer = framebuffer;
        pass_begin.renderArea.extent = {target_size, target_size};
        pass_begin.clearValueCount = 1;
        pass_begin.pClearValues = &clear;
        vkCmdBeginRenderPass(commands, &pass_begin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout, 0, 1, &descriptor_set, 0,
                                nullptr);
        if (!use_overlay) {
            post_constants constants{};
            constants.flip_y = flip_y ? 1u : 0u;
            constants.linear_filtering = linear_filtering ? 1u : 0u;
            constants.pane_width = static_cast<float>(target_size);
            constants.pane_height = static_cast<float>(target_size);
            vkCmdPushConstants(commands, pipeline_layout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(constants), &constants);
        }
        vkCmdDraw(commands, 4, 1, 0, 0);
        vkCmdEndRenderPass(commands);

        VkBufferImageCopy download{};
        download.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        download.imageSubresource.layerCount = 1;
        download.imageExtent = {target_size, target_size, 1};
        vkCmdCopyImageToBuffer(commands, target.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer, 1, &download);
        vkEndCommandBuffer(commands);

        auto submit = vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk.queue);
        vkFreeCommandBuffers(vk.device, vk.command_pool, 1, &commands);

        output.resize(static_cast<std::size_t>(target_size) * target_size);
        std::memcpy(output.data(), readback.mapped,
                    output.size() * sizeof(rgba));
    }

    if (pipeline) vkDestroyPipeline(vk.device, pipeline, nullptr);
    vkDestroyShaderModule(vk.device, vertex, nullptr);
    vkDestroyShaderModule(vk.device, fragment, nullptr);
    vkDestroyDescriptorPool(vk.device, descriptor_pool, nullptr);
    vkDestroyPipelineLayout(vk.device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(vk.device, set_layout, nullptr);
    vkDestroySampler(vk.device, sampler, nullptr);
    vkDestroyFramebuffer(vk.device, framebuffer, nullptr);
    vkDestroyRenderPass(vk.device, render_pass, nullptr);
    destroy_host_buffer(vk, readback);
    destroy_host_buffer(vk, upload);
    destroy_image(vk, target);
    destroy_image(vk, gamma);
    destroy_image(vk, scene);
    return output;
}

rgba pixel_at(const std::vector<rgba>& image, int x, int y) {
    return image[static_cast<std::size_t>(y) * target_size + x];
}

// ---------------------------------------------------------------------------
// System 22 polygon pipeline
//
// Drives the real polygon shaders over a quad covering the top half of the
// board with every input chosen so exactly one palette entry can be produced.
// A wrong tile address, a shifted uniform block or an inverted Y all change
// the answer, and none of them would look obviously broken on a screen.
// ---------------------------------------------------------------------------

// Matches the attribute locations in shaders/system22_polygon.vert.
struct system22_vertex {
    float position[3];
    float u_over_z;
    float v_over_z;
    float brightness_over_z;
    float one_over_z;
    float palette[2];
    float color_mode;
    float texture_bank;
    float direct;
    float fog;
    float fog_color[3];
    float viewport[2];
    float clip_rect[4];
    float render_flags[3];
};

// The pen stored in the texture sheet, and the palette entry it selects.
constexpr uint8_t polygon_pen = 5;
constexpr rgba polygon_color{200, 40, 90, 0};

std::vector<rgba> render_system22(vulkan_harness& vk, bool& dual_source_ok) {
    std::vector<rgba> output;

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(vk.physical_device, &features);
    dual_source_ok = features.dualSrcBlend == VK_TRUE;
    if (!dual_source_ok) return output;

    // One texel each: the decode has to land on index zero of every sheet for
    // the expected palette entry to come out.
    test_image tiles = make_image(
        vk, 1, 1, 1, VK_FORMAT_R8_UINT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    test_image map = make_image(
        vk, 1, 1, 1, VK_FORMAT_R16_UINT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    test_image attr = make_image(
        vk, 1, 1, 1, VK_FORMAT_R8_UINT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    test_image palette = make_image(
        vk, 256, 1, 3, VK_FORMAT_R8_UINT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    test_image sprites = make_image(
        vk, 1, 1, 1, VK_FORMAT_R8_UINT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    test_image target = make_image(
        vk, target_size, target_size, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!tiles.view || !map.view || !attr.view || !palette.view ||
        !sprites.view || !target.view) {
        check(false, "System 22 offscreen resources could not be created");
        return output;
    }

    // Palette layers hold R, G and B separately, indexed by pen.
    std::array<uint8_t, 256 * 3> palette_pixels{};
    palette_pixels[polygon_pen] = polygon_color.r;
    palette_pixels[256 + polygon_pen] = polygon_color.g;
    palette_pixels[512 + polygon_pen] = polygon_color.b;

    struct staged { const void* data; std::size_t bytes; VkDeviceSize offset; };
    const uint8_t tile_pen = polygon_pen;
    const uint16_t map_entry = 0;
    const uint8_t attr_entry = 0;
    const uint8_t sprite_entry = 0;
    std::array<staged, 5> staging{{
        {&tile_pen, 1, 0},
        {&map_entry, 2, 4},
        {&attr_entry, 1, 8},
        {palette_pixels.data(), palette_pixels.size(), 12},
        {&sprite_entry, 1, 12 + palette_pixels.size()},
    }};
    const VkDeviceSize staging_bytes = 12 + palette_pixels.size() + 4;

    system22_uniform_block uniforms{};
    // Identity view matrix: the vertex positions below are already in the
    // board's own coordinate space.
    for (int i = 0; i < 4; ++i) uniforms.view_matrix[i * 5] = 1.0f;
    // texture_control.x = 0 address base, so tile address zero is in range.
    // super_system22 (z) stays 0, which keeps shading after fog and makes
    // source_opacity return 1.0 - an opaque, exactly predictable pixel.

    host_buffer upload = make_host_buffer(vk, staging_bytes,
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    host_buffer uniform_buffer = make_host_buffer(
        vk, sizeof(uniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    // A quad spanning the full board width and the top half of its height.
    // Board +Y is up for direct polygons, so y = +240 is the top edge.
    const auto vertex = [](float x, float y) {
        system22_vertex v{};
        v.position[0] = x;
        v.position[1] = y;
        v.position[2] = 1.0f;
        v.one_over_z = 1.0f;
        // shade = (brightness_over_z / one_over_z) / 64, so 64 is unity.
        v.brightness_over_z = 64.0f;
        v.direct = 1.0f;
        v.clip_rect[0] = 0.0f;
        v.clip_rect[1] = static_cast<float>(target_size - 1);
        v.clip_rect[2] = 0.0f;
        v.clip_rect[3] = static_cast<float>(target_size - 1);
        return v;
    };
    const std::array<system22_vertex, 4> vertices{{
        vertex(-320.0f, 0.0f), vertex(320.0f, 0.0f),
        vertex(-320.0f, 240.0f), vertex(320.0f, 240.0f),
    }};
    host_buffer vertex_buffer = make_host_buffer(
        vk, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    host_buffer readback = make_host_buffer(
        vk, static_cast<VkDeviceSize>(target_size) * target_size * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (const staged& item : staging)
        std::memcpy(static_cast<uint8_t*>(upload.mapped) + item.offset,
                    item.data, item.bytes);
    std::memcpy(uniform_buffer.mapped, &uniforms, sizeof(uniforms));
    std::memcpy(vertex_buffer.mapped, vertices.data(), sizeof(vertices));

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference reference{};
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    auto pass_info = vk_structure<VkRenderPassCreateInfo>(
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    VkRenderPass render_pass{};
    vkCreateRenderPass(vk.device, &pass_info, nullptr, &render_pass);

    auto framebuffer_info = vk_structure<VkFramebufferCreateInfo>(
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &target.view;
    framebuffer_info.width = target_size;
    framebuffer_info.height = target_size;
    framebuffer_info.layers = 1;
    VkFramebuffer framebuffer{};
    vkCreateFramebuffer(vk.device, &framebuffer_info, nullptr, &framebuffer);

    auto sampler_info =
        vk_structure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    VkSampler sampler{};
    vkCreateSampler(vk.device, &sampler_info, nullptr, &sampler);

    // Binding 0 is the uniform block; 1..5 are the sheets.
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = index == 0 ?
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = index == 0 ?
            (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) :
            VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    auto set_info = vk_structure<VkDescriptorSetLayoutCreateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    set_info.bindingCount = static_cast<uint32_t>(bindings.size());
    set_info.pBindings = bindings.data();
    VkDescriptorSetLayout set_layout{};
    vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &set_layout);
    auto layout_info = vk_structure<VkPipelineLayoutCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout pipeline_layout{};
    vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &pipeline_layout);

    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 5;
    auto pool_info = vk_structure<VkDescriptorPoolCreateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    VkDescriptorPool descriptor_pool{};
    vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &descriptor_pool);
    auto allocate = vk_structure<VkDescriptorSetAllocateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout;
    VkDescriptorSet descriptor_set{};
    vkAllocateDescriptorSets(vk.device, &allocate, &descriptor_set);

    const VkShaderModule vertex_module = make_shader(
        vk, system22_polygon_vert_spirv, sizeof(system22_polygon_vert_spirv));
    const VkShaderModule fragment_module = make_shader(
        vk, system22_polygon_frag_spirv, sizeof(system22_polygon_frag_spirv));

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = vk_structure<VkPipelineShaderStageCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1] = vk_structure<VkPipelineShaderStageCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(system22_vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    const std::array<VkVertexInputAttributeDescription, 14> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(system22_vertex, position)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(system22_vertex, u_over_z)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(system22_vertex, v_over_z)},
        {3, 0, VK_FORMAT_R32_SFLOAT,
         offsetof(system22_vertex, brightness_over_z)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(system22_vertex, one_over_z)},
        {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(system22_vertex, palette)},
        {6, 0, VK_FORMAT_R32_SFLOAT, offsetof(system22_vertex, color_mode)},
        {7, 0, VK_FORMAT_R32_SFLOAT, offsetof(system22_vertex, texture_bank)},
        {8, 0, VK_FORMAT_R32_SFLOAT, offsetof(system22_vertex, direct)},
        {9, 0, VK_FORMAT_R32_SFLOAT, offsetof(system22_vertex, fog)},
        {10, 0, VK_FORMAT_R32G32B32_SFLOAT,
         offsetof(system22_vertex, fog_color)},
        {11, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(system22_vertex, viewport)},
        {12, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         offsetof(system22_vertex, clip_rect)},
        {13, 0, VK_FORMAT_R32G32B32_SFLOAT,
         offsetof(system22_vertex, render_flags)},
    }};
    auto vertex_input = vk_structure<VkPipelineVertexInputStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    auto assembly = vk_structure<VkPipelineInputAssemblyStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(target_size),
                        static_cast<float>(target_size), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {target_size, target_size}};
    auto viewport_state = vk_structure<VkPipelineViewportStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    auto raster = vk_structure<VkPipelineRasterizationStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    auto multisample = vk_structure<VkPipelineMultisampleStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // The second colour output's alpha is the blend weight, exactly as the
    // OpenGL path uses it. source_opacity is 1.0 here, so the destination
    // contributes nothing and the result is the palette colour unmodified.
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC1_ALPHA;
    blend_attachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    auto blend = vk_structure<VkPipelineColorBlendStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    auto pipeline_info = vk_structure<VkGraphicsPipelineCreateInfo>(
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    VkPipeline pipeline{};
    const bool built = vkCreateGraphicsPipelines(
        vk.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        &pipeline) == VK_SUCCESS;
    check(built, "System 22 polygon pipeline could not be created");

    if (built) {
        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = uniform_buffer.buffer;
        buffer_info.range = sizeof(uniforms);
        const std::array<VkImageView, 5> views{
            tiles.view, map.view, attr.view, palette.view, sprites.view};
        std::array<VkDescriptorImageInfo, 5> image_info{};
        std::array<VkWriteDescriptorSet, 6> writes{};
        writes[0] = vk_structure<VkWriteDescriptorSet>(
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        writes[0].dstSet = descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        for (std::size_t index = 0; index < views.size(); ++index) {
            image_info[index].sampler = sampler;
            image_info[index].imageView = views[index];
            image_info[index].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[index + 1] = vk_structure<VkWriteDescriptorSet>(
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[index + 1].dstSet = descriptor_set;
            writes[index + 1].dstBinding = static_cast<uint32_t>(index) + 1;
            writes[index + 1].descriptorCount = 1;
            writes[index + 1].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index + 1].pImageInfo = &image_info[index];
        }
        vkUpdateDescriptorSets(vk.device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        auto command_info = vk_structure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        command_info.commandPool = vk.command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        VkCommandBuffer commands{};
        vkAllocateCommandBuffers(vk.device, &command_info, &commands);
        auto begin = vk_structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);

        const std::array<test_image*, 5> sheets{&tiles, &map, &attr, &palette,
                                                &sprites};
        const std::array<int, 5> layers{1, 1, 1, 3, 1};
        const std::array<VkExtent3D, 5> extents{{
            {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {256, 1, 1}, {1, 1, 1}}};
        for (std::size_t index = 0; index < sheets.size(); ++index) {
            barrier(commands, sheets[index]->image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, layers[index]);
            VkBufferImageCopy copy{};
            copy.bufferOffset = staging[index].offset;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount =
                static_cast<uint32_t>(layers[index]);
            copy.imageExtent = extents[index];
            vkCmdCopyBufferToImage(commands, upload.buffer,
                                   sheets[index]->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
            barrier(commands, sheets[index]->image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, layers[index]);
        }

        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        auto pass_begin = vk_structure<VkRenderPassBeginInfo>(
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass_begin.renderPass = render_pass;
        pass_begin.framebuffer = framebuffer;
        pass_begin.renderArea.extent = {target_size, target_size};
        pass_begin.clearValueCount = 1;
        pass_begin.pClearValues = &clear;
        vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout, 0, 1, &descriptor_set, 0,
                                nullptr);
        const VkDeviceSize vertex_offset = 0;
        vkCmdBindVertexBuffers(commands, 0, 1, &vertex_buffer.buffer,
                               &vertex_offset);
        vkCmdDraw(commands, 4, 1, 0, 0);
        vkCmdEndRenderPass(commands);

        VkBufferImageCopy download{};
        download.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        download.imageSubresource.layerCount = 1;
        download.imageExtent = {target_size, target_size, 1};
        vkCmdCopyImageToBuffer(commands, target.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer, 1, &download);
        vkEndCommandBuffer(commands);
        auto submit = vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk.queue);
        vkFreeCommandBuffers(vk.device, vk.command_pool, 1, &commands);

        output.resize(static_cast<std::size_t>(target_size) * target_size);
        std::memcpy(output.data(), readback.mapped,
                    output.size() * sizeof(rgba));
    }

    if (pipeline) vkDestroyPipeline(vk.device, pipeline, nullptr);
    vkDestroyShaderModule(vk.device, vertex_module, nullptr);
    vkDestroyShaderModule(vk.device, fragment_module, nullptr);
    vkDestroyDescriptorPool(vk.device, descriptor_pool, nullptr);
    vkDestroyPipelineLayout(vk.device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(vk.device, set_layout, nullptr);
    vkDestroySampler(vk.device, sampler, nullptr);
    vkDestroyFramebuffer(vk.device, framebuffer, nullptr);
    vkDestroyRenderPass(vk.device, render_pass, nullptr);
    destroy_host_buffer(vk, readback);
    destroy_host_buffer(vk, vertex_buffer);
    destroy_host_buffer(vk, uniform_buffer);
    destroy_host_buffer(vk, upload);
    destroy_image(vk, target);
    destroy_image(vk, sprites);
    destroy_image(vk, palette);
    destroy_image(vk, attr);
    destroy_image(vk, map);
    destroy_image(vk, tiles);
    return output;
}

} // namespace

int main() {
    vulkan_harness vk;
    if (!vk.start()) {
        std::printf("No Vulkan device available; skipping.\n");
        return 0;
    }

    // flip_y is what every board using the RGBA path passes: row zero of the
    // source is the top of the picture, and it must land at the top of the
    // output. The 4-row source doubles into an 8-row target, so output rows
    // 0-1 come from source row 0 and rows 6-7 from source row 3.
    const std::vector<rgba> upright = render(vk, true, false);
    if (upright.size() ==
        static_cast<std::size_t>(target_size) * target_size) {
        check(pixel_at(upright, 0, 0) == row_colors[0],
              "flip_y: source row 0 must appear at the top of the output");
        check(pixel_at(upright, 0, target_size - 1) == row_colors[3],
              "flip_y: source row 3 must appear at the bottom of the output");
        check(pixel_at(upright, 0, 2) == row_colors[1],
              "flip_y: source row 1 must appear in the second band");
        check(pixel_at(upright, target_size - 1, 0) == row_colors[0],
              "flip_y: a row is a constant colour across the output width");
    } else {
        check(false, "post pipeline produced no image for flip_y");
    }

    // Without flip_y the image is deliberately inverted - that is the
    // convention OpenGL readback arrives in - so the same source must come out
    // the other way up. Asserting both directions pins the orientation rather
    // than accidentally passing on a double flip.
    const std::vector<rgba> inverted = render(vk, false, false);
    if (inverted.size() ==
        static_cast<std::size_t>(target_size) * target_size) {
        check(pixel_at(inverted, 0, 0) == row_colors[3],
              "no flip_y: source row 3 must appear at the top of the output");
        check(pixel_at(inverted, 0, target_size - 1) == row_colors[0],
              "no flip_y: source row 0 must appear at the bottom");
    } else {
        check(false, "post pipeline produced no image without flip_y");
    }

    // Sharp bilinear. The source doubles exactly into the target, and at an
    // integer scale the sharp ramp has to collapse to nearest: every output
    // row lands on one source row, with no blended row anywhere. Plain
    // bilinear would mix rows 0 and 1 into output row 1 - which is precisely
    // the softness this replaced.
    const std::vector<rgba> sharp = render(vk, true, false, true);
    if (sharp.size() == static_cast<std::size_t>(target_size) * target_size) {
        check(sharp == upright,
              "sharp bilinear at an exact 2x scale must equal nearest");
        for (int y = 0; y < target_size; ++y) {
            const rgba pixel = pixel_at(sharp, target_size / 2, y);
            const rgba expected =
                row_colors[static_cast<std::size_t>(y / 2)];
            check(pixel == expected,
                  "sharp bilinear must leave texel interiors unblended");
        }
    } else {
        check(false, "post pipeline produced no image for sharp filtering");
    }

    // The overlay quad carries its own orientation: an overlay bitmap's row
    // zero is its top edge, and it must be drawn that way up.
    const std::vector<rgba> overlay = render(vk, false, true);
    if (overlay.size() ==
        static_cast<std::size_t>(target_size) * target_size) {
        check(pixel_at(overlay, 0, 0) == row_colors[0],
              "overlay: bitmap row 0 must be drawn at the top");
        check(pixel_at(overlay, 0, target_size - 1) == row_colors[3],
              "overlay: bitmap row 3 must be drawn at the bottom");
    } else {
        check(false, "overlay pipeline produced no image");
    }

    // The System 22 polygon shaders, driven so that exactly one palette entry
    // can be the answer. Getting the tile address, the uniform block offsets
    // or the Y direction wrong all produce a different pixel.
    bool dual_source_ok = false;
    const std::vector<rgba> polygons = render_system22(vk, dual_source_ok);
    if (!dual_source_ok) {
        std::printf("Device lacks dualSrcBlend; skipping System 22 checks.\n");
    } else if (polygons.size() ==
               static_cast<std::size_t>(target_size) * target_size) {
        // The quad covers board Y 0..240, which is the top half of the screen.
        check(pixel_at(polygons, 0, 0).r == polygon_color.r &&
                  pixel_at(polygons, 0, 0).g == polygon_color.g &&
                  pixel_at(polygons, 0, 0).b == polygon_color.b,
              "System 22: tile decode must select the expected palette entry");
        check(pixel_at(polygons, target_size - 1, 0).r == polygon_color.r,
              "System 22: the polygon must span the full board width");
        check(pixel_at(polygons, 0, target_size / 2 - 1).r == polygon_color.r,
              "System 22: board +Y must reach the top half of the output");
        const rgba below = pixel_at(polygons, 0, target_size - 1);
        check(below.r == 0 && below.g == 0 && below.b == 0,
              "System 22: the bottom half must be left at the clear colour");
    } else {
        check(false, "System 22 polygon pipeline produced no image");
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("Vulkan post, overlay and System 22 pipelines verified.\n");
    return 0;
}
