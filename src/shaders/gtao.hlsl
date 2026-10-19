// GTAO (ground-truth ambient occlusion) in place of Unreal 4.21's screen space AO, for
// Jedi: Fallen Order. It replaces the single-level, full-resolution AO pass
// (FPostProcessAmbientOcclusionPS<0, 0, Quality>, which the game runs when
// r.AmbientOcclusionLevels is 1) and keeps that pass's bindings, so it drops into the
// game's pipeline unchanged. See docs/FINDINGS.md, "Ambient occlusion".
//
// Bindings (from the game's shader; reflection is stripped, so by register):
//   v0      xy = scene texture UV, zw = screen position (-1..1, y up)
//   t0/s0   scene depth (device Z)          t1/s1  GBuffer A (world normal * 0.5 + 0.5)
//   t2/s2   GBuffer B (shading model in a)  t4/s4  random rotation texture
//   cb0     the pass's parameters: [30].x power, .y bias, .w intensity; [31].w aspect ratio,
//           .xy random UV scale; [31].z radius; [32].z radius in world space (1) or
//           screen space (0); [33].w 1 / tan(half FOV), .xy random UV offset;
//           [34].xy distance fade
//   cb1     View: [8..10] TranslatedWorldToView rows, [57] InvDeviceZToWorldZTransform
// Output: the AO factor in every channel (1 = unoccluded), as the original writes it.

cbuffer Parameters : register(b0) { float4 P[39]; };
cbuffer View : register(b1) { float4 V[58]; };

Texture2D SceneDepth : register(t0);
Texture2D GBufferA : register(t1);
Texture2D GBufferB : register(t2);
Texture2D RandomNormals : register(t4);
SamplerState SceneDepthSampler : register(s0);
SamplerState GBufferASampler : register(s1);
SamplerState GBufferBSampler : register(s2);
SamplerState RandomSampler : register(s4);

static const float kPi = 3.14159265;
static const int kSlices = 4;        // each searched both ways
static const int kSteps = 8;         // per side
static const float kMaxRadiusPixels = 256.0;
// A sample below the current horizon pulls it back down a little, so thin objects in front
// (railings, branches) don't cast wide dark halos.
static const float kHorizonDecay = 0.15;
static const uint kShadingModelHair = 7;  // the original gives hair no normal

float ConvertFromDeviceZ(float z) { return z * V[57].x + V[57].y + 1.0 / (z * V[57].z - V[57].w); }

float2 Rotate(float2 v, float2 cosSin) { return float2(v.x * cosSin.x - v.y * cosSin.y, v.x * cosSin.y + v.y * cosSin.x); }

