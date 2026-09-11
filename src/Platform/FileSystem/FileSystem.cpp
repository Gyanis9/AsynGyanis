/**
 * @file FileSystem.cpp
 * @brief 文件系统路径的跨平台构造
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/FileSystem/FileSystem.h"

#include "Platform/Platform.h"
#include "Platform/System/TextEncoding.h"

namespace AsynGyanis::Platform
{
    std::filesystem::path FileSystem::pathFromUtf8(const std::string &utf8Path)
    {
#if ASYN_PLATFORM_WIN32
        // 先转 UTF-16 再构造，绕开 ANSI 代码页对非 ASCII 字符的误解码
        return std::filesystem::path{TextEncoding::toWideString(utf8Path)};
#else
        return std::filesystem::path{utf8Path};
#endif
    }
} // namespace AsynGyanis::Platform
