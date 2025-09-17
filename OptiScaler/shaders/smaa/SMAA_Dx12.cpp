#include "SMAA_Dx12.h"

#include "precompile/SMAA_Edge_Shader.h"
#include "precompile/SMAA_Blend_Shader.h"
#include "precompile/SMAA_Neighborhood_Shader.h"
#include <wrl/client.h>

namespace
{
DXGI_FORMAT ResolveFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

constexpr float kDefaultThreshold = 0.075f;
constexpr float kDefaultBlendStrength = 0.6f;
} // namespace

SMAA_Dx12::SMAA_Dx12(std::string name, ID3D12Device* device) : _name(std::move(name)), _device(device)
{
    if (_device == nullptr)
        return;

    _descriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!CreateRootSignature())
        return;

    bool pipelinesCreated = CreatePipeline(&_edgePipeline, SMAA_Edge_cso, sizeof(SMAA_Edge_cso)) &&
                            CreatePipeline(&_blendPipeline, SMAA_Blend_cso, sizeof(SMAA_Blend_cso)) &&
                            CreatePipeline(&_neighborhoodPipeline, SMAA_Neighborhood_cso,
                                           sizeof(SMAA_Neighborhood_cso));

    _init = pipelinesCreated;
}

bool SMAA_Dx12::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE descriptorRanges[3] = {};

    descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRanges[0].NumDescriptors = 1;
    descriptorRanges[0].BaseShaderRegister = 0;
    descriptorRanges[0].RegisterSpace = 0;
    descriptorRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    descriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRanges[1].NumDescriptors = 1;
    descriptorRanges[1].BaseShaderRegister = 1;
    descriptorRanges[1].RegisterSpace = 0;
    descriptorRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    descriptorRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptorRanges[2].NumDescriptors = 1;
    descriptorRanges[2].BaseShaderRegister = 0;
    descriptorRanges[2].RegisterSpace = 0;
    descriptorRanges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[4] = {};

    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &descriptorRanges[0];
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &descriptorRanges[1];
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[2].DescriptorTable.pDescriptorRanges = &descriptorRanges[2];
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[3].Constants.Num32BitValues = sizeof(Constants) / sizeof(uint32_t);
    rootParameters[3].Constants.ShaderRegister = 0;
    rootParameters[3].Constants.RegisterSpace = 0;
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 4;
    rootDesc.pParameters = rootParameters;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob != nullptr)
            LOG_ERROR("[{0}] D3D12SerializeRootSignature error {1:x}: {2}", _name, (unsigned int) hr,
                      static_cast<const char*>(errorBlob->GetBufferPointer()));
        return false;
    }

    hr = _device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&_rootSignature));

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateRootSignature error {1:x}", _name, (unsigned int) hr);
        return false;
    }

    return true;
}

bool SMAA_Dx12::CreatePipeline(ID3D12PipelineState** pipeline, const unsigned char* shaderData, size_t shaderSize)
{
    if (shaderData == nullptr || shaderSize == 0)
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _rootSignature;
    desc.CS = { shaderData, shaderSize };

    HRESULT hr = _device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline));

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateComputePipelineState error {1:x}", _name, (unsigned int) hr);
        return false;
    }

    return true;
}

bool SMAA_Dx12::EnsureDescriptorHeap(int passIndex)
{
    if (_descriptorHeaps[passIndex] != nullptr)
        return true;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 3;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_descriptorHeaps[passIndex]));

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateDescriptorHeap error {1:x}", _name, (unsigned int) hr);
        return false;
    }

    auto cpuStart = _descriptorHeaps[passIndex]->GetCPUDescriptorHandleForHeapStart();
    _cpuSrvHandles[passIndex][0] = cpuStart;
    _cpuSrvHandles[passIndex][1].ptr = cpuStart.ptr + _descriptorSize;
    _cpuUavHandles[passIndex].ptr = cpuStart.ptr + _descriptorSize * 2;

    auto gpuStart = _descriptorHeaps[passIndex]->GetGPUDescriptorHandleForHeapStart();
    _gpuSrvHandles[passIndex][0] = gpuStart;
    _gpuSrvHandles[passIndex][1].ptr = gpuStart.ptr + _descriptorSize;
    _gpuUavHandles[passIndex].ptr = gpuStart.ptr + _descriptorSize * 2;

    return true;
}

