# VertexAsylum dependencies removed from SMAA sample sources

The deleted reference implementation under `shaders/smaa` previously relied on the following VertexAsylum headers:

- `CMAA2Sample.cpp`: `Core/System/vaFileTools.h`, `Core/Misc/vaProfiler.h`, `Rendering/vaShader.h`, `Rendering/DirectX/vaRenderDeviceDX11.h`, `Rendering/DirectX/vaRenderDeviceDX12.h`, `Rendering/vaAssetPack.h`, and `IntegratedExternals/vaImguiIntegration.h`.
- `CMAA2Sample.h`: `Core/vaCoreIncludes.h`, `vaApplicationWin.h`, `Scene/vaCameraControllers.h`, `Scene/vaScene.h`, `Rendering/vaRenderingIncludes.h`, `Core/vaUI.h`, `Rendering/vaRenderDevice.h`, `Rendering/Effects/vaASSAOLite.h`, `Rendering/Misc/vaZoomTool.h`, `Rendering/Misc/vaImageCompareTool.h`, `Rendering/Misc/vaTextureReductionTestTool.h`, `CMAA2/vaCMAA2.h`, `SMAA/vaSMAAWrapper.h`, and `FXAA/vaFXAAWrapper.h`.
- `RenderTarget.h`: `Rendering/DirectX/vaDirectXIncludes.h`.
- `vaSMAAWrapper.h`: `Core/vaCoreIncludes.h`, `Core/vaUI.h`, and `Rendering/vaRenderingIncludes.h`.
- `vaSMAAWrapperDX11.cpp`: `Core/vaCoreIncludes.h`, `Rendering/DirectX/vaDirectXIncludes.h`, `Rendering/DirectX/vaDirectXTools.h`, `Scene/vaSceneIncludes.h`, `Rendering/vaRenderingIncludes.h`, `Rendering/DirectX/vaRenderDeviceContextDX11.h`, `Rendering/Shaders/vaSharedTypes.h`, `Rendering/DirectX/vaTextureDX11.h`, `Rendering/DirectX/vaRenderBuffersDX11.h`, and `Rendering/DirectX/vaRenderDeviceContextDX12.h`.
- `SMAA.h`: `Rendering/DirectX/vaDirectXIncludes.h` (plus `RenderTarget.h`).

These headers were removed along with the sample sources and replaced by the new standalone SMAA integration that relies solely on the OptiScaler codebase.
