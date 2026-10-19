// Unreal 4.21's shadow projection pass for Jedi: Fallen Order, rebuilt, with soft (PCSS) and
// contact shadows for the sun. It replaces FShadowProjectionPS<Quality 4/5, UseFadePlane,
// UseTransmission> (USE_FADE_PLANE and USE_TRANSMISSION select the variant) with the same
// bindings, so it drops into the game's pipeline unchanged. Without settings from
// FallenOrderTweaks it runs the game's own filtering. See docs/FINDINGS.md, "Shadows".
//
// Bindings (from the game's shader; reflection is stripped, so by register):
//   v0      SV_Position
//   t0/s0   scene depth (device Z)
//   without transmission:
//     t1/s1 custom data (a = subsurface opacity), t2/s2 GBuffer B (shading model in a),
//     t3/s3 shadow depth map (this cascade's region)
//   with transmission:
//     t1/s1 GBuffer A (world normal), t2/s2 subsurface profile ID (a), t3/s3 custom data
//     (a = opacity), t4/s4 GBuffer B, t5/s5 shadow depth map, t6 subsurface profiles
//   cb0     [2].z soft transition scale; [3] shadow map size, 1 / size; [28].y fade fraction,
//           .z sharpen; [30..33] ScreenToShadowMatrix rows; [34].xy subsurface depth bias
//           and density; fade plane offset and 1 / length: [34].zw, or [34].w and [35].x
//           with transmission
//   cb1     View: [8..10] TranslatedWorldToView (transmission), [24..27] ViewToClip,
//           [57] InvDeviceZToWorldZTransform, [58] ScreenPositionScaleBias,
//           [123].zw 1 / buffer size, [135].x StateFrameIndexMod8,
//           [166].x DistanceFieldAOSpecularOcclusionMode (the settings code, see
//           DecodeSettings)
// Output: sqrt of (shadow, subsurface shadow, shadow, subsurface shadow), or with the fade
// plane (shadow, subsurface shadow, shadow, cascade fade).

#ifndef USE_FADE_PLANE
#define USE_FADE_PLANE 0
#endif
#ifndef USE_TRANSMISSION
#define USE_TRANSMISSION 0
#endif

cbuffer Parameters : register(b0) { float4 P[36]; };
cbuffer View : register(b1) { float4 V[167]; };

Texture2D SceneDepth : register(t0);
SamplerState SceneDepthSampler : register(s0);
#if USE_TRANSMISSION
Texture2D GBufferA : register(t1);
Texture2D ProfileId : register(t2);
Texture2D CustomData : register(t3);
Texture2D GBufferB : register(t4);
Texture2D ShadowDepth : register(t5);
Texture2D SubsurfaceProfiles : register(t6);
SamplerState GBufferASampler : register(s1);
SamplerState ProfileIdSampler : register(s2);
SamplerState CustomDataSampler : register(s3);
SamplerState GBufferBSampler : register(s4);
SamplerState ShadowDepthSampler : register(s5);
#else
Texture2D CustomData : register(t1);
Texture2D GBufferB : register(t2);
Texture2D ShadowDepth : register(t3);
SamplerState CustomDataSampler : register(s1);
SamplerState GBufferBSampler : register(s2);
SamplerState ShadowDepthSampler : register(s3);
#endif

static const float kPi = 3.14159265;

float ConvertFromDeviceZ(float z) { return z * V[57].x + V[57].y + 1.0 / (z * V[57].z - V[57].w); }

// ---------------------------------------------------------------- settings channel
// FallenOrderTweaks sets r.AOSpecularOcclusionMode, which reaches every shader as
// View.DistanceFieldAOSpecularOcclusionMode and otherwise only matters for distance field AO
// (the game ships no distance fields), to code = 1000 + sunSize * 100 + contact:
//   sunSize 0..49: apparent sun diameter in 0.1 degree steps (the real sun: 5)
//   contact 0..99: contact shadow length in 2 cm steps (0 = off)
// Any other value (the game's 0 or 1) means the game's own shadows.
struct Settings
{
    bool on;
    float tanHalfSun;
    float contactLength;  // cm
};

Settings DecodeSettings()
{
    Settings st;
    uint code = asuint(V[166].x);
    st.on = code >= 1000 && code < 6000;
    uint p = st.on ? code - 1000 : 0;
    st.tanHalfSun = tan(radians((p / 100) * 0.1) * 0.5);
    st.contactLength = (p % 100) * 2.0;
    return st;
}