float4 main(noperspective float4 UVAndScreenPos : TEXCOORD0, float4 SvPosition : SV_Position) : SV_Target
{
    float2 uv = UVAndScreenPos.xy;
    float2 screenPos = UVAndScreenPos.zw;
    // Screen position -> UV, and the size of a pixel in screen position units, from the
    // interpolants (both are linear across the screen).
    float2 dUv = float2(ddx(UVAndScreenPos.x), ddy(UVAndScreenPos.y));
    float2 dScreen = float2(ddx(UVAndScreenPos.z), ddy(UVAndScreenPos.w));
    float2 uvScale = dUv / dScreen;
    float2 uvBias = uv - uvScale * screenPos;
    float2 pixel = abs(dScreen);

    float depth = ConvertFromDeviceZ(SceneDepth.SampleLevel(SceneDepthSampler, uv, 0).x);
    float fade = saturate(depth * P[34].x + P[34].y);

    // View space: x right, y up, z forward.
    float3 invFovFix = float3(1.0 / P[33].w, 1.0 / (P[31].w * P[33].w), 1.0);
    float3 p = float3(screenPos * depth, depth) * invFovFix;
    float3 v = normalize(-p);

    uint shadingModel = (uint)round(GBufferB.SampleLevel(GBufferBSampler, uv, 0).a * 255.0) & 15;
    float3 n = v;
    if (shadingModel != kShadingModelHair)
    {
        float3 worldN = GBufferA.SampleLevel(GBufferASampler, uv, 0).xyz * 2.0 - 1.0;
        float3 viewN = worldN.x * V[8].xyz + worldN.y * V[9].xyz + worldN.z * V[10].xyz;
        if (dot(viewN, viewN) > 1e-6) n = normalize(viewN);
    }

    float radius = P[31].z * lerp(depth, 1.0, P[32].z);  // view units (cm)
    float radiusPixels = min(radius * P[33].w / depth / pixel.x, kMaxRadiusPixels);
    float visibility = 1.0;
    if (fade < 1.0 && radiusPixels >= 1.0)
    {
        float negInvR2 = -1.0 / (radius * radius);
        // The game's per-pixel random rotation (its offset moves every frame, so TAA and
        // DLSS average the noise away), and a per-pixel step jitter.
        float2 rnd = RandomNormals.Sample(RandomSampler, uv * P[31].xy + P[33].xy).xz * 2.0 - 1.0;
        float2 rotation = rnd / max(length(rnd), 1e-4);
        float jitter = frac(52.9829189 * frac(dot(SvPosition.xy, float2(0.06711056, 0.00583715))) + rnd.x * 0.5);
        float stepPixels = radiusPixels / (kSteps + 1);

        float sum = 0.0, weight = 0.0;
        [unroll] for (int slice = 0; slice < kSlices; ++slice)
        {
            float angle = (slice + 0.5) * (kPi / kSlices);
            float2 dir = Rotate(float2(cos(angle), sin(angle)), rotation);  // pixels, y up
            float3 sliceDir = normalize(float3(dir * pixel * invFovFix.xy, 0.0));
            float3 orthoDir = sliceDir - dot(sliceDir, v) * v;
            float3 axis = normalize(cross(orthoDir, v));
            float3 projN = n - axis * dot(n, axis);
            float projLen = length(projN);
            float cosN = saturate(dot(projN, v) / max(projLen, 1e-4));
            float nAngle = (dot(orthoDir, projN) < 0.0 ? -1.0 : 1.0) * acos(cosN);

            float horizonCos[2];
            [unroll] for (int side = 0; side < 2; ++side)
            {
                float2 sideDir = side ? dir : -dir;
                float h = -1.0;
                [unroll] for (int k = 0; k < kSteps; ++k)
                {
                    float2 s = screenPos + sideDir * pixel * ((k + jitter) * stepPixels + 1.0);
                    float sd = ConvertFromDeviceZ(SceneDepth.SampleLevel(SceneDepthSampler, s * uvScale + uvBias, 0).x);
                    float3 d = float3(s * sd, sd) * invFovFix - p;
                    float dd = dot(d, d);
                    float c = dot(d, v) * rsqrt(max(dd, 1e-8));
                    float falloff = saturate(dd * negInvR2 + 1.0);
                    c = lerp(-1.0, c, falloff);  // fade with distance
                    // Only samples within the radius may lower the horizon.
                    h = c > h ? c : lerp(h, c, kHorizonDecay * falloff);
                }
                horizonCos[side] = h;
            }
            float h0 = nAngle + max(-acos(horizonCos[0]) - nAngle, -kPi * 0.5);
            float h1 = nAngle + min(acos(horizonCos[1]) - nAngle, kPi * 0.5);
            float sinN = sin(nAngle), cosNA = cos(nAngle);
            float arc = (-cos(2.0 * h0 - nAngle) + cosNA + 2.0 * h0 * sinN) +
                        (-cos(2.0 * h1 - nAngle) + cosNA + 2.0 * h1 * sinN);
            // Divided by the same integral with nothing in the way, so an open surface is
            // exactly 1 whatever the slice's tilt.
            float open = 4.0 * (cosNA + nAngle * sinN);
            sum += projLen * saturate(arc / max(open, 1e-4));
            weight += projLen;
        }
        visibility = sum / max(weight, 1e-4);
    }

    // The original's last steps: distance fade, then the post process volume's power and
    // intensity.
    float ao = pow(max(lerp(visibility, 1.0, fade), 1e-6), P[30].x);
    return 1.0 - P[30].w * (1.0 - ao);
}
