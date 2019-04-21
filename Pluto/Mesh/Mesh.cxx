// Because windows is silly and thinks it has the right to define min/max D:<
#define NOMINMAX

#ifndef TINYOBJLOADER_IMPLEMENTATION
#define TINYOBJLOADER_IMPLEMENTATION
#endif

#define TINYGLTF_IMPLEMENTATION
// #define TINYGLTF_NO_FS
// #define TINYGLTF_NO_STB_IMAGE_WRITE

#include <sys/types.h>
#include <sys/stat.h>

#define GLM_ENABLE_EXPERIMENTAL
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#define GLM_FORCE_RIGHT_HANDED

#include <glm/gtx/vector_angle.hpp>

#include "tetgen.h"

#include "./Mesh.hxx"

#include "Pluto/Tools/Options.hxx"
#include "Pluto/Tools/HashCombiner.hxx"
#include <tiny_stl.h>
#include <tiny_gltf.h>

#define GENERATOR_USE_GLM
#include <generator/generator.hpp>

// For some reason, windows is defining MemoryBarrier as something else, preventing me 
// from using the vulkan MemoryBarrier type...
#ifdef WIN32
#undef MemoryBarrier
#endif

Mesh Mesh::meshes[MAX_MESHES];
std::map<std::string, uint32_t> Mesh::lookupTable;
vk::Buffer Mesh::SSBO;
vk::DeviceMemory Mesh::SSBOMemory;
vk::Buffer Mesh::stagingSSBO;
vk::DeviceMemory Mesh::stagingSSBOMemory;
vk::AccelerationStructureNV Mesh::topAS;
vk::DeviceMemory Mesh::topASMemory;
vk::Buffer Mesh::instanceBuffer;
vk::DeviceMemory Mesh::instanceBufferMemory;
std::mutex Mesh::creation_mutex;
bool Mesh::Initialized = false;

class Vertex
{
	public:
	glm::vec3 point = glm::vec3(0.0);
	glm::vec4 color = glm::vec4(1, 0, 1, 1);
	glm::vec3 normal = glm::vec3(0.0);
	glm::vec2 texcoord = glm::vec2(0.0);

	std::vector<glm::vec3> wnormals = {}; // For computing normals

	bool operator==(const Vertex &other) const
	{
		bool result =
			(point == other.point && color == other.color && normal == other.normal && texcoord == other.texcoord);
		return result;
	}
};

void buildOrthonormalBasis(glm::vec3 n, glm::vec3 &b1, glm::vec3 &b2)
{
    if (n.z < -0.9999999)
    {
        b1 = glm::vec3( 0.0, -1.0, 0.0);
        b2 = glm::vec3(-1.0,  0.0, 0.0);
        return;
    }
    float a = 1.0f / (1.0f + n.z);
    float b = -n.x*n.y*a;
    b1 = glm::vec3(1.0 - n.x*n.x*a, b, -n.x);
    b2 = glm::vec3(b, 1.0 - n.y*n.y*a, -n.y);
}

namespace std
{
template <>
struct hash<Vertex>
{
	size_t operator()(const Vertex &k) const
	{
		std::size_t h = 0;
		hash_combine(h, k.point.x, k.point.y, k.point.z,
					 k.color.x, k.color.y, k.color.z, k.color.a,
					 k.normal.x, k.normal.y, k.normal.z,
					 k.texcoord.x, k.texcoord.y);
		return h;
	}
};
} // namespace std

Mesh::Mesh() {
	this->initialized = false;
}

Mesh::Mesh(std::string name, uint32_t id)
{
	this->initialized = true;
	this->name = name;
	this->id = id;
}

std::string Mesh::to_string() {
	std::string output;
	output += "{\n";
	output += "\ttype: \"Mesh\",\n";
	output += "\tname: \"" + name + "\",\n";
	output += "\tnum_positions: \"" + std::to_string(positions.size()) + "\",\n";
	output += "\tnum_indices: \"" + std::to_string(indices.size()) + "\",\n";
	output += "}";
	return output;
}


std::vector<glm::vec3> Mesh::get_positions() {
	return positions;
}

std::vector<glm::vec4> Mesh::get_colors() {
	return colors;
}

std::vector<glm::vec3> Mesh::get_normals() {
	return normals;
}

std::vector<glm::vec2> Mesh::get_texcoords() {
	return texcoords;
}

std::vector<uint32_t> Mesh::get_indices() {
	return indices;
}

vk::Buffer Mesh::get_point_buffer()
{
	return pointBuffer;
}

vk::Buffer Mesh::get_color_buffer()
{
	return colorBuffer;
}

vk::Buffer Mesh::get_index_buffer()
{
	return indexBuffer;
}

vk::Buffer Mesh::get_normal_buffer()
{
	return normalBuffer;
}

vk::Buffer Mesh::get_texcoord_buffer()
{
	return texCoordBuffer;
}

uint32_t Mesh::get_total_indices()
{
	return (uint32_t)indices.size();
}

uint32_t Mesh::get_index_bytes()
{
	return sizeof(uint32_t);
}

void Mesh::compute_metadata()
{
	glm::vec3 s(0.0);
	mesh_struct.bbmin = glm::vec3(0.0f);
	mesh_struct.bbmax = glm::vec3(0.0f);
	for (int i = 0; i < positions.size(); i += 1)
	{
		s += positions[i];
		mesh_struct.bbmin = glm::min(positions[i], mesh_struct.bbmin);
		mesh_struct.bbmax = glm::max(positions[i], mesh_struct.bbmax);
	}
	s /= positions.size();
	mesh_struct.centroid = s;

	mesh_struct.bounding_sphere_radius = 0.0;
	for (int i = 0; i < positions.size(); i += 1) {
		mesh_struct.bounding_sphere_radius = std::max(mesh_struct.bounding_sphere_radius, 
			glm::distance(positions[i], mesh_struct.centroid));
	}
}

void Mesh::compute_simulation_matrices(float mass_, float stiffness_, float step_size_)
{
	if (edges.empty() && !indices.empty())
		generate_edges();

	//number of vertices
	int nN = positions.size();
	//number of springs
	int nM = edges.size();

	// TODO: this model now also assumes zero initial velocities
	// possibly the user can provide a list of velocities along with the vertices
	velocities.resize(nN, glm::vec3(0, 0, 0));

	// TODO: this model now assumes homogeneous stiffness,
	// possibly the user can provide a list of stiffness along with the edges
	// TODO: this model now also assumes homogeneous mass
	// possilby the user can provide a list of mass values along with the vertices

	mass = mass_;
	stiffness = stiffness_;
	step_size = step_size_;
	damping_factor = 0.f; //user can set this using set_damping_factor

	L.resize(nN, nN);
	J.resize(nM, nN);
	int counter = 0;
	for (auto e : edges)
	{
		L.coeffRef(e.first, e.first) += 1;
		L.coeffRef(e.second, e.second) += 1;
		L.coeffRef(e.first, e.second) -= 1;
		L.coeffRef(e.second, e.first) -= 1;
		J.coeffRef(counter, e.first) += 1;
		J.coeffRef(counter, e.second) -= 1;
		counter++;
	}

	L.makeCompressed();
	J.makeCompressed();

	// allocate space for the mass matrix
	M.resize(nN, nN);
	precompute_mass_matrix();

	// matrix for external force
	G.resize(3, nN);

	//allocate space for precomputed cholesky
	precomputed_cholesky = std::make_shared< Eigen::SimplicialLLT<Eigen::SparseMatrix<float>>>();

	//precompute cholesky decomposition
	precompute_cholesky();
}

void Mesh::save_tetrahedralization(float quality_bound, float maximum_volume)
{
	try  {
		/* NOTE, POSSIBLY LEAKING MEMORY */
		tetgenio in, out;
		tetgenio::facet *f;
		tetgenio::polygon *p;
		int i;

		// All indices start from 1.
		in.firstnumber = 1;

		in.numberofpoints = this->positions.size();
		in.pointlist = new REAL[in.numberofpoints * 3];
		for (uint32_t i = 0; i < this->positions.size(); ++i) {
			in.pointlist[i * 3 + 0] = this->positions[i].x;
			in.pointlist[i * 3 + 1] = this->positions[i].y;
			in.pointlist[i * 3 + 2] = this->positions[i].z;
		}

		in.numberoffacets = this->indices.size() / 3; 
		in.facetlist = new tetgenio::facet[in.numberoffacets];
		in.facetmarkerlist = new int[in.numberoffacets];

		for (uint32_t i = 0; i < this->indices.size() / 3; ++i) {
			f = &in.facetlist[i];
			f->numberofpolygons = 1;
			f->polygonlist = new tetgenio::polygon[f->numberofpolygons];
			f->numberofholes = 0;
			f->holelist = NULL;
			p = &f->polygonlist[0];
			p->numberofvertices = 3;
			p->vertexlist = new int[p->numberofvertices];
			// Note, tetgen indices start at one.
			p->vertexlist[0] = indices[i * 3 + 0] + 1; 
			p->vertexlist[1] = indices[i * 3 + 1] + 1; 
			p->vertexlist[2] = indices[i * 3 + 2] + 1; 
			in.facetmarkerlist[i] = 0; // ?
		}

		// // Set 'in.facetmarkerlist'

		// in.facetmarkerlist[0] = -1;
		// in.facetmarkerlist[2] = 0;
		// in.facetmarkerlist[3] = 0;
		// in.facetmarkerlist[4] = 0;
		// in.facetmarkerlist[5] = 0;

		// Output the PLC to files 'barin.node' and 'barin.poly'.
		// in.save_nodes("barin");
		// in.save_poly("barin");

		// Tetrahedralize the PLC. Switches are chosen to read a PLC (p),
		//   do quality mesh generation (q) with a specified quality bound
		//   (1.414), and apply a maximum volume constraint (a0.1).

		std::string flags = "pq";
		flags += std::to_string(quality_bound);
		flags += "a";
		flags += std::to_string(maximum_volume);
		::tetrahedralize((char*)flags.c_str(), &in, &out);

		// // Output mesh to files 'barout.node', 'barout.ele' and 'barout.face'.
		out.save_nodes((char*)this->name.c_str());
		out.save_elements((char*)this->name.c_str());
		// out.save_faces((char*)this->name.c_str());
	}
	catch (...)
	{
		throw std::runtime_error("Error: failed to tetrahedralize mesh");
	}
}

