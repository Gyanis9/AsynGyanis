/**
 * @file NetworkInterface.h
 * @brief 网卡接口名与接口索引的换算，IPv6 作用域标识要用
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#if ASYN_PLATFORM_WIN32
// 接口索引换算在 Windows SDK 里由 netioapi.h 声明，可那个头文件按 __IPHLPAPI_H__ 有没有定义来分辨
// 用户态与内核态分支：单独包含它就走内核态分支，NETIO_STATUS 展开成没有声明过的 NTSTATUS，
// 整个头一页都编不过（实测 100 条 C4430/C2086）。因此必须经由 iphlpapi.h 带进来，库是 iphlpapi。
// Linux 侧这两个换算在 libc 的 <net/if.h>。
#include <iphlpapi.h>
#else
#include <net/if.h>
#endif

#include <string_view>

namespace AsynGyanis::Platform
{
    /**
     * @brief 按网卡名问它的接口索引
     * @param interfaceName 接口名（Linux 形如 "eth0"，Windows 形如 "Ethernet"），原样交给底层换算
     * @return unsigned 接口索引，从 1 起算；名字在本机不存在、文本含 NUL、或换算调用失败时为 0
     * @note IPv6 的「%接口名」写法要靠它换成内核认的数字作用域号（RFC 4007 §11）；本层只回答
     *       「有没有拿到一个可用索引」，失败原因（查无此接口 / 系统调用出错）不在这里分岔，
     *       调用方要的就是这个二值判定
     */
    [[nodiscard]] unsigned interfaceIndexOfName(std::string_view interfaceName) noexcept;
} // namespace AsynGyanis::Platform
