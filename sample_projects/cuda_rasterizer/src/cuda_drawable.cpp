#include "cuda_drawable.h"
#include "d3d12_cuda_rasterizer.h"

#include <fstream>
#include <sstream>

Mesh::Mesh(const char *filename, const char *shaderName) {
	shader = shaderName;

	// read model from obj file
	std::ifstream in;
	in.open(filename, std::ifstream::in);
	if (in.fail()) {
		return;
	}
	std::string line;
	while (!in.eof()) {
		std::getline(in, line);
		std::istringstream iss(line.c_str());
		char trash;
		if (!line.compare(0, 2, "v ")) {
			iss >> trash;
			Vec4 v;
			for (int i = 0; i < 3; i++) {
				iss >> v.data[i];
			}
			v.w = 1.f;
			Vertex res{ v };
			geometry.push_back(res);
		} else if (!line.compare(0, 2, "f ")) {
			int itrash, idx;
			iss >> trash;
			while (iss >> idx >> trash >> itrash >> trash >> itrash) {
				idx--; // in wavefront obj all indices start at 1, not zero
				indices.push_back(idx);
			}
		}
	}
}

void Mesh::draw(CudaRasterizer &renderer) const {
	const unsigned int verticesInTriangle = 3;
	
	renderer.setUseDepthBuffer(true);
	renderer.setCulling(cullType_backface);

	renderer.setShaderProgram(shader);
	renderer.setVertexBuffer(geometry.data(), geometry.size());

	if (indices.empty()) {
		indices.resize(geometry.size() * verticesInTriangle);
		for (int i = 0; i < indices.size(); ++i) {
			indices[i] = i;
		}
	}
	renderer.setIndexBuffer(indices.data(), indices.size());

	CUresult err = cuCtxSetLimit(CU_LIMIT_DEV_RUNTIME_PENDING_LAUNCH_COUNT, indices.size() / verticesInTriangle + 200);
	if (err != CUDA_SUCCESS) {
		return;
	}

	renderer.drawIndexed(indices.size() / verticesInTriangle);
}

void Scene::draw(CudaRasterizer &renderer) const {
	for (const auto &mesh : meshes) {
		mesh.draw(renderer);
	}
}
