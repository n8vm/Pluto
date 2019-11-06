// #pragma optimize("", off)

#include "./Texture.hxx"
#include "Foton/Tools/Options.hxx"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <sys/types.h>
#include <sys/stat.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <gli/gli.hpp>

Texture Texture::textures[MAX_TEXTURES];
vk::Sampler Texture::samplers[MAX_SAMPLERS];
std::map<std::string, uint32_t> Texture::lookupTable;
TextureStruct* Texture::pinnedMemory;
vk::Buffer Texture::SSBO;
vk::DeviceMemory Texture::SSBOMemory;
vk::Buffer Texture::stagingSSBO;
vk::DeviceMemory Texture::stagingSSBOMemory;
std::shared_ptr<std::mutex> Texture::creation_mutex;
bool Texture::Initialized = false;
bool Texture::Dirty = true;

Texture::Texture()
{
	initialized = false;
}

Texture::Texture(std::string name, uint32_t id)
{
	initialized = true;
	this->name = name;
	this->id = id;
	texture_struct.type = 0;
	texture_struct.scale = .1f;
	texture_struct.mip_levels = 0;
	texture_struct.color1 = glm::vec4(1.0, 1.0, 1.0, 1.0);
	texture_struct.color2 = glm::vec4(0.0, 0.0, 0.0, 1.0);
}

std::string Texture::to_string()
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
	// output += "\tcolor_mip_levels: " + std::to_string(data.colorBuffer.mipLevels) + "\n";
	output += "\thas_color: ";
	// output += ((data.colorBuffer.image == vk::Image()) ? "false" : "true");
	output += "\n";
	// output += "\tcolor_sampler_id: " + std::to_string(.sata.colorSamplerId) + "\n";
	// output += "\tcolor_for.sat: " + vk::to_string(data.colorBuffer.format) + "\n";
	output += "\thas_depth: ";
	// output += ((data.depthBuffer.image == vk::Image()) ? "false" : "true");
	output += "\n";
	// output += "\tdepth_sampler_id: " + std::to_string(data.depthSamplerId) + "\n";
	// output += "\tdepth_format: " + vk::to_string(data.depthBuffer.format) + "\n";
	output += "}";
	return output;
}

std::vector<float> Texture::download_color_data(uint32_t width, uint32_t height, uint32_t depth, bool submit_immediately)
{
	/* I'm assuming an image was already loaded for now */
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));
	auto physicalDevice = vulkan->get_physical_device();
	if (physicalDevice == vk::PhysicalDevice())
		throw std::runtime_error( std::string("Invalid vulkan physical device"));

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
	// Note this is the format we want to blit the image to. 
	imInfo.format = vk::Format::eR32G32B32A32Sfloat; 
	imInfo.extent = vk::Extent3D{width, height, depth};
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
	region.srcOffsets[0] = vk::Offset3D{0, 0, 0};
	region.srcOffsets[1] = vk::Offset3D{(int32_t)this->data.width, (int32_t)this->data.height, (int32_t)this->data.depth};
	region.dstSubresource = dstSubresourceLayers;
	region.dstOffsets[0] = vk::Offset3D{0, 0, 0};
	region.dstOffsets[1] = vk::Offset3D{(int32_t)width, (int32_t)height, (int32_t)depth};

	/* Next, specify the filter we'd like to use */
	vk::Filter filter = vk::Filter::eLinear;

	/* Now, create a command buffer */
	vk::CommandBuffer cmdBuffer = vulkan->begin_one_time_graphics_command();

	/* Transition destination image to transfer destination optimal */
	vk::ImageSubresourceRange dstSubresourceRange;
	dstSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	dstSubresourceRange.baseMipLevel = 0;
	dstSubresourceRange.levelCount = 1;
	dstSubresourceRange.layerCount = 1; // TODO

	vk::ImageSubresourceRange srcSubresourceRange;
	srcSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	srcSubresourceRange.baseMipLevel = 0;
	srcSubresourceRange.levelCount = data.colorBuffer.mipLevels;
	srcSubresourceRange.layerCount = 1; //.............

	setImageLayout(
		cmdBuffer,
		blitImage,
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eTransferDstOptimal,
		dstSubresourceRange);

	/* Transition source to shader read optimal */
	auto originalLayout = data.colorBuffer.imageLayout;
	setImageLayout(
		cmdBuffer,
		data.colorBuffer.image,
		data.colorBuffer.imageLayout,
		vk::ImageLayout::eTransferSrcOptimal,
		srcSubresourceRange);
	data.colorBuffer.imageLayout = vk::ImageLayout::eTransferSrcOptimal;

	/* Blit the image... */
	cmdBuffer.blitImage(
		data.colorBuffer.image,
		data.colorBuffer.imageLayout,
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
	imgCopyRegion.imageOffset = vk::Offset3D{0, 0, 0};
	imgCopyRegion.imageExtent = vk::Extent3D{(uint32_t)width, (uint32_t)height, (uint32_t)depth};

	cmdBuffer.copyImageToBuffer(blitImage, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer, imgCopyRegion);

	/* Transition source back to previous layout */
	setImageLayout(
		cmdBuffer,
		data.colorBuffer.image,
		data.colorBuffer.imageLayout,
		originalLayout,
		srcSubresourceRange);
	data.colorBuffer.imageLayout = originalLayout;

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(cmdBuffer, "download color data", true);
	else
		vulkan->end_one_time_graphics_command(cmdBuffer, "download color data", true);

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

void Texture::setData(Data data)
{
	this->madeExternally = true;
	this->data = data;
}

void Texture::set_procedural_color_1(float r, float g, float b, float a)
{
	if (texture_struct.type == 0)
		throw std::runtime_error("Error: texture must be procedural");
	texture_struct.color1 = glm::vec4(r, g, b, a);
}

void Texture::set_procedural_color_2(float r, float g, float b, float a)
{
	if (texture_struct.type == 0)
		throw std::runtime_error("Error: texture must be procedural");
	texture_struct.color2 = glm::vec4(r, g, b, a);
}

void Texture::set_procedural_scale(float scale)
{
	if (texture_struct.type == 0)
		throw std::runtime_error("Error: texture must be procedural");
	texture_struct.scale = scale;
}

void Texture::upload_color_data(uint32_t width, uint32_t height, uint32_t depth, std::vector<float> color_data, bool submit_immediately)
{
	/* I'm assuming an image was already loaded for now */
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));
	auto physicalDevice = vulkan->get_physical_device();
	if (physicalDevice == vk::PhysicalDevice())
		throw std::runtime_error( std::string("Invalid vulkan physical device"));

	uint32_t textureSize = width * height * depth * 4 * sizeof(float);
	if (color_data.size() < width * height * depth)
		throw std::runtime_error( std::string("Not enough data for provided image dimensions"));


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
	imageCreateInfo.extent = vk::Extent3D{width, height, depth};
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

	vk::CommandBuffer command_buffer = vulkan->begin_one_time_graphics_command();

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
	dstSubresourceRange.levelCount = data.colorBuffer.mipLevels;
	dstSubresourceRange.layerCount = 1;

	/* First, copy the staging buffer into our temporary source image. */
	setImageLayout(command_buffer, src_image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, srcSubresourceRange);

	/* Copy mip levels from staging buffer */
	command_buffer.copyBufferToImage(stagingBuffer, src_image, vk::ImageLayout::eTransferDstOptimal, 1, &bufferCopyRegion);

	/* Region to copy (Possibly multiple in the future) */
	vk::ImageBlit region;
	region.dstSubresource = dstSubresourceLayers;
	region.dstOffsets[0] = vk::Offset3D{0, 0, 0};
	region.dstOffsets[1] = vk::Offset3D{(int32_t)this->data.width, (int32_t)this->data.height, (int32_t)this->data.depth};
	region.srcSubresource = srcSubresourceLayers;
	region.srcOffsets[0] = vk::Offset3D{0, 0, 0};
	region.srcOffsets[1] = vk::Offset3D{(int32_t)width, (int32_t)height, (int32_t)depth};

	/* Next, specify the filter we'd like to use */
	vk::Filter filter = vk::Filter::eLinear;

	/* transition source to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL  */
	setImageLayout(command_buffer, src_image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal, srcSubresourceRange);

	/* transition source to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL  */
	setImageLayout(command_buffer, data.colorBuffer.image, data.colorBuffer.imageLayout, vk::ImageLayout::eTransferDstOptimal, dstSubresourceRange);

	/* Blit the uploaded image to this texture... */
	command_buffer.blitImage(src_image, vk::ImageLayout::eTransferSrcOptimal, data.colorBuffer.image, vk::ImageLayout::eTransferDstOptimal, region, filter);

	/* transition source back VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL  */
	//setImageLayout( command_buffer, src_image, vk::ImageLayout::eTransferSrcOptimal, src_layout, srcSubresourceRange);

	/* transition source back VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL  */
	setImageLayout(command_buffer, data.colorBuffer.image, vk::ImageLayout::eTransferDstOptimal, data.colorBuffer.imageLayout, dstSubresourceRange);

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(command_buffer, "upload color data", true);
	else
		vulkan->end_one_time_graphics_command(command_buffer, "upload color data", true);

	device.destroyImage(src_image);
	device.freeMemory(src_image_memory);
	device.destroyBuffer(stagingBuffer);
	device.freeMemory(stagingBufferMemory);
}

