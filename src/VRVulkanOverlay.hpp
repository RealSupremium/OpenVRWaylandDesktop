#pragma once

#include <openvr.h>
#include <spa/param/video/raw.h>
#include <vulkan/vulkan.h>

#include <unordered_map>
#include <vector>

#include "InputManager.hpp"

struct DrmFormatModifier {
    spa_video_format format;
    uint64_t modifier;
};

struct DmaBufTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint64_t modifier = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

class VRVulkanOverlay {
   public:
    VRVulkanOverlay(InputManager& input);
    ~VRVulkanOverlay();

    bool initVR(const std::string& key = "com.wayland.desktop.overlay", const std::string& name = "Wayland Desktop");
    bool initVulkan();
    static void cleanupVulkan();
    void processEvents();
    void submitFrame(VkImage image, uint32_t width, uint32_t height);

    std::vector<DrmFormatModifier> getSupportedDrmModifiers();

    VkImage createFallbackTexture(uint32_t width, uint32_t height);
    void updateFrameData(const uint8_t* srcData, uint32_t width, uint32_t height, uint32_t stride);

    VkImage importDmaBufFrame(const void* bufferId, int fd, uint32_t width, uint32_t height, uint32_t stride = 0, uint32_t spaFormat = 0, uint64_t drmModifier = 0x00ffffffffffffffULL);
    void clearDmaBufCache();

    void setOverlayHeight(float heightInMeters);
    void setOverlayName(const std::string& name);
    void updateOverlaySize();
    bool isOverlayVisible() const;

    void suspendOverlayMouse() { overlayMouseActive = false; }

    VkImage getFallbackImage() const { return currentFallbackImage; }
    uint32_t getTextureWidth() const { return textureWidth; }
    uint32_t getTextureHeight() const { return textureHeight; }

    void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    static VkFormat spaFormatToVkFormat(uint32_t spaFormat);

   private:
    vr::IVROverlay* vrOverlay = nullptr;
    vr::VROverlayHandle_t overlayHandle = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnailHandle = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t cursorOverlayHandle = vr::k_ulOverlayHandleInvalid;
    InputManager& inputManager;

    bool overlayMouseActive = true;
    bool takingOverClick = false;
    bool lastOverlayVisible = false;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue vkQueue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;

    VkImage currentFallbackImage = VK_NULL_HANDLE;
    VkDeviceMemory fallbackMemory = VK_NULL_HANDLE;
    uint8_t* mappedData = nullptr;
    VkDeviceSize fallbackRowPitch = 0;
    VkDeviceSize fallbackOffset = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;

    VkImage cursorVkImage = VK_NULL_HANDLE;
    VkDeviceMemory cursorVkMemory = VK_NULL_HANDLE;

    std::unordered_map<const void*, DmaBufTexture> dmaBufCache;

    float overlayWidthMeters = 4.0f;
    float overlayHeightMeters = 2.0f;

    void createTransparentCursorTexture();
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    std::vector<std::string> getVulkanInstanceExtensions();
    std::vector<std::string> getVulkanDeviceExtensions();
    bool createVulkanInstance(const std::vector<std::string>& instExts);
    bool createVulkanDevice(const std::vector<std::string>& devExts);
    void destroyFallbackResources();
    vr::VRVulkanTextureData_t buildVulkanTextureStruct(VkImage image, uint32_t width, uint32_t height);
};
