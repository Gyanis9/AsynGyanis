/**
 * @file ExceptionMessage.h
 * @brief 异常消息的统一文本生成 —— 供异常体系的两个分支共用
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 异常文本格式（"[异常] 消息 [文件:行 in 函数]"）是两条继承链共用的契约，
 *          而上收成自由函数是唯一的共享手段：Exception 走 std::runtime_error、
 *          LogicException 走 std::logic_error，二者无法共享基类。
 * @note 仅模块内部可见（Detail 命名空间），不属于对外接口。
 */
#pragma once

#include <format>
#include <source_location>
#include <string>
#include <string_view>

namespace AsynGyanis::Base::Detail
{
    /**
     * @brief 取路径的末段（同时认 '/' 与 '\\'）
     * @param path 路径文本
     * @return std::string_view 最后一个分隔符之后的部分；没有分隔符时原样返回
     * @details 不借 Base/Log/SourceLocation.h 的 shortFileName()：那个函数挂在日志侧的值类型上，
     *          异常层为省一份字符串去咬日志层的头，会让两条继承链多一条与消息无关的依赖
     */
    [[nodiscard]] inline std::string_view lastPathSegment(std::string_view path) noexcept
    {
        const std::size_t separatorPosition = path.find_last_of("/\\");
        return separatorPosition == std::string_view::npos ? path : path.substr(separatorPosition + 1);
    }

    /**
     * @brief 把异常消息与抛出位置拼成统一格式的文本
     * @param message 异常描述消息
     * @param sourceLocation 异常抛出位置
     * @return std::string 形如 "[异常] 消息 [文件:行 in 函数]" 的完整文本
     * @details 文件只取末段：`source_location::file_name()` 给的是编译期展开的**绝对路径**，
     *          而 what() 是一个会被到处抄的字符串——日志、错误页、以及消费方直接回给对端的应答
     *          （框架自己的约定是「what() 带抛出点、不外回」，但那挡不住下游按自己的判断外回）。
     *          构建目录的绝对路径本身就是信息泄露，还让同一份日志在不同机器上对不上。
     *          完整路径没有丢：它留在 location() 里，结构化读的口不受影响
     */
    [[nodiscard]] inline std::string formatExceptionMessage(const std::string &message, const std::source_location &sourceLocation)
    {
        return std::format("[异常] {} [{}:{} in {}]", message, lastPathSegment(sourceLocation.file_name()), sourceLocation.line(), sourceLocation.function_name());
    }
} // namespace AsynGyanis::Base::Detail