glm::vec3 Mesh::get_centroid()
{
	return mesh_struct.centroid;
}

float Mesh::get_bounding_sphere_radius()
{
	return mesh_struct.bounding_sphere_radius;
}

glm::vec3 Mesh::get_min_aabb_corner()
{
	return mesh_struct.bbmin;
}

glm::vec3 Mesh::get_max_aabb_corner()
{
	return mesh_struct.bbmax;
}


void Mesh::cleanup()
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error( std::string("Vulkan library is not initialized"));
	auto device = vulkan->get_device();
	if (device == vk::Device())
		throw std::runtime_error( std::string("Invalid vulkan device"));

	/* Destroy index buffer */
	device.destroyBuffer(indexBuffer);
	device.freeMemory(indexBufferMemory);

	/* Destroy vertex buffer */
	device.destroyBuffer(pointBuffer);
	device.freeMemory(pointBufferMemory);

	/* Destroy vertex color buffer */
	device.destroyBuffer(colorBuffer);
	device.freeMemory(colorBufferMemory);

	/* Destroy normal buffer */
	device.destroyBuffer(normalBuffer);
	device.freeMemory(normalBufferMemory);

	/* Destroy uv buffer */
	device.destroyBuffer(texCoordBuffer);
	device.freeMemory(texCoordBufferMemory);
}

