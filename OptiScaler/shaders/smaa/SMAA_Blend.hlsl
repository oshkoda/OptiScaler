cbuffer SmaaConstants : register(b0)
{
    float2 InvResolution;
    float2 _Padding0;
    float Threshold;
    float BlendStrength;
    float2 _Padding1;
};

Texture2D<float4> InputColor : register(t0);
Texture2D<float2> EdgeInput : register(t1);
RWTexture2D<float2> BlendOutput : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    uint width;
    uint height;
    EdgeInput.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float2 edgeData = EdgeInput.Load(int3(pixel, 0)).xy;

    float horizontalWeight = saturate(edgeData.x > Threshold ? edgeData.x * BlendStrength : 0.0f);
    float verticalWeight = saturate(edgeData.y > Threshold ? edgeData.y * BlendStrength : 0.0f);

    uint clampedX1 = pixel.x + 1 < width ? pixel.x + 1 : width - 1;
    uint clampedY1 = pixel.y + 1 < height ? pixel.y + 1 : height - 1;

    float3 center = InputColor.Load(int3(pixel, 0)).rgb;
    float3 right = InputColor.Load(int3(uint2(clampedX1, pixel.y), 0)).rgb;
    float3 bottom = InputColor.Load(int3(uint2(pixel.x, clampedY1), 0)).rgb;

    float horizontalContrast = length(right - center);
    float verticalContrast = length(bottom - center);

    horizontalWeight *= saturate(horizontalContrast * 4.0f);
    verticalWeight *= saturate(verticalContrast * 4.0f);

    BlendOutput[pixel] = float2(horizontalWeight, verticalWeight);
}
