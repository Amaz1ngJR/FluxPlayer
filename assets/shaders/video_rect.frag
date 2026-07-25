#version 330 core

out vec4 FragColor;
in vec2 TexCoord;

// macOS 的 IOSurface 通过 CGLTexImageIOSurface2D 映射为矩形纹理。
// sampler2DRect 使用像素坐标，因此由纹理尺寸把顶点着色器输出的 0~1 坐标换算为像素。
uniform sampler2DRect texY;
uniform sampler2DRect texUV;
uniform vec2 textureSizeY;
uniform vec2 textureSizeUV;

// 色彩空间：0 = BT.601，1 = BT.709，2 = BT.2020
uniform int colorSpace;
// 量化范围：0 = TV/limited，1 = PC/full
uniform int fullRange;

void main()
{
    float y = texture(texY, TexCoord * textureSizeY).r;
    vec2 uv = texture(texUV, TexCoord * textureSizeUV).rg - vec2(0.5);

    if (fullRange == 0) {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        uv *= 255.0 / 224.0;
    }

    float r;
    float g;
    float b;
    if (colorSpace == 2) {
        r = y + 1.4746 * uv.y;
        g = y - 0.1646 * uv.x - 0.5714 * uv.y;
        b = y + 1.8814 * uv.x;
    } else if (colorSpace == 1) {
        r = y + 1.5748 * uv.y;
        g = y - 0.1873 * uv.x - 0.4681 * uv.y;
        b = y + 1.8556 * uv.x;
    } else {
        r = y + 1.402 * uv.y;
        g = y - 0.344 * uv.x - 0.714 * uv.y;
        b = y + 1.772 * uv.x;
    }

    FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
