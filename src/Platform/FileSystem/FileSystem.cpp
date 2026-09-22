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

    std::string FileSystem::utf8FromPath(const std::filesystem::path &path)
    {
#if ASYN_PLATFORM_WIN32
        // 与 pathFromUtf8 成对：窄串刻度要过本地代码页，代码页外的字符直接抛出，
        // 而路径文本多数只出现在日志与报错文案里——一条诊断不该毁掉整次装配
        return TextEncoding::toUtf8String(path.native());
#else
        // Linux 的原生路径本身就是 UTF-8 字节序列，native() 已是窄串，无需转码
        return path.native();
#endif
    }
} // namespace AsynGyanis::Platform
