// TestWebSocketUtf8.cpp —— WebSocket 文本负载 UTF-8 校验（RFC 6455 §5.6 + RFC 3629）的单元测试
//
// 边界候选值逐条列出：合法侧覆盖 ASCII、2/3/4 字节各自的码点上下界（U+0080 / U+07FF / U+0800 /
// U+D7FF / U+E000 / U+FFFF / U+10000 / U+10FFFF）与 NUL 字节；非法侧覆盖过长编码、孤立续字节、
// 代理区码点、超出 U+10FFFF 的码点、截断的多字节序列与续字节形状错误。另有用例钉住「违规位置」
// 的回报——会话层要把它写进中文日志，才能指认是第几个字节出的问题。
// 用例都是纯计算，不起网络、不依赖任何外部服务。

#include "Net/WebSocket/WebSocketUtf8.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 由字节值拼出待校验串
         * @details 候选值里含 0x00，按 const char* 构造会被零终止截断，那样断言就测不到完整字节了
         * @param byteValues 字节值序列
         * @return std::string 逐字节写入的结果
         */
        std::string makeBytes(const std::initializer_list<unsigned char> byteValues)
        {
            std::string bytes;
            bytes.reserve(byteValues.size());
            for (const unsigned char byteValue: byteValues)
            {
                bytes.push_back(static_cast<char>(byteValue));
            }
            return bytes;
        }

        /**
         * @brief 一条待校验候选值
         */
        struct Utf8Candidate
        {
            std::string_view description; ///< 候选值说明：写清码点或违规形态，失败时能直接定位到是哪一条
            std::string      bytes;       ///< 待校验字节
        };
    } // namespace

    /**
     * @brief 钉住合法侧的全部边界：ASCII、2/3/4 字节各自的码点上下界、U+10FFFF 与 NUL 字节
     * @details 逐条列出候选值而不是只测一个普通汉字：UTF-8 的合法性判定集中在各档边界上
     *          （U+007F/U+0080、U+07FF/U+0800、U+D7FF/U+E000、U+FFFF/U+10000），少测哪一档都会
     *          让边界另一侧的错误实现悄悄通过。
     */
    TEST(WebSocketUtf8, AcceptsEveryLegalBoundaryCodePoint)
    {
        const Utf8Candidate candidates[] = {
                {"空串：零长度文本也是合法文本", std::string()},
                {"ASCII：hello", makeBytes({0x68, 0x65, 0x6c, 0x6c, 0x6f})},
                {"ASCII 上界：U+007F = 7F", makeBytes({0x7f})},
                {"NUL 字节：U+0000 单独成串", makeBytes({0x00})},
                {"NUL 字节：U+0000 夹在文本中间", makeBytes({0x61, 0x00, 0x62})},
                {"2 字节下界：U+0080 = C2 80", makeBytes({0xc2, 0x80})},
                {"2 字节上界：U+07FF = DF BF", makeBytes({0xdf, 0xbf})},
                {"3 字节下界：U+0800 = E0 A0 80", makeBytes({0xe0, 0xa0, 0x80})},
                {"代理区下界之前：U+D7FF = ED 9F BF", makeBytes({0xed, 0x9f, 0xbf})},
                {"代理区上界之后：U+E000 = EE 80 80", makeBytes({0xee, 0x80, 0x80})},
                {"替换字符：U+FFFD = EF BF BD", makeBytes({0xef, 0xbf, 0xbd})},
                {"3 字节上界：U+FFFF = EF BF BF", makeBytes({0xef, 0xbf, 0xbf})},
                {"4 字节下界：U+10000 = F0 90 80 80", makeBytes({0xf0, 0x90, 0x80, 0x80})},
                {"Unicode 上限：U+10FFFF = F4 8F BF BF", makeBytes({0xf4, 0x8f, 0xbf, 0xbf})},
                {"混排：汉字（3 字节）+ 表情（4 字节）", makeBytes({0xe6, 0xb1, 0x89, 0xe5, 0xad, 0x97, 0xf0, 0x9f, 0x98, 0x80})},
        };

        for (const Utf8Candidate &candidate: candidates)
        {
            SCOPED_TRACE(candidate.description);
            EXPECT_TRUE(isValidWebSocketUtf8(candidate.bytes)) << "字节数 " << candidate.bytes.size();
            EXPECT_EQ(findInvalidWebSocketUtf8ByteOffset(candidate.bytes), std::string_view::npos) << "合法串不得报出违规位置";
        }
    }

    /**
     * @brief 钉住非法侧的全部形态：过长编码、孤立续字节、代理区、超范围与截断
     * @details 每一条都是一个独立的拒绝面：漏掉任何一类都会让不符合 RFC 3629 的字节流被当成
     *          合法文本交付给业务，而对端正是靠这类字节流的解释差异来做协议的。
     */
    TEST(WebSocketUtf8, RejectsEveryIllegalForm)
    {
        const Utf8Candidate candidates[] = {
                {"过长编码：U+0000 写成 2 字节 C0 80", makeBytes({0xc0, 0x80})},
                {"过长编码：U+007F 写成 2 字节 C1 BF", makeBytes({0xc1, 0xbf})},
                {"过长编码：U+0000 写成 3 字节 E0 80 80", makeBytes({0xe0, 0x80, 0x80})},
                {"过长编码：U+0000 写成 4 字节 F0 80 80 80", makeBytes({0xf0, 0x80, 0x80, 0x80})},
                {"孤立续字节：80", makeBytes({0x80})},
                {"孤立续字节：BF", makeBytes({0xbf})},
                {"首字节形状非法：FE", makeBytes({0xfe})},
                {"首字节形状非法：FF", makeBytes({0xff})},
                {"首字节形状非法：已废弃的 5 字节形态 F8 80 80 80 80", makeBytes({0xf8, 0x80, 0x80, 0x80, 0x80})},
                {"代理区下界：U+D800 = ED A0 80", makeBytes({0xed, 0xa0, 0x80})},
                {"代理区上界：U+DFFF = ED BF BF", makeBytes({0xed, 0xbf, 0xbf})},
                {"超出 U+10FFFF：U+110000 = F4 90 80 80", makeBytes({0xf4, 0x90, 0x80, 0x80})},
                {"超出 U+10FFFF：首字节落在 F5 上", makeBytes({0xf5, 0x80, 0x80, 0x80})},
                {"截断的 2 字节序列：只有 C2", makeBytes({0xc2})},
                {"截断的 3 字节序列：只有 E4 B8", makeBytes({0xe4, 0xb8})},
                {"截断的 4 字节序列：只有 F0 9F 98", makeBytes({0xf0, 0x9f, 0x98})},
                {"续字节形状错：C2 后面跟 ASCII 41", makeBytes({0xc2, 0x41})},
                {"续字节形状错：E4 B8 后面跟 ASCII 41", makeBytes({0xe4, 0xb8, 0x41})},
        };

        for (const Utf8Candidate &candidate: candidates)
        {
            SCOPED_TRACE(candidate.description);
            EXPECT_FALSE(isValidWebSocketUtf8(candidate.bytes)) << "字节数 " << candidate.bytes.size();
            // 每个候选值自身就是一段从 0 开始的违规字节，违规位置因此必须是 0 而不是「随便一个值」
            EXPECT_EQ(findInvalidWebSocketUtf8ByteOffset(candidate.bytes), 0U);
        }
    }

    /**
     * @brief 钉住违规位置的回报：合法前缀之后应指向违规序列的起始字节
     * @details 会话层把位置写进中文日志（「第几个字节起违规」），位置算错排查时就会跳到无关字节上。
     *          这里覆盖 2/3/4 字节三种违规序列的起点，以及前缀含多字节合法字符时的偏移换算。
     */
    TEST(WebSocketUtf8, ReportsViolationOffsetAfterLegalPrefix)
    {
        // 合法前缀 "ok" 之后是截断的 3 字节序列：违规点在第 2 个字节
        EXPECT_EQ(findInvalidWebSocketUtf8ByteOffset(makeBytes({0x6f, 0x6b, 0xe4, 0xb8})), 2U);
        // 合法前缀 "a" 之后是代理区序列（其后还有合法 ASCII，位置必须落在代理序列上而不是那个 ASCII 上）
        EXPECT_EQ(findInvalidWebSocketUtf8ByteOffset(makeBytes({0x61, 0xed, 0xa0, 0x80, 0x62})), 1U);
        // 合法前缀是三字节字符 U+4E16（E4 B8 96）之后是 FF：违规点在第 3 个字节
        EXPECT_EQ(findInvalidWebSocketUtf8ByteOffset(makeBytes({0xe4, 0xb8, 0x96, 0xff})), 3U);
        // 合法前缀含 4 字节字符 U+1F600（F0 9F 98 80）之后是孤立续字节：违规点在第 4 个字节
        EXPECT_EQ(findInvalidWebSocketUtf8ByteOffset(makeBytes({0xf0, 0x9f, 0x98, 0x80, 0x80})), 4U);
        // 整段合法：没有违规位置可报，两个入口必须给出一致的结论
        EXPECT_EQ(findInvalidWebSocketUtf8ByteOffset(makeBytes({0x68, 0x69})), std::string_view::npos);
        EXPECT_TRUE(isValidWebSocketUtf8(makeBytes({0x68, 0x69})));
    }
} // namespace AsynGyanis::Net