void Texture::record_blit_to(vk::CommandBuffer command_buffer, Texture * other, uint32_t layer)
{
	if (!other)
		throw std::runtime_error( std::string("Invalid target texture"));

	auto src_image = data.colorBuffer.image;
	auto dst_image = other->get_color_image();

	auto src_layout = data.colorBuffer.imageLayout;
	auto dst_layout = other->get_color_image_layout();

	/* First, specify the region we'd like to copy */
	vk::ImageSubresourceLayers srcSubresourceLayers;
	srcSubresourceLayers.aspectMask = vk::ImageAspectFlagBits::eColor;
	srcSubresourceLayers.mipLevel = 0;
	srcSubresourceLayers.baseArrayLayer = layer;
	srcSubresourceLayers.layerCount = 1; // TODO

	vk::ImageSubresourceLayers dstSubresourceLayers;
	dstSubresourceLayers.aspectMask = vk::ImageAspectFlagBits::eColor;
	dstSubresourceLayers.mipLevel = 0;
	dstSubresourceLayers.baseArrayLayer = 0;
	dstSubresourceLayers.layerCount = 1; // TODO

	vk::ImageBlit region;
	region.srcSubresource = srcSubresourceLayers;
	region.srcOffsets[0] = vk::Offset3D{0, 0, 0};
	region.srcOffsets[1] = vk::Offset3D{(int32_t)this->data.width, (int32_t)this->data.height, (int32_t)this->data.depth};
	region.dstSubresource = dstSubresourceLayers;
	region.dstOffsets[0] = vk::Offset3D{0, 0, 0};
	region.dstOffsets[1] = vk::Offset3D{(int32_t)other->get_width(), (int32_t)other->get_height(), (int32_t)other->get_depth()};

	/* Next, specify the filter we'd like to use */
	vk::Filter filter = vk::Filter::eNearest;

	/* For layout transitions */
	vk::ImageSubresourceRange srcSubresourceRange;
	srcSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	srcSubresourceRange.baseMipLevel = 0;
	srcSubresourceRange.baseArrayLayer = 0;
	srcSubresourceRange.levelCount = data.colorBuffer.mipLevels;
	srcSubresourceRange.layerCount = data.layers;

	vk::ImageSubresourceRange dstSubresourceRange;
	dstSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	dstSubresourceRange.baseMipLevel = 0;
	srcSubresourceRange.baseArrayLayer = 0;
	dstSubresourceRange.levelCount = 1;
	dstSubresourceRange.layerCount = 1;

	/* transition source to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL  */
	setImageLayout(command_buffer, src_image, src_layout, vk::ImageLayout::eTransferSrcOptimal, srcSubresourceRange);

	/* transition source to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL  */
	setImageLayout(command_buffer, dst_image, dst_layout, vk::ImageLayout::eTransferDstOptimal, dstSubresourceRange);

	/* Blit the image... */
	command_buffer.blitImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image, vk::ImageLayout::eTransferDstOptimal, region, filter);

	/* transition source back VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL  */
	setImageLayout(command_buffer, src_image, vk::ImageLayout::eTransferSrcOptimal, src_layout, srcSubresourceRange);

	/* transition source back VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL  */
	setImageLayout(command_buffer, dst_image, vk::ImageLayout::eTransferDstOptimal, dst_layout, dstSubresourceRange);
}

// TODO
void Texture::Initialize()
{
	if (IsInitialized()) return;

	creation_mutex = std::make_shared<std::mutex>();

	std::string resource_path = Options::GetResourcePath();
	CreateFromKTX("DefaultTex2D", resource_path + "/Defaults/missing-texture.ktx");
	CreateFromKTX("DefaultTexCube", resource_path + "/Defaults/missing-texcube.ktx");
	CreateFromKTX("DefaultTex3D", resource_path + "/Defaults/missing-volume.ktx");

	// Create the default texture here
	std::thread t([](){
		std::string resource_path = Options::GetResourcePath();
		CreateFromKTX("BRDF", resource_path + "/Defaults/brdf-lut.ktx");
		CreateFromKTX("LTCMAT", resource_path + "/Defaults/ltc_mat.ktx");
		CreateFromKTX("LTCAMP", resource_path + "/Defaults/ltc_amp.ktx");
		CreateFromKTX("RANKINGTILE", resource_path + "/Defaults/rankingTile_128_128_8.ktx");
		CreateFromKTX("SCRAMBLETILE", resource_path + "/Defaults/scrambleTile_128_128_8.ktx");
		CreateFromKTX("SOBELTILE", resource_path + "/Defaults/sobelTile.ktx");
		CreateFromKTX("BLUENOISETILE", resource_path + "/Defaults/BlueNoise.ktx");
	});

    t.detach();
	// fatal error here if result is nullptr...

	auto vulkan = Libraries::Vulkan::Get();
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));

	auto physical_device = vulkan->get_physical_device();
	if (physical_device == vk::PhysicalDevice())
		throw std::runtime_error( std::string("Invalid vulkan physical device"));

	{
		vk::BufferCreateInfo bufferInfo = {};
		bufferInfo.size = MAX_TEXTURES * sizeof(TextureStruct);
		bufferInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
		bufferInfo.sharingMode = vk::SharingMode::eExclusive;
		stagingSSBO = device.createBuffer(bufferInfo);

		vk::MemoryRequirements memReqs = device.getBufferMemoryRequirements(stagingSSBO);
		vk::MemoryAllocateInfo allocInfo = {};
		allocInfo.allocationSize = memReqs.size;

		vk::PhysicalDeviceMemoryProperties memProperties = physical_device.getMemoryProperties();
		vk::MemoryPropertyFlags properties = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
		allocInfo.memoryTypeIndex = vulkan->find_memory_type(memReqs.memoryTypeBits, properties);

		stagingSSBOMemory = device.allocateMemory(allocInfo);
		device.bindBufferMemory(stagingSSBO, stagingSSBOMemory, 0);
	}

	{
		vk::BufferCreateInfo bufferInfo = {};
		bufferInfo.size = MAX_TEXTURES * sizeof(TextureStruct);
		bufferInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst ;
		bufferInfo.sharingMode = vk::SharingMode::eExclusive;
		SSBO = device.createBuffer(bufferInfo);

		vk::MemoryRequirements memReqs = device.getBufferMemoryRequirements(SSBO);
		vk::MemoryAllocateInfo allocInfo = {};
		allocInfo.allocationSize = memReqs.size;

		vk::PhysicalDeviceMemoryProperties memProperties = physical_device.getMemoryProperties();
		vk::MemoryPropertyFlags properties = vk::MemoryPropertyFlagBits::eDeviceLocal;
		allocInfo.memoryTypeIndex = vulkan->find_memory_type(memReqs.memoryTypeBits, properties);

		SSBOMemory = device.allocateMemory(allocInfo);
		device.bindBufferMemory(SSBO, SSBOMemory, 0);
	}

	/* Create a sampler to sample from the attachment in the fragment shader */
	vk::SamplerCreateInfo linearSamplerInfo;
	linearSamplerInfo.magFilter = vk::Filter::eLinear;
	linearSamplerInfo.minFilter = vk::Filter::eLinear;
	linearSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
	linearSamplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
	linearSamplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
	linearSamplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
	linearSamplerInfo.mipLodBias = 0.0;
	linearSamplerInfo.maxAnisotropy = vulkan->get_max_anisotropy();
	linearSamplerInfo.anisotropyEnable = (linearSamplerInfo.maxAnisotropy > 0.0) ? VK_TRUE : VK_FALSE;
	linearSamplerInfo.minLod = 0.0;
	linearSamplerInfo.maxLod = 8.0;
	linearSamplerInfo.borderColor = vk::BorderColor::eFloatTransparentBlack;
	samplers[0] = device.createSampler(linearSamplerInfo);


	vk::SamplerCreateInfo nearestSamplerInfo;
	nearestSamplerInfo.magFilter = vk::Filter::eNearest;
	nearestSamplerInfo.minFilter = vk::Filter::eNearest;
	nearestSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
	nearestSamplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	nearestSamplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	nearestSamplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	nearestSamplerInfo.mipLodBias = 0.0;
	nearestSamplerInfo.maxAnisotropy = 0.0;
	nearestSamplerInfo.anisotropyEnable = VK_FALSE;
	nearestSamplerInfo.minLod = 0.0;
	nearestSamplerInfo.maxLod = 0.0;
	nearestSamplerInfo.borderColor = vk::BorderColor::eFloatTransparentBlack;
	samplers[1] = device.createSampler(nearestSamplerInfo);

	Initialized = true;
}

