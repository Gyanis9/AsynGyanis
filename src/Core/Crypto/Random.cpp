#include "Core/Crypto/Random.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"

#include <openssl/rand.h>

#include <algorithm>
#include <climits>
#include <format>
#include <string>
#include <string_view>

namespace AsynGyanis::Core::Random
{
    namespace
    {
        /// OpenSSL 的取字节接口按 int 收长度，超过 INT_MAX 的请求在这里当场拒而不是静默截断
        constexpr std::size_t kMaximumRequestByteLength = static_cast<std::size_t>(INT_MAX);

        /**
         * @brief 三条入口共同的落点：按用途调一份随机源并核对返回码
         * @details OpenSSL 的返回码有三种，语义各不相同，报错必须分开写：
         *          ① `1` 成功；② `-1` 「按策略不能拿来生成密钥材料」——DRBG 没能采到足够熵，
         *          此时字节本身可能可用，但当作长期密钥就是错的；③ `0` 一般失败（熵池被判定不可用、
         *          实例被外部销毁等）。把 ② 与 ③ 混成一条「随机源失败」，排查时就会错过
         *          「这台机器的熵源不足以签密钥」这种必须现场处置的信息。
         * @param destination 待填满的缓冲，调用方已保证非空
         * @param isSecret true 表示取的是密钥材料，走私有 DRBG 实例
         * @param entryName 调用方那条口的名字（只用于报错文案）
         */
        void fillWith(const std::span<std::uint8_t> destination, const bool isSecret, const std::string_view entryName)
        {
            const int status =
                    isSecret ? RAND_priv_bytes(destination.data(), static_cast<int>(destination.size())) : RAND_bytes(destination.data(), static_cast<int>(destination.size()));
            if (status == 1)
            {
                return;
            }

            // 抛出前把缓冲清零：OpenSSL 在失败时不承诺缓冲内容未被改动，留着旧字节就是让调用方
            // 有机会拿一段「上一次调用留下的」数据当本次随机数用
            std::fill(destination.begin(), destination.end(), static_cast<std::uint8_t>(0));

            if (status == -1)
            {
                throw Base::Exception(std::format("{}: 系统的密码学随机源采不到足够的熵（返回 -1），取出的 {} 字节按策略不可用作密钥材料。"
                                                  "原因：默认提供者里的 DRBG 尚未按熵源策略完成实例化，常见于早期启动、容器里无可用熵源、"
                                                  "或外部销毁过随机实例。替代做法：确认熵源就绪后重试；不要退回用时间或地址拼随机数",
                                                  entryName, destination.size()));
            }
            throw Base::Exception(std::format("{}: 系统的密码学随机源取不到 {} 字节（返回 {}）。"
                                              "原因：熵池不可用或随机实例已失效。替代做法：让本次操作直接失败并留证据，不要拿未定义的字节继续跑",
                                              entryName, destination.size(), status));
        }

        /// 长度上限判据：三入口共用一条
        void assertLengthUsable(const std::size_t byteLength, const std::string_view entryName)
        {
            if (byteLength > kMaximumRequestByteLength)
            {
                throw Base::InvalidArgumentException(std::format("{}: 一次请求 {} 字节超出 OpenSSL 接口能表达的长度上限（INT_MAX）。"
                                                                 "原因：这个长度不是要取的随机数，而是调用方的参数算错了。"
                                                                 "替代做法：按用途分段取，或把长度收到合理量级",
                                                                 entryName, byteLength));
            }
        }

        /// 按长度分配再填满（bytes() 与 secretBytes() 同一条路）
        [[nodiscard]] std::vector<std::uint8_t> allocateAndFill(const std::size_t byteLength, const bool isSecret, const std::string_view entryName)
        {
            assertLengthUsable(byteLength, entryName);
            std::vector<std::uint8_t> buffer(byteLength);
            fillWith(buffer, isSecret, entryName);
            return buffer;
        }
    } // namespace

    void fill(const std::span<std::uint8_t> destination)
    {
        if (destination.empty())
        {
            return;
        }
        assertLengthUsable(destination.size(), "Core::Random::fill");
        fillWith(destination, false, "Core::Random::fill");
    }

    void fillSecret(std::span<std::uint8_t> destination)
    {
        if (destination.empty())
        {
            return;
        }
        assertLengthUsable(destination.size(), "Core::Random::fillSecret");
        fillWith(destination, true, "Core::Random::fillSecret");
    }

    std::vector<std::uint8_t> bytes(const std::size_t byteLength)
    {
        return allocateAndFill(byteLength, false, "Core::Random::bytes");
    }

    std::vector<std::uint8_t> secretBytes(const std::size_t byteLength)
    {
        return allocateAndFill(byteLength, true, "Core::Random::secretBytes");
    }
} // namespace AsynGyanis::Core::Random
