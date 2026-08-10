#include "VRVulkanOverlay.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "ManifestManager.hpp"
#include "Util.hpp"

static VkInstance s_vkInstance = VK_NULL_HANDLE;
static VkPhysicalDevice s_vkPhysicalDevice = VK_NULL_HANDLE;
static VkDevice s_vkDevice = VK_NULL_HANDLE;
static VkQueue s_vkQueue = VK_NULL_HANDLE;
static uint32_t s_queueFamilyIndex = 0;
static VkCommandPool s_vkCommandPool = VK_NULL_HANDLE;
static std::mutex s_vulkanDeviceMutex;

VRVulkanOverlay::VRVulkanOverlay(InputManager& input) : inputManager(input) {}

VRVulkanOverlay::~VRVulkanOverlay() {
    {
        std::lock_guard<std::mutex> lock(s_vulkanDeviceMutex);
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
    }
    clearDmaBufCache();
    {
        std::lock_guard<std::mutex> lock(s_vulkanDeviceMutex);
        if (cursorVkImage) {
            vkDestroyImage(vkDevice, cursorVkImage, nullptr);
            cursorVkImage = VK_NULL_HANDLE;
        }
        if (cursorVkMemory) {
            vkFreeMemory(vkDevice, cursorVkMemory, nullptr);
            cursorVkMemory = VK_NULL_HANDLE;
        }
    }
    destroyFallbackResources();
    if (vrOverlay) {
        if (cursorOverlayHandle != vr::k_ulOverlayHandleInvalid) {
            vrOverlay->DestroyOverlay(cursorOverlayHandle);
            cursorOverlayHandle = vr::k_ulOverlayHandleInvalid;
        }
        if (thumbnailHandle != vr::k_ulOverlayHandleInvalid) {
            vrOverlay->DestroyOverlay(thumbnailHandle);
            thumbnailHandle = vr::k_ulOverlayHandleInvalid;
        }
        if (overlayHandle != vr::k_ulOverlayHandleInvalid) {
            vrOverlay->DestroyOverlay(overlayHandle);
            overlayHandle = vr::k_ulOverlayHandleInvalid;
        }
    }
}

void VRVulkanOverlay::cleanupVulkan() {
    std::lock_guard<std::mutex> lock(s_vulkanDeviceMutex);
    if (s_vkCommandPool != VK_NULL_HANDLE && s_vkDevice != VK_NULL_HANDLE) {
        vkDestroyCommandPool(s_vkDevice, s_vkCommandPool, nullptr);
        s_vkCommandPool = VK_NULL_HANDLE;
    }
    if (s_vkDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s_vkDevice);
        vkDestroyDevice(s_vkDevice, nullptr);
        s_vkDevice = VK_NULL_HANDLE;
    }
    if (s_vkInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(s_vkInstance, nullptr);
        s_vkInstance = VK_NULL_HANDLE;
    }
}

