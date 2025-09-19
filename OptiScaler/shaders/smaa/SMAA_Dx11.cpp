#include "SMAA_Dx11.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <vector>

#include <d3dcompiler.h>

#include <Logger.h>
#include <Util.h>

#include "AreaTex.h"
#include "SearchTex.h"

namespace
{
    constexpr UINT kMaxPixelSrvs = 10;
    constexpr UINT kMaxPixelSamplers = 2;

    inline DXGI_FORMAT TranslateTypelessFormat(DXGI_FORMAT format)
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
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return format;
        }
    }

    inline void ClearShaderResources(ID3D11DeviceContext* context, UINT startSlot, UINT count)
    {
        std::array<ID3D11ShaderResourceView*, kMaxPixelSrvs> nullSrvs = {};
        context->PSSetShaderResources(startSlot, count, nullSrvs.data());
    }
}

struct SMAA_Dx11::SavedPipelineState
{
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    UINT vertexStride = 0;
    UINT vertexOffset = 0;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;

    Microsoft::WRL::ComPtr<ID3D11Buffer> vsConstantBuffers[1];
    Microsoft::WRL::ComPtr<ID3D11Buffer> psConstantBuffers[1];

    Microsoft::WRL::ComPtr<ID3D11SamplerState> pixelSamplers[kMaxPixelSamplers];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pixelSrvs[kMaxPixelSrvs];

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState;
    UINT viewportCount = 0;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
    FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    UINT sampleMask = 0xffffffff;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState;
    UINT stencilRef = 0;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthStencilView;
};

SMAA_Dx11::SMAA_Dx11(const char* name, ID3D11Device* device)
    : _name(name ? name : "SMAA")
{
    _init = Initialize(device);
}

SMAA_Dx11::~SMAA_Dx11() = default;

bool SMAA_Dx11::Initialize(ID3D11Device* device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[{}] Invalid device", _name);
        return false;
    }

    _device = device;

    if (!EnsureAreaTextures())
        return false;
    if (!EnsureSamplers())
        return false;
    if (!EnsureStates())
        return false;
    if (!EnsureGeometry())
        return false;
    if (!EnsureShaders())
        return false;

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(SMAAShaderConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(_device->CreateBuffer(&cbDesc, nullptr, &_constantBuffer)))
    {
        LOG_ERROR("[{}] Failed to create constant buffer", _name);
        return false;
    }

    return true;
}

bool SMAA_Dx11::EnsureAreaTextures()
{
    if (_areaTexture && _areaSRV && _searchTexture && _searchSRV)
        return true;

    D3D11_TEXTURE2D_DESC areaDesc = {};
    areaDesc.Width = AREATEX_WIDTH;
    areaDesc.Height = AREATEX_HEIGHT;
    areaDesc.MipLevels = 1;
    areaDesc.ArraySize = 1;
    areaDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    areaDesc.SampleDesc.Count = 1;
    areaDesc.Usage = D3D11_USAGE_IMMUTABLE;
    areaDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA areaData = {};
    areaData.pSysMem = areaTexBytes;
    areaData.SysMemPitch = AREATEX_PITCH;

    if (FAILED(_device->CreateTexture2D(&areaDesc, &areaData, &_areaTexture)))
    {
        LOG_ERROR("[{}] Failed to create area texture", _name);
        return false;
    }

    if (FAILED(_device->CreateShaderResourceView(_areaTexture.Get(), nullptr, &_areaSRV)))
    {
        LOG_ERROR("[{}] Failed to create area texture SRV", _name);
        return false;
    }

    D3D11_TEXTURE2D_DESC searchDesc = {};
    searchDesc.Width = SEARCHTEX_WIDTH;
    searchDesc.Height = SEARCHTEX_HEIGHT;
    searchDesc.MipLevels = 1;
    searchDesc.ArraySize = 1;
    searchDesc.Format = DXGI_FORMAT_R8_UNORM;
    searchDesc.SampleDesc.Count = 1;
    searchDesc.Usage = D3D11_USAGE_IMMUTABLE;
    searchDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA searchData = {};
    searchData.pSysMem = searchTexBytes;
    searchData.SysMemPitch = SEARCHTEX_PITCH;

    if (FAILED(_device->CreateTexture2D(&searchDesc, &searchData, &_searchTexture)))
    {
        LOG_ERROR("[{}] Failed to create search texture", _name);
        return false;
    }

    if (FAILED(_device->CreateShaderResourceView(_searchTexture.Get(), nullptr, &_searchSRV)))
    {
        LOG_ERROR("[{}] Failed to create search texture SRV", _name);
        return false;
    }

    return true;
}

