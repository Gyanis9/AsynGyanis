// TestQpack.cpp —— 自研 QPACK（RFC 9204）的单元测试
//
// 字节向量全部来自规范原文或裁判实现，本端编码器的输出不做自产自验：
//   1) RFC 9204 附录 B.1–B.5 的逐字节示例（编码器流 / 解码器流 / 头块三条线上的数据，连同每一步的表
//      大小 106 → 160 → 217 → 215）。这些字节已用本机 pylsqpack 1.3.0（LSQPACK 真实现）复跑过，
//      解出的头列表与本文件的断言一致；编码方向的同一段字节另用附录 C 的单遍算法手工推导核对。
//   2) RFC 9204 §4.5.1.1 的两个算例：表 100 字节 → Required Insert Count 按 6 取模；收到 10 次插入时
//      Encoded Insert Count 4 还原成 9（以及只有 3 次插入时同一取值还原成 3）。
//   3) pylsqpack 编码器产出的对照向量（Huffman 字面量 + 动态表引用 + 表后索引），只喂给本端解码器解。
//   4) 规范没给向量的分支（N 位、增量喂字节的切分、阻塞名额归还、各类拒绝面）按 §4 的图手搓**输入**
//      字节，判据取自条文；最后一节是往返性质检查，不作为字节向量。
// 用例都是纯计算，不起网络、不依赖任何外部服务。

