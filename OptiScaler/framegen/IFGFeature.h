#pragma once
#include <pch.h>

#include <OwnedMutex.h>

#include <dxgi1_6.h>

class IFGFeature
{
  protected:
    float _jitterX = 0.0;
    float _jitterY = 0.0;
    float _mvScaleX = 0.0;
    float _mvScaleY = 0.0;
    float _cameraNear = 0.0;
    float _cameraFar = 0.0;
    float _cameraVFov = 0.0;
    float _meterFactor = 0.0;
    float _ftDelta = 0.0;
    UINT _reset = 0;

    UINT64 _frameCount = 0;
    UINT64 _lastDispatchedFrame = 0;
    UINT64 _lastUpscaledFrameId = 0;

    bool _isActive = false;
    UINT64 _targetFrame = 0;

    bool _mvAndDepthReady[BUFFER_COUNT] = { false, false, false, false };
    bool _hudlessReady[BUFFER_COUNT] = { false, false, false, false };
    bool _hudlessDispatchReady[BUFFER_COUNT] = { false, false, false, false };
    bool _noHudless[BUFFER_COUNT] = { false, false, false, false };

    IID streamlineRiid {};

    bool CheckForRealObject(std::string functionName, IUnknown* pObject, IUnknown** ppRealObject);

  public:
    OwnedMutex Mutex;

    virtual feature_version Version() = 0;
    virtual const char* Name() = 0;

    virtual void Present() = 0;

    virtual void SetUpscaleInputsReady() = 0;
    virtual void SetHudlessReady() = 0;
    virtual void SetHudlessDispatchReady() = 0;

    virtual bool UpscalerInputsReady() = 0;
    virtual bool HudlessReady() = 0;
    virtual bool ReadyForExecute() = 0;

    virtual void ReleaseObjects() = 0;
    virtual void StopAndDestroyContext(bool destroy, bool shutDown, bool useMutex) = 0;

    UINT64 StartNewFrame();

    bool IsActive();
    int GetIndex();

    void SetJitter(float x, float y);
    void SetMVScale(float x, float y);
    void SetCameraValues(float nearValue, float farValue, float vFov, float meterFactor = 0.0f);
    void SetFrameTimeDelta(float delta);
    void SetReset(UINT reset);

    void ResetCounters();
    void UpdateTarget();

    UINT64 FrameCount();
    UINT64 LastDispatchedFrame();
    UINT64 TargetFrame();

    IFGFeature() = default;
};
