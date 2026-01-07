/**
 * Shader.cpp - OpenGL 着色器管理实现
 *
 * 功能：加载、编译和管理 GLSL 着色器程序
 * 支持从文件或字符串加载着色器源码
 */

#include "FluxPlayer/renderer/Shader.h"
#include "FluxPlayer/utils/Logger.h"
#include <fstream>
#include <sstream>

namespace FluxPlayer {

Shader::Shader()
    : m_program(0)
    , m_vertexShader(0)
    , m_fragmentShader(0) {
    LOG_DEBUG("Shader constructor called");
}

Shader::~Shader() {
    LOG_DEBUG("Shader destructor called");
    if (m_program) {
        glDeleteProgram(m_program);
    }
    if (m_vertexShader) {
        glDeleteShader(m_vertexShader);
    }
    if (m_fragmentShader) {
        glDeleteShader(m_fragmentShader);
    }
}

/**
 * 从文件加载着色器
 * @param vertexPath 顶点着色器文件路径
 * @param fragmentPath 片段着色器文件路径
 * @return 成功返回 true，失败返回 false
 */
bool Shader::loadFromFile(const std::string& vertexPath, const std::string& fragmentPath) {
    LOG_INFO("Loading shaders from files");
    LOG_DEBUG("Vertex shader: " + vertexPath);
    LOG_DEBUG("Fragment shader: " + fragmentPath);

    // 读取着色器源码文件
    std::string vertexSource = readFile(vertexPath);
    std::string fragmentSource = readFile(fragmentPath);

    if (vertexSource.empty() || fragmentSource.empty()) {
        LOG_ERROR("Failed to read shader files (empty content)");
        return false;
    }

    LOG_DEBUG("Shader files read successfully");
    return loadFromSource(vertexSource, fragmentSource);
}

/**
 * 从源码字符串加载着色器
 * @param vertexSource 顶点着色器源码
 * @param fragmentSource 片段着色器源码
 * @return 成功返回 true，失败返回 false
 */
bool Shader::loadFromSource(const std::string& vertexSource, const std::string& fragmentSource) {
    LOG_DEBUG("Compiling shaders from source");

    // 步骤1：创建着色器对象
    m_vertexShader = glCreateShader(GL_VERTEX_SHADER);
    m_fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);

    // 步骤2：编译顶点着色器
    if (!compileShader(m_vertexShader, vertexSource)) {
        LOG_ERROR("Failed to compile vertex shader");
        return false;
    }
    LOG_DEBUG("Vertex shader compiled successfully");

    // 步骤3：编译片段着色器
    if (!compileShader(m_fragmentShader, fragmentSource)) {
        LOG_ERROR("Failed to compile fragment shader");
        return false;
    }
    LOG_DEBUG("Fragment shader compiled successfully");

    // 步骤4：链接着色器程序
    if (!linkProgram()) {
        LOG_ERROR("Failed to link shader program");
        return false;
    }

    LOG_INFO("Shader program created and linked successfully");
    return true;
}

void Shader::use() const {
    if (m_program) {
        glUseProgram(m_program);
    }
}

void Shader::unuse() const {
    glUseProgram(0);
}

void Shader::setInt(const std::string& name, int value) const {
    glUniform1i(glGetUniformLocation(m_program, name.c_str()), value);
}

void Shader::setFloat(const std::string& name, float value) const {
    glUniform1f(glGetUniformLocation(m_program, name.c_str()), value);
}

void Shader::setVec2(const std::string& name, float x, float y) const {
    glUniform2f(glGetUniformLocation(m_program, name.c_str()), x, y);
}

void Shader::setVec3(const std::string& name, float x, float y, float z) const {
    glUniform3f(glGetUniformLocation(m_program, name.c_str()), x, y, z);
}

void Shader::setVec4(const std::string& name, float x, float y, float z, float w) const {
    glUniform4f(glGetUniformLocation(m_program, name.c_str()), x, y, z, w);
}

void Shader::setMat4(const std::string& name, const float* value) const {
    glUniformMatrix4fv(glGetUniformLocation(m_program, name.c_str()), 1, GL_FALSE, value);
}

/**
 * 编译单个着色器
 * @param shader 着色器对象 ID
 * @param source 着色器源码
 * @return 成功返回 true，失败返回 false
 */
bool Shader::compileShader(GLuint shader, const std::string& source) {
    // 设置着色器源码并编译
    const char* sourceCStr = source.c_str();
    glShaderSource(shader, 1, &sourceCStr, nullptr);
    glCompileShader(shader);

    // 检查编译状态
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        // 编译失败，获取错误日志
        GLchar infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LOG_ERROR("Shader compilation error:");
        LOG_ERROR(std::string(infoLog));
        return false;
    }

    return true;
}

/**
 * 链接着色器程序
 * @return 成功返回 true，失败返回 false
 *
 * 链接过程：
 * 1. 创建程序对象
 * 2. 附加顶点和片段着色器
 * 3. 链接程序
 * 4. 验证链接状态
 * 5. 删除独立的着色器对象（已链接到程序中）
 */
bool Shader::linkProgram() {
    LOG_DEBUG("Linking shader program");

    // 创建着色器程序对象
    m_program = glCreateProgram();

    // 附加已编译的着色器
    glAttachShader(m_program, m_vertexShader);
    glAttachShader(m_program, m_fragmentShader);

    // 链接程序
    glLinkProgram(m_program);

    // 检查链接状态
    GLint success;
    glGetProgramiv(m_program, GL_LINK_STATUS, &success);
    if (!success) {
        // 链接失败，获取错误日志
        GLchar infoLog[512];
        glGetProgramInfoLog(m_program, 512, nullptr, infoLog);
        LOG_ERROR("Shader linking error:");
        LOG_ERROR(std::string(infoLog));
        return false;
    }

    // 链接后可以删除独立的着色器对象
    // 它们已经被链接到程序中，不再需要
    LOG_DEBUG("Deleting individual shader objects (already linked)");
    glDeleteShader(m_vertexShader);
    glDeleteShader(m_fragmentShader);
    m_vertexShader = 0;
    m_fragmentShader = 0;

    return true;
}

/**
 * 从文件读取文本内容
 * @param filepath 文件路径
 * @return 文件内容字符串，失败返回空字符串
 */
std::string Shader::readFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open shader file: " + filepath);
        return "";
    }

    // 使用 stringstream 读取整个文件
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace FluxPlayer
