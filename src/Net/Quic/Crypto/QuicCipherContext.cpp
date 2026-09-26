#include "Net/Quic/Crypto/QuicCipherContext.h"

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 一条线程一份上下文，线程退出时统一释放
         * @details 这份持有不属于任何一条连接，也不属于任何一次运算：连接的建立与收口都不该
         *          碰到它，因此这里不做「用完还回去」的归还协议。
         */
        class ThreadCipherContext
        {
        public:
            ThreadCipherContext() = default;

            ThreadCipherContext(const ThreadCipherContext &)            = delete;
            ThreadCipherContext &operator=(const ThreadCipherContext &) = delete;

            ~ThreadCipherContext()
            {
                if (m_context != nullptr)
                {
                    EVP_CIPHER_CTX_free(m_context);
                }
            }

            /**
             * @brief 取上下文，取不到就地再建一次
             * @details 创建失败不是终态：下一次取用还要再试，否则一次偶然的内存紧张会让这条线程
             *          从此再也发不出包
             * @return EVP_CIPHER_CTX * 上下文；仍建不起来时为 nullptr
             */
            [[nodiscard]] EVP_CIPHER_CTX *get() noexcept
            {
                if (m_context == nullptr)
                {
                    m_context = EVP_CIPHER_CTX_new();
                }
                return m_context;
            }

        private:
            EVP_CIPHER_CTX *m_context{nullptr}; ///< 本线程那份上下文，非拥有语义由本类独占
        };
    } // namespace

    EVP_CIPHER_CTX *acquireQuicCipherContext() noexcept
    {
        thread_local ThreadCipherContext threadContext;
        EVP_CIPHER_CTX *const            context = threadContext.get();
        if (context == nullptr)
        {
            return nullptr;
        }
        // 上一次可能停在半程（AEAD 标签不合时直接返回 false），也可能是换了密码套件：
        // 取用即重置，调用方交完密钥与 IV 之后就是全新的运算
        static_cast<void>(EVP_CIPHER_CTX_reset(context));
        return context;
    }
} // namespace AsynGyanis::Net
