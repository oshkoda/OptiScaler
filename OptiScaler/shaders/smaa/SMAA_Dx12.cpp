#include "SMAA_Dx12.h"

#include "precompile/SMAA_Edge_Shader.h"
#include "precompile/SMAA_Blend_Shader.h"
#include "precompile/SMAA_Neighborhood_Shader.h"
#include "AreaTex.h"
#include "SearchTex.h"
#include <wrl/client.h>
#include <limits>

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

bool IsFloatFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

bool SupportsTypedUavStore(ID3D12Device* device, DXGI_FORMAT format)
{
    if (device == nullptr || format == DXGI_FORMAT_UNKNOWN)
        return false;

    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = {};
    formatSupport.Format = format;

    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport));

    if (FAILED(hr))
        return false;

    return (formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

constexpr float kDefaultThreshold = std::numeric_limits<float>::epsilon();
constexpr float kDefaultEdgeIntensity = 1.0f;
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

    if (_init)
    {
        LOG_INFO("[{0}] SMAA Dx12 initialized (device={1:X})", _name, (size_t) _device);
    }
    else
    {
        LOG_WARN("[{0}] SMAA Dx12 initialization incomplete (device={1:X})", _name, (size_t) _device);
    }
}

bool SMAA_Dx12::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE descriptorRanges[2] = {};

    descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRanges[0].NumDescriptors = 4;
    descriptorRanges[0].BaseShaderRegister = 0;
    descriptorRanges[0].RegisterSpace = 0;
    descriptorRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    descriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptorRanges[1].NumDescriptors = 1;
    descriptorRanges[1].BaseShaderRegister = 0;
    descriptorRanges[1].RegisterSpace = 0;
    descriptorRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[3] = {};

    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &descriptorRanges[0];
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &descriptorRanges[1];
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[2].Constants.Num32BitValues = sizeof(Constants) / sizeof(uint32_t);
    rootParameters[2].Constants.ShaderRegister = 0;
    rootParameters[2].Constants.RegisterSpace = 0;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].RegisterSpace = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    staticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].ShaderRegister = 1;
    staticSamplers[1].RegisterSpace = 0;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 3;
    rootDesc.pParameters = rootParameters;
    rootDesc.NumStaticSamplers = 2;
    rootDesc.pStaticSamplers = staticSamplers;
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
    heapDesc.NumDescriptors = 5;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_descriptorHeaps[passIndex]));

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateDescriptorHeap error {1:x}", _name, (unsigned int) hr);
        return false;
    }

    auto cpuStart = _descriptorHeaps[passIndex]->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 4; ++i)
    {
        _cpuSrvHandles[passIndex][i].ptr = cpuStart.ptr + _descriptorSize * i;
    }
    _cpuUavHandles[passIndex].ptr = cpuStart.ptr + _descriptorSize * 4;

    auto gpuStart = _descriptorHeaps[passIndex]->GetGPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 4; ++i)
    {
        _gpuSrvHandles[passIndex][i].ptr = gpuStart.ptr + _descriptorSize * i;
    }
    _gpuUavHandles[passIndex].ptr = gpuStart.ptr + _descriptorSize * 4;

    return true;
}

