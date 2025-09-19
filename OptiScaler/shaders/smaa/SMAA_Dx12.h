#pragma once

#include <string>
#include <d3d12.h>

class SMAA_Dx12
{
  public:
    explicit SMAA_Dx12(const char* name, ID3D12Device* device);

    bool IsInit() const { return _init; }

    bool CreateBufferResources(ID3D12Resource* sourceTexture);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceTexture);

    ID3D12Resource* ProcessedResource() const { return nullptr; }

  private:
    std::string _name;
    ID3D12Device* _device = nullptr;
    bool _init = false;
};

