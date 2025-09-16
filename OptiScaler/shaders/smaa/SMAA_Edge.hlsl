cbuffer SmaaConstants : register(b0)
{
    float2 InvResolution;
    float2 _Padding0;
    float Threshold;
    float EdgeIntensity;
    float2 _Padding1;
};

Texture2D<float4> InputColor : register(t0);
RWTexture2D<float2> EdgeOutput : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    uint width;
    uint height;
    InputColor.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    uint clampedX0 = pixel.x > 0 ? pixel.x - 1 : pixel.x;
    uint clampedY0 = pixel.y > 0 ? pixel.y - 1 : pixel.y;
    uint clampedX1 = pixel.x + 1 < width ? pixel.x + 1 : width - 1;
    uint clampedY1 = pixel.y + 1 < height ? pixel.y + 1 : height - 1;

    float3 center = InputColor.Load(int3(pixel, 0)).rgb;
    float3 left = InputColor.Load(int3(uint2(clampedX0, pixel.y), 0)).rgb;
    float3 right = InputColor.Load(int3(uint2(clampedX1, pixel.y), 0)).rgb;
    float3 top = InputColor.Load(int3(uint2(pixel.x, clampedY0), 0)).rgb;
    float3 bottom = InputColor.Load(int3(uint2(pixel.x, clampedY1), 0)).rgb;

    float3 horizontalDelta = abs(right - center) + abs(center - left);
    float3 verticalDelta = abs(bottom - center) + abs(center - top);

    float horizontalEdge = max(max(horizontalDelta.r, horizontalDelta.g), horizontalDelta.b);
    float verticalEdge = max(max(verticalDelta.r, verticalDelta.g), verticalDelta.b);

    horizontalEdge = horizontalEdge > Threshold ? horizontalEdge * EdgeIntensity : 0.0f;
    verticalEdge = verticalEdge > Threshold ? verticalEdge * EdgeIntensity : 0.0f;

    EdgeOutput[pixel] = float2(horizontalEdge, verticalEdge);
}
