#pragma once

#include <pch.h>

#include <d3d12.h>
#include <d3dx/d3dx12.h>

class SMAA_Dx12
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

    ID3D12Device* _device = nullptr;

    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _edgePipeline = nullptr;
    ID3D12PipelineState* _blendPipeline = nullptr;
    ID3D12PipelineState* _neighborhoodPipeline = nullptr;

    ID3D12DescriptorHeap* _descriptorHeaps[3] = { nullptr, nullptr, nullptr };
    D3D12_CPU_DESCRIPTOR_HANDLE _cpuSrvHandles[3][2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE _cpuUavHandles[3] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE _gpuSrvHandles[3][2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE _gpuUavHandles[3] = {};
    UINT _descriptorSize = 0;

    ID3D12Resource* _edgeTexture = nullptr;
    ID3D12Resource* _blendTexture = nullptr;
    ID3D12Resource* _outputTexture = nullptr;

    DXGI_FORMAT _outputTextureFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_RESOURCE_STATES _edgeState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES _blendState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES _outputState = D3D12_RESOURCE_STATE_COMMON;

    Constants _constants {};

    bool CreateRootSignature();
    bool CreatePipeline(ID3D12PipelineState** pipeline, const unsigned char* shaderData, size_t shaderSize);
    bool EnsureDescriptorHeap(int passIndex);
    bool EnsureTextures(ID3D12Resource* inputColor);
    void TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES& currentState, D3D12_RESOURCE_STATES targetState);
    void UpdateConstants(ID3D12GraphicsCommandList* commandList, UINT width, UINT height);
    void PopulateDescriptors(ID3D12Device* device, ID3D12Resource* colorResource);

  public:
    SMAA_Dx12(std::string name, ID3D12Device* device);

    bool IsInit() const { return _init; }

    bool CreateBufferResources(ID3D12Resource* colorResource);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* colorResource);

    ID3D12Resource* ProcessedResource() const { return _outputTexture; }
    DXGI_FORMAT OutputTextureFormat() const { return _outputTextureFormat; }

    ~SMAA_Dx12();
};
