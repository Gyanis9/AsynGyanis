/**
 * 覆盖地址验证令牌的签发与核验：往返、地址绑定、篡改、有效期、密钥轮换窗口，以及三种不合格形状
 * 与两种不合格配置。载体是令牌这一个计算件——不碰 socket 也不碰事件循环，因此每个判据都可直接复跑；
 * 线上形状（客户端带着这张令牌真能建起连接）由 TestQuicClientConnection 里那条端到端负责。
 */

#include "Net/Quic/QuicAddressValidationToken.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Socket/InetAddress.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    using AsynGyanis::Net::QuicAddressValidationTokenCodec;
    using AsynGyanis::Net::QuicAddressValidationTokenContent;
    using AsynGyanis::Net::QuicAddressValidationTokenExpected;
    using AsynGyanis::Net::QuicAddressValidationTokenStatus;

    /// 签发时刻的基准：用例里所有时刻都从这一格往前推，避免拿本机墙钟当判据
    constexpr std::chrono::seconds kIssuedAtEpochSeconds{1'770'000'000};

    constexpr std::chrono::seconds kDefaultLifetime{30};

    /// 一把够长的密钥：十六进制摆出来便于复现某一条失败
    const std::vector<std::uint8_t> kSigningKey(32, 0xA7U);
    const std::vector<std::uint8_t> kOtherSigningKey(32, 0x3BU);

    /// 客户端第一个 Initial 的目的标识：令牌要把它恢复出来
    const std::vector<std::uint8_t> kOriginalDestinationConnectionId{0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08};

    /**
     * @brief 把「地址 + 时刻」这对入参摆成本测试的用例值
     * @param port 端口：地址绑定这一族的用例只改这一格，其余逐字相同
     */
    AsynGyanis::Platform::SocketAddress peerAddressOn(const std::uint16_t port)
    {
        return AsynGyanis::Core::InetAddress("127.0.0.1", port).platformAddress();
    }

    std::chrono::system_clock::time_point issuedAt()
    {
        return std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(kIssuedAtEpochSeconds));
    }

    /**
     * @brief 造一张令牌，失败即断言当场终止：下面的判据都建立在「令牌确实签出来了」之上
     */
    std::vector<std::uint8_t> issueOrFail(const QuicAddressValidationTokenCodec &codec, const AsynGyanis::Platform::SocketAddress &peerAddress,
                                          const std::chrono::system_clock::time_point moment = issuedAt())
    {
        const std::vector<std::uint8_t> token = codec.issue(peerAddress, std::span<const std::uint8_t>(kOriginalDestinationConnectionId), moment);
        EXPECT_FALSE(token.empty()) << "签发交回了空令牌：这一格坏掉的话，下面所有判据都成了空转";
        return token;
    }
} // namespace

TEST(QuicAddressValidationToken, RoundTripsForTheSameAddress)
{
    const QuicAddressValidationTokenCodec codec(std::span<const std::uint8_t>(kSigningKey), kDefaultLifetime);
    const auto                            token = issueOrFail(codec, peerAddressOn(44333));

    // 一秒之后就回来：Retry 的令牌是按「客户端立刻重发 Initial」设计的（RFC 9000 §8.1.2）
    const QuicAddressValidationTokenExpected<QuicAddressValidationTokenContent> verified =
            codec.verify(std::span<const std::uint8_t>(token), peerAddressOn(44333), issuedAt() + std::chrono::seconds{1});

    ASSERT_HAS_VALUE(verified) << "自家签的令牌自家验不过，状态是 " << static_cast<int>(verified.error());
    // 恢复出来的原始目的标识必须与签进去的那条逐字节相同：参数里的 ODCID 与 Initial 密钥都靠它
    EXPECT_EQ(verified->originalDestinationConnectionId, kOriginalDestinationConnectionId);
    EXPECT_EQ(verified->issuedAge, std::chrono::seconds{1});
}

TEST(QuicAddressValidationToken, RejectsATokenPresentedFromAnotherAddress)
{
    const QuicAddressValidationTokenCodec codec(std::span<const std::uint8_t>(kSigningKey), kDefaultLifetime);
    const auto                            token = issueOrFail(codec, peerAddressOn(44333));

    // RFC 9000 §8.1.4 的 SHOULD 就是这一格：令牌要能核对「源地址与端口保持不变」。搬给同 IP 的另一个端口
    // 也算搬走——攻击者拿受害者地址收到的令牌，是自己打不到本端的
    const QuicAddressValidationTokenExpected<QuicAddressValidationTokenContent> moved =
            codec.verify(std::span<const std::uint8_t>(token), peerAddressOn(44334), issuedAt() + std::chrono::seconds{1});
    ASSERT_FALSE(moved.has_value());
    EXPECT_EQ(moved.error(), QuicAddressValidationTokenStatus::AddressMismatch);
}

