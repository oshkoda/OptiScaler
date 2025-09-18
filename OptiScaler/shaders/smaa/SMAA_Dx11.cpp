#include "SMAA_Dx11.h"

#include "precompile/SMAA_Edge_Shader_Dx11.h"
#include "precompile/SMAA_Blend_Shader_Dx11.h"
#include "precompile/SMAA_Neighborhood_Shader_Dx11.h"

#include <cstring>
#include <d3d11_1.h>

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

bool SupportsTypedUavStore(ID3D11Device* device, DXGI_FORMAT format)
{
    if (device == nullptr || format == DXGI_FORMAT_UNKNOWN)
        return false;

    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 formatSupport2 = {};
    formatSupport2.InFormat = format;

    HRESULT hr = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &formatSupport2,
                                             sizeof(formatSupport2));

    if (FAILED(hr))
        return false;

    return (formatSupport2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

constexpr float kDefaultThreshold = 0.075f;
constexpr float kDefaultBlendStrength = 0.6f;
} // namespace

SMAA_Dx11::SMAA_Dx11(std::string name, ID3D11Device* device) : _name(std::move(name)), _device(device)
{
    if (_device == nullptr)
        return;

    _init = EnsureShaders() && EnsureConstantBuffer();

    if (_init)
    {
        LOG_INFO("[{0}] SMAA Dx11 initialized (device={1:X})", _name, (size_t) _device);
    }
    else
    {
        LOG_WARN("[{0}] SMAA Dx11 initialization incomplete (device={1:X})", _name, (size_t) _device);
    }
}

bool SMAA_Dx11::EnsureShaders()
{
    if (_edgeShader != nullptr && _blendShader != nullptr && _neighborhoodShader != nullptr)
        return true;

    if (_device == nullptr)
        return false;

    HRESULT hr = _device->CreateComputeShader(SMAA_Edge_cso, sizeof(SMAA_Edge_cso), nullptr, &_edgeShader);

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateComputeShader (edge) failed {1:x}", _name, (unsigned int) hr);
        return false;
    }

    hr = _device->CreateComputeShader(SMAA_Blend_cso, sizeof(SMAA_Blend_cso), nullptr, &_blendShader);

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateComputeShader (blend) failed {1:x}", _name, (unsigned int) hr);
        return false;
    }

    hr = _device->CreateComputeShader(SMAA_Neighborhood_cso, sizeof(SMAA_Neighborhood_cso), nullptr,
                                      &_neighborhoodShader);

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateComputeShader (neighborhood) failed {1:x}", _name, (unsigned int) hr);
        return false;
    }

    return true;
}

bool SMAA_Dx11::EnsureConstantBuffer()
{
    if (_constantBuffer != nullptr)
        return true;

    if (_device == nullptr)
        return false;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(Constants);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = _device->CreateBuffer(&desc, nullptr, &_constantBuffer);

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateBuffer (constants) failed {1:x}", _name, (unsigned int) hr);
        return false;
    }

    return true;
}

DXGI_FORMAT SMAA_Dx11::ResolveFormat(DXGI_FORMAT format) const
{
    return ::ResolveFormat(format);
}

