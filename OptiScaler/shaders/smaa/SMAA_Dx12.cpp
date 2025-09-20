#include "SMAA_Dx12.h"

#include <Logger.h>
#include <Util.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3dx/d3dx12.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <numeric>
#include <string>

#include <vector>

namespace
{
    constexpr UINT kSrvDescriptorCount = 4;
    constexpr UINT kUavDescriptorCount = 8;
    constexpr UINT kDispatchArgsCount = 4;
    constexpr UINT kEdgeKernelSizeX = 14; // CMAA2_CS_INPUT_KERNEL_SIZE_X - 2
    constexpr UINT kEdgeKernelSizeY = 14; // CMAA2_CS_INPUT_KERNEL_SIZE_Y - 2

    void ResetHandles(SMAAResourceHandles& handles)
    {
        handles.cpu.ptr = 0;
        handles.gpu.ptr = 0;
    }

    void ResetHandleTable(std::array<SMAAResourceHandles, kSrvDescriptorCount>& table)
    {
        for (auto& entry : table)
        {
            ResetHandles(entry);
        }
    }

    void ResetHandleTable(std::array<SMAAResourceHandles, kUavDescriptorCount>& table)
    {
        for (auto& entry : table)
        {
            ResetHandles(entry);
        }
    }

    DXGI_FORMAT TranslateTypelessFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
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

    bool IsSRGB(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }

    DXGI_FORMAT StripSRGB(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:
            return format;
        }
    }

    bool IsFloatFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return true;
        default:
            return false;
        }
    }
}

SMAA_Dx12::SMAA_Dx12(const char* name, ID3D12Device* device)
    : _name(name ? name : "SMAA"), _device(device)
{
    ResetHandleTable(_srvTable);
    ResetHandleTable(_uavTable);

    if (_device == nullptr)
    {
        LOG_WARN("[{}] SMAA DirectX 12 device is null - SMAA will be disabled", _name);
        _init = false;
    }
    else
    {
        auto basePath = Util::DllPath();
        if (basePath.empty())
        {
            LOG_WARN("[{}] Failed to resolve OptiScaler shader directory", _name);
        }
        else
        {
            _shaderDirectory = basePath.parent_path() / "shaders" / "smaa" / "CMAA project" / "CMAA2";
        }

        _srvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        _uavDescriptorSize = _srvDescriptorSize;

        LOG_INFO("[{}] SMAA DirectX 12 path enabling CMAA2 shader preparation", _name);
        _init = true;
    }
}

bool SMAA_Dx12::CreateBufferResources(ID3D12Resource* sourceTexture)
{
    if (!_init)
    {
        return false;
    }

    if (sourceTexture == nullptr)
    {
        LOG_WARN("[{}] CreateBufferResources called with null source texture", _name);
        _buffersReady = false;
        _processedResource = nullptr;
        _inputResource = nullptr;
        return false;
    }

    const D3D12_RESOURCE_DESC desc = sourceTexture->GetDesc();

    const bool dimensionsChanged = (_cachedInputDesc.Width != desc.Width) ||
                                   (_cachedInputDesc.Height != desc.Height) ||
                                   (_cachedInputDesc.Format != desc.Format) ||
                                   (_cachedInputDesc.DepthOrArraySize != desc.DepthOrArraySize);

    if (!_buffersReady || dimensionsChanged)
    {
        LOG_INFO("[{}] Updating SMAA DX12 buffers to {}x{} (format={})", _name, desc.Width, desc.Height, static_cast<int>(desc.Format));

        _edgeBuffer.Reset();
        _deferredHeadsBuffer.Reset();
        _shapeCandidatesBuffer.Reset();
        _deferredLocationBuffer.Reset();
        _deferredItemBuffer.Reset();
        _controlBuffer.Reset();
        _dispatchArgsBuffer.Reset();

        _edgePipeline.Reset();
        _processPipeline.Reset();
        _deferredPipeline.Reset();
        _dispatchArgsPipeline.Reset();
        _rootSignature.Reset();
        _srvHeap.Reset();
        _uavHeap.Reset();
        _shadersReady = false;
        ResetHandleTable(_srvTable);
        ResetHandleTable(_uavTable);
        _shaderConfig = {};
        _colorSrvDesc = {};
        _colorUavDesc = {};

        _cachedInputDesc = desc;

        _buffersReady = EnsureDescriptorHeaps();
        if (_buffersReady)
        {
            _buffersReady = UpdateInputDescriptors(sourceTexture, desc);
        }
        if (_buffersReady)
        {
            _buffersReady = EnsureIntermediateResources(desc) && EnsureShaders(desc);
        }

        if (!_buffersReady)
        {
            LOG_ERROR("[{}] Failed to allocate CMAA2 intermediate resources", _name);
            return false;
        }
    }

    if (!_buffersReady)
    {
        return false;
    }

    if (!UpdateInputDescriptors(sourceTexture, desc))
    {
        return false;
    }

    _inputResource = sourceTexture;
    _processedResource = sourceTexture;
    _currentInputState = D3D12_RESOURCE_STATE_COMMON;

    return true;
}

