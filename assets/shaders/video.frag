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

// 色彩空间：0 = BT.601（SD），1 = BT.709（HD），2 = BT.2020（UHD）
uniform int colorSpace;
// 量化范围：0 = TV/limited range（Y:16-235），1 = PC/full range（Y:0-255）
uniform int fullRange;

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

    // ===== 量化范围处理 =====
    // TV/limited range (ITU-R BT.601/709/2020 共用量化规则):
    //   Y  有效区间 [16, 235]，归一化后需缩放: (Y - 16/255) * 255/219
    //   Cb/Cr 有效区间 [16, 240]，归一化后需缩放: UV * 255/224
    // PC/full range: Y [0,255] → [0,1]，UV 已减 0.5 无需额外缩放
    if (fullRange == 0) {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        u = u * (255.0 / 224.0);
        v = v * (255.0 / 224.0);
    }

    // ===== YUV → RGB 色彩空间转换 =====
    // 系数来源：各标准的 YCbCr→RGB 反变换公式 R = Y + Cr*(2-2*Kr), B = Y + Cb*(2-2*Kb)
    // G = Y - Cb*(2*Kb*(1-Kb)/Kg) - Cr*(2*Kr*(1-Kr)/Kg)，其中 Kg = 1-Kr-Kb
    float r, g, b;
    if (colorSpace == 2) {
        // ITU-R BT.2020 (UHD/4K): Kr=0.2627, Kb=0.0593
        r = y + 1.4746 * v;
        g = y - 0.1646 * u - 0.5714 * v;
        b = y + 1.8814 * u;
    } else if (colorSpace == 1) {
        // ITU-R BT.709 (HD): Kr=0.2126, Kb=0.0722
        r = y + 1.5748 * v;
        g = y - 0.1873 * u - 0.4681 * v;
        b = y + 1.8556 * u;
    } else {
        // ITU-R BT.601 (SD): Kr=0.299, Kb=0.114
        r = y + 1.402 * v;
        g = y - 0.344 * u - 0.714 * v;
        b = y + 1.772 * u;
    }

    FragColor = vec4(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0), 1.0);
}
