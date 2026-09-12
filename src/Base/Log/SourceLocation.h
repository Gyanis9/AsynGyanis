/**
 * @file SourceLocation.h
 * @brief 轻量级源码位置信息载体
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <source_location>
#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief 轻量级源码位置信息（可 constexpr 构造）
     *
     * @details 包装 std::source_location 的文件名/行号/函数名三项，仅保存指针而非
     *          标准对象，便于放入 LogEvent 并在 Release 构建下退化为全空位置。
     */
    struct SourceLocation
    {
        const char *fileName     = nullptr; ///< 源文件名
        int         line         = 0;       ///< 行号
        const char *functionName = nullptr; ///< 函数名

        /**
         * @brief 默认构造，所有字段置空
         */
        constexpr SourceLocation() = default;

        /**
         * @brief 使用显式字段构造源码位置
         * @param file 源文件名
         * @param lineNumber 行号
         * @param function 函数名
         */
        constexpr SourceLocation(const char *file, const int lineNumber, const char *function) :
            fileName(file)
            , line(lineNumber)
            , functionName(function)
        {
        }

        /**
         * @brief 从 std::source_location 构造
         * @param location 标准源码位置对象
         */
        constexpr explicit SourceLocation(const std::source_location &location) :
            fileName(location.file_name())
            , line(static_cast<int>(location.line()))
            , functionName(location.function_name())
        {
        }

        /**
         * @brief 获取当前调用点的源码位置
         * @param location 由编译器填充的标准源码位置
         * @return SourceLocation 当前源码位置
         */
        [[nodiscard]] static constexpr SourceLocation current(
                const std::source_location &location = std::source_location::current())
        {
            return SourceLocation(location);
        }

        /**
         * @brief 仅返回文件名部分（去掉目录前缀）
         * @details 同时识别 '/' 与 '\\' 两种分隔符，因此 Windows 与 Linux 的
         *          编译器路径都能正确截断。标注 constexpr 后，只要文件名在编译期已知，
         *          截断结果就能在编译期算完（source_location 的文件名对每次调用点都是常量）；
         *          实现在 string_view 上反向查找最后一个分隔符，短文件名的场景远比逐字符正向扫描快。
         * @return const char* 短文件名，位置为空时返回空字符串
         */
        [[nodiscard]] constexpr const char *shortFileName() const noexcept
        {
            if (!fileName)
            {
                return "";
            }

            const std::string_view path{fileName};
            const std::size_t      separatorPosition = path.find_last_of("/\\");
            return separatorPosition == std::string_view::npos ? fileName : fileName + separatorPosition + 1;
        }
    };
} // namespace AsynGyanis::Base