bool VRVulkanOverlay::initVR(const std::string& key, const std::string& name) {
    vrOverlay = vr::VROverlay();
    if (!vrOverlay) {
        LOG_ERROR << "Failed to get VROverlay interface. Is OpenVR initialized?" << std::endl;
        return false;
    }

    if (textureWidth == 0 || textureHeight == 0) {
        // We have to put a placeholder size
        textureWidth = 1;
        textureHeight = 1;
    }

    std::string cursorKey = key + ".cursor";
    std::string cursorName = name + " Invisible Cursor";

    vrOverlay->CreateDashboardOverlay(key.c_str(), name.c_str(), &overlayHandle, &thumbnailHandle);
    if (thumbnailHandle != vr::k_ulOverlayHandleInvalid) {
        std::filesystem::path iconPath = ManifestManager::getIconPath();
        if (std::filesystem::exists(iconPath)) {
            vr::EVROverlayError iconErr = vrOverlay->SetOverlayFromFile(thumbnailHandle, iconPath.string().c_str());
            if (iconErr != vr::VROverlayError_None) {
                LOG_ERROR << "Failed to set dashboard overlay thumbnail image: " << vrOverlay->GetOverlayErrorNameFromEnum(iconErr) << std::endl;
            } else {
                LOG << "Successfully set dashboard overlay thumbnail icon: " << iconPath << std::endl;
            }
        } else {
            LOG_ERROR << "Dashboard overlay thumbnail icon not found at: " << iconPath << std::endl;
        }
    }
    updateOverlaySize();
    vrOverlay->SetOverlayInputMethod(overlayHandle, vr::VROverlayInputMethod_Mouse);

    vrOverlay->SetOverlayFlag(overlayHandle, vr::VROverlayFlags_EnableControlBarKeyboard, true);
    vrOverlay->SetOverlayFlag(overlayHandle, vr::VROverlayFlags_EnableClickStabilization, true);
    vrOverlay->SetOverlayFlag(overlayHandle, vr::VROverlayFlags_EnableControlBar, true);
    vrOverlay->SetOverlayFlag(overlayHandle, vr::VROverlayFlags_SendVRSmoothScrollEvents, true);

    vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
    vrOverlay->SetOverlayTextureBounds(overlayHandle, &bounds);

    // Set a transparent cursor overlay so SteamVR suppresses its default laser "orb"
    vrOverlay->CreateOverlay(cursorKey.c_str(), cursorName.c_str(), &cursorOverlayHandle);

    if (cursorOverlayHandle != vr::k_ulOverlayHandleInvalid) {
        vrOverlay->SetOverlayWidthInMeters(cursorOverlayHandle, 0.001f);
        vr::HmdVector2_t hotspot = {0.0f, 0.0f};
        vrOverlay->SetOverlayTransformCursor(cursorOverlayHandle, &hotspot);
        vrOverlay->SetOverlayCursor(overlayHandle, cursorOverlayHandle);
    }

    return true;
}

void VRVulkanOverlay::updateOverlaySize() {
    if (vrOverlay && overlayHandle != vr::k_ulOverlayHandleInvalid) {
        float aspectRatio = 16.0f / 9.0f;
        if (textureWidth > 0 && textureHeight > 0) {
            aspectRatio = static_cast<float>(textureWidth) / static_cast<float>(textureHeight);
        }
        overlayWidthMeters = overlayHeightMeters * aspectRatio;
        vrOverlay->SetOverlayWidthInMeters(overlayHandle, overlayWidthMeters);

        vr::HmdVector2_t mouseScale = {static_cast<float>(textureWidth), static_cast<float>(textureHeight)};
        vrOverlay->SetOverlayMouseScale(overlayHandle, &mouseScale);
    }
}

void VRVulkanOverlay::setOverlayHeight(float heightInMeters) {
    overlayHeightMeters = heightInMeters;
    updateOverlaySize();
}

void VRVulkanOverlay::setOverlayName(const std::string& name) {
    if (vrOverlay && overlayHandle != vr::k_ulOverlayHandleInvalid) {
        vrOverlay->SetOverlayName(overlayHandle, name.c_str());
    }
}

std::vector<std::string> VRVulkanOverlay::getVulkanInstanceExtensions() {
    std::vector<std::string> instExts;
    if (vr::VRCompositor()) {
        uint32_t instExtSize = vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(nullptr, 0);
        if (instExtSize > 0) {
            std::vector<char> instExtBuffer(instExtSize + 1, 0);
            vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(instExtBuffer.data(), instExtSize);
            std::stringstream ss(instExtBuffer.data());
            std::string token;
            while (ss >> token) {
                instExts.push_back(token);
            }
        }
    }
    instExts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    instExts.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
    instExts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME);
    return instExts;
}

