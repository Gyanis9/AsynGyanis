/**
 * @file TextEncoding.h
 * @brief UTF-8 与 UTF-16 字符串互转，供 Windows 宽字符 API 边界使用
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <string>

namespace AsynGyanis::Platform
{
    /**
     * @brief 文本编码转换工具
     *
     * @details 项目内部统一使用 UTF-8 的 std::string 与 std::filesystem::path，
     *          而 Windows 的 W 系列 API 要求 UTF-16，因此转换集中在本类中，
     *          避免各处手写 MultiByteToWideChar / WideCharToMultiByte。
     */
    class TextEncoding
    {
    public:
        /**
         * @brief 将 UTF-8 字符串转换为 UTF-16 宽字符串
         * @param utf8Text UTF-8 编码文本
         * @return std::wstring 转换结果，输入为空或转换失败时返回空串
         */
        static std::wstring toWideString(const std::string &utf8Text);

        /**
         * @brief 将 UTF-16 宽字符串转换为 UTF-8 字符串
         * @param wideText UTF-16 编码文本
         * @return std::string 转换结果，输入为空或转换失败时返回空串
         */
        static std::string toUtf8String(const std::wstring &wideText);
    };
} // namespace AsynGyanis::Platform