bool SMAA_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceTexture)
{
    if (!_init)
    {
        return false;
    }

    if (commandList == nullptr)
    {
        LOG_WARN("[{}] Dispatch called with null command list", _name);
        return false;
    }

    if (sourceTexture == nullptr)
    {
        LOG_WARN("[{}] Dispatch called with null source texture", _name);
        return false;
    }

    if (!_buffersReady || sourceTexture != _inputResource)
    {
        if (!CreateBufferResources(sourceTexture))
        {
            return false;
        }
    }

    if (sourceTexture == nullptr)
    {
        LOG_WARN("[{}] Dispatch called with null source texture", _name);
        return false;
    }

    if (!_buffersReady || sourceTexture != _inputResource)
    {
        if (!CreateBufferResources(sourceTexture))
        {
            return false;
        }
    }

    if (!_edgePipeline || !_dispatchArgsPipeline || !_processPipeline || !_deferredPipeline || !_rootSignature ||
        !_commandSignature)
    {
        LOG_ERROR("[{}] CMAA2 pipeline state missing", _name);
        return false;
    }

    auto transitionInput = [&](D3D12_RESOURCE_STATES newState) {
        if (_currentInputState != newState)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = sourceTexture;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = _currentInputState;
            barrier.Transition.StateAfter = newState;
            commandList->ResourceBarrier(1, &barrier);
            _currentInputState = newState;
        }
    };

    transitionInput(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const UINT width = static_cast<UINT>(_cachedInputDesc.Width);
    const UINT height = static_cast<UINT>(_cachedInputDesc.Height);
    const UINT groupCountX = (width + kEdgeKernelSizeX * 2 - 1) / (kEdgeKernelSizeX * 2);
    const UINT groupCountY = (height + kEdgeKernelSizeY * 2 - 1) / (kEdgeKernelSizeY * 2);

    const UINT clearValues[4] = { 0, 0, 0, 0 };
    if (_controlBuffer)
    {
        commandList->ClearUnorderedAccessViewUint(_uavTable[6].gpu, _uavTable[6].cpu, _controlBuffer.Get(), clearValues, 0, nullptr);
    }
    if (_dispatchArgsBuffer)
    {
        commandList->ClearUnorderedAccessViewUint(_uavTable[7].gpu, _uavTable[7].cpu, _dispatchArgsBuffer.Get(), clearValues, 0, nullptr);
    }

    // Avoid UAV binding on the input color during the initial passes
    _device->CreateUnorderedAccessView(nullptr, nullptr, nullptr, _uavTable[0].cpu);

    ID3D12DescriptorHeap* heaps[] = { _srvHeap.Get(), _uavHeap.Get() };
    commandList->SetDescriptorHeaps(static_cast<UINT>(sizeof(heaps) / sizeof(heaps[0])), heaps);
    commandList->SetComputeRootSignature(_rootSignature.Get());
    commandList->SetComputeRootDescriptorTable(0, _srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootDescriptorTable(1, _uavHeap->GetGPUDescriptorHandleForHeapStart());

    commandList->SetPipelineState(_edgePipeline.Get());
    commandList->Dispatch(groupCountX, groupCountY, 1);

    auto emitUavBarrier = [&](ID3D12Resource* resource) {
        if (resource == nullptr)
        {
            return;
        }
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        commandList->ResourceBarrier(1, &barrier);
    };

    emitUavBarrier(_edgeBuffer.Get());
    emitUavBarrier(_shapeCandidatesBuffer.Get());
    emitUavBarrier(_deferredLocationBuffer.Get());
    emitUavBarrier(_deferredItemBuffer.Get());
    emitUavBarrier(_controlBuffer.Get());

    commandList->SetPipelineState(_dispatchArgsPipeline.Get());
    commandList->Dispatch(2, 1, 1);

    emitUavBarrier(_dispatchArgsBuffer.Get());
    emitUavBarrier(_controlBuffer.Get());

    auto transitionArgs = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (_dispatchArgsBuffer)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = _dispatchArgsBuffer.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            commandList->ResourceBarrier(1, &barrier);
        }
    };

    transitionArgs(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    commandList->SetPipelineState(_processPipeline.Get());
    commandList->ExecuteIndirect(_commandSignature.Get(), 1, _dispatchArgsBuffer.Get(), 0, nullptr, 0);

    transitionArgs(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    emitUavBarrier(_shapeCandidatesBuffer.Get());
    emitUavBarrier(_deferredLocationBuffer.Get());
    emitUavBarrier(_deferredItemBuffer.Get());
    emitUavBarrier(_deferredHeadsBuffer.Get());

    commandList->SetPipelineState(_dispatchArgsPipeline.Get());
    commandList->Dispatch(1, 2, 1);

    emitUavBarrier(_dispatchArgsBuffer.Get());
    emitUavBarrier(_controlBuffer.Get());

    transitionArgs(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // Prepare to write back into the color buffer
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Format = DXGI_FORMAT_R8_UNORM;
    nullSrv.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(nullptr, &nullSrv, _srvTable[0].cpu);
    _device->CreateUnorderedAccessView(sourceTexture, nullptr, &_colorUavDesc, _uavTable[0].cpu);

    transitionInput(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    emitUavBarrier(_dispatchArgsBuffer.Get());

    commandList->SetPipelineState(_deferredPipeline.Get());
    commandList->ExecuteIndirect(_commandSignature.Get(), 1, _dispatchArgsBuffer.Get(), 0, nullptr, 0);

    transitionArgs(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    emitUavBarrier(sourceTexture);

    // Restore descriptors for the next frame
    _device->CreateShaderResourceView(sourceTexture, &_colorSrvDesc, _srvTable[0].cpu);
    _device->CreateUnorderedAccessView(sourceTexture, nullptr, &_colorUavDesc, _uavTable[0].cpu);

    _processedResource = sourceTexture;

    return true;
}

bool SMAA_Dx12::EnsureDescriptorHeaps()
{
    if (_srvHeap && _uavHeap)
    {
        return true;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    srvDesc.NumDescriptors = kSrvDescriptorCount;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    if (FAILED(_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(_srvHeap.ReleaseAndGetAddressOf()))))
    {
        LOG_ERROR("[{}] Failed to create SMAA SRV descriptor heap", _name);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC uavDesc = {};
    uavDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    uavDesc.NumDescriptors = kUavDescriptorCount;
    uavDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    if (FAILED(_device->CreateDescriptorHeap(&uavDesc, IID_PPV_ARGS(_uavHeap.ReleaseAndGetAddressOf()))))
    {
        LOG_ERROR("[{}] Failed to create SMAA UAV descriptor heap", _name);
        _srvHeap.Reset();
        return false;
    }

    ResetHandleTable(_srvTable);
    ResetHandleTable(_uavTable);

    return true;
}

bool SMAA_Dx12::EnsureIntermediateResources(const D3D12_RESOURCE_DESC& inputDesc)
{
    if (_edgeBuffer && _deferredHeadsBuffer && _shapeCandidatesBuffer && _deferredLocationBuffer && _deferredItemBuffer &&
        _controlBuffer && _dispatchArgsBuffer)
    {
        return true;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto getHeapHandle = [&](UINT index) {
        return DescriptorFromIndex(_uavHeap, index);
    };

    // Edge buffer is packed to half width for single-sample targets
    UINT64 edgeWidth = inputDesc.Width;
    if (inputDesc.SampleDesc.Count <= 1)
    {
        edgeWidth = (edgeWidth + 1) / 2;
    }

    D3D12_RESOURCE_DESC edgeDesc = {};
    edgeDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    edgeDesc.Width = edgeWidth;
    edgeDesc.Height = inputDesc.Height;
    edgeDesc.DepthOrArraySize = 1;
    edgeDesc.MipLevels = 1;
    edgeDesc.Format = (inputDesc.SampleDesc.Count > 4) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8_UINT;
    if (inputDesc.SampleDesc.Count == 4)
    {
        edgeDesc.Format = DXGI_FORMAT_R16_UINT;
    }
    edgeDesc.SampleDesc.Count = 1;
    edgeDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    edgeDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &edgeDesc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(_edgeBuffer.ReleaseAndGetAddressOf()))))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 edge buffer", _name);
        return false;
    }

    auto edgeHandles = getHeapHandle(1);
    _device->CreateUnorderedAccessView(_edgeBuffer.Get(), nullptr, nullptr, edgeHandles.cpu);
    _uavTable[1] = edgeHandles;

    D3D12_RESOURCE_DESC headsDesc = {};
    headsDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    headsDesc.Width = (inputDesc.Width + 1) / 2;
    headsDesc.Height = (inputDesc.Height + 1) / 2;
    headsDesc.DepthOrArraySize = 1;
    headsDesc.MipLevels = 1;
    headsDesc.Format = DXGI_FORMAT_R32_UINT;
    headsDesc.SampleDesc.Count = 1;
    headsDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    headsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &headsDesc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(_deferredHeadsBuffer.ReleaseAndGetAddressOf()))))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 deferred heads buffer", _name);
        _edgeBuffer.Reset();
        return false;
    }

    auto headsHandles = getHeapHandle(5);
    _device->CreateUnorderedAccessView(_deferredHeadsBuffer.Get(), nullptr, nullptr, headsHandles.cpu);
    _uavTable[5] = headsHandles;

    auto createBuffer = [&](UINT index, UINT64 byteWidth, UINT structureStride, bool rawView) -> bool {
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = std::max<UINT64>(structureStride, byteWidth);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        Microsoft::WRL::ComPtr<ID3D12Resource>* target = nullptr;
        switch (index)
        {
        case 2:
            target = &_shapeCandidatesBuffer;
            break;
        case 3:
            target = &_deferredLocationBuffer;
            break;
        case 4:
            target = &_deferredItemBuffer;
            break;
        case 6:
            target = &_controlBuffer;
            break;
        case 7:
            target = &_dispatchArgsBuffer;
            break;
        default:
            break;
        }

        if (target == nullptr)
        {
            LOG_ERROR("[{}] Invalid CMAA2 buffer index {}", _name, index);
            return false;
        }

        if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                    IID_PPV_ARGS(target->ReleaseAndGetAddressOf()))))
        {
            LOG_ERROR("[{}] Failed to create CMAA2 working buffer {}", _name, index);
            return false;
        }

        auto handles = getHeapHandle(index);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        if (rawView)
        {
            uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            uavDesc.Buffer.StructureByteStride = 0;
        }
        else
        {
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.Buffer.StructureByteStride = structureStride;
        }
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = static_cast<UINT>(bufferDesc.Width / (rawView ? 4 : structureStride));

        _device->CreateUnorderedAccessView(target->Get(), nullptr, &uavDesc, handles.cpu);
        _uavTable[index] = handles;
        return true;
    };

    UINT width = static_cast<UINT>(inputDesc.Width);
    UINT height = static_cast<UINT>(inputDesc.Height);
    UINT requiredCandidatePixels = std::max<UINT>(1u, (width * height) / 4);
    UINT requiredDeferredColorApplyBuffer = std::max<UINT>(1u, (width * height) / 2);
    UINT requiredListHeadsPixels = std::max<UINT>(1u, (width * height + 3) / 6);

    if (!createBuffer(2, static_cast<UINT64>(requiredCandidatePixels) * sizeof(UINT), sizeof(UINT), false))
    {
        return false;
    }
    if (!createBuffer(3, static_cast<UINT64>(requiredListHeadsPixels) * sizeof(UINT), sizeof(UINT), false))
    {
        return false;
    }
    if (!createBuffer(4, static_cast<UINT64>(requiredDeferredColorApplyBuffer) * sizeof(UINT) * 2, sizeof(UINT) * 2, false))
    {
        return false;
    }
    if (!createBuffer(6, kDispatchArgsCount * sizeof(UINT), sizeof(UINT), true))
    {
        return false;
    }
    if (!createBuffer(7, kDispatchArgsCount * sizeof(UINT), sizeof(UINT), true))
    {
        return false;
    }

    return true;
}