std::vector<std::string> VRVulkanOverlay::getVulkanDeviceExtensions() {
    std::vector<std::string> devExts;
    if (vr::VRCompositor()) {
        uint32_t devExtSize = vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(reinterpret_cast<VkPhysicalDevice_T*>(vkPhysicalDevice), nullptr, 0);
        if (devExtSize > 0) {
            std::vector<char> devExtBuffer(devExtSize + 1, 0);
            vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(reinterpret_cast<VkPhysicalDevice_T*>(vkPhysicalDevice), devExtBuffer.data(), devExtSize);
            std::stringstream ss(devExtBuffer.data());
            std::string token;
            while (ss >> token) {
                devExts.push_back(token);
            }
        }
    }
    devExts.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    devExts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    devExts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
    devExts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    devExts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    devExts.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    devExts.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
    devExts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    devExts.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    devExts.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    devExts.push_back(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
    return devExts;
}

bool VRVulkanOverlay::createVulkanInstance(const std::vector<std::string>& instExts) {
    std::vector<const char*> instanceExtensions;
    for (const auto& ext : instExts) {
        instanceExtensions.push_back(ext.c_str());
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Wayland VR Overlay";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    createInfo.ppEnabledExtensionNames = instanceExtensions.data();

    if (vkCreateInstance(&createInfo, nullptr, &vkInstance) != VK_SUCCESS) {
        LOG_ERROR << "Failed to create instance" << std::endl;
        return false;
    }

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(vkInstance, &devCount, nullptr);
    if (devCount == 0) return false;

    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(vkInstance, &devCount, devs.data());

    vkPhysicalDevice = VK_NULL_HANDLE;
    if (vr::VRSystem()) {
        uint64_t outputDevice = 0;
        vr::VRSystem()->GetOutputDevice(&outputDevice, vr::TextureType_Vulkan, reinterpret_cast<VkInstance_T*>(vkInstance));
        if (outputDevice != 0) {
            vkPhysicalDevice = reinterpret_cast<VkPhysicalDevice>(outputDevice);
        }
    }

    if (vkPhysicalDevice == VK_NULL_HANDLE) {
        vkPhysicalDevice = devs[0];
    }
    return true;
}

bool VRVulkanOverlay::createVulkanDevice(const std::vector<std::string>& devExts) {
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(vkPhysicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(vkPhysicalDevice, nullptr, &extCount, availableExts.data());

    std::vector<const char*> deviceExtensions;
    for (const auto& reqExt : devExts) {
        bool found = false;
        for (const auto& avail : availableExts) {
            if (reqExt == avail.extensionName) {
                found = true;
                break;
            }
        }
        if (found) {
            deviceExtensions.push_back(reqExt.c_str());
        }
    }

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vkPhysicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(vkPhysicalDevice, &queueFamilyCount, queueFamilies.data());

    queueFamilyIndex = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamilyIndex = i;
            break;
        }
    }

    if (queueFamilyIndex == UINT32_MAX) {
        LOG_ERROR << "Failed to find graphics queue family" << std::endl;
        return false;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan12Features supportedV12{};
    supportedV12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 supportedFeatures2{};
    supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supportedFeatures2.pNext = &supportedV12;
    vkGetPhysicalDeviceFeatures2(vkPhysicalDevice, &supportedFeatures2);

    VkPhysicalDeviceVulkan12Features enabledV12{};
    enabledV12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    if (supportedV12.timelineSemaphore) enabledV12.timelineSemaphore = VK_TRUE;
    if (supportedV12.shaderOutputViewportIndex) enabledV12.shaderOutputViewportIndex = VK_TRUE;
    if (supportedV12.shaderOutputLayer) enabledV12.shaderOutputLayer = VK_TRUE;

    VkPhysicalDeviceFeatures2 enabledFeatures2{};
    enabledFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabledFeatures2.features = supportedFeatures2.features;
    enabledFeatures2.pNext = &enabledV12;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &enabledFeatures2;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(vkPhysicalDevice, &deviceCreateInfo, nullptr, &vkDevice) != VK_SUCCESS) {
        LOG_ERROR << "Failed to create logical device" << std::endl;
        return false;
    }

    vkGetDeviceQueue(vkDevice, queueFamilyIndex, 0, &vkQueue);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &s_vkCommandPool) != VK_SUCCESS) {
        LOG_ERROR << "Failed to create Vulkan command pool" << std::endl;
        return false;
    }

    return true;
}