bool Texture::IsInitialized()
{
	return Initialized;
}

bool Texture::is_ready()
{
	return ready;
}

void Texture::UploadSSBO(vk::CommandBuffer command_buffer)
{
	if (!Dirty) return;
	Dirty = false;
	auto vulkan = Libraries::Vulkan::Get();
	auto device = vulkan->get_device();

	if (SSBOMemory == vk::DeviceMemory()) return;
	if (stagingSSBOMemory == vk::DeviceMemory()) return;

	auto bufferSize = MAX_TEXTURES * sizeof(TextureStruct);

	/* Pin the buffer */
	pinnedMemory = (TextureStruct*) device.mapMemory(stagingSSBOMemory, 0, bufferSize);

	if (pinnedMemory == nullptr) return;
	TextureStruct texture_structs[MAX_TEXTURES];
	
	/* TODO: remove this for loop */
	for (int i = 0; i < MAX_TEXTURES; ++i) {
		if (!textures[i].is_initialized()) continue;
		texture_structs[i] = textures[i].texture_struct;
	};

	/* Copy to GPU mapped memory */
	memcpy(pinnedMemory, texture_structs, sizeof(texture_structs));

	device.unmapMemory(stagingSSBOMemory);

	vk::BufferCopy copyRegion;
	copyRegion.size = bufferSize;
	command_buffer.copyBuffer(stagingSSBO, SSBO, copyRegion);
}

vk::Buffer Texture::GetSSBO()
{
	if ((SSBO != vk::Buffer()) && (SSBOMemory != vk::DeviceMemory()))
		return SSBO;
	else return vk::Buffer();
}

uint32_t Texture::GetSSBOSize()
{
	return MAX_TEXTURES * sizeof(TextureStruct);
}

void Texture::CleanUp()
{
	if (!IsInitialized()) return;

	for (auto &texture : textures) {
		if (texture.initialized) {
			texture.cleanup();
			Texture::Delete(texture.id);
		}
	}
	
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));

	for (int i = 0; i < MAX_SAMPLERS; ++i) {
		if (samplers[i] != vk::Sampler()) {
			device.destroySampler(samplers[i]);
		}
	}

	device.destroyBuffer(SSBO);
	device.freeMemory(SSBOMemory);

	device.destroyBuffer(stagingSSBO);
	device.freeMemory(stagingSSBOMemory);

	SSBO = vk::Buffer();
	SSBOMemory = vk::DeviceMemory();
	stagingSSBO = vk::Buffer();
	stagingSSBOMemory = vk::DeviceMemory();
}

std::vector<vk::ImageView> Texture::GetImageViews(vk::ImageViewType view_type) 
{
	// Get the default texture
	Texture *DefaultTex;
	try {
		if (view_type == vk::ImageViewType::e2D) DefaultTex = Get("DefaultTex2D");
		else if (view_type == vk::ImageViewType::e3D) DefaultTex = Get("DefaultTex3D");
		else if (view_type == vk::ImageViewType::eCube) DefaultTex = Get("DefaultTexCube");
		else return {};
	} catch (...) {
		return {};
	}

	std::vector<vk::ImageView> image_views(MAX_TEXTURES);

	// For each texture
	for (int i = 0; i < MAX_TEXTURES; ++i) {
		if (textures[i].initialized 
			&& (textures[i].data.colorBuffer.imageView != vk::ImageView())
			&& (textures[i].data.colorBuffer.imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal) 
			&& (textures[i].data.viewType == view_type)) {
			// then add it's image view to the vector
			image_views[i] = textures[i].data.colorBuffer.imageView;
		}
		// otherwise, add the default texture image view
		else {
			image_views[i] = DefaultTex->data.colorBuffer.imageView;
		}
	}
	
	// finally, return the image view vector
	return image_views;
}

vk::ImageView Texture::get_depth_image_view() { return data.depthBuffer.imageView; };
vk::ImageView Texture::get_color_image_view() { return data.colorBuffer.imageView; };
vk::ImageView Texture::get_g_buffer_image_view(uint32_t index) { return data.gBuffers[index].imageView; };
std::vector<vk::ImageView> Texture::get_depth_image_view_layers() { return data.depthBuffer.imageViewLayers; };
std::vector<vk::ImageView> Texture::get_color_image_view_layers() { return data.colorBuffer.imageViewLayers; };
std::vector<vk::ImageView> Texture::get_g_buffer_image_view_layers(uint32_t index) { return data.gBuffers[index].imageViewLayers; };
vk::Sampler Texture::get_color_sampler() { return samplers[data.colorBuffer.samplerId]; };
vk::Sampler Texture::get_depth_sampler() { return samplers[data.depthBuffer.samplerId]; };
vk::Sampler Texture::get_g_buffer_sampler(uint32_t index) { return samplers[data.gBuffers[index].samplerId]; };
vk::ImageLayout Texture::get_color_image_layout() { return data.colorBuffer.imageLayout; };
vk::ImageLayout Texture::get_depth_image_layout() { return data.depthBuffer.imageLayout; };
vk::ImageLayout Texture::get_g_buffer_image_layout(uint32_t index) { return data.gBuffers[index].imageLayout; };
vk::Image Texture::get_color_image() { return data.colorBuffer.image; };
vk::Image Texture::get_depth_image() { return data.depthBuffer.image; };
vk::Image Texture::get_g_buffer_image(uint32_t index) { return data.gBuffers[index].image; };
uint32_t Texture::get_color_mip_levels() { return data.colorBuffer.mipLevels; };
vk::Format Texture::get_color_format() { return data.colorBuffer.format; };
vk::Format Texture::get_depth_format() { return data.depthBuffer.format; };
vk::Format Texture::get_g_buffer_format(uint32_t index) { return data.gBuffers[index].format; };
uint32_t Texture::get_width() { return data.width; }
uint32_t Texture::get_height() { return data.height; }
uint32_t Texture::get_depth() { return data.depth; }
uint32_t Texture::get_total_layers() { return data.layers; }
vk::SampleCountFlagBits Texture::get_sample_count() { return data.sampleCount; }
void Texture::setImageLayout(
		vk::CommandBuffer cmdbuffer,
		vk::Image image,
		vk::ImageLayout oldImageLayout,
		vk::ImageLayout newImageLayout,
		vk::ImageSubresourceRange subresourceRange,
		vk::PipelineStageFlags srcStageMask,
		vk::PipelineStageFlags dstStageMask)
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

void Texture::overrideColorImageLayout(vk::ImageLayout layout) {
	data.colorBuffer.imageLayout = layout;
}

void Texture::overrideDepthImageLayout(vk::ImageLayout layout) {
	data.depthBuffer.imageLayout = layout;
}

