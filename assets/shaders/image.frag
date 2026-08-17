#version 330 core

out vec4 FragColor;
in vec2 TexCoord;

// RGBA 纹理采样器（直通，不经过 YUV→RGB 转换）
uniform sampler2D texRGBA;

// 显示亮度倍率，封面和静态图片与视频保持一致的用户体验。
uniform float brightness = 1.0;

// 透明度（用于截图缩放动画等特效）
uniform float alpha = 1.0;

void main()
{
    vec4 color = texture(texRGBA, TexCoord);
    FragColor = vec4(clamp(color.rgb * brightness, 0.0, 1.0), color.a * alpha);
}