bool VRVulkanOverlay::initVulkan() {
    if (s_vkDevice != VK_NULL_HANDLE) {
        vkInstance = s_vkInstance;
        vkPhysicalDevice = s_vkPhysicalDevice;
        vkDevice = s_vkDevice;
        vkQueue = s_vkQueue;
        queueFamilyIndex = s_queueFamilyIndex;
        createTransparentCursorTexture();
        return true;
    }

    auto instExts = getVulkanInstanceExtensions();
    if (!createVulkanInstance(instExts)) return false;

    auto devExts = getVulkanDeviceExtensions();
    if (!createVulkanDevice(devExts)) return false;

    s_vkInstance = vkInstance;
    s_vkPhysicalDevice = vkPhysicalDevice;
    s_vkDevice = vkDevice;
    s_vkQueue = vkQueue;
    s_queueFamilyIndex = queueFamilyIndex;

    createTransparentCursorTexture();
    return true;
}

std::vector<DrmFormatModifier> VRVulkanOverlay::getSupportedDrmModifiers() {
    std::vector<DrmFormatModifier> result;
    if (!vkInstance || !vkPhysicalDevice) return result;

    auto pfnGetFormatProps2 = (PFN_vkGetPhysicalDeviceFormatProperties2)vkGetInstanceProcAddr(vkInstance, "vkGetPhysicalDeviceFormatProperties2");
    if (!pfnGetFormatProps2) {
        pfnGetFormatProps2 = (PFN_vkGetPhysicalDeviceFormatProperties2)vkGetInstanceProcAddr(vkInstance, "vkGetPhysicalDeviceFormatProperties2KHR");
    }
    if (!pfnGetFormatProps2) {
        LOG << "vkGetPhysicalDeviceFormatProperties2 not available." << std::endl;
        return result;
    }

    struct FormatPair {
        VkFormat vkFmt;
        spa_video_format spaFmt;
    };
    FormatPair pairs[] = {{VK_FORMAT_B8G8R8A8_UNORM, SPA_VIDEO_FORMAT_BGRA}, {VK_FORMAT_R8G8B8A8_UNORM, SPA_VIDEO_FORMAT_RGBA}};

    for (const auto& pair : pairs) {
        VkDrmFormatModifierPropertiesListEXT modifierList{};
        modifierList.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        VkFormatProperties2 formatProps2{};
        formatProps2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        formatProps2.pNext = &modifierList;

        pfnGetFormatProps2(vkPhysicalDevice, pair.vkFmt, &formatProps2);

        if (modifierList.drmFormatModifierCount > 0) {
            std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(modifierList.drmFormatModifierCount);
            modifierList.pDrmFormatModifierProperties = modifierProps.data();
            pfnGetFormatProps2(vkPhysicalDevice, pair.vkFmt, &formatProps2);

            for (const auto& prop : modifierProps) {
                if (prop.drmFormatModifierPlaneCount == 1) {
                    result.push_back({pair.spaFmt, prop.drmFormatModifier});
                }
            }
        }

        // Implicit modifier fallback
        result.push_back({pair.spaFmt, 0x00ffffffffffffffULL});
    }

    LOG << "Queried " << result.size() << " DRM format modifier(s) from physical device." << std::endl;
    return result;
}