void Texture::loadKTX(std::string imagePath, bool submit_immediately)
{
	/* First, check if file exists. */
	struct stat st;
	if (stat(imagePath.c_str(), &st) != 0)
		throw std::runtime_error( std::string("Error: image " + imagePath + " does not exist!"));

	/* Load the texture */
	auto texture = gli::load(imagePath);
	uint32_t textureSize = 0;
	void *textureData = nullptr;

	gli::texture_cube texCube;
	gli::texture2d tex2D;
	gli::texture3d tex3D;

	std::vector<uint32_t> textureMipSizes;
	std::vector<uint32_t> textureMipWidths;
	std::vector<uint32_t> textureMipHeights;
	std::vector<uint32_t> textureMipDepths;

	if (texture.target() == gli::target::TARGET_2D)
	{
		tex2D = gli::texture2d(texture);

		if (tex2D.empty())
			throw std::runtime_error( std::string("Error: image " + imagePath + " is empty"));

		data.width = (uint32_t)(tex2D.extent().x);
		data.height = (uint32_t)(tex2D.extent().y);
		data.depth = 1;

		data.colorBuffer.mipLevels = (uint32_t)(tex2D.levels());
		data.colorBuffer.format = (vk::Format)tex2D.format();
		data.viewType = vk::ImageViewType::e2D;
		textureSize = (uint32_t)tex2D.size();
		textureData = tex2D.data();

		for (uint32_t i = 0; i < data.colorBuffer.mipLevels; ++i)
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
		tex3D = gli::texture3d(texture);

		if (tex3D.empty())
			throw std::runtime_error( std::string("Error: image " + imagePath + " is empty"));

		data.width = (uint32_t)(tex3D.extent().x);
		data.height = (uint32_t)(tex3D.extent().y);
		data.depth = (uint32_t)(tex3D.extent().z);

		data.colorBuffer.mipLevels = (uint32_t)(tex3D.levels());
		data.colorBuffer.format = (vk::Format)tex3D.format();
		data.viewType = vk::ImageViewType::e3D;
		textureSize = (uint32_t)tex3D.size();
		textureData = tex3D.data();

		for (uint32_t i = 0; i < data.colorBuffer.mipLevels; ++i)
		{
			textureMipSizes.push_back((uint32_t)tex3D[i].size());
			textureMipWidths.push_back(tex3D[i].extent().x);
			textureMipHeights.push_back(tex3D[i].extent().y);
			textureMipDepths.push_back(tex3D[i].extent().z);
		}
		data.imageType = vk::ImageType::e3D;
	}
	else if (texture.target() == gli::target::TARGET_CUBE)
	{
		texCube = gli::texture_cube(texture);

		if (texCube.empty())
			throw std::runtime_error( std::string("Error: image " + imagePath + " is empty"));

		data.width = (uint32_t)(texCube.extent().x);
		data.height = (uint32_t)(texCube.extent().y);
		data.depth = 1;
		data.viewType = vk::ImageViewType::eCube;
		data.layers = 6;

		data.colorBuffer.mipLevels = (uint32_t)(texCube.levels());
		data.colorBuffer.format = (vk::Format)texCube.format();
		textureSize = (uint32_t)texCube.size();
		textureData = texCube.data();

		for (int face = 0; face < 6; ++face) {
			for (uint32_t i = 0; i < data.colorBuffer.mipLevels; ++i)
			{
				// Assuming all faces have the same extent...
				textureMipSizes.push_back((uint32_t)texCube[face][i].size());
				textureMipWidths.push_back(texCube[face][i].extent().x);
				textureMipHeights.push_back(texCube[face][i].extent().y);
				textureMipDepths.push_back(1);
			}
		}
		
		data.imageType = vk::ImageType::e2D;
	}
	else
		throw std::runtime_error( std::string("Error: image " + imagePath + " uses an unsupported target type. "));

	/* Clean up any existing vulkan stuff */
	cleanup();

	texture_struct.mip_levels = data.colorBuffer.mipLevels;

	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));
	auto physicalDevice = vulkan->get_physical_device();
	if (physicalDevice == vk::PhysicalDevice())
		throw std::runtime_error( std::string("Invalid vulkan physical device"));

	/* Get device properties for the requested texture format. */
	vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(data.colorBuffer.format);

	if (formatProperties.bufferFeatures == vk::FormatFeatureFlags() 
		&& formatProperties.linearTilingFeatures == vk::FormatFeatureFlags() 
		&& formatProperties.optimalTilingFeatures == vk::FormatFeatureFlags())
		throw std::runtime_error( std::string("Error: Unsupported image format used in " + imagePath));

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

	if (data.viewType == vk::ImageViewType::eCube) {
		for (uint32_t face = 0; face < 6; ++face) {
			for (uint32_t level = 0; level < data.colorBuffer.mipLevels; level++) {
				vk::BufferImageCopy bufferCopyRegion;
				bufferCopyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
				bufferCopyRegion.imageSubresource.mipLevel = level;
				bufferCopyRegion.imageSubresource.baseArrayLayer = face;
				bufferCopyRegion.imageSubresource.layerCount = 1;                
				bufferCopyRegion.imageExtent.width = texCube[face][level].extent().x;
				bufferCopyRegion.imageExtent.height = texCube[face][level].extent().y;
				bufferCopyRegion.imageExtent.depth = 1;
				bufferCopyRegion.bufferOffset = offset;
				bufferCopyRegions.push_back(bufferCopyRegion);
				offset += (uint32_t)texCube[face][level].size();
			}
		}
	}
	else {
		for (uint32_t i = 0; i < data.colorBuffer.mipLevels; i++)
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
	}

	/* Create optimal tiled target image */
	vk::ImageCreateInfo imageCreateInfo;
	imageCreateInfo.imageType = data.imageType;
	imageCreateInfo.format = data.colorBuffer.format;
	imageCreateInfo.mipLevels = data.colorBuffer.mipLevels;
	imageCreateInfo.arrayLayers = data.layers;
	imageCreateInfo.samples = vk::SampleCountFlagBits::e1;
	imageCreateInfo.tiling = vk::ImageTiling::eOptimal;
	imageCreateInfo.sharingMode = vk::SharingMode::eExclusive;
	imageCreateInfo.initialLayout = vk::ImageLayout::eUndefined;
	imageCreateInfo.extent = vk::Extent3D{data.width, data.height, data.depth};
	imageCreateInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
	if (data.viewType == vk::ImageViewType::eCube) {
		imageCreateInfo.flags |= vk::ImageCreateFlagBits::eCubeCompatible;
	}
	data.colorBuffer.image = device.createImage(imageCreateInfo);

	/* Allocate and bind memory for the texture */
	vk::MemoryRequirements imageMemReqs = device.getImageMemoryRequirements(data.colorBuffer.image);
	vk::MemoryAllocateInfo imageAllocInfo;
	imageAllocInfo.allocationSize = imageMemReqs.size;
	imageAllocInfo.memoryTypeIndex = vulkan->find_memory_type(
		imageMemReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
	data.colorBuffer.imageMemory = device.allocateMemory(imageAllocInfo);

	device.bindImageMemory(data.colorBuffer.image, data.colorBuffer.imageMemory, 0);

	/* Create a command buffer for changing layouts and copying */
	vk::CommandBuffer copyCmd = vulkan->begin_one_time_graphics_command();

	/* Transition between formats */
	vk::ImageSubresourceRange subresourceRange;
	subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	subresourceRange.baseMipLevel = 0;
	subresourceRange.levelCount = data.colorBuffer.mipLevels;
	subresourceRange.layerCount = data.layers;

	/* Transition to transfer destination optimal */
	setImageLayout(
		copyCmd,
		data.colorBuffer.image,
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eTransferDstOptimal,
		subresourceRange);

	/* Copy mip levels from staging buffer */
	copyCmd.copyBufferToImage(
		stagingBuffer,
		data.colorBuffer.image,
		vk::ImageLayout::eTransferDstOptimal,
		(uint32_t)bufferCopyRegions.size(),
		bufferCopyRegions.data());

	/* Transition to shader read optimal */
	data.colorBuffer.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	setImageLayout(
		copyCmd,
		data.colorBuffer.image,
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		subresourceRange);

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(copyCmd, "transition new ktx image", true);
	else
		vulkan->end_one_time_graphics_command(copyCmd, "transition new ktx image", true);

	/* Clean up staging resources */
	device.destroyBuffer(stagingBuffer);
	device.freeMemory(stagingBufferMemory);
	
	/* Create the image view */
	vk::ImageViewCreateInfo vInfo;
	vInfo.viewType = data.viewType;
	vInfo.format = data.colorBuffer.format;
	vInfo.subresourceRange = subresourceRange;
	vInfo.image = data.colorBuffer.image;
	data.colorBuffer.imageView = device.createImageView(vInfo);

	ready = true;
	mark_dirty();
}


