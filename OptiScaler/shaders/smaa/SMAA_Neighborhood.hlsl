cbuffer SmaaConstants : register(b0)
{
    float2 InvResolution;
    float2 _Padding0;
    float Threshold;
    float BlendStrength;
    float2 _Padding1;
};

Texture2D<float4> InputColor : register(t0);
Texture2D<float2> BlendInput : register(t1);
RWTexture2D<float4> OutputColor : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    uint width;
    uint height;
    InputColor.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float4 center = InputColor.Load(int3(pixel, 0));
    float2 blend = BlendInput.Load(int3(pixel, 0));

    uint2 rightCoord = uint2(pixel.x + 1 < width ? pixel.x + 1 : width - 1, pixel.y);
    uint2 bottomCoord = uint2(pixel.x, pixel.y + 1 < height ? pixel.y + 1 : height - 1);

    float4 rightColor = InputColor.Load(int3(rightCoord, 0));
    float4 bottomColor = InputColor.Load(int3(bottomCoord, 0));

    float4 blended = lerp(center, rightColor, blend.x);
    blended = lerp(blended, bottomColor, blend.y);

    OutputColor[pixel] = blended;
}
