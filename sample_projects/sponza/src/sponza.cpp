#include "sponza.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "d3d12_app.h"
#include "d3d12_asset_manager.h"
#include "d3d12_pipeline_state.h"
#include "d3d12_resource_manager.h"
#include "d3d12_utils.h"
#include "scene_loader.h"

// TODO: To make things simple, child projects should not rely on third party software
// Expose some input controller interface or something like that.
#include "GLFW/glfw3.h" // keyboard input

// For loading the texture image
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "stb_image.h"

#include "imgui.h"

struct ImageData {
	void *data = nullptr;
	int width = 0;
	int height = 0;
	int ncomp = 0;
};

ImageData loadImage(const String &imgPath) {
	WString imgPathWStr(imgPath.begin(), imgPath.end());
	const WString fullPathWStr = getAssetFullPath(imgPathWStr.c_str(), AssetType::Texture);
	const SizeType bufferLen = fullPathWStr.size() * sizeof(wchar_t) + 1;
	char *path = new char[bufferLen];
	stbi_convert_wchar_to_utf8(path, bufferLen, fullPathWStr.c_str());

	ImageData result = {};
	result.data = stbi_load(path, &result.width, &result.height, nullptr, 4);
	result.ncomp = 4;

	return result;
}

Sponza::Sponza(const UINT w, const UINT h, const String &windowTitle) :
	D3D12App(w, h, windowTitle.c_str()),
	rtvHeapHandleIncrementSize(0),
	vertexBufferHandle(INVALID_RESOURCE_HANDLE),
	indexBufferHandle(INVALID_RESOURCE_HANDLE),
	vertexBufferView{},
	indexBufferView{},
	depthBufferHandle(INVALID_RESOURCE_HANDLE),
	sceneMatricesHandles{ INVALID_RESOURCE_HANDLE, INVALID_RESOURCE_HANDLE },
	textureHandles{ INVALID_RESOURCE_HANDLE },
	viewport{ 0.f, 0.f, static_cast<float>(w), static_cast<float>(h), 0.f, 1.f },
	scissorRect{ 0, 0, LONG_MAX, LONG_MAX }, // always render on the entire screen
	aspectRatio(static_cast<float>(w) / static_cast<float>(h)),
	fenceValues{ 0 },
	camControl(&cam, 100.f),
	cursorHidden(true),
	fps(0.0),
	totalTime(0.0),
	deltaTime(0.0)
{
	cam = Camera::perspectiveCamera(Vec3(0.f, 100.f, 0.f), 90.f, static_cast<float>(w) / static_cast<float>(h), 10.f, 10000.f);
}

int Sponza::initImpl() {
	setUseImGui();

	setGLFWCursorHiddenState();

	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_6 };
	RETURN_FALSE_ON_ERROR(
		device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)),
		"Device does not support shader model 6.6!"
	);

	if (shaderModel.HighestShaderModel != D3D_SHADER_MODEL_6_6) {
		fprintf(stderr, "Shader model 6.6 not supported!");
		return false;
	}

	/* Create a descriptor heap for RTVs */
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { };
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.NumDescriptors = frameCount;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;

	RETURN_FALSE_ON_ERROR(
		device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)),
		"Failed to create RTV descriptor heap!"
	);

	rtvHeapHandleIncrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = { };
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;

	RETURN_FALSE_ON_ERROR(
		device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)),
		"Failed to create DSV descriptor heap!"
	);
	
	if (!resizeDepthBuffer()) {
		return false;
	}

	if (!updateRenderTargetViews()) {
		return false;
	}

	return true;
}

void Sponza::deinit() {
	flush();
	Super::deinit();
}

