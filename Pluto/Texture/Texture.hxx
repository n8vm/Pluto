#pragma once

#include <iostream>
#include <gli/gli.hpp>

//#include "stb_image.h"
#include "Pluto/Tools/Options.hxx"
#include "Pluto/Libraries/Vulkan/Vulkan.hxx"
#include "Pluto/Tools/StaticFactory.hxx"
#include <map>

class Texture : public StaticFactory
{
    public:
    struct Data {
        vk::Image colorImage, depthImage;
        vk::Format colorFormat, depthFormat;
        vk::DeviceMemory colorImageMemory, depthImageMemory;
        vk::ImageView colorImageView, depthImageView;
        vk::ImageLayout colorImageLayout, depthImageLayout;
        vk::Sampler colorSampler, depthSampler;
        uint32_t width = 1, height = 1, depth = 1, colorMipLevels = 1, layers = 1;
        vk::ImageViewType viewType;
        vk::ImageType imageType;
    };

    /* Constructors */
    static std::shared_ptr<Texture> CreateFromKTX(std::string name, std::string filepath);
    static std::shared_ptr<Texture> CreateFromExternalData(std::string name, Data data);
    std::shared_ptr<Texture> Texture::Create2DFromColorData (
        std::string name, uint32_t width, uint32_t height, std::vector<float> data);
    static std::shared_ptr<Texture> CreateCubemap(std::string name, uint32_t width, uint32_t height, bool hasColor, bool hasDepth);
    static std::shared_ptr<Texture> Create2D(std::string name, uint32_t width, uint32_t height, bool hasColor, bool hasDepth);
    
    static std::shared_ptr<Texture> Get(std::string name);
    
    Texture(std::string name)
    {
        this->name = name;
    }

    /* Accessors and mutators */
    void setData(Data data) {
        this->madeExternally = true;
        this->data = data;
    }