bool SMAA_Dx11::EnsureSamplers()
{
    if (_linearSampler && _pointSampler)
        return true;

    D3D11_SAMPLER_DESC linearDesc = {};
    linearDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    linearDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    linearDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    linearDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    linearDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    linearDesc.MinLOD = 0.0f;
    linearDesc.MaxLOD = D3D11_FLOAT32_MAX;

    if (FAILED(_device->CreateSamplerState(&linearDesc, &_linearSampler)))
    {
        LOG_ERROR("[{}] Failed to create linear sampler", _name);
        return false;
    }

    D3D11_SAMPLER_DESC pointDesc = linearDesc;
    pointDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

    if (FAILED(_device->CreateSamplerState(&pointDesc, &_pointSampler)))
    {
        LOG_ERROR("[{}] Failed to create point sampler", _name);
        return false;
    }

    return true;
}

bool SMAA_Dx11::EnsureStates()
{
    if (_depthStateDisabled && _blendStateDisabled && _blendStateEnabled)
        return true;

    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dsDesc.StencilEnable = FALSE;

    if (FAILED(_device->CreateDepthStencilState(&dsDesc, &_depthStateDisabled)))
    {
        LOG_ERROR("[{}] Failed to create depth state", _name);
        return false;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(_device->CreateBlendState(&blendDesc, &_blendStateDisabled)))
    {
        LOG_ERROR("[{}] Failed to create blend state", _name);
        return false;
    }

    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;

    if (FAILED(_device->CreateBlendState(&blendDesc, &_blendStateEnabled)))
    {
        LOG_ERROR("[{}] Failed to create blending state", _name);
        return false;
    }

    return true;
}

