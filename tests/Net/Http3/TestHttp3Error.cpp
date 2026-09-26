#include "Net/Http3/Http3Error.h"

#include <gtest/gtest.h>

namespace
{
    using AsynGyanis::Net::Http3ErrorCode;
    using AsynGyanis::Net::http3ErrorCodeName;
} // namespace

TEST(Http3Error, NamesMatchTheWireIdentifiers)
{
    // 钉住日志里出现的名字与 RFC 里的标识符逐字一致：名字写错了，运维照名字去查规范会查不到条目
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::NoError), "H3_NO_ERROR");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::GeneralProtocolError), "H3_GENERAL_PROTOCOL_ERROR");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::StreamCreationError), "H3_STREAM_CREATION_ERROR");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::ClosedCriticalStream), "H3_CLOSED_CRITICAL_STREAM");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::FrameUnexpected), "H3_FRAME_UNEXPECTED");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::FrameError), "H3_FRAME_ERROR");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::SettingsError), "H3_SETTINGS_ERROR");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::MissingSettings), "H3_MISSING_SETTINGS");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::RequestRejected), "H3_REQUEST_REJECTED");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::RequestCancelled), "H3_REQUEST_CANCELLED");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::MessageError), "H3_MESSAGE_ERROR");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::VersionFallback), "H3_VERSION_FALLBACK");
}

TEST(Http3Error, ConnectErrorCodeUsesThePublishedName)
{
    // 钉住 0x010f 的名字：早期草案叫 H3_CONNECT_PROTOCOL，RFC 9114 终稿改叫 H3_CONNECT_ERROR。
    // 取错名字会让按名字比对日志的脚本认不出这是扩展 CONNECT 那侧的失败
    EXPECT_EQ(static_cast<std::uint64_t>(Http3ErrorCode::ConnectError), 0x010FULL);
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::ConnectError), "H3_CONNECT_ERROR");
}

TEST(Http3Error, QpackCodesShareTheIntegerSpaceAndKeepTheirOwnPrefix)
{
    // 钉住 QPACK 的三个码：取值来自 RFC 9204，名字保留 QPACK_ 前缀而不是 H3_，
    // 因为上层对「头块解不开」和「编码器流坏了」的处置完全不同（前者只废一条流）
    EXPECT_EQ(static_cast<std::uint64_t>(Http3ErrorCode::DecompressionFailed), 0x0200ULL);
    EXPECT_EQ(static_cast<std::uint64_t>(Http3ErrorCode::EncoderStreamError), 0x0201ULL);
    EXPECT_EQ(static_cast<std::uint64_t>(Http3ErrorCode::DecoderStreamError), 0x0202ULL);
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::DecompressionFailed), "QPACK_DECOMPRESSION_FAILED");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::EncoderStreamError), "QPACK_ENCODER_STREAM_ERROR");
    EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::DecoderStreamError), "QPACK_DECODER_STREAM_ERROR");
}

TEST(Http3Error, UnknownCodeGetsAFallbackNameInsteadOfThrowing)
{
    // 钉住拒绝面：对端可以发 62 位内的任意整数，未知取值既不能抛也不能返回空串
    EXPECT_EQ(http3ErrorCodeName(static_cast<Http3ErrorCode>(0x01ff)), "未知 HTTP/3 错误码");
    EXPECT_EQ(http3ErrorCodeName(static_cast<Http3ErrorCode>(0x3fffffffffffffffULL)), "未知 HTTP/3 错误码");
    EXPECT_FALSE(http3ErrorCodeName(static_cast<Http3ErrorCode>(0x01ff)).empty());
}