void Mesh::CleanUp()
{
	if (!IsInitialized()) return;

	auto vulkan = Libraries::Vulkan::Get();
    if (!vulkan->is_initialized())
        throw std::runtime_error( std::string("Vulkan library is not initialized"));
    auto device = vulkan->get_device();
    if (device == vk::Device())
        throw std::runtime_error( std::string("Invalid vulkan device"));

	for (auto &mesh : meshes) {
		if (mesh.initialized) {
			mesh.cleanup();
			Mesh::Delete(mesh.id);
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

	Initialized = false;
}

void Mesh::Initialize() {
	if (IsInitialized()) return;

	CreateBox("BoundingBox");

	auto vulkan = Libraries::Vulkan::Get();
    auto device = vulkan->get_device();
    if (device == vk::Device())
        throw std::runtime_error( std::string("Invalid vulkan device"));

    auto physical_device = vulkan->get_physical_device();
    if (physical_device == vk::PhysicalDevice())
        throw std::runtime_error( std::string("Invalid vulkan physical device"));

    {
        vk::BufferCreateInfo bufferInfo = {};
        bufferInfo.size = MAX_MESHES * sizeof(MeshStruct);
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
        bufferInfo.size = MAX_MESHES * sizeof(MeshStruct);
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

	Initialized = true;
}

bool Mesh::IsInitialized()
{
    return Initialized;
}

void Mesh::UploadSSBO(vk::CommandBuffer command_buffer)
{
    auto vulkan = Libraries::Vulkan::Get();
    auto device = vulkan->get_device();

    if (SSBOMemory == vk::DeviceMemory()) return;
    if (stagingSSBOMemory == vk::DeviceMemory()) return;

    auto bufferSize = MAX_MESHES * sizeof(MeshStruct);

    /* Pin the buffer */
	auto pinnedMemory = (MeshStruct*) device.mapMemory(stagingSSBOMemory, 0, bufferSize);
	if (pinnedMemory == nullptr) return;
	
	for (uint32_t i = 0; i < MAX_MESHES; ++i) {
		if (!meshes[i].is_initialized()) continue;
		pinnedMemory[i] = meshes[i].mesh_struct;
	};

	device.unmapMemory(stagingSSBOMemory);

    vk::BufferCopy copyRegion;
	copyRegion.size = bufferSize;
    command_buffer.copyBuffer(stagingSSBO, SSBO, copyRegion);
}

vk::Buffer Mesh::GetSSBO()
{
    if ((SSBO != vk::Buffer()) && (SSBOMemory != vk::DeviceMemory()))
        return SSBO;
    else return vk::Buffer();
}

uint32_t Mesh::GetSSBOSize()
{
    return MAX_MESHES * sizeof(MeshStruct);
}

void Mesh::load_obj(std::string objPath, bool allow_edits, bool submit_immediately)
{
	allowEdits = allow_edits;

	struct stat st;
	if (stat(objPath.c_str(), &st) != 0)
		throw std::runtime_error( std::string(objPath + " does not exist!"));

	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string err;

	if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, objPath.c_str()))
		throw std::runtime_error( std::string("Error: Unable to load " + objPath));

	std::vector<Vertex> vertices;

	bool has_normals = false;

	/* If the mesh has a set of shapes, merge them all into one */
	if (shapes.size() > 0)
	{
		for (const auto &shape : shapes)
		{
			for (const auto &index : shape.mesh.indices)
			{
				Vertex vertex = Vertex();
				vertex.point = {
					attrib.vertices[3 * index.vertex_index + 0],
					attrib.vertices[3 * index.vertex_index + 1],
					attrib.vertices[3 * index.vertex_index + 2]};
				if (attrib.colors.size() != 0)
				{
					vertex.color = {
						attrib.colors[3 * index.vertex_index + 0],
						attrib.colors[3 * index.vertex_index + 1],
						attrib.colors[3 * index.vertex_index + 2],
						1.f};
				}
				if (attrib.normals.size() != 0)
				{
					vertex.normal = {
						attrib.normals[3 * index.normal_index + 0],
						attrib.normals[3 * index.normal_index + 1],
						attrib.normals[3 * index.normal_index + 2]};
					has_normals = true;
				}
				if (attrib.texcoords.size() != 0)
				{
					vertex.texcoord = {
						attrib.texcoords[2 * index.texcoord_index + 0],
						attrib.texcoords[2 * index.texcoord_index + 1]};
				}
				vertices.push_back(vertex);
			}
		}
	}

	/* If the obj has no shapes, eg polylines, then try looking for per vertex data */
	else if (shapes.size() == 0)
	{
		for (int idx = 0; idx < attrib.vertices.size() / 3; ++idx)
		{
			Vertex v = Vertex();
			v.point = glm::vec3(attrib.vertices[(idx * 3)], attrib.vertices[(idx * 3) + 1], attrib.vertices[(idx * 3) + 2]);
			if (attrib.normals.size() != 0)
			{
				v.normal = glm::vec3(attrib.normals[(idx * 3)], attrib.normals[(idx * 3) + 1], attrib.normals[(idx * 3) + 2]);
				has_normals = true;
			}
			if (attrib.colors.size() != 0)
			{
				v.normal = glm::vec3(attrib.colors[(idx * 3)], attrib.colors[(idx * 3) + 1], attrib.colors[(idx * 3) + 2]);
			}
			if (attrib.texcoords.size() != 0)
			{
				v.texcoord = glm::vec2(attrib.texcoords[(idx * 2)], attrib.texcoords[(idx * 2) + 1]);
			}
			vertices.push_back(v);
		}
	}

	/* Eliminate duplicate positions */
	std::unordered_map<Vertex, uint32_t> uniqueVertexMap = {};
	std::vector<Vertex> uniqueVertices;
	for (int i = 0; i < vertices.size(); ++i)
	{
		Vertex vertex = vertices[i];
		if (uniqueVertexMap.count(vertex) == 0)
		{
			uniqueVertexMap[vertex] = static_cast<uint32_t>(uniqueVertices.size());
			uniqueVertices.push_back(vertex);
		}
		indices.push_back(uniqueVertexMap[vertex]);
	}

	if (!has_normals) {
		compute_smooth_normals(false);
	}


	/* Map vertices to buffers */
	for (int i = 0; i < uniqueVertices.size(); ++i)
	{
		Vertex v = uniqueVertices[i];
		positions.push_back(v.point);
		colors.push_back(v.color);
		normals.push_back(v.normal);
		texcoords.push_back(v.texcoord);
	}

	cleanup();
	compute_metadata();
	createPointBuffer(allow_edits, submit_immediately);
	createColorBuffer(allow_edits, submit_immediately);
	createIndexBuffer(allow_edits, submit_immediately);
	createNormalBuffer(allow_edits, submit_immediately);
	createTexCoordBuffer(allow_edits, submit_immediately);
}


void Mesh::load_stl(std::string stlPath, bool allow_edits, bool submit_immediately) {
	allowEdits = allow_edits;

	struct stat st;
	if (stat(stlPath.c_str(), &st) != 0)
		throw std::runtime_error( std::string(stlPath + " does not exist!"));

	std::vector<float> p;
	std::vector<float> n;

	if (!read_stl(stlPath, p, n) )
		throw std::runtime_error( std::string("Error: Unable to load " + stlPath));

	std::vector<Vertex> vertices;

	/* STLs only have positions and face normals, so generate colors and UVs */
	for (uint32_t i = 0; i < p.size() / 3; ++i) {
		Vertex vertex = Vertex();
		vertex.point = {
			p[i * 3 + 0],
			p[i * 3 + 1],
			p[i * 3 + 2],
		};
		vertex.normal = {
			n[i * 3 + 0],
			n[i * 3 + 1],
			n[i * 3 + 2],
		};
		vertices.push_back(vertex);
	}

	/* Eliminate duplicate positions */
	std::unordered_map<Vertex, uint32_t> uniqueVertexMap = {};
	std::vector<Vertex> uniqueVertices;
	for (int i = 0; i < vertices.size(); ++i)
	{
		Vertex vertex = vertices[i];
		if (uniqueVertexMap.count(vertex) == 0)
		{
			uniqueVertexMap[vertex] = static_cast<uint32_t>(uniqueVertices.size());
			uniqueVertices.push_back(vertex);
		}
		indices.push_back(uniqueVertexMap[vertex]);
	}

	/* Map vertices to buffers */
	for (int i = 0; i < uniqueVertices.size(); ++i)
	{
		Vertex v = uniqueVertices[i];
		positions.push_back(v.point);
		colors.push_back(v.color);
		normals.push_back(v.normal);
		texcoords.push_back(v.texcoord);
	}

	cleanup();
	compute_metadata();
	createPointBuffer(allow_edits, submit_immediately);
	createColorBuffer(allow_edits, submit_immediately);
	createIndexBuffer(allow_edits, submit_immediately);
	createNormalBuffer(allow_edits, submit_immediately);
	createTexCoordBuffer(allow_edits, submit_immediately);
}

void Mesh::load_glb(std::string glbPath, bool allow_edits, bool submit_immediately)
{
	allowEdits = allow_edits;
	struct stat st;
	if (stat(glbPath.c_str(), &st) != 0)
	{
		throw std::runtime_error(std::string("Error: " + glbPath + " does not exist"));
	}

	// read file
	unsigned char *file_buffer = NULL;
	uint32_t file_size = 0;
	{
		FILE *fp = fopen(glbPath.c_str(), "rb");
		if (!fp) {
			throw std::runtime_error( std::string(glbPath + " does not exist!"));
		}
		assert(fp);
		fseek(fp, 0, SEEK_END);
		file_size = (uint32_t)ftell(fp);
		rewind(fp);
		file_buffer = (unsigned char *)malloc(file_size);
		assert(file_buffer);
		fread(file_buffer, 1, file_size, fp);
		fclose(fp);
	}

	tinygltf::Model model;
	tinygltf::TinyGLTF loader;

	std::string err, warn;
	if (!loader.LoadBinaryFromMemory(&model, &err, &warn, file_buffer, file_size, "", tinygltf::REQUIRE_ALL))
		throw std::runtime_error( std::string("Error: Unable to load " + glbPath + " " + err));

	std::vector<Vertex> vertices;

	for (const auto &mesh : model.meshes) {
		for (const auto &primitive : mesh.primitives)
		{
			const auto &idx_accessor = model.accessors[primitive.indices];
			const auto &pos_accessor = model.accessors[primitive.attributes.find("POSITION")->second];
			const auto &nrm_accessor = model.accessors[primitive.attributes.find("NORMAL")->second];
			const auto &tex_accessor = model.accessors[primitive.attributes.find("TEXCOORD_0")->second];

			const auto &idx_bufferView = model.bufferViews[idx_accessor.bufferView];
			const auto &pos_bufferView = model.bufferViews[pos_accessor.bufferView];
			const auto &nrm_bufferView = model.bufferViews[nrm_accessor.bufferView];
			const auto &tex_bufferView = model.bufferViews[tex_accessor.bufferView];

			const auto &idx_buffer = model.buffers[idx_bufferView.buffer]; 
			const auto &pos_buffer = model.buffers[pos_bufferView.buffer]; 
			const auto &nrm_buffer = model.buffers[nrm_bufferView.buffer]; 
			const auto &tex_buffer = model.buffers[tex_bufferView.buffer]; 

			const float *pos = (const float *) pos_buffer.data.data();
			const float *nrm = (const float *) nrm_buffer.data.data();
			const float *tex = (const float *) tex_buffer.data.data();
			const char* idx  = (const char *) &idx_buffer.data[idx_bufferView.byteOffset];

			/* For each vertex */
			for (int i = 0; i < idx_accessor.count; ++ i) {
				unsigned int index = -1;
				if (idx_accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
					index = (unsigned int) ((unsigned int*)idx)[i];
				else 
					index = (unsigned int) ((unsigned short*)idx)[i];
				
				Vertex vertex = Vertex();
				vertex.point = {
					pos[3 * index + 0],
					pos[3 * index + 1],
					pos[3 * index + 2]};

				vertex.normal = {
					nrm[3 * index + 0],
					nrm[3 * index + 1],
					nrm[3 * index + 2]};

				vertex.texcoord = {
					tex[2 * index + 0],
					tex[2 * index + 1]};
				
				vertices.push_back(vertex);
			}
		}
	}

	/* Eliminate duplicate positions */
	std::unordered_map<Vertex, uint32_t> uniqueVertexMap = {};
	std::vector<Vertex> uniqueVertices;
	for (int i = 0; i < vertices.size(); ++i)
	{
		Vertex vertex = vertices[i];
		if (uniqueVertexMap.count(vertex) == 0)
		{
			uniqueVertexMap[vertex] = static_cast<uint32_t>(uniqueVertices.size());
			uniqueVertices.push_back(vertex);
		}
		indices.push_back(uniqueVertexMap[vertex]);
	}

	/* Map vertices to buffers */
	for (int i = 0; i < uniqueVertices.size(); ++i)
	{
		Vertex v = uniqueVertices[i];
		positions.push_back(v.point);
		colors.push_back(v.color);
		normals.push_back(v.normal);
		texcoords.push_back(v.texcoord);
	}

	cleanup();
	compute_metadata();
	createPointBuffer(allow_edits, submit_immediately);
	createColorBuffer(allow_edits, submit_immediately);
	createIndexBuffer(allow_edits, submit_immediately);
	createNormalBuffer(allow_edits, submit_immediately);
	createTexCoordBuffer(allow_edits, submit_immediately);
}

void Mesh::load_tetgen(std::string path, bool allow_edits, bool submit_immediately)
{
	struct stat st;
	allowEdits = allow_edits;
	
	size_t lastindex = path.find_last_of("."); 
	std::string rawname = path.substr(0, lastindex); 

	std::string nodePath = rawname + ".node";
	std::string elePath = rawname + ".node";
	
	if (stat(nodePath.c_str(), &st) != 0)
		throw std::runtime_error(std::string("Error: " + nodePath + " does not exist"));

	if (stat(elePath.c_str(), &st) != 0)
		throw std::runtime_error(std::string("Error: " + elePath + " does not exist"));

	// Somehow here, verify the node and ele files are in the same directory...
	tetgenio in;

	in.load_tetmesh((char*)rawname.c_str());

	if (in.mesh_dim != 3) 
		throw std::runtime_error(std::string("Error: Node dimension must be 3"));

	if (in.numberoftetrahedra <= 0)
		throw std::runtime_error(std::string("Error: number of tetrahedra must be more than 0"));

	std::vector<Vertex> vertices;
	for (uint32_t i = 0; i < (uint32_t)in.numberoftetrahedra; ++i) {
		uint32_t i1 = in.tetrahedronlist[i * 4 + 0] - 1;
		uint32_t i2 = in.tetrahedronlist[i * 4 + 1] - 1;
		uint32_t i3 = in.tetrahedronlist[i * 4 + 2] - 1;
		uint32_t i4 = in.tetrahedronlist[i * 4 + 3] - 1;

		Vertex v1, v2, v3, v4;
		v1.point = glm::vec3(in.pointlist[i1 * 3 + 0], in.pointlist[i1 * 3 + 1], in.pointlist[i1 * 3 + 2]);
		v2.point = glm::vec3(in.pointlist[i2 * 3 + 0], in.pointlist[i2 * 3 + 1], in.pointlist[i2 * 3 + 2]);
		v3.point = glm::vec3(in.pointlist[i3 * 3 + 0], in.pointlist[i3 * 3 + 1], in.pointlist[i3 * 3 + 2]);
		v4.point = glm::vec3(in.pointlist[i4 * 3 + 0], in.pointlist[i4 * 3 + 1], in.pointlist[i4 * 3 + 2]);

		vertices.push_back(v1); vertices.push_back(v4); vertices.push_back(v3);
		vertices.push_back(v1); vertices.push_back(v2); vertices.push_back(v4);
		vertices.push_back(v2); vertices.push_back(v3); vertices.push_back(v4);
		vertices.push_back(v1); vertices.push_back(v3); vertices.push_back(v2);
	}

	/* Eliminate duplicate positions */
	std::unordered_map<Vertex, uint32_t> uniqueVertexMap = {};
	std::vector<Vertex> uniqueVertices;
	for (int i = 0; i < vertices.size(); ++i)
	{
		Vertex vertex = vertices[i];
		if (uniqueVertexMap.count(vertex) == 0)
		{
			uniqueVertexMap[vertex] = static_cast<uint32_t>(uniqueVertices.size());
			uniqueVertices.push_back(vertex);
		}
		indices.push_back(uniqueVertexMap[vertex]);
	}

	/* Map vertices to buffers */
	for (int i = 0; i < uniqueVertices.size(); ++i)
	{
		Vertex v = uniqueVertices[i];
		positions.push_back(v.point);
		colors.push_back(v.color);
		normals.push_back(v.normal);
		texcoords.push_back(v.texcoord);
	}

	cleanup();
	compute_metadata();
	createPointBuffer(allow_edits, submit_immediately);
	createColorBuffer(allow_edits, submit_immediately);
	createIndexBuffer(allow_edits, submit_immediately);
	createNormalBuffer(allow_edits, submit_immediately);
	createTexCoordBuffer(allow_edits, submit_immediately);
}

void Mesh::load_raw(
	std::vector<glm::vec3> &positions_, 
	std::vector<glm::vec3> &normals_, 
	std::vector<glm::vec4> &colors_, 
	std::vector<glm::vec2> &texcoords_, 
	std::vector<uint32_t> indices_,
	std::vector<glm::ivec2>& edges_,
	std::vector<float>& rest_lengths_,
	bool allow_edits, bool submit_immediately
)
{
	allowEdits = allow_edits;
	bool reading_normals = normals_.size() > 0;
	bool reading_colors = colors_.size() > 0;
	bool reading_texcoords = texcoords_.size() > 0;
	bool reading_indices = indices_.size() > 0;
	bool reading_edges = edges_.size() > 0;
	bool reading_rest_lengths = rest_lengths_.size() > 0;

	if (positions_.size() == 0)
		throw std::runtime_error( std::string("Error, no positions supplied. "));

	if ((!reading_indices) && ((positions_.size() % 3) != 0))
		throw std::runtime_error( std::string("Error: No indices provided, and length of positions (") + std::to_string(positions_.size()) + std::string(") is not a multiple of 3."));

	if ((reading_indices) && ((indices_.size() % 3) != 0))
		throw std::runtime_error( std::string("Error: Length of indices (") + std::to_string(indices.size()) + std::string(") is not a multiple of 3."));
	
	if (reading_normals && (normals_.size() != positions_.size()))
		throw std::runtime_error( std::string("Error, length mismatch. Total normals: " + std::to_string(normals_.size()) + " does not equal total positions: " + std::to_string(positions_.size())));

	if (reading_colors && (colors_.size() != positions_.size()))
		throw std::runtime_error( std::string("Error, length mismatch. Total colors: " + std::to_string(colors_.size()) + " does not equal total positions: " + std::to_string(positions_.size())));
		
	if (reading_texcoords && (texcoords_.size() != positions_.size()))
		throw std::runtime_error( std::string("Error, length mismatch. Total texcoords: " + std::to_string(texcoords_.size()) + " does not equal total positions: " + std::to_string(positions_.size())));
	
	if (reading_indices) {
		for (uint32_t i = 0; i < indices_.size(); ++i) {
			if (indices_[i] >= positions_.size())
				throw std::runtime_error( std::string("Error, index out of bounds. Index " + std::to_string(i) + " is greater than total positions: " + std::to_string(positions_.size())));
		}
	}

	if (!reading_edges && !reading_indices)
		throw std::runtime_error(std::string("Error, indices must be provided to automatically generate edges"));

	if (edges_.size() > rest_lengths_.size() && reading_rest_lengths)
		throw std::runtime_error(std::string("Error, number of rest lengths provided (>0) is smaller than the number of edges."));
		
	std::vector<Vertex> vertices;

	/* For each vertex */
	for (int i = 0; i < positions_.size(); ++ i) {
		Vertex vertex = Vertex();
		vertex.point = positions_[i];
		if (reading_normals) vertex.normal = normals_[i];
		if (reading_colors) vertex.color = colors_[i];
		if (reading_texcoords) vertex.texcoord = texcoords_[i];        
		vertices.push_back(vertex);
	}

	/* Eliminate duplicate positions */
	std::unordered_map<Vertex, uint32_t> uniqueVertexMap = {};
	std::vector<Vertex> uniqueVertices;

	/* Don't bin positions as unique when editing, 
	since it's unexpected for a user to lose positions */
	if (allow_edits && !reading_indices) {
		uniqueVertices = vertices;
		for (int i = 0; i < vertices.size(); ++i) {
			indices.push_back(i);
		}
	}
	else if (reading_indices) {
		indices = indices_;
		uniqueVertices = vertices;
	}
	/* If indices werent supplied and editing isn't allowed, optimize by binning unique verts */
	else {    
		for (int i = 0; i < vertices.size(); ++i)
		{
			Vertex vertex = vertices[i];
			if (uniqueVertexMap.count(vertex) == 0)
			{
				uniqueVertexMap[vertex] = static_cast<uint32_t>(uniqueVertices.size());
				uniqueVertices.push_back(vertex);
			}
			indices.push_back(uniqueVertexMap[vertex]);
		}
	}

	/* Map vertices to buffers */
	for (int i = 0; i < uniqueVertices.size(); ++i)
	{
		Vertex v = uniqueVertices[i];
		positions.push_back(v.point);
		colors.push_back(v.color);
		normals.push_back(v.normal);
		texcoords.push_back(v.texcoord);
	}

	// if edges (constraints) are not provided, generate them 
	if (edges_.empty())
	{
		generate_edges();
	}
	else
	{
		for (auto e : edges_)
		{
			edges.push_back(std::make_pair(e.x, e.y));
			rest_lengths.push_back(length(positions[e.x] - positions[e.y]));
		}
		rest_lengths = rest_lengths_;
		rest_lengths.resize(edges.size()); //truncate
	}

	cleanup();
	compute_metadata();
	createPointBuffer(allow_edits, submit_immediately);
	createColorBuffer(allow_edits, submit_immediately);
	createIndexBuffer(allow_edits, submit_immediately);
	createNormalBuffer(allow_edits, submit_immediately);
	createTexCoordBuffer(allow_edits, submit_immediately);
}

void Mesh::generate_edges()
{
	edges.clear();
	rest_lengths.clear();
	std::set<std::pair<int, int>> unique_edges;
	// WARNING: here I assume that indices are always a list of triangle indices
	for (uint32_t i = 0; i < indices.size(); i += 3) {
		float first = indices[i], second = indices[i + 1];
		if (first > second) std::swap(first, second);
		unique_edges.insert(std::make_pair(first, second));

		first = indices[i], second = indices[i + 2];
		if (first > second) std::swap(first, second);
		unique_edges.insert(std::make_pair(first, second));

		first = indices[i + 1], second = indices[i + 2];
		if (first > second) std::swap(first, second);
		unique_edges.insert(std::make_pair(first, second));
	}

	for (auto e : unique_edges)
	{
		edges.push_back(std::make_pair(e.first, e.second));
		rest_lengths.push_back(length(positions[e.first] - positions[e.second]));
	}
}

void Mesh::precompute_mass_matrix()
{
	int nN = (int)positions.size();
	M.setIdentity();
	float unitmass = mass / nN;
	M *= unitmass;
}

void Mesh::precompute_cholesky()
{
	int nN = (int)positions.size();
	float unitmass = mass / nN;

	Eigen::SparseMatrix<float> A = stiffness * L;
	for (int i = 0; i < nN; i++)
	{
		A.coeffRef(i, i) += 1.f / (step_size*step_size) * unitmass;
	}

	precomputed_cholesky->compute(A.transpose());
}

void Mesh::edit_position(uint32_t index, glm::vec3 new_position)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");
	
	if (index >= this->positions.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->positions.size() - 1));
	
	positions[index] = new_position;
	compute_metadata();

	void *data = device.mapMemory(pointBufferMemory, (index * sizeof(glm::vec3)), sizeof(glm::vec3), vk::MemoryMapFlags());
	memcpy(data, &new_position, sizeof(glm::vec3));
	device.unmapMemory(pointBufferMemory);
}

