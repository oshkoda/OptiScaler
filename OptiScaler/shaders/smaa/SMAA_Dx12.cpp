#include "SMAA_Dx12.h"

#include <Logger.h>

#include <d3d12.h>

namespace
{
    void ResetHandles(SMAAResourceHandles& handles)
    {
        handles.cpu.ptr = 0;
        handles.gpu.ptr = 0;
    }
}

SMAA_Dx12::SMAA_Dx12(const char* name, ID3D12Device* device)
    : _name(name ? name : "SMAA"), _device(device)
{
    ResetHandles(_inputColorSrv);
    ResetHandles(_edgeBufferUav);
    ResetHandles(_blendBufferUav);
    ResetHandles(_processedColorSrv);

    if (_device == nullptr)
    {
        LOG_WARN("[{}] SMAA DirectX 12 device is null - SMAA will be disabled", _name);
        _init = false;
    }
    else
    {
        LOG_INFO("[{}] SMAA DirectX 12 path is using a placeholder implementation", _name);
        _init = true;
    }
}

bool SMAA_Dx12::CreateBufferResources(ID3D12Resource* sourceTexture)
{
    if (!_init)
    {
        return false;
    }

    if (sourceTexture == nullptr)
    {
        LOG_WARN("[{}] CreateBufferResources called with null source texture", _name);
        _buffersReady = false;
        _processedResource = nullptr;
        _inputResource = nullptr;
        return false;
    }

    const D3D12_RESOURCE_DESC desc = sourceTexture->GetDesc();

    const bool dimensionsChanged = (_cachedInputDesc.Width != desc.Width) ||
                                   (_cachedInputDesc.Height != desc.Height) ||
                                   (_cachedInputDesc.Format != desc.Format) ||
                                   (_cachedInputDesc.DepthOrArraySize != desc.DepthOrArraySize);

    if (!_buffersReady || dimensionsChanged)
    {
        LOG_INFO("[{}] Updating SMAA DX12 buffers to {}x{} (format={})", _name, desc.Width, desc.Height, static_cast<int>(desc.Format));

        _edgeBuffer.Reset();
        _blendBuffer.Reset();
        _srvHeap.Reset();
        _uavHeap.Reset();
        ResetHandles(_inputColorSrv);
        ResetHandles(_edgeBufferUav);
        ResetHandles(_blendBufferUav);
        ResetHandles(_processedColorSrv);

        _cachedInputDesc = desc;
        _buffersReady = true;
    }

    _inputResource = sourceTexture;
    _processedResource = sourceTexture;
    _currentInputState = D3D12_RESOURCE_STATE_COMMON;

    return true;
}

bool SMAA_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceTexture)
{
    if (!_init)
    {
        return false;
    }

    if (commandList == nullptr)
    {
        LOG_WARN("[{}] Dispatch called with null command list", _name);
        return false;
    }

    if (sourceTexture == nullptr)
    {
        LOG_WARN("[{}] Dispatch called with null source texture", _name);
        return false;
    }

    if (!_buffersReady || sourceTexture != _inputResource)
    {
        if (!CreateBufferResources(sourceTexture))
        {
            return false;
        }
    }

    if (_currentInputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = sourceTexture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = _currentInputState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        commandList->ResourceBarrier(1, &barrier);
        _currentInputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = sourceTexture;
    commandList->ResourceBarrier(1, &uavBarrier);

    if (_currentInputState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = sourceTexture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = _currentInputState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &barrier);
        _currentInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    _processedResource = sourceTexture;

    return true;
}

