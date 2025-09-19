#include "SMAA_Dx12.h"

#include <Logger.h>
#include <Util.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3dx/d3dx12.h>

#include <array>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    void ResetHandles(SMAAResourceHandles& handles)
    {
        handles.cpu.ptr = 0;
        handles.gpu.ptr = 0;
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
    ResetHandles(_inputColorSrv);
    ResetHandles(_edgeBufferUav);
    ResetHandles(_blendBufferUav);
    ResetHandles(_processedColorSrv);

    if (_device == nullptr)
    {
        LOG_WARN("[{}] SMAA DirectX 12 device is null - SMAA will be disabled", _name);
        _init = false;
    }
    else
    {
        std::error_code ec;
        auto basePath = Util::DllPath(ec);
        if (ec)
        {
            LOG_WARN("[{}] Failed to resolve OptiScaler shader directory ({})", _name, ec.value());
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
        _blendBuffer.Reset();
        _edgePipeline.Reset();
        _processPipeline.Reset();
        _deferredPipeline.Reset();
        _dispatchArgsPipeline.Reset();
        _rootSignature.Reset();
        _srvHeap.Reset();
        _uavHeap.Reset();
        _shadersReady = false;
        ResetHandles(_inputColorSrv);
        ResetHandles(_edgeBufferUav);
        ResetHandles(_blendBufferUav);
        ResetHandles(_processedColorSrv);

        _cachedInputDesc = desc;
        _buffersReady = EnsureDescriptorHeaps() && EnsureIntermediateResources(desc) && EnsureShaders(desc);

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

    UpdateInputDescriptors(sourceTexture, desc);

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

    if (_currentInputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = sourceTexture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = _currentInputState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        commandList->ResourceBarrier(1, &barrier);
        _currentInputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = sourceTexture;
    commandList->ResourceBarrier(1, &uavBarrier);

    if (_currentInputState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = sourceTexture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = _currentInputState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &barrier);
        _currentInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

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
    srvDesc.NumDescriptors = 4;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    if (FAILED(_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&_srvHeap))))
    {
        LOG_ERROR("[{}] Failed to create SMAA SRV descriptor heap", _name);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC uavDesc = {};
    uavDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    uavDesc.NumDescriptors = 8;
    uavDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    if (FAILED(_device->CreateDescriptorHeap(&uavDesc, IID_PPV_ARGS(&_uavHeap))))
    {
        LOG_ERROR("[{}] Failed to create SMAA UAV descriptor heap", _name);
        _srvHeap.Reset();
        return false;
    }

    ResetHandles(_inputColorSrv);
    ResetHandles(_processedColorSrv);
    ResetHandles(_edgeBufferUav);
    ResetHandles(_blendBufferUav);

    return true;
}

bool SMAA_Dx12::EnsureIntermediateResources(const D3D12_RESOURCE_DESC& inputDesc)
{
    if (_edgeBuffer && _blendBuffer)
    {
        return true;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

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
                                                IID_PPV_ARGS(&_edgeBuffer))))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 edge buffer", _name);
        return false;
    }

    D3D12_RESOURCE_DESC blendDesc = {};
    blendDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    blendDesc.Width = (inputDesc.Width + 1) / 2;
    blendDesc.Height = (inputDesc.Height + 1) / 2;
    blendDesc.DepthOrArraySize = 1;
    blendDesc.MipLevels = 1;
    blendDesc.Format = DXGI_FORMAT_R32_UINT;
    blendDesc.SampleDesc.Count = 1;
    blendDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    blendDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &blendDesc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&_blendBuffer))))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 deferred blend buffer", _name);
        _edgeBuffer.Reset();
        return false;
    }

    // Create UAV descriptors
    auto edgeCpu = _uavHeap->GetCPUDescriptorHandleForHeapStart();
    auto edgeGpu = _uavHeap->GetGPUDescriptorHandleForHeapStart();
    _device->CreateUnorderedAccessView(_edgeBuffer.Get(), nullptr, nullptr, edgeCpu);
    _edgeBufferUav.cpu = edgeCpu;
    _edgeBufferUav.gpu = edgeGpu;

    D3D12_CPU_DESCRIPTOR_HANDLE blendCpu = { edgeCpu.ptr + _uavDescriptorSize };
    D3D12_GPU_DESCRIPTOR_HANDLE blendGpu = { edgeGpu.ptr + _uavDescriptorSize };
    _device->CreateUnorderedAccessView(_blendBuffer.Get(), nullptr, nullptr, blendCpu);
    _blendBufferUav.cpu = blendCpu;
    _blendBufferUav.gpu = blendGpu;

    return true;
}