VkFormat VRVulkanOverlay::spaFormatToVkFormat(uint32_t spaFormat) {
    switch (spaFormat) {
        case SPA_VIDEO_FORMAT_BGRA:
        case SPA_VIDEO_FORMAT_BGRx:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case SPA_VIDEO_FORMAT_RGBA:
        case SPA_VIDEO_FORMAT_RGBx:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

void VRVulkanOverlay::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    if (image == VK_NULL_HANDLE || vkDevice == VK_NULL_HANDLE || vkQueue == VK_NULL_HANDLE || s_vkCommandPool == VK_NULL_HANDLE) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_vulkanDeviceMutex);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = s_vkCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd) != VK_SUCCESS) {
        LOG_ERROR << "Failed to allocate command buffer for image layout transition" << std::endl;
        return;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(vkQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(vkQueue);

    vkFreeCommandBuffers(vkDevice, s_vkCommandPool, 1, &cmd);
}

void VRVulkanOverlay::createTransparentCursorTexture() {
    uint32_t width = 4;
    uint32_t height = 4;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &cursorVkImage) != VK_SUCCESS) {
        LOG_ERROR << "Failed to create transparent cursor image" << std::endl;
        return;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vkDevice, cursorVkImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.size > 0 ? memReqs.memoryTypeBits : 0x1, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &cursorVkMemory) != VK_SUCCESS) {
        LOG_ERROR << "Failed to allocate memory for transparent cursor image" << std::endl;
        return;
    }

    vkBindImageMemory(vkDevice, cursorVkImage, cursorVkMemory, 0);

    uint8_t* cursorData = nullptr;
    if (vkMapMemory(vkDevice, cursorVkMemory, 0, memReqs.size, 0, (void**)&cursorData) == VK_SUCCESS) {
        std::memset(cursorData, 0, memReqs.size);
        vkUnmapMemory(vkDevice, cursorVkMemory);
    }

    transitionImageLayout(cursorVkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (cursorOverlayHandle != vr::k_ulOverlayHandleInvalid) {
        vr::VRVulkanTextureData_t vulkanCursorData = buildVulkanTextureStruct(cursorVkImage, width, height);
        vr::Texture_t cursorTexture{(void*)&vulkanCursorData, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
        vrOverlay->SetOverlayTexture(cursorOverlayHandle, &cursorTexture);
    }
}

vr::VRVulkanTextureData_t VRVulkanOverlay::buildVulkanTextureStruct(VkImage image, uint32_t width, uint32_t height) {
    vr::VRVulkanTextureData_t vulkanData{};
    vulkanData.m_nImage = (uint64_t)image;
    vulkanData.m_pDevice = (VkDevice_T*)vkDevice;
    vulkanData.m_pPhysicalDevice = (VkPhysicalDevice_T*)vkPhysicalDevice;
    vulkanData.m_pInstance = (VkInstance_T*)vkInstance;
    vulkanData.m_pQueue = (VkQueue_T*)vkQueue;
    vulkanData.m_nQueueFamilyIndex = queueFamilyIndex;
    vulkanData.m_nWidth = width;
    vulkanData.m_nHeight = height;
    vulkanData.m_nFormat = VK_FORMAT_B8G8R8A8_UNORM;
    for (const auto& pair : dmaBufCache) {
        if (pair.second.image == image && pair.second.format != VK_FORMAT_UNDEFINED) {
            vulkanData.m_nFormat = pair.second.format;
            break;
        }
    }
    vulkanData.m_nSampleCount = 1;
    return vulkanData;
}

void VRVulkanOverlay::destroyFallbackResources() {
    std::lock_guard<std::mutex> lock(s_vulkanDeviceMutex);
    if (vkDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vkDevice);
    }
    if (mappedData && fallbackMemory) {
        vkUnmapMemory(vkDevice, fallbackMemory);
        mappedData = nullptr;
    }
    if (currentFallbackImage) {
        vkDestroyImage(vkDevice, currentFallbackImage, nullptr);
        currentFallbackImage = VK_NULL_HANDLE;
    }
    if (fallbackMemory) {
        vkFreeMemory(vkDevice, fallbackMemory, nullptr);
        fallbackMemory = VK_NULL_HANDLE;
    }
}

uint32_t VRVulkanOverlay::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if (typeFilter & (1 << i)) {
            return i;
        }
    }
    return 0;
}

