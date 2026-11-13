#version 450

layout(binding = 0, std140) uniform Params
{
    vec4 values;
} params;

layout(binding = 1) uniform sampler2D videoPlane;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 uv = vUv;
    const float quarterTurns = params.values.y;
    if (quarterTurns > 0.5 && quarterTurns < 1.5) {
        uv = vec2(vUv.y, 1.0 - vUv.x);
    } else if (quarterTurns > 1.5 && quarterTurns < 2.5) {
        uv = vec2(1.0 - vUv.x, 1.0 - vUv.y);
    } else if (quarterTurns > 2.5) {
        uv = vec2(1.0 - vUv.y, vUv.x);
    }

    fragColor = texture(videoPlane, uv);
}
