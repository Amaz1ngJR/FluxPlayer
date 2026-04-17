#version 330 core

out vec4 FragColor;
in vec2 TexCoord;

// YUV 纹理采样器
// YUV420P 模式：texY = Y平面, texU = U平面, texV = V平面（均为单通道 GL_RED）
// NV12 模式：  texY = Y平面(GL_RED), texU = UV交错平面(GL_RG8), texV 不使用
uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;

// 像素格式标志：0 = YUV420P（软件解码），1 = NV12（硬件解码）
// 所有片段取同一值，GPU 不存在分支发散惩罚
uniform int isNV12;

void main()
{
    // 采样 Y 分量（两种格式共用）
    float y = texture(texY, TexCoord).r;
    float u, v;

    if (isNV12 == 1) {
        // NV12：U 和 V 交错存储在同一纹理的 R/G 两个通道中
        vec2 uv = texture(texU, TexCoord).rg;
        u = uv.r - 0.5;
        v = uv.g - 0.5;
    } else {
        // YUV420P：U 和 V 各自独立的单通道纹理
        u = texture(texU, TexCoord).r - 0.5;
        v = texture(texV, TexCoord).r - 0.5;
    }

    // YUV 转 RGB (BT.601 标准)
    float r = y + 1.402 * v;
    float g = y - 0.344 * u - 0.714 * v;
    float b = y + 1.772 * u;

    FragColor = vec4(r, g, b, 1.0);
}
