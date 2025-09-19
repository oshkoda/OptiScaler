#include "CMAA2_Dx11.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <Logger.h>
#include <Util.h>

#include <d3dcompiler.h>

#include "CMAA project/CMAA2/CMAA2.hlsl"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kDispatchArgsCount = 4;

    DXGI_FORMAT TranslateTypelessFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return DXGI_FORMAT_R11G11B10_FLOAT;
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

    bool CheckUAVTypedStoreFormatSupport(ID3D11Device* device, DXGI_FORMAT format)
    {
        if (format == DXGI_FORMAT_UNKNOWN)
            return false;

        D3D11_FEATURE_DATA_FORMAT_SUPPORT support = { format };
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2 = { format };

        if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
            return false;
        if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2))))
            return false;

        bool typedUAV = (support.OutFormatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
        bool typedStore = (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        return typedUAV && typedStore;
    }

    bool CreateBufferAndViews(ID3D11Device* device, const D3D11_BUFFER_DESC& desc,
                              ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11UnorderedAccessView>& uav,
                              UINT uavFlags = 0)
    {
        ComPtr<ID3D11Buffer> localBuffer;
        HRESULT hr = device->CreateBuffer(&desc, nullptr, &localBuffer);
        if (FAILED(hr))
        {
            LOG_ERROR("[CMAA2] Failed to create buffer (hr={:x})", hr);
            return false;
        }

        ComPtr<ID3D11UnorderedAccessView> localUav;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        if (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)
        {
            uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW | uavFlags;
            uavDesc.Buffer.NumElements = desc.ByteWidth / 4;
        }
        else if (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
        {
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.Buffer.NumElements = desc.ByteWidth / desc.StructureByteStride;
            uavDesc.Buffer.Flags = uavFlags;
        }
        else
        {
            uavDesc.Format = DXGI_FORMAT_R32_UINT;
            uavDesc.Buffer.NumElements = desc.ByteWidth / sizeof(UINT);
            uavDesc.Buffer.Flags = uavFlags;
        }

        hr = device->CreateUnorderedAccessView(localBuffer.Get(), &uavDesc, &localUav);
        if (FAILED(hr))
        {
            LOG_ERROR("[CMAA2] Failed to create UAV (hr={:x})", hr);
            return false;
        }

        buffer = localBuffer;
        uav = localUav;

        return true;
    }

    bool CreateTextureUAV(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                          ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11UnorderedAccessView>& uav)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        ComPtr<ID3D11Texture2D> localTexture;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &localTexture);
        if (FAILED(hr))
        {
            LOG_ERROR("[CMAA2] Failed to create texture ({}x{}, format={}, hr={:x})", width, height, static_cast<int>(format), hr);
            return false;
        }

        ComPtr<ID3D11UnorderedAccessView> localUav;
        if (true)
        {
            hr = device->CreateUnorderedAccessView(localTexture.Get(), nullptr, &localUav);
            if (FAILED(hr))
            {
                LOG_ERROR("[CMAA2] Failed to create texture UAV (hr={:x})", hr);
                return false;
            }
        }

        texture = localTexture;
        uav = localUav;

        return true;
    }

    std::wstring ToWide(const std::filesystem::path& path)
    {
        return path.wstring();
    }
}

CMAA2_Dx11::CMAA2_Dx11(const char* name, ID3D11Device* device)
    : _name(name != nullptr ? name : "CMAA2")
{
    _init = Initialize(device);
}

CMAA2_Dx11::~CMAA2_Dx11()
{
    ReleaseResources();
}

bool CMAA2_Dx11::Initialize(ID3D11Device* device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[{}] Invalid device", _name);
        return false;
    }

    _device = device;

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = _device->CreateSamplerState(&samplerDesc, &_pointSampler);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] Failed to create point sampler (hr={:x})", _name, hr);
        return false;
    }

    return true;
}

void CMAA2_Dx11::ReleaseIntermediates()
{
    _inOutTexture.Reset();
    _inOutSRV.Reset();
    _inOutUAV.Reset();

    _edgesTexture.Reset();
    _edgesUAV.Reset();

    _deferredHeadsTexture.Reset();
    _deferredHeadsUAV.Reset();

    _shapeCandidatesBuffer.Reset();
    _shapeCandidatesUAV.Reset();

    _deferredLocationBuffer.Reset();
    _deferredLocationUAV.Reset();

    _deferredItemBuffer.Reset();
    _deferredItemUAV.Reset();

    _controlBuffer.Reset();
    _controlBufferUAV.Reset();

    _dispatchArgsBuffer.Reset();
    _dispatchArgsUAV.Reset();

    _csEdges.Reset();
    _csProcess.Reset();
    _csDeferred.Reset();
    _csDispatchArgs.Reset();
    _csDebug.Reset();

    _width = 0;
    _height = 0;
    _format = DXGI_FORMAT_UNKNOWN;
    _sampleCount = 1;
    _shaderConfig = {};
    _resourcesDirty = true;
}

