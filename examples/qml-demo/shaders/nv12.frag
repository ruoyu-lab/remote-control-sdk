#version 440

// Limited-range NV12 to RGB, BT.709 or BT.601.
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Parameters {
    mat4 mvp;
    float bt709;
    float opacity;
};
layout(binding = 1) uniform sampler2D lumaTexture;
layout(binding = 2) uniform sampler2D chromaTexture;

void main() {
    float y = (texture(lumaTexture, v_texcoord).r - 16.0 / 255.0) * (255.0 / 219.0);
    vec2 c = (texture(chromaTexture, v_texcoord).rg - 128.0 / 255.0) * (255.0 / 224.0);
    vec3 rgb = bt709 > 0.5 ? vec3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x)
                           : vec3(y + 1.402 * c.y, y - 0.3441 * c.x - 0.7141 * c.y, y + 1.772 * c.x);
    fragColor = vec4(clamp(rgb, 0.0, 1.0) * opacity, opacity);
}