void Texture::loadPNG(std::string imagePath, bool convert_bump, bool submit_immediately)
{
	/* First, check if file exists. */
	struct stat st;
	if (stat(imagePath.c_str(), &st) != 0)
		throw std::runtime_error( std::string("Error: image " + imagePath + " does not exist!"));

	/* Verify Vulkan is ready */
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));
	auto physicalDevice = vulkan->get_physical_device();
	if (physicalDevice == vk::PhysicalDevice())
		throw std::runtime_error( std::string("Invalid vulkan physical device"));

	/* Load the texture */
	uint32_t textureSize = 0;
	void *textureData = nullptr;
	int texWidth, texHeight, texChannels;
	stbi_set_flip_vertically_on_load(true);
	stbi_uc* pixels = stbi_load(imagePath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
	if (!pixels) { throw std::runtime_error("failed to load texture image!"); }

	textureData = (void*) pixels;

	textureSize = texWidth * texHeight * 4;
	data.width = texWidth;
	data.height = texHeight;
	data.depth = 1;
	data.colorBuffer.mipLevels = 1;
	data.layers = 1;
	data.viewType = vk::ImageViewType::e2D;
	data.imageType = vk::ImageType::e2D;
	/* For PNG, we assume the following format */
	data.colorBuffer.format = vk::Format::eR8G8B8A8Unorm;

	if (convert_bump) {
		float strength = 2.0f;
		std::vector<uint32_t> texture_copy(texWidth * texHeight); 
		memcpy(texture_copy.data(), textureData, textureSize);

		for (int y = 0; y < texHeight; ++y)
		{
			for (int x = 0; x < texWidth; ++x)
			{
				glm::vec3 n;
				int x_change = 0;
				if (x == 0) x_change = 1;
				if (x == texWidth - 1) x_change = -1;

				int y_change = 0;
				if (y == 0) y_change = 1;
				if (y == texHeight - 1) y_change = -1;

				int x1 = x + 1 + x_change;
				int x2 = x + x_change;
				int x3 = x - 1 + x_change;

				int y1 = y + 1 + y_change;
				int y2 = y + y_change;
				int y3 = y - 1 + y_change;

				float tr = (texture_copy[y1 * texWidth + x1 ] & 255) / 255.f;
				float r = (texture_copy[y2 * texWidth + x1] & 255) / 255.f;
				float br = (texture_copy[y3 * texWidth + x1] & 255) / 255.f;

				float tl = (texture_copy[y1 * texWidth + x3] & 255) / 255.f;
				float l = (texture_copy[y2 * texWidth + x3] & 255) / 255.f;
				float bl = (texture_copy[y3 * texWidth + x3] & 255) / 255.f;

				float b = (texture_copy[y3 * texWidth + x2] & 255) / 255.f;
				float t = (texture_copy[y1 * texWidth + x2] & 255) / 255.f;

				// Compute dx using Sobel:
				//           -1 0 1 
				//           -2 0 2
				//           -1 0 1
				float dX = tr + 2 * r + br - tl - 2 * l - bl;

				// Compute dy using Sobel:
				//           -1 -2 -1 
				//            0  0  0
				//            1  2  1
				float dY = bl + 2 * b + br - tl - 2 * t - tr;

				n = vec3(dX, dY, 1.0f / strength);
				n = glm::normalize(n);
				n = n * .5f;
				n += .5f;
				((uint32_t*)textureData)[y * texWidth + x] = ((uint32_t)(n.x * 255) << 0) | ((uint32_t)(n.y * 255) << 8) | ((uint32_t)(n.z * 255) << 16) | (255 << 24);
			}
		}
	}

	std::vector<uint32_t> textureMipSizes;
	std::vector<uint32_t> textureMipWidths;
	std::vector<uint32_t> textureMipHeights;
	std::vector<uint32_t> textureMipDepths;

	/* TODO: Generate mipmaps */
	textureMipSizes.push_back(textureSize);
	textureMipWidths.push_back(texWidth);
	textureMipHeights.push_back(texHeight);
	textureMipDepths.push_back(1);

	/* Get device properties for the requested texture format. */
	vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(data.colorBuffer.format);

	if (formatProperties.bufferFeatures == vk::FormatFeatureFlags() 
		&& formatProperties.linearTilingFeatures == vk::FormatFeatureFlags() 
		&& formatProperties.optimalTilingFeatures == vk::FormatFeatureFlags())
		throw std::runtime_error( std::string("Error: Unsupported image format used in " + imagePath));

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

	for (uint32_t i = 0; i < data.colorBuffer.mipLevels; i++)
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
	imageCreateInfo.format = data.colorBuffer.format;
	imageCreateInfo.mipLevels = data.colorBuffer.mipLevels;
	imageCreateInfo.arrayLayers = data.layers;
	imageCreateInfo.samples = vk::SampleCountFlagBits::e1;
	imageCreateInfo.tiling = vk::ImageTiling::eOptimal;
	imageCreateInfo.sharingMode = vk::SharingMode::eExclusive;
	imageCreateInfo.initialLayout = vk::ImageLayout::eUndefined;
	imageCreateInfo.extent = vk::Extent3D{data.width, data.height, data.depth};
	imageCreateInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
	data.colorBuffer.image = device.createImage(imageCreateInfo);


	/* Allocate and bind memory for the texture */
	vk::MemoryRequirements imageMemReqs = device.getImageMemoryRequirements(data.colorBuffer.image);
	vk::MemoryAllocateInfo imageAllocInfo;
	imageAllocInfo.allocationSize = imageMemReqs.size;
	imageAllocInfo.memoryTypeIndex = vulkan->find_memory_type(
		imageMemReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
	data.colorBuffer.imageMemory = device.allocateMemory(imageAllocInfo);
	device.bindImageMemory(data.colorBuffer.image, data.colorBuffer.imageMemory, 0);

	/* Create a command buffer for changing layouts and copying */
	vk::CommandBuffer copyCmd = vulkan->begin_one_time_graphics_command();

	/* Transition between formats */
	vk::ImageSubresourceRange subresourceRange;
	subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	subresourceRange.baseMipLevel = 0;
	subresourceRange.levelCount = data.colorBuffer.mipLevels;
	subresourceRange.layerCount = data.layers;

	/* Transition to transfer destination optimal */
	setImageLayout(
		copyCmd,
		data.colorBuffer.image,
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eTransferDstOptimal,
		subresourceRange);

	/* Copy mip levels from staging buffer */
	copyCmd.copyBufferToImage(
		stagingBuffer,
		data.colorBuffer.image,
		vk::ImageLayout::eTransferDstOptimal,
		(uint32_t)bufferCopyRegions.size(),
		bufferCopyRegions.data());

	/* Transition to shader read optimal */
	data.colorBuffer.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	setImageLayout(
		copyCmd,
		data.colorBuffer.image,
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		subresourceRange);

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(copyCmd, "transition new png image", true);
	else
		vulkan->end_one_time_graphics_command(copyCmd, "transition new png image", true);

	/* Clean up staging resources */
	device.destroyBuffer(stagingBuffer);
	device.freeMemory(stagingBufferMemory);
	
	/* Create the image view */
	vk::ImageViewCreateInfo vInfo;
	vInfo.viewType = data.viewType;
	vInfo.format = data.colorBuffer.format;
	vInfo.subresourceRange = subresourceRange;
	vInfo.image = data.colorBuffer.image;
	data.colorBuffer.imageView = device.createImageView(vInfo);

	/* Clean up original image array */
	stbi_image_free(pixels);

	ready = true;
}

void Texture::create_color_image_resources(ImageData &imageData, bool submit_immediately, bool attachment_optimal)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));
	auto physicalDevice = vulkan->get_physical_device();
	if (physicalDevice == vk::PhysicalDevice())
		throw std::runtime_error( std::string("Invalid vulkan physical device"));

	
	// /* Destroy samplers */
	// if (data.colorSampler)
	//     device.destroySampler(data.colorSampler);

	/* Destroy Image Views */
	if (imageData.imageView)
		device.destroyImageView(imageData.imageView);

	/* Destroy Images */
	if (imageData.image)
		device.destroyImage(imageData.image);

	/* Free Memory */
	if (imageData.imageMemory)
		device.freeMemory(imageData.imageMemory);

	/* For now, assume the following format: */
	imageData.format = vk::Format::eR16G16B16A16Sfloat;

	imageData.imageLayout = vk::ImageLayout::eUndefined;

	vk::ImageCreateInfo imageInfo;
	imageInfo.imageType = data.imageType;
	imageInfo.format = imageData.format;
	imageInfo.extent.width = data.width;
	imageInfo.extent.height = data.height;
	imageInfo.extent.depth = data.depth;
	imageInfo.mipLevels = imageData.mipLevels;
	imageInfo.arrayLayers = data.layers;
	imageInfo.samples = data.sampleCount;
	imageInfo.tiling = vk::ImageTiling::eOptimal;
	imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage;
	imageInfo.initialLayout = imageData.imageLayout;
	if (data.viewType == vk::ImageViewType::eCube)
	{
		imageInfo.flags = vk::ImageCreateFlagBits::eCubeCompatible;
	}
	imageData.image = device.createImage(imageInfo);

	vk::MemoryRequirements memReqs = device.getImageMemoryRequirements(imageData.image);
	vk::MemoryAllocateInfo memAlloc;
	memAlloc.allocationSize = memReqs.size;
	memAlloc.memoryTypeIndex = vulkan->find_memory_type(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
	imageData.imageMemory = device.allocateMemory(memAlloc);
	device.bindImageMemory(imageData.image, imageData.imageMemory, 0);

	/* Transition to a usable format */
	vk::ImageSubresourceRange subresourceRange;
	subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	subresourceRange.baseMipLevel = 0;
	subresourceRange.baseArrayLayer = 0;
	subresourceRange.levelCount = imageData.mipLevels;
	subresourceRange.layerCount = data.layers;

	vk::CommandBuffer cmdBuffer = vulkan->begin_one_time_graphics_command();

	if (attachment_optimal) {
		setImageLayout(cmdBuffer, imageData.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal, subresourceRange);
		imageData.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	} else {
		setImageLayout(cmdBuffer, imageData.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal, subresourceRange);
		imageData.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(cmdBuffer, "transition new color image", true);
	else
		vulkan->end_one_time_graphics_command(cmdBuffer, "transition new color image", true);

	/* Create the image view */
	vk::ImageViewCreateInfo vInfo;
	vInfo.viewType = data.viewType;
	vInfo.format = imageData.format;
	vInfo.subresourceRange = subresourceRange;
	vInfo.image = imageData.image;
	imageData.imageView = device.createImageView(vInfo);
	
	/* Create more image views */
	imageData.imageViewLayers.clear();
	for(uint32_t i = 0; i < data.layers; i++) {
		subresourceRange.baseMipLevel = 0;
		subresourceRange.baseArrayLayer = i;
		subresourceRange.levelCount = imageData.mipLevels;
		subresourceRange.layerCount = 1;
		
		vInfo.viewType = vk::ImageViewType::e2D;
		vInfo.format = imageData.format;
		vInfo.subresourceRange = subresourceRange;
		vInfo.image = imageData.image;
		imageData.imageViewLayers.push_back(device.createImageView(vInfo));
	}
}

void Texture::create_depth_stencil_resources(ImageData &imageData, bool submit_immediately)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));
	auto physicalDevice = vulkan->get_physical_device();
	if (physicalDevice == vk::PhysicalDevice())
		throw std::runtime_error( std::string("Invalid vulkan physical device"));

	// /* Destroy samplers */
	// if (data.depthSampler)
	//     device.destroySampler(data.depthSampler);

	/* Destroy Image Views */
	if (imageData.imageView)
		device.destroyImageView(imageData.imageView);

	/* Destroy Images */
	if (imageData.image)
		device.destroyImage(imageData.image);

	/* Free Memory */
	if (imageData.imageMemory)
		device.freeMemory(imageData.imageMemory);

	bool result = get_supported_depth_format(physicalDevice, &imageData.format);
	if (!result)
		throw std::runtime_error( std::string("Error: Unable to find a suitable depth format"));

	imageData.imageLayout = vk::ImageLayout::eUndefined;
	
	vk::ImageCreateInfo imageInfo;
	imageInfo.imageType = vk::ImageType::e2D;
	imageInfo.format = imageData.format;
	imageInfo.extent.width = data.width;
	imageInfo.extent.height = data.height;
	imageInfo.extent.depth = data.depth;
	imageInfo.mipLevels = 1; // Doesn't make much sense for a depth map to be mipped... Could change later...
	imageInfo.arrayLayers = data.layers;
	imageInfo.samples = data.sampleCount;
	imageInfo.tiling = vk::ImageTiling::eOptimal;
	imageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
	imageInfo.initialLayout = imageData.imageLayout;
	if (data.viewType == vk::ImageViewType::eCube)
	{
		imageInfo.flags = vk::ImageCreateFlagBits::eCubeCompatible;
	}
	imageData.image = device.createImage(imageInfo);

	vk::MemoryRequirements memReqs = device.getImageMemoryRequirements(imageData.image);
	vk::MemoryAllocateInfo memAlloc;
	memAlloc.allocationSize = memReqs.size;
	memAlloc.memoryTypeIndex = vulkan->find_memory_type(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
	imageData.imageMemory = device.allocateMemory(memAlloc);
	device.bindImageMemory(imageData.image, imageData.imageMemory, 0);

	/* Transition to a usable format */
	vk::ImageSubresourceRange subresourceRange;
	subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
	subresourceRange.baseMipLevel = 0;
	subresourceRange.baseArrayLayer = 0;
	subresourceRange.levelCount = 1;
	subresourceRange.layerCount = data.layers;

	vk::CommandBuffer cmdBuffer = vulkan->begin_one_time_graphics_command();
	setImageLayout(cmdBuffer, imageData.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal, subresourceRange);
	imageData.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(cmdBuffer, "transition new depth image", true);
	else
		vulkan->end_one_time_graphics_command(cmdBuffer, "transition new depth image", true);

	/* Create the image view */
	vk::ImageViewCreateInfo vInfo;
	vInfo.viewType = data.viewType;
	vInfo.format = imageData.format;
	vInfo.subresourceRange = subresourceRange;
	vInfo.image = imageData.image;
	imageData.imageView = device.createImageView(vInfo);
	
	/* Create more image views */
	imageData.imageViewLayers.clear();
	for(uint32_t i = 0; i < data.layers; i++) {
		subresourceRange.baseMipLevel = 0;
		subresourceRange.baseArrayLayer = i;
		subresourceRange.levelCount = 1;
		subresourceRange.layerCount = 1;
		
		vInfo.viewType = vk::ImageViewType::e2D;
		vInfo.format = imageData.format;
		vInfo.subresourceRange = subresourceRange;
		vInfo.image = imageData.image;
		imageData.imageViewLayers.push_back(device.createImageView(vInfo));
	}
}