void CMAA2_Dx11::ReleaseResources()
{
    ReleaseIntermediates();
    _pointSampler.Reset();
    _device.Reset();
    _init = false;
}

bool CMAA2_Dx11::CreateBufferResources(ID3D11Texture2D* sourceTexture)
{
    if (!_init || _device == nullptr)
        return false;

    if (sourceTexture == nullptr)
    {
        LOG_WARN("[{}] CMAA2 CreateBufferResources called with null texture", _name);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    sourceTexture->GetDesc(&desc);

    if (desc.SampleDesc.Count > 1)
    {
        LOG_WARN("[{}] CMAA2 currently only supports single-sample color buffers (sampleCount={})", _name, desc.SampleDesc.Count);
        return false;
    }

    bool sizeChanged = desc.Width != _width || desc.Height != _height || desc.Format != _format || desc.SampleDesc.Count != _sampleCount;
    bool textureChanged = _inOutTexture.Get() != sourceTexture;

    if (!sizeChanged && !textureChanged && !_resourcesDirty)
        return true;

    ReleaseIntermediates();

    _inOutTexture = sourceTexture;
    _width = desc.Width;
    _height = desc.Height;
    _format = desc.Format;
    _sampleCount = desc.SampleDesc.Count;

    if (!UpdateInputViews(sourceTexture))
    {
        LOG_ERROR("[{}] Failed to create input views", _name);
        ReleaseIntermediates();
        return false;
    }

    if (!EnsureIntermediateResources(_width, _height, desc.Format, _sampleCount))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 intermediate resources", _name);
        ReleaseIntermediates();
        return false;
    }

    if (!CompileShaders())
    {
        LOG_ERROR("[{}] Failed to compile CMAA2 shaders", _name);
        ReleaseIntermediates();
        return false;
    }

    _resourcesDirty = false;
    return true;
}

bool CMAA2_Dx11::UpdateInputViews(ID3D11Texture2D* sourceTexture)
{
    _shaderConfig = {};
    _shaderConfig.colorFormat = _format;
    _shaderConfig.sampleCount = _sampleCount;

    DXGI_FORMAT srvFormat = TranslateTypelessFormat(_format);
    DXGI_FORMAT uavFormat = srvFormat;

    bool isSRGB = IsSRGB(srvFormat);
    if (isSRGB)
        uavFormat = StripSRGB(srvFormat);

    bool typedStoreSupported = CheckUAVTypedStoreFormatSupport(_device.Get(), uavFormat);
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
            _shaderConfig.untypedStoreMode = 1;
        else if (stripped == DXGI_FORMAT_R10G10B10A2_UNORM)
            _shaderConfig.untypedStoreMode = 2;
        else
        {
            LOG_ERROR("[{}] Unsupported color format for untyped CMAA2 storage ({})", _name, static_cast<int>(stripped));
            return false;
        }
    }

    _shaderConfig.hdrInput = IsFloatFormat(srvFormat);
    _shaderConfig.srvFormat = finalSrvFormat;
    _shaderConfig.uavFormat = finalUavFormat;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = finalSrvFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    HRESULT hr = _device->CreateShaderResourceView(sourceTexture, &srvDesc, &_inOutSRV);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 SRV (hr={:x}, format={})", _name, hr, static_cast<int>(finalSrvFormat));
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = finalUavFormat;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

    hr = _device->CreateUnorderedAccessView(sourceTexture, &uavDesc, &_inOutUAV);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] Failed to create CMAA2 UAV (hr={:x}, format={})", _name, hr, static_cast<int>(finalUavFormat));
        return false;
    }

    return true;
}