// ---------------------------------------------------------------- the game's filtering
// 5x5 percentage closer filter from nine gathers over 6x6 texels, the outer rows and columns
// weighted by the sub-texel position (Quality 4 and 5). Each texel compares softly:
// saturate((occluder - receiver) * transition + 1). The same texels give the subsurface
// shadow: exp(-max(receiver + bias - occluder, 0) * density), 1 past depth 0.99.
void GamePcf(float2 shadowUv, float receiverZ, float sssReceiverZ, float density, bool subsurface,
             out float shadow, out float sssShadow)
{
    float2 texelPos = shadowUv * P[3].xy - 0.5;
    float2 f = frac(texelPos);
    float2 center = (floor(texelPos) + 0.5) * P[3].zw;
    float d[6][6];
    [unroll] for (int gy = 0; gy < 3; ++gy)
        [unroll] for (int gx = 0; gx < 3; ++gx)
        {
            // Gather order: x (0,1), y (1,1), z (1,0), w (0,0).
            float4 g = ShadowDepth.GatherRed(ShadowDepthSampler, center, int2(gx * 2 - 2, gy * 2 - 2));
            d[gy * 2][gx * 2] = g.w;
            d[gy * 2][gx * 2 + 1] = g.z;
            d[gy * 2 + 1][gx * 2] = g.x;
            d[gy * 2 + 1][gx * 2 + 1] = g.y;
        }
    float weightX[6] = {1.0 - f.x, 1.0, 1.0, 1.0, 1.0, f.x};
    float weightY[6] = {1.0 - f.y, 1.0, 1.0, 1.0, 1.0, f.y};
    shadow = 0.0;
    sssShadow = 0.0;
    [unroll] for (int y = 0; y < 6; ++y)
        [unroll] for (int x = 0; x < 6; ++x)
        {
            float w = weightX[x] * weightY[y];
            shadow += w * saturate((d[y][x] - receiverZ) * P[2].z + 1.0);
            float s = min(exp(-max(sssReceiverZ - d[y][x], 0.0) * density), 1.0);
            sssShadow += w * (d[y][x] > 0.99 ? 1.0 : s);
        }
    shadow *= 0.04;
    sssShadow = subsurface ? sssShadow * 0.04 : 1.0;
}

// ---------------------------------------------------------------- the sun's light space
// The ScreenToShadow matrix maps (screen position * depth, depth, 1) to shadow UV and depth.
// With the projection's scale (ViewToClip) its rows become view-space gradients of shadow u,
// v and z per cm: u and v are perpendicular to the light, and z grows away from it.
struct LightSpace
{
    float uvPerCm;
    float zPerCm;
    float3 toLight;  // view space (x right, y up, z forward)
};

LightSpace GetLightSpace()
{
    float px = V[24].x, py = V[25].y;
    float2 jitter = float2(V[26].x, V[26].y);
    float3 gx = P[30].xyz * px;                         // d shadow / d view x
    float3 gy = P[31].xyz * py;                         // d shadow / d view y
    float3 gz = P[32].xyz + P[30].xyz * jitter.x + P[31].xyz * jitter.y;  // d shadow / d view z
    float3 du = float3(gx.x, gy.x, gz.x), dv = float3(gx.y, gy.y, gz.y), dz = float3(gx.z, gy.z, gz.z);
    LightSpace ls;
    ls.uvPerCm = length(du);
    ls.zPerCm = length(dz);
    float3 c = normalize(cross(du, dv));
    ls.toLight = dot(c, dz) > 0.0 ? -c : c;
    return ls;
}

float2 Vogel(int i, int n, float rotation)
{
    float r = sqrt((i + 0.5) / n);
    float a = i * 2.39996323 + rotation;
    return r * float2(cos(a), sin(a));
}

// ---------------------------------------------------------------- soft shadows (PCSS)
static const int kSearchTaps = 16;
static const int kFilterTaps = 24;
// Kernel cap in texels: keeps taps inside the cascade's region of the shadow depth atlas.
static const float kMaxRadiusTexels = 16.0;
static const float kMinRadiusTexels = 1.5;  // about the game's own filter

