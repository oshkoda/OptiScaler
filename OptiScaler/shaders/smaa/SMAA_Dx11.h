#pragma once

#include <pch.h>

#include <d3d11.h>

class SMAA_Dx11
{
  public:
    struct alignas(16) Constants
    {
        float invResolution[2];
        float padding0[2];
        float threshold;
        float edgeIntensity;
        float blendStrength;
        float padding1[2];
    };

  private:
    std::string _name;
    bool _init = false;

    ID3D11Device* _device = nullptr;

    ID3D11ComputeShader* _edgeShader = nullptr;
    ID3D11ComputeShader* _blendShader = nullptr;
    ID3D11ComputeShader* _neighborhoodShader = nullptr;

    ID3D11Buffer* _constantBuffer = nullptr;

    ID3D11Texture2D* _edgeTexture = nullptr;
    ID3D11Texture2D* _blendTexture = nullptr;
    ID3D11Texture2D* _outputTexture = nullptr;

    ID3D11ShaderResourceView* _edgeSRV = nullptr;
    ID3D11UnorderedAccessView* _edgeUAV = nullptr;

    ID3D11ShaderResourceView* _blendSRV = nullptr;
    ID3D11UnorderedAccessView* _blendUAV = nullptr;

    ID3D11UnorderedAccessView* _outputUAV = nullptr;

    Constants _constants {};

    bool EnsureShaders();
    bool EnsureTextures(ID3D11Texture2D* colorTexture);
    bool EnsureConstantBuffer();
    DXGI_FORMAT ResolveFormat(DXGI_FORMAT format) const;

  public:
    SMAA_Dx11(std::string name, ID3D11Device* device);

    bool IsInit() const { return _init; }

    bool CreateBufferResources(ID3D11Texture2D* colorTexture);
    bool Dispatch(ID3D11DeviceContext* context, ID3D11Texture2D* colorTexture);

    ID3D11Texture2D* ProcessedResource() const { return _outputTexture; }

    ~SMAA_Dx11();
};
