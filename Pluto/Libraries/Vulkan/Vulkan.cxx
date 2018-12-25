#include <cstring>
#include <chrono>
#include <assert.h>
#include <iostream>
#include <set>
#include <string>

#include <vulkan/vulkan.hpp>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Vulkan.hxx"
#include "../GLFW/GLFW.hxx"

namespace Libraries
{

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT objType,
    uint64_t obj,
    size_t location,
    int32_t code,
    const char *layerPrefix,
    const char *msg,
    void *userData)
{
    std::cerr << "validation layer: " << msg << std::endl << std::endl;
    return VK_FALSE;
}

Vulkan *Vulkan::Get()
{
    static Vulkan instance;
    return &instance;
}

Vulkan::Vulkan() {}

Vulkan::~Vulkan() {}

/* Vulkan Instance */
bool Vulkan::create_instance(bool enable_validation_layers, vector<string> validation_layers, vector<string> instance_extensions)
{
    /* Prevent multiple instance creation */
    cout << "Creating vulkan instance" << endl;

    auto appInfo = vk::ApplicationInfo();
    appInfo.pApplicationName = "TEMPORARY APPLICATION NAME";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 1, 0);
    appInfo.pEngineName = "Pluto";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 1, 0);
    appInfo.apiVersion = VK_MAKE_VERSION(1, 1, 0);

    /* Determine the required instance extensions */
    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions;
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    instanceExtensions.clear();
    for (uint32_t i = 0; i < glfwExtensionCount; ++i)
        instanceExtensions.insert(glfwExtensions[i]);

    validationEnabled = enable_validation_layers;
    if (validationEnabled)
        instanceExtensions.insert(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    for (auto &string : instance_extensions)
        instanceExtensions.insert(string);

    /* Verify those extensions are supported */
    auto extensionProperties = vk::enumerateInstanceExtensionProperties();

    /* Check to see if the extensions we have are what are required by GLFW */
    for (auto requestedExtension : instanceExtensions)
    {
        bool extensionFound = false;
        for (auto extensionProp : extensionProperties)
            if (requestedExtension.compare(extensionProp.extensionName) == 0)
            {
                extensionFound = true;
                break;
            }
        if (!extensionFound)
        {
            cout << "Missing extension " + string(requestedExtension) << endl;
            return false;
        }
    }

    /* Check to see if we support the requested validation layers */
    auto layerProperties = vk::enumerateInstanceLayerProperties();
    validationLayers.clear();
    for (auto &string : validation_layers)
        validationLayers.insert(string);

    /* Check to see if the validation layers we have selected are available */
    for (auto requestedLayer : validationLayers)
    {
        bool layerFound = false;
        for (auto layerProp : layerProperties)
            if (requestedLayer.compare(layerProp.layerName) == 0)
            {
                layerFound = true;
                break;
            }
        if (!layerFound)
        {
            cout << "Missing validation layer " + string(requestedLayer) << endl;
            return false;
        }
    }

    /* Instance Info: Specifies global extensions and validation layers we'd like to use */
    vector<const char *> ext, vl;
    for (auto &string : instanceExtensions)
        ext.push_back(&string.front());
    for (auto &string : validationLayers)
        vl.push_back(&string.front());
    auto info = vk::InstanceCreateInfo();
    info.pApplicationInfo = &appInfo;
    info.enabledExtensionCount = (uint32_t)(ext.size());
    info.ppEnabledExtensionNames = ext.data();
    info.enabledLayerCount = (validationEnabled) ? (uint32_t)vl.size() : 0;
    info.ppEnabledLayerNames = (validationEnabled) ? vl.data() : nullptr;
    instance = vk::createInstance(info);

    /* This dispatch loader allows us to call extension functions */
    dldi = vk::DispatchLoaderDynamic(instance);

    /* Add an internal callback for the validation layers */
    if (validationEnabled)
    {
        auto createCallbackInfo = vk::DebugReportCallbackCreateInfoEXT();
        createCallbackInfo.flags = vk::DebugReportFlagBitsEXT::eWarning | vk::DebugReportFlagBitsEXT::eError;
        createCallbackInfo.pfnCallback = DebugCallback;

        /* The function to assign a callback for validation layers isn't loaded by default. Here, we get that function. */
        instance.createDebugReportCallbackEXT(createCallbackInfo, nullptr, dldi);
    }
    
    return true;
}