void Mesh::edit_positions(uint32_t index, std::vector<glm::vec3> new_positions)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");
	
	if (index >= this->positions.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->positions.size() - 1));
	
	if ((index + new_positions.size()) > this->positions.size())
		throw std::runtime_error("Error: too many positions for given index, out of bounds. Max index is " + std::to_string(this->positions.size() - 1));
	
	memcpy(&positions[index], new_positions.data(), new_positions.size() * sizeof(glm::vec3));
	compute_metadata();

	void *data = device.mapMemory(pointBufferMemory, (index * sizeof(glm::vec3)), sizeof(glm::vec3) * new_positions.size(), vk::MemoryMapFlags());
	memcpy(data, new_positions.data(), sizeof(glm::vec3) * new_positions.size());
	device.unmapMemory(pointBufferMemory);
}

void Mesh::edit_velocity(uint32_t index, glm::vec3 new_velocity)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");

	if (index >= this->velocities.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->velocities.size() - 1));

	velocities[index] = new_velocity;
}

void Mesh::edit_velocities(uint32_t index, std::vector<glm::vec3> new_velocities)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");

	if (index >= this->velocities.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->velocities.size() - 1));

	if ((index + new_velocities.size()) > this->velocities.size())
		throw std::runtime_error("Error: too many positions for given index, out of bounds. Max index is " + std::to_string(this->velocities.size() - 1));

	memcpy(&velocities[index], new_velocities.data(), new_velocities.size() * sizeof(glm::vec3));
}

void Mesh::edit_normal(uint32_t index, glm::vec3 new_normal)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");
	
	if (index >= this->normals.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->normals.size() - 1));
	
	void *data = device.mapMemory(normalBufferMemory, (index * sizeof(glm::vec3)), sizeof(glm::vec3), vk::MemoryMapFlags());
	memcpy(data, &new_normal, sizeof(glm::vec3));
	device.unmapMemory(normalBufferMemory);
}

void Mesh::edit_normals(uint32_t index, std::vector<glm::vec3> new_normals)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");
	
	if (index >= this->normals.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->normals.size() - 1));
	
	if ((index + new_normals.size()) > this->normals.size())
		throw std::runtime_error("Error: too many normals for given index, out of bounds. Max index is " + std::to_string(this->normals.size() - 1));
	
	void *data = device.mapMemory(normalBufferMemory, (index * sizeof(glm::vec3)), sizeof(glm::vec3) * new_normals.size(), vk::MemoryMapFlags());
	memcpy(data, new_normals.data(), sizeof(glm::vec3) * new_normals.size());
	device.unmapMemory(normalBufferMemory);
}