    std::vector<float> download_color_data(uint32_t width, uint32_t height, uint32_t depth, uint32_t pool = 1)
    {
        /* I'm assuming an image was already loaded for now */
        auto vulkan = Libraries::Vulkan::Get();
        auto physicalDevice = vulkan->get_physical_device();
        auto device = vulkan->get_device();

        /* Since the image memory is device local and tiled, we need to 
            make a copy, transform to an untiled format. */

        /* Create staging buffer */
        vk::BufferCreateInfo bufferInfo;
        bufferInfo.size = width * height * depth * 4 * sizeof(float);
        bufferInfo.usage = vk::BufferUsageFlagBits::eTransferDst;
        bufferInfo.sharingMode = vk::SharingMode::eExclusive;
        vk::Buffer stagingBuffer = device.createBuffer(bufferInfo);

        vk::MemoryRequirements stagingMemRequirements = device.getBufferMemoryRequirements(stagingBuffer);
        vk::MemoryAllocateInfo stagingAllocInfo;
        stagingAllocInfo.allocationSize = (uint32_t)stagingMemRequirements.size;
        stagingAllocInfo.memoryTypeIndex = vulkan->find_memory_type(stagingMemRequirements.memoryTypeBits,
                                                                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::DeviceMemory stagingBufferMemory = device.allocateMemory(stagingAllocInfo);

        device.bindBufferMemory(stagingBuffer, stagingBufferMemory, 0);

        /* Create a copy of image */
        vk::ImageCreateInfo imInfo;
        // imInfo.flags; // May need this later for cubemaps, texture arrays, etc
        imInfo.imageType = data.imageType;
        imInfo.format = vk::Format::eR32G32B32A32Sfloat;
        imInfo.extent = {width, height, depth};
        imInfo.mipLevels = 1;
        imInfo.arrayLayers = 1;
        imInfo.samples = vk::SampleCountFlagBits::e1;
        imInfo.tiling = vk::ImageTiling::eOptimal;
        imInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
        imInfo.sharingMode = vk::SharingMode::eExclusive;
        imInfo.initialLayout = vk::ImageLayout::eUndefined;
        vk::Image blitImage = device.createImage(imInfo);

        /* Create memory for that image */
        vk::MemoryRequirements imageMemReqs = device.getImageMemoryRequirements(blitImage);
        vk::MemoryAllocateInfo imageAllocInfo;
        imageAllocInfo.allocationSize = imageMemReqs.size;
        imageAllocInfo.memoryTypeIndex = vulkan->find_memory_type(
            imageMemReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        vk::DeviceMemory blitImageMemory = device.allocateMemory(imageAllocInfo);

        /* Bind the image with its memory */
        device.bindImageMemory(blitImage, blitImageMemory, 0);

        /* Now, we need to blit the texture to this host image */

        /* First, specify the region we'd like to copy */
        vk::ImageSubresourceLayers srcSubresourceLayers;
        srcSubresourceLayers.aspectMask = vk::ImageAspectFlagBits::eColor;
        srcSubresourceLayers.mipLevel = 0;
        srcSubresourceLayers.baseArrayLayer = 0;
        srcSubresourceLayers.layerCount = 1; // TODO

        vk::ImageSubresourceLayers dstSubresourceLayers;
        dstSubresourceLayers.aspectMask = vk::ImageAspectFlagBits::eColor;
        dstSubresourceLayers.mipLevel = 0;
        dstSubresourceLayers.baseArrayLayer = 0;
        dstSubresourceLayers.layerCount = 1; // TODO

        vk::ImageBlit region;
        region.srcSubresource = srcSubresourceLayers;
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {(int32_t)this->data.width, (int32_t)this->data.height, (int32_t)this->data.depth};
        region.dstSubresource = dstSubresourceLayers;
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {(int32_t)width, (int32_t)height, (int32_t)depth};

        /* Next, specify the filter we'd like to use */
        vk::Filter filter = vk::Filter::eLinear;

        /* Now, create a command buffer */
        vk::CommandBuffer cmdBuffer = vulkan->begin_one_time_graphics_command(pool);

        /* Transition destination image to transfer destination optimal */
        vk::ImageSubresourceRange dstSubresourceRange;
        dstSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        dstSubresourceRange.baseMipLevel = 0;
        dstSubresourceRange.levelCount = 1;
        dstSubresourceRange.layerCount = 1; // TODO

        vk::ImageSubresourceRange srcSubresourceRange;
        srcSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        srcSubresourceRange.baseMipLevel = 0;
        srcSubresourceRange.levelCount = data.colorMipLevels;
        srcSubresourceRange.layerCount = 1; //.............

        setImageLayout(
            cmdBuffer,
            blitImage,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,
            dstSubresourceRange);

        /* Transition source to shader read optimal */
        auto originalLayout = data.colorImageLayout;
        setImageLayout(
            cmdBuffer,
            data.colorImage,
            data.colorImageLayout,
            vk::ImageLayout::eTransferSrcOptimal,
            srcSubresourceRange);
        data.colorImageLayout = vk::ImageLayout::eTransferSrcOptimal;

        /* Blit the image... */
        cmdBuffer.blitImage(
            data.colorImage,
            data.colorImageLayout,
            blitImage,
            vk::ImageLayout::eTransferDstOptimal,
            region,
            filter);

        setImageLayout(
            cmdBuffer,
            blitImage,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferSrcOptimal,
            dstSubresourceRange);

        /* Copy the blit image to a host visable staging buffer */
        vk::BufferImageCopy imgCopyRegion;
        // imgCopyRegion.bufferOffset = 0;
        // imgCopyRegion.bufferRowLength =
        // imgCopyRegion.bufferImageHeight = 0;
        imgCopyRegion.imageSubresource = dstSubresourceLayers;
        imgCopyRegion.imageOffset = {0, 0, 0};
        imgCopyRegion.imageExtent = {(uint32_t)width, (uint32_t)height, (uint32_t)depth};

        cmdBuffer.copyImageToBuffer(blitImage, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer, imgCopyRegion);

        /* Transition source back to previous layout */
        setImageLayout(
            cmdBuffer,
            data.colorImage,
            data.colorImageLayout,
            originalLayout,
            srcSubresourceRange);
        data.colorImageLayout = originalLayout;
        vulkan->end_one_time_graphics_command(cmdBuffer, pool);

        /* Memcpy from host visable image here... */
        /* Copy texture data into staging buffer */
        std::vector<float> result(width * height * depth * 4, 0.0);
        void *data = device.mapMemory(stagingBufferMemory, 0, width * height * depth * 4 * sizeof(float), vk::MemoryMapFlags());
        memcpy(result.data(), data, width * height * depth * 4 * sizeof(float));
        device.unmapMemory(stagingBufferMemory);

        /* Clean up */
        device.destroyBuffer(stagingBuffer);
        device.freeMemory(stagingBufferMemory);
        device.destroyImage(blitImage);
        device.freeMemory(blitImageMemory);

        return result;
    }

    bool upload_color_data(uint32_t width, uint32_t height, uint32_t depth, std::vector<float> color_data, uint32_t pool_id = 1)
    {
        /* I'm assuming an image was already loaded for now */
        auto vulkan = Libraries::Vulkan::Get();
        auto physicalDevice = vulkan->get_physical_device();
        auto device = vulkan->get_device();

        uint32_t textureSize = width * height * depth * 4 * sizeof(float);
        if (color_data.size() < width * height * depth) {
            std::cout<<"Not enough data for provided image dimensions"<<std::endl;
            return false;
        }

        /* Create staging buffer */
        vk::BufferCreateInfo bufferInfo;
        bufferInfo.size = textureSize;
        bufferInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
        bufferInfo.sharingMode = vk::SharingMode::eExclusive;
        vk::Buffer stagingBuffer = device.createBuffer(bufferInfo);

        vk::MemoryRequirements stagingMemRequirements = device.getBufferMemoryRequirements(stagingBuffer);
        vk::MemoryAllocateInfo stagingAllocInfo;
        stagingAllocInfo.allocationSize = (uint32_t)stagingMemRequirements.size;
        stagingAllocInfo.memoryTypeIndex = vulkan->find_memory_type(stagingMemRequirements.memoryTypeBits,
                                                                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::DeviceMemory stagingBufferMemory = device.allocateMemory(stagingAllocInfo);

        device.bindBufferMemory(stagingBuffer, stagingBufferMemory, 0);

        /* Copy texture data into staging buffer */
        void *dataptr = device.mapMemory(stagingBufferMemory, 0, textureSize, vk::MemoryMapFlags());
        memcpy(dataptr, color_data.data(), textureSize);
        device.unmapMemory(stagingBufferMemory);

        /* Setup buffer copy regions for one mip level */
        vk::BufferImageCopy bufferCopyRegion;
        bufferCopyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        bufferCopyRegion.imageSubresource.mipLevel = 0;
        bufferCopyRegion.imageSubresource.baseArrayLayer = 0;
        bufferCopyRegion.imageSubresource.layerCount = 1;
        bufferCopyRegion.imageExtent.width = width;
        bufferCopyRegion.imageExtent.height = height;
        bufferCopyRegion.imageExtent.depth = depth;
        bufferCopyRegion.bufferOffset = 0;

        /* Create optimal tiled target image */
        vk::ImageCreateInfo imageCreateInfo;
        imageCreateInfo.imageType = data.imageType; // src and dst types must match. Deal with this in error check
        imageCreateInfo.format = vk::Format::eR32G32B32A32Sfloat;
        imageCreateInfo.mipLevels = 1;
        imageCreateInfo.arrayLayers = 1; 
        imageCreateInfo.samples = vk::SampleCountFlagBits::e1;
        imageCreateInfo.tiling = vk::ImageTiling::eOptimal;
        imageCreateInfo.sharingMode = vk::SharingMode::eExclusive;
        imageCreateInfo.initialLayout = vk::ImageLayout::eUndefined;
        imageCreateInfo.extent = {width, height, depth};
        imageCreateInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
        vk::Image src_image = device.createImage(imageCreateInfo);

        /* Allocate and bind memory for the texture */
        vk::MemoryRequirements imageMemReqs = device.getImageMemoryRequirements(src_image);
        vk::MemoryAllocateInfo imageAllocInfo;
        imageAllocInfo.allocationSize = imageMemReqs.size;
        imageAllocInfo.memoryTypeIndex = vulkan->find_memory_type(
            imageMemReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        vk::DeviceMemory src_image_memory = device.allocateMemory(imageAllocInfo);

        device.bindImageMemory(src_image, src_image_memory, 0);
        
        /* END CREATE IMAGE */

        vk::CommandBuffer command_buffer = vulkan->begin_one_time_graphics_command(pool_id);

        /* Which mip level, array layer, layer count, access mask to use */
        vk::ImageSubresourceLayers srcSubresourceLayers;
        srcSubresourceLayers.aspectMask = vk::ImageAspectFlagBits::eColor;
        srcSubresourceLayers.mipLevel = 0;
        srcSubresourceLayers.baseArrayLayer = 0;
        srcSubresourceLayers.layerCount = 1; // TODO

        vk::ImageSubresourceLayers dstSubresourceLayers;
        dstSubresourceLayers.aspectMask = vk::ImageAspectFlagBits::eColor;
        dstSubresourceLayers.mipLevel = 0;
        dstSubresourceLayers.baseArrayLayer = 0;
        dstSubresourceLayers.layerCount = 1; // TODO

        /* For layout transitions */
        vk::ImageSubresourceRange srcSubresourceRange;
        srcSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        srcSubresourceRange.baseMipLevel = 0;
        srcSubresourceRange.levelCount = 1;
        srcSubresourceRange.layerCount = 1;

        vk::ImageSubresourceRange dstSubresourceRange;
        dstSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        dstSubresourceRange.baseMipLevel = 0;
        dstSubresourceRange.levelCount = data.colorMipLevels;
        dstSubresourceRange.layerCount = 1;

        /* First, copy the staging buffer into our temporary source image. */
        setImageLayout(command_buffer, src_image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, srcSubresourceRange);

        /* Copy mip levels from staging buffer */
        command_buffer.copyBufferToImage( stagingBuffer, src_image, vk::ImageLayout::eTransferDstOptimal, 1, &bufferCopyRegion);

        /* Region to copy (Possibly multiple in the future) */
        vk::ImageBlit region;
        region.dstSubresource = dstSubresourceLayers;
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {(int32_t)this->data.width, (int32_t)this->data.height, (int32_t)this->data.depth};
        region.srcSubresource = srcSubresourceLayers;
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {(int32_t)width, (int32_t)height, (int32_t)depth};

        /* Next, specify the filter we'd like to use */
        vk::Filter filter = vk::Filter::eLinear;

        /* transition source to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL  */
        setImageLayout( command_buffer, src_image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal, srcSubresourceRange);
            
        /* transition source to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL  */
        setImageLayout( command_buffer, data.colorImage, data.colorImageLayout, vk::ImageLayout::eTransferDstOptimal, dstSubresourceRange);

        /* Blit the uploaded image to this texture... */
        command_buffer.blitImage(src_image, vk::ImageLayout::eTransferSrcOptimal, data.colorImage, vk::ImageLayout::eTransferDstOptimal, region, filter);

        /* transition source back VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL  */
        //setImageLayout( command_buffer, src_image, vk::ImageLayout::eTransferSrcOptimal, src_layout, srcSubresourceRange);
            
        /* transition source back VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL  */
        setImageLayout( command_buffer, data.colorImage, vk::ImageLayout::eTransferDstOptimal, data.colorImageLayout, dstSubresourceRange);

        vulkan->end_one_time_graphics_command(command_buffer, pool_id);

        device.destroyImage(src_image);
        device.freeMemory(src_image_memory);
        device.destroyBuffer(stagingBuffer);
        device.freeMemory(stagingBufferMemory);

        return true;
    }

    bool record_blit_to(vk::CommandBuffer command_buffer, std::shared_ptr<Texture> other) {
        auto src_image = data.colorImage;
        auto dst_image = other->get_color_image();

        auto src_layout = data.colorImageLayout;
        auto dst_layout = other->get_color_image_layout();

        /* First, specify the region we'd like to copy */
        vk::ImageSubresourceLayers srcSubresourceLayers;
        srcSubresourceLayers.aspectMask = vk::ImageAspectFlagBits::eColor;
        srcSubresourceLayers.mipLevel = 0;
        srcSubresourceLayers.baseArrayLayer = 0;
        srcSubresourceLayers.layerCount = 1; // TODO

        vk::ImageSubresourceLayers dstSubresourceLayers;
        dstSubresourceLayers.aspectMask = vk::ImageAspectFlagBits::eColor;
        dstSubresourceLayers.mipLevel = 0;
        dstSubresourceLayers.baseArrayLayer = 0;
        dstSubresourceLayers.layerCount = 1; // TODO

        vk::ImageBlit region;
        region.srcSubresource = srcSubresourceLayers;
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {(int32_t)this->data.width, (int32_t)this->data.height, (int32_t)this->data.depth};
        region.dstSubresource = dstSubresourceLayers;
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {(int32_t)other->get_width(), (int32_t)other->get_height(), (int32_t)other->get_depth()};

        /* Next, specify the filter we'd like to use */
        vk::Filter filter = vk::Filter::eLinear;

        /* For layout transitions */
        vk::ImageSubresourceRange srcSubresourceRange;
        srcSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        srcSubresourceRange.baseMipLevel = 0;
        srcSubresourceRange.levelCount = data.colorMipLevels;
        srcSubresourceRange.layerCount = 1;

        vk::ImageSubresourceRange dstSubresourceRange;
        dstSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        dstSubresourceRange.baseMipLevel = 0;
        dstSubresourceRange.levelCount = 1;
        dstSubresourceRange.layerCount = 1;

        /* transition source to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL  */
        setImageLayout( command_buffer, src_image, src_layout, vk::ImageLayout::eTransferSrcOptimal, srcSubresourceRange);
            
        /* transition source to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL  */
        setImageLayout( command_buffer, dst_image, dst_layout, vk::ImageLayout::eTransferDstOptimal, dstSubresourceRange);

        /* Blit the image... */
        command_buffer.blitImage( src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image, vk::ImageLayout::eTransferDstOptimal, region, filter);

        /* transition source back VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL  */
        setImageLayout( command_buffer, src_image, vk::ImageLayout::eTransferSrcOptimal, src_layout, srcSubresourceRange);
            
        /* transition source back VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL  */
        setImageLayout( command_buffer, dst_image, vk::ImageLayout::eTransferDstOptimal, dst_layout, dstSubresourceRange);

        return true;
    }

    void cleanup()
    {
        if (madeExternally) return;

        auto vulkan = Libraries::Vulkan::Get();
        auto device = vulkan->get_device();

        /* Destroy samplers */
        if (data.colorSampler)
            device.destroySampler(data.colorSampler);
        if (data.depthSampler)
            device.destroySampler(data.depthSampler);

        /* Destroy Image Views */
        if (data.colorImageView)
            device.destroyImageView(data.colorImageView);
        if (data.depthImageView)
            device.destroyImageView(data.depthImageView);

        /* Destroy Images */
        if (data.colorImage)
            device.destroyImage(data.colorImage);
        if (data.depthImage)
            device.destroyImage(data.depthImage);

        /* Free Memory */
        if (data.colorImageMemory)
            device.freeMemory(data.colorImageMemory);
        if (data.depthImageMemory)
            device.freeMemory(data.depthImageMemory);
    };

    vk::ImageView get_depth_image_view() { return data.depthImageView; };
    vk::ImageView get_color_image_view() { return data.colorImageView; };
    vk::Sampler get_color_sampler() { return data.colorSampler; };
    vk::Sampler get_depth_sampler() { return data.depthSampler; };
    vk::ImageLayout get_color_image_layout() { return data.colorImageLayout; };
    vk::ImageLayout get_depth_image_layout() { return data.depthImageLayout; };
    vk::Image get_color_image() { return data.colorImage; };
    vk::Image get_depth_image() { return data.depthImage; };
    uint32_t get_color_mip_levels() { return data.colorMipLevels; };
    vk::Format get_color_format() { return data.colorFormat; };
    vk::Format get_depth_format() { return data.depthFormat; };
    uint32_t get_width() { return data.width; }
    uint32_t get_height() { return data.height; }
    uint32_t get_depth() { return data.depth; }
    uint32_t get_total_layers() { return data.layers; }

    std::string to_string()
    {
        std::string output;
        output += "{\n";
        output += "\ttype: \"Texture\",\n";
        output += "\tname: \"" + name + "\",\n";
        output += "\twidth: " + std::to_string(data.width) + "\n";
        output += "\theight: " + std::to_string(data.height) + "\n";
        output += "\tdepth: " + std::to_string(data.depth) + "\n";
        output += "\tlayers: " + std::to_string(data.layers) + "\n";
        output += "\tview_type: " + vk::to_string(data.viewType) + "\n";
        output += "\tcolor_mip_levels: " + std::to_string(data.colorMipLevels) + "\n";
        output += "\thas_color: ";
        output += ((data.colorImage == vk::Image()) ? "false" : "true");
        output += "\n";
        output += "\thas_color_sampler: " ;
        output += ((data.colorSampler == vk::Sampler()) ? "false" : "true");
        output += "\n";
        output += "\tcolor_format: " + vk::to_string(data.colorFormat) + "\n";
        output += "\thas_depth: ";
        output += ((data.depthImage == vk::Image()) ? "false" : "true");
        output += "\n";
        output += "\thas_depth_sampler: " ;
        output += ((data.depthSampler == vk::Sampler()) ? "false" : "true");
        output += "\n";
        output += "\tdepth_format: " + vk::to_string(data.depthFormat) + "\n";
        output += "}";
        return output;
    }

    // Create an image memory barrier for changing the layout of
    // an image and put it into an active command buffer
    // See chapter 11.4 "Image Layout" for details
    void setImageLayout(
        vk::CommandBuffer cmdbuffer,
        vk::Image image,
        vk::ImageLayout oldImageLayout,
        vk::ImageLayout newImageLayout,
        vk::ImageSubresourceRange subresourceRange,
        vk::PipelineStageFlags srcStageMask = vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlags dstStageMask = vk::PipelineStageFlagBits::eAllCommands)
    {
        // Create an image barrier object
        vk::ImageMemoryBarrier imageMemoryBarrier;
        imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageMemoryBarrier.oldLayout = oldImageLayout;
        imageMemoryBarrier.newLayout = newImageLayout;
        imageMemoryBarrier.image = image;
        imageMemoryBarrier.subresourceRange = subresourceRange;

        // Source layouts (old)
        // Source access mask controls actions that have to be finished on the old layout
        // before it will be transitioned to the new layout
        switch (oldImageLayout)
        {
        case vk::ImageLayout::eUndefined:
            // Image layout is undefined (or does not matter)
            // Only valid as initial layout
            // No flags required, listed only for completeness
            imageMemoryBarrier.srcAccessMask = vk::AccessFlags();
            break;

        case vk::ImageLayout::ePreinitialized:
            // Image is preinitialized
            // Only valid as initial layout for linear images, preserves memory contents
            // Make sure host writes have been finished
            imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
            break;

        case vk::ImageLayout::eColorAttachmentOptimal:
            // Image is a color attachment
            // Make sure any writes to the color buffer have been finished
            imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
            break;

        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            // Image is a depth/stencil attachment
            // Make sure any writes to the depth/stencil buffer have been finished
            imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            break;

        case vk::ImageLayout::eTransferSrcOptimal:
            // Image is a transfer source
            // Make sure any reads from the image have been finished
            imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
            break;

        case vk::ImageLayout::eTransferDstOptimal:
            // Image is a transfer destination
            // Make sure any writes to the image have been finished
            imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            break;

        case vk::ImageLayout::eShaderReadOnlyOptimal:
            // Image is read by a shader
            // Make sure any shader reads from the image have been finished
            imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eShaderRead;
            break;
        default:
            // Other source layouts aren't handled (yet)
            break;
        }

        // Target layouts (new)
        // Destination access mask controls the dependency for the new image layout
        switch (newImageLayout)
        {
        case vk::ImageLayout::eTransferDstOptimal:
            // Image will be used as a transfer destination
            // Make sure any writes to the image have been finished
            imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
            break;

        case vk::ImageLayout::eTransferSrcOptimal:
            // Image will be used as a transfer source
            // Make sure any reads from the image have been finished
            imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
            break;

        case vk::ImageLayout::eColorAttachmentOptimal:
            // Image will be used as a color attachment
            // Make sure any writes to the color buffer have been finished
            imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
            break;

        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            // Image layout will be used as a depth/stencil attachment
            // Make sure any writes to depth/stencil buffer have been finished
            imageMemoryBarrier.dstAccessMask = imageMemoryBarrier.dstAccessMask | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            break;

        case vk::ImageLayout::eShaderReadOnlyOptimal:
            // Image will be read in a shader (sampler, input attachment)
            // Make sure any writes to the image have been finished
            if (imageMemoryBarrier.srcAccessMask == vk::AccessFlags())
            {
                imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eTransferWrite;
            }
            imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
            break;
        default:
            // Other source layouts aren't handled (yet)
            break;
        }

        // Put barrier inside setup command buffer
        cmdbuffer.pipelineBarrier(
            srcStageMask,
            dstStageMask,
            vk::DependencyFlags(),
            0, nullptr,
            0, nullptr,
            1, &imageMemoryBarrier);
    }

    // Fixed sub resource on first mip level and layer
    void setImageLayout(
        vk::CommandBuffer cmdbuffer,
        vk::Image image,
        vk::ImageAspectFlags aspectMask,
        vk::ImageLayout oldImageLayout,
        vk::ImageLayout newImageLayout,
        vk::PipelineStageFlags srcStageMask = vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlags dstStageMask = vk::PipelineStageFlagBits::eAllCommands)
    {
        vk::ImageSubresourceRange subresourceRange = {};
        subresourceRange.aspectMask = aspectMask;
        subresourceRange.baseMipLevel = 0;
        subresourceRange.levelCount = 1;
        subresourceRange.layerCount = 1;
        setImageLayout(cmdbuffer, image, oldImageLayout, newImageLayout, subresourceRange, srcStageMask, dstStageMask);
    }

  private:
    static bool does_component_exist(std::string name);

    bool loadKTX(std::string imagePath)
    {
        /* First, check if file exists. */
        struct stat st;
        if (stat(imagePath.c_str(), &st) != 0)
        {
            std::cout << "Texture: Error, image " << imagePath << " does not exist!" << std::endl;
            return false;
        }

        /* Load the texture */
        auto texture = gli::load(imagePath);
        uint32_t textureSize = 0;
        void *textureData = nullptr;

        std::vector<uint32_t> textureMipSizes;
        std::vector<uint32_t> textureMipWidths;
        std::vector<uint32_t> textureMipHeights;
        std::vector<uint32_t> textureMipDepths;

        if (texture.target() == gli::target::TARGET_2D)
        {
            gli::texture2d tex2D(texture);

            if (tex2D.empty())
            {
                std::cout << "Texture: Error, image " << imagePath << " is empty!" << std::endl;
                return false;
            }

            data.width = (uint32_t)(tex2D.extent().x);
            data.height = (uint32_t)(tex2D.extent().y);
            data.depth = 1;

            data.colorMipLevels = (uint32_t)(tex2D.levels());
            data.colorFormat = (vk::Format)tex2D.format();
            data.viewType = vk::ImageViewType::e2D;
            textureSize = (uint32_t)tex2D.size();
            textureData = tex2D.data();

            for (uint32_t i = 0; i < data.colorMipLevels; ++i)
            {
                textureMipSizes.push_back((uint32_t)tex2D[i].size());
                textureMipWidths.push_back(tex2D[i].extent().x);
                textureMipHeights.push_back(tex2D[i].extent().y);
                textureMipDepths.push_back(1);
            }

            data.imageType = vk::ImageType::e2D;
        }
        else if (texture.target() == gli::target::TARGET_3D)
        {
            gli::texture3d tex3D(texture);

            if (tex3D.empty())
            {
                std::cout << "Texture: Error, image " << imagePath << " is empty!" << std::endl;
                return false;
            }

            data.width = (uint32_t)(tex3D.extent().x);
            data.height = (uint32_t)(tex3D.extent().y);
            data.depth = (uint32_t)(tex3D.extent().z);

            data.colorMipLevels = (uint32_t)(tex3D.levels());
            data.colorFormat = (vk::Format)tex3D.format();
            data.viewType = vk::ImageViewType::e3D;
            textureSize = (uint32_t)tex3D.size();
            textureData = tex3D.data();

            for (uint32_t i = 0; i < data.colorMipLevels; ++i)
            {
                textureMipSizes.push_back((uint32_t)tex3D[i].size());
                textureMipWidths.push_back(tex3D[i].extent().x);
                textureMipHeights.push_back(tex3D[i].extent().y);
                textureMipDepths.push_back(tex3D[i].extent().z);
            }
            data.imageType = vk::ImageType::e2D;
        }
        else if (texture.target() == gli::target::TARGET_CUBE)
        {
            gli::texture_cube texCube(texture);

            if (texCube.empty())
            {
                std::cout << "Texture: Error, image " << imagePath << " is empty!" << std::endl;
                return false;
            }

            data.width = (uint32_t)(texCube.extent().x);
            data.height = (uint32_t)(texCube.extent().y);
            data.depth = 1;
            data.viewType = vk::ImageViewType::eCube;

            data.colorMipLevels = (uint32_t)(texCube.levels());
            data.colorFormat = (vk::Format)texCube.format();
            textureSize = (uint32_t)texCube.size();
            textureData = texCube.data();

            for (uint32_t i = 0; i < data.colorMipLevels; ++i)
            {
                textureMipSizes.push_back((uint32_t)texCube[i].size());
                textureMipWidths.push_back(texCube[i].extent().x);
                textureMipHeights.push_back(texCube[i].extent().y);
                textureMipDepths.push_back(1);
            }
            data.imageType = vk::ImageType::e2D;
        }
        else
        {
            std::cout << "Texture: Error, image " << imagePath << " has unsupported target type!" << std::endl;
            return false;
        }

        /* Clean up any existing vulkan stuff */
        cleanup();

        auto vulkan = Libraries::Vulkan::Get();
        auto physicalDevice = vulkan->get_physical_device();
        auto device = vulkan->get_device();

        /* Get device properties for the requested texture format. */
        vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(data.colorFormat);
        if (formatProperties.bufferFeatures == vk::FormatFeatureFlags() && formatProperties.linearTilingFeatures == vk::FormatFeatureFlags() && formatProperties.optimalTilingFeatures == vk::FormatFeatureFlags())
        {
            std::cout << "Unsupported image format for " << imagePath << std::endl;
            return false;
        }

        /* Create staging buffer */
        vk::BufferCreateInfo bufferInfo;
        bufferInfo.size = textureSize;
        bufferInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
        bufferInfo.sharingMode = vk::SharingMode::eExclusive;
        vk::Buffer stagingBuffer = device.createBuffer(bufferInfo);

        vk::MemoryRequirements stagingMemRequirements = device.getBufferMemoryRequirements(stagingBuffer);
        vk::MemoryAllocateInfo stagingAllocInfo;
        stagingAllocInfo.allocationSize = (uint32_t)stagingMemRequirements.size;
        stagingAllocInfo.memoryTypeIndex = vulkan->find_memory_type(stagingMemRequirements.memoryTypeBits,
                                                                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::DeviceMemory stagingBufferMemory = device.allocateMemory(stagingAllocInfo);

        device.bindBufferMemory(stagingBuffer, stagingBufferMemory, 0);

        /* Copy texture data into staging buffer */
        void *dataptr = device.mapMemory(stagingBufferMemory, 0, textureSize, vk::MemoryMapFlags());
        memcpy(dataptr, textureData, textureSize);
        device.unmapMemory(stagingBufferMemory);

        /* Setup buffer copy regions for each mip level */
        std::vector<vk::BufferImageCopy> bufferCopyRegions;
        uint32_t offset = 0;
        for (uint32_t i = 0; i < data.colorMipLevels; i++)
        {
            vk::BufferImageCopy bufferCopyRegion;
            bufferCopyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
            bufferCopyRegion.imageSubresource.mipLevel = i;
            bufferCopyRegion.imageSubresource.baseArrayLayer = 0;
            bufferCopyRegion.imageSubresource.layerCount = 1;
            bufferCopyRegion.imageExtent.width = textureMipWidths[i];
            bufferCopyRegion.imageExtent.height = textureMipHeights[i];
            bufferCopyRegion.imageExtent.depth = textureMipDepths[i];
            bufferCopyRegion.bufferOffset = offset;
            bufferCopyRegions.push_back(bufferCopyRegion);
            offset += textureMipSizes[i];
        }

        /* Create optimal tiled target image */
        vk::ImageCreateInfo imageCreateInfo;
        imageCreateInfo.imageType = data.imageType;
        imageCreateInfo.format = data.colorFormat;
        imageCreateInfo.mipLevels = data.colorMipLevels;
        imageCreateInfo.arrayLayers = 1; // .......
        imageCreateInfo.samples = vk::SampleCountFlagBits::e1;
        imageCreateInfo.tiling = vk::ImageTiling::eOptimal;
        imageCreateInfo.sharingMode = vk::SharingMode::eExclusive;
        imageCreateInfo.initialLayout = vk::ImageLayout::eUndefined;
        imageCreateInfo.extent = {data.width, data.height, data.depth};
        imageCreateInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
        data.colorImage = device.createImage(imageCreateInfo);

        /* Allocate and bind memory for the texture */
        vk::MemoryRequirements imageMemReqs = device.getImageMemoryRequirements(data.colorImage);
        vk::MemoryAllocateInfo imageAllocInfo;
        imageAllocInfo.allocationSize = imageMemReqs.size;
        imageAllocInfo.memoryTypeIndex = vulkan->find_memory_type(
            imageMemReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        data.colorImageMemory = device.allocateMemory(imageAllocInfo);

        device.bindImageMemory(data.colorImage, data.colorImageMemory, 0);

        /* Create a command buffer for changing layouts and copying */
        vk::CommandBuffer copyCmd = vulkan->begin_one_time_graphics_command(1);

        /* Transition between formats */
        vk::ImageSubresourceRange subresourceRange;
        subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        subresourceRange.baseMipLevel = 0;
        subresourceRange.levelCount = data.colorMipLevels;
        subresourceRange.layerCount = 1; //.............

        /* Transition to transfer destination optimal */
        setImageLayout(
            copyCmd,
            data.colorImage,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,
            subresourceRange);

        /* Copy mip levels from staging buffer */
        copyCmd.copyBufferToImage(
            stagingBuffer,
            data.colorImage,
            vk::ImageLayout::eTransferDstOptimal,
            (uint32_t)bufferCopyRegions.size(),
            bufferCopyRegions.data());

        /* Transition to shader read optimal */
        data.colorImageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        setImageLayout(
            copyCmd,
            data.colorImage,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            subresourceRange);

        vulkan->end_one_time_graphics_command(copyCmd, 1);

        /* Clean up staging resources */
        device.destroyBuffer(stagingBuffer);
        device.freeMemory(stagingBufferMemory);

        return true;
    }

    bool create_color_image_resources()
    {
        auto vulkan = Libraries::Vulkan::Get();
        auto device = vulkan->get_device();
        auto physicalDevice = vulkan->get_physical_device();

        /* Destroy samplers */
        if (data.colorSampler)
            device.destroySampler(data.colorSampler);
        
        /* Destroy Image Views */
        if (data.colorImageView)
            device.destroyImageView(data.colorImageView);
        
        /* Destroy Images */
        if (data.colorImage)
            device.destroyImage(data.colorImage);
        
        /* Free Memory */
        if (data.colorImageMemory)
            device.freeMemory(data.colorImageMemory);

        /* For now, assume the following format: */
        data.colorFormat = vk::Format::eR8G8B8A8Srgb;
        
        data.colorImageLayout = vk::ImageLayout::eUndefined;

        vk::ImageCreateInfo imageInfo;
        imageInfo.imageType = vk::ImageType::e2D;
        imageInfo.format = data.colorFormat;
        imageInfo.extent.width = data.width;
        imageInfo.extent.height = data.height;
        imageInfo.extent.depth = data.depth;
        imageInfo.mipLevels = data.colorMipLevels;
        imageInfo.arrayLayers = data.layers;
        imageInfo.samples = vk::SampleCountFlagBits::e1;
        imageInfo.tiling = vk::ImageTiling::eOptimal;
        imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
        imageInfo.initialLayout = data.colorImageLayout;
        if (data.viewType == vk::ImageViewType::eCube) {
            imageInfo.flags = vk::ImageCreateFlagBits::eCubeCompatible;
        }
        data.colorImage = device.createImage(imageInfo);

        vk::MemoryRequirements memReqs = device.getImageMemoryRequirements(data.colorImage);
        vk::MemoryAllocateInfo memAlloc;
        memAlloc.allocationSize = memReqs.size;
        memAlloc.memoryTypeIndex = vulkan->find_memory_type(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        data.colorImageMemory = device.allocateMemory(memAlloc);
        device.bindImageMemory(data.colorImage, data.colorImageMemory, 0);

        /* Transition to a usable format */
        vk::ImageSubresourceRange subresourceRange;
        subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        subresourceRange.baseMipLevel = 0;
        subresourceRange.levelCount = 1;
        subresourceRange.layerCount = 1;

        vk::CommandBuffer cmdBuffer = vulkan->begin_one_time_graphics_command(1);
        setImageLayout( cmdBuffer, data.colorImage, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal, subresourceRange);
        data.colorImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        vulkan->end_one_time_graphics_command(cmdBuffer, 1);

        /* Create the image view */
        vk::ImageViewCreateInfo vInfo;
        vInfo.viewType = data.viewType;
        vInfo.format = data.colorFormat;
        vInfo.subresourceRange = subresourceRange;
        vInfo.image = data.colorImage;
        data.colorImageView = device.createImageView(vInfo);

        /* Create a sampler to sample from the attachment in the fragment shader */
        vk::SamplerCreateInfo sInfo;
        sInfo.magFilter = vk::Filter::eNearest;
        sInfo.minFilter = vk::Filter::eNearest;
        sInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        sInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        sInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        sInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        sInfo.mipLodBias = 0.0;
        sInfo.maxAnisotropy = 1.0;
        sInfo.minLod = 0.0;
        sInfo.maxLod = 1.0;
        sInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
        data.colorSampler = device.createSampler(sInfo);
        return true;
    }

    bool create_depth_stencil_resources()
    {
        auto vulkan = Libraries::Vulkan::Get();
        auto device = vulkan->get_device();
        auto physicalDevice = vulkan->get_physical_device();

        /* Destroy samplers */
        if (data.depthSampler)
            device.destroySampler(data.depthSampler);

        /* Destroy Image Views */
        if (data.depthImageView)
            device.destroyImageView(data.depthImageView);

        /* Destroy Images */
        if (data.depthImage)
            device.destroyImage(data.depthImage);

        /* Free Memory */
        if (data.depthImageMemory)
            device.freeMemory(data.depthImageMemory);

        bool result = get_supported_depth_format(physicalDevice, &data.depthFormat);
        if (!result) {
            std::cout<<"Error, unable to find suitable depth format"<<std::endl;
            return false;
        }

        data.depthImageLayout = vk::ImageLayout::eUndefined;;
        
        vk::ImageCreateInfo imageInfo;
        imageInfo.imageType = vk::ImageType::e2D;
        imageInfo.format = data.depthFormat;
        imageInfo.extent.width = data.width;
        imageInfo.extent.height = data.height;
        imageInfo.extent.depth = data.depth;
        imageInfo.mipLevels = 1; // Doesn't make much sense for a depth map to be mipped... Could change later...
        imageInfo.arrayLayers = data.layers;
        imageInfo.samples = vk::SampleCountFlagBits::e1;
        imageInfo.tiling = vk::ImageTiling::eOptimal;
        imageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
        imageInfo.initialLayout = data.depthImageLayout;
        if (data.viewType == vk::ImageViewType::eCube) {
            imageInfo.flags = vk::ImageCreateFlagBits::eCubeCompatible;
        }
        data.depthImage = device.createImage(imageInfo);

        vk::MemoryRequirements memReqs = device.getImageMemoryRequirements(data.depthImage);
        vk::MemoryAllocateInfo memAlloc;
        memAlloc.allocationSize = memReqs.size;
        memAlloc.memoryTypeIndex = vulkan->find_memory_type(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        data.depthImageMemory = device.allocateMemory(memAlloc);
        device.bindImageMemory(data.depthImage, data.depthImageMemory, 0);

        /* Transition to a usable format */
        vk::ImageSubresourceRange subresourceRange;
        subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        subresourceRange.baseMipLevel = 0;
        subresourceRange.levelCount = 1;
        subresourceRange.layerCount = 1;

        vk::CommandBuffer cmdBuffer = vulkan->begin_one_time_graphics_command(1);
        setImageLayout( cmdBuffer, data.depthImage, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal, subresourceRange);
        data.depthImageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        vulkan->end_one_time_graphics_command(cmdBuffer, 1);

        /* Create the image view */
        vk::ImageViewCreateInfo vInfo;
        vInfo.viewType = data.viewType;
        vInfo.format = data.depthFormat;
        vInfo.subresourceRange = subresourceRange;
        vInfo.image = data.depthImage;
        data.depthImageView = device.createImageView(vInfo);

        /* Create a sampler to sample from the attachment in the fragment shader */
        vk::SamplerCreateInfo sInfo;
        sInfo.magFilter = vk::Filter::eNearest;
        sInfo.minFilter = vk::Filter::eNearest;
        sInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        sInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        sInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        sInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        sInfo.mipLodBias = 0.0;
        sInfo.maxAnisotropy = 1.0;
        sInfo.minLod = 0.0;
        sInfo.maxLod = 1.0;
        sInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
        data.depthSampler = device.createSampler(sInfo);
        return true;
    }

    

    void createColorImageView()
    {
        auto vulkan = Libraries::Vulkan::Get();
        auto device = vulkan->get_device();

        // Create image view
        vk::ImageViewCreateInfo info;
        // Cube map view type
        info.viewType = data.viewType;
        info.format = data.colorFormat;
        info.components = {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA};
        info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        // 6 array layers (faces)
        info.subresourceRange.layerCount = data.layers;
        // Set number of mip levels
        info.subresourceRange.levelCount = data.colorMipLevels;
        info.image = data.colorImage;
        data.colorImageView = device.createImageView(info);
    }

    bool get_supported_depth_format(vk::PhysicalDevice physicalDevice, vk::Format *depthFormat)
    {
        // Since all depth formats may be optional, we need to find a suitable depth format to use
        // Start with the highest precision packed format
        std::vector<vk::Format> depthFormats = {
            vk::Format::eD32SfloatS8Uint,
            vk::Format::eD32Sfloat,
            vk::Format::eD24UnormS8Uint,
            vk::Format::eD16UnormS8Uint,
            vk::Format::eD16Unorm
        };

        for (auto &format : depthFormats)
        {
            vk::FormatProperties formatProps = physicalDevice.getFormatProperties(format);
            // Format must support depth stencil attachment for optimal tiling
            if (formatProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment)
            {
                *depthFormat = format;
                return true;
            }
        }

        return false;
    }

  	static std::map<std::string, std::shared_ptr<Texture>> Texture::textures;
  protected:
    std::string name;
    Data data;
    bool madeExternally = false;
};