bool Vulkan::destroy_instance()
{
    try
    {
        /* Don't delete null instance */
        if (!instance)
            return false;

        /* Delete instance validation if enabled */
        if (internalCallback)
        {
            instance.destroyDebugReportCallbackEXT(internalCallback, nullptr, dldi);
        }

        // if (device)
        // {
        //     device.destroy();
        // }

        if (instance) {
            instance.destroy();
        }
        instanceExtensions.clear();
    }

    catch (const std::exception &e)
    {
        cout << "Exception thrown while destroying instance!" << e.what();
        return false;
    }

    return true;
}

vk::Instance Vulkan::get_instance() const
{
    return instance;
}

/* Vulkan Device */ 
bool Vulkan::create_device(vector<string> device_extensions, vector<string> device_features, uint32_t num_command_pools, vk::SurfaceKHR surface)
{
    if (device)
        return false;

    cout << "Picking first capable device" << endl;

    /* We need to look for and select a graphics card in the system that supports the features we need */
    /* We could technically choose multiple graphics cards and use them simultaneously, but for now just choose the first one */
    auto devices = instance.enumeratePhysicalDevices();
    if (devices.size() == 0)
    {
        cout << "Failed to find GPUs with Vulkan support!" << endl;
        return false;
    }

    /* There are three things we're going to look for. We need to support requested device extensions, support a
        graphics queue, and if targeting a surface, we need a present queue as well as an adequate swap chain.*/
    presentFamilyIndex = -1;
    graphicsFamilyIndex = -1;
    uint32_t numGraphicsQueues = -1;
    uint32_t numPresentQueues = -1;
    bool swapChainAdequate, extensionsSupported, featuresSupported, queuesFound;

    /* vector of string to vector of c string... */
    deviceExtensions.clear();
    for (auto &string : device_extensions)
    {
        deviceExtensions.insert(string);
        if (string.compare("VK_NVX_raytracing") == 0) {
            rayTracingEnabled = true;
        }
    }
    if (surface)
        deviceExtensions.insert(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    /* Check and see if any physical devices are suitable, since not all cards are equal */
    physicalDevice = vk::PhysicalDevice();
    std::string devicename;
    for (const auto &device : devices)
    {
        swapChainAdequate = extensionsSupported = queuesFound = false;
        deviceProperties = device.getProperties();
        auto supportedFeatures = device.getFeatures();
        auto queueFamilyProperties = device.getQueueFamilyProperties();
        std::cout<<"\tAvailable device: " << deviceProperties.deviceName <<std::endl;

        /* Look for a queue family which supports what we need (graphics, maybe also present) */
        int32_t i = 0;
        for (auto queueFamily : queueFamilyProperties)
        {
            auto hasQueues = (queueFamily.queueCount > 0);
            auto presentSupport = (surface) ? device.getSurfaceSupportKHR(i, surface) : false;
            if (hasQueues && presentSupport) {
                presentFamilyIndex = i;
                numPresentQueues = queueFamily.queueCount;
            }
            if (hasQueues && queueFamily.queueFlags & vk::QueueFlagBits::eGraphics) {
                graphicsFamilyIndex = i;
                numGraphicsQueues = queueFamily.queueCount;
            }
            if (((!surface) || presentFamilyIndex != -1) && graphicsFamilyIndex != -1)
            {
                queuesFound = true;
                break;
            }
            i++;
        }

        /* Check if the device meets our extension requirements */
        auto availableExtensions = device.enumerateDeviceExtensionProperties();
        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

        /* Indicate extensions found by removing them from this set. */
        for (const auto &extension : availableExtensions)
            requiredExtensions.erase(extension.extensionName);
        extensionsSupported = requiredExtensions.empty();

        /* If presentation required, see if swapchain support is adequate */
        /* For this example, all we require is one supported image format and one supported presentation mode */
        if (surface && extensionsSupported)
        {
            Vulkan::SwapChainSupportDetails details;
            auto capabilities = device.getSurfaceCapabilitiesKHR(surface);
            auto formats = device.getSurfaceFormatsKHR(surface);
            auto presentModes = device.getSurfacePresentModesKHR(surface);
            swapChainAdequate = !formats.empty() && !presentModes.empty();
        }

        /* Check if the device supports the featrues we want */
        featuresSupported = GetFeaturesFromList(device_features, supportedFeatures, deviceFeatures);

        if (queuesFound && extensionsSupported && featuresSupported && ((!surface) || swapChainAdequate))
        {
            physicalDevice = device;
            devicename = deviceProperties.deviceName;
            if (deviceProperties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
                break;
            }
        }
    }
    
    cout << "\tChoosing device " << std::string(deviceProperties.deviceName) << endl;

    if (!physicalDevice)
    {
        cout << "Failed to find a GPU which meets demands!" << endl;
        return false;
    }

    /* We now need to create a logical device, which is like an instance of a physical device */
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
    
    /* Add these queue create infos to a vector to be used when creating the logical device */
    /* Vulkan allows you to specify a queue priority between 0 and one, which influences scheduling */
    
    std::vector<float> queuePriorities(numGraphicsQueues, 1.0f);
    
    /* Graphics queue */
    vk::DeviceQueueCreateInfo gQueueInfo;
    gQueueInfo.queueFamilyIndex = graphicsFamilyIndex;
    gQueueInfo.queueCount = numGraphicsQueues;
    gQueueInfo.pQueuePriorities = queuePriorities.data();
    queueCreateInfos.push_back(gQueueInfo);

    /* Present queue (if different than graphics queue) */
    if (surface && (presentFamilyIndex != graphicsFamilyIndex)) {
        vk::DeviceQueueCreateInfo pQueueInfo;
        pQueueInfo.queueFamilyIndex = presentFamilyIndex;
        pQueueInfo.queueCount = numPresentQueues;
        pQueueInfo.pQueuePriorities = queuePriorities.data();
        queueCreateInfos.push_back(pQueueInfo);
    }

    auto createInfo = vk::DeviceCreateInfo();

    /* Add pointers to the device features and queue creation structs */
    createInfo.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;

    /* We can specify device specific extensions, like "VK_KHR_swapchain", which may not be
    available for particular compute only devices. */
    vector<const char *> ext;
    for (auto &string : deviceExtensions)
        ext.push_back(&string.front());
    createInfo.enabledExtensionCount = (uint32_t)(ext.size());
    createInfo.ppEnabledExtensionNames = ext.data();

    /* Now create the logical device! */
    device = physicalDevice.createDevice(createInfo);

    /* Queues are implicitly created when creating device. This just gets handles. */
    for (uint32_t i = 0; i < numGraphicsQueues; ++i)
        graphicsQueues.push_back(device.getQueue(graphicsFamilyIndex, i));
    if (surface)
        for (uint32_t i = 0; i < numPresentQueues; ++i)
            presentQueues.push_back(device.getQueue(presentFamilyIndex, i));

    /* Command pools manage the memory that is used to store the buffers and command buffers are allocated from them */
    for (uint32_t i = 0; i < num_command_pools; ++i) {
        auto poolInfo = vk::CommandPoolCreateInfo();
        poolInfo.queueFamilyIndex = graphicsFamilyIndex;
        poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer; // Optional
        commandPools.push_back(device.createCommandPool(poolInfo));
    }


    initialized = true;
    return true;
}

bool Vulkan::destroy_device()
{
    try
    {
        if (!device)
            return false;

        // for (uint32_t i = 0; i < commandPools.size(); ++i) {
        //     if (commandPools[i] != vk::CommandPool())
        //         device.destroyCommandPool(commandPools[i]);
        // }
        device.destroy();
        
        return true;
    }
    catch (const std::exception &e)
    {
        cout << "Exception thrown while destroying device!" << e.what();
        return false;
    }
    return true;
}

vk::PhysicalDevice Vulkan::get_physical_device() const
{
    if (!physicalDevice)
        return vk::PhysicalDevice();
    return physicalDevice;
}

vk::Device Vulkan::get_device() const
{
    if (!device)
        return vk::Device();
    return device;
}

/* Todo, macro this */
bool Vulkan::GetFeaturesFromList(vector<string> device_features, vk::PhysicalDeviceFeatures &supportedFeatures, vk::PhysicalDeviceFeatures &requestedFeatures)
{
    for (auto feature : device_features)
    {
        if (feature.compare("robustBufferAccess") == 0)
        {
            if (!supportedFeatures.robustBufferAccess)
                return false;
            requestedFeatures.robustBufferAccess = VK_TRUE;
        }
        else if (feature.compare("fullDrawIndexUint32") == 0)
        {
            if (!supportedFeatures.fullDrawIndexUint32)
                return false;
            requestedFeatures.fullDrawIndexUint32 = VK_TRUE;
        }
        else if (feature.compare("imageCubeArray") == 0)
        {
            if (!supportedFeatures.imageCubeArray)
                return false;
            requestedFeatures.imageCubeArray = VK_TRUE;
        }
        else if (feature.compare("independentBlend") == 0)
        {
            if (!supportedFeatures.independentBlend)
                return false;
            requestedFeatures.independentBlend = VK_TRUE;
        }
        else if (feature.compare("geometryShader") == 0)
        {
            if (!supportedFeatures.geometryShader)
                return false;
            requestedFeatures.geometryShader = VK_TRUE;
        }
        else if (feature.compare("tessellationShader") == 0)
        {
            if (!supportedFeatures.tessellationShader)
                return false;
            requestedFeatures.tessellationShader = VK_TRUE;
        }
        else if (feature.compare("sampleRateShading") == 0)
        {
            if (!supportedFeatures.sampleRateShading)
                return false;
            requestedFeatures.sampleRateShading = VK_TRUE;
        }
        else if (feature.compare("dualSrcBlend") == 0)
        {
            if (!supportedFeatures.dualSrcBlend)
                return false;
            requestedFeatures.dualSrcBlend = VK_TRUE;
        }
        else if (feature.compare("logicOp") == 0)
        {
            if (!supportedFeatures.logicOp)
                return false;
            requestedFeatures.logicOp = VK_TRUE;
        }
        else if (feature.compare("multiDrawIndirect") == 0)
        {
            if (!supportedFeatures.multiDrawIndirect)
                return false;
            requestedFeatures.multiDrawIndirect = VK_TRUE;
        }
        else if (feature.compare("drawIndirectFirstInstance") == 0)
        {
            if (!supportedFeatures.drawIndirectFirstInstance)
                return false;
            requestedFeatures.drawIndirectFirstInstance = VK_TRUE;
        }
        else if (feature.compare("depthClamp") == 0)
        {
            if (!supportedFeatures.depthClamp)
                return false;
            requestedFeatures.depthClamp = VK_TRUE;
        }
        else if (feature.compare("depthBiasClamp") == 0)
        {
            if (!supportedFeatures.depthBiasClamp)
                return false;
            requestedFeatures.depthBiasClamp = VK_TRUE;
        }
        else if (feature.compare("fillModeNonSolid") == 0)
        {
            if (!supportedFeatures.fillModeNonSolid)
                return false;
            requestedFeatures.fillModeNonSolid = VK_TRUE;
        }
        else if (feature.compare("depthBounds") == 0)
        {
            if (!supportedFeatures.depthBounds)
                return false;
            requestedFeatures.depthBounds = VK_TRUE;
        }
        else if (feature.compare("wideLines") == 0)
        {
            if (!supportedFeatures.wideLines)
                return false;
            requestedFeatures.wideLines = VK_TRUE;
        }
        else if (feature.compare("largePoints") == 0)
        {
            if (!supportedFeatures.largePoints)
                return false;
            requestedFeatures.largePoints = VK_TRUE;
        }
        else if (feature.compare("alphaToOne") == 0)
        {
            if (!supportedFeatures.alphaToOne)
                return false;
            requestedFeatures.alphaToOne = VK_TRUE;
        }
        else if (feature.compare("multiViewport") == 0)
        {
            if (!supportedFeatures.multiViewport)
                return false;
            requestedFeatures.multiViewport = VK_TRUE;
        }
        else if (feature.compare("samplerAnisotropy") == 0)
        {
            if (!supportedFeatures.samplerAnisotropy)
                return false;
            requestedFeatures.samplerAnisotropy = VK_TRUE;
        }
        else if (feature.compare("textureCompressionETC2") == 0)
        {
            if (!supportedFeatures.textureCompressionETC2)
                return false;
            requestedFeatures.textureCompressionETC2 = VK_TRUE;
        }
        else if (feature.compare("textureCompressionASTC_LDR") == 0)
        {
            if (!supportedFeatures.textureCompressionASTC_LDR)
                return false;
            requestedFeatures.textureCompressionASTC_LDR = VK_TRUE;
        }
        else if (feature.compare("textureCompressionBC") == 0)
        {
            if (!supportedFeatures.textureCompressionBC)
                return false;
            requestedFeatures.textureCompressionBC = VK_TRUE;
        }
        else if (feature.compare("occlusionQueryPrecise") == 0)
        {
            if (!supportedFeatures.occlusionQueryPrecise)
                return false;
            requestedFeatures.occlusionQueryPrecise = VK_TRUE;
        }
        else if (feature.compare("pipelineStatisticsQuery") == 0)
        {
            if (!supportedFeatures.pipelineStatisticsQuery)
                return false;
            requestedFeatures.pipelineStatisticsQuery = VK_TRUE;
        }
        else if (feature.compare("vertexPipelineStoresAndAtomics") == 0)
        {
            if (!supportedFeatures.vertexPipelineStoresAndAtomics)
                return false;
            requestedFeatures.vertexPipelineStoresAndAtomics = VK_TRUE;
        }
        else if (feature.compare("fragmentStoresAndAtomics") == 0)
        {
            if (!supportedFeatures.fragmentStoresAndAtomics)
                return false;
            requestedFeatures.fragmentStoresAndAtomics = VK_TRUE;
        }
        else if (feature.compare("shaderTessellationAndGeometryPointSize") == 0)
        {
            if (!supportedFeatures.shaderTessellationAndGeometryPointSize)
                return false;
            requestedFeatures.shaderTessellationAndGeometryPointSize = VK_TRUE;
        }
        else if (feature.compare("shaderImageGatherExtended") == 0)
        {
            if (!supportedFeatures.shaderImageGatherExtended)
                return false;
            requestedFeatures.shaderImageGatherExtended = VK_TRUE;
        }
        else if (feature.compare("shaderStorageImageExtendedFormats") == 0)
        {
            if (!supportedFeatures.shaderStorageImageExtendedFormats)
                return false;
            requestedFeatures.shaderStorageImageExtendedFormats = VK_TRUE;
        }
        else if (feature.compare("shaderStorageImageMultisample") == 0)
        {
            if (!supportedFeatures.shaderStorageImageMultisample)
                return false;
            requestedFeatures.shaderStorageImageMultisample = VK_TRUE;
        }
        else if (feature.compare("shaderStorageImageReadWithoutFormat") == 0)
        {
            if (!supportedFeatures.shaderStorageImageReadWithoutFormat)
                return false;
            requestedFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
        }
        else if (feature.compare("shaderStorageImageWriteWithoutFormat") == 0)
        {
            if (!supportedFeatures.shaderStorageImageWriteWithoutFormat)
                return false;
            requestedFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        }
        else if (feature.compare("shaderUniformBufferArrayDynamicIndexing") == 0)
        {
            if (!supportedFeatures.shaderUniformBufferArrayDynamicIndexing)
                return false;
            requestedFeatures.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
        }
        else if (feature.compare("shaderSampledImageArrayDynamicIndexing") == 0)
        {
            if (!supportedFeatures.shaderSampledImageArrayDynamicIndexing)
                return false;
            requestedFeatures.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
        }
        else if (feature.compare("shaderStorageBufferArrayDynamicIndexing") == 0)
        {
            if (!supportedFeatures.shaderStorageBufferArrayDynamicIndexing)
                return false;
            requestedFeatures.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
        }
        else if (feature.compare("shaderStorageImageArrayDynamicIndexing") == 0)
        {
            if (!supportedFeatures.shaderStorageImageArrayDynamicIndexing)
                return false;
            requestedFeatures.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
        }
        else if (feature.compare("shaderClipDistance") == 0)
        {
            if (!supportedFeatures.shaderClipDistance)
                return false;
            requestedFeatures.shaderClipDistance = VK_TRUE;
        }
        else if (feature.compare("shaderCullDistance") == 0)
        {
            if (!supportedFeatures.shaderCullDistance)
                return false;
            requestedFeatures.shaderCullDistance = VK_TRUE;
        }
        else if (feature.compare("shaderFloat64") == 0)
        {
            if (!supportedFeatures.shaderFloat64)
                return false;
            requestedFeatures.shaderFloat64 = VK_TRUE;
        }
        else if (feature.compare("shaderInt64") == 0)
        {
            if (!supportedFeatures.shaderInt64)
                return false;
            requestedFeatures.shaderInt64 = VK_TRUE;
        }
        else if (feature.compare("shaderInt16") == 0)
        {
            if (!supportedFeatures.shaderInt16)
                return false;
            requestedFeatures.shaderInt16 = VK_TRUE;
        }
        else if (feature.compare("shaderResourceResidency") == 0)
        {
            if (!supportedFeatures.shaderResourceResidency)
                return false;
            requestedFeatures.shaderResourceResidency = VK_TRUE;
        }
        else if (feature.compare("shaderResourceMinLod") == 0)
        {
            if (!supportedFeatures.shaderResourceMinLod)
                return false;
            requestedFeatures.shaderResourceMinLod = VK_TRUE;
        }
        else if (feature.compare("sparseBinding") == 0)
        {
            if (!supportedFeatures.sparseBinding)
                return false;
            requestedFeatures.sparseBinding = VK_TRUE;
        }
        else if (feature.compare("sparseResidencyBuffer") == 0)
        {
            if (!supportedFeatures.sparseResidencyBuffer)
                return false;
            requestedFeatures.sparseResidencyBuffer = VK_TRUE;
        }
        else if (feature.compare("sparseResidencyImage2D") == 0)
        {
            if (!supportedFeatures.sparseResidencyImage2D)
                return false;
            requestedFeatures.sparseResidencyImage2D = VK_TRUE;
        }
        else if (feature.compare("sparseResidencyImage3D") == 0)
        {
            if (!supportedFeatures.sparseResidencyImage3D)
                return false;
            requestedFeatures.sparseResidencyImage3D = VK_TRUE;
        }
        else if (feature.compare("sparseResidency2Samples") == 0)
        {
            if (!supportedFeatures.sparseResidency2Samples)
                return false;
            requestedFeatures.sparseResidency2Samples = VK_TRUE;
        }
        else if (feature.compare("sparseResidency4Samples") == 0)
        {
            if (!supportedFeatures.sparseResidency4Samples)
                return false;
            requestedFeatures.sparseResidency4Samples = VK_TRUE;
        }
        else if (feature.compare("sparseResidency8Samples") == 0)
        {
            if (!supportedFeatures.sparseResidency8Samples)
                return false;
            requestedFeatures.sparseResidency8Samples = VK_TRUE;
        }
        else if (feature.compare("sparseResidency16Samples") == 0)
        {
            if (!supportedFeatures.sparseResidency16Samples)
                return false;
            requestedFeatures.sparseResidency16Samples = VK_TRUE;
        }
        else if (feature.compare("sparseResidencyAliased") == 0)
        {
            if (!supportedFeatures.sparseResidencyAliased)
                return false;
            requestedFeatures.sparseResidencyAliased = VK_TRUE;
        }
        else if (feature.compare("variableMultisampleRate") == 0)
        {
            if (!supportedFeatures.variableMultisampleRate)
                return false;
            requestedFeatures.variableMultisampleRate = VK_TRUE;
        }
        else if (feature.compare("inheritedQueries") == 0)
        {
            if (!supportedFeatures.inheritedQueries)
                return false;
            requestedFeatures.inheritedQueries = VK_TRUE;
        }
        else
        {
            return false;
        }
    }
    return true;
}

/* Accessors */
uint32_t Vulkan::get_graphics_family() const
{
    return graphicsFamilyIndex;
}

uint32_t Vulkan::get_present_family() const
{
    return presentFamilyIndex;
}

vk::CommandPool Vulkan::get_command_pool(uint32_t index) const
{
    if (index >= commandPools.size()) {
        std::cout<<"Error, max command pool index is " << commandPools.size() - 1 << std::endl;
        return vk::CommandPool();
    }
    return commandPools[index];
}

vk::Queue Vulkan::get_graphics_queue(uint32_t index) const
{
    if (index >= graphicsQueues.size()) {
        std::cout<<"Error, max graphics queue index is " << graphicsQueues.size() - 1 << std::endl;
        return vk::Queue();
    }
    return graphicsQueues[index];
}
vk::Queue Vulkan::get_present_queue(uint32_t index) const
{
    if (index >= presentQueues.size()) {
        std::cout<<"Error, max command pool index is " << presentQueues.size() - 1 << std::endl;
        return vk::Queue();
    }
    return presentQueues[index];
}

vk::DispatchLoaderDynamic Vulkan::get_dispatch_loader_dynamic() const
{
    return dldi;
}

/* Utility functions */

uint32_t Vulkan::find_memory_type(uint32_t typeFilter, vk::MemoryPropertyFlags properties) {
    /* Query available device memory properties */
    vk::PhysicalDeviceMemoryProperties memProperties = physicalDevice.getMemoryProperties();
    
    /* Try to find some memory that matches the type we'd like. */
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return -1;
}

vk::CommandBuffer Vulkan::begin_one_time_graphics_command(uint32_t pool_id) {
    vk::CommandBufferAllocateInfo cmdAllocInfo;
    cmdAllocInfo.commandPool = get_command_pool(pool_id);
    cmdAllocInfo.level = vk::CommandBufferLevel::ePrimary;
    cmdAllocInfo.commandBufferCount = 1;
    vk::CommandBuffer cmdBuffer = device.allocateCommandBuffers(cmdAllocInfo)[0];

    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    cmdBuffer.begin(beginInfo);
    return cmdBuffer;
}

bool Vulkan::end_one_time_graphics_command(vk::CommandBuffer command_buffer, uint32_t pool_id, bool free_after_use, bool submit_immediately) {
    command_buffer.end();
    vk::SubmitInfo submit_info;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    
    vk::FenceCreateInfo fenceInfo;
    vk::Fence fence = device.createFence(fenceInfo);
    std::future<void> fut = enqueue_graphics_commands(submit_info, fence);

    if (submit_immediately || pool_id == 0) submit_graphics_commands();

    fut.wait();

    device.waitForFences(fence, true, 100000000000);

    if (free_after_use)
        device.freeCommandBuffers(get_command_pool(pool_id), {command_buffer});
    device.destroyFence(fence);
    return true;
}

std::future<void> Vulkan::enqueue_graphics_commands(vk::SubmitInfo submit_info, vk::Fence fence) {
    graphicsCommandQueue.submissions.push_back(submit_info);
    graphicsCommandQueue.fences.push_back(fence);
    graphicsCommandQueue.promises.push_back(std::promise<void>());
    return graphicsCommandQueue.promises[graphicsCommandQueue.promises.size() - 1].get_future();
}

std::future<void> Vulkan::enqueue_present_commands(vk::PresentInfoKHR present_info) {
    presentCommandQueue.presentations.push_back(present_info);
    presentCommandQueue.promises.push_back(std::promise<void>());
    return presentCommandQueue.promises[presentCommandQueue.promises.size() - 1].get_future();
}

bool Vulkan::submit_graphics_commands() {
    for (int i = 0; i < graphicsCommandQueue.submissions.size(); ++i) {
        graphicsQueues[0].submit(graphicsCommandQueue.submissions[i], graphicsCommandQueue.fences[i]);
        graphicsCommandQueue.promises[i].set_value();
    }
    graphicsCommandQueue.submissions.clear();
    graphicsCommandQueue.fences.clear();
    graphicsCommandQueue.promises.clear();
    return true;
}

bool Vulkan::submit_present_commands() {
    bool result = true;
    for (int i = 0; i < presentCommandQueue.presentations.size(); ++i) {
        try {
            presentQueues[0].presentKHR(presentCommandQueue.presentations[i]);
            //presentQueues[0].waitIdle();
        }
        catch (...) {
            result = false;
        }
        presentCommandQueue.promises[i].set_value();
    }
    presentCommandQueue.presentations.clear();
    presentCommandQueue.promises.clear();
    return result;
}

uint32_t get_thread_id() {
    return 0;
    // Determine if thread is in our map.
    // If it isn't, increment the registered number of threads, add this thread to the map.
    // Return this thread's mapped id.
    /* Could eventually be mapped to a range of ids for different pools. */
}

bool Vulkan::is_ray_tracing_enabled() {
    return rayTracingEnabled;
}

} // namespace Libraries