void Mesh::compute_smooth_normals(bool upload)
{
	std::vector<std::vector<glm::vec3>> w_normals(positions.size());

	for (uint32_t f = 0; f < indices.size(); f += 3)
	{
		uint32_t i1 = indices[f + 0];
		uint32_t i2 = indices[f + 1];
		uint32_t i3 = indices[f + 2];

		// p1, p2 and p3 are the positions in the face (f)
		auto &p1 = positions[i1];
		auto &p2 = positions[i2];
		auto &p3 = positions[i3];

		// calculate facet normal of the triangle  using cross product;
		// both components are "normalized" against a common point chosen as the base
		glm::vec3 n = glm::cross((p2 - p1), (p3 - p1));    // p1 is the 'base' here

		// get the angle between the two other positions for each point;
		// the starting point will be the 'base' and the two adjacent positions will be normalized against it
		auto a1 = glm::angle(glm::normalize(p2 - p1), glm::normalize(p3 - p1));    // p1 is the 'base' here
		auto a2 = glm::angle(glm::normalize(p3 - p2), glm::normalize(p1 - p2));    // p2 is the 'base' here
		auto a3 = glm::angle(glm::normalize(p1 - p3), glm::normalize(p2 - p3));    // p3 is the 'base' here

		// normalize the initial facet normals if you want to ignore surface area
		// if (!area_weighting)
		// {
		//    n = glm::normalize(n);
		// }

		// store the weighted normal in an structured array
		w_normals[i1].push_back(n * a1);
		w_normals[i2].push_back(n * a2);
		w_normals[i3].push_back(n * a3);
	}
	for (uint32_t v = 0; v < w_normals.size(); v++)
	{
		glm::vec3 N = glm::vec3(0.0);

		// run through the normals in each vertex's array and interpolate them
		// vertex(v) here fetches the data of the vertex at index 'v'
		for (uint32_t n = 0; n < w_normals[v].size(); n++)
		{
			N += w_normals[v][n];
		}

		// normalize the final normal
		normals[v] = glm::normalize(glm::vec3(N.x, N.y, N.z));
	}

	if (upload) {
		edit_normals(0, normals);
	}
}

float Mesh::get_stiffness()
{
	return stiffness;
}

float Mesh::get_mass()
{
	return mass;
}

float Mesh::get_damping_factor()
{
	return damping_factor;
}

void Mesh::set_stiffness(float stiffness_)
{
	stiffness = stiffness_;
	precompute_cholesky();
}

void Mesh::set_mass(float mass_)
{
	mass = mass_;
	precompute_mass_matrix();
	precompute_cholesky();
}

void Mesh::set_damping_factor(float damping_factor_)
{
	damping_factor = damping_factor_;
}

void Mesh::update(float time_step, uint32_t iterations, glm::vec3 f_ext)
{
	int nN = (int)positions.size();

	// update the precomputed cholesky when time step size changes
	if (time_step != step_size)
	{
		step_size = time_step;
		precompute_cholesky();
	}

	// compute the external force matrix
	float unitmass = mass / nN;
	G.colwise() = Eigen::Vector3f(f_ext.x, f_ext.y, f_ext.z);
	G *= unitmass;

	using namespace Eigen;
	MatrixXf Y(3, nN);
	for (int i = 0; i < nN; i++)
		for (int j = 0; j < 3; j++)
			Y(j, i) = positions[i][j] + velocities[i][j];

	MatrixXf X(3, nN);
	X = Y;

	for (int iter = 0; iter < iterations; iter++)
	{
		int nM = (int)edges.size();
		// compute D
		MatrixXf D(3, nM);
		for (int i = 0; i < nM; i++)
		{
			auto spr = edges[i];
			Vector3f diff = X.col(spr.first) - X.col(spr.second);
			D.col(i) = rest_lengths[i] * diff.normalized();
		}

		// compute X
		MatrixXf b = D * (stiffness * J) + 1.f / (time_step*time_step) * Y * M + G;
		X = precomputed_cholesky->solve(b.transpose()).transpose();
	}

	//update velocities and positions
	for (int i = 0; i < nN; i++)
		for (int j = 0; j < 3; j++)
		{
			velocities[i][j] = (float)(1 - damping_factor) * (X(j, i) - positions[i][j]);
			positions[i][j] += velocities[i][j];
		}

	//upload new positions 
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();
	void *data = device.mapMemory(pointBufferMemory, 0, nN * sizeof(glm::vec3), vk::MemoryMapFlags());
	memcpy(data, positions.data(), nN * sizeof(glm::vec3));
	device.unmapMemory(pointBufferMemory);

	compute_metadata();
}

void Mesh::edit_vertex_color(uint32_t index, glm::vec4 new_color)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");
	
	if (index >= this->colors.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->colors.size() - 1));
	
	void *data = device.mapMemory(colorBufferMemory, (index * sizeof(glm::vec4)), sizeof(glm::vec4), vk::MemoryMapFlags());
	memcpy(data, &new_color, sizeof(glm::vec4));
	device.unmapMemory(colorBufferMemory);
}

void Mesh::edit_vertex_colors(uint32_t index, std::vector<glm::vec4> new_colors)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");
	
	if (index >= this->colors.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->colors.size() - 1));
	
	if ((index + new_colors.size()) > this->colors.size())
		throw std::runtime_error("Error: too many colors for given index, out of bounds. Max index is " + std::to_string(this->colors.size() - 1));
	
	void *data = device.mapMemory(colorBufferMemory, (index * sizeof(glm::vec4)), sizeof(glm::vec4) * new_colors.size(), vk::MemoryMapFlags());
	memcpy(data, new_colors.data(), sizeof(glm::vec4) * new_colors.size());
	device.unmapMemory(colorBufferMemory);
}

void Mesh::edit_texture_coordinate(uint32_t index, glm::vec2 new_texcoord)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");
	
	if (index >= this->texcoords.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->texcoords.size() - 1));
	
	void *data = device.mapMemory(texCoordBufferMemory, (index * sizeof(glm::vec2)), sizeof(glm::vec2), vk::MemoryMapFlags());
	memcpy(data, &new_texcoord, sizeof(glm::vec2));
	device.unmapMemory(texCoordBufferMemory);
}

void Mesh::edit_texture_coordinates(uint32_t index, std::vector<glm::vec2> new_texcoords)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized())
		throw std::runtime_error("Error: Vulkan is not initialized");
	auto device = vulkan->get_device();

	if (!allowEdits)
		throw std::runtime_error("Error: editing this component is not allowed. \
			Edits can be enabled during creation.");
	
	if (index >= this->texcoords.size())
		throw std::runtime_error("Error: index out of bounds. Max index is " + std::to_string(this->texcoords.size() - 1));
	
	if ((index + new_texcoords.size()) > this->texcoords.size())
		throw std::runtime_error("Error: too many texture coordinates for given index, out of bounds. Max index is " + std::to_string(this->texcoords.size() - 1));
	
	void *data = device.mapMemory(texCoordBufferMemory, (index * sizeof(glm::vec2)), sizeof(glm::vec2) * new_texcoords.size(), vk::MemoryMapFlags());
	memcpy(data, new_texcoords.data(), sizeof(glm::vec2) * new_texcoords.size());
	device.unmapMemory(texCoordBufferMemory);
}