void Texture::createColorImageView()
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));

	// Create image view
	vk::ImageViewCreateInfo info;
	// Cube map view type
	info.viewType = data.viewType;
	info.format = data.colorBuffer.format;
	info.components = {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA};
	info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	// 6 array layers (faces)
	info.subresourceRange.layerCount = data.layers;
	// Set number of mip levels
	info.subresourceRange.levelCount = data.colorBuffer.mipLevels;
	info.image = data.colorBuffer.image;
	data.colorBuffer.imageView = device.createImageView(info);
}

bool Texture::get_supported_depth_format(vk::PhysicalDevice physicalDevice, vk::Format *depthFormat)
{
	// Since all depth formats may be optional, we need to find a suitable depth format to use
	// Start with the highest precision packed format
	
	std::vector<vk::Format> depthFormats = {
		vk::Format::eD32SfloatS8Uint,
		vk::Format::eD32Sfloat,
		vk::Format::eD24UnormS8Uint,
		vk::Format::eD16UnormS8Uint,
		vk::Format::eD16Unorm};

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

void Texture::cleanup()
{
	if (madeExternally)
		return;

	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));

	/* Destroy samplers */
	// if (data.colorSampler)
	//     device.destroySampler(data.colorSampler);
	// if (data.depthSampler)
	//     device.destroySampler(data.depthSampler);

	/* Destroy Image Views */
	if (data.colorBuffer.imageView)
		device.destroyImageView(data.colorBuffer.imageView);
	if (data.depthBuffer.imageView)
		device.destroyImageView(data.depthBuffer.imageView);

	/* Destroy Images */
	if (data.colorBuffer.image)
		device.destroyImage(data.colorBuffer.image);
	if (data.depthBuffer.image)
		device.destroyImage(data.depthBuffer.image);

	/* Free Memory */
	if (data.colorBuffer.imageMemory)
		device.freeMemory(data.colorBuffer.imageMemory);
	if (data.depthBuffer.imageMemory)
		device.freeMemory(data.depthBuffer.imageMemory);
}