VkImage VRVulkanOverlay::createFallbackTexture(uint32_t width, uint32_t height) {
    destroyFallbackResources();

    textureWidth = width > 0 ? width : 1;
    textureHeight = height > 0 ? height : 1;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.extent = {textureWidth, textureHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        LOG_ERROR << "Failed to create fallback image" << std::endl;
        return VK_NULL_HANDLE;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vkDevice, image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.size > 0 ? memReqs.memoryTypeBits : 0x1, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &fallbackMemory) != VK_SUCCESS) {
        LOG_ERROR << "Failed to allocate memory for fallback image" << std::endl;
        return VK_NULL_HANDLE;
    }

    vkBindImageMemory(vkDevice, image, fallbackMemory, 0);

    VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(vkDevice, image, &subresource, &layout);
    fallbackRowPitch = layout.rowPitch > 0 ? layout.rowPitch : (textureWidth * 4);
    fallbackOffset = layout.offset;

    vkMapMemory(vkDevice, fallbackMemory, 0, memReqs.size, 0, (void**)&mappedData);
    if (mappedData) {
        std::memset(mappedData, 0, memReqs.size);
    }

    currentFallbackImage = image;
    transitionImageLayout(currentFallbackImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    updateOverlaySize();
    return currentFallbackImage;
}

void VRVulkanOverlay::clearDmaBufCache() {
    std::lock_guard<std::mutex> lock(s_vulkanDeviceMutex);
    if (vkDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vkDevice);
    }
    if (vrOverlay && overlayHandle != vr::k_ulOverlayHandleInvalid) {
        vrOverlay->ClearOverlayTexture(overlayHandle);
    }
    for (auto& pair : dmaBufCache) {
        if (pair.second.image) {
            vkDestroyImage(vkDevice, pair.second.image, nullptr);
            pair.second.image = VK_NULL_HANDLE;
        }
        if (pair.second.memory) {
            vkFreeMemory(vkDevice, pair.second.memory, nullptr);
            pair.second.memory = VK_NULL_HANDLE;
        }
    }
    dmaBufCache.clear();
}