void Mesh::build_top_level_bvh(bool submit_immediately)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized()) throw std::runtime_error("Error: vulkan is not initialized");

	if (!vulkan->is_ray_tracing_enabled()) 
		throw std::runtime_error("Error: Vulkan device extension VK_NVX_raytracing is not currently enabled.");
	
	auto dldi = vulkan->get_dldi();
	auto device = vulkan->get_device();
	if (!device) 
		throw std::runtime_error("Error: vulkan device not initialized");

	auto CreateAccelerationStructure = [&](vk::AccelerationStructureTypeNV type, uint32_t geometryCount,
		vk::GeometryNV* geometries, uint32_t instanceCount, vk::AccelerationStructureNV& AS, vk::DeviceMemory& memory)
	{
		vk::AccelerationStructureCreateInfoNV accelerationStructureInfo;
		accelerationStructureInfo.compactedSize = 0;
		accelerationStructureInfo.info.type = type;
		accelerationStructureInfo.info.instanceCount = instanceCount;
		accelerationStructureInfo.info.geometryCount = geometryCount;
		accelerationStructureInfo.info.pGeometries = geometries;

		AS = device.createAccelerationStructureNV(accelerationStructureInfo, nullptr, dldi);

		vk::AccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo;
		memoryRequirementsInfo.accelerationStructure = AS;
		memoryRequirementsInfo.type = vk::AccelerationStructureMemoryRequirementsTypeNV::eObject;

		vk::MemoryRequirements2 memoryRequirements;
		memoryRequirements = device.getAccelerationStructureMemoryRequirementsNV(memoryRequirementsInfo, dldi);

		vk::MemoryAllocateInfo memoryAllocateInfo;
		memoryAllocateInfo.allocationSize = memoryRequirements.memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = vulkan->find_memory_type(memoryRequirements.memoryRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

		memory = device.allocateMemory(memoryAllocateInfo);
		
		vk::BindAccelerationStructureMemoryInfoNV bindInfo;
		bindInfo.accelerationStructure = AS;
		bindInfo.memory = memory;
		bindInfo.memoryOffset = 0;
		bindInfo.deviceIndexCount = 0;
		bindInfo.pDeviceIndices = nullptr;

		device.bindAccelerationStructureMemoryNV({bindInfo}, dldi);
	};

	/* Create top level acceleration structure */
	CreateAccelerationStructure(vk::AccelerationStructureTypeNV::eTopLevel,
		0, nullptr, 1, topAS, topASMemory);


	/* Gather Instances */
	std::vector<VkGeometryInstance> instances;
	for (uint32_t i = 0; i < MAX_MESHES; ++i)
	{
		if (!meshes[i].is_initialized()) continue;
		if (!meshes[i].lowBVHBuilt) continue;
		instances.push_back(meshes[i].instance);
	}

	/* ----- Create Instance Buffer ----- */
	{
		uint32_t instanceBufferSize = (uint32_t)(sizeof(VkGeometryInstance) * instances.size());

		vk::BufferCreateInfo instanceBufferInfo;
		instanceBufferInfo.size = instanceBufferSize;
		instanceBufferInfo.usage = vk::BufferUsageFlagBits::eRayTracingNV;
		instanceBufferInfo.sharingMode = vk::SharingMode::eExclusive;

		instanceBuffer = device.createBuffer(instanceBufferInfo);

		vk::MemoryRequirements instanceBufferRequirements;
		instanceBufferRequirements = device.getBufferMemoryRequirements(instanceBuffer);

		vk::MemoryAllocateInfo instanceMemoryAllocateInfo;
		instanceMemoryAllocateInfo.allocationSize = instanceBufferRequirements.size;
		instanceMemoryAllocateInfo.memoryTypeIndex = vulkan->find_memory_type(instanceBufferRequirements.memoryTypeBits, 
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent );

		instanceBufferMemory = device.allocateMemory(instanceMemoryAllocateInfo);
		
		device.bindBufferMemory(instanceBuffer, instanceBufferMemory, 0);

		void* ptr = device.mapMemory(instanceBufferMemory, 0, instanceBufferSize);
		memcpy(ptr, instances.data(), instanceBufferSize);
		device.unmapMemory(instanceBufferMemory);
	}

	/* Build top level BVH */
	auto GetScratchBufferSize = [&](vk::AccelerationStructureNV handle)
	{
		vk::AccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo;
		memoryRequirementsInfo.accelerationStructure = handle;
		memoryRequirementsInfo.type = vk::AccelerationStructureMemoryRequirementsTypeNV::eBuildScratch;

		vk::MemoryRequirements2 memoryRequirements;
		memoryRequirements = device.getAccelerationStructureMemoryRequirementsNV( memoryRequirementsInfo, dldi);

		vk::DeviceSize result = memoryRequirements.memoryRequirements.size;
		return result;
	};

	{
		vk::DeviceSize scratchBufferSize = GetScratchBufferSize(topAS);

		vk::BufferCreateInfo bufferInfo;
		bufferInfo.size = scratchBufferSize;
		bufferInfo.usage = vk::BufferUsageFlagBits::eRayTracingNV;
		bufferInfo.sharingMode = vk::SharingMode::eExclusive;
		vk::Buffer accelerationStructureScratchBuffer = device.createBuffer(bufferInfo);
		
		vk::MemoryRequirements scratchBufferRequirements;
		scratchBufferRequirements = device.getBufferMemoryRequirements(accelerationStructureScratchBuffer);
		
		vk::MemoryAllocateInfo scratchMemoryAllocateInfo;
		scratchMemoryAllocateInfo.allocationSize = scratchBufferRequirements.size;
		scratchMemoryAllocateInfo.memoryTypeIndex = vulkan->find_memory_type(scratchBufferRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

		vk::DeviceMemory accelerationStructureScratchMemory = device.allocateMemory(scratchMemoryAllocateInfo);
		device.bindBufferMemory(accelerationStructureScratchBuffer, accelerationStructureScratchMemory, 0);

		/* Now we can build our acceleration structure */
		vk::MemoryBarrier memoryBarrier;
		memoryBarrier.srcAccessMask  = vk::AccessFlagBits::eAccelerationStructureWriteNV;
		memoryBarrier.srcAccessMask |= vk::AccessFlagBits::eAccelerationStructureReadNV;
		memoryBarrier.dstAccessMask  = vk::AccessFlagBits::eAccelerationStructureWriteNV;
		memoryBarrier.dstAccessMask |= vk::AccessFlagBits::eAccelerationStructureReadNV;

		auto cmd = vulkan->begin_one_time_graphics_command();

		/* TODO: MOVE INSTANCE STUFF INTO HERE */
		{
			vk::AccelerationStructureInfoNV asInfo;
			asInfo.type = vk::AccelerationStructureTypeNV::eTopLevel;
			asInfo.instanceCount = (uint32_t) instances.size();
			asInfo.geometryCount = 0;// (uint32_t)geometries.size();
			asInfo.pGeometries = nullptr;//&geometries[0];

			cmd.buildAccelerationStructureNV(&asInfo, 
				instanceBuffer, 0, VK_FALSE, 
				topAS, vk::AccelerationStructureNV(),
				accelerationStructureScratchBuffer, 0, dldi);
		}
		
		// cmd.pipelineBarrier(
		//     vk::PipelineStageFlagBits::eAccelerationStructureBuildNV, 
		//     vk::PipelineStageFlagBits::eRayTracingShaderNV, 
		//     vk::DependencyFlags(), {memoryBarrier}, {}, {});

		if (submit_immediately)
			vulkan->end_one_time_graphics_command_immediately(cmd, "build acceleration structure", true);
		else
			vulkan->end_one_time_graphics_command(cmd, "build acceleration structure", true);

	}

}

void Mesh::build_low_level_bvh(bool submit_immediately)
{
	auto vulkan = Libraries::Vulkan::Get();
	if (!vulkan->is_initialized()) throw std::runtime_error("Error: vulkan is not initialized");

	if (!vulkan->is_ray_tracing_enabled()) 
		throw std::runtime_error("Error: Vulkan device extension VK_NVX_raytracing is not currently enabled.");
	
	auto dldi = vulkan->get_dldi();
	auto device = vulkan->get_device();
	if (!device) 
		throw std::runtime_error("Error: vulkan device not initialized");



	/* ----- Make geometry handle ----- */
	vk::GeometryDataNV geoData;

	{
		vk::GeometryTrianglesNV tris;
		tris.vertexData = this->pointBuffer;
		tris.vertexOffset = 0;
		tris.vertexCount = (uint32_t) this->positions.size();
		tris.vertexStride = sizeof(glm::vec3);
		tris.vertexFormat = vk::Format::eR32G32B32A32Sfloat;
		tris.indexData = this->indexBuffer;
		tris.indexOffset = 0;
		tris.indexType = vk::IndexType::eUint32;

		geoData.triangles = tris;
		geometry.geometryType = vk::GeometryTypeNV::eTriangles;
		geometry.geometry = geoData;
	}
	


	/* ----- Create the bottom level acceleration structure ----- */
	// Bottom level acceleration structures correspond to the geometry

	auto CreateAccelerationStructure = [&](vk::AccelerationStructureTypeNV type, uint32_t geometryCount,
		vk::GeometryNV* geometries, uint32_t instanceCount, vk::AccelerationStructureNV& AS, vk::DeviceMemory& memory)
	{
		vk::AccelerationStructureCreateInfoNV accelerationStructureInfo;
		accelerationStructureInfo.compactedSize = 0;
		accelerationStructureInfo.info.type = type;
		accelerationStructureInfo.info.instanceCount = instanceCount;
		accelerationStructureInfo.info.geometryCount = geometryCount;
		accelerationStructureInfo.info.pGeometries = geometries;

		AS = device.createAccelerationStructureNV(accelerationStructureInfo, nullptr, dldi);

		vk::AccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo;
		memoryRequirementsInfo.accelerationStructure = AS;
		memoryRequirementsInfo.type = vk::AccelerationStructureMemoryRequirementsTypeNV::eObject;

		vk::MemoryRequirements2 memoryRequirements;
		memoryRequirements = device.getAccelerationStructureMemoryRequirementsNV(memoryRequirementsInfo, dldi);

		vk::MemoryAllocateInfo memoryAllocateInfo;
		memoryAllocateInfo.allocationSize = memoryRequirements.memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = vulkan->find_memory_type(memoryRequirements.memoryRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

		memory = device.allocateMemory(memoryAllocateInfo);
		
		vk::BindAccelerationStructureMemoryInfoNV bindInfo;
		bindInfo.accelerationStructure = AS;
		bindInfo.memory = memory;
		bindInfo.memoryOffset = 0;
		bindInfo.deviceIndexCount = 0;
		bindInfo.pDeviceIndices = nullptr;

		device.bindAccelerationStructureMemoryNV({bindInfo}, dldi);
	};

	CreateAccelerationStructure(vk::AccelerationStructureTypeNV::eBottomLevel,
		1, &geometry, 0, lowAS, lowASMemory);

	/* Create Instance */
	{
		uint64_t accelerationStructureHandle;
		device.getAccelerationStructureHandleNV(lowAS, sizeof(uint64_t), &accelerationStructureHandle, dldi);
		float transform[12] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
		};

		memcpy(instance.transform, transform, sizeof(instance.transform));
		instance.instanceId = 0;
		instance.mask = 0xff;
		instance.instanceOffset = 0;
		instance.flags = (uint32_t) vk::GeometryInstanceFlagBitsNV::eTriangleCullDisable;
		instance.accelerationStructureHandle = accelerationStructureHandle;
	}


	/* Build low level BVH */
	auto GetScratchBufferSize = [&](vk::AccelerationStructureNV handle)
	{
		vk::AccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo;
		memoryRequirementsInfo.accelerationStructure = handle;
		memoryRequirementsInfo.type = vk::AccelerationStructureMemoryRequirementsTypeNV::eBuildScratch;

		vk::MemoryRequirements2 memoryRequirements;
		memoryRequirements = device.getAccelerationStructureMemoryRequirementsNV( memoryRequirementsInfo, dldi);

		vk::DeviceSize result = memoryRequirements.memoryRequirements.size;
		return result;
	};

	{
		vk::DeviceSize scratchBufferSize = GetScratchBufferSize(lowAS);

		vk::BufferCreateInfo bufferInfo;
		bufferInfo.size = scratchBufferSize;
		bufferInfo.usage = vk::BufferUsageFlagBits::eRayTracingNV;
		bufferInfo.sharingMode = vk::SharingMode::eExclusive;
		vk::Buffer accelerationStructureScratchBuffer = device.createBuffer(bufferInfo);
		
		vk::MemoryRequirements scratchBufferRequirements;
		scratchBufferRequirements = device.getBufferMemoryRequirements(accelerationStructureScratchBuffer);
		
		vk::MemoryAllocateInfo scratchMemoryAllocateInfo;
		scratchMemoryAllocateInfo.allocationSize = scratchBufferRequirements.size;
		scratchMemoryAllocateInfo.memoryTypeIndex = vulkan->find_memory_type(scratchBufferRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

		vk::DeviceMemory accelerationStructureScratchMemory = device.allocateMemory(scratchMemoryAllocateInfo);
		device.bindBufferMemory(accelerationStructureScratchBuffer, accelerationStructureScratchMemory, 0);

		/* Now we can build our acceleration structure */
		vk::MemoryBarrier memoryBarrier;
		memoryBarrier.srcAccessMask  = vk::AccessFlagBits::eAccelerationStructureWriteNV;
		memoryBarrier.srcAccessMask |= vk::AccessFlagBits::eAccelerationStructureReadNV;
		memoryBarrier.dstAccessMask  = vk::AccessFlagBits::eAccelerationStructureWriteNV;
		memoryBarrier.dstAccessMask |= vk::AccessFlagBits::eAccelerationStructureReadNV;

		auto cmd = vulkan->begin_one_time_graphics_command();

		{
			vk::AccelerationStructureInfoNV asInfo;
			asInfo.type = vk::AccelerationStructureTypeNV::eBottomLevel;
			asInfo.instanceCount = 0;
			asInfo.geometryCount = 1;// (uint32_t)geometries.size();
			asInfo.pGeometries = &geometry;//&geometries[0];

			cmd.buildAccelerationStructureNV(&asInfo, 
				vk::Buffer(), 0, VK_FALSE, 
				lowAS, vk::AccelerationStructureNV(),
				accelerationStructureScratchBuffer, 0, dldi);
		}
		
		// cmd.pipelineBarrier(
		//     vk::PipelineStageFlagBits::eAccelerationStructureBuildNV, 
		//     vk::PipelineStageFlagBits::eAccelerationStructureBuildNV, 
		//     vk::DependencyFlags(), {memoryBarrier}, {}, {});

		if (submit_immediately)
			vulkan->end_one_time_graphics_command_immediately(cmd, "build acceleration structure", true);
		else
			vulkan->end_one_time_graphics_command(cmd, "build acceleration structure", true);
	}

	/* Might need a fence here */
	lowBVHBuilt = true;
}

/* Static Factory Implementations */
Mesh* Mesh::Get(std::string name) {
	return StaticFactory::Get(name, "Mesh", lookupTable, meshes, MAX_MESHES);
}

Mesh* Mesh::Get(uint32_t id) {
	return StaticFactory::Get(id, "Mesh", lookupTable, meshes, MAX_MESHES);
}

Mesh* Mesh::CreateBox(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::BoxMesh gen_mesh{{1, 1, 1}, {1, 1, 1}};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateCappedCone(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::CappedConeMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateCappedCylinder(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::CappedCylinderMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateCappedTube(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::CappedTubeMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateCapsule(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::CapsuleMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateCone(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::ConeMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreatePentagon(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::ConvexPolygonMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateCylinder(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::CylinderMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateDisk(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::DiskMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateDodecahedron(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::DodecahedronMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreatePlane(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::PlaneMesh gen_mesh{{1, 1}, {1, 1}};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateIcosahedron(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::IcosahedronMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateIcosphere(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::IcoSphereMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

/* Might add this later. Requires a callback which defines a function mapping R2->R */
// Mesh* Mesh::CreateParametricMesh(std::string name, uint32_t x_segments = 16, uint32_t y_segments = 16, bool allow_edits, bool submit_immediately)
// {
//     auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
//     if (!mesh) return nullptr;
//     auto gen_mesh = generator::ParametricMesh( , glm::ivec2(x_segments, y_segments));
//     mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
//     return mesh;
// }

Mesh* Mesh::CreateRoundedBox(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::RoundedBoxMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateSphere(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::SphereMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateSphericalCone(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::SphericalConeMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateSphericalTriangle(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::SphericalTriangleMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateSpring(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::SpringMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateTeapotahedron(std::string name, uint32_t segments, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::TeapotMesh gen_mesh(segments);
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateTorus(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::TorusMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateTorusKnot(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::TorusKnotMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateTriangle(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::TriangleMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateTube(std::string name, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	generator::TubeMesh gen_mesh{};
	mesh->make_primitive(gen_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateTubeFromPolyline(std::string name, std::vector<glm::vec3> positions, float radius, uint32_t segments, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	if (positions.size() <= 1)
		throw std::runtime_error("Error: positions must be greater than 1!");
	
	using namespace generator;
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	
	ParametricPath parametricPath {
		[positions](double t) {
			// t is 1.0 / positions.size() - 1 and goes from 0 to 1.0
			float t_scaled = (float)t * (((float)positions.size()) - 1.0f);
			uint32_t p1_idx = (uint32_t) floor(t_scaled);
			uint32_t p2_idx = p1_idx + 1;

			float t_segment = t_scaled - floor(t_scaled);

			glm::vec3 p1 = positions[p1_idx];
			glm::vec3 p2 = positions[p2_idx];

			PathVertex vertex;
			
			vertex.position = (p2 * t_segment) + (p1 * (1.0f - t_segment));

			glm::vec3 next = (p2 * (t_segment + .01f)) + (p1 * (1.0f - (t_segment + .01f)));
			glm::vec3 prev = (p2 * (t_segment - .01f)) + (p1 * (1.0f - (t_segment - .01f)));

			glm::vec3 tangent = glm::normalize(next - prev);
			glm::vec3 B1;
			glm::vec3 B2;
			buildOrthonormalBasis(tangent, B1, B2);
			vertex.tangent = tangent;
			vertex.normal = B1;
			vertex.texCoord = t;

			return vertex;
		},
		((int32_t) positions.size() - 1) // number of segments
	} ;
	CircleShape circle_shape(radius, segments);
	ExtrudeMesh extrude_mesh(circle_shape, parametricPath);
	mesh->make_primitive(extrude_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateRoundedRectangleTubeFromPolyline(std::string name, std::vector<glm::vec3> positions, float radius, float size_x, float size_y, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	if (positions.size() <= 1)
		throw std::runtime_error("Error: positions must be greater than 1!");
	
	using namespace generator;
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	
	ParametricPath parametricPath {
		[positions](double t) {
			// t is 1.0 / positions.size() - 1 and goes from 0 to 1.0
			float t_scaled = (float)t * ((float)(positions.size()) - 1.0f);
			uint32_t p1_idx = (uint32_t) floor(t_scaled);
			uint32_t p2_idx = p1_idx + 1;

			float t_segment = t_scaled - floor(t_scaled);

			glm::vec3 p1 = positions[p1_idx];
			glm::vec3 p2 = positions[p2_idx];

			PathVertex vertex;
			
			vertex.position = (p2 * t_segment) + (p1 * (1.0f - t_segment));

			glm::vec3 next = (p2 * (t_segment + .01f)) + (p1 * (1.0f - (t_segment + .01f)));
			glm::vec3 prev = (p2 * (t_segment - .01f)) + (p1 * (1.0f - (t_segment - .01f)));

			glm::vec3 tangent = glm::normalize(next - prev);
			glm::vec3 B1;
			glm::vec3 B2;
			buildOrthonormalBasis(tangent, B1, B2);
			vertex.tangent = tangent;
			vertex.normal = B1;
			vertex.texCoord = t;

			return vertex;
		},
		((int32_t) positions.size() - 1) // number of segments
	} ;
	RoundedRectangleShape rounded_rectangle_shape(radius, {size_x, size_y}, 4, {1, 1});
	ExtrudeMesh extrude_mesh(rounded_rectangle_shape, parametricPath);
	mesh->make_primitive(extrude_mesh, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateRectangleTubeFromPolyline(std::string name, std::vector<glm::vec3> positions, float size_x, float size_y, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	if (positions.size() <= 1)
		throw std::runtime_error("Error: positions must be greater than 1!");
	
	using namespace generator;
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	if (!mesh) return nullptr;
	
	ParametricPath parametricPath {
		[positions](double t) {
			// t is 1.0 / positions.size() - 1 and goes from 0 to 1.0
			float t_scaled = (float)t * ((float)(positions.size()) - 1.0f);
			uint32_t p1_idx = (uint32_t) floor(t_scaled);
			uint32_t p2_idx = p1_idx + 1;

			float t_segment = t_scaled - floor(t_scaled);

			glm::vec3 p1 = positions[p1_idx];
			glm::vec3 p2 = positions[p2_idx];

			PathVertex vertex;
			
			vertex.position = (p2 * t_segment) + (p1 * (1.0f - t_segment));

			glm::vec3 next = (p2 * (t_segment + .01f)) + (p1 * (1.0f - (t_segment + .01f)));
			glm::vec3 prev = (p2 * (t_segment - .01f)) + (p1 * (1.0f - (t_segment - .01f)));

			glm::vec3 tangent = glm::normalize(next - prev);
			glm::vec3 B1;
			glm::vec3 B2;
			buildOrthonormalBasis(tangent, B1, B2);
			vertex.tangent = tangent;
			vertex.normal = B1;
			vertex.texCoord = t;

			return vertex;
		},
		((int32_t) positions.size() - 1) // number of segments
	} ;
	RectangleShape rectangle_shape({size_x, size_y}, {1, 1});
	ExtrudeMesh extrude_mesh(rectangle_shape, parametricPath);
	mesh->make_primitive(extrude_mesh, allow_edits, submit_immediately);
	return mesh;
}


Mesh* Mesh::CreateFromOBJ(std::string name, std::string objPath, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	mesh->load_obj(objPath, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateFromSTL(std::string name, std::string stlPath, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	mesh->load_stl(stlPath, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateFromGLB(std::string name, std::string glbPath, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	mesh->load_glb(glbPath, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateFromTetgen(std::string name, std::string path, bool allow_edits, bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	mesh->load_tetgen(path, allow_edits, submit_immediately);
	return mesh;
}

Mesh* Mesh::CreateFromRaw (
	std::string name,
	std::vector<glm::vec3> positions, 
	std::vector<glm::vec3> normals, 
	std::vector<glm::vec4> colors, 
	std::vector<glm::vec2> texcoords, 
	std::vector<uint32_t> indices, 
	std::vector<glm::ivec2> edges,
	std::vector<float> rest_lengths,
	bool allow_edits, 
	bool submit_immediately)
{
	std::lock_guard<std::mutex> lock(creation_mutex);
	auto mesh = StaticFactory::Create(name, "Mesh", lookupTable, meshes, MAX_MESHES);
	mesh->load_raw(positions, normals, colors, texcoords, indices, edges, rest_lengths, allow_edits, submit_immediately);
	return mesh;
}

void Mesh::Delete(std::string name) {
	StaticFactory::Delete(name, "Mesh", lookupTable, meshes, MAX_MESHES);
}

void Mesh::Delete(uint32_t id) {
	StaticFactory::Delete(id, "Mesh", lookupTable, meshes, MAX_MESHES);
}

Mesh* Mesh::GetFront() {
	return meshes;
}

uint32_t Mesh::GetCount() {
	return MAX_MESHES;
}

void Mesh::createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties, vk::Buffer &buffer, vk::DeviceMemory &bufferMemory)
{
	auto vulkan = Libraries::Vulkan::Get();
	auto device = vulkan->get_device();

	/* To create a VBO, we need to use this struct: */
	vk::BufferCreateInfo bufferInfo;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = vk::SharingMode::eExclusive;

	/* Now create the buffer */
	buffer = device.createBuffer(bufferInfo);

	/* Identify the memory requirements for the vertex buffer */
	vk::MemoryRequirements memRequirements = device.getBufferMemoryRequirements(buffer);

	/* Look for a suitable type that meets our property requirements */
	vk::MemoryAllocateInfo allocInfo;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = vulkan->find_memory_type(memRequirements.memoryTypeBits, properties);

	/* Now, allocate the memory for that buffer */
	bufferMemory = device.allocateMemory(allocInfo);

	/* Associate the allocated memory with the VBO handle */
	device.bindBufferMemory(buffer, bufferMemory, 0);
}

void Mesh::createPointBuffer(bool allow_edits, bool submit_immediately)
{
	auto vulkan = Libraries::Vulkan::Get();
	auto device = vulkan->get_device();

	vk::DeviceSize bufferSize = positions.size() * sizeof(glm::vec3);
	vk::Buffer stagingBuffer;
	vk::DeviceMemory stagingBufferMemory;
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer, stagingBufferMemory);

	/* Map the memory to a pointer on the host */
	void *data = device.mapMemory(stagingBufferMemory, 0, bufferSize,  vk::MemoryMapFlags());

	/* Copy over our vertex data, then unmap */
	memcpy(data, positions.data(), (size_t)bufferSize);
	device.unmapMemory(stagingBufferMemory);

	vk::MemoryPropertyFlags memoryProperties;
	if (!allowEdits) memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
	else {
		memoryProperties = vk::MemoryPropertyFlagBits::eHostVisible;
		memoryProperties |= vk::MemoryPropertyFlagBits::eHostCoherent;
	}
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer, memoryProperties, pointBuffer, pointBufferMemory);
	
	auto cmd = vulkan->begin_one_time_graphics_command();
	vk::BufferCopy copyRegion;
	copyRegion.size = bufferSize;
	cmd.copyBuffer(stagingBuffer, pointBuffer, copyRegion);

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(cmd, "copy point buffer", true);
	else
		vulkan->end_one_time_graphics_command(cmd, "copy point buffer", true);

	/* Clean up the staging buffer */
	device.destroyBuffer(stagingBuffer);
	device.freeMemory(stagingBufferMemory);
}

void Mesh::createColorBuffer(bool allow_edits, bool submit_immediately)
{
	auto vulkan = Libraries::Vulkan::Get();
	auto device = vulkan->get_device();

	vk::DeviceSize bufferSize = colors.size() * sizeof(glm::vec4);
	vk::Buffer stagingBuffer;
	vk::DeviceMemory stagingBufferMemory;
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer, stagingBufferMemory);

	/* Map the memory to a pointer on the host */
	void *data = device.mapMemory(stagingBufferMemory, 0, bufferSize,  vk::MemoryMapFlags());

	/* Copy over our vertex data, then unmap */
	memcpy(data, colors.data(), (size_t)bufferSize);
	device.unmapMemory(stagingBufferMemory);

	vk::MemoryPropertyFlags memoryProperties;
	if (!allowEdits) memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
	else {
		memoryProperties = vk::MemoryPropertyFlagBits::eHostVisible;
		memoryProperties |= vk::MemoryPropertyFlagBits::eHostCoherent;
	}
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer, memoryProperties, colorBuffer, colorBufferMemory);
	
	auto cmd = vulkan->begin_one_time_graphics_command();
	vk::BufferCopy copyRegion;
	copyRegion.size = bufferSize;
	cmd.copyBuffer(stagingBuffer, colorBuffer, copyRegion);

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(cmd, "copy point color buffer", true);
	else
		vulkan->end_one_time_graphics_command(cmd, "copy point color buffer", true);

	/* Clean up the staging buffer */
	device.destroyBuffer(stagingBuffer);
	device.freeMemory(stagingBufferMemory);
}

void Mesh::createIndexBuffer(bool allow_edits, bool submit_immediately)
{
	auto vulkan = Libraries::Vulkan::Get();
	auto device = vulkan->get_device();

	vk::DeviceSize bufferSize = indices.size() * sizeof(uint32_t);
	vk::Buffer stagingBuffer;
	vk::DeviceMemory stagingBufferMemory;
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer, stagingBufferMemory);

	void *data = device.mapMemory(stagingBufferMemory, 0, bufferSize, vk::MemoryMapFlags());
	memcpy(data, indices.data(), (size_t)bufferSize);
	device.unmapMemory(stagingBufferMemory);

	vk::MemoryPropertyFlags memoryProperties;
	// if (!allowEdits) memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
	// else {
		memoryProperties = vk::MemoryPropertyFlagBits::eHostVisible;
		memoryProperties |= vk::MemoryPropertyFlagBits::eHostCoherent;
	// }
	// Why cant I create a device local index buffer?..
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer, memoryProperties, indexBuffer, indexBufferMemory);
	
	auto cmd = vulkan->begin_one_time_graphics_command();
	vk::BufferCopy copyRegion;
	copyRegion.size = bufferSize;
	cmd.copyBuffer(stagingBuffer, indexBuffer, copyRegion);

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(cmd, "copy point index buffer", true);
	else
		vulkan->end_one_time_graphics_command(cmd, "copy point index buffer", true);

	device.destroyBuffer(stagingBuffer);
	device.freeMemory(stagingBufferMemory);
}

void Mesh::createNormalBuffer(bool allow_edits, bool submit_immediately)
{
	auto vulkan = Libraries::Vulkan::Get();
	auto device = vulkan->get_device();

	vk::DeviceSize bufferSize = normals.size() * sizeof(glm::vec3);
	vk::Buffer stagingBuffer;
	vk::DeviceMemory stagingBufferMemory;
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer, stagingBufferMemory);

	/* Map the memory to a pointer on the host */
	void *data = device.mapMemory(stagingBufferMemory, 0, bufferSize, vk::MemoryMapFlags());

	/* Copy over our normal data, then unmap */
	memcpy(data, normals.data(), (size_t)bufferSize);
	device.unmapMemory(stagingBufferMemory);

	vk::MemoryPropertyFlags memoryProperties;
	if (!allowEdits) memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
	else {
		memoryProperties = vk::MemoryPropertyFlagBits::eHostVisible;
		memoryProperties |= vk::MemoryPropertyFlagBits::eHostCoherent;
	}
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer, memoryProperties, normalBuffer, normalBufferMemory);
	
	auto cmd = vulkan->begin_one_time_graphics_command();
	vk::BufferCopy copyRegion;
	copyRegion.size = bufferSize;
	cmd.copyBuffer(stagingBuffer, normalBuffer, copyRegion);

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(cmd, "copy point normal buffer", true);
	else
		vulkan->end_one_time_graphics_command(cmd, "copy point normal buffer", true);

	/* Clean up the staging buffer */
	device.destroyBuffer(stagingBuffer);
	device.freeMemory(stagingBufferMemory);
}

void Mesh::createTexCoordBuffer(bool allow_edits, bool submit_immediately)
{
	auto vulkan = Libraries::Vulkan::Get();
	auto device = vulkan->get_device();

	vk::DeviceSize bufferSize = texcoords.size() * sizeof(glm::vec2);
	vk::Buffer stagingBuffer;
	vk::DeviceMemory stagingBufferMemory;
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, stagingBuffer, stagingBufferMemory);

	/* Map the memory to a pointer on the host */
	void *data = device.mapMemory(stagingBufferMemory, 0, bufferSize, vk::MemoryMapFlags());

	/* Copy over our normal data, then unmap */
	memcpy(data, texcoords.data(), (size_t)bufferSize);
	device.unmapMemory(stagingBufferMemory);

	vk::MemoryPropertyFlags memoryProperties;
	if (!allowEdits) memoryProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
	else {
		memoryProperties = vk::MemoryPropertyFlagBits::eHostVisible;
		memoryProperties |= vk::MemoryPropertyFlagBits::eHostCoherent;
	}
	createBuffer(bufferSize, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer, memoryProperties, texCoordBuffer, texCoordBufferMemory);
	
	auto cmd = vulkan->begin_one_time_graphics_command();
	vk::BufferCopy copyRegion;
	copyRegion.size = bufferSize;
	cmd.copyBuffer(stagingBuffer, texCoordBuffer, copyRegion);

	if (submit_immediately)
		vulkan->end_one_time_graphics_command_immediately(cmd, "copy point texcoord buffer", true);
	else
		vulkan->end_one_time_graphics_command(cmd, "copy point texcoord buffer", true);

	/* Clean up the staging buffer */
	device.destroyBuffer(stagingBuffer);
	device.freeMemory(stagingBufferMemory);
}

/* TODO */
void Mesh::show_bounding_box(bool should_show)
{
	this->showBoundingBox = should_show;
}

bool Mesh::should_show_bounding_box()
{
	return showBoundingBox;
}