SMAAResourceHandles SMAA_Dx12::DescriptorFromIndex(const Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& heap, UINT index) const
{
    SMAAResourceHandles handles;
    if (!heap)
    {
        return handles;
    }

    UINT descriptorSize = (heap.Get() == _srvHeap.Get()) ? _srvDescriptorSize : _uavDescriptorSize;
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpu(heap->GetCPUDescriptorHandleForHeapStart(), index, descriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpu(heap->GetGPUDescriptorHandleForHeapStart(), index, descriptorSize);
    handles.cpu = cpu;
    handles.gpu = gpu;
    return handles;
}

bool SMAA_Dx12::UpdateInputDescriptors(ID3D12Resource* sourceTexture, const D3D12_RESOURCE_DESC& inputDesc)
{
    if (!sourceTexture || !_srvHeap || !_uavHeap)
    {
        return false;
    }

    _shaderConfig = {};
    _shaderConfig.colorFormat = inputDesc.Format;

    DXGI_FORMAT srvFormat = TranslateTypelessFormat(inputDesc.Format);
    DXGI_FORMAT uavFormat = srvFormat;

    bool isSRGB = IsSRGB(srvFormat);
    if (isSRGB)
    {
        uavFormat = StripSRGB(uavFormat);
    }

    bool typedStoreSupported = false;
    if (uavFormat != DXGI_FORMAT_UNKNOWN)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { uavFormat, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
        if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        {
            typedStoreSupported = ((support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0) &&
                                 ((support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0);
        }
    }

    DXGI_FORMAT finalSrvFormat = srvFormat;
    DXGI_FORMAT finalUavFormat = uavFormat;

    if (typedStoreSupported)
    {
        _shaderConfig.typedStore = true;
        _shaderConfig.convertToSRGB = false;
        _shaderConfig.typedStoreIsUnorm = !IsFloatFormat(uavFormat);
    }
    else
    {
        finalUavFormat = DXGI_FORMAT_R32_UINT;
        _shaderConfig.typedStore = false;
        _shaderConfig.convertToSRGB = isSRGB;

        DXGI_FORMAT stripped = StripSRGB(srvFormat);
        if (stripped == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            _shaderConfig.untypedStoreMode = 1;
        }
        else if (stripped == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            _shaderConfig.untypedStoreMode = 2;
        }
        else
        {
            LOG_ERROR("[{}] Unsupported CMAA2 format for untyped UAV store ({})", _name, static_cast<int>(stripped));
            return false;
        }
    }

    _shaderConfig.hdrInput = IsFloatFormat(srvFormat);
    _shaderConfig.srvFormat = finalSrvFormat;
    _shaderConfig.uavFormat = finalUavFormat;

    _colorSrvDesc = {};
    _colorSrvDesc.Format = finalSrvFormat;
    _colorSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    _colorSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    _colorSrvDesc.Texture2D.MipLevels = 1;
    _colorSrvDesc.Texture2D.MostDetailedMip = 0;

    auto colorSrv = DescriptorFromIndex(_srvHeap, 0);
    _device->CreateShaderResourceView(sourceTexture, &_colorSrvDesc, colorSrv.cpu);
    _srvTable[0] = colorSrv;

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Format = DXGI_FORMAT_R8_UNORM;
    nullSrv.Texture2D.MipLevels = 1;

    for (UINT i = 1; i < kSrvDescriptorCount; ++i)
    {
        auto handle = DescriptorFromIndex(_srvHeap, i);
        _device->CreateShaderResourceView(nullptr, &nullSrv, handle.cpu);
        _srvTable[i] = handle;
    }

    _colorUavDesc = {};
    _colorUavDesc.Format = finalUavFormat;
    _colorUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    auto colorUav = DescriptorFromIndex(_uavHeap, 0);
    _device->CreateUnorderedAccessView(sourceTexture, nullptr, &_colorUavDesc, colorUav.cpu);
    _uavTable[0] = colorUav;

    return true;
}

bool SMAA_Dx12::EnsureShaders(const D3D12_RESOURCE_DESC& inputDesc)
{
    if (_shaderConfig.srvFormat == DXGI_FORMAT_UNKNOWN)
    {
        DXGI_FORMAT srvFormat = TranslateTypelessFormat(inputDesc.Format);
        _shaderConfig.srvFormat = srvFormat;
    }

    if (_shadersReady && _compiledFormat == _shaderConfig.srvFormat)
    {
        return true;
    }

    if (_shaderDirectory.empty())
    {
        LOG_ERROR("[{}] CMAA2 shader directory not resolved", _name);
        return false;
    }

    std::filesystem::path shaderPath = _shaderDirectory / "CMAA2.hlsl";
    if (!std::filesystem::exists(shaderPath))
    {
        LOG_ERROR("[{}] CMAA2 shader file missing: {}", _name, shaderPath.string());
        return false;
    }

    std::vector<std::pair<std::string, std::string>> macroPairs;
    macroPairs.emplace_back("CMAA2_STATIC_QUALITY_PRESET", "2");
    macroPairs.emplace_back("CMAA2_EXTRA_SHARPNESS", "0");
    macroPairs.emplace_back("CMAA2_EDGE_DETECTION_LUMA_PATH", "1");
    macroPairs.emplace_back("CMAA_MSAA_SAMPLE_COUNT", std::to_string(inputDesc.SampleDesc.Count));

    if (_shaderConfig.typedStore)
    {
        macroPairs.emplace_back("CMAA2_UAV_STORE_TYPED", "1");
        macroPairs.emplace_back("CMAA2_UAV_STORE_TYPED_UNORM_FLOAT", _shaderConfig.typedStoreIsUnorm ? "1" : "0");
        macroPairs.emplace_back("CMAA2_UAV_STORE_CONVERT_TO_SRGB", _shaderConfig.convertToSRGB ? "1" : "0");
    }
    else
    {
        macroPairs.emplace_back("CMAA2_UAV_STORE_TYPED", "0");
        macroPairs.emplace_back("CMAA2_UAV_STORE_CONVERT_TO_SRGB", _shaderConfig.convertToSRGB ? "1" : "0");
        macroPairs.emplace_back("CMAA2_UAV_STORE_TYPED_UNORM_FLOAT", "0");
        macroPairs.emplace_back("CMAA2_UAV_STORE_UNTYPED_FORMAT", std::to_string(_shaderConfig.untypedStoreMode));
    }

    macroPairs.emplace_back("CMAA2_SUPPORT_HDR_COLOR_RANGE", _shaderConfig.hdrInput ? "1" : "0");

    std::vector<D3D_SHADER_MACRO> macros;
    std::vector<std::string> macroNameStorage;
    std::vector<std::string> macroValueStorage;
    macroNameStorage.reserve(macroPairs.size());
    macroValueStorage.reserve(macroPairs.size());
    macros.reserve(macroPairs.size() + 1);

    for (const auto& entry : macroPairs)
    {
        macroNameStorage.emplace_back(entry.first);
        macroValueStorage.emplace_back(entry.second);
        macros.push_back({ macroNameStorage.back().c_str(), macroValueStorage.back().c_str() });
    }
    macros.push_back({ nullptr, nullptr });

    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileShader = [&](const char* entryPoint, Microsoft::WRL::ComPtr<ID3DBlob>& blob) -> bool {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompileFromFile(shaderPath.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint,
                                        "cs_5_1", compileFlags, 0, &blob, &errors);
        if (FAILED(hr))
        {
            if (errors)
            {
                LOG_ERROR("[{}] CMAA2 shader compile error ({}): {}", _name, entryPoint,
                          static_cast<const char*>(errors->GetBufferPointer()));
            }
            else
            {
                LOG_ERROR("[{}] CMAA2 shader compile failed ({}, hr={:x})", _name, entryPoint, hr);
            }
            return false;
        }
        return true;
    };

    Microsoft::WRL::ComPtr<ID3DBlob> edgesCS;
    Microsoft::WRL::ComPtr<ID3DBlob> dispatchArgsCS;
    Microsoft::WRL::ComPtr<ID3DBlob> processCS;
    Microsoft::WRL::ComPtr<ID3DBlob> deferredCS;

    if (!compileShader("EdgesColor2x2CS", edgesCS) || !compileShader("ComputeDispatchArgsCS", dispatchArgsCS) ||
        !compileShader("ProcessCandidatesCS", processCS) || !compileShader("DeferredColorApply2x2CS", deferredCS))
    {
        return false;
    }

    CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);
    CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0);
    std::array<CD3DX12_ROOT_PARAMETER, 2> rootParams;
    rootParams[0].InitAsDescriptorTable(1, &srvRange);
    rootParams[1].InitAsDescriptorTable(1, &uavRange);

    CD3DX12_STATIC_SAMPLER_DESC samplerDesc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = static_cast<UINT>(rootParams.size());
    rootDesc.pParameters = rootParams.data();
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &samplerDesc;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr))
    {
        if (errors)
        {
            LOG_ERROR("[{}] Failed to serialize CMAA2 root signature: {}", _name,
                      static_cast<const char*>(errors->GetBufferPointer()));
        }
        else
        {
            LOG_ERROR("[{}] Failed to serialize CMAA2 root signature (hr={:x})", _name, hr);
        }
        return false;
    }

    hr = _device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(_rootSignature.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 root signature (hr={:x})", _name, hr);
        return false;
    }

    if (!_commandSignature)
    {
        D3D12_INDIRECT_ARGUMENT_DESC argumentDesc = {};
        argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

        D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc = {};
        commandSignatureDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        commandSignatureDesc.NumArgumentDescs = 1;
        commandSignatureDesc.pArgumentDescs = &argumentDesc;

        hr = _device->CreateCommandSignature(&commandSignatureDesc, nullptr,
                                             IID_PPV_ARGS(_commandSignature.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ERROR("[{}] Failed to create CMAA2 command signature (hr={:x})", _name, hr);
            return false;
        }
    }

    auto createPipeline = [&](Microsoft::WRL::ComPtr<ID3DBlob>& shaderBlob,
                              Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = _rootSignature.Get();
        psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };
        HRESULT localHr = _device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipeline));
        if (FAILED(localHr))
        {
            LOG_ERROR("[{}] Failed to create CMAA2 pipeline state (hr={:x})", _name, localHr);
            return false;
        }
        return true;
    };

    if (!createPipeline(edgesCS, _edgePipeline) || !createPipeline(dispatchArgsCS, _dispatchArgsPipeline) ||
        !createPipeline(processCS, _processPipeline) || !createPipeline(deferredCS, _deferredPipeline))
    {
        _edgePipeline.Reset();
        _dispatchArgsPipeline.Reset();
        _processPipeline.Reset();
        _deferredPipeline.Reset();
        _rootSignature.Reset();
        return false;
    }

    _compiledFormat = _shaderConfig.srvFormat;
    _shadersReady = true;
    LOG_INFO("[{}] Compiled CMAA2 shaders for format {}", _name, static_cast<int>(_shaderConfig.srvFormat));
    return true;
}