VkImage VRVulkanOverlay::importDmaBufFrame(const void* bufferId, int fd, uint32_t width, uint32_t height, uint32_t stride, uint32_t spaFormat, uint64_t drmModifier) {
    if (fd < 0 || width == 0 || height == 0) return VK_NULL_HANDLE;

    const void* cacheKey = bufferId ? bufferId : reinterpret_cast<const void*>(static_cast<uintptr_t>(fd));
    VkFormat vkFmt = spaFormatToVkFormat(spaFormat);

    auto it = dmaBufCache.find(cacheKey);
    if (it != dmaBufCache.end()) {
        if (it->second.width == width && it->second.height == height && it->second.modifier == drmModifier && it->second.format == vkFmt) {
            return it->second.image;
        } else {
            std::lock_guard<std::mutex> lock(s_vulkanDeviceMutex);
            if (vkDevice != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(vkDevice);
            }
            if (it->second.image) {
                vkDestroyImage(vkDevice, it->second.image, nullptr);
            }
            if (it->second.memory) {
                vkFreeMemory(vkDevice, it->second.memory, nullptr);
            }
            dmaBufCache.erase(it);
        }
    }

    int dupFd = dup(fd);
    if (dupFd < 0) return VK_NULL_HANDLE;

    VkExternalMemoryImageCreateInfo extImageInfo{};
    extImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkSubresourceLayout planeLayout{};
    planeLayout.offset = 0;
    planeLayout.size = 0;
    planeLayout.rowPitch = (stride > 0) ? stride : (width * 4);
    planeLayout.arrayPitch = 0;
    planeLayout.depthPitch = 0;

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{};
    modifierInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifierInfo.drmFormatModifier = drmModifier;
    modifierInfo.drmFormatModifierPlaneCount = 1;
    modifierInfo.pPlaneLayouts = &planeLayout;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = vkFmt;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool hasModifier = (drmModifier != 0x00ffffffffffffffULL);
    if (hasModifier) {
        imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        modifierInfo.pNext = &extImageInfo;
        imageInfo.pNext = &modifierInfo;
    } else {
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.pNext = &extImageInfo;
    }

    VkImage image = VK_NULL_HANDLE;
    VkResult res = vkCreateImage(vkDevice, &imageInfo, nullptr, &image);
    if (res != VK_SUCCESS && hasModifier) {
        LOG_ERROR << "Failed to create image with modifier 0x" << std::hex << drmModifier << std::dec << ", falling back to OPTIMAL tiling" << std::endl;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.pNext = &extImageInfo;
        res = vkCreateImage(vkDevice, &imageInfo, nullptr, &image);
    }

    if (res != VK_SUCCESS) {
        LOG_ERROR << "Failed to create DMA-BUF image in Vulkan, err=" << res << std::endl;
        close(dupFd);
        return VK_NULL_HANDLE;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vkDevice, image, &memReqs);

    VkImportMemoryFdInfoKHR importFdInfo{};
    importFdInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importFdInfo.fd = dupFd;

    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.pNext = &importFdInfo;
    dedicatedInfo.image = image;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &dedicatedInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        LOG_ERROR << "Failed to allocate memory for DMA-BUF image" << std::endl;
        vkDestroyImage(vkDevice, image, nullptr);
        close(dupFd);
        return VK_NULL_HANDLE;
    }

    if (vkBindImageMemory(vkDevice, image, memory, 0) != VK_SUCCESS) {
        LOG_ERROR << "Failed to bind memory for DMA-BUF image" << std::endl;
        vkFreeMemory(vkDevice, memory, nullptr);
        vkDestroyImage(vkDevice, image, nullptr);
        return VK_NULL_HANDLE;
    }

    // Transition image from UNDEFINED to SHADER_READ_ONLY_OPTIMAL for OpenVR compositor
    transitionImageLayout(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    DmaBufTexture tex;
    tex.image = image;
    tex.memory = memory;
    tex.width = width;
    tex.height = height;
    tex.stride = stride;
    tex.modifier = drmModifier;
    tex.format = vkFmt;
    dmaBufCache[cacheKey] = tex;

    LOG << "Successfully imported zero-copy DMA-BUF (FD " << fd << ", key=" << cacheKey << ", " << width << "x" << height << ", mod=0x" << std::hex << drmModifier << std::dec << ")" << std::endl;
    return image;
}

void VRVulkanOverlay::updateFrameData(const uint8_t* srcData, uint32_t frameWidth, uint32_t frameHeight, uint32_t stride) {
    if (!srcData || frameWidth == 0 || frameHeight == 0) return;
    if (!isOverlayVisible()) return;

    if (frameWidth != textureWidth || frameHeight != textureHeight || !mappedData) {
        LOG << "Resizing overlay texture from " << textureWidth << "x" << textureHeight << " to " << frameWidth << "x" << frameHeight << std::endl;
        createFallbackTexture(frameWidth, frameHeight);
    }

    if (!mappedData) return;

    uint8_t* dstBase = mappedData + fallbackOffset;
    uint32_t rowBytes = std::min(frameWidth * 4, stride);

    for (uint32_t y = 0; y < frameHeight; ++y) {
        std::memcpy(dstBase + (y * fallbackRowPitch), srcData + (y * stride), rowBytes);
    }

    if (vkDevice != VK_NULL_HANDLE && fallbackMemory != VK_NULL_HANDLE) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = fallbackMemory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(vkDevice, 1, &range);
    }
}