bool SMAA_Dx11::EnsureGeometry()
{
    if (_vertexBuffer && _inputLayout)
        return true;

    struct Vertex
    {
        float position[4];
        float texcoord[2];
    };

    const std::array<Vertex, 4> vertices = {
        Vertex{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
        Vertex{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        Vertex{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
        Vertex{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } }
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = static_cast<UINT>(sizeof(Vertex) * vertices.size());
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices.data();

    if (FAILED(_device->CreateBuffer(&vbDesc, &vbData, &_vertexBuffer)))
    {
        LOG_ERROR("[{}] Failed to create vertex buffer", _name);
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 4, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    // Input layout will be created after compiling the first vertex shader in EnsureShaders.
    // Defer actual creation until shaders are ready.

    return true;
}

namespace
{
    bool CompileShader(const std::filesystem::path& shaderPath,
                       const char* entryPoint,
                       const char* target,
                       const std::vector<D3D_SHADER_MACRO>& macros,
                       Microsoft::WRL::ComPtr<ID3DBlob>& blob)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG;
        flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompileFromFile(shaderPath.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, target,
                                        flags, 0, &blob, &errorBlob);
        if (FAILED(hr))
        {
            if (errorBlob)
            {
                LOG_ERROR("SMAA shader compile error ({}:{}): {}", entryPoint, target,
                          static_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            else
            {
                LOG_ERROR("Failed to compile SMAA shader {}:{} (hr={:x})", entryPoint, target, hr);
            }
            return false;
        }

        return true;
    }
}

bool SMAA_Dx11::EnsureShaders()
{
    if (_edgeVS && _edgePS && _weightVS && _weightPS && _blendVS && _blendPS && _inputLayout)
        return true;

    std::filesystem::path shaderPath = Util::DllPath().parent_path() / "shaders" / "smaa" / "SMAAWrapper.hlsl";

    if (!std::filesystem::exists(shaderPath))
    {
        LOG_ERROR("[{}] SMAA shader file not found: {}", _name, shaderPath.string());
        return false;
    }

    std::vector<D3D_SHADER_MACRO> macros;
    macros.push_back({ "SMAA_PRESET_HIGH", "1" });
    macros.push_back({ nullptr, nullptr });

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    if (!CompileShader(shaderPath, "DX10_SMAAEdgeDetectionVS", "vs_5_0", macros, vsBlob))
        return false;
    if (FAILED(_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &_edgeVS)))
        return false;

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 4, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    if (FAILED(_device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                          &_inputLayout)))
        return false;

    if (!CompileShader(shaderPath, "DX10_SMAALumaEdgeDetectionPS", "ps_5_0", macros, psBlob))
        return false;
    if (FAILED(_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &_edgePS)))
        return false;

    if (!CompileShader(shaderPath, "DX10_SMAABlendingWeightCalculationVS", "vs_5_0", macros, vsBlob))
        return false;
    if (FAILED(_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &_weightVS)))
        return false;

    if (!CompileShader(shaderPath, "DX10_SMAABlendingWeightCalculationPS", "ps_5_0", macros, psBlob))
        return false;
    if (FAILED(_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &_weightPS)))
        return false;

    if (!CompileShader(shaderPath, "DX10_SMAANeighborhoodBlendingVS", "vs_5_0", macros, vsBlob))
        return false;
    if (FAILED(_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &_blendVS)))
        return false;

    if (!CompileShader(shaderPath, "DX10_SMAANeighborhoodBlendingPS", "ps_5_0", macros, psBlob))
        return false;
    if (FAILED(_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &_blendPS)))
        return false;

    return true;
}

bool SMAA_Dx11::CreateIntermediateTargets(UINT width, UINT height, DXGI_FORMAT format, UINT sampleCount)
{
    if (_width == width && _height == height && _format == format && _sampleCount == sampleCount && _edgesTexture && _blendTexture && _outputTexture)
        return true;

    _edgesTexture.Reset();
    _edgesRTV.Reset();
    _edgesSRV.Reset();
    _blendTexture.Reset();
    _blendRTV.Reset();
    _blendSRV.Reset();
    _outputTexture.Reset();
    _outputRTV.Reset();
    _outputSRV.Reset();

    _width = width;
    _height = height;
    _format = format;
    _sampleCount = sampleCount;

    D3D11_TEXTURE2D_DESC edgeDesc = {};
    edgeDesc.Width = width;
    edgeDesc.Height = height;
    edgeDesc.MipLevels = 1;
    edgeDesc.ArraySize = 1;
    edgeDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    edgeDesc.SampleDesc.Count = sampleCount;
    edgeDesc.Usage = D3D11_USAGE_DEFAULT;
    edgeDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(_device->CreateTexture2D(&edgeDesc, nullptr, &_edgesTexture)))
        return false;
    if (FAILED(_device->CreateRenderTargetView(_edgesTexture.Get(), nullptr, &_edgesRTV)))
        return false;
    if (FAILED(_device->CreateShaderResourceView(_edgesTexture.Get(), nullptr, &_edgesSRV)))
        return false;

    D3D11_TEXTURE2D_DESC blendDesc = edgeDesc;
    if (FAILED(_device->CreateTexture2D(&blendDesc, nullptr, &_blendTexture)))
        return false;
    if (FAILED(_device->CreateRenderTargetView(_blendTexture.Get(), nullptr, &_blendRTV)))
        return false;
    if (FAILED(_device->CreateShaderResourceView(_blendTexture.Get(), nullptr, &_blendSRV)))
        return false;

    D3D11_TEXTURE2D_DESC outputDesc = {};
    outputDesc.Width = width;
    outputDesc.Height = height;
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = format;
    outputDesc.SampleDesc.Count = sampleCount;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(_device->CreateTexture2D(&outputDesc, nullptr, &_outputTexture)))
        return false;
    if (FAILED(_device->CreateRenderTargetView(_outputTexture.Get(), nullptr, &_outputRTV)))
        return false;
    if (FAILED(_device->CreateShaderResourceView(_outputTexture.Get(), nullptr, &_outputSRV)))
        return false;

    _viewport.TopLeftX = 0.0f;
    _viewport.TopLeftY = 0.0f;
    _viewport.Width = static_cast<float>(width);
    _viewport.Height = static_cast<float>(height);
    _viewport.MinDepth = 0.0f;
    _viewport.MaxDepth = 1.0f;

    return true;
}

