cbuffer SmaaConstants : register(b0)
{
    float2 InvResolution;
    float2 _Padding0;
    float Threshold;
    float BlendStrength;
    float2 _Padding1;
};

Texture2D<float2> EdgeInput : register(t0);
Texture2D<float2> AreaTex : register(t1);
Texture2D<float> SearchTex : register(t2);
RWTexture2D<float4> BlendOutput : register(u0);

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

static float4 gSmaaMetrics;

#define SMAA_RT_METRICS gSmaaMetrics
#define SMAA_DISABLE_DIAG_DETECTION 1
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / float2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)
#define SMAA_AREATEX_SELECT(sample) sample.xy
#define SMAA_SEARCHTEX_SELECT(sample) sample.x
#define SMAA_CORNER_ROUNDING 25
#define SMAA_CORNER_ROUNDING_NORM (float(SMAA_CORNER_ROUNDING) / 100.0)
#define SMAA_EPSILON 1e-5

float2 SampleEdges(float2 coord)
{
    return EdgeInput.SampleLevel(LinearSampler, coord, 0);
}

float2 SampleEdgesOffset(float2 coord, int2 offset)
{
    return EdgeInput.SampleLevel(LinearSampler, coord + float2(offset) * SMAA_RT_METRICS.xy, 0);
}

float SMAASearchLength(float2 e, float offset)
{
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);
    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);

    scale += float2(-1.0, 1.0);
    bias += float2(0.5, -0.5);

    scale /= SMAA_SEARCHTEX_PACKED_SIZE;
    bias /= SMAA_SEARCHTEX_PACKED_SIZE;

    return SMAA_SEARCHTEX_SELECT(SearchTex.SampleLevel(PointSampler, mad(scale, e, bias), 0));
}

float SMAASearchXLeft(float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0)
    {
        e = SampleEdges(texcoord);
        texcoord = mad(-float2(2.0, 0.0), SMAA_RT_METRICS.xy, texcoord);
    }

    float offset = mad(-(255.0 / 127.0), SMAASearchLength(e, 0.0), 3.25);
    return mad(SMAA_RT_METRICS.x, offset, texcoord.x);
}

float SMAASearchXRight(float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0)
    {
        e = SampleEdges(texcoord);
        texcoord = mad(float2(2.0, 0.0), SMAA_RT_METRICS.xy, texcoord);
    }

    float offset = mad(-(255.0 / 127.0), SMAASearchLength(e, 0.5), 3.25);
    return mad(-SMAA_RT_METRICS.x, offset, texcoord.x);
}

float SMAASearchYUp(float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0)
    {
        e = SampleEdges(texcoord);
        texcoord = mad(-float2(0.0, 2.0), SMAA_RT_METRICS.xy, texcoord);
    }

    float offset = mad(-(255.0 / 127.0), SMAASearchLength(e.gr, 0.0), 3.25);
    return mad(SMAA_RT_METRICS.y, offset, texcoord.y);
}

float SMAASearchYDown(float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0)
    {
        e = SampleEdges(texcoord);
        texcoord = mad(float2(0.0, 2.0), SMAA_RT_METRICS.xy, texcoord);
    }

    float offset = mad(-(255.0 / 127.0), SMAASearchLength(e.gr, 0.5), 3.25);
    return mad(-SMAA_RT_METRICS.y, offset, texcoord.y);
}

float2 SMAAArea(float2 dist, float e1, float e2, float offset)
{
    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE), round(4.0 * float2(e1, e2)), dist);
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    texcoord.y = mad(SMAA_AREATEX_SUBTEX_SIZE, offset, texcoord.y);

    return SMAA_AREATEX_SELECT(AreaTex.SampleLevel(LinearSampler, texcoord, 0));
}

void SMAADetectHorizontalCornerPattern(inout float2 weights, float4 texcoord, float2 d)
{
#if !defined(SMAA_DISABLE_CORNER_DETECTION)
    float2 leftRight = step(d.xy, d.yx);
    float sum = leftRight.x + leftRight.y;

    if (sum > SMAA_EPSILON)
    {
        float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight / sum;
        float2 factor = float2(1.0, 1.0);

        factor.x -= rounding.x * SampleEdgesOffset(texcoord.xy, int2(0, 1)).r;
        factor.x -= rounding.y * SampleEdgesOffset(texcoord.zw, int2(1, 1)).r;
        factor.y -= rounding.x * SampleEdgesOffset(texcoord.xy, int2(0, -2)).r;
        factor.y -= rounding.y * SampleEdgesOffset(texcoord.zw, int2(1, -2)).r;

        weights *= saturate(factor);
    }
#endif
}

