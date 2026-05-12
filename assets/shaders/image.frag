#version 330 core

out vec4 FragColor;
in vec2 TexCoord;

// RGBA 纹理采样器（直通，不经过 YUV→RGB 转换）
uniform sampler2D texRGBA;

void main()
{
    FragColor = texture(texRGBA, TexCoord);
}
