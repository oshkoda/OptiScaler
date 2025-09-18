cbuffer SmaaConstants : register(b0)
{
    float2 InvResolution;
    float2 _Padding0;
    float Threshold;
    float BlendStrength;
    float2 _Padding1;
};

Texture2D<float4> InputColor : register(t0);
Texture2D<float4> BlendInput : register(t1);
RWTexture2D<float4> OutputColor : register(u0);

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

#define SMAA_EPSILON 1e-5

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    uint width;
    uint height;
    InputColor.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float2 texcoord = (float2(pixel) + 0.5) * InvResolution;
    float4 centerWeights = BlendInput.SampleLevel(LinearSampler, texcoord, 0);

    float4 offset = texcoord.xyxy + float4(InvResolution.x, 0.0, 0.0, InvResolution.y);

    float4 a;
    a.x = BlendInput.SampleLevel(LinearSampler, offset.xy, 0).w;
    a.y = BlendInput.SampleLevel(LinearSampler, offset.zw, 0).g;
    a.z = centerWeights.z;
    a.w = centerWeights.x;

    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) <= SMAA_EPSILON)
    {
        OutputColor[pixel] = InputColor.SampleLevel(LinearSampler, texcoord, 0);
        return;
    }

    bool horizontal = max(a.x, a.z) > max(a.y, a.w);

    float4 blendingOffset = float4(0.0, a.y, 0.0, a.w);
    float2 blendingWeight = a.yw;

    if (horizontal)
    {
        blendingOffset = float4(a.x, 0.0, a.z, 0.0);
        blendingWeight = a.xz;
    }

    float weightSum = blendingWeight.x + blendingWeight.y;

    if (weightSum <= SMAA_EPSILON)
    {
        OutputColor[pixel] = InputColor.SampleLevel(LinearSampler, texcoord, 0);
        return;
    }

    blendingWeight /= weightSum;

    float4 blendingCoord = texcoord.xyxy + blendingOffset * float4(InvResolution.x, InvResolution.y, -InvResolution.x, -InvResolution.y);

    float4 color = blendingWeight.x * InputColor.SampleLevel(LinearSampler, blendingCoord.xy, 0);
    color += blendingWeight.y * InputColor.SampleLevel(LinearSampler, blendingCoord.zw, 0);

    OutputColor[pixel] = color;
}
