#version 330 core

out vec4 FragColor;
in vec2 TexCoord;

// RGBA 纹理采样器（直通，不经过 YUV→RGB 转换）
uniform sampler2D texRGBA;

// 透明度（用于截图缩放动画等特效）
uniform float alpha = 1.0;

void main()
{
    vec4 color = texture(texRGBA, TexCoord);
    FragColor = vec4(color.rgb, color.a * alpha);
}
