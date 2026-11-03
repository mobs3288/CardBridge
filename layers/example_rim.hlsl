// layer: Example - rim light
// blend: add
// default: off
//
// A sample layer that shows the file format (see src/export/ShaderLayer.h); it is not one of the game's shaders. Copy it, rename it
// and change `ps` to try another look: the viewer lists every .hlsl in this folder in the Look tab and V toggles all of them.
//
// A soft light along the silhouette, where the surface turns away from the view, in the material's own colour so the edge
// glows with the cloth or skin it belongs to.
float4 ps(VO i) : SV_Target {
    float3 n = normalize(i.n);
    float facing = saturate(dot(n, view.xyz));
    float rim = pow(1.0 - facing, 3.0);
    float4 tex = mainTex.Sample(samp, i.uv);
    float3 tint = lerp(float3(1, 1, 1), tex.rgb * diffuse.rgb, 0.6);
    return float4(tint * rim * 0.35, 1);
}