void VRVulkanOverlay::processEvents() {
    if (!vrOverlay) return;

    bool currentVisible = isOverlayVisible();
    if (currentVisible && !lastOverlayVisible) {
        overlayMouseActive = true;
    } else if (!currentVisible && lastOverlayVisible) {
        if (overlayHandle != vr::k_ulOverlayHandleInvalid) {
            vrOverlay->ClearOverlayTexture(overlayHandle);
        }
    }
    lastOverlayVisible = currentVisible;

    vr::VREvent_t event;
    while (vrOverlay->PollNextOverlayEvent(overlayHandle, &event, sizeof(event))) {
        switch (event.eventType) {
            case vr::VREvent_OverlayShown:
            case vr::VREvent_DashboardActivated:
                overlayMouseActive = true;
                break;

            case vr::VREvent_OverlayHidden:
            case vr::VREvent_DashboardDeactivated:
                if (overlayHandle != vr::k_ulOverlayHandleInvalid) {
                    vrOverlay->ClearOverlayTexture(overlayHandle);
                }
                break;

            case vr::VREvent_MouseMove: {
                if (!overlayMouseActive) break;

                float rawX = event.data.mouse.x;
                float rawY = event.data.mouse.y;

                uint32_t texW = textureWidth;
                uint32_t texH = textureHeight;

                float u = std::clamp(rawX / static_cast<float>(texW), 0.0f, 1.0f);
                float v = 1.0f - std::clamp(rawY / static_cast<float>(texH), 0.0f, 1.0f);

                inputManager.moveMouse(u, v);
                break;
            }

            case vr::VREvent_MouseButtonDown: {
                if (!overlayMouseActive) {
                    overlayMouseActive = true;
                    takingOverClick = true;

                    float rawX = event.data.mouse.x;
                    float rawY = event.data.mouse.y;
                    uint32_t texW = textureWidth;
                    uint32_t texH = textureHeight;
                    float u = std::clamp(rawX / static_cast<float>(texW), 0.0f, 1.0f);
                    float v = 1.0f - std::clamp(rawY / static_cast<float>(texH), 0.0f, 1.0f);
                    inputManager.moveMouse(u, v);

                    LOG << "Virtual mouse takeover activated by click." << std::endl;
                    break;
                }

                if (event.data.mouse.button & vr::VRMouseButton_Left) inputManager.sendClick(0, true);
                if (event.data.mouse.button & vr::VRMouseButton_Right) inputManager.sendClick(1, true);
                break;
            }

            case vr::VREvent_MouseButtonUp: {
                if (takingOverClick) {
                    takingOverClick = false;
                    break;
                }

                if (event.data.mouse.button & vr::VRMouseButton_Left) inputManager.sendClick(0, false);
                if (event.data.mouse.button & vr::VRMouseButton_Right) inputManager.sendClick(1, false);
                break;
            }

            case vr::VREvent_KeyboardCharInput: {
                if (!overlayMouseActive) break;
                std::string inputStr = event.data.keyboard.cNewInput;
                if (!inputStr.empty()) {
                    inputManager.typeText(inputStr);
                }
                break;
            }

            case vr::VREvent_ScrollSmooth:
            case vr::VREvent_ScrollDiscrete: {
                if (!overlayMouseActive) break;
                float x = event.data.scroll.xdelta;
                float y = event.data.scroll.ydelta;
                inputManager.sendScroll(x, y);
                break;
            }
        }
    }
}

bool VRVulkanOverlay::isOverlayVisible() const {
    if (!vrOverlay || overlayHandle == vr::k_ulOverlayHandleInvalid) return false;
    return vrOverlay->IsOverlayVisible(overlayHandle);
}

void VRVulkanOverlay::submitFrame(VkImage image, uint32_t width, uint32_t height) {
    if (!vrOverlay || overlayHandle == vr::k_ulOverlayHandleInvalid) return;

    if (!isOverlayVisible()) return;

    if (width > 0 && height > 0 && (textureWidth != width || textureHeight != height)) {
        textureWidth = width;
        textureHeight = height;
        updateOverlaySize();
    }

    vr::VRVulkanTextureData_t vulkanData = buildVulkanTextureStruct(image, width, height);
    vr::Texture_t texture{(void*)&vulkanData, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
    std::lock_guard<std::mutex> lock(s_vulkanDeviceMutex);
    vrOverlay->SetOverlayTexture(overlayHandle, &texture);
}
