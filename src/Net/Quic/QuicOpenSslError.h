/**
 * @file QuicOpenSslError.h
 * @brief 取 OpenSSL 错误队列的文本，让 QUIC 侧的失败文案带上底层原因
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 服务端启动、密钥导出与包保护都要在失败时回答「OpenSSL 为什么拒」。三处曾各写一份
 *          只取首条的抄本，这里收成一份并把整个队列排空。
 */

#pragma once

#include <openssl/err.h>

#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 把当前线程 OpenSSL 错误队列里剩下的条目拼成一行
     * @details 一次失败常常连带多条：只取首条会把余下的留在队列里，下一次调用又把**上一次的**
     *          错误当成新原因报出来，定位时会被带偏。
     * @return std::string 各条目以 "; " 连接；队列为空时给出「未给出错误详情」的固定说明
     */
    [[nodiscard]] inline std::string quicOpenSslErrorText()
    {
        std::string text;
        for (unsigned long errorCode = ERR_get_error(); errorCode != 0; errorCode = ERR_get_error())
        {
            char buffer[256]{};
            ERR_error_string_n(errorCode, buffer, sizeof(buffer));
            if (!text.empty())
            {
                text += "; ";
            }
            text += buffer;
        }
        return text.empty() ? "OpenSSL 未给出错误详情" : text;
    }
} // namespace AsynGyanis::Net