void SMAA_Dx12::UpdateInputDescriptors(ID3D12Resource* sourceTexture, const D3D12_RESOURCE_DESC& inputDesc)
{
    if (!sourceTexture || !_srvHeap)
    {
        return;
    }

    DXGI_FORMAT srvFormat = TranslateTypelessFormat(inputDesc.Format);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = srvFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    auto cpuStart = _srvHeap->GetCPUDescriptorHandleForHeapStart();
    auto gpuStart = _srvHeap->GetGPUDescriptorHandleForHeapStart();

    _device->CreateShaderResourceView(sourceTexture, &srvDesc, cpuStart);
    _inputColorSrv.cpu = cpuStart;
    _inputColorSrv.gpu = gpuStart;

    D3D12_CPU_DESCRIPTOR_HANDLE processedCpu = { cpuStart.ptr + _srvDescriptorSize };
    D3D12_GPU_DESCRIPTOR_HANDLE processedGpu = { gpuStart.ptr + _srvDescriptorSize };
    _device->CreateShaderResourceView(sourceTexture, &srvDesc, processedCpu);
    _processedColorSrv.cpu = processedCpu;
    _processedColorSrv.gpu = processedGpu;
}

bool SMAA_Dx12::EnsureShaders(const D3D12_RESOURCE_DESC& inputDesc)
{
    DXGI_FORMAT srvFormat = TranslateTypelessFormat(inputDesc.Format);
    if (_shadersReady && srvFormat == _compiledFormat)
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

    DXGI_FORMAT stripped = StripSRGB(srvFormat);
    bool isSRGB = IsSRGB(srvFormat);
    bool hdrInput = IsFloatFormat(stripped);

    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { stripped, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
    bool typedStore = false;
    if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport))))
    {
        typedStore = (formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0 &&
                     (formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    }

    bool convertToSRGB = isSRGB;
    bool unormFloat = !hdrInput;

    std::vector<std::pair<std::string, std::string>> macroPairs;
    macroPairs.emplace_back("CMAA2_STATIC_QUALITY_PRESET", "2");
    macroPairs.emplace_back("CMAA2_EXTRA_SHARPNESS", "0");
    macroPairs.emplace_back("CMAA2_EDGE_DETECTION_LUMA_PATH", "1");

    if (typedStore)
    {
        macroPairs.emplace_back("CMAA2_UAV_STORE_TYPED", "1");
        macroPairs.emplace_back("CMAA2_UAV_STORE_TYPED_UNORM_FLOAT", unormFloat ? "1" : "0");
        macroPairs.emplace_back("CMAA2_UAV_STORE_CONVERT_TO_SRGB", convertToSRGB ? "1" : "0");
    }
    else
    {
        macroPairs.emplace_back("CMAA2_UAV_STORE_TYPED", "0");
        macroPairs.emplace_back("CMAA2_UAV_STORE_CONVERT_TO_SRGB", convertToSRGB ? "1" : "0");
        macroPairs.emplace_back("CMAA2_UAV_STORE_TYPED_UNORM_FLOAT", "0");

        switch (stripped)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            macroPairs.emplace_back("CMAA2_UAV_STORE_UNTYPED_FORMAT", "1");
            break;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            macroPairs.emplace_back("CMAA2_UAV_STORE_UNTYPED_FORMAT", "2");
            break;
        default:
            LOG_ERROR("[{}] Unsupported CMAA2 untyped format ({})", _name, static_cast<int>(stripped));
            return false;
        }
    }

    macroPairs.emplace_back("CMAA2_SUPPORT_HDR_COLOR_RANGE", hdrInput ? "1" : "0");

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
                                      IID_PPV_ARGS(&_rootSignature));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 root signature (hr={:x})", _name, hr);
        return false;
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

    _compiledFormat = srvFormat;
    _shadersReady = true;
    LOG_INFO("[{}] Compiled CMAA2 shaders for format {}", _name, static_cast<int>(srvFormat));
    return true;
}

