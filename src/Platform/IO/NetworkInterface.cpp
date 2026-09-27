#include "Platform/IO/NetworkInterface.h"

#include <string>

namespace AsynGyanis::Platform
{
    unsigned interfaceIndexOfName(const std::string_view interfaceName) noexcept
    {
        // 底层按零终止 C 字符串取名：内嵌 NUL 会让 "lo\0eth0" 被当成 "lo" 去查，于是问到的索引
        // 并不是调用方写的那块网卡，因此在进入 C API 之前就拒掉
        if (interfaceName.find('\0') != std::string_view::npos)
        {
            return 0;
        }

        const std::string name(interfaceName);
        return static_cast<unsigned>(if_nametoindex(name.c_str()));
    }
} // namespace AsynGyanis::Platform