bool CMAA2_Dx11::EnsureIntermediateResources(UINT width, UINT height, DXGI_FORMAT, UINT sampleCount)
{
    UINT packedWidth = width;
#if CMAA_PACK_SINGLE_SAMPLE_EDGE_TO_HALF_WIDTH
    if (sampleCount == 1)
        packedWidth = (packedWidth + 1) / 2;
#endif

    DXGI_FORMAT edgesFormat = DXGI_FORMAT_R8_UINT;
    if (!CreateTextureUAV(_device.Get(), packedWidth, height, edgesFormat, _edgesTexture, _edgesUAV))
        return false;

    UINT headsWidth = (width + 1) / 2;
    UINT headsHeight = (height + 1) / 2;
    if (!CreateTextureUAV(_device.Get(), headsWidth, headsHeight, DXGI_FORMAT_R32_UINT, _deferredHeadsTexture, _deferredHeadsUAV))
        return false;

    // working buffers
    UINT requiredCandidatePixels = std::max<UINT>(1u, (width * height) / 4);
    UINT requiredDeferredColorApplyBuffer = std::max<UINT>(1u, (width * height) / 2);
    UINT requiredListHeadsPixels = std::max<UINT>(1u, (width * height + 3) / 6);

    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

    desc.ByteWidth = (std::max)(1u, requiredCandidatePixels) * sizeof(UINT);
    desc.StructureByteStride = sizeof(UINT);
    if (!CreateBufferAndViews(_device.Get(), desc, _shapeCandidatesBuffer, _shapeCandidatesUAV))
        return false;

    desc.ByteWidth = (std::max)(1u, requiredDeferredColorApplyBuffer) * sizeof(UINT) * 2;
    desc.StructureByteStride = sizeof(UINT) * 2;
    if (!CreateBufferAndViews(_device.Get(), desc, _deferredItemBuffer, _deferredItemUAV))
        return false;

    desc.ByteWidth = (std::max)(1u, requiredListHeadsPixels) * sizeof(UINT);
    desc.StructureByteStride = sizeof(UINT);
    if (!CreateBufferAndViews(_device.Get(), desc, _deferredLocationBuffer, _deferredLocationUAV))
        return false;

    // control buffer (raw)
    D3D11_BUFFER_DESC controlDesc = {};
    controlDesc.ByteWidth = kDispatchArgsCount * sizeof(UINT);
    controlDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    controlDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    controlDesc.Usage = D3D11_USAGE_DEFAULT;
    controlDesc.StructureByteStride = 0;
    if (!CreateBufferAndViews(_device.Get(), controlDesc, _dispatchArgsBuffer, _dispatchArgsUAV, D3D11_BUFFER_UAV_FLAG_RAW))
        return false;

    D3D11_BUFFER_DESC workingControl = {};
    workingControl.ByteWidth = 16;
    workingControl.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    workingControl.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    workingControl.Usage = D3D11_USAGE_DEFAULT;
    workingControl.StructureByteStride = 0;
    if (!CreateBufferAndViews(_device.Get(), workingControl, _controlBuffer, _controlBufferUAV, D3D11_BUFFER_UAV_FLAG_RAW))
        return false;

    return true;
}