#include "AllocationProbe.h"
#include "Net/Http3/Qpack.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::Net::TestSupport::containsText;
        using AsynGyanis::Net::TestSupport::makeBytesFromHex;
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;
        using AsynGyanis::TestSupport::resetAllocationHistogram;
        using AsynGyanis::TestSupport::snapshotAllocationHistogram;
        using AsynGyanis::TestSupport::AllocationHistogram;
        using AsynGyanis::TestSupport::AllocationProfile;

        /// 头列表按规范里的「名 = 值」二元组表述
        using FieldListEntry = std::pair<const char *, const char *>;

        /**
         * @brief 把十六进制文本解析成字节容器
         * @param hexadecimalText 十六进制文本，空白不限（照抄 RFC 的排版）
         * @return std::vector<std::uint8_t> 字节容器，调用方持有后再取 span
         */
        [[nodiscard]] std::vector<std::uint8_t> hexToBytes(std::string_view hexadecimalText)
        {
            return makeBytesFromHex(hexadecimalText);
        }

        /**
         * @brief 取字节容器的只读视图
         * @param bytes 字节容器
         * @return std::span<const std::uint8_t> 同一块字节的视图
         */
        [[nodiscard]] std::span<const std::uint8_t> asSpan(const std::vector<std::uint8_t> &bytes)
        {
            return std::span<const std::uint8_t>(bytes);
        }

        /**
         * @brief 把字节写成小写十六进制文本，便于失败时与规范原文逐字节对照
         * @param bytes 字节序列
         * @return std::string 十六进制串
         */
        [[nodiscard]] std::string toHex(std::string_view bytes)
        {
            static constexpr char hexDigits[] = "0123456789abcdef";
            std::string text;
            for (const char item : bytes)
            {
                const auto byteValue = static_cast<unsigned char>(item);
                text.push_back(hexDigits[byteValue >> 4]);
                text.push_back(hexDigits[byteValue & 0x0F]);
            }
            return text;
        }

        /**
         * @brief 按名值对造一份头列表
         * @param entries 名值对序列
         * @return std::vector<QpackHeaderField> 可直接交给编码器的头列表
         */
        [[nodiscard]] std::vector<QpackHeaderField> makeFieldList(const std::vector<FieldListEntry> &entries)
        {
            std::vector<QpackHeaderField> fieldLines;
            fieldLines.reserve(entries.size());
            for (const FieldListEntry &entry : entries)
            {
                fieldLines.push_back(QpackHeaderField{entry.first, entry.second});
            }
            return fieldLines;
        }

        /**
         * @brief 断言解出的头列表与期望逐项相等
         * @param fields 解出的头列表
         * @param expected 期望的名值对
         * @param context 失败时的上下文说明
         */
        void expectFieldsEqual(const std::vector<QpackHeaderField> &fields, const std::vector<FieldListEntry> &expected,
                               const std::string_view context)
        {
            ASSERT_EQ(fields.size(), expected.size()) << context;
            for (std::size_t index = 0; index < fields.size() && index < expected.size(); ++index)
            {
                EXPECT_EQ(fields[index].name, std::string(expected[index].first)) << context << " 第 " << index << " 项的名";
                EXPECT_EQ(fields[index].value, std::string(expected[index].second)) << context << " 第 " << index << " 项的值";
            }
        }

        /**
         * @brief 断言动态表内容与期望的名值对逐项相等（下标 0 是最新插入项）
         * @param entries 表内容
         * @param expected 期望的名值对，按「新 → 旧」给出
         * @param context 失败时的上下文说明
         */
        void expectTableEqual(const std::deque<QpackHeaderField> &entries, const std::vector<FieldListEntry> &expected,
                              const std::string_view context)
        {
            ASSERT_EQ(entries.size(), expected.size()) << context;
            for (std::size_t index = 0; index < entries.size() && index < expected.size(); ++index)
            {
                EXPECT_EQ(entries[index].name, std::string(expected[index].first)) << context << " 表内第 " << index << " 项的名";
                EXPECT_EQ(entries[index].value, std::string(expected[index].second)) << context << " 表内第 " << index << " 项的值";
            }
        }

        /**
         * @brief 造一份解码器约束
         * @param maximumTableCapacityByteCount 本端公布的动态表容量上限
         * @param maximumBlockedStreamCount 本端承诺支持的阻塞流数
         * @param maximumFieldSectionSizeByteCount 本端公布的头段大小上限，0 为不限
         * @return QpackDecoderSettings 约束对象
         */
        [[nodiscard]] QpackDecoderSettings makeDecoderSettings(std::size_t maximumTableCapacityByteCount,
                                                              std::size_t maximumBlockedStreamCount,
                                                              std::size_t maximumFieldSectionSizeByteCount = 0)
        {
            return QpackDecoderSettings{maximumTableCapacityByteCount, maximumBlockedStreamCount, maximumFieldSectionSizeByteCount};
        }

        /**
         * @brief 把附录 B.2 的容量指令与两条插入喂进解码器，丢弃其间产生的解码器流字节
         * @param decoder 目标解码器
         */
        void feedAppendixB2Instructions(QpackDecoder &decoder)
        {
            const std::vector<std::uint8_t> instructions = hexToBytes("3fbd01"
                                                                      "c00f 7777 772e 6578 616d 706c 652e 636f 6d"
                                                                      "c10c 2f73 616d 706c 652f 7061 7468");
            std::vector<std::uint64_t> unblockedStreamIds;
            std::string controlBytes;
            ASSERT_TRUE(decoder.feedEncoderStream(asSpan(instructions), unblockedStreamIds, controlBytes).has_value());
        }

        /**
         * @brief 依次喂完附录 B.2、B.3、B.4 的编码器流指令，把表推到 Size=217 的四项状态
         * @param decoder 目标解码器
         */
        void replayAppendixB2ToB4Instructions(QpackDecoder &decoder)
        {
            feedAppendixB2Instructions(decoder);
            const std::vector<std::uint8_t> literalInsert = hexToBytes("4a63 7573 746f 6d2d 6b65 790c 6375 7374 6f6d 2d76 616c 7565");
            const std::vector<std::uint8_t> duplicate = hexToBytes("02");
            std::vector<std::uint64_t> unblockedStreamIds;
            std::string controlBytes;
            ASSERT_TRUE(decoder.feedEncoderStream(asSpan(literalInsert), unblockedStreamIds, controlBytes).has_value());
            ASSERT_TRUE(decoder.feedEncoderStream(asSpan(duplicate), unblockedStreamIds, controlBytes).has_value());
        }

        /**
         * @brief 走一遍附录 B.2 的编码：设容量 220 并插入两条带静态名引用的表项
         * @param encoder 目标编码器
         * @return std::string 本段解出的头块字节，供调用方继续比对
         */
        [[nodiscard]] std::string encodeAppendixB2FieldSection(QpackEncoder &encoder)
        {
            const std::vector<QpackHeaderField> fieldLines = makeFieldList({{":authority", "www.example.com"},
                                                                           {":path", "/sample/path"}});
            std::string headerBlock;
            std::string encoderStreamBytes;
            const auto result = encoder.encodeFieldSection(4, std::span<const QpackHeaderField>(fieldLines), headerBlock,
                                                           encoderStreamBytes);
            EXPECT_TRUE(result.has_value()) << (result.has_value() ? std::string() : result.error().message);
            EXPECT_EQ(toHex(encoderStreamBytes), "3fbd01c00f7777772e6578616d706c652e636f6dc10c2f73616d706c652f70617468");
            return headerBlock;
        }
    } // namespace

    // ============================================================================
    // 静态表（RFC 9204 附录 A）
    // ============================================================================

    /**
     * @brief 钉住静态表的形状：99 项、每项有名、下标即线上索引（QPACK 从 0 起编号）
     */
    TEST(Qpack, StaticTableHasNinetyNineEntriesAllNamed)
    {
        EXPECT_EQ(kQpackStaticTable.size(), 99U) << "RFC 9204 附录 A 的 Table 4 共 99 项";
        EXPECT_EQ(kQpackStaticTableEntryCount, 99U);
        for (std::size_t index = 0; index < kQpackStaticTable.size(); ++index)
        {
            EXPECT_FALSE(kQpackStaticTable[index].name.empty()) << "索引 " << index << " 的项必须有名字";
            EXPECT_EQ(kQpackStaticTable[index].name.find(' '), std::string_view::npos) << "索引 " << index << " 的名不该含空格";
            // 名里最多只有开头一个冒号（伪头），出现第二次就是抄错了
            const std::size_t firstColonIndex = kQpackStaticTable[index].name.find(':');
            if (firstColonIndex != std::string_view::npos)
            {
                EXPECT_EQ(firstColonIndex, 0U) << "索引 " << index << " 的名把冒号放错了位置";
                EXPECT_EQ(kQpackStaticTable[index].name.find(':', 1), std::string_view::npos) << "索引 " << index << " 的名里有两个冒号";
            }
        }
        // 前 15 项与 15..28 项是伪头段，按原文顺序排布
        EXPECT_EQ(kQpackStaticTable[0].name, ":authority");
        EXPECT_EQ(kQpackStaticTable[63].name, ":status");
        EXPECT_EQ(kQpackStaticTable[63].value, "100");
    }

    /**
     * @brief 钉住静态表若干条与附录 A 原文对照，重点是原文里被排版折行的那几项
     */
    TEST(Qpack, StaticTableEntriesMatchRfcAppendixA)
    {
        static constexpr std::array<std::tuple<std::size_t, std::string_view, std::string_view>, 24> samples{{
            {0, ":authority", ""},
            {1, ":path", "/"},
            {2, "age", "0"},
            {4, "content-length", "0"},
            {14, "set-cookie", ""},
            {15, ":method", "CONNECT"},
            {21, ":method", "PUT"},
            {25, ":status", "200"},
            {28, ":status", "503"},
            {29, "accept", "*/*"},
            {30, "accept", "application/dns-message"},
            {31, "accept-encoding", "gzip, deflate, br"},
            {41, "cache-control", "public, max-age=31536000"},
            {45, "content-type", "application/javascript"},
            {47, "content-type", "application/x-www-form-urlencoded"},
            {52, "content-type", "text/html; charset=utf-8"},
            {54, "content-type", "text/plain;charset=utf-8"},
            {57, "strict-transport-security", "max-age=31536000; includesubdomains"},
            {58, "strict-transport-security", "max-age=31536000; includesubdomains; preload"},
            {62, "x-xss-protection", "1; mode=block"},
            {85, "content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
            {91, "purpose", "prefetch"},
            {94, "upgrade-insecure-requests", "1"},
            {98, "x-frame-options", "sameorigin"},
        }};

        for (const auto &[index, expectedName, expectedValue] : samples)
        {
            EXPECT_EQ(kQpackStaticTable[index].name, expectedName) << "索引 " << index << " 的名与原文不符";
            EXPECT_EQ(kQpackStaticTable[index].value, expectedValue) << "索引 " << index << " 的值与原文不符";
        }
    }

    /**
     * @brief 钉住两个查表助手：整项命中优先、同名取索引最小者、未命中用哨兵而不是 0
     */
    TEST(Qpack, StaticTableLookupHelpers)
    {
        // 索引 0 是合法项（:authority 且值为空），所以「未命中」绝不能借用 0 表达
        EXPECT_EQ(findQpackStaticTableIndex(":authority", ""), 0U);
        EXPECT_EQ(findQpackStaticTableIndex("x-frame-options", "sameorigin"), 98U);
        EXPECT_EQ(findQpackStaticTableIndex(":status", "200"), 25U) << "同名多值时按值精确匹配";
        EXPECT_EQ(findQpackStaticTableIndex(":status", "418"), kQpackStaticTableNoIndex);
        EXPECT_EQ(findQpackStaticTableIndex("no-such-field", ""), kQpackStaticTableNoIndex);

        EXPECT_EQ(findQpackStaticTableNameIndex(":method"), 15U) << "只按名匹配时取索引最小的那一项";
        EXPECT_EQ(findQpackStaticTableNameIndex("content-type"), 44U);
        EXPECT_EQ(findQpackStaticTableNameIndex("accept"), 29U);
        EXPECT_EQ(findQpackStaticTableNameIndex("no-such-field"), kQpackStaticTableNoIndex);
        // 值区分大小写：表里有 "TRUE"（74）与 "FALSE"（73），没有 "true"
        EXPECT_EQ(findQpackStaticTableIndex("access-control-allow-credentials", "TRUE"), 74U);
        EXPECT_EQ(findQpackStaticTableIndex("access-control-allow-credentials", "true"), kQpackStaticTableNoIndex);
    }

    /**
     * @brief 钉住失败类别到线上错误码的映射：三类 QPACK 码各自独立，别把流级错误升成连接级
     */
    TEST(Qpack, ErrorKindMapsToWireErrorCode)
    {
        EXPECT_EQ(toHttp3ErrorCode(QpackErrorKind::DecompressionFailed), Http3ErrorCode::DecompressionFailed);
        EXPECT_EQ(toHttp3ErrorCode(QpackErrorKind::EncoderStreamError), Http3ErrorCode::EncoderStreamError);
        EXPECT_EQ(toHttp3ErrorCode(QpackErrorKind::DecoderStreamError), Http3ErrorCode::DecoderStreamError);
        EXPECT_EQ(toHttp3ErrorCode(QpackErrorKind::FieldSectionTooLarge), Http3ErrorCode::ExcessiveLoad);
        EXPECT_EQ(toHttp3ErrorCode(QpackErrorKind::InvalidLocalState), Http3ErrorCode::InternalError);
        EXPECT_EQ(http3ErrorCodeName(Http3ErrorCode::EncoderStreamError), std::string_view("QPACK_ENCODER_STREAM_ERROR"));
    }

    // ============================================================================
    // RFC 9204 附录 B：逐字节示例（解码方向）
    // ============================================================================

    /**
     * @brief 钉住 B.1：无动态表时的「带静态名引用的字面量」，前缀两个字段都是 0 且不发任何解码器流字节
     */
    TEST(Qpack, DecoderReadsAppendixB1LiteralFieldLineWithNameReference)
    {
        QpackDecoder decoder(makeDecoderSettings(0, 0));
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        const std::vector<std::uint8_t> section = hexToBytes("0000"
                                                            "510b 2f69 6e64 6578 2e68 746d 6c");
        const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(*result, QpackFieldSectionDecodeStatus::Decoded) << "本段不含动态表引用，不该挂起";
        expectFieldsEqual(fields, {{":path", "/index.html"}}, "B.1 的头列表");
        EXPECT_TRUE(controlBytes.empty()) << "Required Insert Count 为 0 的段不发 Section Ack：" << toHex(controlBytes);
        EXPECT_EQ(decoder.insertCount(), 0U);
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 0U);
        EXPECT_FALSE(decoder.hasBlockedStreams());
    }

    /**
     * @brief 钉住 B.2 的编码器流：Set Capacity=220 与两条带名引用的插入，表大小按 §3.2.1 算得 106
     */
    TEST(Qpack, DecoderReadsAppendixB2EncoderStreamInstructions)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;

        const std::vector<std::uint8_t> instructions = hexToBytes("3fbd01"
                                                                  "c00f 7777 772e 6578 616d 706c 652e 636f 6d"
                                                                  "c10c 2f73 616d 706c 652f 7061 7468");
        const auto consumed = decoder.feedEncoderStream(asSpan(instructions), unblockedStreamIds, controlBytes);
        ASSERT_TRUE(consumed.has_value()) << consumed.error().message;
        EXPECT_EQ(*consumed, instructions.size()) << "整段指令都要被消费掉";
        EXPECT_TRUE(unblockedStreamIds.empty());
        EXPECT_EQ(decoder.tableCapacityByteCount(), 220U) << "Set Dynamic Table Capacity=220";
        EXPECT_EQ(decoder.insertCount(), 2U);
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 106U) << "RFC 的 B.2 标注 Size=106（57 + 49）";
        // 表首是最新插入项：:path=/sample/path 在绝对索引 1，:authority 在 0
        expectTableEqual(decoder.dynamicTableEntries(), {{":path", "/sample/path"}, {":authority", "www.example.com"}}, "B.2 的表");
        EXPECT_EQ(toHex(controlBytes), "02") << "处理完 2 次插入即回 Insert Count Increment(2)：模式 '00' + 6 位前缀";
    }

    /**
     * @brief 钉住 B.2 的头块（Required Insert Count=2、Base=0、两条表后索引）与「Section Ack 只在交付后发」
     */
    TEST(Qpack, DecoderReadsAppendixB2FieldSectionAndDefersSectionAcknowledgement)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::string controlBytes;
        feedAppendixB2Instructions(decoder);

        const std::vector<std::uint8_t> section = hexToBytes("0381"
                                                            "10"
                                                            "11");
        std::vector<QpackHeaderField> fields;
        controlBytes.clear();
        const auto result = decoder.decodeFieldSection(4, asSpan(section), fields, controlBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(*result, QpackFieldSectionDecodeStatus::Decoded) << "两次插入都已收到，不必挂起";
        expectFieldsEqual(fields, {{":authority", "www.example.com"}, {":path", "/sample/path"}}, "B.2 的头列表");
        EXPECT_TRUE(controlBytes.empty()) << "解码本身不产生 Section Ack：" << toHex(controlBytes);

        const auto delivered = decoder.noteFieldSectionDelivered(4, controlBytes);
        ASSERT_TRUE(delivered.has_value()) << delivered.error().message;
        EXPECT_EQ(toHex(controlBytes), "84") << "RFC 的 B.2 解码器流上就是单个字节 0x84（Section Ack, stream=4）";
        EXPECT_EQ(decoder.knownReceivedInsertCount(), 2U);

        controlBytes.clear();
        const auto repeated = decoder.noteFieldSectionDelivered(4, controlBytes);
        ASSERT_FALSE(repeated.has_value()) << "同一计数不允许重复确认";
        EXPECT_EQ(repeated.error().kind, QpackErrorKind::DecoderStreamError) << repeated.error().message;
        EXPECT_TRUE(controlBytes.empty()) << "失败时不得写出半个指令";
    }

    /**
     * @brief 钉住 B.3：带字面量名的插入把表推到 160 字节，解码器流上是 Insert Count Increment(1) = 0x01
     */
    TEST(Qpack, DecoderReadsAppendixB3SpeculativeInsert)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;
        feedAppendixB2Instructions(decoder);

        controlBytes.clear();
        const std::vector<std::uint8_t> insert = hexToBytes("4a63 7573 746f 6d2d 6b65 790c 6375 7374 6f6d 2d76 616c 7565");
        const auto consumed = decoder.feedEncoderStream(asSpan(insert), unblockedStreamIds, controlBytes);
        ASSERT_TRUE(consumed.has_value()) << consumed.error().message;
        EXPECT_EQ(*consumed, insert.size());
        EXPECT_EQ(decoder.insertCount(), 3U);
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 160U) << "RFC 的 B.3 标注 Size=160";
        EXPECT_EQ(toHex(controlBytes), "01") << "RFC 的 B.3 解码器流上就是 0x01（Insert Count Increment(1)）";
        expectTableEqual(decoder.dynamicTableEntries(),
                         {{"custom-key", "custom-value"}, {":path", "/sample/path"}, {":authority", "www.example.com"}},
                         "B.3 的表");

        controlBytes.clear();
        EXPECT_EQ(decoder.emitInsertCountIncrement(controlBytes), 0U) << "同一个计数不得重复告诉对端（§4.4.3）";
        EXPECT_TRUE(controlBytes.empty());
    }

    /**
     * @brief 钉住 B.4：Duplicate 复制绝对索引 0 使表到 217 字节，头块用相对索引引用 3/2，取消时发 0x48
     */
    TEST(Qpack, DecoderReadsAppendixB4DuplicateAndStreamCancellation)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::string controlBytes;
        replayAppendixB2ToB4Instructions(decoder);

        EXPECT_EQ(decoder.insertCount(), 4U) << "Duplicate 也占一次插入计数（§4.3.4）";
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 217U) << "RFC 的 B.4 标注 Size=217";
        expectTableEqual(decoder.dynamicTableEntries(),
                         {{":authority", "www.example.com"},
                          {"custom-key", "custom-value"},
                          {":path", "/sample/path"},
                          {":authority", "www.example.com"}},
                         "B.4 的表：绝对索引 3 是 0 的副本");

        const std::vector<std::uint8_t> section = hexToBytes("0500"
                                                            "80"
                                                            "c1"
                                                            "81");
        std::vector<QpackHeaderField> fields;
        controlBytes.clear();
        const auto result = decoder.decodeFieldSection(8, asSpan(section), fields, controlBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        expectFieldsEqual(fields,
                          {{":authority", "www.example.com"}, {":path", "/"}, {"custom-key", "custom-value"}},
                          "B.4 的头列表：Base=4，相对索引 0/1 指向绝对索引 3/2");

        // 头块已解出但被放弃：按 §4.4.2 只发 Stream Cancellation，之前欠的 Section Ack 一并作废
        controlBytes.clear();
        decoder.noteStreamAbandoned(8, controlBytes);
        EXPECT_EQ(toHex(controlBytes), "48") << "RFC 的 B.4 解码器流上就是 0x48（Stream Cancellation, stream=8）";
        std::string lateControlBytes;
        const auto delivered = decoder.noteFieldSectionDelivered(8, lateControlBytes);
        ASSERT_FALSE(delivered.has_value()) << "被放弃的段不再欠 Section Ack";
        EXPECT_EQ(delivered.error().kind, QpackErrorKind::DecoderStreamError) << delivered.error().message;
    }

    /**
     * @brief 钉住 B.5：带动态名引用的插入把最旧项挤出表，大小回到 215、Dropping Point 前进一格
     */
    TEST(Qpack, DecoderReadsAppendixB5InsertEvictsOldestEntry)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;
        replayAppendixB2ToB4Instructions(decoder);

        controlBytes.clear();
        const std::vector<std::uint8_t> insert = hexToBytes("810d 6375 7374 6f6d 2d76 616c 7565 32");
        const auto consumed = decoder.feedEncoderStream(asSpan(insert), unblockedStreamIds, controlBytes);
        ASSERT_TRUE(consumed.has_value()) << consumed.error().message;
        EXPECT_EQ(*consumed, insert.size());
        EXPECT_EQ(decoder.insertCount(), 5U) << "累计插入数不因淘汰而回退（§3.2.4）";
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 215U) << "RFC 的 B.5 标注 Size=215";
        EXPECT_EQ(toHex(controlBytes), "01") << "又收到 1 次插入";
        expectTableEqual(decoder.dynamicTableEntries(),
                         {{"custom-key", "custom-value2"},
                          {":authority", "www.example.com"},
                          {"custom-key", "custom-value"},
                          {":path", "/sample/path"}},
                         "B.5 的表：绝对索引 0 已被淘汰");
        // 淘汰后按绝对索引取项必须失败，而不是悄悄给一项别的
        const std::vector<std::uint8_t> referenceToEvicted = hexToBytes("0683"
                                                                       "80");
        std::vector<QpackHeaderField> fields;
        controlBytes.clear();
        const auto result = decoder.decodeFieldSection(12, asSpan(referenceToEvicted), fields, controlBytes);
        ASSERT_FALSE(result.has_value()) << "引用已淘汰的绝对索引 0 必须判错（§2.2.3）";
        EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        EXPECT_TRUE(fields.empty()) << "失败时不留下半截头列表";
    }

    // ============================================================================
    // RFC 9204 附录 B：逐字节示例（编码方向，按附录 C 的单遍算法推导）
    // ============================================================================

    /**
     * @brief 钉住 B.1 的编码：对端容量上限为 0 时不产生任何编码器流指令，头块逐字节等于原文
     */
    TEST(Qpack, EncoderEmitsAppendixB1Bytes)
    {
        QpackEncoder encoder(0, 0, 0);
        const std::vector<QpackHeaderField> fieldLines = makeFieldList({{":path", "/index.html"}});
        std::string headerBlock;
        std::string encoderStreamBytes;

        const auto result = encoder.encodeFieldSection(0, std::span<const QpackHeaderField>(fieldLines), headerBlock,
                                                       encoderStreamBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(toHex(headerBlock), "0000510b2f696e6465782e68746d6c") << "RFC 的 B.1 原文：0000 510b 2f69 6e64 6578 2e68 746d 6c";
        EXPECT_TRUE(encoderStreamBytes.empty()) << "§3.2.3：对端上限为 0 时不得发任何编码器流指令";
        EXPECT_EQ(encoder.insertCount(), 0U);
        EXPECT_FALSE(encoder.hasBlockedStreams());
    }

    /**
     * @brief 钉住 B.2 的编码：容量指令 + 两条带静态名引用的插入，头块是「03 81 10 11」
     */
    TEST(Qpack, EncoderEmitsAppendixB2Bytes)
    {
        QpackEncoder encoder(220, 100, 220);
        const std::vector<QpackHeaderField> fieldLines = makeFieldList({{":authority", "www.example.com"},
                                                                        {":path", "/sample/path"}});
        std::string headerBlock;
        std::string encoderStreamBytes;

        const auto result = encoder.encodeFieldSection(4, std::span<const QpackHeaderField>(fieldLines), headerBlock,
                                                      encoderStreamBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(toHex(encoderStreamBytes), "3fbd01c00f7777772e6578616d706c652e636f6dc10c2f73616d706c652f70617468")
            << "RFC 的 B.2 编码器流：3fbd01 + c00f www.example.com + c10c /sample/path";
        EXPECT_EQ(toHex(headerBlock), "03811011") << "Required Insert Count=2、Base=0、两条表后索引";
        EXPECT_EQ(encoder.dynamicTableSizeByteCount(), 106U);
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 0U);
        EXPECT_TRUE(encoder.hasBlockedStreams()) << "引用了对端尚未确认的表项，这条流占一个阻塞名额";
        EXPECT_EQ(encoder.blockedStreamCount(), 1U);
    }

    /**
     * @brief 钉住 B.3/B.4/B.5 的编码器流字节：字面量名插入、draining 项触发的 Duplicate、动态名引用插入
     */
    TEST(Qpack, EncoderEmitsAppendixB3ToB5InstructionBytes)
    {
        QpackEncoder encoder(220, 100, 220);
        std::string headerBlock;
        std::string encoderStreamBytes;

        // B.2 的两条插入（含构造时欠下的容量指令），然后按 RFC 的样子把这一段确认掉
        headerBlock = encodeAppendixB2FieldSection(encoder);
        EXPECT_EQ(toHex(headerBlock), "03811011");
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("84"))).has_value());
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 2U);

        // B.3：只有一行 custom-key=custom-value，双字面量插入 + 表后索引
        const std::vector<QpackHeaderField> third = makeFieldList({{"custom-key", "custom-value"}});
        const auto thirdResult = encoder.encodeFieldSection(8, std::span<const QpackHeaderField>(third), headerBlock,
                                                           encoderStreamBytes);
        ASSERT_TRUE(thirdResult.has_value()) << thirdResult.error().message;
        EXPECT_EQ(toHex(encoderStreamBytes), "4a637573746f6d2d6b65790c637573746f6d2d76616c7565") << "RFC 的 B.3 原文";
        EXPECT_EQ(toHex(headerBlock), "048010") << "Required Insert Count=3、Base=2";
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("01"))).has_value()) << "Insert Count Increment(1)";
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 3U);

        // B.4：命中的 :authority 落在绝对索引 0（已被 draining 甩在身后），改发 Duplicate 并引用新副本
        const std::vector<QpackHeaderField> fourth = makeFieldList({{":authority", "www.example.com"}});
        const auto fourthResult = encoder.encodeFieldSection(12, std::span<const QpackHeaderField>(fourth), headerBlock,
                                                            encoderStreamBytes);
        ASSERT_TRUE(fourthResult.has_value()) << fourthResult.error().message;
        EXPECT_EQ(toHex(encoderStreamBytes), "02") << "RFC 的 B.4 原文：Duplicate（相对索引 2）";
        EXPECT_EQ(toHex(headerBlock), "058010") << "Base=3、Required Insert Count=4，引用表首的新副本";
        EXPECT_EQ(encoder.dynamicTableSizeByteCount(), 217U) << "RFC 的 B.4 标注 Size=217";

        // B.5：custom-key=custom-value2 只有名命中（绝对索引 2，相对索引 1）
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("8c"))).has_value()) << "Section Ack, stream=12";
        const std::vector<QpackHeaderField> fifth = makeFieldList({{"custom-key", "custom-value2"}});
        const auto fifthResult = encoder.encodeFieldSection(16, std::span<const QpackHeaderField>(fifth), headerBlock,
                                                           encoderStreamBytes);
        ASSERT_TRUE(fifthResult.has_value()) << fifthResult.error().message;
        EXPECT_EQ(toHex(encoderStreamBytes), "810d637573746f6d2d76616c756532") << "RFC 的 B.5 原文";
        EXPECT_EQ(encoder.insertCount(), 5U);
        EXPECT_EQ(encoder.dynamicTableSizeByteCount(), 215U) << "最旧的一项被挤出（RFC 标注 Size=215）";
    }

    /**
     * @brief 钉住「静态表整项命中不碰动态表」：既不发 Duplicate，也不发任何编码器流指令（§4.3.4）
     */
    TEST(Qpack, EncoderNeverDuplicatesStaticEntries)
    {
        QpackEncoder encoder(4096, 100, 4096);
        std::string headerBlock;
        std::string encoderStreamBytes;
        ASSERT_TRUE(encoder.setMaximumTableCapacityByteCount(4096, encoderStreamBytes).has_value());
        EXPECT_EQ(toHex(encoderStreamBytes), "3fe11f") << "4096 的 5 位前缀编码是 3f e1 1f";

        const std::vector<QpackHeaderField> fieldLines = makeFieldList({{":status", "200"},
                                                                       {":method", "GET"},
                                                                       {":path", "/"}});
        headerBlock.clear();
        encoderStreamBytes.clear();
        const auto result = encoder.encodeFieldSection(0, std::span<const QpackHeaderField>(fieldLines), headerBlock,
                                                      encoderStreamBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_TRUE(encoderStreamBytes.empty()) << "静态表整项命中不得产生任何指令：" << toHex(encoderStreamBytes);
        EXPECT_EQ(toHex(headerBlock), "0000" "d9" "d1" "c1") << "前缀 0000 + 静态索引 25/17/1";
        EXPECT_EQ(encoder.insertCount(), 0U);
        EXPECT_FALSE(encoder.hasBlockedStreams());
    }

    // ============================================================================
    // 动态表与索引换算（§2.1.1、§3.2.1、§3.2.5、§4.5.1.1）
    // ============================================================================

    /**
     * @brief 钉住表项大小算式（名长 + 值长 + 32）与 MaxEntries 的下取整
     */
    TEST(Qpack, EntrySizeAndMaximumEntryCountFollowRfc)
    {
        // 附录 B 的实际数字：:authority(10) + www.example.com(15) + 32 = 57
        EXPECT_EQ(QpackDynamicTable::entrySizeByteCountOf(QpackHeaderField{":authority", "www.example.com"}), 57U);
        EXPECT_EQ(QpackDynamicTable::entrySizeByteCountOf(QpackHeaderField{"", ""}), kQpackEntryOverheadByteCount) << "空项也是 32 字节";
        EXPECT_EQ(QpackDynamicTable::maximumEntryCount(100), 3U) << "§4.5.1.1 的例子：100 字节的表按 6 取模";
        EXPECT_EQ(QpackDynamicTable::maximumEntryCount(32), 1U);
        EXPECT_EQ(QpackDynamicTable::maximumEntryCount(63), 1U);
        EXPECT_EQ(QpackDynamicTable::maximumEntryCount(64), 2U);
        // 容量不足 32 时 MaxEntries 为 0：这里必须是 0，不得当成 1 去做取模
        EXPECT_EQ(QpackDynamicTable::maximumEntryCount(31), 0U);
        EXPECT_EQ(QpackDynamicTable::maximumEntryCount(0), 0U);
    }

    /**
     * @brief 钉住动态表的插入、淘汰与两种索引换算（绝对索引 / 相对表首的相对索引）
     */
    TEST(Qpack, DynamicTableInsertEvictAndIndexMapping)
    {
        QpackDynamicTable table(64);
        const std::optional<std::uint64_t> firstIndex = table.insert(QpackHeaderField{"a", ""});
        ASSERT_TRUE(firstIndex.has_value()) << "33 字节的项放得进 64 字节的表";
        EXPECT_EQ(*firstIndex, 0U) << "首项的绝对索引是 0（§3.2.4）";
        const std::optional<std::uint64_t> secondIndex = table.insert(QpackHeaderField{"b", ""});
        ASSERT_TRUE(secondIndex.has_value());
        EXPECT_EQ(*secondIndex, 1U);
        // 64 字节的表装不下两项 33 字节，插入前必须先从表尾淘汰（§3.2.2），故此刻只剩刚插入的这一项
        EXPECT_EQ(table.sizeByteCount(), 33U) << "第二项把首项挤掉了，表里只有一项";

        // 第三项同样放不下：先淘汰表尾那一项再插入（§3.2.2 的顺序）
        const std::optional<std::uint64_t> thirdIndex = table.insert(QpackHeaderField{"c", ""});
        ASSERT_TRUE(thirdIndex.has_value());
        EXPECT_EQ(*thirdIndex, 2U);
        EXPECT_EQ(table.entryCount(), 1U) << "容量 64 只放得下一项";
        EXPECT_EQ(table.insertCount(), 3U);
        EXPECT_EQ(table.droppedEntryCount(), 2U) << "绝对索引 0、1 已被淘汰";

        QpackHeaderField field;
        EXPECT_TRUE(table.tryGetEntryByAbsoluteIndex(2, field));
        EXPECT_EQ(field.name, "c");
        EXPECT_FALSE(table.tryGetEntryByAbsoluteIndex(1, field)) << "已淘汰的绝对索引不能命中";
        EXPECT_FALSE(table.tryGetEntryByAbsoluteIndex(3, field)) << "尚未插入的绝对索引不能命中";
        // 相对索引 0 = 最新插入项（编码器指令的语境，§3.2.5）
        EXPECT_TRUE(table.tryGetEntryByRelativeIndexFromInsertionPoint(0, field));
        EXPECT_EQ(field.name, "c");
        EXPECT_FALSE(table.tryGetEntryByRelativeIndexFromInsertionPoint(1, field)) << "越过表尾的相对索引不能命中";

        // 单项大于容量：不插入也不清空表（§3.2.2 判错），而不是把表当 1 项处理
        const std::optional<std::uint64_t> tooLarge = table.insert(QpackHeaderField{std::string(40, 'x'), ""});
        EXPECT_FALSE(tooLarge.has_value());
        EXPECT_EQ(table.entryCount(), 1U) << "失败的插入不得动过表内容";
        EXPECT_EQ(table.sizeByteCount(), 33U) << "表大小仍是那唯一一项的 1 + 0 + 32（§3.2.1）";
    }

    /**
     * @brief 钉住容量变更：缩容按 §3.2.2 从表尾淘汰，容量 0 清空表内容但绝对索引空间不回退
     */
    TEST(Qpack, DynamicTableCapacityChangeKeepsAbsoluteIndexSpace)
    {
        QpackDynamicTable table(220);
        for (const char *name : {"a", "b", "c"})
        {
            ASSERT_TRUE(table.insert(QpackHeaderField{name, std::string(20, 'v')}).has_value());
        }
        const std::uint64_t insertCountBefore = table.insertCount();
        EXPECT_TRUE(table.setCapacityByteCount(0, nullptr));
        EXPECT_EQ(table.entryCount(), 0U) << "容量 0 把表清空";
        EXPECT_EQ(table.sizeByteCount(), 0U);
        EXPECT_EQ(table.insertCount(), insertCountBefore) << "累计插入数不回退（否则两端索引空间会错位）";
        EXPECT_EQ(table.droppedEntryCount(), insertCountBefore);

        // 容量恢复后可以继续插入，新项的绝对索引接着上一次的编号
        ASSERT_TRUE(table.setCapacityByteCount(220, nullptr));
        const std::optional<std::uint64_t> nextIndex = table.insert(QpackHeaderField{"d", ""});
        ASSERT_TRUE(nextIndex.has_value());
        EXPECT_EQ(*nextIndex, insertCountBefore);
    }

    /**
     * @brief 钉住淘汰许可判据：编码器不允许淘汰尚未确认或被未确认头块引用的项（§2.1.1）
     */
    TEST(Qpack, DynamicTableRespectsEvictionPredicate)
    {
        // 96 字节恰好放得下两项 33 字节而放不下第三项：第三项必须靠淘汰表尾才能插入（§3.2.2）
        QpackDynamicTable table(96);
        ASSERT_TRUE(table.insert(QpackHeaderField{"a", ""}).has_value());
        ASSERT_TRUE(table.insert(QpackHeaderField{"b", ""}).has_value());

        const auto neverEvict = [](std::uint64_t) { return false; };
        EXPECT_FALSE(table.insert(QpackHeaderField{"c", ""}, neverEvict)) << "挤不进来自旧项的淘汰就不许插（§2.1.1）";
        EXPECT_EQ(table.entryCount(), 2U) << "被拒的插入不得动过表内容";
        EXPECT_FALSE(table.setCapacityByteCount(0, neverEvict)) << "缩容同样不能淘汰不可淘汰项（§4.3.1）";
        EXPECT_EQ(table.entryCount(), 2U);
    }

    /**
     * @brief 钉住 §4.5.1.1 的取模还原：100 字节的表按 6 取模，10 次插入时 Encoded 4 还原成 9
     */
    TEST(Qpack, RequiredInsertCountRestoresAcrossModuloWrap)
    {
        QpackDecoder decoder(makeDecoderSettings(100, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;

        // 十次插入，名依次是 0..9（每项 1 + 0 + 32 = 33 字节），容量 100 → 表里只留最近三项（绝对索引
        // 7/8/9）。名字取成可辨的字符，才能从解出的项反推还原出的 Required Insert Count。动态表初始容量
        // 为 0，插入前必须先来一条 Set Dynamic Table Capacity=100（§3.2.2、§4.3.1）
        const std::vector<std::uint8_t> instructions = hexToBytes("3f45"
                                                                 "413000 413100 413200 413300 413400"
                                                                 "413500 413600 413700 413800 413900");
        const auto fed = decoder.feedEncoderStream(asSpan(instructions), unblockedStreamIds, controlBytes);
        ASSERT_TRUE(fed.has_value()) << fed.error().message;
        EXPECT_EQ(decoder.insertCount(), 10U);
        EXPECT_EQ(decoder.dynamicTableEntries().size(), 3U);

        // 规范原文算例：Encoded Insert Count = 4 → Required Insert Count = 9
        const std::vector<std::uint8_t> section = hexToBytes("0400"
                                                            "80");
        std::vector<QpackHeaderField> fields;
        controlBytes.clear();
        const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message << "：Base=9、相对索引 0 → 绝对索引 8，且 8 < 9";
        EXPECT_EQ(*result, QpackFieldSectionDecodeStatus::Decoded);
        EXPECT_EQ(fields.size(), 1U);
        EXPECT_EQ(fields[0].name, "8") << "还原值只可能是 9：换成 10 就会指到绝对索引 9 那一项";

        // 同一取值在只收到 3 次插入的本端要还原成 3（离当前插入数最近的那一圈）
        QpackDecoder shallowDecoder(makeDecoderSettings(100, 100));
        std::vector<std::uint64_t> shallowUnblocked;
        std::string shallowControl;
        const std::vector<std::uint8_t> threeInserts = hexToBytes("3f45 413000 413100 413200");
        ASSERT_TRUE(shallowDecoder.feedEncoderStream(asSpan(threeInserts), shallowUnblocked, shallowControl).has_value());
        const std::vector<std::uint8_t> shallowSection = hexToBytes("0400"
                                                                   "80");
        std::vector<QpackHeaderField> shallowFields;
        const auto shallowResult = shallowDecoder.decodeFieldSection(0, asSpan(shallowSection), shallowFields, shallowControl);
        ASSERT_TRUE(shallowResult.has_value()) << shallowResult.error().message << "：RIC=3 时 Base=3，绝对索引 2 仍在表里";
        EXPECT_EQ(shallowFields.size(), 1U);
        EXPECT_EQ(shallowFields[0].name, "2") << "还原成 3 才落在绝对索引 2 上，还原成 9 会因超出本端插入数而挂起";
    }

    /**
     * @brief 钉住容量容不下一项时（MaxEntries=0）Encoded Insert Count 只能为 0
     */
    TEST(Qpack, EncodedInsertCountMustBeZeroWhenTableHoldsNoEntry)
    {
        QpackDecoder decoder(makeDecoderSettings(31, 0));
        const std::vector<std::uint8_t> section = hexToBytes("0100"
                                                            "80");
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;
        const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_FALSE(result.has_value()) << "MaxEntries 为 0 时全Range 为 0：不得做取模，只能把非 0 编码值判错";
        EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        EXPECT_TRUE(containsText(result.error().message, "4.5.1.1")) << result.error().message;
    }

    // ============================================================================
    // 阻塞与解除（§2.1.2、§2.2.1、§2.2.2、§4.4）
    // ============================================================================

    /**
     * @brief 钉住阻塞的完整生命周期：挂起 → 编码器流补齐 → 续解 → 交付后发 Section Ack
     */
    TEST(Qpack, DecoderBlocksStreamThenResumesAfterEncoderStream)
    {
        QpackDecoder decoder(makeDecoderSettings(4096, 4));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;

        // 头块先到：Required Insert Count=1、Base=0、表后索引 0，但插入指令还在路上
        const std::vector<std::uint8_t> section = hexToBytes("0280"
                                                            "10");
        std::vector<QpackHeaderField> fields;
        const auto blocked = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_TRUE(blocked.has_value()) << blocked.error().message;
        EXPECT_EQ(*blocked, QpackFieldSectionDecodeStatus::Blocked);
        EXPECT_TRUE(fields.empty()) << "挂起时不产出字段行";
        EXPECT_EQ(decoder.blockedStreamCount(), 1U);
        EXPECT_TRUE(decoder.hasBlockedStreams());

        // 动态表初始容量为 0，插入得先由 Set Dynamic Table Capacity=4096 打开（§3.2.2、§4.3.1）
        const std::vector<std::uint8_t> encoderStream = hexToBytes("3fe11f"
                                                                   "41610162");
        controlBytes.clear();
        const auto consumed = decoder.feedEncoderStream(asSpan(encoderStream), unblockedStreamIds, controlBytes);
        ASSERT_TRUE(consumed.has_value()) << consumed.error().message;
        ASSERT_EQ(unblockedStreamIds.size(), 1U);
        EXPECT_EQ(unblockedStreamIds[0], 0U) << "表补齐后要报出可续解的流";

        controlBytes.clear();
        const auto resumed = decoder.resumeBlockedFieldSection(0, fields, controlBytes);
        ASSERT_TRUE(resumed.has_value()) << resumed.error().message;
        EXPECT_EQ(*resumed, QpackFieldSectionDecodeStatus::Decoded);
        expectFieldsEqual(fields, {{"a", "b"}}, "续解出的头列表");
        EXPECT_EQ(decoder.blockedStreamCount(), 0U) << "续解后不再挂起";

        const auto delivered = decoder.noteFieldSectionDelivered(0, controlBytes);
        ASSERT_TRUE(delivered.has_value()) << delivered.error().message;
        EXPECT_EQ(toHex(controlBytes), "80") << "Section Ack：模式 '1' + 7 位前缀的 stream=0";
        EXPECT_EQ(decoder.knownReceivedInsertCount(), 1U);
    }

    /**
     * @brief 钉住「没有挂起记录的流不能续解」与「还没补齐时续解仍返回挂起」
     */
    TEST(Qpack, ResumeBlockedFieldSectionReportsStillBlockedWithoutLosingState)
    {
        QpackDecoder decoder(makeDecoderSettings(4096, 4));
        const std::vector<std::uint8_t> section = hexToBytes("0280"
                                                            "10");
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;
        ASSERT_EQ(*decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes), QpackFieldSectionDecodeStatus::Blocked);

        // 表还没补齐就急着续解：仍然是挂起，且挂起记录不能被消耗掉
        const auto early = decoder.resumeBlockedFieldSection(0, fields, controlBytes);
        ASSERT_TRUE(early.has_value()) << early.error().message;
        EXPECT_EQ(*early, QpackFieldSectionDecodeStatus::Blocked);
        EXPECT_EQ(decoder.blockedStreamCount(), 1U) << "未成功的续解不得吃掉挂起的头块";

        const auto missing = decoder.resumeBlockedFieldSection(4, fields, controlBytes);
        ASSERT_FALSE(missing.has_value()) << "没挂起过的流无段可续";
        EXPECT_EQ(missing.error().kind, QpackErrorKind::DecompressionFailed) << missing.error().message;
    }

    /**
     * @brief 钉住 §2.1.2：超过本端承诺的阻塞流数即判 DecompressionFailed，同一条流再挂起不占新名额
     */
    TEST(Qpack, DecoderRejectsMoreBlockedStreamsThanAdvertised)
    {
        QpackDecoder decoder(makeDecoderSettings(4096, 1));
        const std::vector<std::uint8_t> section = hexToBytes("0280"
                                                            "10");
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        EXPECT_EQ(*decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes), QpackFieldSectionDecodeStatus::Blocked);
        const auto secondStream = decoder.decodeFieldSection(4, asSpan(section), fields, controlBytes);
        ASSERT_FALSE(secondStream.has_value()) << "只承诺 1 条阻塞流，第二条必须判错";
        EXPECT_EQ(secondStream.error().kind, QpackErrorKind::DecompressionFailed) << secondStream.error().message;
        EXPECT_EQ(decoder.blockedStreamCount(), 1U) << "判错的那条流不该被记账";

        // 同一条流上的第二段只是排队，不再占一个新名额
        const std::vector<std::uint8_t> another = hexToBytes("0380"
                                                            "10");
        const auto sameStream = decoder.decodeFieldSection(0, asSpan(another), fields, controlBytes);
        ASSERT_TRUE(sameStream.has_value()) << sameStream.error().message;
        EXPECT_EQ(*sameStream, QpackFieldSectionDecodeStatus::Blocked);
        EXPECT_EQ(decoder.blockedStreamCount(), 1U);
    }

    /**
     * @brief 钉住 §4.4.2：本端放弃一条挂起的流时发 Stream Cancellation 并放掉记录
     */
    TEST(Qpack, DecoderStreamCancellationClearsBlockedState)
    {
        QpackDecoder decoder(makeDecoderSettings(4096, 4));
        const std::vector<std::uint8_t> section = hexToBytes("0280"
                                                            "10");
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;
        ASSERT_EQ(*decoder.decodeFieldSection(4, asSpan(section), fields, controlBytes), QpackFieldSectionDecodeStatus::Blocked);

        controlBytes.clear();
        decoder.noteStreamAbandoned(4, controlBytes);
        EXPECT_EQ(toHex(controlBytes), "44") << "模式 '01' + 6 位前缀的 stream=4";
        EXPECT_FALSE(decoder.hasBlockedStreams());
        EXPECT_TRUE(decoder.noteFieldSectionDelivered(4, controlBytes).has_value() == false) << "取消后不再欠确认";
    }

    /**
     * @brief 钉住编码器侧的阻塞名额：名额用尽后只用已确认的项，不再冒险
     */
    TEST(Qpack, EncoderStopsReferencingUnacknowledgedEntriesWhenQuotaIsUsed)
    {
        QpackEncoder encoder(220, 1, 220);
        std::string headerBlock;
        std::string encoderStreamBytes;
        const std::vector<QpackHeaderField> authority = makeFieldList({{":authority", "www.example.com"}});

        // 第一条流：插入并引用，本端承认这条流可能让对端阻塞
        ASSERT_TRUE(encoder.encodeFieldSection(0, std::span<const QpackHeaderField>(authority), headerBlock, encoderStreamBytes)
                        .has_value());
        EXPECT_EQ(toHex(headerBlock), "028010");
        EXPECT_EQ(encoder.blockedStreamCount(), 1U);

        // 第二条流：名额已满 → 不许引用未确认项，也不许新插入，退化成带静态名引用的字面量
        std::string secondHeaderBlock;
        std::string secondEncoderStream;
        ASSERT_TRUE(encoder.encodeFieldSection(4, std::span<const QpackHeaderField>(authority), secondHeaderBlock,
                                               secondEncoderStream).has_value());
        EXPECT_TRUE(toHex(secondEncoderStream).empty()) << "本段没有新的表项要告诉对端：" << toHex(secondEncoderStream);
        EXPECT_EQ(toHex(secondHeaderBlock), "0000" "50" "0f" "7777772e6578616d706c652e636f6d") << "Required Insert Count 为 0";
        EXPECT_EQ(encoder.blockedStreamCount(), 1U) << "第二条流不该新增名额";
        EXPECT_EQ(encoder.insertCount(), 1U) << "不能引用的项也不必插入";

        // 对端确认第一条流后名额归还，第二条流可以正常引用已确认的项
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("80"))).has_value());
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 1U);
        EXPECT_EQ(encoder.blockedStreamCount(), 0U);
        std::string thirdHeaderBlock;
        ASSERT_TRUE(encoder.encodeFieldSection(8, std::span<const QpackHeaderField>(authority), thirdHeaderBlock,
                                               encoderStreamBytes).has_value());
        EXPECT_EQ(toHex(thirdHeaderBlock), "020080") << "Base=1、Required Insert Count=1，相对索引 0 指向绝对索引 0";
        EXPECT_TRUE(encoderStreamBytes.empty()) << "引用已确认的项不需要任何指令";
        EXPECT_EQ(encoder.blockedStreamCount(), 0U) << "引用全在已知计数之内，这条流不会阻塞";
    }

    /**
     * @brief 钉住 §4.4.1 的确认语义：Ack 只认最早一段、无据 Ack 忽略、取消释放全部引用、增量推进已知计数
     */
    TEST(Qpack, EncoderTracksSectionAcknowledgementsAndCancellations)
    {
        QpackEncoder encoder(220, 100, 220);
        std::string headerBlock;
        std::string encoderStreamBytes;
        const std::vector<QpackHeaderField> authority = makeFieldList({{":authority", "www.example.com"}});
        const std::vector<QpackHeaderField> path = makeFieldList({{":path", "/sample/path"}});
        const std::vector<QpackHeaderField> customKey = makeFieldList({{"custom-key", "custom-value"}});

        // 流 4 上两段头块各自插入一项，Required Insert Count 依次是 1 与 2，只占一个阻塞名额；流 8 那段
        // 引用自己新插入的项（Required Insert Count=3），才会在流 4 全部确认后仍然占着名额（§2.1.2）
        ASSERT_TRUE(encoder.encodeFieldSection(4, std::span<const QpackHeaderField>(authority), headerBlock, encoderStreamBytes)
                        .has_value());
        ASSERT_TRUE(encoder.encodeFieldSection(4, std::span<const QpackHeaderField>(path), headerBlock, encoderStreamBytes)
                        .has_value());
        ASSERT_TRUE(encoder.encodeFieldSection(8, std::span<const QpackHeaderField>(customKey), headerBlock, encoderStreamBytes)
                        .has_value());
        EXPECT_EQ(encoder.insertCount(), 3U) << "三段各自插入一项（§2.1.1 允许插入任意字段行）";
        EXPECT_EQ(encoder.blockedStreamCount(), 2U);

        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("84"))).has_value());
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 1U) << "Ack 只认流 4 上最早的那段（§2.2.2.1）";
        EXPECT_EQ(encoder.blockedStreamCount(), 2U) << "流 4 的第二段与流 8 都还没确认";

        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("84"))).has_value());
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 2U);
        EXPECT_EQ(encoder.blockedStreamCount(), 1U) << "流 4 两段都确认后只剩流 8";

        // 放弃流 8：发 Stream Cancellation 并放掉它占的名额，但已知计数不前进
        encoder.noteStreamAbandoned(8, encoderStreamBytes);
        EXPECT_EQ(toHex(encoderStreamBytes), "48");
        EXPECT_EQ(encoder.blockedStreamCount(), 0U) << "取消把流 8 的阻塞名额还回来";
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 2U) << "取消不意味着对端收到了任何插入（§2.2.2.2）";

        // Insert Count Increment 把已知计数推到本端已发出的插入数（§4.4.3）
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("01"))).has_value());
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 3U);

        // 被放弃的流上再来一条 Ack：忽略，不动任何计数，也不把连接判死。本端的收口指令与对端这条 Ack
        // 分属两条独立的单向流、彼此没有先后保证（RFC 9204 §2.1），「Ack 骑在收口之上」与「对端凭空
        // Ack」无从区分，而误判的代价是一整条连接——任何客户端每条连接取消一次请求就能打到
        const std::uint64_t receivedInsertCountBeforeStrayAck = encoder.knownReceivedInsertCount();
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("88"))).has_value()) << "被放弃的流上的 Ack 不该判成协议错误";
        EXPECT_EQ(encoder.knownReceivedInsertCount(), receivedInsertCountBeforeStrayAck) << "忽略一条 Ack 不该动已知计数";
        EXPECT_EQ(encoder.blockedStreamCount(), 0U) << "无据 Ack 不该再造出阻塞名额";

        // 什么都没发过的流同样按忽略处理，且解析游标要照常前进：把「无据 Ack」与「一条合法指令」拼进
        // 同一趟喂进来，后一条必须照样生效——否则就是游标没走、把后面的字节当垃圾重解了一遍
        QpackEncoder strayThenValid(220, 100, 220);
        std::string validHeaderBlock;
        std::string validEncoderStreamBytes;
        ASSERT_TRUE(strayThenValid.encodeFieldSection(4, std::span<const QpackHeaderField>(authority), validHeaderBlock,
                                                     validEncoderStreamBytes)
                        .has_value());
        // 0x90 是 Section Ack(stream=16)（本端从没在这条流上发过头块），0x84 才是刚发出去那段的确认
        const auto strayThenValidConsumed = strayThenValid.feedDecoderStream(asSpan(hexToBytes("9084")));
        ASSERT_TRUE(strayThenValidConsumed.has_value()) << "无据 Ack 之后同趟的合法指令被判坏了";
        EXPECT_EQ(*strayThenValidConsumed, 2U) << "两条指令都要算作本趟消费";
        EXPECT_EQ(strayThenValid.knownReceivedInsertCount(), 1U) << "忽略无据 Ack 之后，后一条合法 Ack 仍要生效";
    }

    /**
     * @brief 钉住 §4.4.3 的判定：增量为 0 或把已知计数推过本端已发插入数都判 DecoderStreamError
     */
    TEST(Qpack, EncoderRejectsInvalidInsertCountIncrements)
    {
        QpackEncoder encoder(220, 100, 220);
        const auto zeroIncrement = encoder.feedDecoderStream(asSpan(hexToBytes("00")));
        ASSERT_FALSE(zeroIncrement.has_value()) << "增量为 0 非法";
        EXPECT_EQ(zeroIncrement.error().kind, QpackErrorKind::DecoderStreamError) << zeroIncrement.error().message;

        const auto tooBig = encoder.feedDecoderStream(asSpan(hexToBytes("0a")));
        ASSERT_FALSE(tooBig.has_value()) << "本端一次插入都没发，对端却说收到 10 次";
        EXPECT_EQ(tooBig.error().kind, QpackErrorKind::DecoderStreamError) << tooBig.error().message;
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 0U) << "判错时不得把已知计数改坏";
    }

    /**
     * @brief 钉住编码器对整段引用释放的判据：确认前不可淘汰的项，确认后才能被新插入挤掉（§2.1.1）
     */
    TEST(Qpack, EncoderCannotEvictEntriesStillReferenced)
    {
        QpackEncoder encoder(106, 100, 106);
        std::string headerBlock;
        std::string encoderStreamBytes;

        // 两项就把表填满：:authority(57) + :path(49)
        const std::vector<QpackHeaderField> first = makeFieldList({{":authority", "www.example.com"},
                                                                 {":path", "/sample/path"}});
        ASSERT_TRUE(encoder.encodeFieldSection(4, std::span<const QpackHeaderField>(first), headerBlock, encoderStreamBytes)
                        .has_value());
        EXPECT_EQ(encoder.dynamicTableSizeByteCount(), 106U);
        encoderStreamBytes.clear();

        // 表满且两项都被未确认头块引用：新项挤不进来，只能退化成字面量
        const std::vector<QpackHeaderField> second = makeFieldList({{"custom-key", "custom-value"}});
        ASSERT_TRUE(encoder.encodeFieldSection(8, std::span<const QpackHeaderField>(second), headerBlock, encoderStreamBytes)
                        .has_value());
        EXPECT_TRUE(encoderStreamBytes.empty()) << "插不进去时不得留下半条插入指令：" << toHex(encoderStreamBytes);
        EXPECT_EQ(encoder.insertCount(), 2U);
        EXPECT_EQ(toHex(headerBlock), "0000" "2703" "637573746f6d2d6b6579" "0c" "637573746f6d2d76616c7565")
            << "双字面量表示：'001' + N=0 + 4 位前缀串的名长 10（3 位前缀满值 7 + 续字节 3）+ 值长 12";

        // 确认掉第一段后两项变得可淘汰，同样的字段这次就能插进表
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("84"))).has_value());
        encoderStreamBytes.clear();
        ASSERT_TRUE(encoder.encodeFieldSection(12, std::span<const QpackHeaderField>(second), headerBlock, encoderStreamBytes)
                        .has_value());
        EXPECT_EQ(toHex(encoderStreamBytes), "4a637573746f6d2d6b65790c637573746f6d2d76616c7565");
        EXPECT_EQ(encoder.insertCount(), 3U);
    }

    // ============================================================================
    // 增量喂字节：指令边界不保证落在包边界上
    // ============================================================================

    /**
     * @brief 钉住解码器吃编码器流时的分片：一条指令拆成单字节喂入，状态不丢、计数只算本趟吃掉的字节
     */
    TEST(Qpack, DecoderFeedsEncoderStreamOneByteAtATime)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        const std::vector<std::uint8_t> instructions = hexToBytes("3fbd01"
                                                                 "c00f 7777772e6578616d706c652e636f6d"
                                                                 "c10c2f73616d706c652f70617468");
        std::size_t totalConsumed = 0;
        for (std::size_t index = 0; index < instructions.size(); ++index)
        {
            std::vector<std::uint64_t> unblockedStreamIds;
            std::string controlBytes;
            const std::span<const std::uint8_t> oneByte(instructions.data() + index, 1);
            const auto consumed = decoder.feedEncoderStream(oneByte, unblockedStreamIds, controlBytes);
            ASSERT_TRUE(consumed.has_value()) << "第 " << index << " 字节被判错：" << consumed.error().message;
            EXPECT_LE(*consumed, 1U) << "一次最多喂了一个字节";
            totalConsumed += *consumed;
            EXPECT_TRUE(unblockedStreamIds.empty());
        }
        // 计数口径是「本趟真正吃掉的输入字节」：凑不齐的指令整条留在内部缓冲里等下趟（§4.2 的编码器流是
        // 无框架字节流），补齐那一趟只按本趟喂入的 1 字节记账，故三条指令各贡献 1，合计 3
        EXPECT_EQ(totalConsumed, 3U) << "每条指令只在补齐的那一趟被记为消费了本趟那一个字节";
        EXPECT_EQ(decoder.insertCount(), 2U);
        EXPECT_EQ(decoder.tableCapacityByteCount(), 220U);
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 106U) << "逐字节喂入解出的表必须与整段喂入完全一致";
    }

    /**
     * @brief 钉住解码器在半条字面量处停住：已解出的指令生效，剩下的字节留下趟
     */
    TEST(Qpack, DecoderKeepsPartialInstructionForNextFeed)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;

        // 容量指令 + 第一条插入完整，第二条插入只给到名字索引
        const std::vector<std::uint8_t> firstHalf = hexToBytes("3fbd01 c00f7777772e6578616d706c652e636f6d c1");
        const auto firstConsumed = decoder.feedEncoderStream(asSpan(firstHalf), unblockedStreamIds, controlBytes);
        ASSERT_TRUE(firstConsumed.has_value()) << firstConsumed.error().message;
        EXPECT_EQ(*firstConsumed, firstHalf.size() - 1U) << "最后那个 0xc1 是半条指令，不该被消费";
        EXPECT_EQ(decoder.insertCount(), 1U);
        EXPECT_EQ(decoder.tableCapacityByteCount(), 220U);

        const std::vector<std::uint8_t> secondHalf = hexToBytes("0c2f73616d706c652f70617468");
        controlBytes.clear();
        const auto secondConsumed = decoder.feedEncoderStream(asSpan(secondHalf), unblockedStreamIds, controlBytes);
        ASSERT_TRUE(secondConsumed.has_value()) << secondConsumed.error().message;
        EXPECT_EQ(*secondConsumed, secondHalf.size()) << "上趟的半个首字节 + 本趟全部正好凑成一条指令";
        EXPECT_EQ(decoder.insertCount(), 2U);
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 106U);
    }

    /**
     * @brief 钉住编码器吃解码器流时的分片：跨趟的整数头字节与多指令同批
     */
    TEST(Qpack, EncoderFeedsDecoderStreamAcrossBoundaries)
    {
        QpackEncoder encoder(4096, 100, 4096);
        std::string headerBlock;
        std::string encoderStreamBytes;
        const std::vector<std::vector<QpackHeaderField>> sections = {
            makeFieldList({{"custom-key", "custom-value"}}),
            makeFieldList({{"custom-key", "custom-value2"}}),
            makeFieldList({{"custom-key", "custom-value3"}}),
        };
        for (std::size_t index = 0; index < sections.size(); ++index)
        {
            const std::uint64_t streamId = index == 2 ? 200 : index * 4;
            ASSERT_TRUE(encoder.encodeFieldSection(streamId, std::span<const QpackHeaderField>(sections[index]), headerBlock,
                                                   encoderStreamBytes).has_value());
        }
        EXPECT_EQ(encoder.insertCount(), 3U);
        EXPECT_EQ(encoder.blockedStreamCount(), 3U);

        auto feedOneByte = [&encoder](std::uint8_t byteValue) -> std::expected<std::size_t, QpackError>
        {
            const std::uint8_t storage = byteValue;
            return encoder.feedDecoderStream(std::span<const std::uint8_t>(&storage, 1));
        };

        // 一整批里最后一条只到一半：前面两条都要生效，消费的字节数只算完整的那两条
        const std::vector<std::uint8_t> batch = hexToBytes("80 84 ff");
        const auto batchConsumed = encoder.feedDecoderStream(asSpan(batch));
        ASSERT_TRUE(batchConsumed.has_value()) << batchConsumed.error().message;
        EXPECT_EQ(*batchConsumed, 2U) << "0xff 是半条 Section Ack，留下趟";
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 2U) << "确认了流 0（RIC=1）与流 4（RIC=2）";
        EXPECT_EQ(encoder.blockedStreamCount(), 1U);

        // Section Ack(stream=200) 的第二个字节：7 位前缀满值 127 + 续字节 73 = 200
        const auto finishing = feedOneByte(0x49);
        ASSERT_TRUE(finishing.has_value()) << finishing.error().message;
        EXPECT_EQ(*finishing, 1U) << "第二趟把整条指令消费完，只算本趟真正吃掉的字节";
        EXPECT_EQ(encoder.knownReceivedInsertCount(), 3U);
        EXPECT_EQ(encoder.blockedStreamCount(), 0U) << "全部确认后阻塞名额归还";

        // 已确认过的流再来一条 Ack：与「无据 Ack」同属一类——本端无从分辨它是重复、错序还是凭空来的，
        // 而误判的代价是一整条连接，故一并忽略且不动任何计数
        const std::uint64_t receivedInsertCountAfterAllAcks = encoder.knownReceivedInsertCount();
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("80"))).has_value()) << "重复的 Section Ack 不该判成协议错误";
        EXPECT_EQ(encoder.knownReceivedInsertCount(), receivedInsertCountAfterAllAcks) << "忽略一条 Ack 不该动已知计数";
        EXPECT_EQ(encoder.blockedStreamCount(), 0U) << "忽略一条 Ack 也不该造出阻塞名额";
    }

    // ============================================================================
    // 指令与表示的细则：Duplicate、Set Capacity、Huffman、N 位
    // ============================================================================

    /**
     * @brief 钉住 §4.3.1：容量可以在编码器流的任意位置出现，但超过本端公布的上限即判 EncoderStreamError
     */
    TEST(Qpack, DecoderAcceptsCapacityChangeAndRejectsItAboveAdvertisedMaximum)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;

        // 容量 220 → 插一项 → 容量 0 清空表 → 容量再回到 220：四条指令一趟吃完
        const std::vector<std::uint8_t> instructions = hexToBytes("3fbd01 4a637573746f6d2d6b65790c637573746f6d2d76616c7565 20 3fbd01");
        const auto consumed = decoder.feedEncoderStream(asSpan(instructions), unblockedStreamIds, controlBytes);
        ASSERT_TRUE(consumed.has_value()) << consumed.error().message;
        EXPECT_EQ(*consumed, instructions.size()) << "容量指令不必打头，中间的变更也照收";
        EXPECT_EQ(decoder.insertCount(), 1U) << "累计插入数不因清空而回退";
        EXPECT_EQ(decoder.dynamicTableEntries().size(), 0U) << "容量 0 把表清空（§3.2.2）";
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 0U);
        EXPECT_EQ(decoder.tableCapacityByteCount(), 220U);

        const auto tooBig = decoder.feedEncoderStream(asSpan(hexToBytes("3fe11f")), unblockedStreamIds, controlBytes);
        ASSERT_FALSE(tooBig.has_value()) << "4096 超过本端公布的 220";
        EXPECT_EQ(tooBig.error().kind, QpackErrorKind::EncoderStreamError) << tooBig.error().message;
        EXPECT_EQ(decoder.tableCapacityByteCount(), 220U) << "被判错的容量不得改动本端状态";
    }

    /**
     * @brief 钉住 §3.2.2：单项大于容量的插入判 EncoderStreamError，而不是悄悄丢项
     */
    TEST(Qpack, DecoderRejectsEntryLargerThanCapacity)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;
        ASSERT_TRUE(decoder.feedEncoderStream(asSpan(hexToBytes("3f01")), unblockedStreamIds, controlBytes).has_value())
            << "先把容量设成 32 字节";
        EXPECT_EQ(decoder.tableCapacityByteCount(), 32U);

        const auto oversized = decoder.feedEncoderStream(asSpan(hexToBytes("416100")), unblockedStreamIds, controlBytes);
        ASSERT_FALSE(oversized.has_value()) << "33 字节的项放不进 32 字节的表";
        EXPECT_EQ(oversized.error().kind, QpackErrorKind::EncoderStreamError) << oversized.error().message;
    }

    /**
     * @brief 钉住 §2.2.3：编码器指令引用已淘汰或不存在的动态表项时判 EncoderStreamError
     */
    TEST(Qpack, DecoderRejectsEncoderInstructionReferencingMissingEntry)
    {
        // 每个用例都用新的解码器：一条指令被判错后本层的表状态已不可信，接着喂只会重复报同一个错
        {
            QpackDecoder emptyTable(makeDecoderSettings(220, 100));
            std::vector<std::uint64_t> unblockedStreamIds;
            std::string controlBytes;
            const auto badDuplicate = emptyTable.feedEncoderStream(asSpan(hexToBytes("02")), unblockedStreamIds, controlBytes);
            ASSERT_FALSE(badDuplicate.has_value()) << "空表上的 Duplicate（相对索引 2）没有来源项";
            EXPECT_EQ(badDuplicate.error().kind, QpackErrorKind::EncoderStreamError) << badDuplicate.error().message;
        }
        {
            QpackDecoder emptyTable(makeDecoderSettings(220, 100));
            std::vector<std::uint64_t> unblockedStreamIds;
            std::string controlBytes;
            const auto badNameReference = emptyTable.feedEncoderStream(asSpan(hexToBytes("8000 00")), unblockedStreamIds, controlBytes);
            ASSERT_FALSE(badNameReference.has_value()) << "插入指令引用了不存在的动态表名";
            EXPECT_EQ(badNameReference.error().kind, QpackErrorKind::EncoderStreamError) << badNameReference.error().message;
        }
        {
            QpackDecoder emptyTable(makeDecoderSettings(220, 100));
            std::vector<std::uint64_t> unblockedStreamIds;
            std::string controlBytes;
            const auto badStaticIndex = emptyTable.feedEncoderStream(asSpan(hexToBytes("ff24 00")), unblockedStreamIds, controlBytes);
            ASSERT_FALSE(badStaticIndex.has_value()) << "静态表只有 99 项，索引 99 非法";
            EXPECT_EQ(badStaticIndex.error().kind, QpackErrorKind::EncoderStreamError) << badStaticIndex.error().message;
        }
        {
            // 引用落在被淘汰过的区间上：相对索引按当前插入数换算，越过表尾即判错（§2.2.3）
            QpackDecoder decoder(makeDecoderSettings(64, 100));
            std::vector<std::uint64_t> unblockedStreamIds;
            std::string controlBytes;
            ASSERT_TRUE(decoder.feedEncoderStream(asSpan(hexToBytes("3f21 40 00 40 00 40 00")), unblockedStreamIds, controlBytes)
                            .has_value()) << "容量 64 里塞进三次空项，最早那次插入的项已被淘汰";
            EXPECT_EQ(decoder.insertCount(), 3U);
            EXPECT_EQ(decoder.dynamicTableEntries().size(), 2U);
            const auto stale = decoder.feedEncoderStream(asSpan(hexToBytes("8200")), unblockedStreamIds, controlBytes);
            ASSERT_FALSE(stale.has_value()) << "相对索引 2 指向绝对索引 0，早就不在表里了";
            EXPECT_EQ(stale.error().kind, QpackErrorKind::EncoderStreamError) << stale.error().message;
        }
    }

    /**
     * @brief 钉住 Huffman 解码路径（§4.1.2 复用 RFC 7541 附录 B 的码表），向量由 pylsqpack 产出
     */
    TEST(Qpack, DecoderReadsHuffmanEncodedLiteralsFromLsqpackVector)
    {
        QpackDecoder decoder(makeDecoderSettings(4096, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;

        // lsqpack 1.3.0 的编码器在容量 4096 的首个流上产出：全静态/字面量 + Huffman 头值
        const std::vector<std::uint8_t> headerBlock = hexToBytes("0000d1518860d5485f2bce9a68d7dd5f508825b650c3cb8170"
                                                                "7f55861c01fb523805");
        std::vector<QpackHeaderField> fields;
        const auto result = decoder.decodeFieldSection(0, asSpan(headerBlock), fields, controlBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        expectFieldsEqual(fields,
                          {{":method", "GET"},
                           {":path", "/index.html"},
                           {":scheme", "https"},
                           {"accept", "*/*"},
                           {"user-agent", "curl/8.0.0"},
                           {"cookie", "a=1; b=2"}},
                          "lsqpack 向量的头列表");
        EXPECT_FALSE(decoder.hasBlockedStreams());
    }

    /**
     * @brief 钉住与真实现互通的另一半：lsqpack 的动态表引用 + 表后索引 + N 位
     */
    TEST(Qpack, DecoderReadsDynamicReferencesFromLsqpackVector)
    {
        QpackDecoder decoder(makeDecoderSettings(4096, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;

        // ls-qpack 的编码器流以容量指令开头：动态表初始容量为 0，没有它就谈不上插入（§3.2.2）
        const std::vector<std::uint8_t> encoderStream = hexToBytes("3fe11f"
                                                                  "c18860d5485f2bce9a68ff208825b650c3cb8170"
                                                                  "7fc5861c01fb523805");
        ASSERT_TRUE(decoder.feedEncoderStream(asSpan(encoderStream), unblockedStreamIds, controlBytes).has_value());
        EXPECT_EQ(decoder.insertCount(), 3U) << "三条插入：:path、user-agent、cookie";

        const std::vector<std::uint8_t> headerBlock = hexToBytes("0482d110d7dd1112");
        std::vector<QpackHeaderField> fields;
        controlBytes.clear();
        const auto result = decoder.decodeFieldSection(4, asSpan(headerBlock), fields, controlBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(*result, QpackFieldSectionDecodeStatus::Decoded) << "Required Insert Count=3、Base=0，全走表后索引";
        expectFieldsEqual(fields,
                          {{":method", "GET"},
                           {":path", "/index.html"},
                           {":scheme", "https"},
                           {"accept", "*/*"},
                           {"user-agent", "curl/8.0.0"},
                           {"cookie", "a=1; b=2"}},
                          "lsqpack 第二段的头列表");
        // 插入指令里也带了 Huffman 编码的字面量名/值，表内容要解压正确
        expectTableEqual(decoder.dynamicTableEntries(),
                         {{"cookie", "a=1; b=2"}, {"user-agent", "curl/8.0.0"}, {":path", "/index.html"}},
                         "lsqpack 三条插入的表内容");
    }

    /**
     * @brief 钉住 N 位（never-indexed）：带名引用的字面量置了 N 位仍要照原样解回（§4.5.4、§7.1.3）
     */
    TEST(Qpack, DecoderAcceptsNeverIndexedLiteralRepresentation)
    {
        QpackDecoder decoder(makeDecoderSettings(0, 0));
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        // 0x71 = '01' + N=1 + T=1 + 静态索引 1（:path），值长度为 0
        const std::vector<std::uint8_t> section = hexToBytes("0000"
                                                            "71"
                                                            "00");
        const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        expectFieldsEqual(fields, {{":path", ""}}, "N 位为 1 的带名引用字面量");
    }

    /**
     * @brief 钉住长度为 0 的字面量在名与值两个位置上都合法（§4.1.2 只要求长度可编 0）
     */
    TEST(Qpack, DecoderAcceptsZeroLengthStringLiterals)
    {
        QpackDecoder decoder(makeDecoderSettings(0, 0));
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        // 双字面量表示：0x20 = '001' + N=0 + H=0 + 名长(3 位前缀)=0，值同样是长度 0
        const std::vector<std::uint8_t> bothEmpty = hexToBytes("0000"
                                                              "20"
                                                              "00");
        const auto result = decoder.decodeFieldSection(0, asSpan(bothEmpty), fields, controlBytes);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        expectFieldsEqual(fields, {{"", ""}}, "名与值都为空的字面量在本层是合法的（语义由 HTTP 层判）");

        // 带静态名引用（索引 0 = :authority）+ 空值：0x50 = '01' + N=0 + T=1 + 索引 0
        const std::vector<std::uint8_t> emptyValue = hexToBytes("0000"
                                                               "50"
                                                               "00");
        std::vector<QpackHeaderField> secondFields;
        const auto secondResult = decoder.decodeFieldSection(4, asSpan(emptyValue), secondFields, controlBytes);
        ASSERT_TRUE(secondResult.has_value()) << secondResult.error().message;
        expectFieldsEqual(secondFields, {{":authority", ""}}, "静态表里 :authority 的值本就是空串");
    }

    /**
     * @brief 钉住 Huffman 的拒绝面：填充位不是全 1、出现 EOS、声明长度越界
     */
    TEST(Qpack, DecoderRejectsMalformedHuffmanStrings)
    {
        QpackDecoder decoder(makeDecoderSettings(0, 0));
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        {
            // 'a'(00011) 之后残留 010：既不是完整码字也不是全 1 填充
            const std::vector<std::uint8_t> section = hexToBytes("0000 51 81 1a");
            const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "填充位不是全 1 必须判错";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        }
        {
            // 30 位全 1 撞上 EOS 码字
            const std::vector<std::uint8_t> section = hexToBytes("0000 51 84 ffffffff");
            const auto result = decoder.decodeFieldSection(4, asSpan(section), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "编码数据里出现 EOS 必须判错";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        }
        {
            // 声明长度 8 字节却只给 1 字节
            const std::vector<std::uint8_t> section = hexToBytes("0000 51 88 ff");
            const auto result = decoder.decodeFieldSection(8, asSpan(section), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "字面量长度越界必须判错";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        }
    }

    /**
     * @brief 钉住字面量长度上限：为永远凑不齐的超长字面量无界等待是攻击面（§7.4）
     */
    TEST(Qpack, DecoderRejectsAbsurdStringDeclarationInsteadOfWaitingForBytes)
    {
        QpackDecoder decoder(makeDecoderSettings(0, 0));
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        // 值长度用 7 位前缀的多字节整数推到 2^62 以上：要么 62 位超限要么本端上限，总之必须立刻判错
        const std::vector<std::uint8_t> section = hexToBytes("0000 51"
                                                            "ff ff ff ff ff ff ff ff ff ff 7f");
        const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_FALSE(result.has_value()) << "超长字面量必须判错，不能当成「还没到齐」";
        EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;

        // 编码器流上的同一形状要判 EncoderStreamError：类别跟着通道走
        QpackDecoder encoderStreamDecoder(makeDecoderSettings(4096, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string decoderStreamBytes;
        const std::vector<std::uint8_t> instructions = hexToBytes("c0"
                                                                  "ff ff ff ff ff ff ff ff ff ff 7f");
        const auto consumed = encoderStreamDecoder.feedEncoderStream(asSpan(instructions), unblockedStreamIds, decoderStreamBytes);
        ASSERT_FALSE(consumed.has_value()) << "字段值长度越界的插入指令";
        EXPECT_EQ(consumed.error().kind, QpackErrorKind::EncoderStreamError) << consumed.error().message;
    }

    /**
     * @brief 钉住截断的头块：整段字节都用完了才承认解不开，不留半截状态
     */
    TEST(Qpack, DecoderRejectsTruncatedFieldSection)
    {
        QpackDecoder decoder(makeDecoderSettings(0, 0));
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        {
            const std::vector<std::uint8_t> noPrefix = hexToBytes("00");
            const auto result = decoder.decodeFieldSection(0, asSpan(noPrefix), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "只有 Required Insert Count 一个字节";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        }
        {
            const std::vector<std::uint8_t> noBase = hexToBytes("00");
            const auto result = decoder.decodeFieldSection(4, asSpan(noBase), fields, controlBytes);
            EXPECT_FALSE(result.has_value()) << "缺 Base 那一字节";
        }
        {
            const std::vector<std::uint8_t> cutLiteral = hexToBytes("0000 510b 2f69");
            const auto result = decoder.decodeFieldSection(8, asSpan(cutLiteral), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "值声明 11 字节只给了 2 字节";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
            EXPECT_TRUE(fields.empty()) << "失败时头列表必须是空的";
        }
    }

    /**
     * @brief 钉住头块索引的越界面：静态索引超 99、相对索引不小于 Base、引用不小于 Required Insert Count
     */
    TEST(Qpack, DecoderRejectsOutOfRangeIndexes)
    {
        QpackDecoder decoder(makeDecoderSettings(220, 100));
        std::vector<std::uint64_t> unblockedStreamIds;
        std::string controlBytes;
        feedAppendixB2Instructions(decoder);

        {
            // 静态表索引 99（0xdf 后跟 0x24）越界
            const std::vector<std::uint8_t> section = hexToBytes("0000 df24");
            std::vector<QpackHeaderField> fields;
            const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "静态表只有 99 项";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        }
        {
            // Base=1 时相对索引 1 会算出负的绝对索引
            const std::vector<std::uint8_t> section = hexToBytes("0280 81");
            std::vector<QpackHeaderField> fields;
            const auto result = decoder.decodeFieldSection(4, asSpan(section), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "相对索引不小于 Base";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        }
        {
            // Required Insert Count=1、Base=2：相对索引 0 指向绝对索引 1，不小于 RIC
            const std::vector<std::uint8_t> section = hexToBytes("0201 80");
            std::vector<QpackHeaderField> fields;
            const auto result = decoder.decodeFieldSection(8, asSpan(section), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "引用的表项必须严格小于本段声明的 Required Insert Count（§2.2.3）";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        }
        {
            // 符号位为 1 且 Delta Base 不小于 Required Insert Count：Base 会为负（§4.5.1.2）
            const std::vector<std::uint8_t> section = hexToBytes("0281 10");
            std::vector<QpackHeaderField> fields;
            const auto result = decoder.decodeFieldSection(12, asSpan(section), fields, controlBytes);
            ASSERT_FALSE(result.has_value()) << "Required Insert Count=1、Delta Base=1 → Base 为负";
            EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
            EXPECT_TRUE(containsText(result.error().message, "4.5.1.2")) << result.error().message;
        }
    }

    /**
     * @brief 钉住 §4.5.1.1 的还原失败：Encoded Insert Count 超过 2×MaxEntries 即判错
     */
    TEST(Qpack, DecoderRejectsImpossibleEncodedInsertCount)
    {
        QpackDecoder decoder(makeDecoderSettings(32, 100));
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        // MaxEntries=1 → FullRange=2；Encoded Insert Count 3 不可能由合规编码器产生
        const std::vector<std::uint8_t> section = hexToBytes("0300 80");
        const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, QpackErrorKind::DecompressionFailed) << result.error().message;
        EXPECT_TRUE(containsText(result.error().message, "可表示范围")) << result.error().message;
    }

    /**
     * @brief 钉住 SETTINGS_MAX_FIELD_SECTION_SIZE 的约束：按解码后的大小判，超限归为过量负载（RFC 9114 §4.2.2）
     */
    TEST(Qpack, DecoderRejectsFieldSectionAboveAdvertisedSize)
    {
        QpackDecoder decoder(makeDecoderSettings(0, 0, 40));
        std::vector<QpackHeaderField> fields;
        std::string controlBytes;

        const std::vector<std::uint8_t> section = hexToBytes("0000"
                                                            "510b 2f69 6e64 6578 2e68 746d 6c");
        const auto result = decoder.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_FALSE(result.has_value()) << ":path + /index.html + 32 = 48 字节，超过 40 字节的约束";
        EXPECT_EQ(result.error().kind, QpackErrorKind::FieldSectionTooLarge) << result.error().message;
        EXPECT_EQ(toHttp3ErrorCode(result.error().kind), Http3ErrorCode::ExcessiveLoad);
        EXPECT_TRUE(fields.empty());

        // 上限为 0 表示不限（RFC 9114 §7.2.4.1 的默认取值）
        QpackDecoder unlimited(makeDecoderSettings(0, 0, 0));
        const auto unlimitedResult = unlimited.decodeFieldSection(0, asSpan(section), fields, controlBytes);
        ASSERT_TRUE(unlimitedResult.has_value()) << unlimitedResult.error().message;
        EXPECT_EQ(fields.size(), 1U);
    }

    /**
     * @brief 钉住 §3.2.3 与 §4.3.1 的本端一侧：容量超过对端上限不静默收窄，直接失败且不产出字节
     */
    TEST(Qpack, EncoderRejectsCapacityAbovePeerMaximum)
    {
        QpackEncoder encoder(220, 100, 4096);
        std::string headerBlock;
        std::string encoderStreamBytes;
        const std::vector<QpackHeaderField> fieldLines = makeFieldList({{":path", "/index.html"}});

        const auto result = encoder.encodeFieldSection(0, std::span<const QpackHeaderField>(fieldLines), headerBlock,
                                                      encoderStreamBytes);
        ASSERT_FALSE(result.has_value()) << "要求的容量超过对端上限时不得偷偷降到 220";
        EXPECT_EQ(result.error().kind, QpackErrorKind::InvalidLocalState) << result.error().message;
        EXPECT_TRUE(headerBlock.empty()) << "失败时不留半截头块";
        EXPECT_TRUE(encoderStreamBytes.empty());

        std::string capacityBytes;
        const auto raised = encoder.setMaximumTableCapacityByteCount(221, capacityBytes);
        ASSERT_FALSE(raised.has_value()) << "运行期改容量同样受对端上限约束";
        EXPECT_EQ(raised.error().kind, QpackErrorKind::InvalidLocalState) << raised.error().message;
        EXPECT_TRUE(capacityBytes.empty());
    }

    /**
     * @brief 钉住 §4.3.1 的「不得淘汰不可淘汰项」：缩容会把被引用项挤掉时本次变更整体失败
     */
    TEST(Qpack, EncoderRejectsCapacityChangeThatWouldEvictReferencedEntries)
    {
        QpackEncoder encoder(220, 100, 220);
        std::string headerBlock;
        std::string encoderStreamBytes;
        ASSERT_TRUE(encodeAppendixB2FieldSection(encoder).size() > 0);
        EXPECT_EQ(encoder.dynamicTableSizeByteCount(), 106U);

        encoderStreamBytes.clear();
        const auto shrink = encoder.setMaximumTableCapacityByteCount(50, encoderStreamBytes);
        ASSERT_FALSE(shrink.has_value()) << "两项都还被未确认的头块引用";
        EXPECT_EQ(shrink.error().kind, QpackErrorKind::InvalidLocalState) << shrink.error().message;
        EXPECT_EQ(encoder.tableCapacityByteCount(), 220U) << "失败的变更不得改坏生效容量";
        EXPECT_TRUE(encoderStreamBytes.empty()) << "失败时不得写出容量指令";

        // 确认掉那一段之后，同一个缩容就允许了
        ASSERT_TRUE(encoder.feedDecoderStream(asSpan(hexToBytes("84"))).has_value());
        encoderStreamBytes.clear();
        const auto allowed = encoder.setMaximumTableCapacityByteCount(50, encoderStreamBytes);
        ASSERT_TRUE(allowed.has_value()) << allowed.error().message;
        EXPECT_EQ(toHex(encoderStreamBytes), "3f13") << "容量 50 用 5 位前缀要两个字节";
        EXPECT_EQ(encoder.dynamicTableSizeByteCount(), 49U) << "只剩 :path 那一项，:authority 被挤掉";
        EXPECT_EQ(encoder.dynamicTableEntries().size(), 1U);
    }

    /**
     * @brief 钉住对端上限为 0 时的静默：容量保持 0 不产生任何指令，也不插任何项
     */
    TEST(Qpack, EncoderStaysSilentWhenPeerDisablesDynamicTable)
    {
        QpackEncoder encoder(0, 0, 0);
        std::string encoderStreamBytes;
        ASSERT_TRUE(encoder.setMaximumTableCapacityByteCount(0, encoderStreamBytes).has_value()) << "本端要的容量与对端上限都是 0";
        EXPECT_TRUE(encoderStreamBytes.empty()) << "初始容量本就是 0，无需通告（§3.2.2）";

        std::string headerBlock;
        const std::vector<QpackHeaderField> fieldLines = makeFieldList({{"custom-key", "custom-value"},
                                                                        {":path", "/"}});
        ASSERT_TRUE(encoder.encodeFieldSection(0, std::span<const QpackHeaderField>(fieldLines), headerBlock, encoderStreamBytes)
                        .has_value());
        EXPECT_TRUE(encoderStreamBytes.empty()) << "§3.2.3：对端上限为 0 时不得发任何编码器流指令";
        EXPECT_EQ(encoder.insertCount(), 0U);
        EXPECT_EQ(toHex(headerBlock), "0000" "2703" "637573746f6d2d6b6579" "0c" "637573746f6d2d76616c7565" "c1");
    }

    // ============================================================================
    // 往返性质（不作为字节向量）
    // ============================================================================

    /**
     * @brief 性质检查：本端编码的字节在「头块先到、指令后到」的最坏顺序下仍能被本端解回
     * @details 这一条不钉任何固定字节，只验证阻塞—解除—确认这条状态机在自己的字节上闭合；
     *          线上互通由附录 B 与 lsqpack 的向量负责。
     */
    TEST(Qpack, RoundTripSurvivesOutOfOrderDelivery)
    {
        QpackEncoder encoder(4096, 8, 4096);
        QpackDecoder decoder(makeDecoderSettings(4096, 8));

        std::string headerBlock;
        std::string capacityBytes;
        ASSERT_TRUE(encoder.setMaximumTableCapacityByteCount(4096, capacityBytes).has_value());
        std::string accumulatedInstructions = capacityBytes;

        std::vector<std::vector<QpackHeaderField>> expectedSections;
        std::vector<std::string> headerBlocks;
        const std::vector<std::vector<FieldListEntry>> sections = {
            {{":method", "POST"}, {":path", "/submit"}, {"content-type", "application/json"}},
            {{":method", "POST"}, {":path", "/submit"}, {"content-type", "application/json"}, {"x-request-id", "0123456789abcdef"}},
            {{":status", "200"}, {"content-type", "application/json"}, {"x-request-id", "0123456789abcdef"}},
        };
        for (std::size_t index = 0; index < sections.size(); ++index)
        {
            std::vector<QpackHeaderField> fieldLines = makeFieldList(sections[index]);
            std::string localEncoderStream;
            const auto encoded = encoder.encodeFieldSection(index * 4, std::span<const QpackHeaderField>(fieldLines), headerBlock,
                                                           localEncoderStream);
            ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
            expectedSections.push_back(std::move(fieldLines));
            headerBlocks.push_back(headerBlock);
            accumulatedInstructions += localEncoderStream;
        }
        EXPECT_GT(encoder.blockedStreamCount(), 0U) << "引用了对端还没收到的表项，这些流都可能阻塞";

        // 最坏顺序：三段头块都先于编码器流指令到达
        std::vector<std::uint64_t> blockedStreamIds;
        std::string decoderStreamBytes;
        for (std::size_t index = 0; index < headerBlocks.size(); ++index)
        {
            const std::vector<std::uint8_t> sectionBytes(headerBlocks[index].begin(), headerBlocks[index].end());
            std::vector<QpackHeaderField> fields;
            const auto result = decoder.decodeFieldSection(index * 4, asSpan(sectionBytes), fields, decoderStreamBytes);
            ASSERT_TRUE(result.has_value()) << result.error().message;
            EXPECT_EQ(*result, QpackFieldSectionDecodeStatus::Blocked) << "第 " << index << " 段引用的表项还没收到";
            EXPECT_TRUE(fields.empty()) << "挂起时不产出字段行";
            blockedStreamIds.push_back(index * 4);
        }
        EXPECT_EQ(decoder.blockedStreamCount(), blockedStreamIds.size());

        // 编码器流补齐后，报出的可续解流要能一一解回原始头列表
        const std::vector<std::uint8_t> instructionBytes(accumulatedInstructions.begin(), accumulatedInstructions.end());
        std::vector<std::uint64_t> unblockedStreamIds;
        decoderStreamBytes.clear();
        const auto fed = decoder.feedEncoderStream(asSpan(instructionBytes), unblockedStreamIds, decoderStreamBytes);
        ASSERT_TRUE(fed.has_value()) << fed.error().message;
        EXPECT_EQ(unblockedStreamIds.size(), blockedStreamIds.size()) << "三条流都该被报出来";

        for (const std::uint64_t streamId : blockedStreamIds)
        {
            std::vector<QpackHeaderField> fields;
            decoderStreamBytes.clear();
            const auto resumed = decoder.resumeBlockedFieldSection(streamId, fields, decoderStreamBytes);
            ASSERT_TRUE(resumed.has_value()) << resumed.error().message;
            ASSERT_EQ(*resumed, QpackFieldSectionDecodeStatus::Decoded) << "补齐后不该仍然挂起";
            expectFieldsEqual(fields, sections[streamId / 4], "流 " + std::to_string(streamId) + " 续解出的头列表");

            // 整段交给上层之后才发 Section Ack，并把解码器流字节送回编码器
            const auto delivered = decoder.noteFieldSectionDelivered(streamId, decoderStreamBytes);
            ASSERT_TRUE(delivered.has_value()) << delivered.error().message;
            const std::vector<std::uint8_t> acknowledgementBytes(decoderStreamBytes.begin(), decoderStreamBytes.end());
            ASSERT_FALSE(acknowledgementBytes.empty()) << "每段都要回一条 Section Ack";
            const auto fedBack = encoder.feedDecoderStream(asSpan(acknowledgementBytes));
            ASSERT_TRUE(fedBack.has_value()) << fedBack.error().message;
            EXPECT_EQ(*fedBack, acknowledgementBytes.size());
        }

        EXPECT_FALSE(decoder.hasBlockedStreams());
        EXPECT_EQ(encoder.blockedStreamCount(), 0U) << "确认后阻塞名额全部归还";
        EXPECT_EQ(encoder.knownReceivedInsertCount(), encoder.insertCount()) << "三段都确认后，已知接收计数应追上本端插入数";
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), encoder.dynamicTableSizeByteCount()) << "两端的表必须长到同一个大小";
    }

    /**
     * @brief 编一段响应头块要碰几次堆
     * @details 编码原先要新建四份中间结果（字段行表示、两字节前缀、编码器流指令、引用清单），
     *          再把它们抄进调用方给的两条串；改动前实测一段 83 字节的响应头块要 19 次分配 / 481 字节
     *          （MSVC 调试版；GCC 侧 3 次 / 276 字节，它的短缓冲吃掉了一部分临时串），而这是每条响应
     *          都要付的固定成本。判据打在「中间结果按线程复用、调用方缓冲也复用」这一侧：归零才说明
     *          编码过程自己没有留中间容器（两侧平台上改动前的同一条用例都读得出不为零）。
     */
    TEST(QpackAllocations, EncodesResponseHeaderSectionIntoReusedBuffersWithoutAllocating)
    {
        QpackEncoder encoder(0, 0, 0);
        const std::vector<QpackHeaderField> fieldLines = makeFieldList({
            {":status", "200"},
            {"content-type", "application/json"},
            {"content-length", "1024"},
            {"date", "Tue, 23 Sep 2025 10:00:00 GMT"},
            {"server", "AsynGyanis"},
            {"x-trace-id", "0f1e2d3c4b5a6978"},
        });

        std::string headerBlock;
        std::string encoderStreamBytes;
        ASSERT_TRUE(encoder.encodeFieldSection(0U, std::span<const QpackHeaderField>(fieldLines), headerBlock, encoderStreamBytes)
                        .has_value());
        const std::size_t blockByteCount = headerBlock.size();
        ASSERT_GT(blockByteCount, 40U) << "这段短到看不出扩容代价，读数量的不是被测形状";

        const auto encodeOnce = [&encoder, &fieldLines, &headerBlock, &encoderStreamBytes]() -> std::size_t
        {
            return encoder.encodeFieldSection(0U, std::span<const QpackHeaderField>(fieldLines), headerBlock, encoderStreamBytes)
                       ? headerBlock.size()
                       : 0U;
        };

        resetAllocationHistogram();
        const AllocationProfile profile = measurePerOperation(encodeOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * blockByteCount) << "有一千次没编出同一段头块，读数不可信";
        std::printf("quic h3 编一段 %zu 字节的响应头块：每次 %llu 次分配 / %llu 字节\n", blockByteCount,
                    static_cast<unsigned long long>(profile.totalAllocations / kMeasurementIterations),
                    static_cast<unsigned long long>(profile.totalBytes / kMeasurementIterations));
        const AllocationHistogram histogram = snapshotAllocationHistogram();
        for (std::size_t bucket = 0; bucket < histogram.size(); ++bucket)
        {
            if (histogram[bucket] != 0)
            {
                std::printf("  桶 %zu-%zu 字节：一千段合计 %llu 次\n", bucket * 16U, bucket * 16U + 15U,
                            static_cast<unsigned long long>(histogram[bucket]));
            }
        }
        EXPECT_EQ(profile.totalAllocations, 0ULL)
                << "编一段响应头块仍在碰堆：读数为每次 "
                << static_cast<unsigned long long>(profile.totalAllocations / kMeasurementIterations) << " 次";
    }

    /**
     * @brief 解一段请求头块要碰几次堆
     * @details 解出来的字段行是 owning 串，交付进请求的头部存储后整份作废，故解码器自持一份复用槽位、
     *          交付时逐条抄进调用方缓冲（整块交换会把对方的容量带走）。头部条数由对端决定，本端只对
     *          「同一形状的重复请求」这一最常见形状负责零分配。取关掉动态表的段：不登记、不待确认。
     */
    TEST(QpackAllocations, DecodesRequestHeaderSectionIntoReusedBuffersWithoutAllocating)
    {
        QpackEncoder encoder(0, 0, 0);
        QpackDecoder decoder(makeDecoderSettings(0, 0));
        const std::vector<QpackHeaderField> requestLines = makeFieldList({
            {":method", "POST"},
            {":scheme", "https"},
            {":path", "/api/v1/orders/12345?page=2"},
            {":authority", "api.example.com"},
            {"user-agent", "AsynGyanisH3Client/1.0"},
            {"accept", "application/json;charset=utf-8"},
            {"content-type", "application/json"},
            {"x-request-id", "0f1e2d3c4b5a69789abcdef"},
        });
        std::string encoded;
        std::string instructions;
        ASSERT_TRUE(encoder.encodeFieldSection(0U, std::span<const QpackHeaderField>(requestLines), encoded, instructions)
                        .has_value());
        ASSERT_TRUE(instructions.empty()) << "这台形状不该产生编码器流指令，否则读的是动态表路径";
        const std::vector<std::uint8_t> fieldSection(encoded.begin(), encoded.end());

        std::vector<QpackHeaderField> fields;
        std::string decoderStreamBytes;
        const auto decodeOnce = [&decoder, &fieldSection, &fields, &decoderStreamBytes]() -> std::size_t
        {
            const auto decoded = decoder.decodeFieldSection(0U, std::span<const std::uint8_t>(fieldSection), fields,
                                                            decoderStreamBytes);
            return decoded.has_value() && *decoded == QpackFieldSectionDecodeStatus::Decoded ? fields.size() : 0U;
        };

        ASSERT_EQ(decodeOnce(), requestLines.size()) << "第一条就没解全，稳态无从谈起";
        resetAllocationHistogram();
        const AllocationProfile profile = measurePerOperation(decodeOnce);
        std::printf("quic h3 解一段 %zu 条字段的请求头块：每次 %llu 次分配 / %llu 字节\n", requestLines.size(),
                    static_cast<unsigned long long>(profile.totalAllocations / kMeasurementIterations),
                    static_cast<unsigned long long>(profile.totalBytes / kMeasurementIterations));
        const AllocationHistogram histogram = snapshotAllocationHistogram();
        for (std::size_t bucket = 0; bucket < histogram.size(); ++bucket)
        {
            if (histogram[bucket] != 0)
            {
                std::printf("  桶 %zu-%zu 字节：一千段合计 %llu 次\n", bucket * 16U, bucket * 16U + 15U,
                            static_cast<unsigned long long>(histogram[bucket]));
            }
        }
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * requestLines.size()) << "有一千段没解出全部字段，读数不可信";
        // 准入线取「每条字段行不超过 4 次分配」：GCC 与 MSVC 发布版这里读到每次 0 次；MSVC 调试版
        // （_ITERATOR_DEBUG_LEVEL=2）下每个局部 std::string 都要一份 16 字节的迭代器调试代理，而每行
        // 要经过三个带错误出参的解析助手，实测每次 3 条 * 字段行数。交付点若改成临时量再移动，
        // 同一形状实测涨到每次 45 次，当场越过这条线。
        EXPECT_LE(profile.totalAllocations, kMeasurementIterations * requestLines.size() * 4U)
                << "解一段请求头块的逐条分配超线：读数为每次 "
                << static_cast<unsigned long long>(profile.totalAllocations / kMeasurementIterations) << " 次";
    }
} // namespace AsynGyanis::Net
