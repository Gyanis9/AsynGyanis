// TestHpack.cpp —— HPACK（RFC 7541）的单元测试
//
// 用例以规范自带的黄金向量为主：Appendix C.1.1/C.1.2/C.1.3 的整数表示、C.2.1–C.2.4 的四种表示、
// C.3.1–C.3.3（不带 Huffman 的请求头）、C.4.1–C.4.3（带 Huffman 的请求头，逐字节对照）、
// C.5.1–C.5.3 与 C.6.1–C.6.3（响应头，动态表上限 256 字节，含驱逐与表大小演进）——每节的字节都按
// 规范原文的 hex dump 抄录，解出的头列表与动态表状态也逐条对照规范给出的结果表。
// 自造用例只补规范没给的部分：Huffman 码表的完整性与全 256 个符号的往返、Huffman 的拒绝面
// （EOS、填充越界）、动态表大小更新的位置与上限、各类资源上限、粘滞错误态与出参契约。
// 用例都是纯计算，不起网络、不依赖任何外部服务。

#include "Net/Http2/Hpack.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 7541 各类示例里的头列表都按「名 = 值」的二元组表述
        using HeaderListEntry = std::pair<const char *, const char *>;

        /**
         * @brief 判断文本里是否出现指定子串（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::containsText;

        /**
         * @brief 取一个十六进制字符的数值
         * @param hexDigit 字符
         * @return int 0..15；不是十六进制字符时返回 -1
         */
        int hexDigitValue(const char hexDigit)
        {
            if (hexDigit >= '0' && hexDigit <= '9')
            {
                return hexDigit - '0';
            }
            if (hexDigit >= 'a' && hexDigit <= 'f')
            {
                return hexDigit - 'a' + 10;
            }
            if (hexDigit >= 'A' && hexDigit <= 'F')
            {
                return hexDigit - 'A' + 10;
            }
            return -1;
        }

        /**
         * @brief 由十六进制串拼出字节序列
         * @details RFC 7541 Appendix C 的示例都以 hex dump 给出，直接抄成十六进制串比逐个字节写
         *          成数字更难抄错，也便于与规范原文逐字对照。
         * @param hexDigits 十六进制串，字符数必须是偶数
         * @return std::string 字节序列；遇到非法字符时返回已解出的前缀并记一次失败
         */
        std::string makeBytesFromHex(const std::string_view hexDigits)
        {
            EXPECT_EQ(hexDigits.size() % 2, 0U) << "十六进制串必须是偶数个字符：" << hexDigits;
            std::string bytes;
            bytes.reserve(hexDigits.size() / 2);
            for (std::size_t index = 0; index + 1 < hexDigits.size(); index += 2)
            {
                const int highValue = hexDigitValue(hexDigits[index]);
                const int lowValue = hexDigitValue(hexDigits[index + 1]);
                EXPECT_GE(highValue, 0) << "非法十六进制字符：" << hexDigits[index];
                EXPECT_GE(lowValue, 0) << "非法十六进制字符：" << hexDigits[index + 1];
                if (highValue < 0 || lowValue < 0)
                {
                    return bytes;
                }
                bytes.push_back(static_cast<char>(static_cast<unsigned>(highValue * 16 + lowValue)));
            }
            return bytes;
        }

        /**
         * @brief 造一条「带增量索引的字面量」表示（RFC 7541 §6.2.1），名与值都用字面量
         * @details 名与值的长度都要小于 128 字节，因此长度各占一字节、且不置 Huffman 标志位。
         * @param name 头名
         * @param value 头值
         * @return std::string 该表示的字节
         */
        std::string makeIncrementalLiteralField(const std::string_view name, const std::string_view value)
        {
            EXPECT_LT(name.size(), 128U) << "本助手不编码多位长度前缀";
            EXPECT_LT(value.size(), 128U) << "本助手不编码多位长度前缀";
            std::string bytes;
            bytes.push_back(static_cast<char>(0x40));
            bytes.push_back(static_cast<char>(name.size()));
            bytes += name;
            bytes.push_back(static_cast<char>(value.size()));
            bytes += value;
            return bytes;
        }

        /**
         * @brief 由「名 = 值」二元组拼出期望的头列表
         * @param entries 头部条目
         * @return std::vector<HpackHeaderField> 期望值
         */
        std::vector<HpackHeaderField> makeHeaderList(const std::initializer_list<HeaderListEntry> &entries)
        {
            std::vector<HpackHeaderField> headerFields;
            headerFields.reserve(entries.size());
            for (const HeaderListEntry &entry: entries)
            {
                headerFields.push_back(HpackHeaderField{entry.first, entry.second});
            }
            return headerFields;
        }

        /**
         * @brief 断言解出的头列表与规范给出的结果逐条一致（含顺序）
         * @param actual 实际解出的头列表
         * @param expected 规范给出的头列表
         */
        void expectHeaderListEquals(const std::vector<HpackHeaderField> &actual,
                                    const std::initializer_list<HeaderListEntry> &expected)
        {
            const std::vector<HpackHeaderField> expectedFields = makeHeaderList(expected);
            ASSERT_EQ(actual.size(), expectedFields.size()) << "头列表条数不符";
            for (std::size_t index = 0; index < expectedFields.size(); ++index)
            {
                EXPECT_EQ(actual[index].name, expectedFields[index].name) << "第 " << index << " 条头名";
                EXPECT_EQ(actual[index].value, expectedFields[index].value) << "第 " << index << " 条头值";
            }
        }

        /**
         * @brief 断言动态表内容与表大小都与规范给出的结果一致
         * @param entries 实际条目（下标 0 是最新插入的一项，对应索引空间里的 62）
         * @param sizeByteCount 实际表大小
         * @param expectedEntries 规范给出的条目顺序
         * @param expectedSizeByteCount 规范给出的表大小
         */
        void expectDynamicTableEquals(const std::deque<HpackHeaderField> &entries, const std::size_t sizeByteCount,
                                      const std::initializer_list<HeaderListEntry> &expectedEntries,
                                      const std::size_t expectedSizeByteCount)
        {
            const std::vector<HpackHeaderField> expectedFields = makeHeaderList(expectedEntries);
            ASSERT_EQ(entries.size(), expectedFields.size()) << "动态表条目数不符";
            for (std::size_t index = 0; index < expectedFields.size(); ++index)
            {
                EXPECT_EQ(entries[index].name, expectedFields[index].name) << "动态表第 " << index + 1 << " 项的头名";
                EXPECT_EQ(entries[index].value, expectedFields[index].value) << "动态表第 " << index + 1 << " 项的头值";
            }
            EXPECT_EQ(sizeByteCount, expectedSizeByteCount) << "动态表大小不符（RFC 7541 §4.1 的算式）";
        }

        /**
         * @brief 按 RFC 7541 §4.1 的算式从期望条目推出表大小，再断言动态表与之一致
         * @details 自造向量没有规范给出的表大小可比，改用算式推导：名长 + 值长 + 32 逐项累加，
         *          避免在用例里手算总和抄错。这条入口直接收 std::string 组成的条目，因为二进制头值
         *          写成字符串字面量会被 NUL 截断。
         * @param entries 实际条目
         * @param sizeByteCount 实际表大小
         * @param expectedFields 期望条目顺序
         */
        void expectDynamicTableFieldsMatch(const std::deque<HpackHeaderField> &entries, const std::size_t sizeByteCount,
                                          const std::vector<HpackHeaderField> &expectedFields)
        {
            std::size_t expectedSizeByteCount = 0;
            for (const HpackHeaderField &field: expectedFields)
            {
                expectedSizeByteCount += field.name.size() + field.value.size() + kHpackDynamicTableEntryOverheadBytes;
            }

            ASSERT_EQ(entries.size(), expectedFields.size()) << "动态表条目数不符";
            for (std::size_t index = 0; index < expectedFields.size(); ++index)
            {
                EXPECT_EQ(entries[index].name, expectedFields[index].name) << "动态表第 " << index + 1 << " 项的头名";
                EXPECT_EQ(entries[index].value, expectedFields[index].value) << "动态表第 " << index + 1 << " 项的头值";
            }
            EXPECT_EQ(sizeByteCount, expectedSizeByteCount) << "动态表大小不符（RFC 7541 §4.1 的算式）";
        }

        /**
         * @brief 断言头块解不开，返回中文原因
         * @param decoder 解码器
         * @param headerBlock 头块字节
         * @param expectedKind 期望的失败类别
         * @return std::string 错误文案，已断言非空
         */
        std::string expectDecodeFailure(HpackDecoder &decoder, const std::string_view headerBlock,
                                        const HpackErrorKind expectedKind)
        {
            std::vector<HpackHeaderField> headerFields;
            std::string reason;
            EXPECT_FALSE(decoder.decode(headerBlock, headerFields, &reason)) << "这个头块本应解不开";
            EXPECT_EQ(decoder.errorKind(), expectedKind);
            // 只有压缩上下文出错才粘滞：越限的头块整块解完，两端的表仍然同步，下一个头块照常能解
            EXPECT_EQ(decoder.hasError(), expectedKind == HpackErrorKind::CompressionError);
            EXPECT_EQ(decoder.isLimitExceeded(), expectedKind == HpackErrorKind::LimitExceeded);
            EXPECT_FALSE(reason.empty()) << "失败必须给出可排查的原因";
            EXPECT_EQ(reason, decoder.errorMessage()) << "出参与 errorMessage() 必须是同一份原因";
            return reason;
        }

        /**
         * @brief 用码表把一段字节编成 Huffman 位流（用例侧的编码器）
         * @details 与解码器共用同一张表，但打包方向由这里决定：码字从高位到低位写出、最后不足一字节
         *          的位用全 1 填充（RFC 7541 §5.2）。解码器若把位序弄反，这条往返用例必然失败。
         * @param text 待编码字节
         * @return std::string Huffman 编码结果
         */
        std::string encodeHuffmanWithCodeTable(const std::string_view text)
        {
            std::string encoded;
            std::uint64_t bitBuffer = 0;
            int bufferedBitCount = 0;
            for (const char character: text)
            {
                const HpackHuffmanCode &code = kHpackHuffmanCodeTable[static_cast<unsigned char>(character)];
                for (int bitIndex = static_cast<int>(code.bitCount) - 1; bitIndex >= 0; --bitIndex)
                {
                    bitBuffer = (bitBuffer << 1) | ((code.code >> bitIndex) & 1U);
                    ++bufferedBitCount;
                    if (bufferedBitCount == 8)
                    {
                        encoded.push_back(static_cast<char>(bitBuffer & 0xFFU));
                        bitBuffer = 0;
                        bufferedBitCount = 0;
                    }
                }
            }
            if (bufferedBitCount > 0)
            {
                // 填充取自 EOS 码字的高位，也就是若干个 1
                const int paddingBitCount = 8 - bufferedBitCount;
                bitBuffer = (bitBuffer << paddingBitCount) | ((1U << paddingBitCount) - 1U);
                encoded.push_back(static_cast<char>(bitBuffer & 0xFFU));
            }
            return encoded;
        }

        /// RFC 7541 C.3.1/C.4.1 第一个请求的头列表（两个示例共用）
        const std::initializer_list<HeaderListEntry> kFirstRequestHeaders{{":method", "GET"},
                                                                          {":scheme", "http"},
                                                                          {":path", "/"},
                                                                          {":authority", "www.example.com"}};

        /// RFC 7541 C.3.2/C.4.2 第二个请求的头列表
        const std::initializer_list<HeaderListEntry> kSecondRequestHeaders{{":method", "GET"},
                                                                           {":scheme", "http"},
                                                                           {":path", "/"},
                                                                           {":authority", "www.example.com"},
                                                                           {"cache-control", "no-cache"}};

        /// RFC 7541 C.3.3/C.4.3 第三个请求的头列表
        const std::initializer_list<HeaderListEntry> kThirdRequestHeaders{{":method", "GET"},
                                                                          {":scheme", "https"},
                                                                          {":path", "/index.html"},
                                                                          {":authority", "www.example.com"},
                                                                          {"custom-key", "custom-value"}};

        /// RFC 7541 C.5.1/C.6.1 第一个响应的头列表
        const std::initializer_list<HeaderListEntry> kFirstResponseHeaders{{":status", "302"},
                                                                           {"cache-control", "private"},
                                                                           {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                                                           {"location", "https://www.example.com"}};

        /// RFC 7541 C.5.2/C.6.2 第二个响应的头列表（:status 换成 307）
        const std::initializer_list<HeaderListEntry> kSecondResponseHeaders{{":status", "307"},
                                                                            {"cache-control", "private"},
                                                                            {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                                                            {"location", "https://www.example.com"}};

        /// RFC 7541 C.5.3/C.6.3 第三个响应的头列表
        const std::initializer_list<HeaderListEntry> kThirdResponseHeaders{
                {":status", "200"},
                {"cache-control", "private"},
                {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
                {"location", "https://www.example.com"},
                {"content-encoding", "gzip"},
                {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"}};

        /// RFC 7541 C.5.3/C.6.3 结束时的动态表（表大小 215）
        const std::initializer_list<HeaderListEntry> kThirdResponseDynamicTable{
                {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"},
                {"content-encoding", "gzip"},
                {"date", "Mon, 21 Oct 2013 20:13:22 GMT"}};
    } // namespace

    // ============================================================================
    // RFC 7541 §5.1：整数表示（Appendix C.1.1–C.1.3 黄金向量）
    // ============================================================================

    /**
     * @brief 钉住 C.1.1/C.1.2/C.1.3 三个整数示例的编码与解码
     * @details 10 用 5 位前缀只占一个字节；1337 溢出前缀后按 7 位一组续写两个字节；
     *          42 用 8 位前缀（首字节没有模式位）同样只占一个字节。
     */
    TEST(Hpack, IntegerRepresentationMatchesRfc7541C11ToC13)
    {
        // C.1.1：I = 10，5 位前缀
        EXPECT_EQ(encodeHpackInteger(10, 5, 0), makeBytesFromHex("0a"));
        // C.1.2：I = 1337，5 位前缀 —— 规范原文给出的三个字节是 1f 9a 0a
        EXPECT_EQ(encodeHpackInteger(1337, 5, 0), makeBytesFromHex("1f9a0a"));
        // C.1.3：I = 42，8 位前缀
        EXPECT_EQ(encodeHpackInteger(42, 8, 0), makeBytesFromHex("2a"));

        std::uint64_t value = 0;
        std::size_t consumed = 0;
        std::string reason;
        ASSERT_TRUE(decodeHpackInteger(makeBytesFromHex("0a"), 5, value, consumed, &reason)) << reason;
        EXPECT_EQ(value, 10U);
        EXPECT_EQ(consumed, 1U);

        // 前缀满值表示「后面还有续字节」，因此 1f 本身解不出 31 之后的值
        ASSERT_TRUE(decodeHpackInteger(makeBytesFromHex("1f9a0a"), 5, value, consumed, &reason)) << reason;
        EXPECT_EQ(value, 1337U);
        EXPECT_EQ(consumed, 3U);

        ASSERT_TRUE(decodeHpackInteger(makeBytesFromHex("2a"), 8, value, consumed, &reason)) << reason;
        EXPECT_EQ(value, 42U);
        EXPECT_EQ(consumed, 1U);

        // 模式位与整数的前缀共用首字节：编码时要按位或进去，解码时要按掩码取出
        EXPECT_EQ(encodeHpackInteger(2, 7, 0x80), makeBytesFromHex("82"));
        ASSERT_TRUE(decodeHpackInteger(makeBytesFromHex("82"), 7, value, consumed, &reason)) << reason;
        EXPECT_EQ(value, 2U);

        // 大数值走完整条续写链路：2^31-1 用 5 位前缀要 5 个字节
        const std::string encodedLarge = encodeHpackInteger(0x7FFFFFFFU, 5, 0);
        ASSERT_TRUE(decodeHpackInteger(encodedLarge, 5, value, consumed, &reason)) << reason;
        EXPECT_EQ(value, 0x7FFFFFFFU);
        EXPECT_EQ(consumed, encodedLarge.size());

        // 编码侧的用法错误：前缀位数越界、模式位压到前缀位上
        EXPECT_THROW(static_cast<void>(encodeHpackInteger(1, 0, 0)), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(encodeHpackInteger(1, 9, 0)), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(encodeHpackInteger(1, 7, 0x40)), Base::InvalidArgumentException);
    }

    /**
     * @brief 整数表示的拒绝面：截断、溢出、前缀位数非法
     * @details 多字节溢出必须判错而不是回绕：回绕后得到的是一个「看起来合法」的值，索引随即指向
     *          另一条头部，同一段字节在不同实现上解出不同结果。
     */
    TEST(Hpack, RejectsMalformedIntegerRepresentation)
    {
        std::uint64_t value = 0;
        std::size_t consumed = 0;
        std::string reason;

        // 前缀满值之后没有续字节：表示被截断
        EXPECT_FALSE(decodeHpackInteger(makeBytesFromHex("1f"), 5, value, consumed, &reason));
        EXPECT_TRUE(containsText(reason, "断开")) << reason;

        // 续字节的最高位一直是 1：一直拼下去会越过 64 位
        std::string endlessContinuation = makeBytesFromHex("1f");
        for (int byteIndex = 0; byteIndex < 12; ++byteIndex)
        {
            endlessContinuation += makeBytesFromHex("ff");
        }
        EXPECT_FALSE(decodeHpackInteger(endlessContinuation, 5, value, consumed, &reason));
        EXPECT_TRUE(containsText(reason, "64 位")) << reason;

        // 空输入与非法前缀位数属于调用方用法问题，同样必须有明确失败
        EXPECT_FALSE(decodeHpackInteger("", 5, value, consumed, &reason));
        EXPECT_TRUE(containsText(reason, "首字节")) << reason;
        EXPECT_FALSE(decodeHpackInteger(makeBytesFromHex("0a"), 9, value, consumed, &reason));
        EXPECT_TRUE(containsText(reason, "1..8")) << reason;
    }

    // ============================================================================
    // RFC 7541 Appendix A：静态表
    // ============================================================================

    /**
     * @brief 钉住静态表的常用条目与其索引，以及两个查找入口的语义
     * @details 索引是线格式的一部分：编码器靠它把常见头压成一个字节，对端的解码器按同一份表解析。
     */
    TEST(Hpack, StaticTableMatchesRfc7541AppendixA)
    {
        EXPECT_EQ(kHpackStaticTable.size(), kHpackStaticTableEntryCount);
        EXPECT_EQ(kHpackFirstDynamicTableIndex, 62U);

        struct EntryProbe
        {
            std::size_t index;           ///< 索引（1 起）
            const char *expectedName;    ///< 期望头名
            const char *expectedValue;   ///< 期望头值
        };

        const std::vector<EntryProbe> probes{{1, ":authority", ""},         {2, ":method", "GET"},
                                             {3, ":method", "POST"},        {4, ":path", "/"},
                                             {5, ":path", "/index.html"},   {6, ":scheme", "http"},
                                             {7, ":scheme", "https"},       {8, ":status", "200"},
                                             {14, ":status", "500"},        {15, "accept-charset", ""},
                                             {16, "accept-encoding", "gzip, deflate"},
                                             {24, "cache-control", ""},     {33, "date", ""},
                                             {55, "set-cookie", ""},        {61, "www-authenticate", ""}};

        for (const EntryProbe &probe: probes)
        {
            const HpackStaticTableEntry &entry = kHpackStaticTable[probe.index - 1];
            EXPECT_EQ(entry.name, probe.expectedName) << "索引 " << probe.index;
            EXPECT_EQ(entry.value, probe.expectedValue) << "索引 " << probe.index;
        }

        EXPECT_EQ(findHpackStaticTableIndex(":method", "GET"), 2U);
        EXPECT_EQ(findHpackStaticTableIndex("accept-encoding", "gzip, deflate"), 16U);
        EXPECT_EQ(findHpackStaticTableIndex(":method", "DELETE"), 0U) << "名对上了但值不对，不能给精确索引";
        EXPECT_EQ(findHpackStaticTableIndex("x-unknown", "v"), 0U);
        EXPECT_EQ(findHpackStaticTableNameIndex("cache-control"), 24U);
        EXPECT_EQ(findHpackStaticTableNameIndex("x-unknown"), 0U);
    }

    /**
     * @brief 钉住：静态表的段索引 + 二分查找，结果与逐项线性扫表在每一项上都一致
     * @details 段表与它的排序都在编译期算出，前提「同名条目相邻」由 static_assert 把守；这个用例补上
     *          运行期的对拍：全表逐项比两种求法，再专门试前缀名与大小写——HPACK 的字符串比较是逐字节
     *          精确的，二分边界判错会让两端压缩上下文不同步（RFC 7540 §4.3）。
     */
    TEST(Hpack, StaticTableIndexedLookupMatchesLinearScan)
    {
        const auto linearExactIndex = [](const std::string_view name, const std::string_view value)
        {
            for (std::size_t entryIndex = 0; entryIndex < kHpackStaticTable.size(); ++entryIndex)
            {
                if (kHpackStaticTable[entryIndex].name == name && kHpackStaticTable[entryIndex].value == value)
                {
                    return entryIndex + 1;
                }
            }
            return std::size_t{0};
        };
        const auto linearNameIndex = [](const std::string_view name)
        {
            for (std::size_t entryIndex = 0; entryIndex < kHpackStaticTable.size(); ++entryIndex)
            {
                if (kHpackStaticTable[entryIndex].name == name)
                {
                    return entryIndex + 1;
                }
            }
            return std::size_t{0};
        };

        for (std::size_t entryIndex = 0; entryIndex < kHpackStaticTable.size(); ++entryIndex)
        {
            const HpackStaticTableEntry &entry = kHpackStaticTable[entryIndex];
            EXPECT_EQ(findHpackStaticTableIndex(entry.name, entry.value), linearExactIndex(entry.name, entry.value))
                    << "第 " << entryIndex + 1 << " 项 " << entry.name;
            EXPECT_EQ(findHpackStaticTableNameIndex(entry.name), linearNameIndex(entry.name))
                    << "第 " << entryIndex + 1 << " 项 " << entry.name;
            // 名对得上而值对不上时必须落空：这是「只发索引名的字面量」这条路的前提
            EXPECT_EQ(findHpackStaticTableIndex(entry.name, "必然不在表里的值"), linearExactIndex(entry.name, "必然不在表里的值"))
                    << "第 " << entryIndex + 1 << " 项 " << entry.name;
        }

        for (const std::string_view name: {std::string_view{":stat"},
                                           std::string_view{":statuss"},
                                           std::string_view{"accept"},
                                           std::string_view{"accept-"},
                                           std::string_view{"accept-encodin"},
                                           std::string_view{"acceptance"},
                                           std::string_view{"content-type "},
                                           std::string_view{"Content-Type"},
                                           std::string_view{"www-authenticat"},
                                           std::string_view{"x-custom"},
                                           std::string_view{},
                                           std::string_view{"ZZZ"}})
        {
            EXPECT_EQ(findHpackStaticTableNameIndex(name), linearNameIndex(name)) << "名字 [" << name << ']';
            EXPECT_EQ(findHpackStaticTableIndex(name, "200"), linearExactIndex(name, "200")) << "名字 [" << name << ']';
        }
    }

    // ============================================================================
    // RFC 7541 Appendix C.2：四种表示（黄金向量）
    // ============================================================================

    /**
     * @brief 钉住 C.2.1：带增量索引的字面量（0x40 开头）解出 custom-key: custom-header
     * @details 规范给出解码后动态表只有这一项、表大小 55 字节（10 + 13 + 32）。
     */
    TEST(Hpack, DecodesRfc7541C21LiteralWithIncrementalIndexing)
    {
        HpackDecoder decoder;
        std::vector<HpackHeaderField> headerFields;
        std::string reason;
        const std::string block = makeBytesFromHex("400a637573746f6d2d6b65790d637573746f6d2d686561646572");

        ASSERT_TRUE(decoder.decode(block, headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, {{"custom-key", "custom-header"}});
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{"custom-key", "custom-header"}}, 55);
    }

    /**
     * @brief 钉住 C.2.2：不索引的字面量（0000 开头）解出 :path: /sample/path，动态表保持为空
     */
    TEST(Hpack, DecodesRfc7541C22LiteralWithoutIndexing)
    {
        HpackDecoder decoder;
        std::vector<HpackHeaderField> headerFields;
        std::string reason;
        const std::string block = makeBytesFromHex("040c2f73616d706c652f70617468");

        ASSERT_TRUE(decoder.decode(block, headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, {{":path", "/sample/path"}});
        EXPECT_TRUE(decoder.dynamicTableEntries().empty()) << "「不索引」不插动态表（RFC 7541 §6.2.2）";
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 0U);
    }

    /**
     * @brief 钉住 C.2.3：永不索引的字面量（0001 开头）解出 password: secret，动态表保持为空
     * @details 「永不索引」是给中间设施的指示，解码结果与「不索引」相同（RFC 7541 §6.2.3）。
     */
    TEST(Hpack, DecodesRfc7541C23LiteralNeverIndexed)
    {
        HpackDecoder decoder;
        std::vector<HpackHeaderField> headerFields;
        std::string reason;
        const std::string block = makeBytesFromHex("100870617373776f726406736563726574");

        ASSERT_TRUE(decoder.decode(block, headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, {{"password", "secret"}});
        EXPECT_TRUE(decoder.dynamicTableEntries().empty()) << "「永不索引」同样不插动态表（RFC 7541 §6.2.3）";
    }

    /**
     * @brief 钉住 C.2.4：索引表示（0x82 = 索引 2）解出 :method: GET；非法索引当场判错
     */
    TEST(Hpack, DecodesRfc7541C24IndexedHeaderFieldAndRejectsBadIndexes)
    {
        {
            HpackDecoder decoder;
            std::vector<HpackHeaderField> headerFields;
            std::string reason;
            ASSERT_TRUE(decoder.decode(makeBytesFromHex("82"), headerFields, &reason)) << reason;
            expectHeaderListEquals(headerFields, {{":method", "GET"}});
            EXPECT_TRUE(decoder.dynamicTableEntries().empty());
        }
        {
            // 索引 0 非法（RFC 7541 §6.1 规定索引从 1 起）
            HpackDecoder decoder;
            EXPECT_TRUE(containsText(expectDecodeFailure(decoder, makeBytesFromHex("80"), HpackErrorKind::CompressionError), "0"));
        }
        {
            // 静态表只有 61 项、动态表还是空的：索引 62 没有落点
            HpackDecoder decoder;
            const std::string reason = expectDecodeFailure(decoder, makeBytesFromHex("be"), HpackErrorKind::CompressionError);
            EXPECT_TRUE(containsText(reason, "不同步")) << reason;
        }
    }

    // ============================================================================
    // RFC 7541 Appendix C.3/C.4：请求示例（不带 / 带 Huffman，逐字节对照）
    // ============================================================================

    /**
     * @brief 钉住 C.3.1–C.3.3：三个连续请求的头列表与动态表演进（不带 Huffman）
     * @details 第二个请求用 0xbe（索引 62）引用第一个请求插进动态表的 :authority，第三个请求用 0xbf
     *          引用同一项 —— 只有动态表按规范演进，这两个索引才对得上。
     */
    TEST(Hpack, DecodesRfc7541C31ToC33RequestSequenceWithoutHuffman)
    {
        HpackDecoder decoder;
        std::vector<HpackHeaderField> headerFields;
        std::string reason;

        ASSERT_TRUE(decoder.decode(makeBytesFromHex("828684410f7777772e6578616d706c652e636f6d"), headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, kFirstRequestHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{":authority", "www.example.com"}}, 57);

        ASSERT_TRUE(decoder.decode(makeBytesFromHex("828684be58086e6f2d6361636865"), headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, kSecondRequestHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{"cache-control", "no-cache"}, {":authority", "www.example.com"}}, 110);

        ASSERT_TRUE(decoder.decode(makeBytesFromHex("828785bf400a637573746f6d2d6b65790c637573746f6d2d76616c7565"), headerFields, &reason))
                << reason;
        expectHeaderListEquals(headerFields, kThirdRequestHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{"custom-key", "custom-value"}, {"cache-control", "no-cache"}, {":authority", "www.example.com"}}, 164);
    }

    /**
     * @brief 钉住 C.4.1–C.4.3：同一批请求改用 Huffman 编码的头值，逐字节对照
     * @details 头块与 C.3.x 只差字面量值的字节，因此这条用例是 Huffman 解码器对着规范原文的第一判据。
     */
    TEST(Hpack, DecodesRfc7541C41ToC43RequestSequenceWithHuffman)
    {
        HpackDecoder decoder;
        std::vector<HpackHeaderField> headerFields;
        std::string reason;

        // C.4.1：f1e3 c2e5 f23a 6ba0 ab90 f4ff = "www.example.com"
        ASSERT_TRUE(decoder.decode(makeBytesFromHex("828684418cf1e3c2e5f23a6ba0ab90f4ff"), headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, kFirstRequestHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{":authority", "www.example.com"}}, 57);

        // C.4.2：a8eb 1064 9cbf = "no-cache"
        ASSERT_TRUE(decoder.decode(makeBytesFromHex("828684be5886a8eb10649cbf"), headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, kSecondRequestHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{"cache-control", "no-cache"}, {":authority", "www.example.com"}}, 110);

        // C.4.3：25a8 49e9 5ba9 7d7f = "custom-key"，25a8 49e9 5bb8 e8b4 bf = "custom-value"
        ASSERT_TRUE(decoder.decode(makeBytesFromHex("828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf"), headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, kThirdRequestHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{"custom-key", "custom-value"}, {"cache-control", "no-cache"}, {":authority", "www.example.com"}}, 164);
    }

    // ============================================================================
    // RFC 7541 Appendix C.5/C.6：响应示例（动态表上限 256，含驱逐）
    // ============================================================================

    /**
     * @brief 钉住 C.5.1–C.5.3：服务端方向的响应头，动态表上限 256 字节时按规范驱逐（不带 Huffman）
     * @details 三个响应的表大小演进为 222 → 222 → 215，中间两次驱逐都是因为 256 字节的上限。
     */
    TEST(Hpack, DecodesRfc7541C51ToC53ResponseSequenceWithoutHuffman)
    {
        // 上限 256 = 规范示例里 SETTINGS_HEADER_TABLE_SIZE 的取值，本端的解码器按它建表
        HpackDecoder decoder(HpackDecoderLimits{.maximumDynamicTableSizeByteCount = 256});
        std::vector<HpackHeaderField> headerFields;
        std::string reason;

        ASSERT_TRUE(decoder.decode(makeBytesFromHex(
                            "4803333032580770726976617465611d4d6f6e2c203231204f637420323031332032303a31333a323120474d546e1768747470733a2f2f7777772e6578616d706c652e636f6d"),
                    headerFields, &reason))
                << reason;
        expectHeaderListEquals(headerFields, kFirstResponseHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{"location", "https://www.example.com"},
                                  {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                  {"cache-control", "private"},
                                  {":status", "302"}},
                                 222);

        ASSERT_TRUE(decoder.decode(makeBytesFromHex("4803333037c1c0bf"), headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, kSecondResponseHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{":status", "307"},
                                  {"location", "https://www.example.com"},
                                  {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                  {"cache-control", "private"}},
                                 222);

        ASSERT_TRUE(decoder.decode(
                            makeBytesFromHex("88c1611d4d6f6e2c203231204f637420323031332032303a31333a323220474d54c05a04677a69707738666f6f3d4153444a4b48514b"
                                             "425a584f5157454f50495541585157454f49553b206d61782d6167653d333630303b2076657273696f6e3d31"),
                            headerFields, &reason))
                << reason;
        expectHeaderListEquals(headerFields, kThirdResponseHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(), kThirdResponseDynamicTable, 215);
    }

    /**
     * @brief 钉住 C.6.1–C.6.3：同一批响应改用 Huffman 编码，动态表演进与 C.5.x 完全一致
     * @details 规范特意说明：驱逐用的是**解码后**的字面量长度，因此 Huffman 不会改变表状态。
     */
    TEST(Hpack, DecodesRfc7541C61ToC63ResponseSequenceWithHuffman)
    {
        HpackDecoder decoder(HpackDecoderLimits{.maximumDynamicTableSizeByteCount = 256});
        std::vector<HpackHeaderField> headerFields;
        std::string reason;

        ASSERT_TRUE(decoder.decode(makeBytesFromHex("488264025885aec3771a4b6196d07abe941054d444a8200595040b8166e082a62d1bff6e919d29ad171863c78f0b9"
                                                    "7c8e9ae82ae43d3"),
                                   headerFields, &reason))
                << reason;
        expectHeaderListEquals(headerFields, kFirstResponseHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{"location", "https://www.example.com"},
                                  {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                  {"cache-control", "private"},
                                  {":status", "302"}},
                                 222);

        ASSERT_TRUE(decoder.decode(makeBytesFromHex("4883640effc1c0bf"), headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, kSecondResponseHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(),
                                 {{":status", "307"},
                                  {"location", "https://www.example.com"},
                                  {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                  {"cache-control", "private"}},
                                 222);

        ASSERT_TRUE(decoder.decode(makeBytesFromHex("88c16196d07abe941054d444a8200595040b8166e084a62d1bffc05a839bd9ab77ad94e7821dd7f2e6c7b335dfdfc"
                                                    "d5b3960d5af27087f3672c1ab270fb5291f9587316065c003ed4ee5b1063d5007"),
                                   headerFields, &reason))
                << reason;
        expectHeaderListEquals(headerFields, kThirdResponseHeaders);
        expectDynamicTableEquals(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(), kThirdResponseDynamicTable, 215);
    }

    // ============================================================================
    // 编码器：能编出被解码器正确解回的字节
    // ============================================================================

    /**
     * @brief 钉住编码器逐字节复现 RFC 7541 C.3.1–C.3.3 的三个请求头块
     * @details 编码器走的是「静态表精确命中 → 动态表精确命中 → 静态表同名（带增量索引的字面量）→
     *          双字面量」这条策略，与规范示例的编码结果完全一致，因此可以直接拿黄金字节对照。
     */
    TEST(Hpack, EncoderReproducesRfc7541C31ToC33RequestBlocks)
    {
        HpackEncoder encoder;

        EXPECT_EQ(encoder.encode(makeHeaderList(kFirstRequestHeaders)), makeBytesFromHex("828684410f7777772e6578616d706c652e636f6d"));
        EXPECT_EQ(encoder.encode(makeHeaderList(kSecondRequestHeaders)), makeBytesFromHex("828684be58086e6f2d6361636865"));
        EXPECT_EQ(encoder.encode(makeHeaderList(kThirdRequestHeaders)),
                  makeBytesFromHex("828785bf400a637573746f6d2d6b65790c637573746f6d2d76616c7565"));

        // 编码器的动态表必须与规范给出的状态一致（否则下一个头块的索引就对不上了）
        expectDynamicTableEquals(encoder.dynamicTableEntries(), encoder.dynamicTableSizeByteCount(),
                                 {{"custom-key", "custom-value"}, {"cache-control", "no-cache"}, {":authority", "www.example.com"}}, 164);
    }

    /**
     * @brief 钉住编码器逐字节复现 RFC 7541 C.5.1–C.5.3 的三个响应头块
     * @details 构造时就把表上限定为 256：这是连接上**已经约定好**的初始上限（等于对端通告的
     *          SETTINGS_HEADER_TABLE_SIZE），因此头块开头不需要再补「动态表大小更新」，
     *          编出来的字节与规范示例逐字节相同。
     */
    TEST(Hpack, EncoderReproducesRfc7541C51ToC53ResponseBlocks)
    {
        HpackEncoder encoder(256);

        const std::string firstResponse = encoder.encode(makeHeaderList(kFirstResponseHeaders));
        EXPECT_EQ(firstResponse,
                  makeBytesFromHex("4803333032580770726976617465611d4d6f6e2c203231204f637420323031332032303a31333a323120474d546e1768747470733a2f2f"
                                   "7777772e6578616d706c652e636f6d"));
        expectDynamicTableEquals(encoder.dynamicTableEntries(), encoder.dynamicTableSizeByteCount(),
                                 {{"location", "https://www.example.com"},
                                  {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                  {"cache-control", "private"},
                                  {":status", "302"}},
                                 222);

        const std::string secondResponse = encoder.encode(makeHeaderList(kSecondResponseHeaders));
        EXPECT_EQ(secondResponse, makeBytesFromHex("4803333037c1c0bf"));
        expectDynamicTableEquals(encoder.dynamicTableEntries(), encoder.dynamicTableSizeByteCount(),
                                 {{":status", "307"},
                                  {"location", "https://www.example.com"},
                                  {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                  {"cache-control", "private"}},
                                 222);

        const std::string thirdResponse = encoder.encode(makeHeaderList(kThirdResponseHeaders));
        EXPECT_EQ(thirdResponse, makeBytesFromHex("88c1611d4d6f6e2c203231204f637420323031332032303a31333a323220474d54c05a04677a69707738666f6f3d4153444a4b48514b425a"
                                                  "584f5157454f50495541585157454f49553b206d61782d6167653d333630303b2076657273696f6e3d31"));
        expectDynamicTableEquals(encoder.dynamicTableEntries(), encoder.dynamicTableSizeByteCount(), kThirdResponseDynamicTable, 215);
    }

    /**
     * @brief 编码 → 解码往返：任意字节（含 NUL 与高位字节）与重复头都能原样回来
     * @details 两端各自的动态表必须同步演进：编码器每发一个「带增量索引的字面量」，解码器就插一项。
     */
    TEST(Hpack, EncoderRoundTripsThroughDecoder)
    {
        const std::string binaryValue = std::string("\x00\x01\x80\xff", 4);
        const std::vector<HpackHeaderField> expectedFields{
                {":status", "200"},
                {"content-type", "application/json; charset=utf-8"},
                {"content-length", "1234"},
                {"x-binary", binaryValue},
                {"set-cookie", "a=1; Path=/"},
                {"set-cookie", "b=2; Path=/"},
                {"x-custom-header", "first"},
                {"x-custom-header", "second"},
        };

        HpackEncoder encoder;
        HpackDecoder decoder;

        for (int round = 0; round < 2; ++round)
        {
            const std::string headerBlock = encoder.encode(expectedFields);

            std::vector<HpackHeaderField> decodedFields;
            std::string reason;
            ASSERT_TRUE(decoder.decode(headerBlock, decodedFields, &reason)) << "第 " << round + 1 << " 轮：" << reason;
            ASSERT_EQ(decodedFields.size(), expectedFields.size());
            for (std::size_t index = 0; index < expectedFields.size(); ++index)
            {
                EXPECT_EQ(decodedFields[index].name, expectedFields[index].name) << "第 " << index << " 条头名";
                EXPECT_EQ(decodedFields[index].value, expectedFields[index].value) << "第 " << index << " 条头值";
            }

            // 两端的动态表必须逐项一致：任何一侧多插、漏插或错驱逐，后续头块的索引都会指错
            // （:status: 200 命中静态表，因此不会进动态表）
            const std::vector<HpackHeaderField> expectedTableEntries{{"x-custom-header", "second"},
                                                                    {"x-custom-header", "first"},
                                                                    {"set-cookie", "b=2; Path=/"},
                                                                    {"set-cookie", "a=1; Path=/"},
                                                                    {"x-binary", binaryValue},
                                                                    {"content-length", "1234"},
                                                                    {"content-type", "application/json; charset=utf-8"}};
            expectDynamicTableFieldsMatch(decoder.dynamicTableEntries(), decoder.dynamicTableSizeByteCount(), expectedTableEntries);
            EXPECT_EQ(decoder.dynamicTableSizeByteCount(), encoder.dynamicTableSizeByteCount());
            EXPECT_EQ(decoder.dynamicTableEntries().size(), encoder.dynamicTableEntries().size());
        }
    }

    /**
     * @brief 重复的头列表在第二个头块里改用动态表索引，字节数明显变少且仍能解回
     */
    TEST(Hpack, RepeatedHeaderListsUseDynamicIndexesAndStayInSync)
    {
        const std::vector<HpackHeaderField> headerFields{
                {":status", "200"},
                {"x-request-id", "0f8fad5b-d9cb-469f-a165-70867728950e"},
        };

        HpackEncoder encoder;
        HpackDecoder decoder;
        const std::string firstBlock = encoder.encode(headerFields);
        const std::string secondBlock = encoder.encode(headerFields);

        EXPECT_LT(secondBlock.size(), firstBlock.size()) << "第二次应当用动态表索引，而不是再把字面量发一遍";

        std::vector<HpackHeaderField> decodedFields;
        std::string reason;
        ASSERT_TRUE(decoder.decode(firstBlock, decodedFields, &reason)) << reason;
        ASSERT_TRUE(decoder.decode(secondBlock, decodedFields, &reason)) << reason;
        ASSERT_EQ(decodedFields.size(), 2U);
        EXPECT_EQ(decodedFields[1].name, "x-request-id");
        EXPECT_EQ(decodedFields[1].value, "0f8fad5b-d9cb-469f-a165-70867728950e");
    }

    // ============================================================================
    // RFC 7541 §5.2 + Appendix B：Huffman 解码（码表与拒绝面）
    // ============================================================================

    /**
     * @brief 钉住码表本身是完整前缀码：各码字位长的 2 的负幂之和恰为 1，且没有任何码字是别人的前缀
     * @details 这两个性质是「逐位沿树推进」这种解码方式成立的前提；任何一项被改坏，解码会解出
     *          错误的符号或走进死路，而黄金向量往往覆盖不到冷门符号。
     */
    TEST(Hpack, HuffmanCodeTableIsACompletePrefixCode)
    {
        ASSERT_EQ(kHpackHuffmanCodeTable.size(), kHpackHuffmanEndOfStringSymbol + 1);
        EXPECT_EQ(kHpackHuffmanCodeTable[kHpackHuffmanEndOfStringSymbol].code, 0x3FFFFFFFU) << "EOS 是 30 位全 1";
        EXPECT_EQ(kHpackHuffmanCodeTable[kHpackHuffmanEndOfStringSymbol].bitCount, 30);

        std::uint64_t kraftSum = 0;
        for (const HpackHuffmanCode &code: kHpackHuffmanCodeTable)
        {
            ASSERT_GE(code.bitCount, 5) << "RFC 7541 Appendix B 的最短码字是 5 位";
            ASSERT_LE(code.bitCount, 30);
            // 码字按低位对齐存放：高于位长的部分必须为 0，否则位流会多出无效的高位
            EXPECT_EQ(code.code >> code.bitCount, 0U);
            kraftSum += 1ULL << (30 - code.bitCount);
        }
        EXPECT_EQ(kraftSum, 1ULL << 30) << "Σ 2^-位长 必须等于 1（完整前缀码）";

        for (std::size_t leftIndex = 0; leftIndex < kHpackHuffmanCodeTable.size(); ++leftIndex)
        {
            for (std::size_t rightIndex = 0; rightIndex < kHpackHuffmanCodeTable.size(); ++rightIndex)
            {
                const HpackHuffmanCode &left = kHpackHuffmanCodeTable[leftIndex];
                const HpackHuffmanCode &right = kHpackHuffmanCodeTable[rightIndex];
                if (left.bitCount >= right.bitCount || leftIndex == rightIndex)
                {
                    continue;
                }
                // 短码字不得成为长码字的前缀，否则同一段位流有两种读法
                EXPECT_NE(right.code >> (right.bitCount - left.bitCount), left.code)
                        << "符号 " << leftIndex << " 的码字成了符号 " << rightIndex << " 的前缀";
            }
        }
    }

    /**
     * @brief 用码表编出 0..255 全部符号与常见头文本，解码器必须原样解回
     * @details 黄金向量只覆盖得到请求/响应头里出现过的符号；这条往返用例把 256 个符号都走一遍，
     *          顺带钉住「不足一字节的填充取全 1」这一打包约定。
     */
    TEST(Hpack, HuffmanDecoderRoundTripsEverySymbol)
    {
        std::string allByteValues;
        for (int byteValue = 0; byteValue < 256; ++byteValue)
        {
            allByteValues.push_back(static_cast<char>(byteValue));
        }

        const std::vector<std::string> probes{
                allByteValues,
                "",
                "a",
                "www.example.com",
                "Mon, 21 Oct 2013 20:13:21 GMT",
                "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1",
                "\x00""nul-in-the-middle\x00",
        };

        for (const std::string &probe: probes)
        {
            const std::string encoded = encodeHuffmanWithCodeTable(probe);

            std::string decoded;
            std::string reason;
            ASSERT_TRUE(decodeHpackHuffmanString(encoded, decoded, &reason)) << reason;

            EXPECT_EQ(decoded.size(), probe.size()) << "解出的字节数不符";
            EXPECT_EQ(decoded, probe) << "Huffman 往返必须逐字节一致";
        }

        // 长文本的往返：编码后应显著变短（Huffman 的收益就在这里）
        const std::string longText(1024, 'e');
        const std::string encodedLongText = encodeHuffmanWithCodeTable(longText);
        std::string decodedLongText;
        std::string reason;
        ASSERT_TRUE(decodeHpackHuffmanString(encodedLongText, decodedLongText, &reason)) << reason;
        EXPECT_EQ(decodedLongText, longText);
        EXPECT_LT(encodedLongText.size(), longText.size());
    }

    /**
     * @brief Huffman 的拒绝面：EOS 符号、超过 7 位的填充、不是全 1 的填充
     * @details RFC 7541 §5.2 规定 EOS 只用于填充位型、填充必须取自 EOS 码字的高位且严格短于 8 位。
     */
    TEST(Hpack, RejectsHuffmanEndOfStringAndBadPadding)
    {
        std::string decoded;
        std::string reason;

        {
            // 30 位全 1 正好是 EOS 码字：四个 0xff 的前 30 位就把它撞出来了
            EXPECT_FALSE(decodeHpackHuffmanString(makeBytesFromHex("ffffffff"), decoded, &reason));
            EXPECT_TRUE(containsText(reason, "EOS")) << reason;
        }
        {
            // 'a'（00011）之后跟 8 个以上的 1：填充超过 7 位
            EXPECT_FALSE(decodeHpackHuffmanString(makeBytesFromHex("1ff8"), decoded, &reason));
            EXPECT_TRUE(containsText(reason, "填充") || containsText(reason, "7 位")) << reason;
        }
        {
            // 'a'（00011）之后跟 010：残留位不是全 1，也不是 EOS 的前缀
            EXPECT_FALSE(decodeHpackHuffmanString(makeBytesFromHex("1a"), decoded, &reason));
            EXPECT_TRUE(containsText(reason, "全 1")) << reason;
        }
        {
            // 空的 Huffman 数据是合法的：长度前缀为 0 的字符串就是空串
            ASSERT_TRUE(decodeHpackHuffmanString("", decoded, &reason)) << reason;
            EXPECT_TRUE(decoded.empty());
        }
    }

    /**
     * @brief 字符串字面量的拒绝面：长度超过剩余字节、H 位与长度字段混在一起时的截断
     */
    TEST(Hpack, RejectsMalformedStringLiteral)
    {
        std::string value;
        std::size_t consumed = 0;
        std::string reason;

        // 声明 15 字节但只给 3 字节
        EXPECT_FALSE(decodeHpackString(makeBytesFromHex("0f616263"), value, consumed, &reason));
        EXPECT_TRUE(containsText(reason, "剩余")) << reason;
        EXPECT_TRUE(containsText(reason, "15")) << "原因里要给声明的长度：" << reason;

        // H 位置位且长度超过剩余字节
        EXPECT_FALSE(decodeHpackString(makeBytesFromHex("8f616263"), value, consumed, &reason));
        EXPECT_TRUE(containsText(reason, "剩余")) << reason;

        // 空输入
        EXPECT_FALSE(decodeHpackString("", value, consumed, &reason));
        EXPECT_TRUE(containsText(reason, "首字节")) << reason;

        // 合法：长度为 0 的空串
        ASSERT_TRUE(decodeHpackString(makeBytesFromHex("00"), value, consumed, &reason)) << reason;
        EXPECT_TRUE(value.empty());
        EXPECT_EQ(consumed, 1U);
    }

    // ============================================================================
    // RFC 7541 §6.3 + §4：动态表大小更新、插入与驱逐
    // ============================================================================

    /**
     * @brief 动态表大小更新只许出现在头块开头，且新上限不得大于本端通告的上限
     * @details RFC 7541 §4.2 要求上限变化在头块开头通告；§6.3 要求超过协议限度的取值按解码错误处理。
     */
    TEST(Hpack, AcceptsDynamicTableSizeUpdateOnlyAtTheStartOfTheBlock)
    {
        {
            // 0x20 = 大小更新到 0（5 位前缀里直接写下 0），随后是一个普通索引表示
            HpackDecoder decoder;
            std::vector<HpackHeaderField> headerFields;
            std::string reason;
            ASSERT_TRUE(decoder.decode(makeBytesFromHex("2082"), headerFields, &reason)) << reason;
            expectHeaderListEquals(headerFields, {{":method", "GET"}});
            EXPECT_EQ(decoder.dynamicTableMaximumSizeByteCount(), 0U);
        }
        {
            // 3f e1 01 = 大小更新到 256（前缀满值后按 7 位一组续写）
            HpackDecoder decoder;
            std::vector<HpackHeaderField> headerFields;
            std::string reason;
            ASSERT_TRUE(decoder.decode(makeBytesFromHex("3fe10182"), headerFields, &reason)) << reason;
            expectHeaderListEquals(headerFields, {{":method", "GET"}});
            EXPECT_EQ(decoder.dynamicTableMaximumSizeByteCount(), 256U);
        }
        {
            // 同一个头块开头可以连着两个更新，取最后生效的那个
            HpackDecoder decoder;
            std::vector<HpackHeaderField> headerFields;
            std::string reason;
            ASSERT_TRUE(decoder.decode(makeBytesFromHex("203fe10182"), headerFields, &reason)) << reason;
            EXPECT_EQ(decoder.dynamicTableMaximumSizeByteCount(), 256U);
        }
        {
            // 放在头部之后就是解码错误（前后两段的索引会指向不同的表状态）
            HpackDecoder decoder;
            const std::string reason =
                    expectDecodeFailure(decoder, makeBytesFromHex("8220"), HpackErrorKind::CompressionError);
            EXPECT_TRUE(containsText(reason, "开头")) << reason;
        }
        {
            // 新上限超过本端通告的 SETTINGS_HEADER_TABLE_SIZE（本端通告 128，对端却要 256）
            HpackDecoder decoder(HpackDecoderLimits{.maximumDynamicTableSizeByteCount = 128});
            const std::string reason =
                    expectDecodeFailure(decoder, makeBytesFromHex("3fe10182"), HpackErrorKind::CompressionError);
            EXPECT_TRUE(containsText(reason, "128")) << "原因里要给本端通告的上限：" << reason;
            EXPECT_EQ(toHttp2ErrorCode(decoder.errorKind()), Http2ErrorCode::CompressionError);
        }
        {
            // 收小上限会立刻驱逐放不下的旧项（RFC 7541 §4.3）：先插入一项 57 字节，再把上限收到 8
            HpackDecoder decoder;
            std::vector<HpackHeaderField> headerFields;
            std::string reason;
            ASSERT_TRUE(decoder.decode(makeBytesFromHex("410f7777772e6578616d706c652e636f6d"), headerFields, &reason)) << reason;
            EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 57U);

            // 0x28 = 大小更新到 8（5 位前缀里直接写下 8）
            ASSERT_TRUE(decoder.decode(makeBytesFromHex("2882"), headerFields, &reason)) << reason;
            EXPECT_EQ(decoder.dynamicTableMaximumSizeByteCount(), 8U);
            EXPECT_TRUE(decoder.dynamicTableEntries().empty()) << "收到 8 字节后，57 字节的那一项已放不下";
            EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 0U);
        }
    }

    /**
     * @brief 动态表的插入顺序、驱逐顺序与「放不进空表就清空且不插」的规则（RFC 7541 §4.3、§4.4）
     * @details 索引空间里 62 是最新插入的一项，因此驱逐总从最旧的一项开始；表大小按名长 + 值长 + 32 算。
     */
    TEST(Hpack, DynamicTableInsertsAndEvictsInOrder)
    {
        HpackDynamicTable dynamicTable(60);
        EXPECT_EQ(dynamicTable.maximumSizeByteCount(), 60U);
        EXPECT_EQ(dynamicTable.entryCount(), 0U);

        // 每一项占 4 + 4 + 32 = 40 字节，表上限 60 只放得下一项
        dynamicTable.insert(HpackHeaderField{"aaaa", "bbbb"});
        EXPECT_EQ(dynamicTable.sizeByteCount(), 40U);

        HpackHeaderField entry;
        EXPECT_TRUE(dynamicTable.tryGetEntry(0, entry));
        EXPECT_EQ(entry.name, "aaaa");
        EXPECT_FALSE(dynamicTable.tryGetEntry(1, entry)) << "动态表只有一项时下标 1 越界";

        dynamicTable.insert(HpackHeaderField{"cccc", "dddd"});
        ASSERT_EQ(dynamicTable.entryCount(), 1U) << "第二项插进来时第一项必须被驱逐（40 + 40 > 60）";
        EXPECT_EQ(dynamicTable.entries()[0].name, "cccc") << "最新插入的一项在索引 62 的位置";
        EXPECT_EQ(dynamicTable.sizeByteCount(), 40U);

        // 放不进空表的项：清空整张表且不插入（放进去会让所有后续索引都失效）
        dynamicTable.insert(HpackHeaderField{std::string(64, 'n'), std::string(64, 'v')});
        EXPECT_EQ(dynamicTable.entryCount(), 0U);
        EXPECT_EQ(dynamicTable.sizeByteCount(), 0U);

        // 上限收到 0 之后什么都插不进去（等价于禁用动态表）
        dynamicTable.setMaximumSizeByteCount(0);
        dynamicTable.insert(HpackHeaderField{"aaaa", "bbbb"});
        EXPECT_EQ(dynamicTable.entryCount(), 0U);
        EXPECT_EQ(dynamicTable.sizeByteCount(), 0U);

        // 清空只清条目，上限保持不变
        dynamicTable.setMaximumSizeByteCount(128);
        dynamicTable.insert(HpackHeaderField{"eeee", "ffff"});
        dynamicTable.clear();
        EXPECT_EQ(dynamicTable.entryCount(), 0U);
        EXPECT_EQ(dynamicTable.maximumSizeByteCount(), 128U);
    }

    /**
     * @brief 编码器改过表上限后，下一个头块的开头必须补一个「动态表大小更新」
     * @details RFC 7541 §4.2 要求上限变化在头块开头通告；本端编码器把它放在第一个字节，
     *          对端的解码器才能在解头部之前把表调整到同一状态。
     */
    TEST(Hpack, EncoderAnnouncesTableSizeChangeAtTheStartOfTheBlock)
    {
        HpackEncoder encoder;
        HpackDecoder decoder;

        const std::vector<HpackHeaderField> headerFields{{":status", "204"}};
        std::vector<HpackHeaderField> decodedFields;
        std::string reason;
        ASSERT_TRUE(decoder.decode(encoder.encode(headerFields), decodedFields, &reason)) << reason;

        encoder.setMaximumDynamicTableSizeByteCount(128);
        EXPECT_EQ(encoder.maximumDynamicTableSizeByteCount(), 128U);

        const std::string blockAfterLimitChange = encoder.encode(headerFields);
        ASSERT_FALSE(blockAfterLimitChange.empty());
        EXPECT_EQ(static_cast<unsigned char>(blockAfterLimitChange[0]) & 0xE0U, 0x20U)
                << "头块开头必须是 001 模式的「动态表大小更新」";

        ASSERT_TRUE(decoder.decode(blockAfterLimitChange, decodedFields, &reason)) << reason;
        EXPECT_EQ(decoder.dynamicTableMaximumSizeByteCount(), 128U) << "对端的表上限要跟着变";
        expectHeaderListEquals(decodedFields, {{":status", "204"}});

        // 上限没变化时不再重复通告
        const std::string blockWithoutChange = encoder.encode(headerFields);
        EXPECT_NE(static_cast<unsigned char>(blockWithoutChange[0]) & 0xE0U, 0x20U) << "没有变化就不该再打一个字节";
    }

    // ============================================================================
    // 资源上限与粘滞错误态
    // ============================================================================

    /**
     * @brief 头列表总大小、单个名与值的长度上限都会拒绝，并归入 LimitExceeded（对应 ENHANCE_YOUR_CALM）
     * @details 头列表大小的算式按 RFC 7540 §6.5.2：每项名长 + 值长 + 32。上限是本端策略，
     *          不是对端违规，因此与 CompressionError 分开。
     */
    TEST(Hpack, RejectsHeaderListAndFieldLengthLimits)
    {
        {
            // C.3.1 的头列表净大小是 57 + 42 + 32 + 35... 这里把上限压到 100 字节
            HpackDecoder decoder(HpackDecoderLimits{.maximumHeaderListByteCount = 100});
            const std::string reason =
                    expectDecodeFailure(decoder, makeBytesFromHex("828684410f7777772e6578616d706c652e636f6d"), HpackErrorKind::LimitExceeded);
            EXPECT_TRUE(containsText(reason, "100")) << "原因里要给上限数值：" << reason;
            EXPECT_EQ(toHttp2ErrorCode(decoder.errorKind()), Http2ErrorCode::EnhanceYourCalm);
        }
        {
            HpackDecoder decoder(HpackDecoderLimits{.maximumHeaderFieldNameLength = 4});
            const std::string reason = expectDecodeFailure(
                    decoder, makeBytesFromHex("400a637573746f6d2d6b65790d637573746f6d2d686561646572"), HpackErrorKind::LimitExceeded);
            EXPECT_TRUE(containsText(reason, "头名")) << reason;
        }
        {
            HpackDecoder decoder(HpackDecoderLimits{.maximumHeaderFieldValueLength = 4});
            const std::string reason = expectDecodeFailure(
                    decoder, makeBytesFromHex("400a637573746f6d2d6b65790d637573746f6d2d686561646572"), HpackErrorKind::LimitExceeded);
            EXPECT_TRUE(containsText(reason, "头值")) << reason;
        }
        {
            // 关闭动态表（上限 0）后，对端还发「带增量索引」也不算错：这一项进不了表，但头列表照常交出来
            HpackDecoder decoder(HpackDecoderLimits{.maximumDynamicTableSizeByteCount = 0});
            std::vector<HpackHeaderField> headerFields;
            std::string reason;
            ASSERT_TRUE(decoder.decode(makeBytesFromHex("400a637573746f6d2d6b65790d637573746f6d2d686561646572"), headerFields, &reason))
                    << reason;
            expectHeaderListEquals(headerFields, {{"custom-key", "custom-header"}});
            EXPECT_TRUE(decoder.dynamicTableEntries().empty()) << "上限为 0 时任何项都进不了动态表";
        }
    }

    /**
     * @brief 超出头部上限不粘滞解码器，且被拒绝的头块仍把动态表更新到位
     * @details 这是「只作废这一条流」的前提：对端的编码器已经把这些项插进了它自己的表，本端不跟着
     *          插就会让两端从此错开（RFC 9113 §10.5.1 要求头块必须处理完以保证连接状态一致）。
     *          判据取「被拒绝块里最后插入的那一项随后能用索引取到」——表没同步就会解成 CompressionError。
     */
    TEST(Hpack, KeepsDynamicTableSyncedWhenHeaderLimitsRejectTheBlock)
    {
        // 名长 + 值长 + 32：第一项 63 字节过得去，第二项累计 136 字节撑爆 100 的上限
        constexpr std::size_t kHeaderListLimit = 100;
        const std::string firstField = makeIncrementalLiteralField("a", std::string(30, 'u'));
        const std::string secondField = makeIncrementalLiteralField("b", std::string(40, 'v'));

        HpackDecoder decoder(HpackDecoderLimits{.maximumHeaderListByteCount = kHeaderListLimit});
        std::vector<HpackHeaderField> headerFields;
        std::string reason;
        ASSERT_FALSE(decoder.decode(firstField + secondField, headerFields, &reason)) << "累计 136 字节应当被 100 的上限挡下";
        EXPECT_TRUE(headerFields.empty()) << "越限的头块不交出任何字段";
        EXPECT_TRUE(decoder.isLimitExceeded()) << "越限要能与压缩错误区分开";
        EXPECT_FALSE(decoder.hasError()) << "越限不许把解码器钉死，同一条连接的下一个头块还得解";
        EXPECT_EQ(decoder.dynamicTableEntries().size(), 2U) << "被拒绝的块里两项带增量索引的都要进表";

        // 62 是最后插入的那一项（b）：能按索引解回来就说明两端没有错开
        ASSERT_TRUE(decoder.decode(std::string_view("\xbe", 1), headerFields, &reason)) << reason;
        EXPECT_FALSE(decoder.isLimitExceeded());
        ASSERT_EQ(headerFields.size(), 1U);
        EXPECT_EQ(headerFields.front().name, "b");
        EXPECT_EQ(headerFields.front().value, std::string(40, 'v'));
    }

    /**
     * @brief 头块被截断或表示不完整时判 CompressionError，并给出可排查的中文原因
     */
    TEST(Hpack, RejectsTruncatedHeaderBlock)
    {
        {
            // 带增量索引的字面量只给了名字的一部分
            HpackDecoder decoder;
            expectDecodeFailure(decoder, makeBytesFromHex("400a637573"), HpackErrorKind::CompressionError);
        }
        {
            // 名字索引满值之后没有续字节
            HpackDecoder decoder;
            const std::string reason = expectDecodeFailure(decoder, makeBytesFromHex("7f"), HpackErrorKind::CompressionError);
            EXPECT_TRUE(containsText(reason, "索引")) << reason;
        }
        {
            // 长度前缀声明 5 字节但负载不足
            HpackDecoder decoder;
            expectDecodeFailure(decoder, makeBytesFromHex("40056162"), HpackErrorKind::CompressionError);
        }
    }

    /**
     * @brief 解出的头列表保持到达顺序，重复的头名与空值都原样交出
     */
    TEST(Hpack, PreservesFieldOrderAndDuplicates)
    {
        HpackEncoder encoder;
        HpackDecoder decoder;

        const std::vector<HpackHeaderField> headerFields{
                {"set-cookie", "a=1"},
                {":status", "200"},
                {"set-cookie", "b=2"},
                {"x-empty", ""},
                {"accept-encoding", "gzip, deflate"},
                {"set-cookie", "a=1"},
        };

        std::vector<HpackHeaderField> decodedFields;
        std::string reason;
        ASSERT_TRUE(decoder.decode(encoder.encode(headerFields), decodedFields, &reason)) << reason;
        ASSERT_EQ(decodedFields.size(), headerFields.size());
        for (std::size_t index = 0; index < headerFields.size(); ++index)
        {
            EXPECT_EQ(decodedFields[index].name, headerFields[index].name) << "第 " << index << " 条头名";
            EXPECT_EQ(decodedFields[index].value, headerFields[index].value) << "第 " << index << " 条头值";
        }
    }

    /**
     * @brief 失败后解码器粘滞在错误态：动态表不再改动，reset() 才能回到初态
     * @details 头块解不开意味着压缩上下文已经与对端不同步（RFC 7540 §4.3），继续解只会解出错的头部。
     */
    TEST(Hpack, ErrorStateIsStickyUntilReset)
    {
        HpackDecoder decoder;
        std::vector<HpackHeaderField> headerFields;
        std::string reason;

        // 先解一个正常的头块，把动态表建立起来
        ASSERT_TRUE(decoder.decode(makeBytesFromHex("828684410f7777772e6578616d706c652e636f6d"), headerFields, &reason)) << reason;
        ASSERT_EQ(decoder.dynamicTableSizeByteCount(), 57U);

        // 再喂一个非法头块（动态表只有一项，索引 63 越界）：进入粘滞错误态
        expectDecodeFailure(decoder, makeBytesFromHex("bf"), HpackErrorKind::CompressionError);
        const std::size_t tableSizeAfterFailure = decoder.dynamicTableSizeByteCount();

        // 后续调用一律失败，且动态表不再被改动
        EXPECT_FALSE(decoder.decode(makeBytesFromHex("82"), headerFields, &reason));
        EXPECT_TRUE(containsText(reason, "不同步") || containsText(reason, "索引")) << reason;
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), tableSizeAfterFailure);
        EXPECT_TRUE(headerFields.empty()) << "失败时头列表只能是空壳或不可信内容，这里在进入调用时就被清空";

        // reset() 回到「新连接」初态：动态表清空、错误清除，可以重新解同一个头块
        decoder.reset();
        EXPECT_FALSE(decoder.hasError());
        EXPECT_EQ(decoder.dynamicTableSizeByteCount(), 0U);
        EXPECT_TRUE(decoder.errorMessage().empty());
        EXPECT_TRUE(decoder.decode(makeBytesFromHex("828684410f7777772e6578616d706c652e636f6d"), headerFields, &reason)) << reason;
        expectHeaderListEquals(headerFields, kFirstRequestHeaders);
    }
} // namespace AsynGyanis::Net