bool SMAA_Dx11::UpdateInputView(ID3D11Texture2D* sourceTexture)
{
    if (sourceTexture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC sourceDesc;
    sourceTexture->GetDesc(&sourceDesc);

    if (_inputTexture.Get() != sourceTexture)
    {
        _inputTexture = sourceTexture;
        _inputSRV.Reset();
    }

    if (!_inputSRV)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = TranslateTypelessFormat(sourceDesc.Format);
        srvDesc.ViewDimension = sourceDesc.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = sourceDesc.MipLevels;

        if (FAILED(_device->CreateShaderResourceView(sourceTexture, &srvDesc, &_inputSRV)))
        {
            LOG_ERROR("[{}] Failed to create input SRV", _name);
            return false;
        }
    }

    return true;
}

bool SMAA_Dx11::CreateBufferResources(ID3D11Texture2D* sourceTexture)
{
    if (!_init || sourceTexture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC sourceDesc;
    sourceTexture->GetDesc(&sourceDesc);

    if (!CreateIntermediateTargets(sourceDesc.Width, sourceDesc.Height, TranslateTypelessFormat(sourceDesc.Format),
                                   sourceDesc.SampleDesc.Count))
        return false;

    return UpdateInputView(sourceTexture);
}

void SMAA_Dx11::CapturePipeline(ID3D11DeviceContext* context, SavedPipelineState& state)
{
    state.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&state.topology);

    UINT stride = 0;
    UINT offset = 0;
    ID3D11Buffer* vb = nullptr;
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    state.vertexBuffer = vb;
    state.vertexStride = stride;
    state.vertexOffset = offset;
    if (vb)
        vb->Release();

    ID3D11InputLayout* inputLayout = nullptr;
    context->IAGetInputLayout(&inputLayout);
    state.inputLayout = inputLayout;
    if (inputLayout)
        inputLayout->Release();

    ID3D11VertexShader* vs = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    state.vertexShader = vs;
    if (vs)
        vs->Release();

    ID3D11PixelShader* ps = nullptr;
    context->PSGetShader(&ps, nullptr, nullptr);
    state.pixelShader = ps;
    if (ps)
        ps->Release();

    ID3D11Buffer* cb = nullptr;
    context->VSGetConstantBuffers(0, 1, &cb);
    state.vsConstantBuffers[0] = cb;
    if (cb)
        cb->Release();

    cb = nullptr;
    context->PSGetConstantBuffers(0, 1, &cb);
    state.psConstantBuffers[0] = cb;
    if (cb)
        cb->Release();

    for (UINT i = 0; i < kMaxPixelSamplers; ++i)
    {
        ID3D11SamplerState* sampler = nullptr;
        context->PSGetSamplers(i, 1, &sampler);
        state.pixelSamplers[i] = sampler;
        if (sampler)
            sampler->Release();
    }

    for (UINT i = 0; i < kMaxPixelSrvs; ++i)
    {
        ID3D11ShaderResourceView* srv = nullptr;
        context->PSGetShaderResources(i, 1, &srv);
        state.pixelSrvs[i] = srv;
        if (srv)
            srv->Release();
    }

    ID3D11RasterizerState* rs = nullptr;
    context->RSGetState(&rs);
    state.rasterizerState = rs;
    if (rs)
        rs->Release();

    state.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&state.viewportCount, state.viewports);

    ID3D11BlendState* blend = nullptr;
    context->OMGetBlendState(&blend, state.blendFactor, &state.sampleMask);
    state.blendState = blend;
    if (blend)
        blend->Release();

    ID3D11DepthStencilState* depth = nullptr;
    context->OMGetDepthStencilState(&depth, &state.stencilRef);
    state.depthStencilState = depth;
    if (depth)
        depth->Release();

    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs = {};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs.data(), &dsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        state.renderTargets[i] = rtvs[i];
        if (rtvs[i])
            rtvs[i]->Release();
    }
    state.depthStencilView = dsv;
    if (dsv)
        dsv->Release();
}