bool SMAA_Dx12::EnsureTextures(ID3D12Resource* inputColor)
{
    if (inputColor == nullptr)
        return false;

    auto desc = inputColor->GetDesc();

    CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    HRESULT hr = S_OK;

    if (inputColor != nullptr)
    {
        D3D12_HEAP_PROPERTIES inputHeapProperties = {};
        D3D12_HEAP_FLAGS inputHeapFlags = D3D12_HEAP_FLAG_NONE;

        hr = inputColor->GetHeapProperties(&inputHeapProperties, &inputHeapFlags);

        if (SUCCEEDED(hr))
        {
            bool isDefaultHeap = inputHeapProperties.Type == D3D12_HEAP_TYPE_DEFAULT ||
                                 inputHeapProperties.Type == D3D12_HEAP_TYPE_CUSTOM;

            if (isDefaultHeap)
            {
                heapProperties = inputHeapProperties;
                heapFlags = inputHeapFlags;
            }
        }
    }

    auto createTexture = [&](ID3D12Resource** target, DXGI_FORMAT format, D3D12_RESOURCE_STATES& state) -> bool {
        if (*target != nullptr)
        {
            auto texDesc = (*target)->GetDesc();
            if (texDesc.Width == desc.Width && texDesc.Height == desc.Height && texDesc.Format == format)
                return true;
            (*target)->Release();
            *target = nullptr;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = desc.Width;
        texDesc.Height = desc.Height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = format;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        hr = _device->CreateCommittedResource(&heapProperties, heapFlags, &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              nullptr, IID_PPV_ARGS(target));

        if (FAILED(hr))
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) hr);
            return false;
        }

        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    };

    if (!createTexture(&_edgeTexture, DXGI_FORMAT_R16G16_FLOAT, _edgeState))
        return false;

    if (!createTexture(&_blendTexture, DXGI_FORMAT_R16G16_FLOAT, _blendState))
        return false;

    DXGI_FORMAT outputFormat = ResolveFormat(desc.Format);
    if (!createTexture(&_outputTexture, outputFormat, _outputState))
        return false;

    return true;
}

void SMAA_Dx12::TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                                   D3D12_RESOURCE_STATES& currentState, D3D12_RESOURCE_STATES targetState)
{
    if (resource == nullptr || currentState == targetState)
        return;

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, currentState, targetState);
    commandList->ResourceBarrier(1, &barrier);
    currentState = targetState;
}

void SMAA_Dx12::UpdateConstants(ID3D12GraphicsCommandList* commandList, UINT width, UINT height)
{
    _constants.invWidth = width > 0 ? 1.0f / static_cast<float>(width) : 0.0f;
    _constants.invHeight = height > 0 ? 1.0f / static_cast<float>(height) : 0.0f;
    _constants.threshold = kDefaultThreshold;
    _constants.blendStrength = kDefaultBlendStrength;
    _constants.pad0 = 0.0f;
    _constants.pad1 = 0.0f;

    commandList->SetComputeRoot32BitConstants(3, sizeof(Constants) / sizeof(uint32_t), &_constants, 0);
}

void SMAA_Dx12::PopulateDescriptors(ID3D12Device* device, ID3D12Resource* colorResource)
{
    auto colorDesc = colorResource->GetDesc();
    DXGI_FORMAT colorFormat = ResolveFormat(colorDesc.Format);

    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = {};
    colorSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    colorSrvDesc.Texture2D.MipLevels = 1;
    colorSrvDesc.Format = colorFormat;

    D3D12_SHADER_RESOURCE_VIEW_DESC intermediateSrv = colorSrvDesc;
    intermediateSrv.Format = DXGI_FORMAT_R16G16_FLOAT;

    D3D12_UNORDERED_ACCESS_VIEW_DESC edgeUavDesc = {};
    edgeUavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    edgeUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc = {};
    outputUavDesc.Format = colorFormat;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    // Edge detection pass descriptors
    device->CreateShaderResourceView(colorResource, &colorSrvDesc, _cpuSrvHandles[0][0]);
    device->CreateShaderResourceView(_edgeTexture, &intermediateSrv, _cpuSrvHandles[0][1]);
    device->CreateUnorderedAccessView(_edgeTexture, nullptr, &edgeUavDesc, _cpuUavHandles[0]);

    // Blend weights pass descriptors
    device->CreateShaderResourceView(colorResource, &colorSrvDesc, _cpuSrvHandles[1][0]);
    device->CreateShaderResourceView(_edgeTexture, &intermediateSrv, _cpuSrvHandles[1][1]);
    device->CreateUnorderedAccessView(_blendTexture, nullptr, &edgeUavDesc, _cpuUavHandles[1]);

    // Neighborhood blending pass descriptors
    device->CreateShaderResourceView(colorResource, &colorSrvDesc, _cpuSrvHandles[2][0]);
    device->CreateShaderResourceView(_blendTexture, &intermediateSrv, _cpuSrvHandles[2][1]);
    device->CreateUnorderedAccessView(_outputTexture, nullptr, &outputUavDesc, _cpuUavHandles[2]);
}