std::vector<vk::Sampler> Texture::GetSamplers() 
{
	// Get the default texture (for now, just use the default 2D texture)
	Texture* DefaultTex;
	try {
		DefaultTex = Get("DefaultTex2D");
	} catch (...) {
		return {};
	}
	
	std::vector<vk::Sampler> samplers_(MAX_SAMPLERS);

	// For each texture
	for (int i = 0; i < MAX_SAMPLERS; ++i) {
		if (samplers[i] == vk::Sampler())
			samplers_[i] = samplers[0]; //  Sampler 0 is always defined. (this might change)
		else
			samplers_[i] = samplers[i];
	}
	
	// finally, return the sampler vector
	return samplers_;
}

std::vector<vk::ImageLayout> Texture::GetLayouts(vk::ImageViewType view_type) 
{
	// Get the default texture
	Texture *DefaultTex;
	try {
		if (view_type == vk::ImageViewType::e2D) DefaultTex = Get("DefaultTex2D");
		else if (view_type == vk::ImageViewType::e3D) DefaultTex = Get("DefaultTex3D");
		else if (view_type == vk::ImageViewType::eCube) DefaultTex = Get("DefaultTexCube");
		else return {};
	} catch (...) {
		return {};
	}

	std::vector<vk::ImageLayout> layouts(MAX_TEXTURES);

	// For each texture
	for (int i = 0; i < MAX_TEXTURES; ++i) {
		if (textures[i].initialized 
			&& (textures[i].data.colorBuffer.imageView != vk::ImageView())
			&& (textures[i].data.colorBuffer.imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal) 
			&& (textures[i].data.viewType == view_type)) {
			// then add it's layout to the vector
			layouts[i] = textures[i].data.colorBuffer.imageLayout;
		}
		// otherwise, add the default 2D texture layout
		else {
			layouts[i] = DefaultTex->data.colorBuffer.imageLayout;
		}
	}
	
	// finally, return the layout vector
	return layouts;
}

