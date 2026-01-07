#pragma once

#include "Shader.h"
#include <glad/glad.h>
#include <memory>

namespace FluxPlayer {

class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    bool init(int videoWidth, int videoHeight);
    void destroy();

    void renderFrame(uint8_t* yData, uint8_t* uData, uint8_t* vData,
                     int yPitch, int uPitch, int vPitch);
    void clear(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f);

    void setVideoSize(int width, int height);

private:
    void setupQuad();
    void updateYUVTextures(uint8_t* yData, uint8_t* uData, uint8_t* vData,
                          int yPitch, int uPitch, int vPitch);

    std::unique_ptr<Shader> m_shader;

    GLuint m_VAO;
    GLuint m_VBO;

    GLuint m_textureY;
    GLuint m_textureU;
    GLuint m_textureV;

    int m_videoWidth;
    int m_videoHeight;
};

} // namespace FluxPlayer