bool SMAA_Dx12::EnsureTextures(ID3D12Resource* inputColor)
{
    if (inputColor == nullptr)
        return false;

    auto desc = inputColor->GetDesc();

    CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES heapProperties = defaultHeapProperties;
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    HRESULT hr = S_OK;

    if (inputColor != nullptr)
    {
        D3D12_HEAP_PROPERTIES inputHeapProperties = {};
        D3D12_HEAP_FLAGS inputHeapFlags = D3D12_HEAP_FLAG_NONE;

        hr = inputColor->GetHeapProperties(&inputHeapProperties, &inputHeapFlags);

        if (SUCCEEDED(hr))
        {
            bool canUseInputHeap = inputHeapProperties.Type == D3D12_HEAP_TYPE_DEFAULT;

            if (canUseInputHeap)
            {
                heapProperties = inputHeapProperties;
                heapFlags = inputHeapFlags;
            }
            else
            {
                heapProperties = defaultHeapProperties;
                heapFlags = D3D12_HEAP_FLAG_NONE;
            }
        }
        else
        {
            LOG_ERROR("[{0}] GetHeapProperties error {1:x}", _name, (unsigned int) hr);
            heapProperties = defaultHeapProperties;
            heapFlags = D3D12_HEAP_FLAG_NONE;
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

    if (!createTexture(&_blendTexture, DXGI_FORMAT_R16G16B16A16_FLOAT, _blendState))
        return false;

    DXGI_FORMAT outputFormat = ResolveFormat(desc.Format);
    DXGI_FORMAT outputTextureFormat = outputFormat;

    if (!SupportsTypedUavStore(_device, outputFormat) && !IsFloatFormat(outputFormat))
    {
        LOG_DEBUG("[{0}] EnsureTextures fallback UAV format for {1}x{2}: {3} -> DXGI_FORMAT_R16G16B16A16_FLOAT", _name,
                  static_cast<unsigned long long>(desc.Width), desc.Height, (int) outputFormat);
        outputTextureFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    _outputTextureFormat = outputTextureFormat;

    if (!createTexture(&_outputTexture, outputTextureFormat, _outputState))
        return false;

    return true;
}

bool SMAA_Dx12::EnsureLookupTextures(ID3D12GraphicsCommandList* commandList)
{
    if (_device == nullptr)
        return false;

    if (_areaTexture != nullptr && _searchTexture != nullptr)
        return true;

    if (commandList == nullptr)
        return false;

    auto createLookup = [&](ID3D12Resource** texture, ID3D12Resource** upload, D3D12_RESOURCE_STATES& state,
                            DXGI_FORMAT format, UINT width, UINT height, UINT pitch, const void* data,
                            const char* label) -> bool {
        if (*texture == nullptr)
        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height);
            CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
            HRESULT hr = _device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(texture));

            if (FAILED(hr))
            {
                LOG_ERROR("[{0}] CreateCommittedResource ({1}) failed {2:x}", _name, label, (unsigned int) hr);
                return false;
            }

            state = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        if (*upload == nullptr)
        {
            UINT64 uploadSize = GetRequiredIntermediateSize(*texture, 0, 1);
            auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);

            CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
            HRESULT hr = _device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(upload));

            if (FAILED(hr))
            {
                LOG_ERROR("[{0}] CreateCommittedResource (upload {1}) failed {2:x}", _name, label, (unsigned int) hr);
                return false;
            }
        }

        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = data;
        subresource.RowPitch = pitch;
        subresource.SlicePitch = static_cast<UINT64>(pitch) * height;

        UpdateSubresources(commandList, *texture, *upload, 0, 0, 1, &subresource);
        TransitionResource(commandList, *texture, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        return true;
    };

    bool areaOk = createLookup(&_areaTexture, &_areaUpload, _areaState, DXGI_FORMAT_R8G8_UNORM, AREATEX_WIDTH,
                               AREATEX_HEIGHT, AREATEX_PITCH, areaTexBytes, "area");
    bool searchOk = createLookup(&_searchTexture, &_searchUpload, _searchState, DXGI_FORMAT_R8_UNORM, SEARCHTEX_WIDTH,
                                 SEARCHTEX_HEIGHT, SEARCHTEX_PITCH, searchTexBytes, "search");

    return areaOk && searchOk;
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

void SMAA_Dx12::UpdateConstants(ID3D12GraphicsCommandList* commandList, UINT width, UINT height,
                                float sharedFactor)
{
    _constants = {};
    _constants.invResolution[0] = width > 0 ? 1.0f / static_cast<float>(width) : 0.0f;
    _constants.invResolution[1] = height > 0 ? 1.0f / static_cast<float>(height) : 0.0f;
    _constants.threshold = kDefaultThreshold;
    _constants.sharedFactor = sharedFactor;

    commandList->SetComputeRoot32BitConstants(2, sizeof(Constants) / sizeof(uint32_t), &_constants, 0);
}

void SMAA_Dx12::PopulateDescriptors(ID3D12Device* device, ID3D12Resource* colorResource)
{
    auto colorDesc = colorResource->GetDesc();
    DXGI_FORMAT colorFormat = ResolveFormat(colorDesc.Format);

    if (_outputTextureFormat == DXGI_FORMAT_UNKNOWN)
        _outputTextureFormat = colorFormat;

    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = {};
    colorSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    colorSrvDesc.Texture2D.MipLevels = 1;
    colorSrvDesc.Format = colorFormat;

    D3D12_SHADER_RESOURCE_VIEW_DESC edgeSrvDesc = colorSrvDesc;
    edgeSrvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;

    D3D12_SHADER_RESOURCE_VIEW_DESC blendSrvDesc = colorSrvDesc;
    blendSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12_SHADER_RESOURCE_VIEW_DESC areaSrvDesc = colorSrvDesc;
    areaSrvDesc.Format = DXGI_FORMAT_R8G8_UNORM;

    D3D12_SHADER_RESOURCE_VIEW_DESC searchSrvDesc = colorSrvDesc;
    searchSrvDesc.Format = DXGI_FORMAT_R8_UNORM;

    D3D12_UNORDERED_ACCESS_VIEW_DESC edgeUavDesc = {};
    edgeUavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    edgeUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_UNORDERED_ACCESS_VIEW_DESC blendUavDesc = {};
    blendUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    blendUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc = {};
    outputUavDesc.Format = _outputTextureFormat;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    // Edge detection pass descriptors
    device->CreateShaderResourceView(colorResource, &colorSrvDesc, _cpuSrvHandles[0][0]);
    device->CreateShaderResourceView(_edgeTexture, &edgeSrvDesc, _cpuSrvHandles[0][1]);
    device->CreateShaderResourceView(_areaTexture, &areaSrvDesc, _cpuSrvHandles[0][2]);
    device->CreateShaderResourceView(_searchTexture, &searchSrvDesc, _cpuSrvHandles[0][3]);
    device->CreateUnorderedAccessView(_edgeTexture, nullptr, &edgeUavDesc, _cpuUavHandles[0]);

    // Blend weights pass descriptors
    device->CreateShaderResourceView(_edgeTexture, &edgeSrvDesc, _cpuSrvHandles[1][0]);
    device->CreateShaderResourceView(_areaTexture, &areaSrvDesc, _cpuSrvHandles[1][1]);
    device->CreateShaderResourceView(_searchTexture, &searchSrvDesc, _cpuSrvHandles[1][2]);
    device->CreateShaderResourceView(colorResource, &colorSrvDesc, _cpuSrvHandles[1][3]);
    device->CreateUnorderedAccessView(_blendTexture, nullptr, &blendUavDesc, _cpuUavHandles[1]);

    // Neighborhood blending pass descriptors
    device->CreateShaderResourceView(colorResource, &colorSrvDesc, _cpuSrvHandles[2][0]);
    device->CreateShaderResourceView(_blendTexture, &blendSrvDesc, _cpuSrvHandles[2][1]);
    device->CreateShaderResourceView(_areaTexture, &areaSrvDesc, _cpuSrvHandles[2][2]);
    device->CreateShaderResourceView(_searchTexture, &searchSrvDesc, _cpuSrvHandles[2][3]);
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

    if (_areaTexture != nullptr && _searchTexture != nullptr)
        PopulateDescriptors(_device, colorResource);

    return true;
}

bool SMAA_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* colorResource)
{
    if (!_init)
    {
        LOG_DEBUG("[{0}] Dispatch skipped: not initialized", _name);
        return false;
    }

    if (commandList == nullptr)
    {
        LOG_DEBUG("[{0}] Dispatch skipped: command list is null", _name);
        return false;
    }

    if (colorResource == nullptr)
    {
        LOG_DEBUG("[{0}] Dispatch skipped: color resource is null", _name);
        return false;
    }

    if (!EnsureLookupTextures(commandList))
    {
        LOG_DEBUG("[{0}] Dispatch aborted: lookup textures unavailable", _name);
        return false;
    }

    if (!CreateBufferResources(colorResource))
    {
        LOG_DEBUG("[{0}] Dispatch aborted: CreateBufferResources failed", _name);
        return false;
    }

    auto desc = colorResource->GetDesc();
    UINT width = static_cast<UINT>(desc.Width);
    UINT height = desc.Height;
    UINT dispatchX = (width + 7) / 8;
    UINT dispatchY = (height + 7) / 8;

    LOG_DEBUG("[{0}] Dispatch begin {1}x{2}, format={3}", _name, width, height, (int) desc.Format);

    ID3D12DescriptorHeap* heap = nullptr;

    commandList->SetComputeRootSignature(_rootSignature);

    TransitionResource(commandList, _areaTexture, _areaState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(commandList, _searchTexture, _searchState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Edge detection
    heap = _descriptorHeaps[0];
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->SetComputeRootDescriptorTable(0, _gpuSrvHandles[0][0]);
    commandList->SetComputeRootDescriptorTable(1, _gpuUavHandles[0]);

    TransitionResource(commandList, _edgeTexture, _edgeState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    UpdateConstants(commandList, width, height, kDefaultEdgeIntensity);

    commandList->SetPipelineState(_edgePipeline);
    commandList->Dispatch(dispatchX, dispatchY, 1);

    TransitionResource(commandList, _edgeTexture, _edgeState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Blend weights
    heap = _descriptorHeaps[1];
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->SetComputeRootDescriptorTable(0, _gpuSrvHandles[1][0]);
    commandList->SetComputeRootDescriptorTable(1, _gpuUavHandles[1]);

    TransitionResource(commandList, _blendTexture, _blendState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    UpdateConstants(commandList, width, height, kDefaultBlendStrength);

    commandList->SetPipelineState(_blendPipeline);
    commandList->Dispatch(dispatchX, dispatchY, 1);

    TransitionResource(commandList, _blendTexture, _blendState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Neighborhood blending
    heap = _descriptorHeaps[2];
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->SetComputeRootDescriptorTable(0, _gpuSrvHandles[2][0]);
    commandList->SetComputeRootDescriptorTable(1, _gpuUavHandles[2]);

    TransitionResource(commandList, _outputTexture, _outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    UpdateConstants(commandList, width, height, kDefaultBlendStrength);

    commandList->SetPipelineState(_neighborhoodPipeline);
    commandList->Dispatch(dispatchX, dispatchY, 1);

    TransitionResource(commandList, _outputTexture, _outputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    LOG_DEBUG("[{0}] Dispatch complete {1}x{2}", _name, width, height);

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

    if (_areaTexture != nullptr)
    {
        _areaTexture->Release();
        _areaTexture = nullptr;
    }

    if (_searchTexture != nullptr)
    {
        _searchTexture->Release();
        _searchTexture = nullptr;
    }

    if (_areaUpload != nullptr)
    {
        _areaUpload->Release();
        _areaUpload = nullptr;
    }

    if (_searchUpload != nullptr)
    {
        _searchUpload->Release();
        _searchUpload = nullptr;
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
