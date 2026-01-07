#version 330 core

out vec4 FragColor;
in vec2 TexCoord;

// YUV 纹理（三个独立的纹理）
uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;

void main()
{
    // 采样 YUV 分量
    float y = texture(texY, TexCoord).r;
    float u = texture(texU, TexCoord).r - 0.5;
    float v = texture(texV, TexCoord).r - 0.5;

    // YUV 转 RGB (BT.601 标准)
    float r = y + 1.402 * v;
    float g = y - 0.344 * u - 0.714 * v;
    float b = y + 1.772 * u;

    FragColor = vec4(r, g, b, 1.0);
}
