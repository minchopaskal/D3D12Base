#include "d3d12_pipeline_state.h"

#include "d3d12_asset_manager.h"
#include "d3d12_math.h"
#include "d3dx12.h"

#include <d3dcompiler.h>

PipelineStateStream::PipelineStateStream() {
}

void* PipelineStateStream::getData() {
	return data.data();
}

size_t PipelineStateStream::getSize() const {
	return data.size();
}

PipelineState::PipelineState() { }

struct D3D12Empty { };

bool PipelineState::init(const ComPtr<ID3D12Device2> &device, PipelineStateStream &pss) {
	if (!initPipeline(device, pss)) {
		return false;
	}

	using EmptyToken = PipelineStateStreamToken<D3D12Empty, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID>;

	UINT8 *pipelineData = reinterpret_cast<UINT8*>(pss.getData());
	while (pipelineData != pipelineData + pss.getSize()) {
		EmptyToken *emptyToken = reinterpret_cast<EmptyToken*>(pipelineData); // some template magick

		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *type = reinterpret_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(pipelineData);
		if (*type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
			pipelineData += sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE);
			this->rootSignature = ComPtr<ID3D12RootSignature>{ reinterpret_cast<ID3D12RootSignature*>(pipelineData) };
			break;
		}

		pipelineData += emptyToken->getUnderlyingSize();
	}

	return true;
}

