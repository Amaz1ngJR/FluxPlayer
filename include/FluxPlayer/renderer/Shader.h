/**
 * @file Shader.h
 * @brief OpenGL 着色器程序管理，支持从文件或源码加载 GLSL 着色器
 */

#pragma once

#include <string>

namespace FluxPlayer {

/**
 * @brief GLSL 着色器程序封装类
 *
 * 管理顶点着色器和片段着色器的编译、链接，以及 uniform 变量的设置。
 * 支持从文件路径或源码字符串两种方式加载着色器。
 */
class Shader {
public:
    Shader();
    /** @brief 析构函数，释放着色器程序和着色器对象 */
    ~Shader();

    /**
     * @brief 从文件加载并编译着色器
     * @param vertexPath   顶点着色器文件路径
     * @param fragmentPath 片段着色器文件路径
     * @return 成功返回 true，失败返回 false
     */
    bool loadFromFile(const std::string& vertexPath, const std::string& fragmentPath);

    /**
     * @brief 从源码字符串加载并编译着色器
     * @param vertexSource   顶点着色器 GLSL 源码
     * @param fragmentSource 片段着色器 GLSL 源码
     * @return 成功返回 true，失败返回 false
     */
    bool loadFromSource(const std::string& vertexSource, const std::string& fragmentSource);

    /** @brief 激活此着色器程序（glUseProgram） */
    void use() const;

    /** @brief 取消激活着色器程序（glUseProgram(0)） */
    void unuse() const;

    /**
     * @brief 获取底层 OpenGL 着色器程序 ID
     * @return 着色器程序 ID
     */
    unsigned int getProgram() const { return m_program; }

    // ==================== Uniform 设置函数 ====================

    /**
     * @brief 设置 int 类型 uniform 变量（常用于纹理采样器单元编号）
     * @param name  uniform 变量名
     * @param value 整数值
     */
    void setInt(const std::string& name, int value) const;

    /**
     * @brief 设置 float 类型 uniform 变量
     * @param name  uniform 变量名
     * @param value 浮点值
     */
    void setFloat(const std::string& name, float value) const;

    /**
     * @brief 设置 vec2 类型 uniform 变量
     * @param name uniform 变量名
     * @param x, y 二维向量分量
     */
    void setVec2(const std::string& name, float x, float y) const;

    /**
     * @brief 设置 vec3 类型 uniform 变量
     * @param name    uniform 变量名
     * @param x, y, z 三维向量分量
     */
    void setVec3(const std::string& name, float x, float y, float z) const;

    /**
     * @brief 设置 vec4 类型 uniform 变量
     * @param name       uniform 变量名
     * @param x, y, z, w 四维向量分量
     */
    void setVec4(const std::string& name, float x, float y, float z, float w) const;

    /**
     * @brief 设置 mat4 类型 uniform 变量
     * @param name  uniform 变量名
     * @param value 指向 16 个 float 的指针（列主序 4x4 矩阵）
     */
    void setMat4(const std::string& name, const float* value) const;

private:
    /**
     * @brief 编译单个着色器对象
     * @param shader 已创建的着色器对象 ID
     * @param source GLSL 源码
     * @return 编译成功返回 true，失败返回 false 并输出错误日志
     */
    bool compileShader(unsigned int shader, const std::string& source);

    /**
     * @brief 链接着色器程序，将顶点和片段着色器组合为可执行程序
     * @return 链接成功返回 true，失败返回 false 并输出错误日志
     */
    bool linkProgram();

    /**
     * @brief 读取文件全部内容为字符串
     * @param filepath 文件路径
     * @return 文件内容，失败返回空字符串
     */
    std::string readFile(const std::string& filepath);

    unsigned int m_program;             ///< 链接后的着色器程序 ID
    unsigned int m_vertexShader;        ///< 顶点着色器对象 ID（链接后被删除，置为 0）
    unsigned int m_fragmentShader;      ///< 片段着色器对象 ID（链接后被删除，置为 0）
};

} // namespace FluxPlayer