void Sponza::update() {
	timeIt();

	camControl.processKeyboardInput(this, deltaTime);

	// Update VP matrices
	Mat4 viewMat = cam.getViewMatrix();
	Mat4 projectionMat = cam.getProjectionMatrix();

	struct SceneMatrices {
		Mat4 viewProjectionMat;
	} sceneMatrices;

	sceneMatrices.viewProjectionMat = projectionMat * viewMat;

	/// Initialize the MVP constant buffer resource if needed
	if (sceneMatricesHandles[frameIndex] == INVALID_RESOURCE_HANDLE) {
		wchar_t frameMVPName[32] = L"";
		swprintf(frameMVPName, 32, L"SceneMatrices[%d]", frameIndex);

		ResourceInitData resData(ResourceType::DataBuffer);
		resData.size = sizeof(SceneMatrices);
		resData.name = frameMVPName;
		sceneMatricesHandles[frameIndex] = resManager->createBuffer(resData);
	}

	UploadHandle uploadHandle = resManager->beginNewUpload();
	resManager->uploadBufferData(uploadHandle, sceneMatricesHandles[frameIndex], reinterpret_cast<void*>(&sceneMatrices), sizeof(SceneMatrices));
	resManager->uploadBuffers();
}

void Sponza::render() {
	commandQueueDirect.addCommandListForExecution(populateCommandList());
	fenceValues[frameIndex] = commandQueueDirect.executeCommandLists();

	const UINT syncInterval = vSyncEnabled ? 1 : 0;
	const UINT presentFlags = allowTearing && !vSyncEnabled ? DXGI_PRESENT_ALLOW_TEARING : 0;
	RETURN_ON_ERROR(swapChain->Present(syncInterval, presentFlags), ,"Failed to execute command list!");
	
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	// wait for the next frame's buffer
	commandQueueDirect.waitForFenceValue(fenceValues[frameIndex]);
}

void Sponza::drawUI() {
	ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);

	ImGui::Begin("Stats", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		ImGui::Text("FPS: %.2f", fps);
		ImGui::Text("Camera FOV: %.2f", cam.getFOV());
		ImGui::Text("Camera Speed: %.2f", camControl.getSpeed());
		Vec3 pos = cam.getPos();
		ImGui::Text("Camera Position: %.2f %.2f %.2f", pos.x, pos.y, pos.z);
		ImGui::Text("Camera Vectors:");
		Vec3 x = cam.getCameraX();
		Vec3 y = cam.getCameraY();
		Vec3 z = cam.getCameraZ();
		ImGui::Text("Right: %.2f %.2f %.2f", x.x, x.y, x.z);
		ImGui::Text("Up: %.2f %.2f %.2f", y.x, y.y, y.z);
		ImGui::Text("Forward: %.2f %.2f %.2f", z.x, z.y, z.z);
	ImGui::End();

	ImGui::SetNextWindowPos(ImVec2(0, 170.f), ImGuiCond_FirstUseEver);
	ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::Text("[mouse move] - Turn around");
	ImGui::Text("[mouse scroll] - Zoom/unzoom");
	ImGui::Text("[wasd] - Move forwards/left/backwards/right");
	ImGui::Text("[qe] - Move up/down");
	ImGui::Text("[rt] - Increase/Decrease camera speed");
	ImGui::Text("[k] - Make/Stop camera keeping on the plane of walking");
	ImGui::Text("[m] - Show/Hide mouse cursor");
	ImGui::End();
}

void Sponza::onResize(const unsigned int w, const unsigned int h) {
	if (width == w && height == h) {
		return;
	}

	this->width = std::max(1u, w);
	this->height = std::max(1u, h);
	viewport = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
	aspectRatio = static_cast<float>(width) / static_cast<float>(height);

	flush();

	for (unsigned int i = 0; i < frameCount; ++i) {
		backBuffers[i].Reset();
		// TODO: handle this automatically
		// It's important to deregister an outside resource if you want it deallocated
		// since the ResourceManager keeps a ref if it was registered with it.
		resManager->deregisterResource(backBuffersHandles[i]);
	}
	resManager->deregisterResource(depthBufferHandle);

	DXGI_SWAP_CHAIN_DESC scDesc = { };
	RETURN_ON_ERROR(
		swapChain->GetDesc(&scDesc), ,
		"Failed to retrieve swap chain's description"
	);
	RETURN_ON_ERROR(
		swapChain->ResizeBuffers(
			frameCount,
			this->width,
			this->height,
			scDesc.BufferDesc.Format,
			scDesc.Flags
		), ,
		"Failed to resize swap chain buffer"
	);

	frameIndex = swapChain->GetCurrentBackBufferIndex();

	updateRenderTargetViews();

	resizeDepthBuffer();
}