TEST(QuicAddressValidationToken, RejectsTamperedTokens)
{
    const QuicAddressValidationTokenCodec codec(std::span<const std::uint8_t>(kSigningKey), kDefaultLifetime);
    const auto                            original = issueOrFail(codec, peerAddressOn(44333));

    // 三处各改一比特：明文前缀里的时刻、封在里面的原始目的标识、以及地址栏。认证码盖住整段前缀，
    // 三处都只可能是 IntegrityFailed——改动者没有密钥，凑不出第二份认证码
    std::vector<std::uint8_t> tamperedIssuedSecond = original;
    tamperedIssuedSecond[1] ^= 0x01U;
    std::vector<std::uint8_t> tamperedConnectionId = original;
    tamperedConnectionId[6] ^= 0x01U;
    std::vector<std::uint8_t> tamperedAddressTag = original;
    tamperedAddressTag[original.size() - 17] ^= 0x01U;

    for (const std::vector<std::uint8_t> &tampered: {tamperedIssuedSecond, tamperedConnectionId, tamperedAddressTag})
    {
        const auto rejected = codec.verify(std::span<const std::uint8_t>(tampered), peerAddressOn(44333), issuedAt() + std::chrono::seconds{1});
        ASSERT_FALSE(rejected.has_value()) << "被改过的令牌验过了，这一格的前缀字节是 " << static_cast<int>(tampered[0]);
        EXPECT_EQ(rejected.error(), QuicAddressValidationTokenStatus::IntegrityFailed);
    }
}

TEST(QuicAddressValidationToken, AcceptsUntilTheLastSecondAndRefusesAfter)
{
    const QuicAddressValidationTokenCodec codec(std::span<const std::uint8_t>(kSigningKey), kDefaultLifetime);
    const auto                            token = issueOrFail(codec, peerAddressOn(44333));
    const std::span<const std::uint8_t>   tokenView(token);

    // 有效期的边界要说死：整三十秒这一刻仍在窗口内（「短时接受」是 SHOULD 而不是硬截断，本端取闭区间）
    EXPECT_TRUE(codec.verify(tokenView, peerAddressOn(44333), issuedAt() + kDefaultLifetime).has_value());
    const auto oneSecondLate = codec.verify(tokenView, peerAddressOn(44333), issuedAt() + kDefaultLifetime + std::chrono::seconds{1});
    ASSERT_FALSE(oneSecondLate.has_value());
    EXPECT_EQ(oneSecondLate.error(), QuicAddressValidationTokenStatus::Expired) << "过了有效期还认：令牌就成了可反复使用的通行证";

    // 未来时刻：32 位回绕或时钟倒退都会把「签发于未来」读成一个巨大的年龄，这一格单独拒掉才不误导
    const auto fromTheFuture = codec.verify(tokenView, peerAddressOn(44333), issuedAt() - std::chrono::seconds{1});
    ASSERT_FALSE(fromTheFuture.has_value());
    EXPECT_EQ(fromTheFuture.error(), QuicAddressValidationTokenStatus::Expired);
}

TEST(QuicAddressValidationToken, KeepsAcceptingOldTokensForOneLifetimeAfterRotation)
{
    QuicAddressValidationTokenCodec codec(std::span<const std::uint8_t>(kSigningKey), kDefaultLifetime);
    const auto                      oldToken = issueOrFail(codec, peerAddressOn(44333));

    codec.rotateSigningKey(std::span<const std::uint8_t>(kOtherSigningKey));
    const auto freshToken = issueOrFail(codec, peerAddressOn(44333));
    ASSERT_NE(oldToken, freshToken) << "换了密钥却签出同一段字节，说明签名根本没参与";

    const std::span<const std::uint8_t> oldTokenView(oldToken);
    const std::span<const std::uint8_t> freshTokenView(freshToken);
    // 轮换之后、旧令牌还在有效期内：正在握手的客户端手里那份必须还能用，否则一次轮换就把在途的握手掐死
    EXPECT_TRUE(codec.verify(oldTokenView, peerAddressOn(44333), issuedAt() + std::chrono::seconds{1}).has_value()) << "轮换把在途的令牌一起作废了";
    EXPECT_TRUE(codec.verify(freshTokenView, peerAddressOn(44333), issuedAt() + std::chrono::seconds{1}).has_value());
    // 过了有效期，旧密钥签的那批就再也无人认领；新密钥签的仍按自家窗口算
    EXPECT_FALSE(codec.verify(oldTokenView, peerAddressOn(44333), issuedAt() + kDefaultLifetime + std::chrono::seconds{1}).has_value());
    EXPECT_TRUE(codec.verify(freshTokenView, peerAddressOn(44333), issuedAt() + kDefaultLifetime).has_value());
}