float SoftShadow(float2 shadowUv, float receiverZ, float rotation, LightSpace ls, Settings st)
{
    float texel = P[3].z;
    float maxRadius = kMaxRadiusTexels * texel;
    // Each cm of occluder height widens the penumbra by tan(sun radius) cm.
    float widen = st.tanHalfSun * ls.uvPerCm / max(ls.zPerCm, 1e-12);

    float blockers = 0.0, blockerZ = 0.0;
    [unroll] for (int i = 0; i < kSearchTaps; ++i)
    {
        float d = ShadowDepth.SampleLevel(ShadowDepthSampler, shadowUv + Vogel(i, kSearchTaps, rotation) * maxRadius, 0).r;
        if (d < receiverZ)
        {
            blockers += 1.0;
            blockerZ += d;
        }
    }
    if (blockers == 0.0)
        return 1.0;
    blockerZ /= blockers;
    float radius = clamp((receiverZ - blockerZ) * widen, kMinRadiusTexels * texel, maxRadius);

    // Wider kernels reach across sloped receivers: bias each tap by its distance (a 45 degree
    // slope stays clean), on top of the game's soft comparison.
    float slope = ls.zPerCm / max(ls.uvPerCm, 1e-12);
    float lit = 0.0;
    [unroll] for (int j = 0; j < kFilterTaps; ++j)
    {
        float2 o = Vogel(j, kFilterTaps, rotation) * radius;
        float d = ShadowDepth.SampleLevel(ShadowDepthSampler, shadowUv + o, 0).r;
        lit += saturate((d - receiverZ + slope * length(o)) * P[2].z + 1.0);
    }
    return lit / kFilterTaps;
}

// ---------------------------------------------------------------- contact shadows
// March from the pixel towards the sun through the depth buffer, and darken if something
// blocks the ray: the small-scale contact the shadow map is too coarse for.
float2 UvOfView(float3 p)
{
    float2 ndc = float2(p.x * V[24].x, p.y * V[25].y) / p.z + float2(V[26].x, V[26].y);
    return ndc * V[58].xy + V[58].wz;
}

float ContactShadow(float3 viewPos, float rotation, LightSpace ls, Settings st)
{
    const int kSteps = 12;
    float stepLength = st.contactLength / kSteps;
    float jitter = frac(rotation * 0.15915494 + 0.5);
    float thickness = max(15.0, st.contactLength * 0.5);
    float occlusion = 0.0;
    [loop] for (int i = 0; i < kSteps; ++i)
    {
        float3 p = viewPos + ls.toLight * (stepLength * (i + jitter));
        if (p.z <= 1.0)
            break;
        float2 uv = UvOfView(p);
        if (any(uv < 0.0) || any(uv > 1.0))
            break;
        float sceneZ = ConvertFromDeviceZ(SceneDepth.SampleLevel(SceneDepthSampler, uv, 0).x);
        float behind = p.z - sceneZ;
        if (behind > 2.0 && behind < thickness)
        {
            occlusion = 1.0 - (float)i / kSteps;  // taper towards the end of the ray
            break;
        }
    }
    // Fade out with distance: the shadow map covers far detail well.
    return 1.0 - occlusion * saturate(2.0 - viewPos.z / 2500.0);
}

// ---------------------------------------------------------------- transmission
#if USE_TRANSMISSION
// Subsurface profile transmission (shading model 5), as the original: push the shadow lookup
// into the surface along the view-space normal by the profile's normal scale, then sum a
// clamped thickness over 16 Poisson taps (the game's table, entries 15-30).
static const float2 kPoisson[16] = {
    float2(0.403645, -0.793536), float2(0.382645, -0.273012), float2(-0.041249, -0.597179),
    float2(-0.370926, -0.917990), float2(-0.379535, -0.335349), float2(-0.315447, 0.106907),
    float2(-0.767181, -0.614345), float2(-0.486522, 0.639513),  float2(0.235987, 0.151055),
    float2(0.030929, 0.730902),   float2(-0.828460, -0.105583), float2(-0.873238, 0.303417),
    float2(0.926868, -0.116035),  float2(0.698010, 0.376487),   float2(0.823992, -0.515003),
    float2(0.508441, 0.753353)};

float ProfileTransmission(float2 uv, float3 screenPosition, bool noProfile)
{
    float3 n = normalize(GBufferA.SampleLevel(GBufferASampler, uv, 0).xyz * 2.0 - 1.0);
    float idRaw = noProfile ? 0.0 : ProfileId.SampleLevel(ProfileIdSampler, uv, 0).a;
    uint profile = (uint)(idRaw * 255.0 + 0.5);
    float2 params = SubsurfaceProfiles.Load(int3(1, profile, 0)).xy;
    float3 viewN = n.x * V[8].xyz + n.y * V[9].xyz + n.z * V[10].xyz;
    float extinction = params.x * P[34].y * 3.1;
    float normalScale = params.y * 0.5;
    float3 p = screenPosition - normalScale * viewN;
    float4 t = p.x * P[30] + p.y * P[31] + p.z * P[32] + P[33];
    float2 tuv = t.xy / t.w;
    float tz = t.z + P[34].x;
    float sum = 0.0;
    [loop] for (int i = 0; i < 16; ++i)
    {
        float d = ShadowDepth.SampleLevel(ShadowDepthSampler, tuv + kPoisson[i] * P[3].w, 0).r;
        float delta = tz - d;
        float v = delta * extinction + normalScale;
        v = delta * extinction > 0.0 ? v : max(v, 0.0);
        sum += clamp(abs(v), 0.15, 5.0) + 0.25;
    }
    return 1.0 - sum * 0.0125;
}
#endif