void Sponza::onKeyboardInput(int key, int action) {
	if (keyPressed[GLFW_KEY_F] && !keyRepeated[GLFW_KEY_F]) {
		toggleFullscreen();
	}

	if (keyPressed[GLFW_KEY_V] && !keyRepeated[GLFW_KEY_V]) {
		vSyncEnabled = !vSyncEnabled;
	}

	if (keyPressed[GLFW_KEY_O] && !keyRepeated[GLFW_KEY_O]) {
		projectionType = static_cast<ProjectionType>((static_cast<int>(projectionType) + 1) % 2);
	}

	ButtonState mState = query('m');
	if (mState.pressed && !mState.repeated) {
		cursorHidden = !cursorHidden;
		setGLFWCursorHiddenState();
	}
}

void Sponza::onMouseScroll(double xOffset, double yOffset) {
	camControl.onMouseScroll(xOffset, yOffset, deltaTime);
}

void Sponza::onMouseMove(double xPos, double yPos) {
	camControl.onMouseMove(xPos, yPos, deltaTime);
}

bool Sponza::loadAssets() {
	SceneLoaderError sceneLoadErr = loadScene("res\\scenes\\Sponza\\glTF\\Sponza.gltf", scene);
	if (sceneLoadErr != SceneLoaderError::Success) {
		dassert(false);
		return false;
	}

	scene.uploadSceneData();
	// TODO: upload textures through the scene, also

	/* Create shader resource view heap which will store the handles to the textures */
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.NumDescriptors = static_cast<UINT>(scene.getNumTextures()) + 2;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	srvHeapDesc.NodeMask = 0;
	RETURN_FALSE_ON_ERROR(
		device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)),
		"Failed to create DSV descriptor heap!"
	);

	D3D12_INPUT_ELEMENT_DESC inputLayouts[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};

	CD3DX12_STATIC_SAMPLER_DESC sampler{ D3D12_FILTER_MIN_MAG_MIP_POINT };
	PipelineStateDesc psDesc = {};
	psDesc.shaderName = L"basic";
	psDesc.shadersMask = sif_useVertex;
	psDesc.inputLayouts = inputLayouts;
	psDesc.staticSamplerDesc = &sampler;
	psDesc.numInputLayouts = _countof(inputLayouts);
	psDesc.depthStencilBufferFormat = DXGI_FORMAT_D32_FLOAT;
	psDesc.numConstantBufferViews = static_cast<unsigned int>(ConstantBufferView::Count);
	psDesc.numTextures = static_cast<UINT>(scene.getNumTextures());
	psDesc.maxVersion = rootSignatureFeatureData.HighestVersion;
	if (!pipelineState.init(device, psDesc)) {
		return false;
	}

	{
		/* Create the vertex buffer*/
		ResourceInitData vertData(ResourceType::DataBuffer);
		vertData.size = scene.getVertexBufferSize();
		vertData.name = L"VertexBuffer";
		vertexBufferHandle = resManager->createBuffer(vertData);
		if (vertexBufferHandle == INVALID_RESOURCE_HANDLE) {
			return false;
		}

		/* Create the index buffer*/
		ResourceInitData indexBufferData(ResourceType::DataBuffer);
		indexBufferData.size = scene.getIndexBufferSize();
		indexBufferData.name = L"IndexBuffer";
		indexBufferHandle = resManager->createBuffer(indexBufferData);
		if (indexBufferHandle == INVALID_RESOURCE_HANDLE) {
			return false;
		}

		/* Load the texture and create buffers for them */
		const SizeType numTextures = scene.getNumTextures();
		textureHandles.resize(numTextures);

		Vector<ImageData> texData(numTextures);
		Vector<ComPtr<ID3D12Resource>> stagingImageBuffers(numTextures);
		for (int i = 0; i < numTextures; ++i) {
			const Texture &tex = scene.getTexture(i);
			texData[i] = loadImage(tex.path);
			wchar_t textureName[32] = L"";
			swprintf(textureName, 32, L"Texture[%d]", i);

			ResourceInitData texInitData(ResourceType::TextureBuffer);
			texInitData.textureData.width = texData[i].width;
			texInitData.textureData.height = texData[i].height;
			texInitData.textureData.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			texInitData.name = textureName;
			textureHandles[i] = resManager->createBuffer(texInitData);
			if (textureHandles[i] == INVALID_RESOURCE_HANDLE) {
				return false;
			}
		}

		/* Upload the vertex, index and texture buffers */
		UploadHandle uploadHandle = resManager->beginNewUpload();

		resManager->uploadBufferData(uploadHandle, vertexBufferHandle, scene.getVertexBuffer(), scene.getVertexBufferSize());
		resManager->uploadBufferData(uploadHandle, indexBufferHandle, scene.getIndexBuffer(), scene.getIndexBufferSize());

		for (int i = 0; i < numTextures; ++i) {
			D3D12_SUBRESOURCE_DATA textureSubresources = {};
			textureSubresources.pData = texData[i].data;
			textureSubresources.RowPitch = static_cast<UINT64>(texData[i].width) * static_cast<UINT64>(texData[i].ncomp);
			textureSubresources.SlicePitch = textureSubresources.RowPitch * texData[i].height;
			resManager->uploadTextureData(uploadHandle, textureHandles[i], &textureSubresources, 1, 0);
		}

		resManager->uploadBuffers();

		/* Create views for the vertex and index buffers */
		{
			vertexBufferView.BufferLocation = vertexBufferHandle->GetGPUVirtualAddress();
			vertexBufferView.SizeInBytes = static_cast<UINT>(scene.getVertexBufferSize());
			vertexBufferView.StrideInBytes = sizeof(Vertex);

			indexBufferView.BufferLocation = indexBufferHandle->GetGPUVirtualAddress();
			indexBufferView.SizeInBytes = static_cast<UINT>(scene.getIndexBufferSize());
			indexBufferView.Format = DXGI_FORMAT_R32_UINT;
		}

		SizeType srvHeapHandleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();

		// Create SRV for the lights
		if (scene.getNumLights() > 0) {
			D3D12_SHADER_RESOURCE_VIEW_DESC lightsSrvDesc = {};
			lightsSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
			lightsSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			lightsSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			lightsSrvDesc.Buffer = {};
			lightsSrvDesc.Buffer.FirstElement = 0;
			lightsSrvDesc.Buffer.NumElements = scene.getNumLights();
			lightsSrvDesc.Buffer.StructureByteStride = sizeof(Light);
			device->CreateShaderResourceView(scene.lightsHandle.get(), &lightsSrvDesc, descriptorHandle);
		}
		descriptorHandle.ptr += srvHeapHandleSize;

		// Create SRV for the materials
		if (scene.getNumMaterials() > 0) {
			D3D12_SHADER_RESOURCE_VIEW_DESC materialsSrvDesc = {};
			materialsSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
			materialsSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			materialsSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			materialsSrvDesc.Buffer = {};
			materialsSrvDesc.Buffer.FirstElement = 0;
			materialsSrvDesc.Buffer.NumElements = scene.getNumMaterials();
			materialsSrvDesc.Buffer.StructureByteStride = sizeof(GPUMaterial);
			device->CreateShaderResourceView(scene.materialsHandle.get(), &materialsSrvDesc, descriptorHandle);
		}
		descriptorHandle.ptr += srvHeapHandleSize;

		/* Create SRVs for the textures so we can read them bindlessly in the shader */
		for (int i = 0; i < numTextures; ++i) {
			dassert(texData[i].ncomp == 4);

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = texData[i].ncomp == 4 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Texture2D.MipLevels = 1;

			device->CreateShaderResourceView(textureHandles[i].get(), &srvDesc, descriptorHandle);
			descriptorHandle.ptr += srvHeapHandleSize;
		}
	}

	return true;
}