/* Static Factory Implementations */
Texture *Texture::CreateFromKTX(std::string name, std::string filepath, bool submit_immediately)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	try {
		tex->loadKTX(filepath, submit_immediately);
		tex->texture_struct.sampler_id = 0;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture *Texture::CreateFromPNG(std::string name, std::string filepath, bool submit_immediately)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	try {
		tex->loadPNG(filepath, false, submit_immediately);
		tex->texture_struct.sampler_id = 0;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture *Texture::CreateFromBumpPNG(std::string name, std::string filepath, bool submit_immediately)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	try {
		tex->loadPNG(filepath, true, submit_immediately);
		tex->texture_struct.sampler_id = 0;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture* Texture::CreateCubemap(
	std::string name, uint32_t width, uint32_t height, bool hasColor, bool hasDepth, bool submit_immediately) 
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	try {		
		tex->data.width = width;
		tex->data.height = height;
		tex->data.layers = 6;
		tex->data.imageType = vk::ImageType::e2D;
		tex->data.viewType  = vk::ImageViewType::eCube;
		if (hasColor) tex->create_color_image_resources(tex->data.colorBuffer, submit_immediately);
		if (hasDepth) tex->create_depth_stencil_resources(tex->data.depthBuffer, submit_immediately);
		tex->texture_struct.sampler_id = 0;
		tex->ready = true;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture* Texture::CreateCubemapGBuffers(
	std::string name, uint32_t width, uint32_t height, uint32_t sampleCount, bool submit_immediately) 
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	try {
		auto vulkan = Libraries::Vulkan::Get();
		if (!vulkan->is_initialized())
			throw std::runtime_error( std::string("Vulkan library is not initialized"));
		auto sampleFlag = vulkan->highest(vulkan->min(vulkan->get_closest_sample_count_flag(sampleCount), vulkan->get_msaa_sample_flags()));

		tex->data.width = width;
		tex->data.height = height;
		tex->data.layers = 6;
		tex->data.imageType = vk::ImageType::e2D;
		tex->data.viewType  = vk::ImageViewType::eCube;
		tex->data.sampleCount = vulkan->highest(vulkan->min(sampleFlag, vulkan->get_msaa_sample_flags()));

		if (tex->data.sampleCount != sampleFlag)
			std::cout<<"Warning: provided sample count is larger than max supported sample count on the device. Using " 
				<< vk::to_string(tex->data.sampleCount) << " instead."<<std::endl;

		tex->create_color_image_resources(tex->data.colorBuffer, submit_immediately, false);
		for (uint32_t i = 0; i < tex->data.gBuffers.size(); ++i) {
			tex->create_color_image_resources(tex->data.gBuffers[i], submit_immediately, false); 
		}
		tex->create_depth_stencil_resources(tex->data.depthBuffer, submit_immediately);
		tex->texture_struct.sampler_id = 0;
		tex->ready = true;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture* Texture::CreateChecker(std::string name, bool submit_immediately)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);

	tex->texture_struct.type = 1;
	tex->texture_struct.mip_levels = 0;
	tex->texture_struct.sampler_id = 0;
	tex->ready = true;
	tex->mark_dirty();
	return tex;
}

Texture* Texture::Create2D(
	std::string name, uint32_t width, uint32_t height, 
	bool hasColor, bool hasDepth, uint32_t sampleCount, uint32_t layers, 
	bool submit_immediately)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	
	try {
		auto vulkan = Libraries::Vulkan::Get();
		if (!vulkan->is_initialized())
			throw std::runtime_error( std::string("Vulkan library is not initialized"));
		auto sampleFlag = vulkan->highest(vulkan->min(vulkan->get_closest_sample_count_flag(sampleCount), vulkan->get_msaa_sample_flags()));

		tex->data.width = width;
		tex->data.height = height;
		tex->data.layers = layers;
		tex->data.viewType  = vk::ImageViewType::e2D;
		tex->data.imageType = vk::ImageType::e2D;
		tex->data.sampleCount = vulkan->highest(vulkan->min(sampleFlag, vulkan->get_msaa_sample_flags()));

		if (tex->data.sampleCount != sampleFlag)
			std::cout<<"Warning: provided sample count is larger than max supported sample count on the device. Using " 
				<< vk::to_string(tex->data.sampleCount) << " instead."<<std::endl;
		if (hasColor) tex->create_color_image_resources(tex->data.colorBuffer, submit_immediately, false); 
		if (hasDepth) tex->create_depth_stencil_resources(tex->data.depthBuffer, submit_immediately);
		tex->ready = true;
		tex->texture_struct.sampler_id = 0;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture* Texture::Create2DGBuffers (
	std::string name, uint32_t width, uint32_t height, uint32_t sampleCount, uint32_t layers, bool submit_immediately
)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	
	try {
		auto vulkan = Libraries::Vulkan::Get();
		if (!vulkan->is_initialized()) throw std::runtime_error( std::string("Vulkan library is not initialized"));
		auto sampleFlag = vulkan->highest(vulkan->min(vulkan->get_closest_sample_count_flag(sampleCount), vulkan->get_msaa_sample_flags()));

		tex->data.width = width;
		tex->data.height = height;
		tex->data.layers = layers;
		tex->data.viewType  = vk::ImageViewType::e2D;
		tex->data.imageType = vk::ImageType::e2D;
		tex->data.sampleCount = vulkan->highest(vulkan->min(sampleFlag, vulkan->get_msaa_sample_flags()));

		if (tex->data.sampleCount != sampleFlag)
			std::cout<<"Warning: provided sample count is larger than max supported sample count on the device. Using " 
				<< vk::to_string(tex->data.sampleCount) << " instead."<<std::endl;

		tex->create_color_image_resources(tex->data.colorBuffer, submit_immediately, false); 
		for (uint32_t i = 0; i < tex->data.gBuffers.size(); ++i) {
			tex->create_color_image_resources(tex->data.gBuffers[i], submit_immediately, false); 
		}
		tex->create_depth_stencil_resources(tex->data.depthBuffer, submit_immediately);

		tex->texture_struct.sampler_id = 0;
		tex->ready = true;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture* Texture::Create3D (
	std::string name, uint32_t width, uint32_t height, uint32_t depth, 
	uint32_t layers, bool submit_immediately)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	
	try {
		auto vulkan = Libraries::Vulkan::Get();
		if (!vulkan->is_initialized())
			throw std::runtime_error( std::string("Vulkan library is not initialized"));
		
		tex->data.width = width;
		tex->data.height = height;
		tex->data.depth = depth;
		tex->data.layers = layers;
		tex->data.viewType  = vk::ImageViewType::e3D;
		tex->data.imageType = vk::ImageType::e3D;
		tex->data.sampleCount = vk::SampleCountFlagBits::e1;

		tex->create_color_image_resources(tex->data.colorBuffer, submit_immediately, false);

		tex->texture_struct.sampler_id = 0;
		tex->ready = true;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture* Texture::Create2DFromColorData (
	std::string name, uint32_t width, uint32_t height, std::vector<float> data, bool submit_immediately)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	
	try {
		tex->data.width = width;
		tex->data.height = height;
		tex->data.layers = 1;
		tex->data.viewType  = vk::ImageViewType::e2D;
		tex->data.imageType = vk::ImageType::e2D;
		tex->create_color_image_resources(tex->data.colorBuffer, submit_immediately);
		tex->upload_color_data(width, height, 1, data);

		tex->texture_struct.sampler_id = 0;
		tex->ready = true;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

Texture* Texture::CreateFromExternalData(std::string name, Data data)
{
	auto tex = StaticFactory::Create(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	
	try {
		tex->setData(data);
		tex->texture_struct.sampler_id = 0;
		tex->ready = true;
		tex->mark_dirty();
		return tex;
	} catch (...) {
		StaticFactory::DeleteIfExists(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
		throw;
	}
}

bool Texture::DoesItemExist(std::string name) {
	return StaticFactory::DoesItemExist(lookupTable, name);
}

Texture* Texture::Get(std::string name) {
	auto tex = StaticFactory::Get(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	if (!tex->is_ready()) throw std::runtime_error("Error, texture not yet ready for use");
	return tex;
}

Texture* Texture::Get(uint32_t id) {
	auto tex = StaticFactory::Get(creation_mutex, id, "Texture", lookupTable, textures, MAX_TEXTURES);
	if (!tex->is_ready()) throw std::runtime_error("Error, texture not yet ready for use");
	return tex;
}

void Texture::Delete(std::string name) {
	StaticFactory::Delete(creation_mutex, name, "Texture", lookupTable, textures, MAX_TEXTURES);
	Dirty = true;
}

void Texture::Delete(uint32_t id) {
	StaticFactory::Delete(creation_mutex, id, "Texture", lookupTable, textures, MAX_TEXTURES);
	Dirty = true;
}

Texture* Texture::GetFront() {
	return textures;
}

uint32_t Texture::GetCount() {
	return MAX_TEXTURES;
}

void Texture::make_renderable(vk::CommandBuffer commandBuffer, vk::PipelineStageFlags srcStageMask, vk::PipelineStageFlags dstStageMask)
{
	if (this->data.colorBuffer.imageLayout != vk::ImageLayout::eColorAttachmentOptimal)
	{
		/* Transition destination image to transfer destination optimal */
		vk::ImageSubresourceRange subresourceRange;
		subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = this->data.colorBuffer.mipLevels;
		subresourceRange.layerCount = this->data.layers;

		setImageLayout(
			commandBuffer,
			this->data.colorBuffer.image,
			this->data.colorBuffer.imageLayout,
			vk::ImageLayout::eColorAttachmentOptimal,
			subresourceRange, srcStageMask, dstStageMask);
		this->data.colorBuffer.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	}

	for (uint32_t i = 0; i < this->data.gBuffers.size(); ++i) {
		if (this->data.gBuffers[i].imageLayout != vk::ImageLayout::eColorAttachmentOptimal)
		{
			/* Transition destination image to transfer destination optimal */
			vk::ImageSubresourceRange subresourceRange;
			subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.levelCount = this->data.gBuffers[i].mipLevels;
			subresourceRange.layerCount = this->data.layers;

			setImageLayout(
				commandBuffer,
				this->data.gBuffers[i].image,
				this->data.gBuffers[i].imageLayout,
				vk::ImageLayout::eColorAttachmentOptimal,
				subresourceRange, srcStageMask, dstStageMask);
			this->data.gBuffers[i].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		}
	}

	if (this->data.depthBuffer.imageLayout != vk::ImageLayout::eDepthStencilAttachmentOptimal)
	{
		/* Transition destination image to transfer destination optimal */
		vk::ImageSubresourceRange subresourceRange;
		subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = 1;
		subresourceRange.layerCount = this->data.layers;

		setImageLayout(
			commandBuffer,
			this->data.depthBuffer.image,
			this->data.depthBuffer.imageLayout,
			vk::ImageLayout::eDepthStencilAttachmentOptimal,
			subresourceRange, srcStageMask, dstStageMask);
		this->data.depthBuffer.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	}
}

void Texture::make_samplable(vk::CommandBuffer commandBuffer, 
	vk::PipelineStageFlags srcStageMask,
	vk::PipelineStageFlags dstStageMask)
{
	if (this->data.colorBuffer.imageLayout != vk::ImageLayout::eShaderReadOnlyOptimal) {
		/* Transition destination image to transfer destination optimal */
		vk::ImageSubresourceRange subresourceRange;
		subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = this->data.colorBuffer.mipLevels;
		subresourceRange.layerCount = this->data.layers;

		setImageLayout(
			commandBuffer,
			this->data.colorBuffer.image,
			this->data.colorBuffer.imageLayout,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			subresourceRange, srcStageMask, dstStageMask);
		this->data.colorBuffer.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}

	for (uint32_t i = 0; i < this->data.gBuffers.size(); ++i) {
		if (this->data.gBuffers[i].imageLayout != vk::ImageLayout::eShaderReadOnlyOptimal)
		{
			/* Transition destination image to transfer destination optimal */
			vk::ImageSubresourceRange subresourceRange;
			subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.levelCount = this->data.gBuffers[i].mipLevels;
			subresourceRange.layerCount = this->data.layers;

			setImageLayout(
				commandBuffer,
				this->data.gBuffers[i].image,
				this->data.gBuffers[i].imageLayout,
				vk::ImageLayout::eShaderReadOnlyOptimal,
				subresourceRange, srcStageMask, dstStageMask);
			this->data.gBuffers[i].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		}
	}
}

void Texture::make_general(vk::CommandBuffer commandBuffer, 
	vk::PipelineStageFlags srcStageMask,
	vk::PipelineStageFlags dstStageMask)
{
	if (this->data.colorBuffer.imageLayout != vk::ImageLayout::eGeneral) {
		/* Transition destination image to transfer destination optimal */
		vk::ImageSubresourceRange subresourceRange;
		subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = this->data.colorBuffer.mipLevels;
		subresourceRange.layerCount = this->data.layers;

		setImageLayout(
			commandBuffer,
			this->data.colorBuffer.image,
			this->data.colorBuffer.imageLayout,
			vk::ImageLayout::eGeneral,
			subresourceRange, srcStageMask, dstStageMask);
		this->data.colorBuffer.imageLayout = vk::ImageLayout::eGeneral;
	}

	for (uint32_t i = 0; i < this->data.gBuffers.size(); ++i) {
		if (this->data.gBuffers[i].imageLayout != vk::ImageLayout::eGeneral)
		{
			/* Transition destination image to transfer destination optimal */
			vk::ImageSubresourceRange subresourceRange;
			subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.levelCount = this->data.gBuffers[i].mipLevels;
			subresourceRange.layerCount = this->data.layers;

			setImageLayout(
				commandBuffer,
				this->data.gBuffers[i].image,
				this->data.gBuffers[i].imageLayout,
				vk::ImageLayout::eGeneral,
				subresourceRange, srcStageMask, dstStageMask);
			this->data.gBuffers[i].imageLayout = vk::ImageLayout::eGeneral;
		}
	}
}

void Texture::save_as_ktx(std::string rawfilepath) {
	auto float_data = download_color_data(data.width, data.height, data.depth);

	gli::target target;
	if (data.viewType == vk::ImageViewType::e3D) target = gli::target::TARGET_3D;
	if (data.viewType == vk::ImageViewType::e2D) target = gli::target::TARGET_2D;

	gli::extent3d extent = gli::extent3d(data.width, data.height, data.depth);

	auto texture = gli::texture(target, gli::format::FORMAT_RGBA32_SFLOAT_PACK32, extent, 1, 1, 1);
	memcpy(texture.data(), float_data.data(), float_data.size() * sizeof(float));

	gli::save_ktx(texture, rawfilepath);
}