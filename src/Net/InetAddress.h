/**
 * @file InetAddress.h
 * @brief 将 Core::InetAddress 重新导出到 Net 命名空间
 * @copyright Copyright (c) 2026
 */
#ifndef NET_INETADDRESS_H
#define NET_INETADDRESS_H

#include "Core/InetAddress.h"

namespace Net
{
    /**
     * @brief 网络地址类型，直接使用 Core 命名空间中的 InetAddress。
     *
     * 该类型表示 IP 地址和端口，用于网络连接和监听。
     * 通过类型别名的方式将 Core::InetAddress 引入 Net 命名空间，
     * 使得网络层代码无需直接依赖 Core 的命名前缀。
     */
    using InetAddress = Core::InetAddress;
}

#endif
