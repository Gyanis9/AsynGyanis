/**
 * @file InetAddress.h
 * @brief Re-export Core::InetAddress into Net namespace
 * @copyright Copyright (c) 2026
 */
#ifndef NET_INETADDRESS_H
#define NET_INETADDRESS_H

#include "Core/InetAddress.h"

namespace Net
{
    using InetAddress = Core::InetAddress;
}

#endif
