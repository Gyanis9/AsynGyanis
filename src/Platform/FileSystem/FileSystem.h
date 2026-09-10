/**
 * @file FileSystem.h
 * @brief 文件系统路径的跨平台构造
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <filesystem>
#include <string>

namespace AsynGyanis::Platform
{
    /**
     * @brief 文件系统路径工具
     *
     * @details Windows 的窄字符路径接口按当前 ANSI 代码页解释字节，含中文的
     *          UTF-8 路径会被解析成乱码目录；Linux 的原生路径本身就是字节序列。
     *          std::filesystem::u8path 已在 C++20 中标记弃用，故统一走本类。
     */
    class FileSystem
    {
    public:
        /**
         * @brief 由 UTF-8 字符串构造路径对象
         * @param utf8Path UTF-8 编码的文件或目录路径
         * @return std::filesystem::path 可安全用于后续文件系统操作的路径
         */
        static std::filesystem::path pathFromUtf8(const std::string &utf8Path);
    };
} // namespace AsynGyanis::Platform
