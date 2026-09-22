/**
 * @file FileSystem.h
 * @brief 文件系统路径的跨平台构造
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

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

        /**
         * @brief 把路径对象转成 UTF-8 文本，用于写日志与报错文案
         * @details 与 `path::string()` 的区别：后者按本地代码页转换，代码页外的字符在 Windows 上
         *          直接抛出（"No mapping for the Unicode character exists..."）。把路径打进诊断
         *          文本是常见写法，一条日志文案因此毁掉整次装配、一次报错因此二次抛出都不该发生。
         * @param path 待转换的路径
         * @return std::string UTF-8 编码的路径文本；无法编码的码位按替换字符处理，不抛异常
         */
        [[nodiscard]] static std::string utf8FromPath(const std::filesystem::path &path);
    };
} // namespace AsynGyanis::Platform
