#pragma once
#include "DLSSFeature.h"
#include <upscalers/IFeature_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <string>

class DLSSFeatureDx12 : public DLSSFeature, public IFeature_Dx12
{
  private:
    ID3D12Resource* _dlssDebugTexture = nullptr;
    D3D12_RESOURCE_STATES _dlssDebugState = D3D12_RESOURCE_STATE_COMMON;

    bool EnsureDebugTexture(ID3D12Resource* source);
    void CopyDebugTexture(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source);
  protected:
  public:
    bool Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList,
              NVSDK_NGX_Parameter* InParameters) override;
    bool Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    static void Shutdown(ID3D12Device* InDevice);

    DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~DLSSFeatureDx12();
};
