// Output is premultiplied alpha (DXGI_ALPHA_MODE_PREMULTIPLIED, blend ONE / INV_SRC_ALPHA)

cbuffer EffectConstants : register(b0)
{
    float2 gScreenSize;
    float2 gInvScreenSize;
    float  gSharpStrength;   // 0 = sharpening off
    float  gSharpClamp;
    float  gZoomFactor;
    float  gZoomRadius;
    float  gVignetteStrength;
    float  gVignetteRadius;
    float  gVignetteSoftness;
    float  gVibrance;        // -1..1, 0 = off
    float4 gZoomBorderColor;
};

Texture2D<float4> gScene       : register(t0);
SamplerState      gLinearClamp : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 Tap(float2 uv, float2 texel, float2 offset)
{
    return gScene.SampleLevel(gLinearClamp, uv + offset * texel, 0).rgb;
}

float3 Sharpen(float2 uv, float3 c)
{
    uint w, h;
    gScene.GetDimensions(w, h);
    float2 t = 1.0 / float2(w, h);

    float3 n  = Tap(uv, t, float2( 0, -1));
    float3 s  = Tap(uv, t, float2( 0,  1));
    float3 e  = Tap(uv, t, float2( 1,  0));
    float3 we = Tap(uv, t, float2(-1,  0));
    float3 ne = Tap(uv, t, float2( 1, -1));
    float3 nw = Tap(uv, t, float2(-1, -1));
    float3 se = Tap(uv, t, float2( 1,  1));
    float3 sw = Tap(uv, t, float2(-1,  1));

    float3 blur = (c * 4.0 + (n + s + e + we) * 2.0 + (ne + nw + se + sw)) / 16.0;

    float lc = dot(c, kLuma);
    float l1 = dot(n, kLuma), l2 = dot(s, kLuma), l3 = dot(e, kLuma), l4 = dot(we, kLuma);
    float lmin = min(lc, min(min(l1, l2), min(l3, l4)));
    float lmax = max(lc, max(max(l1, l2), max(l3, l4)));
    float range = lmax - lmin;

    // fade out on hard edges and near-flat areas: fewer halos, no grain
    float k = (1.0 - smoothstep(0.20, 0.55, range)) * smoothstep(0.004, 0.03, range);
    float d = clamp((lc - dot(blur, kLuma)) * gSharpStrength * 2.5 * k, -gSharpClamp, gSharpClamp);

    float slack = range * 0.25;
    float target = clamp(lc + d, lmin - slack, lmax + slack);
    return c + (target - lc);
}

// vibrance: dull colors get boosted more, already saturated ones barely move
float3 Vibrance(float3 c)
{
    float y = dot(c, kLuma);
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    float k = 1.0 + gVibrance * (1.0 - sat);
    return y + (c - y) * k;
}

float4 PSPost(VSOut i) : SV_Target
{
    float3 c = gScene.SampleLevel(gLinearClamp, i.uv, 0).rgb;
    if (gSharpStrength > 0.0)
        c = Sharpen(i.uv, c);
    if (gVibrance != 0.0)
        c = Vibrance(c);
    return float4(saturate(c), 1.0);
}

float4 PSVignette(VSOut i) : SV_Target
{
    float2 d = (i.uv - 0.5) * 2.0;
    d.x *= gScreenSize.x / gScreenSize.y * 0.5625;
    float r = length(d) * 0.7071;
    float a = gVignetteStrength * smoothstep(gVignetteRadius, gVignetteRadius + gVignetteSoftness, r);
    return float4(0.0, 0.0, 0.0, saturate(a));
}

float4 PSZoom(VSOut i) : SV_Target
{
    float2 center = gScreenSize * 0.5;
    float2 d = i.pos.xy - center;
    float r = length(d);
    clip(gZoomRadius + 2.0 - r);

    float2 src = center + d / gZoomFactor;
    float3 col = gScene.SampleLevel(gLinearClamp, src * gInvScreenSize, 0).rgb;

    float ring  = smoothstep(gZoomRadius - 3.5, gZoomRadius - 1.5, r) * gZoomBorderColor.a;
    float alpha = 1.0 - smoothstep(gZoomRadius - 0.5, gZoomRadius + 1.5, r);

    float3 o = lerp(col, gZoomBorderColor.rgb, ring);
    return float4(o * alpha, alpha);
}
