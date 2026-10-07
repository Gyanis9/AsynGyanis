#include "Net/Quic/QuicOpenSslError.h"

#include <openssl/err.h>

namespace AsynGyanis::Net
{
    std::string quicOpenSslErrorText()
    {
        std::string text;
        // 一路取到队列空为止：留下未取走的条目，下一次调用就会把上一轮的原因当成新失败报出去
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
        // 队列为空不等于「没有失败」：调用方拿到的可能是被别处排空过的队列，如实说明比给空串好读
        return text.empty() ? "OpenSSL 未给出错误详情" : text;
    }
} // namespace AsynGyanis::Net