CommandList Sponza::populateCommandList() {
	CommandList commandList = commandQueueDirect.getCommandList();

	if (!commandList.isValid()) {
		return commandList;
	}

	commandList->SetPipelineState(pipelineState.getPipelineState());

	commandList.transition(scene.lightsHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList.transition(scene.materialsHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	const SizeType numTextures = textureHandles.size();
	for (int i = 0; i < numTextures; ++i) {
		commandList.transition(textureHandles[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	commandList->SetDescriptorHeaps(1, srvHeap.GetAddressOf());

	commandList->SetGraphicsRootSignature(pipelineState.getRootSignature());

	commandList->RSSetViewports(1, &viewport);
	commandList->RSSetScissorRects(1, &scissorRect);

	commandList.transition(backBuffersHandles[frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<int>(frameIndex), rtvHeapHandleIncrementSize);
	const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();

	constexpr float blue[] = { 0.2f, 0.2f, 0.8f, 1.f };
	//constexpr float red[] = { 1.f, 0.2f, 0.2f, 1.f };
	const float *clearColor = blue;
	commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	commandList.transition(depthBufferHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
	
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	commandList.transition(vertexBufferHandle, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	commandList.transition(indexBufferHandle, D3D12_RESOURCE_STATE_INDEX_BUFFER);
	commandList.transition(sceneMatricesHandles[frameIndex], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

	commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
	commandList->IASetIndexBuffer(&indexBufferView);
	commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
	commandList.setMVPBuffer(sceneMatricesHandles[frameIndex]);

	scene.draw(commandList);

	renderUI(commandList, rtvHandle);

	commandList.transition(backBuffersHandles[frameIndex], D3D12_RESOURCE_STATE_PRESENT);

	return commandList;
}

bool Sponza::updateRenderTargetViews() {
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());

	for (UINT i = 0; i < frameCount; ++i) {
		RETURN_FALSE_ON_ERROR_FMT(
			swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])),
			"Failed to create Render-Target-View for buffer %u!", i
		);
		device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, rtvHandle);
		rtvHandle.Offset(static_cast<int>(rtvHeapHandleIncrementSize));

		// Register the back buffer's resources manually since the resource manager doesn't own them, the swap chain does.
#ifdef D3D12_DEBUG
		backBuffersHandles[i] = resManager->registerResource(backBuffers[i].Get(), 1, D3D12_RESOURCE_STATE_PRESENT, ResourceType::RenderTargetView);
#else
		backBuffersHandles[i] = resManager->registerResource(backBuffers[i].Get(), 1, D3D12_RESOURCE_STATE_PRESENT);
#endif

		wchar_t backBufferName[32];
		swprintf(backBufferName, 32, L"BackBuffer[%u]", i);
		backBuffers[i]->SetName(backBufferName);
	}

	return true;
}

bool Sponza::resizeDepthBuffer() {
	width = std::max(1u, width);
	height = std::max(1u, height);

	ResourceInitData resData(ResourceType::DepthStencilBuffer);
	resData.textureData.width = width;
	resData.textureData.height = height;
	resData.textureData.format = DXGI_FORMAT_D32_FLOAT;

	depthBufferHandle = resManager->createBuffer(resData);

	D3D12_DEPTH_STENCIL_VIEW_DESC dsDesc = {};
	dsDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsDesc.Texture2D.MipSlice = 0;

	device->CreateDepthStencilView(depthBufferHandle.get(), &dsDesc, dsvHeap->GetCPUDescriptorHandleForHeapStart());

	return true;
}

void Sponza::setGLFWCursorHiddenState() {
	glfwSetInputMode(getGLFWWindow(), GLFW_CURSOR, cursorHidden ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Sponza::timeIt() {
	using std::chrono::duration_cast;
	using Hrc = std::chrono::high_resolution_clock;

	static constexpr double seconds_in_nanosecond = 1e-9;
	static UINT64 frameCount = 0;
	static double elapsedTime = 0.0;
	static Hrc::time_point t0 = Hrc::now();

	const Hrc::time_point t1 = Hrc::now();
	deltaTime = static_cast<double>((t1 - t0).count()) * seconds_in_nanosecond;
	elapsedTime += deltaTime;
	totalTime += deltaTime;
	
	++frameCount;
	t0 = t1;

	if (elapsedTime > 1.0) {
		fps = static_cast<double>(frameCount) / elapsedTime;

#if defined(D3D12_DEBUG)
		char buffer[512];
		sprintf_s(buffer, "FPS: %.2f\n", fps);
		OutputDebugString(buffer);
#endif // defined(D3D12_DEBUG)

		frameCount = 0;
		elapsedTime = 0.0;
	}
}
