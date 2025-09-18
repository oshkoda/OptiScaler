#pragma once

#include <pch.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <imgui/imgui_impl_dx12.h>

namespace MenuOverlayDx
{
ID3D12GraphicsCommandList* MenuCommandList();
void CleanupRenderTarget(bool clearQueue, HWND hWnd);
void Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
             const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP);
ID3D12DescriptorHeap* SrvDescriptorHeap();
DescriptorHeapAllocator* SrvDescriptorAllocator();
} // namespace MenuOverlayDx