bool SMAA_Dx11::EnsureTextures(ID3D11Texture2D* colorTexture)
{
    if (colorTexture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    colorTexture->GetDesc(&desc);

    auto createTexture = [&](ID3D11Texture2D** texture, DXGI_FORMAT format, ID3D11ShaderResourceView** srv,
                             ID3D11UnorderedAccessView** uav) -> bool {
        if (*texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC existing = {};
            (*texture)->GetDesc(&existing);

            if (existing.Width == desc.Width && existing.Height == desc.Height && existing.Format == format)
                return true;

            if (srv != nullptr && *srv != nullptr)
            {
                (*srv)->Release();
                *srv = nullptr;
            }

            if (uav != nullptr && *uav != nullptr)
            {
                (*uav)->Release();
                *uav = nullptr;
            }

            (*texture)->Release();
            *texture = nullptr;
        }

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = desc.Width;
        texDesc.Height = desc.Height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = format;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        HRESULT hr = _device->CreateTexture2D(&texDesc, nullptr, texture);

        if (FAILED(hr))
        {
            LOG_ERROR("[{0}] CreateTexture2D failed {1:x}", _name, (unsigned int) hr);
            return false;
        }

        if (srv != nullptr)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;

            hr = _device->CreateShaderResourceView(*texture, &srvDesc, srv);

            if (FAILED(hr))
            {
                LOG_ERROR("[{0}] CreateShaderResourceView failed {1:x}", _name, (unsigned int) hr);
                return false;
            }
        }

        if (uav != nullptr)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = format;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

            hr = _device->CreateUnorderedAccessView(*texture, &uavDesc, uav);

            if (FAILED(hr))
            {
                LOG_ERROR("[{0}] CreateUnorderedAccessView failed {1:x}", _name, (unsigned int) hr);
                return false;
            }
        }

        return true;
    };

    if (!createTexture(&_edgeTexture, DXGI_FORMAT_R16G16_FLOAT, &_edgeSRV, &_edgeUAV))
        return false;

    if (!createTexture(&_blendTexture, DXGI_FORMAT_R16G16_FLOAT, &_blendSRV, &_blendUAV))
        return false;

    DXGI_FORMAT outputFormat = ResolveFormat(desc.Format);
    DXGI_FORMAT outputTextureFormat = outputFormat;

    if (!SupportsTypedUavStore(_device, outputFormat) && !IsFloatFormat(outputFormat))
    {
        LOG_DEBUG("[{0}] EnsureTextures fallback UAV format for {1}x{2}: {3} -> DXGI_FORMAT_R16G16B16A16_FLOAT", _name,
                  desc.Width, desc.Height, (int) outputFormat);
        outputTextureFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    if (!createTexture(&_outputTexture, outputTextureFormat, nullptr, &_outputUAV))
        return false;

    return true;
}

bool SMAA_Dx11::CreateBufferResources(ID3D11Texture2D* colorTexture)
{
    if (!_init)
        return false;

    if (!EnsureConstantBuffer())
        return false;

    if (!EnsureTextures(colorTexture))
        return false;

    return true;
}

