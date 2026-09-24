/**
 * @file SessionTicketKeyRing.cpp
 * @brief 会话票据密钥环的实现：密钥文件的读取校验，与装在 SSL_CTX 上的签发/解票回调
 * @author Gyanis
 * @date 2026-09-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/Tls/SessionTicketKeyRing.h"

#include "Core/Exception/CoreException.h"

#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 票据密钥的两种合法长度（字节）：48 = 名 16 + HMAC 16 + AES-128 密钥 16；80 = 名 16 + HMAC 32 + AES-256 密钥 32
        constexpr std::size_t kTicketKeyLengthAes128 = 48;
        constexpr std::size_t kTicketKeyLengthAes256 = 80;

        /// 票据里标识「用哪份密钥」的名字段长度，与 OpenSSL 的 TLSEXT_KEYNAME_LENGTH 同值
        constexpr std::size_t kTicketKeyNameLength = 16;

        /// AES-256 布局里 HMAC 段与 AES 段各自的长度（80 字节减去名字段后两等分）
        constexpr std::size_t kTicketAes256SegmentLength = 32;

        /**
         * @brief 判断一份密钥字节的长度是否可用（长度决定 HMAC 段与 AES 段的切分，错一位就会读越界）
         * @param length 密钥字节数
         * @return bool true 表示是 48 或 80 这两种合法布局之一
         */
        bool isUsableTicketKeyLength(const std::size_t length) noexcept
        {
            return length == kTicketKeyLengthAes128 || length == kTicketKeyLengthAes256;
        }

        /**
         * @brief 整个文件读成字节串
         * @param filePath 文件路径（二进制读取）
         * @param output 出参，文件内容
         * @return true 读取成功且文件非空
         */
        bool readFileBytes(const std::string &filePath, std::string &output)
        {
            std::ifstream stream(filePath, std::ios::in | std::ios::binary);
            if (!stream)
            {
                return false;
            }

            output.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            return !output.empty();
        }

        /**
         * @brief 挂在 SSL_CTX 上的密钥环持有者
         * @details 用 ex_data 而不是上层对象的成员存放：解票回调跑在握手线程上，解引用一个可能已析构的
         *          持有者会悬垂，而这块的生死跟随 SSL_CTX 本身（注册时给的释放回调负责 delete）。
         *          密钥用原子 shared_ptr 快照，使运行中替换整份密钥环是原子的。
         */
        struct StoredKeyRing
        {
            std::atomic<std::shared_ptr<const std::vector<std::string>>> keys{std::shared_ptr<const std::vector<std::string>>{}};
        };

        /**
         * @brief 取密钥环的 ex_data 下标（首次调用时注册，释放回调负责 delete 持有者）
         * @return int 下标；注册失败返回 -1，调用方据此跳过票据密钥能力
         */
        int keyRingExDataIndex()
        {
            static const int index = SSL_CTX_get_ex_new_index(
                    0, nullptr, nullptr, nullptr,
                    [](void *, void *pointer, CRYPTO_EX_DATA *, int, long, void *)
                    {
                        delete static_cast<StoredKeyRing *>(pointer);
                    });
            return index;
        }

        /**
         * @brief 会话票据密钥回调：签发用环里首份密钥，解开按票据带来的密钥名在环里找
         * @param ssl 当前握手对象
         * @param keyName 密钥名缓冲（16 字节）：签发时**必须由本回调写入**，解开时是票据里带来的
         * @param iv 初始化向量：签发时 OpenSSL 已填好随机值，解开时来自票据
         * @param cipherContext 票据正文的加解密上下文
         * @param macContext 票据校验码的 MAC 上下文
         * @param isEncrypting 非 0 表示签发新票据，0 表示解开对端带来的票据
         * @return int 1 成功；2 解开成功但命中的是轮换前的旧密钥（OpenSSL 据此顺手换发一张新票据）；
         *         0 婉拒——本次不签发、或不认这张票据，握手退回全量；**任何分支都不返回 -1**
         * @warning -1 在 OpenSSL 3.x 的这个回调里是「致命错误、中止握手」，不是「婉拒」
         *          （实测 3.6.2：客户端带来一张本服务不认的票据，整条握手以
         *          `SSL routines::internal error` 收场）。婉拒要用 0——nginx 的同一回调在
         *          「密钥名对不上」时也返回 0。票据只是加速手段，密钥对不上不该让连接建不起来
         * @details 签发时把密钥名写进 keyName 是**必需的一步**：票据里的名字段取自这块缓冲，
         *          不写就是 OpenSSL 自己生成的随机值，下次解密时环里任何一份都对不上（实测过的症状是
         *          「密钥明明装了、恢复永远不命中」，而且一个错都不报）。环里一份密钥都没有时同样回 0，
         *          而不是让 OpenSSL 退回它自己那份随机密钥——回调一旦装上，内部密钥那条路就不再生效
         */
        int selectSessionTicketKey(SSL *ssl, unsigned char *keyName, unsigned char *iv, EVP_CIPHER_CTX *cipherContext,
                                   EVP_MAC_CTX *macContext, int isEncrypting)
        {
            SSL_CTX *context = SSL_get_SSL_CTX(ssl);
            if (context == nullptr)
            {
                return 0;
            }

            const auto *ring = static_cast<const StoredKeyRing *>(SSL_CTX_get_ex_data(context, keyRingExDataIndex()));
            if (ring == nullptr)
            {
                return 0;
            }

            const std::shared_ptr<const std::vector<std::string>> keys = ring->keys.load(std::memory_order_acquire);
            if (!keys || keys->empty())
            {
                return 0;
            }

            // 校验码算法与 OpenSSL 内部一致取 SHA256；参数数组要的是可写的名字缓冲，不能直接给字面量
            char macDigestName[] = "SHA256";
            const OSSL_PARAM macParameters[] = {
                OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, macDigestName, 0),
                OSSL_PARAM_construct_end()};

            // 签发只用首份——轮换的语义就是「新的签、旧的解」；解开则按名字在环里逐份比对
            std::size_t chosenIndex = 0;
            if (isEncrypting == 0)
            {
                chosenIndex = keys->size(); // 越界值兼作「没找到」的哨兵
                for (std::size_t index = 0; index < keys->size(); ++index)
                {
                    if (std::memcmp(keyName, (*keys)[index].data(), kTicketKeyNameLength) == 0)
                    {
                        chosenIndex = index;
                        break;
                    }
                }

                if (chosenIndex == keys->size())
                {
                    // 名字对不上环里任何一份：这张票据不是本服务签的，或签它的那份密钥已被摘掉
                    return 0;
                }
            }

            const std::string &key = (*keys)[chosenIndex];
            // 段切分完全由长度决定，因此在做偏移算术之前再确认一次长度合法：装载与换代两条入口都校验过，
            // 这里挡的是「将来新增第三条入口忘了校验」——错一位就是读越界的密钥材料
            if (!isUsableTicketKeyLength(key.size()))
            {
                return 0;
            }

            // 名字段之后 HMAC 段与 AES 段等长，两种合法长度都按这一条切
            const std::size_t cipherKeyLength = (key.size() - kTicketKeyNameLength) / 2;
            const auto       *hmacKey         = reinterpret_cast<const unsigned char *>(key.data()) + kTicketKeyNameLength;
            const auto       *aesKey          = hmacKey + cipherKeyLength;
            const EVP_CIPHER *cipher          = cipherKeyLength == kTicketAes256SegmentLength ? EVP_aes_256_cbc() : EVP_aes_128_cbc();

            if (isEncrypting != 0)
            {
                // 票据里的名字段就是这块缓冲的内容，写进去下次才认得出来
                std::memcpy(keyName, key.data(), kTicketKeyNameLength);
                if (EVP_EncryptInit_ex(cipherContext, cipher, nullptr, aesKey, iv) != 1)
                {
                    return 0;
                }
            } else if (EVP_DecryptInit_ex(cipherContext, cipher, nullptr, aesKey, iv) != 1)
            {
                return 0;
            }

            if (EVP_MAC_init(macContext, hmacKey, cipherKeyLength, macParameters) != 1)
            {
                return 0;
            }

            // 命中的是首份（当前密钥）就照常收下；命中轮换前的旧密钥则回 2，让对端拿到一张新票据
            return (isEncrypting != 0 || chosenIndex == 0) ? 1 : 2;
        }
    } // namespace

    void SessionTicketKeyRing::readKeyFiles(const std::vector<std::string> &keyFiles, std::vector<std::string> &keys)
    {
        if (keyFiles.empty())
        {
            // 空列表不是「取消共享」而是「一张票据都不发」：回调一旦装上，OpenSSL 的内部随机密钥
            // 那条路就不再生效了。这种配置错误必须当场告状，而不是让恢复命中率悄悄归零
            throw CoreException("装载会话票据密钥失败：密钥文件列表为空；不需要共享票据密钥就不要装载");
        }

        keys.clear();
        keys.reserve(keyFiles.size());

        for (const std::string &keyFile: keyFiles)
        {
            std::string keyBytes;
            if (!readFileBytes(keyFile, keyBytes))
            {
                throw CoreException("装载会话票据密钥失败：读不出内容，文件不可读或为空——" + keyFile);
            }
            if (!isUsableTicketKeyLength(keyBytes.size()))
            {
                // 长度决定 HMAC 段与 AES 段怎么切，写错就是配置错误：48 走 AES-128、80 走 AES-256
                throw CoreException("装载会话票据密钥失败：" + keyFile + " 是 " + std::to_string(keyBytes.size()) +
                                    " 字节，只接受 48（AES-128）或 80（AES-256）字节的二进制密钥"
                                    "（openssl rand 48 > ticket.key 即可产出）");
            }
            keys.push_back(std::move(keyBytes));
        }
    }

    void SessionTicketKeyRing::install(SSL_CTX *context, std::vector<std::string> keys)
    {
        const int index = keyRingExDataIndex();
        if (index < 0)
        {
            return;
        }

        auto *ring = static_cast<StoredKeyRing *>(SSL_CTX_get_ex_data(context, index));
        if (ring == nullptr)
        {
            ring = new StoredKeyRing();
            if (SSL_CTX_set_ex_data(context, index, ring) != 1)
            {
                delete ring;
                return;
            }

            // 回调只在首次装上密钥环时挂一次：没装过密钥的上下文保持 OpenSSL 默认行为
            // （每个 SSL_CTX 自己随机生成一份密钥），与本类被调用之前完全一致
            SSL_CTX_set_tlsext_ticket_key_evp_cb(context, selectSessionTicketKey);
        }
        ring->keys.store(std::make_shared<const std::vector<std::string>>(std::move(keys)), std::memory_order_release);
    }
} // namespace AsynGyanis::Core