TEST(QuicAddressValidationToken, TellsAbsentMalformedAndForeignShapesApart)
{
    const QuicAddressValidationTokenCodec codec(std::span<const std::uint8_t>(kSigningKey), kDefaultLifetime);
    const auto                            token   = issueOrFail(codec, peerAddressOn(44333));
    const auto                            address = peerAddressOn(44333);
    const auto                            later   = issuedAt() + std::chrono::seconds{1};

    // 没带令牌是「还没验过地址」，调用方据此回 Retry，与「伪造」必须分档
    EXPECT_EQ(codec.verify({}, address, later).error(), QuicAddressValidationTokenStatus::Absent);

    // 类别不是本端签的那一档（NEW_TOKEN 那一类本实现不认）
    std::vector<std::uint8_t> foreignKind = token;
    foreignKind[0]                        = 0x02U;
    EXPECT_EQ(codec.verify(std::span<const std::uint8_t>(foreignKind), address, later).error(), QuicAddressValidationTokenStatus::Malformed);

    // 少一比特标签、多一字节尾巴、以及把标识长度字段撑过 v1 上限：三种都是形状不合，不猜对端想表达什么
    EXPECT_EQ(codec.verify(std::span<const std::uint8_t>(token.data(), token.size() - 1), address, later).error(), QuicAddressValidationTokenStatus::Malformed);
    std::vector<std::uint8_t> withTrailer = token;
    withTrailer.push_back(0x00U);
    EXPECT_EQ(codec.verify(std::span<const std::uint8_t>(withTrailer), address, later).error(), QuicAddressValidationTokenStatus::Malformed);
    std::vector<std::uint8_t> oversizedConnectionIdLength = token;
    oversizedConnectionIdLength[5]                        = 0xFFU;
    EXPECT_EQ(codec.verify(std::span<const std::uint8_t>(oversizedConnectionIdLength), address, later).error(), QuicAddressValidationTokenStatus::Malformed);

    // 一字节长什么都读不出来：这一格排在长度字段之前判，别越界读
    const std::vector<std::uint8_t> singleByte{0x01U};
    EXPECT_EQ(codec.verify(std::span<const std::uint8_t>(singleByte), address, later).error(), QuicAddressValidationTokenStatus::Malformed);
}

TEST(QuicAddressValidationToken, RefusesWeakConfiguration)
{
    const std::vector<std::uint8_t> shortKey(15, 0x01U);
    // 密钥短于 128 位就不叫「难猜」：构造当场拒，而不是签出一张能被穷举的令牌
    EXPECT_THROW(QuicAddressValidationTokenCodec(std::span<const std::uint8_t>(shortKey), kDefaultLifetime), AsynGyanis::Base::InvalidArgumentException);
    // 有效期为非正数：签出来即过期，等于让每笔握手白跑一趟
    EXPECT_THROW(QuicAddressValidationTokenCodec(std::span<const std::uint8_t>(kSigningKey), std::chrono::seconds{0}), AsynGyanis::Base::InvalidArgumentException);
    EXPECT_THROW(QuicAddressValidationTokenCodec(std::span<const std::uint8_t>(kSigningKey), std::chrono::seconds{-5}), AsynGyanis::Base::InvalidArgumentException);

    // 轮换会改对象自己那份密钥表，因此这一格的载体不是 const 对象
    QuicAddressValidationTokenCodec codec(std::span<const std::uint8_t>(kSigningKey), kDefaultLifetime);
    EXPECT_THROW(codec.rotateSigningKey(std::span<const std::uint8_t>(shortKey)), AsynGyanis::Base::InvalidArgumentException);
    EXPECT_EQ(codec.lifetime(), kDefaultLifetime) << "读数该原样说出配置的那一档";
}

TEST(QuicAddressValidationToken, IssuesDistinctTokensPerAddressAndPerInstant)
{
    const QuicAddressValidationTokenCodec codec(std::span<const std::uint8_t>(kSigningKey), kDefaultLifetime);

    const auto firstAddressToken  = issueOrFail(codec, peerAddressOn(44333));
    const auto secondAddressToken = issueOrFail(codec, peerAddressOn(44334));
    const auto oneSecondLater     = issueOrFail(codec, peerAddressOn(44333), issuedAt() + std::chrono::seconds{1});

    // 地址不同则令牌不同（地址栏在里面），同一地址换个时刻也不同：客户端因此拿不到一张能复用的通行证，
    // 本端也不必为「谁用过哪张」留任何状态
    EXPECT_NE(firstAddressToken, secondAddressToken);
    EXPECT_NE(firstAddressToken, oneSecondLater);
    EXPECT_EQ(firstAddressToken.size(), secondAddressToken.size()) << "同一格式的令牌长度应当只差在封进去的标识长度上";
}