bool SMAA_Dx11::Dispatch(ID3D11DeviceContext* context, ID3D11Texture2D* colorTexture)
{
    if (!_init)
    {
        LOG_DEBUG("[{0}] Dispatch skipped: not initialized", _name);
        return false;
    }

    if (context == nullptr)
    {
        LOG_DEBUG("[{0}] Dispatch skipped: context is null", _name);
        return false;
    }

    if (colorTexture == nullptr)
    {
        LOG_DEBUG("[{0}] Dispatch skipped: color texture is null", _name);
        return false;
    }

    if (!CreateBufferResources(colorTexture))
    {
        LOG_DEBUG("[{0}] Dispatch aborted: CreateBufferResources failed", _name);
        return false;
    }

    D3D11_TEXTURE2D_DESC colorDesc = {};
    colorTexture->GetDesc(&colorDesc);

    LOG_DEBUG("[{0}] Dispatch begin {1}x{2}, format={3}", _name, colorDesc.Width, colorDesc.Height,
              (int) colorDesc.Format);

    D3D11_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = {};
    colorSrvDesc.Format = ResolveFormat(colorDesc.Format);
    colorSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    colorSrvDesc.Texture2D.MostDetailedMip = 0;
    colorSrvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* colorSRV = nullptr;
    HRESULT hr = _device->CreateShaderResourceView(colorTexture, &colorSrvDesc, &colorSRV);

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] CreateShaderResourceView (input) failed {1:x}", _name, (unsigned int) hr);
        LOG_DEBUG("[{0}] Dispatch aborted: CreateShaderResourceView (input) failed", _name);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

    if (FAILED(hr))
    {
        LOG_ERROR("[{0}] Map constant buffer failed {1:x}", _name, (unsigned int) hr);
        LOG_DEBUG("[{0}] Dispatch aborted during constant buffer update", _name);
        colorSRV->Release();
        return false;
    }

    Constants constants = {};
    constants.invWidth = colorDesc.Width > 0 ? 1.0f / static_cast<float>(colorDesc.Width) : 0.0f;
    constants.invHeight = colorDesc.Height > 0 ? 1.0f / static_cast<float>(colorDesc.Height) : 0.0f;
    constants.threshold = kDefaultThreshold;
    constants.blendStrength = kDefaultBlendStrength;

    std::memcpy(mapped.pData, &constants, sizeof(Constants));
    context->Unmap(_constantBuffer, 0);

    UINT dispatchX = (colorDesc.Width + 7) / 8;
    UINT dispatchY = (colorDesc.Height + 7) / 8;

    ID3D11Buffer* constantBuffers[1] = { _constantBuffer };

    // Edge detection pass
    ID3D11ShaderResourceView* edgeSrvs[2] = { colorSRV, _edgeSRV };
    context->CSSetShader(_edgeShader, nullptr, 0);
    context->CSSetShaderResources(0, 2, edgeSrvs);
    context->CSSetConstantBuffers(0, 1, constantBuffers);
    context->CSSetUnorderedAccessViews(0, 1, &_edgeUAV, nullptr);
    context->Dispatch(dispatchX, dispatchY, 1);

    ID3D11UnorderedAccessView* nullUav = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    context->CSSetShaderResources(0, 2, nullSrvs);

    // Blend weight pass
    ID3D11ShaderResourceView* blendSrvs[2] = { colorSRV, _edgeSRV };
    context->CSSetShader(_blendShader, nullptr, 0);
    context->CSSetShaderResources(0, 2, blendSrvs);
    context->CSSetConstantBuffers(0, 1, constantBuffers);
    context->CSSetUnorderedAccessViews(0, 1, &_blendUAV, nullptr);
    context->Dispatch(dispatchX, dispatchY, 1);

    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context->CSSetShaderResources(0, 2, nullSrvs);

    // Neighborhood blending
    ID3D11ShaderResourceView* neighborhoodSrvs[2] = { colorSRV, _blendSRV };
    context->CSSetShader(_neighborhoodShader, nullptr, 0);
    context->CSSetShaderResources(0, 2, neighborhoodSrvs);
    context->CSSetConstantBuffers(0, 1, constantBuffers);
    context->CSSetUnorderedAccessViews(0, 1, &_outputUAV, nullptr);
    context->Dispatch(dispatchX, dispatchY, 1);

    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context->CSSetShaderResources(0, 2, nullSrvs);

    context->CSSetShader(nullptr, nullptr, 0);

    colorSRV->Release();

    LOG_DEBUG("[{0}] Dispatch complete {1}x{2}", _name, colorDesc.Width, colorDesc.Height);

    return true;
}

SMAA_Dx11::~SMAA_Dx11()
{
    if (_edgeSRV != nullptr)
    {
        _edgeSRV->Release();
        _edgeSRV = nullptr;
    }

    if (_edgeUAV != nullptr)
    {
        _edgeUAV->Release();
        _edgeUAV = nullptr;
    }

    if (_blendSRV != nullptr)
    {
        _blendSRV->Release();
        _blendSRV = nullptr;
    }

    if (_blendUAV != nullptr)
    {
        _blendUAV->Release();
        _blendUAV = nullptr;
    }

    if (_outputUAV != nullptr)
    {
        _outputUAV->Release();
        _outputUAV = nullptr;
    }

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

    if (_constantBuffer != nullptr)
    {
        _constantBuffer->Release();
        _constantBuffer = nullptr;
    }

    if (_edgeShader != nullptr)
    {
        _edgeShader->Release();
        _edgeShader = nullptr;
    }

    if (_blendShader != nullptr)
    {
        _blendShader->Release();
        _blendShader = nullptr;
    }

    if (_neighborhoodShader != nullptr)
    {
        _neighborhoodShader->Release();
        _neighborhoodShader = nullptr;
    }
}