bool CMAA2_Dx11::CompileShaders()
{
    std::vector<std::pair<std::string, std::string>> macroStorage;
    macroStorage.emplace_back("CMAA2_STATIC_QUALITY_PRESET", "2");
    macroStorage.emplace_back("CMAA2_EXTRA_SHARPNESS", "0");
    macroStorage.emplace_back("CMAA_MSAA_SAMPLE_COUNT", "1");

    if (_shaderConfig.typedStore)
    {
        macroStorage.emplace_back("CMAA2_UAV_STORE_TYPED", "1");
        macroStorage.emplace_back("CMAA2_UAV_STORE_TYPED_UNORM_FLOAT", _shaderConfig.typedStoreIsUnorm ? "1" : "0");
        macroStorage.emplace_back("CMAA2_UAV_STORE_CONVERT_TO_SRGB", "0");
    }
    else
    {
        macroStorage.emplace_back("CMAA2_UAV_STORE_TYPED", "0");
        macroStorage.emplace_back("CMAA2_UAV_STORE_CONVERT_TO_SRGB", _shaderConfig.convertToSRGB ? "1" : "0");
        macroStorage.emplace_back("CMAA2_UAV_STORE_TYPED_UNORM_FLOAT", "0");
        macroStorage.emplace_back("CMAA2_UAV_STORE_UNTYPED_FORMAT", std::to_string(_shaderConfig.untypedStoreMode));
    }

    macroStorage.emplace_back("CMAA2_SUPPORT_HDR_COLOR_RANGE", _shaderConfig.hdrInput ? "1" : "0");
    macroStorage.emplace_back("CMAA2_EDGE_DETECTION_LUMA_PATH", "1");

    std::vector<D3D_SHADER_MACRO> macros;
    macros.reserve(macroStorage.size() + 1);
    for (const auto& entry : macroStorage)
        macros.push_back({ entry.first.c_str(), entry.second.c_str() });
    macros.push_back({ nullptr, nullptr });

    std::filesystem::path shaderPath = Util::DllPath().parent_path() / "shaders" / "smaa" / "CMAA project" / "CMAA2" / "CMAA2.hlsl";
    if (!std::filesystem::exists(shaderPath))
    {
        LOG_ERROR("[{}] CMAA2 shader file not found: {}", _name, shaderPath.string());
        return false;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#else
    compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    auto compile = [&](const char* entryPoint, ComPtr<ID3D11ComputeShader>& shader) -> bool
    {
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompileFromFile(ToWide(shaderPath).c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                        entryPoint, "cs_5_0", compileFlags, 0, &bytecode, &errors);
        if (FAILED(hr))
        {
            if (errors != nullptr)
            {
                std::string_view message(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
                LOG_ERROR("[{}] CMAA2 shader compile error ({}): {}", _name, entryPoint, message);
            }
            else
            {
                LOG_ERROR("[{}] CMAA2 shader compile failed ({}: hr={:x})", _name, entryPoint, hr);
            }
            return false;
        }

        hr = _device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader);
        if (FAILED(hr))
        {
            LOG_ERROR("[{}] Failed to create CMAA2 compute shader ({} hr={:x})", _name, entryPoint, hr);
            return false;
        }

        return true;
    };

    return compile("EdgesColor2x2CS", _csEdges)
        && compile("ProcessCandidatesCS", _csProcess)
        && compile("DeferredColorApply2x2CS", _csDeferred)
        && compile("ComputeDispatchArgsCS", _csDispatchArgs)
        && compile("DebugDrawEdgesCS", _csDebug);
}

bool CMAA2_Dx11::Dispatch(ID3D11DeviceContext* context, ID3D11Texture2D* sourceTexture)
{
    if (context == nullptr || sourceTexture == nullptr || sourceTexture != _inOutTexture.Get())
        return false;

    if (_csEdges == nullptr || _csProcess == nullptr || _csDeferred == nullptr || _csDispatchArgs == nullptr)
        return false;

    std::array<ID3D11UnorderedAccessView*, 8> uavs = {
        nullptr,
        _edgesUAV.Get(),
        _shapeCandidatesUAV.Get(),
        _deferredLocationUAV.Get(),
        _deferredItemUAV.Get(),
        _deferredHeadsUAV.Get(),
        _controlBufferUAV.Get(),
        _dispatchArgsUAV.Get()
    };

    std::array<ID3D11ShaderResourceView*, 4> srvs = {
        _inOutSRV.Get(),
        nullptr,
        nullptr,
        nullptr
    };

    context->CSSetSamplers(0, 1, _pointSampler.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
    context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());

    UINT kernelSizeX = CMAA2_CS_INPUT_KERNEL_SIZE_X - 2;
    UINT kernelSizeY = CMAA2_CS_INPUT_KERNEL_SIZE_Y - 2;
    UINT groupCountX = (_width + kernelSizeX * 2 - 1) / (kernelSizeX * 2);
    UINT groupCountY = (_height + kernelSizeY * 2 - 1) / (kernelSizeY * 2);

    context->CSSetShader(_csEdges.Get(), nullptr, 0);
    context->Dispatch(groupCountX, groupCountY, 1);

    context->CSSetShader(_csDispatchArgs.Get(), nullptr, 0);
    context->Dispatch(2, 1, 1);

    context->CSSetShader(_csProcess.Get(), nullptr, 0);
    context->DispatchIndirect(_dispatchArgsBuffer.Get(), 0);

    context->CSSetShader(_csDispatchArgs.Get(), nullptr, 0);
    context->Dispatch(1, 2, 1);

    srvs[0] = nullptr;
    context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());

    uavs[0] = _inOutUAV.Get();
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);

    context->CSSetShader(_csDeferred.Get(), nullptr, 0);
    context->DispatchIndirect(_dispatchArgsBuffer.Get(), 0);

    std::array<ID3D11UnorderedAccessView*, 8> nullUAVs = {};
    std::array<ID3D11ShaderResourceView*, 4> nullSRVs = {};
    ID3D11SamplerState* nullSampler = nullptr;

    context->CSSetShader(nullptr, nullptr, 0);
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUAVs.size()), nullUAVs.data(), nullptr);
    context->CSSetShaderResources(0, static_cast<UINT>(nullSRVs.size()), nullSRVs.data());
    context->CSSetSamplers(0, 1, &nullSampler);

    return true;
}
