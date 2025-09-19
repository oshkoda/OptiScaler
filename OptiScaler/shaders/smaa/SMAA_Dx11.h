#pragma once

#include <string>
#include <wrl/client.h>
#include <d3d11.h>

class SMAA_Dx11
{
  public:
    explicit SMAA_Dx11(const char* name, ID3D11Device* device);
    ~SMAA_Dx11();

    bool IsInit() const { return _init; }

    bool CreateBufferResources(ID3D11Texture2D* sourceTexture);
    bool Dispatch(ID3D11DeviceContext* context, ID3D11Texture2D* sourceTexture);

    ID3D11Texture2D* ProcessedResource() const { return _outputTexture.Get(); }

  private:
    struct SMAAShaderConstants
    {
        float subsampleIndices[4];
        float metrics[4];
        float blendFactor;
        float threshold;
        float maxSearchSteps;
        float maxSearchStepsDiag;
        float cornerRounding;
        float padding0;
        float padding1;
        float padding2;
    };

    struct SavedPipelineState;

    bool Initialize(ID3D11Device* device);
    bool EnsureAreaTextures();
    bool EnsureSamplers();
    bool EnsureStates();
    bool EnsureGeometry();
    bool EnsureShaders();
    bool CreateIntermediateTargets(UINT width, UINT height, DXGI_FORMAT format, UINT sampleCount);
    bool UpdateInputView(ID3D11Texture2D* sourceTexture);

    bool RunEdgeDetection(ID3D11DeviceContext* context);
    bool RunBlendingWeight(ID3D11DeviceContext* context);
    bool RunNeighborhoodBlending(ID3D11DeviceContext* context);

    void UpdateConstants(ID3D11DeviceContext* context, UINT width, UINT height);
    void BindCommonResources(ID3D11DeviceContext* context);
    void RestorePipeline(ID3D11DeviceContext* context, SavedPipelineState& state);
    void CapturePipeline(ID3D11DeviceContext* context, SavedPipelineState& state);

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    std::string _name;
    ComPtr<ID3D11Device> _device;
    bool _init = false;

    UINT _width = 0;
    UINT _height = 0;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    UINT _sampleCount = 1;

    ComPtr<ID3D11Texture2D> _edgesTexture;
    ComPtr<ID3D11RenderTargetView> _edgesRTV;
    ComPtr<ID3D11ShaderResourceView> _edgesSRV;

    ComPtr<ID3D11Texture2D> _blendTexture;
    ComPtr<ID3D11RenderTargetView> _blendRTV;
    ComPtr<ID3D11ShaderResourceView> _blendSRV;

    ComPtr<ID3D11Texture2D> _outputTexture;
    ComPtr<ID3D11RenderTargetView> _outputRTV;
    ComPtr<ID3D11ShaderResourceView> _outputSRV;

    ComPtr<ID3D11Texture2D> _inputTexture;
    ComPtr<ID3D11ShaderResourceView> _inputSRV;

    ComPtr<ID3D11Texture2D> _areaTexture;
    ComPtr<ID3D11ShaderResourceView> _areaSRV;
    ComPtr<ID3D11Texture2D> _searchTexture;
    ComPtr<ID3D11ShaderResourceView> _searchSRV;

    ComPtr<ID3D11SamplerState> _linearSampler;
    ComPtr<ID3D11SamplerState> _pointSampler;

    ComPtr<ID3D11DepthStencilState> _depthStateDisabled;
    ComPtr<ID3D11BlendState> _blendStateDisabled;
    ComPtr<ID3D11BlendState> _blendStateEnabled;

    ComPtr<ID3D11Buffer> _constantBuffer;
    ComPtr<ID3D11Buffer> _vertexBuffer;
    ComPtr<ID3D11InputLayout> _inputLayout;

    ComPtr<ID3D11VertexShader> _edgeVS;
    ComPtr<ID3D11PixelShader> _edgePS;
    ComPtr<ID3D11VertexShader> _weightVS;
    ComPtr<ID3D11PixelShader> _weightPS;
    ComPtr<ID3D11VertexShader> _blendVS;
    ComPtr<ID3D11PixelShader> _blendPS;

    D3D11_VIEWPORT _viewport = {};
    SMAAShaderConstants _constants = {};
};