bool PipelineState::init(const ComPtr<ID3D12Device2> &device, const PipelineStateDesc &desc) {
	PipelineStateStream stream;

	auto mask = desc.shadersMask;
	auto &base = desc.shaderName;
	auto *rootSignatureFlags = desc.rootSignatureFlags;
	D3D12_ROOT_SIGNATURE_FLAGS rsFlags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
	
	if (mask & sif_useVertex) {
		rsFlags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	}
	if (rootSignatureFlags != nullptr) {
		rsFlags |= *rootSignatureFlags;
	}

	ComPtr<ID3DBlob> psShader;
	RETURN_FALSE_ON_ERROR(
		D3DReadFileToBlob(
		getAssetFullPath((base + L"_ps.bin").c_str(), AssetType::shader).c_str(),
		&psShader
	),
		"Failed to read pixel shader!"
	);
	stream.insert(PixelShaderToken(D3D12_SHADER_BYTECODE{ psShader->GetBufferPointer(), psShader->GetBufferSize() }));

	ComPtr<ID3DBlob> vsShader;
	if (mask & sif_useVertex) {
		RETURN_FALSE_ON_ERROR(
			D3DReadFileToBlob(
			getAssetFullPath((base + L"_vs.bin").c_str(), AssetType::shader).c_str(),
			&vsShader
		),
			"Failed to read vertex shader!"
		);
		stream.insert(VertexShaderToken({ vsShader->GetBufferPointer(), vsShader->GetBufferSize() }));
	} else {
		rsFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
	}

	ComPtr<ID3DBlob> geomShader;
	if (mask & sif_useGeometry) {
		RETURN_FALSE_ON_ERROR(
			D3DReadFileToBlob(
				getAssetFullPath((base + L"_gs.bin").c_str(), AssetType::shader).c_str(),
				&geomShader
			),
			"Failed to read geometry shader!"
		);
		stream.insert(GeometryShaderToken({ geomShader->GetBufferPointer(), geomShader->GetBufferSize() }));
	} else {
		rsFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
	}

	ComPtr<ID3DBlob> domShader;
	if (mask & sif_useDomain) {
		RETURN_FALSE_ON_ERROR(
			D3DReadFileToBlob(
				getAssetFullPath((base + L"_ds.bin").c_str(), AssetType::shader).c_str(),
				&domShader
			),
			"Failed to read domain shader!"
		);
		stream.insert(DomainShaderToken({ domShader->GetBufferPointer(), domShader->GetBufferSize() }));
	} else {
		rsFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
	}

	ComPtr<ID3DBlob> hullShader;
	if (mask & sif_useHull) {
		RETURN_FALSE_ON_ERROR(
			D3DReadFileToBlob(
				getAssetFullPath((base + L"_hs.bin").c_str(), AssetType::shader).c_str(),
				&hullShader
			),
			"Failed to read hull shader!"
		);
		stream.insert(HullShaderToken({ hullShader->GetBufferPointer(), hullShader->GetBufferSize() }));
	} else {
		rsFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
	}

	ComPtr<ID3DBlob> compShader;
	if (mask & sif_useCompute) {
		RETURN_FALSE_ON_ERROR(
			D3DReadFileToBlob(
				getAssetFullPath((base + L"_cs.bin").c_str(), AssetType::shader).c_str(),
				&compShader
			),
			"Failed to read compute shader!"
		);
		stream.insert(ComputeShaderToken({ compShader->GetBufferPointer(), compShader->GetBufferSize() }));
	}

	ComPtr<ID3DBlob> meshShader;
	if (mask & sif_useMesh) {
		RETURN_FALSE_ON_ERROR(
			D3DReadFileToBlob(
				getAssetFullPath((base + L"_ms.bin").c_str(), AssetType::shader).c_str(),
				&meshShader
			),
			"Failed to read mesh shader!"
		);
		stream.insert(MeshShaderToken({ meshShader->GetBufferPointer(), meshShader->GetBufferSize() }));
	} else {
		rsFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS;
	}

	ComPtr<ID3DBlob> ampShader;
	if (mask & sif_useAmplification) {
		RETURN_FALSE_ON_ERROR(
			D3DReadFileToBlob(
				getAssetFullPath((base + L"_gs.bin").c_str(), AssetType::shader).c_str(),
				&ampShader
			),
			"Failed to read amplification shader!"
		);
		stream.insert(AmplificationShaderToken({ ampShader->GetBufferPointer(), ampShader->GetBufferSize() }));
	} else {
		rsFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS;
	}

	int numConstantBufferViews = dmath::min(UINT(15), desc.numConstantBufferViews);
	Vector<CD3DX12_ROOT_PARAMETER1> rsParams(numConstantBufferViews);
	for (int i = 0; i < numConstantBufferViews; ++i) {
		rsParams[i].InitAsConstantBufferView(i);
	}

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
	rootSignatureDesc.Init_1_1(numConstantBufferViews, rsParams.data(), 0, nullptr, rsFlags);

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;
	// TODO: read error if any
	RETURN_FALSE_ON_ERROR(
		D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, desc.maxVersion, &signature, &error),
		"Failed to create root signature!\n"
	);

	RETURN_FALSE_ON_ERROR(
		device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature)),
		"Failed to create root signature!\n"
	);

	stream.insert(RootSignatureToken{ rootSignature.Get() });

	stream.insert(PrimitiveTopologyToken{ D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE });
	
	D3D12_RT_FORMAT_ARRAY rtFormat = {};
	rtFormat.NumRenderTargets = 1;
	rtFormat.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	stream.insert(RTFormatsToken{ rtFormat });

	stream.insert(DepthStencilFormatToken{ DXGI_FORMAT_D32_FLOAT });

	if (desc.inputLayouts) {
		stream.insert(InputLayoutToken(D3D12_INPUT_LAYOUT_DESC{ desc.inputLayouts, desc.numInputLayouts }));
	}

	return initPipeline(device, stream);
}

ID3D12PipelineState* PipelineState::getPipelineState() {
	return pipelineState.Get();
}

ID3D12RootSignature* PipelineState::getRootSignature() {
	return rootSignature.Get();
}

bool PipelineState::initPipeline(const ComPtr<ID3D12Device2> &device, PipelineStateStream &pss) {
	D3D12_PIPELINE_STATE_STREAM_DESC pipelineDesc = {};
	pipelineDesc.pPipelineStateSubobjectStream = pss.getData();
	pipelineDesc.SizeInBytes = pss.getSize();

	RETURN_FALSE_ON_ERROR(
		device->CreatePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipelineState)),
		"Failed to create pipeline state!\n"
	);

	return true;
}
