#include "Net/Quic/Crypto/QuicPacketProtection.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"
#include "Net/Quic/Crypto/QuicCipherContext.h"
#include "Net/Quic/QuicOpenSslError.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace AsynGyanis::Net
{
    namespace
    {
        /// EVP_CipherFinal_ex 的收尾输出缓冲：GCM 与 ChaCha20-Poly1305 在这里不吐字节，但接口要一个可写地址
        inline constexpr std::size_t kCipherTrailerByteLength = 32;

        /**
         * @brief 取套件对应的 AEAD 密码
         * @param cipherSuite 密码套件
         * @return const EVP_CIPHER * 未定义取值时返回空指针
         */
        const EVP_CIPHER *cipherFor(const QuicCipherSuite cipherSuite) noexcept
        {
            switch (cipherSuite)
            {
            case QuicCipherSuite::Aes128Gcm: return EVP_aes_128_gcm();
            case QuicCipherSuite::Aes256Gcm: return EVP_aes_256_gcm();
            case QuicCipherSuite::ChaCha20Poly1305: return EVP_chacha20_poly1305();
            }
            return nullptr;
        }

        /**
         * @brief 按 RFC 9001 §5.3 拼出 AEAD 的 IV
         * @details 取「12 字节 IV XOR 包号的大端表示（左补零到 12 字节）」。同一个包号只加密一次，
         *          块计数器恒为 0，因此不必显式拼它。
         * @param keys 密钥组
         * @param packetNumber 完整包号
         * @return std::array<std::uint8_t, kQuicInitializationVectorByteLength> 拼好的 IV
         */
        std::array<std::uint8_t, kQuicInitializationVectorByteLength> buildInitializationVector(const QuicPacketKeys &keys,
                                                                                                   const std::uint64_t packetNumber) noexcept
        {
            std::array<std::uint8_t, kQuicInitializationVectorByteLength> initializationVector{};
            const std::span<const std::uint8_t> baseInitializationVector = keys.initializationVectorBytes();
            for (std::size_t byteIndex = 0; byteIndex < baseInitializationVector.size(); ++byteIndex)
            {
                const std::size_t shiftBitCount = (baseInitializationVector.size() - 1 - byteIndex) * 8;
                // 包号最多 62 位，最高那几个字节位上恒为 0；移位量到 64 以上是未定义行为，必须跳过而不是靠运气
                const std::uint8_t packetNumberByte = shiftBitCount >= 64
                        ? 0
                        : static_cast<std::uint8_t>((packetNumber >> shiftBitCount) & 0xFFULL);
                initializationVector[byteIndex] = static_cast<std::uint8_t>(baseInitializationVector[byteIndex] ^ packetNumberByte);
            }
            return initializationVector;
        }

        /**
         * @brief 跑一次 AEAD
         * @param keys 密钥组
         * @param packetNumber 完整包号，用于拼 IV
         * @param additionalData 未加密头部
         * @param input 输入字节（明文或密文）
         * @param destination 输出字节，长度必须与 input 相同
         * @param tag 标签缓冲区：加密时写入，解密时读出
         * @param isEncryption true 加密、false 解密
         * @return true 成功（解密时表示标签校验通过）
         * @return false 仅解密：标签不合
         * @throws Base::Exception 运行期故障：建不起上下文、OpenSSL 拒绝参数或加密本身失败
         */
        bool runAead(const QuicPacketKeys &keys, const std::uint64_t packetNumber, const std::span<const std::uint8_t> additionalData,
                     const std::span<const std::uint8_t> input, const std::span<std::uint8_t> destination,
                     std::uint8_t *const tag, const bool isEncryption)
        {
            const EVP_CIPHER *const cipher = cipherFor(keys.cipherSuite);
            if (cipher == nullptr)
            {
                throw Base::InvalidArgumentException(std::format("QUIC 包保护失败：套件取值 {} 不是已定义的密码套件：请检查密钥组是怎么填的",
                                                                 static_cast<int>(keys.cipherSuite)));
            }
            if (destination.size() != input.size())
            {
                throw Base::InvalidArgumentException(std::format("QUIC 包保护失败：输出缓冲 {} 字节装不下 {} 字节结果：两者长度必须相同",
                                                                 destination.size(), input.size()));
            }

            EVP_CIPHER_CTX *const context = acquireQuicCipherContext();
            if (context == nullptr)
            {
                throw Base::Exception("QUIC 包保护失败：拿不到可复用的 AEAD 上下文（" + quicOpenSslErrorText() + "）");
            }

            // 三种套件的 IV 都取密码的默认长度，因此不必发 SET_IVLEN；哪天支持到非 12 字节 IV 的
            // AEAD，再在这里补那条控制
            const auto initializationVector = buildInitializationVector(keys, packetNumber);
            const std::span<const std::uint8_t> encryptionKey = keys.encryptionKeyBytes();
            std::array<std::uint8_t, kCipherTrailerByteLength> trailer{};
            int writtenLength = 0;
            // 逐步判：一次 AEAD 失败可能出在密码设定、IV 长度、密钥交付、AAD、数据或收尾任意一处，
            // 只报「运算失败」等于让下一个人从头猜
            const char *failedStage = nullptr;
            if (EVP_CipherInit_ex(context, cipher, nullptr, nullptr, nullptr, isEncryption ? 1 : 0) != 1)
            {
                failedStage = "设定密码与方向";
            }
            else if (EVP_CipherInit_ex(context, nullptr, nullptr, encryptionKey.data(), initializationVector.data(),
                                       isEncryption ? 1 : 0) != 1)
            {
                failedStage = "交付密钥与 IV";
            }
            else if (!additionalData.empty() &&
                     EVP_CipherUpdate(context, nullptr, &writtenLength, additionalData.data(),
                                      static_cast<int>(additionalData.size())) != 1)
            {
                failedStage = "喂入附加认证数据";
            }
            else if (!input.empty() &&
                     EVP_CipherUpdate(context, destination.data(), &writtenLength, input.data(),
                                      static_cast<int>(input.size())) != 1)
            {
                failedStage = isEncryption ? "加密载荷" : "解密载荷";
            }
            else if (!isEncryption &&
                     EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_TAG,
                                         static_cast<int>(kQuicAuthenticationTagByteLength), tag) != 1)
            {
                // SET_TAG 的签名收 void* 但只读：解密一侧交进去的是标签的局部副本，不动调用方的内存
                failedStage = "交入期望标签";
            }
            else if (isEncryption && EVP_CipherFinal_ex(context, trailer.data(), &writtenLength) != 1)
            {
                failedStage = "收尾加密";
            }
            else if (isEncryption &&
                     EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_GET_TAG,
                                         static_cast<int>(kQuicAuthenticationTagByteLength), tag) != 1)
            {
                failedStage = "取出认证标签";
            }
            else if (!isEncryption && EVP_CipherFinal_ex(context, trailer.data(), &writtenLength) != 1)
            {
                // 交完标签再 Final 才会做校验：返回 0 就是标签不合，属正常失败而不是环境故障。
                // 上下文留给下一次取用时重置，不在这里归还
                return false;
            }

            if (failedStage != nullptr)
            {
                // 先把错误队列取空再拼文案：std::format 的实参求值顺序不定，晚一步就报不出真凶
                const std::string detail = quicOpenSslErrorText();
                throw Base::Exception(std::format("QUIC 包保护失败：{}这一步被拒（套件 {}，密钥 {} 字节，IV {} 字节，{}）：OpenSSL 报 {}",
                                                  failedStage, quicCipherSuiteName(keys.cipherSuite), encryptionKey.size(),
                                                  initializationVector.size(), isEncryption ? "加密" : "解密", detail));
            }
            return true;
        }
    } // namespace

    void appendQuicProtectedPayload(std::string &output, const QuicPacketKeys &keys, const std::uint64_t packetNumber,
                                     const std::span<const std::uint8_t> additionalData, const std::span<const std::uint8_t> plaintext)
    {
        // 包号超过 2^62-1 就没有合法编码（RFC 9000 §12.3），异或进 IV 只会拼出一个对端解不开的 nonce
        if (packetNumber > kQuicMaximumIntegerValue)
        {
            throw Base::InvalidArgumentException(std::format("包号 {} 超过 QUIC 的上限 {}（RFC 9000 §12.3 的 2^62-1）：请先停止发送，不要用它加密",
                                                             packetNumber, kQuicMaximumIntegerValue));
        }

        const std::size_t offset = output.size();
        output.resize(offset + plaintext.size() + kQuicAuthenticationTagByteLength);
        std::uint8_t *const tail = reinterpret_cast<std::uint8_t *>(output.data()) + offset;
        // 上面按「明文 + 16 字节标签」扩过容量，这里交出去的指针后面就有整整 16 字节可写
        runAead(keys, packetNumber, additionalData, plaintext, {tail, plaintext.size()},
                tail + plaintext.size(), true);
    }

    std::expected<std::size_t, QuicDecodeError> openQuicProtectedPayload(
            const std::span<std::uint8_t> plaintextOutput, const QuicPacketKeys &keys, const std::uint64_t packetNumber,
            const std::span<const std::uint8_t> additionalData, const std::span<const std::uint8_t> protectedPayload)
    {
        if (protectedPayload.size() < kQuicAuthenticationTagByteLength)
        {
            return std::unexpected(QuicDecodeError{
                    QuicDecodeErrorKind::Truncated,
                    std::format("QUIC 解包：密文总共 {} 字节，容不下 {} 字节认证标签（RFC 9001 §5.3：AEAD 输出比输入大 16 字节）：本包丢弃",
                                protectedPayload.size(), kQuicAuthenticationTagByteLength)});
        }

        const std::size_t plaintextLength = protectedPayload.size() - kQuicAuthenticationTagByteLength;
        if (plaintextOutput.size() != plaintextLength)
        {
            throw Base::InvalidArgumentException(std::format("QUIC 解包：明文缓冲是 {} 字节，密文却要产出 {} 字节：请按「密文长度减 {}」准备输出缓冲",
                                                             plaintextOutput.size(), plaintextLength, kQuicAuthenticationTagByteLength));
        }
        const std::span<const std::uint8_t> ciphertext = protectedPayload.subspan(0, plaintextLength);

        // OpenSSL 的 SET_TAG 签名收 void*（它只读），而 runAead 的标签参数是双侧共用的可写 span：
        // 这里把 16 字节标签拷进局部数组，不去动调用方给的内存
        std::array<std::uint8_t, kQuicAuthenticationTagByteLength> tagBuffer{};
        std::copy(protectedPayload.begin() + static_cast<std::ptrdiff_t>(plaintextLength),
                  protectedPayload.end(), tagBuffer.begin());
        if (!runAead(keys, packetNumber, additionalData, ciphertext, plaintextOutput, tagBuffer.data(), false))
        {
            return std::unexpected(QuicDecodeError{
                    QuicDecodeErrorKind::AuthenticationFailed,
                    std::format("QUIC 解包：包号 {} 的认证标签校验未通过（套件 {}）：按 RFC 9001 §4.1.4 整包丢弃，不要回错误码",
                                packetNumber, quicCipherSuiteName(keys.cipherSuite))});
        }
        return plaintextLength;
    }
} // namespace AsynGyanis::Net
