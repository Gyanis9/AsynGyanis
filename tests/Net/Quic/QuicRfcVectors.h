/**
 * @file QuicRfcVectors.h
 * @brief RFC 9000/9001 附录里的官方测试向量，供 QUIC 各层的用例共用
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 向量是从 rfc9001.txt 原文机器抽取后落下来的，改动请重新抽取而不是就地编辑：抄错一个字符的
 *          表现是「实现看起来是对的但解不开」，比编译失败难查得多。覆盖 RFC 9001 的附录 A.1（Initial
 *          密钥）、A.2（AES-128-GCM 的客户端 Initial，含组包器要用的整条保护态报文）与 A.5
 *          （ChaCha20-Poly1305 的最小短头包）。
 */

#pragma once

#include "NetTestSupport.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net::TestSupport
{
    /// 各附录共用的样例目的连接标识：Initial 密钥的输入，也是 A.2 报文里的目的标识
    constexpr std::string_view kVectorDestinationConnectionIdHex = "8394c8f03e515708";

    /// 附录 A.1 的客户端 Initial AEAD 密钥与 IV
    constexpr std::string_view kClientInitialKeyHex = "1f369613dd76d5467730efcbe3b1a22d";

    /// 附录 A.1 的客户端 Initial 初始化向量
    constexpr std::string_view kClientInitialInitializationVectorHex = "fa044b2f42a3fd3b46fb255c";

    /// 附录 A.2：未保护头部（18 字节 + 4 字节包号 2），同时就是 AEAD 的附加认证数据
    constexpr std::string_view kAppendixA2AdditionalDataHex = "c300000001088394c8f03e5157080000449e00000002";

    /// 附录 A.2：包号与整包载荷长度
    constexpr std::uint64_t kAppendixA2PacketNumber = 2ULL;

    /// 附录 A.2 那条报文的载荷长度：列出的帧加上补足用的 PADDING
    constexpr std::size_t kAppendixA2PayloadByteCount = 1162;

    /// 附录 A.2：RFC 只列出这 245 字节的帧，余下补足 1162 字节的部分全是 PADDING 帧
    constexpr std::string_view kAppendixA2FramesHex =
        "060040f1010000ed0303ebf8fa56f12939b9584a3896472ec40bb863cfd3e86804fe3a47f06a2b69484c0000041301130201"
        "0000c000000010000e00000b6578616d706c652e636f6dff01000100000a00080006001d0017001800100007000504616c70"
        "6e000500050100000000003300260024001d00209370b2c9caa47fbabaf4559fedba753de171fa71f50f1ce15d43e994ec74"
        "d748002b0003020304000d0010000e0403050306030203080408050806002d00020101001c00024001003900320408ffffff"
        "ffffffffff05048000ffff07048000ffff0801100104800075300901100f088394c8f03e51570806048000ffff";

    /// 附录 A.2：保护态报文的密文加标签（整包 1200 减去 22 字节保护态头部），AEAD 的期望值
    constexpr std::string_view kAppendixA2ProtectedPayloadHex =
        "d1b1c98dd7689fb8ec11d242b123dc9bd8bab936b47d92ec356c0bab7df5976d27cd449f63300099f3991c260ec4c60d17b3"
        "1f8429157bb35a1282a643a8d2262cad67500cadb8e7378c8eb7539ec4d4905fed1bee1fc8aafba17c750e2c7ace01e6005f"
        "80fcb7df621230c83711b39343fa028cea7f7fb5ff89eac2308249a02252155e2347b63d58c5457afd84d05dfffdb2039284"
        "4ae812154682e9cf012f9021a6f0be17ddd0c2084dce25ff9b06cde535d0f920a2db1bf362c23e596d11a4f5a6cf3948838a"
        "3aec4e15daf8500a6ef69ec4e3feb6b1d98e610ac8b7ec3faf6ad760b7bad1db4ba3485e8a94dc250ae3fdb41ed15fb6a8e5"
        "eba0fc3dd60bc8e30c5c4287e53805db059ae0648db2f64264ed5e39be2e20d82df566da8dd5998ccabdae053060ae6c7b43"
        "78e846d29f37ed7b4ea9ec5d82e7961b7f25a9323851f681d582363aa5f89937f5a67258bf63ad6f1a0b1d96dbd4faddfcef"
        "c5266ba6611722395c906556be52afe3f565636ad1b17d508b73d8743eeb524be22b3dcbc2c7468d54119c7468449a13d8e3"
        "b95811a198f3491de3e7fe942b330407abf82a4ed7c1b311663ac69890f4157015853d91e923037c227a33cdd5ec281ca3f7"
        "9c44546b9d90ca00f064c99e3dd97911d39fe9c5d0b23a229a234cb36186c4819e8b9c5927726632291d6a418211cc2962e2"
        "0fe47feb3edf330f2c603a9d48c0fcb5699dbfe5896425c5bac4aee82e57a85aaf4e2513e4f05796b07ba2ee47d80506f8d2"
        "c25e50fd14de71e6c418559302f939b0e1abd576f279c4b2e0feb85c1f28ff18f58891ffef132eef2fa09346aee33c28eb13"
        "0ff28f5b766953334113211996d20011a198e3fc433f9f2541010ae17c1bf202580f6047472fb36857fe843b19f5984009dd"
        "c324044e847a4f4a0ab34f719595de37252d6235365e9b84392b061085349d73203a4a13e96f5432ec0fd4a1ee65accdd5e3"
        "904df54c1da510b0ff20dcc0c77fcb2c0e0eb605cb0504db87632cf3d8b4dae6e705769d1de354270123cb11450efc60ac47"
        "683d7b8d0f811365565fd98c4c8eb936bcab8d069fc33bd801b03adea2e1fbc5aa463d08ca19896d2bf59a071b851e6c2390"
        "52172f296bfb5e72404790a2181014f3b94a4e97d117b438130368cc39dbb2d198065ae3986547926cd2162f40a29f0c3c87"
        "45c0f50fba3852e566d44575c29d39a03f0cda721984b6f440591f355e12d439ff150aab7613499dbd49adabc8676eef023b"
        "15b65bfc5ca06948109f23f350db82123535eb8a7433bdabcb909271a6ecbcb58b936a88cd4e8f2e6ff5800175f113253d8f"
        "a9ca8885c2f552e657dc603f252e1a8e308f76f0be79e2fb8f5d5fbbe2e30ecadd220723c8c0aea8078cdfcb3868263ff8f0"
        "940054da48781893a7e49ad5aff4af300cd804a6b6279ab3ff3afb64491c85194aab760d58a606654f9f4400e8b38591356f"
        "bf6425aca26dc85244259ff2b19c41b9f96f3ca9ec1dde434da7d2d392b905ddf3d1f9af93d1af5950bd493f5aa731b4056d"
        "f31bd267b6b90a079831aaf579be0a39013137aac6d404f518cfd46840647e78bfe706ca4cf5e9c5453e9f7cfd2b8b4c8d16"
        "9a44e55c88d4a9a7f9474241e221af44860018ab0856972e194cd934";

    /// 附录 A.2：完整保护态报文，组包器的端到端断言比这条（头部编码 + Length + AEAD + 头部保护）
    constexpr std::string_view kAppendixA2ProtectedPacketHex =
        "c000000001088394c8f03e5157080000449e7b9aec34d1b1c98dd7689fb8ec11d242b123dc9bd8bab936b47d92ec356c0bab"
        "7df5976d27cd449f63300099f3991c260ec4c60d17b31f8429157bb35a1282a643a8d2262cad67500cadb8e7378c8eb7539e"
        "c4d4905fed1bee1fc8aafba17c750e2c7ace01e6005f80fcb7df621230c83711b39343fa028cea7f7fb5ff89eac2308249a0"
        "2252155e2347b63d58c5457afd84d05dfffdb20392844ae812154682e9cf012f9021a6f0be17ddd0c2084dce25ff9b06cde5"
        "35d0f920a2db1bf362c23e596d11a4f5a6cf3948838a3aec4e15daf8500a6ef69ec4e3feb6b1d98e610ac8b7ec3faf6ad760"
        "b7bad1db4ba3485e8a94dc250ae3fdb41ed15fb6a8e5eba0fc3dd60bc8e30c5c4287e53805db059ae0648db2f64264ed5e39"
        "be2e20d82df566da8dd5998ccabdae053060ae6c7b4378e846d29f37ed7b4ea9ec5d82e7961b7f25a9323851f681d582363a"
        "a5f89937f5a67258bf63ad6f1a0b1d96dbd4faddfcefc5266ba6611722395c906556be52afe3f565636ad1b17d508b73d874"
        "3eeb524be22b3dcbc2c7468d54119c7468449a13d8e3b95811a198f3491de3e7fe942b330407abf82a4ed7c1b311663ac698"
        "90f4157015853d91e923037c227a33cdd5ec281ca3f79c44546b9d90ca00f064c99e3dd97911d39fe9c5d0b23a229a234cb3"
        "6186c4819e8b9c5927726632291d6a418211cc2962e20fe47feb3edf330f2c603a9d48c0fcb5699dbfe5896425c5bac4aee8"
        "2e57a85aaf4e2513e4f05796b07ba2ee47d80506f8d2c25e50fd14de71e6c418559302f939b0e1abd576f279c4b2e0feb85c"
        "1f28ff18f58891ffef132eef2fa09346aee33c28eb130ff28f5b766953334113211996d20011a198e3fc433f9f2541010ae1"
        "7c1bf202580f6047472fb36857fe843b19f5984009ddc324044e847a4f4a0ab34f719595de37252d6235365e9b84392b0610"
        "85349d73203a4a13e96f5432ec0fd4a1ee65accdd5e3904df54c1da510b0ff20dcc0c77fcb2c0e0eb605cb0504db87632cf3"
        "d8b4dae6e705769d1de354270123cb11450efc60ac47683d7b8d0f811365565fd98c4c8eb936bcab8d069fc33bd801b03ade"
        "a2e1fbc5aa463d08ca19896d2bf59a071b851e6c239052172f296bfb5e72404790a2181014f3b94a4e97d117b438130368cc"
        "39dbb2d198065ae3986547926cd2162f40a29f0c3c8745c0f50fba3852e566d44575c29d39a03f0cda721984b6f440591f35"
        "5e12d439ff150aab7613499dbd49adabc8676eef023b15b65bfc5ca06948109f23f350db82123535eb8a7433bdabcb909271"
        "a6ecbcb58b936a88cd4e8f2e6ff5800175f113253d8fa9ca8885c2f552e657dc603f252e1a8e308f76f0be79e2fb8f5d5fbb"
        "e2e30ecadd220723c8c0aea8078cdfcb3868263ff8f0940054da48781893a7e49ad5aff4af300cd804a6b6279ab3ff3afb64"
        "491c85194aab760d58a606654f9f4400e8b38591356fbf6425aca26dc85244259ff2b19c41b9f96f3ca9ec1dde434da7d2d3"
        "92b905ddf3d1f9af93d1af5950bd493f5aa731b4056df31bd267b6b90a079831aaf579be0a39013137aac6d404f518cfd468"
        "40647e78bfe706ca4cf5e9c5453e9f7cfd2b8b4c8d169a44e55c88d4a9a7f9474241e221af44860018ab0856972e194cd934";

    /// 附录 A.5：ChaCha20-Poly1305 的最小短头包，目的连接标识为空、3 字节包号 49140
    constexpr std::string_view kAppendixA5ProtectedPacketHex = "4cfe4189655e5cd55c41f69080575d7999c25a5bfb";

    /// 附录 A.5：同一包的 AEAD 输出（1 字节 PING 的密文加 16 字节标签），即整包去掉 4 字节头部
    constexpr std::string_view kAppendixA5ProtectedPayloadHex = "655e5cd55c41f69080575d7999c25a5bfb";

    /// 附录 A.5 的附加认证数据：短头首字节加截断后的包号
    constexpr std::string_view kAppendixA5AdditionalDataHex = "4200bff4";

    /// 附录 A.5 的明文载荷：就一条 PING 帧
    constexpr std::string_view kAppendixA5PlaintextHex = "01";

    /// 附录 A.5 的包号（线上写 3 字节）
    constexpr std::uint64_t kAppendixA5PacketNumber = 654360564ULL;

    constexpr std::string_view kAppendixA5KeyHex = "c6d98ff3441c3fe1b2182094f69caa2e"
                                                   "d4b716b65488960a7a984979fb23e1c8";

    /// 附录 A.5 的 ChaCha20-Poly1305 初始化向量
    constexpr std::string_view kAppendixA5InitializationVectorHex = "e0459b3474bdd0e44a41c144";

    constexpr std::string_view kAppendixA5HeaderProtectionKeyHex = "25a282b9e82f06f21f488917a4fc8f1b"
                                                                   "73573685608597d0efcb076b0ab7a7a4";

    /// 样例目的连接标识的字节形态，供各层用例直接当 span/容器用
    inline const std::vector<std::uint8_t> kDestinationConnectionIdBytes = makeBytesFromHex(kVectorDestinationConnectionIdHex);

    /**
     * @brief 拼出附录 A.2 的完整明文载荷
     * @details 用 resize 补零而不是贴两千多个字符：余下 917 字节全是 PADDING 帧，而 PADDING 的
     *          类型字节就是 0x00。
     * @return std::vector<std::uint8_t> 1162 字节载荷
     */
    [[nodiscard]] inline std::vector<std::uint8_t> buildAppendixA2Plaintext()
    {
        auto plaintext = makeBytesFromHex(kAppendixA2FramesHex);
        plaintext.resize(kAppendixA2PayloadByteCount);
        return plaintext;
    }
} // namespace AsynGyanis::Net::TestSupport