bool SMAA_Dx12::CreateBufferResources(ID3D12Resource* colorResource)
{
    if (!_init || colorResource == nullptr)
        return false;

    if (!EnsureTextures(colorResource))
        return false;

    for (int i = 0; i < 3; i++)
    {
        if (!EnsureDescriptorHeap(i))
            return false;
    }

    PopulateDescriptors(_device, colorResource);

    return true;
}

bool SMAA_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* colorResource)
{
    if (!_init || commandList == nullptr || colorResource == nullptr)
        return false;

    if (!CreateBufferResources(colorResource))
        return false;

    auto desc = colorResource->GetDesc();
    UINT width = static_cast<UINT>(desc.Width);
    UINT height = desc.Height;
    UINT dispatchX = (width + 7) / 8;
    UINT dispatchY = (height + 7) / 8;

    ID3D12DescriptorHeap* heap = nullptr;

    commandList->SetComputeRootSignature(_rootSignature);

    UpdateConstants(commandList, width, height);

    // Edge detection
    heap = _descriptorHeaps[0];
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->SetComputeRootDescriptorTable(0, _gpuSrvHandles[0][0]);
    commandList->SetComputeRootDescriptorTable(1, _gpuSrvHandles[0][1]);
    commandList->SetComputeRootDescriptorTable(2, _gpuUavHandles[0]);

    TransitionResource(commandList, _edgeTexture, _edgeState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState(_edgePipeline);
    commandList->Dispatch(dispatchX, dispatchY, 1);

    TransitionResource(commandList, _edgeTexture, _edgeState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Blend weights
    heap = _descriptorHeaps[1];
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->SetComputeRootDescriptorTable(0, _gpuSrvHandles[1][0]);
    commandList->SetComputeRootDescriptorTable(1, _gpuSrvHandles[1][1]);
    commandList->SetComputeRootDescriptorTable(2, _gpuUavHandles[1]);

    TransitionResource(commandList, _blendTexture, _blendState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState(_blendPipeline);
    commandList->Dispatch(dispatchX, dispatchY, 1);

    TransitionResource(commandList, _blendTexture, _blendState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Neighborhood blending
    heap = _descriptorHeaps[2];
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->SetComputeRootDescriptorTable(0, _gpuSrvHandles[2][0]);
    commandList->SetComputeRootDescriptorTable(1, _gpuSrvHandles[2][1]);
    commandList->SetComputeRootDescriptorTable(2, _gpuUavHandles[2]);

    TransitionResource(commandList, _outputTexture, _outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState(_neighborhoodPipeline);
    commandList->Dispatch(dispatchX, dispatchY, 1);

    TransitionResource(commandList, _outputTexture, _outputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    return true;
}

SMAA_Dx12::~SMAA_Dx12()
{
    if (_edgeTexture != nullptr)
    {
        _edgeTexture->Release();
        _edgeTexture = nullptr;
    }

    if (_blendTexture != nullptr)
    {
        _blendTexture->Release();
        _blendTexture = nullptr;
    }

    if (_outputTexture != nullptr)
    {
        _outputTexture->Release();
        _outputTexture = nullptr;
    }

    for (auto& heap : _descriptorHeaps)
    {
        if (heap != nullptr)
        {
            heap->Release();
            heap = nullptr;
        }
    }

    if (_edgePipeline != nullptr)
    {
        _edgePipeline->Release();
        _edgePipeline = nullptr;
    }

    if (_blendPipeline != nullptr)
    {
        _blendPipeline->Release();
        _blendPipeline = nullptr;
    }

    if (_neighborhoodPipeline != nullptr)
    {
        _neighborhoodPipeline->Release();
        _neighborhoodPipeline = nullptr;
    }

    if (_rootSignature != nullptr)
    {
        _rootSignature->Release();
        _rootSignature = nullptr;
    }
}
