#pragma once

#include <array>

#include <filesystem>

#include <string>

#include <d3d12.h>
#include <wrl/client.h>

struct ID3D12DescriptorHeap;

struct SMAAResourceHandles
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = { 0 };
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = { 0 };
};

class SMAA_Dx12
{
  public:
    explicit SMAA_Dx12(const char* name, ID3D12Device* device);

    bool IsInit() const { return _init; }

    bool CreateBufferResources(ID3D12Resource* sourceTexture);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceTexture);

    ID3D12Resource* ProcessedResource() const { return _processedResource; }

  private:
    bool EnsureShaders(const D3D12_RESOURCE_DESC& inputDesc);
    bool EnsureDescriptorHeaps();
    bool EnsureIntermediateResources(const D3D12_RESOURCE_DESC& inputDesc);
    bool UpdateInputDescriptors(ID3D12Resource* sourceTexture, const D3D12_RESOURCE_DESC& inputDesc);

    SMAAResourceHandles DescriptorFromIndex(const Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& heap, UINT index) const;

    std::string _name;
    ID3D12Device* _device = nullptr;
    bool _init = false;
    bool _buffersReady = false;

    bool _shadersReady = false;

    std::array<SMAAResourceHandles, 4> _srvTable = {};
    std::array<SMAAResourceHandles, 8> _uavTable = {};

    SMAAResourceHandles _inputColorSrv;
    SMAAResourceHandles _edgeBufferUav;
    SMAAResourceHandles _blendBufferUav;
    SMAAResourceHandles _processedColorSrv;


    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _uavHeap;

    Microsoft::WRL::ComPtr<ID3D12Resource> _edgeBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> _deferredHeadsBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> _shapeCandidatesBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> _deferredLocationBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> _deferredItemBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> _controlBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> _dispatchArgsBuffer;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> _rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _edgePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _dispatchArgsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _processPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _deferredPipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> _blendBuffer;

    ID3D12Resource* _processedResource = nullptr;
    ID3D12Resource* _inputResource = nullptr;

    D3D12_RESOURCE_DESC _cachedInputDesc = {};
    D3D12_RESOURCE_STATES _currentInputState = D3D12_RESOURCE_STATE_COMMON;

    UINT _srvDescriptorSize = 0;
    UINT _uavDescriptorSize = 0;
    DXGI_FORMAT _compiledFormat = DXGI_FORMAT_UNKNOWN;
    std::filesystem::path _shaderDirectory;

    struct ShaderConfig
    {
        DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT uavFormat = DXGI_FORMAT_UNKNOWN;
        bool typedStore = false;
        bool typedStoreIsUnorm = false;
        bool convertToSRGB = false;
        bool hdrInput = false;
        UINT untypedStoreMode = 0;
    };

    ShaderConfig _shaderConfig = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC _colorSrvDesc = {};
    D3D12_UNORDERED_ACCESS_VIEW_DESC _colorUavDesc = {};
};