void SMAADetectVerticalCornerPattern(inout float2 weights, float4 texcoord, float2 d)
{
#if !defined(SMAA_DISABLE_CORNER_DETECTION)
    float2 leftRight = step(d.xy, d.yx);
    float sum = leftRight.x + leftRight.y;

    if (sum > SMAA_EPSILON)
    {
        float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight / sum;
        float2 factor = float2(1.0, 1.0);

        factor.x -= rounding.x * SampleEdgesOffset(texcoord.xy, int2(1, 0)).g;
        factor.x -= rounding.y * SampleEdgesOffset(texcoord.zw, int2(1, 1)).g;
        factor.y -= rounding.x * SampleEdgesOffset(texcoord.xy, int2(-2, 0)).g;
        factor.y -= rounding.y * SampleEdgesOffset(texcoord.zw, int2(-2, 1)).g;

        weights *= saturate(factor);
    }
#endif
}

float4 CalculateBlendWeights(float2 texcoord, float2 pixcoord, float4 offset[3])
{
    float4 weights = float4(0.0, 0.0, 0.0, 0.0);
    float2 e = SampleEdges(texcoord);

    if (e.g > 0.0)
    {
        float2 d;
        float3 coords;

        coords.x = SMAASearchXLeft(offset[0].xy, offset[2].x);
        coords.y = offset[1].y;
        d.x = coords.x;

        float e1 = SampleEdges(float2(coords.x, coords.y)).r;

        coords.z = SMAASearchXRight(offset[0].zw, offset[2].y);
        d.y = coords.z;

        d = abs(round(mad(SMAA_RT_METRICS.zz, d, -pixcoord.xx)));
        float2 sqrt_d = sqrt(d);

        float2 coordRight = float2(coords.z, coords.y) + float2(1.0, 0.0) * SMAA_RT_METRICS.xy;
        float e2 = SampleEdges(coordRight).r;

        weights.rg = SMAAArea(sqrt_d, e1, e2, 0.0);

        coords.y = texcoord.y;
        float4 cornerCoords = float4(coords.x, coords.y, coords.z, coords.y);
        SMAADetectHorizontalCornerPattern(weights.rg, cornerCoords, d);
    }

    if (e.r > 0.0)
    {
        float2 d;
        float3 coords;

        coords.y = SMAASearchYUp(offset[1].xy, offset[2].z);
        coords.x = offset[0].x;
        d.x = coords.y;

        float e1 = SampleEdges(float2(coords.x, coords.y)).g;

        coords.z = SMAASearchYDown(offset[1].zw, offset[2].w);
        d.y = coords.z;

        d = abs(round(mad(SMAA_RT_METRICS.ww, d, -pixcoord.yy)));
        float2 sqrt_d = sqrt(d);

        float2 coordDown = float2(coords.x, coords.z) + float2(0.0, 1.0) * SMAA_RT_METRICS.xy;
        float e2 = SampleEdges(coordDown).g;

        weights.ba = SMAAArea(sqrt_d, e1, e2, 0.0);

        coords.x = texcoord.x;
        float4 cornerCoords = float4(coords.x, coords.y, coords.x, coords.z);
        SMAADetectVerticalCornerPattern(weights.ba, cornerCoords, d);
    }

    return saturate(weights * BlendStrength);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    uint width;
    uint height;
    EdgeInput.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    if (width == 0 || height == 0)
        return;

    gSmaaMetrics = float4(InvResolution.xy, (float) width, (float) height);

    float2 texcoord = (float2(pixel) + 0.5) * InvResolution;
    float2 pixcoord = float2(pixel);

    float4 offset[3];
    offset[0] = mad(SMAA_RT_METRICS.xyxy, float4(-0.25, -0.125, 1.25, -0.125), texcoord.xyxy);
    offset[1] = mad(SMAA_RT_METRICS.xyxy, float4(-0.125, -0.25, -0.125, 1.25), texcoord.xyxy);
    offset[2] = mad(SMAA_RT_METRICS.xxyy, float4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS),
                    float4(offset[0].xz, offset[1].yw));

    float4 weights = CalculateBlendWeights(texcoord, pixcoord, offset);
    BlendOutput[pixel] = weights;
}
