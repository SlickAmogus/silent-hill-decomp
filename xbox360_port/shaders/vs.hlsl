//-----------------------------------------------------------------------------
// vs.hlsl - Silent Hill Xbox 360 vertex shader.
//
// gpu_xbox.c has already done the PSX work: it walked the ordering table,
// decoded the primitives and ran the software GTE, so vertices arrive in SCREEN
// PIXELS, not model space. There is no model/view/projection here on purpose --
// all this stage does is map pixels to clip space and pass the rest through.
//
// The vertex layout is ShVertex (gpu_nv2a.h), 64 bytes:
//   POSITION  float4  screen x, y, z, w   (w = 1.0 affine; PGXP is compiled out)
//   NORMAL    float4  diffuse colour 0..1, ALREADY x2-modulated by PutVertUV
//   TEXCOORD0 float2  texture coords in TEXELS, not normalised
//   TEXCOORD1 float4  per-vertex fog, ADDED after modulation (tex*col + spec)
//
// Colour rides in the NORMAL slot because that is what the vertex format
// declares; Xe_ShaderApplyVFetchPatches binds by semantic, so the name is just a
// label. It is not lighting and must not be normalised.
//-----------------------------------------------------------------------------

// (2/viewportW, -2/viewportH, -1, +1): pixels -> clip. Y is negated because PSX
// screen Y grows downward and clip space grows upward.
float4 gViewport : register(c0);

// (1/textureW, 1/textureH, 0, 0): texels -> normalised UV. Set per texture bind
// rather than hardcoded to 1/256, because the decoded page is not always 256².
float4 gTexScale : register(c1);

struct Input
{
    float4 pos  : POSITION;
    float4 col  : NORMAL;
    float2 uv   : TEXCOORD0;
    float4 spec : TEXCOORD1;
};

struct Output
{
    float4 pos  : POSITION;
    float4 col  : NORMAL;
    float2 uv   : TEXCOORD0;
    float4 spec : TEXCOORD1;
};

Output main(Input input)
{
    Output output;

    // Depth is resolved by the ordering table (painter order), not by a Z test,
    // so everything sits on one plane. W is 1 for the affine PSX look; with
    // PGXP on (use_pgxp), gpu_xbox.c feeds the real per-vertex view depth, and
    // pre-multiplying the already-projected clip position by it makes the
    // rasteriser interpolate colour/UV perspective-correctly (and clip W <= 0).
    output.pos.x = (input.pos.x * gViewport.x + gViewport.z) * input.pos.w;
    output.pos.y = (input.pos.y * gViewport.y + gViewport.w) * input.pos.w;
    output.pos.z = 0.0f;
    output.pos.w = input.pos.w;

    output.col  = input.col;
    output.uv   = input.uv * gTexScale.xy;
    output.spec = input.spec;

    return output;
}
