#pragma once

#include <string>

#include <d3d11.h>
#include <wrl/client.h>

class CMAA2_Dx11
{
  public:
    explicit CMAA2_Dx11(const char* name, ID3D11Device* device);
    ~CMAA2_Dx11();

    bool IsInit() const { return _init; }

    bool CreateBufferResources(ID3D11Texture2D* sourceTexture);
    bool Dispatch(ID3D11DeviceContext* context, ID3D11Texture2D* sourceTexture);

    ID3D11Texture2D* ProcessedResource() const { return _inOutTexture.Get(); }

  private:
    bool Initialize(ID3D11Device* device);
    void ReleaseResources();
    void ReleaseIntermediates();
    bool UpdateInputViews(ID3D11Texture2D* sourceTexture);
    bool EnsureIntermediateResources(UINT width, UINT height, DXGI_FORMAT format, UINT sampleCount);
    bool CompileShaders();

    struct ShaderConfig
    {
        DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT uavFormat = DXGI_FORMAT_UNKNOWN;
        bool typedStore = false;
        bool typedStoreIsUnorm = false;
        bool convertToSRGB = false;
        bool hdrInput = false;
        UINT untypedStoreMode = 0; // 0 - typed, 1 - rgba8, 2 - rgb10a2
        UINT sampleCount = 1;
    };

  private:
    std::string _name;
    bool _init = false;

    Microsoft::WRL::ComPtr<ID3D11Device> _device;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> _inOutTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _inOutSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _inOutUAV;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> _pointSampler;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> _edgesTexture;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _edgesUAV;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> _deferredHeadsTexture;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _deferredHeadsUAV;

    Microsoft::WRL::ComPtr<ID3D11Buffer> _shapeCandidatesBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _shapeCandidatesUAV;

    Microsoft::WRL::ComPtr<ID3D11Buffer> _deferredLocationBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _deferredLocationUAV;

    Microsoft::WRL::ComPtr<ID3D11Buffer> _deferredItemBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _deferredItemUAV;

    Microsoft::WRL::ComPtr<ID3D11Buffer> _controlBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _controlBufferUAV;

    Microsoft::WRL::ComPtr<ID3D11Buffer> _dispatchArgsBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _dispatchArgsUAV;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> _csEdges;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> _csProcess;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> _csDeferred;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> _csDispatchArgs;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> _csDebug;

    UINT _width = 0;
    UINT _height = 0;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    UINT _sampleCount = 1;

    ShaderConfig _shaderConfig = {};
    bool _resourcesDirty = true;
};