void SMAA_Dx11::RestorePipeline(ID3D11DeviceContext* context, SavedPipelineState& state)
{
    if (state.vertexBuffer || state.inputLayout)
    {
        ID3D11Buffer* vb = state.vertexBuffer.Get();
        UINT stride = state.vertexStride;
        UINT offset = state.vertexOffset;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    }
    context->IASetInputLayout(state.inputLayout.Get());
    context->IASetPrimitiveTopology(state.topology);

    context->VSSetShader(state.vertexShader.Get(), nullptr, 0);
    ID3D11Buffer* vsCB = state.vsConstantBuffers[0].Get();
    context->VSSetConstantBuffers(0, 1, vsCB ? &vsCB : nullptr);

    context->PSSetShader(state.pixelShader.Get(), nullptr, 0);
    ID3D11Buffer* psCB = state.psConstantBuffers[0].Get();
    context->PSSetConstantBuffers(0, 1, psCB ? &psCB : nullptr);

    for (UINT i = 0; i < kMaxPixelSamplers; ++i)
    {
        ID3D11SamplerState* sampler = state.pixelSamplers[i].Get();
        context->PSSetSamplers(i, 1, sampler ? &sampler : nullptr);
    }

    for (UINT i = 0; i < kMaxPixelSrvs; ++i)
    {
        ID3D11ShaderResourceView* srv = state.pixelSrvs[i].Get();
        context->PSSetShaderResources(i, 1, srv ? &srv : nullptr);
    }

    context->RSSetState(state.rasterizerState.Get());
    if (state.viewportCount > 0)
        context->RSSetViewports(state.viewportCount, state.viewports);

    context->OMSetBlendState(state.blendState.Get(), state.blendFactor, state.sampleMask);
    context->OMSetDepthStencilState(state.depthStencilState.Get(), state.stencilRef);

    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs = {};
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        rtvs[i] = state.renderTargets[i].Get();
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs.data(), state.depthStencilView.Get());
}

void SMAA_Dx11::UpdateConstants(ID3D11DeviceContext* context, UINT width, UINT height)
{
    _constants.subsampleIndices[0] = 0.0f;
    _constants.subsampleIndices[1] = 0.0f;
    _constants.subsampleIndices[2] = 0.0f;
    _constants.subsampleIndices[3] = 0.0f;
    _constants.blendFactor = 1.0f;
    _constants.threshold = 0.1f;
    _constants.maxSearchSteps = 16.0f;
    _constants.maxSearchStepsDiag = 8.0f;
    _constants.cornerRounding = 0.25f;
    _constants.metrics[0] = width > 0 ? 1.0f / static_cast<float>(width) : 0.0f;
    _constants.metrics[1] = height > 0 ? 1.0f / static_cast<float>(height) : 0.0f;
    _constants.metrics[2] = static_cast<float>(width);
    _constants.metrics[3] = static_cast<float>(height);
    _constants.padding0 = _constants.padding1 = _constants.padding2 = 0.0f;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, &_constants, sizeof(_constants));
        context->Unmap(_constantBuffer.Get(), 0);
    }

    ID3D11Buffer* buffer = _constantBuffer.Get();
    context->VSSetConstantBuffers(0, 1, &buffer);
    context->PSSetConstantBuffers(0, 1, &buffer);
}

