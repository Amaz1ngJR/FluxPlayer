#pragma once

#include <string>
#include <glad/glad.h>

namespace FluxPlayer {

class Shader {
public:
    Shader();
    ~Shader();

    bool loadFromFile(const std::string& vertexPath, const std::string& fragmentPath);
    bool loadFromSource(const std::string& vertexSource, const std::string& fragmentSource);

    void use() const;
    void unuse() const;

    GLuint getProgram() const { return m_program; }

    // Uniform 设置函数
    void setInt(const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setVec2(const std::string& name, float x, float y) const;
    void setVec3(const std::string& name, float x, float y, float z) const;
    void setVec4(const std::string& name, float x, float y, float z, float w) const;
    void setMat4(const std::string& name, const float* value) const;

private:
    bool compileShader(GLuint shader, const std::string& source);
    bool linkProgram();
    std::string readFile(const std::string& filepath);

    GLuint m_program;
    GLuint m_vertexShader;
    GLuint m_fragmentShader;
};

} // namespace FluxPlayer