// ---------------------------------------------------------------- main
[earlydepthstencil]  // as the original: the cascade's stencil bounds are tested first
float4 main(float4 SvPosition : SV_Position) : SV_Target
{
    float2 uv = SvPosition.xy * V[123].zw;
    float sceneW = ConvertFromDeviceZ(SceneDepth.SampleLevel(SceneDepthSampler, uv, 0).x);
    float2 screenPos = (uv - V[58].wz) / V[58].xy;
    float4 s = (screenPos.x * sceneW) * P[30] + (screenPos.y * sceneW) * P[31] + sceneW * P[32] + P[33];
    float2 shadowUv = s.xy / s.w;
    float receiverZ = min(s.z, 0.99999);

    // Subsurface shadow inputs, as the original: shading models 2, 3, 5, 6, 7 and 9 get it;
    // 5, 7 and 9 at full density, the others by their opacity.
    uint id = (uint)round(GBufferB.SampleLevel(GBufferBSampler, uv, 0).a * 255.0);
    uint model = id & 15;
    bool subsurface = model == 2 || model == 3 || model == 5 || model == 6 || model == 7 || model == 9;
    float density = 0.0;
    if (subsurface)
    {
        float opacity = min(CustomData.SampleLevel(CustomDataSampler, uv, 0).a, 0.999);
        density = (id & 16) ? 0.0 : log(1.0 - opacity) * -0.034657;
        if (model == 5 || model == 7 || model == 9)
            density = 1.0;
        density *= P[34].y;
    }

    float shadow, sssShadow;
    GamePcf(shadowUv, receiverZ, s.z + P[34].x, density, subsurface, shadow, sssShadow);
#if USE_TRANSMISSION
    if (model == 5)
        sssShadow = ProfileTransmission(uv, float3(screenPos * sceneW, sceneW), (id & 16) != 0);
#endif
    // The game's sharpening (the light's Shadow Sharpen).
    shadow = saturate((shadow - 0.5) * P[28].z + 0.5);

    // The sun only: its projection is orthographic (w doesn't vary); spot lights' isn't.
    Settings st = DecodeSettings();
    bool sun = abs(P[30].w) + abs(P[31].w) + abs(P[32].w) < 1e-6;
    if (st.on && sun)
    {
        LightSpace ls = GetLightSpace();
        // Interleaved gradient noise, moving every frame so TAA and DLSS average it away.
        float2 noisePos = SvPosition.xy + (asuint(V[135].x) & 7) * float2(5.588238, 5.588238);
        float rotation = frac(52.9829189 * frac(dot(noisePos, float2(0.06711056, 0.00583715)))) * 2.0 * kPi;
        if (st.tanHalfSun > 0.0)
            shadow = SoftShadow(shadowUv, receiverZ, rotation, ls, st);
        if (st.contactLength > 0.0)
        {
            float3 viewPos = float3((screenPos - float2(V[26].x, V[26].y)) * sceneW / float2(V[24].x, V[25].y), sceneW);
            shadow = min(shadow, ContactShadow(viewPos, rotation, ls, st));
        }
    }

    float fade = P[28].y;
    float faded = fade * (shadow * shadow - 1.0) + 1.0;
    float fadedSss = fade * (sssShadow * sssShadow - 1.0) + 1.0;
#if USE_FADE_PLANE && USE_TRANSMISSION
    float planeFade = saturate((sceneW - P[34].w) * P[35].x);
#elif USE_FADE_PLANE
    float planeFade = saturate((sceneW - P[34].z) * P[34].w);
#endif
#if USE_FADE_PLANE
    return float4(sqrt(faded), sqrt(fadedSss), sqrt(faded), 1.0 - planeFade);
#else
    return sqrt(float4(faded, fadedSss, faded, fadedSss));
#endif
}