void SMAA_Dx11::BindCommonResources(ID3D11DeviceContext* context)
{
    ID3D11SamplerState* samplers[] = { _linearSampler.Get(), _pointSampler.Get() };
    context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);
}

bool SMAA_Dx11::RunEdgeDetection(ID3D11DeviceContext* context)
{
    context->OMSetRenderTargets(1, _edgesRTV.GetAddressOf(), nullptr);
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView(_edgesRTV.Get(), clearColor);

    ID3D11ShaderResourceView* srvs[kMaxPixelSrvs] = {};
    srvs[2] = _inputSRV.Get();
    srvs[3] = _inputSRV.Get();
    context->PSSetShaderResources(0, kMaxPixelSrvs, srvs);

    context->IASetInputLayout(_inputLayout.Get());
    UINT stride = sizeof(float) * 6;
    UINT offset = 0;
    ID3D11Buffer* vb = _vertexBuffer.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    context->VSSetShader(_edgeVS.Get(), nullptr, 0);
    context->PSSetShader(_edgePS.Get(), nullptr, 0);

    context->RSSetViewports(1, &_viewport);
    context->OMSetBlendState(_blendStateDisabled.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(_depthStateDisabled.Get(), 0);

    context->Draw(4, 0);

    ClearShaderResources(context, 0, kMaxPixelSrvs);

    return true;
}

bool SMAA_Dx11::RunBlendingWeight(ID3D11DeviceContext* context)
{
    context->OMSetRenderTargets(1, _blendRTV.GetAddressOf(), nullptr);
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView(_blendRTV.Get(), clearColor);

    ID3D11ShaderResourceView* srvs[kMaxPixelSrvs] = {};
    srvs[0] = _areaSRV.Get();
    srvs[1] = _searchSRV.Get();
    srvs[8] = _edgesSRV.Get();
    context->PSSetShaderResources(0, kMaxPixelSrvs, srvs);

    context->VSSetShader(_weightVS.Get(), nullptr, 0);
    context->PSSetShader(_weightPS.Get(), nullptr, 0);

    context->RSSetViewports(1, &_viewport);
    context->OMSetBlendState(_blendStateDisabled.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(_depthStateDisabled.Get(), 0);

    context->Draw(4, 0);

    ClearShaderResources(context, 0, kMaxPixelSrvs);

    return true;
}

bool SMAA_Dx11::RunNeighborhoodBlending(ID3D11DeviceContext* context)
{
    context->OMSetRenderTargets(1, _outputRTV.GetAddressOf(), nullptr);
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView(_outputRTV.Get(), clearColor);

    ID3D11ShaderResourceView* srvs[kMaxPixelSrvs] = {};
    srvs[2] = _inputSRV.Get();
    srvs[9] = _blendSRV.Get();
    context->PSSetShaderResources(0, kMaxPixelSrvs, srvs);

    context->VSSetShader(_blendVS.Get(), nullptr, 0);
    context->PSSetShader(_blendPS.Get(), nullptr, 0);

    context->RSSetViewports(1, &_viewport);
    context->OMSetDepthStencilState(_depthStateDisabled.Get(), 0);
    context->OMSetBlendState(_blendStateEnabled.Get(), nullptr, 0xffffffff);

    context->Draw(4, 0);

    ClearShaderResources(context, 0, kMaxPixelSrvs);

    return true;
}

bool SMAA_Dx11::Dispatch(ID3D11DeviceContext* context, ID3D11Texture2D* sourceTexture)
{
    if (!_init || context == nullptr || sourceTexture == nullptr)
        return false;

    if (!CreateBufferResources(sourceTexture))
        return false;

    SavedPipelineState state;
    CapturePipeline(context, state);

    BindCommonResources(context);
    UpdateConstants(context, _width, _height);

    bool success = RunEdgeDetection(context) && RunBlendingWeight(context) && RunNeighborhoodBlending(context);

    RestorePipeline(context, state);

    return success;
}

