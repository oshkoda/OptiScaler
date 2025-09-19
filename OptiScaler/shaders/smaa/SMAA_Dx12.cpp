#include "SMAA_Dx12.h"

#include <Logger.h>

SMAA_Dx12::SMAA_Dx12(const char* name, ID3D12Device* device)
    : _name(name ? name : "SMAA"), _device(device)
{
    if (_device == nullptr)
    {
        LOG_WARN("[{}] SMAA DirectX 12 device is null - SMAA will be disabled", _name);
        _init = false;
    }
    else
    {
        LOG_WARN("[{}] SMAA DirectX 12 path is not implemented yet", _name);
        _init = false;
    }
}

bool SMAA_Dx12::CreateBufferResources(ID3D12Resource*)
{
    return false;
}

bool SMAA_Dx12::Dispatch(ID3D12GraphicsCommandList*, ID3D12Resource*)
{
    return false;
}

