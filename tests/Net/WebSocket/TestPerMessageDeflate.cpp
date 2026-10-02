// permessage-deflate 用例：扩展协商、消息往返、不可压内容与解压上限 往返一律逐字节比较；解压上限那条单独钉住 zip bomb——压缩比可以做到几百倍，
// 不设上限时一条小消息就能把服务端内存撑爆。
#include "Net/WebSocket/PerMessageDeflate.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 可压且够长的正文
        const std::string kRepetitivePayload = []
        {
            std::string payload;
            payload.reserve(8192);
            for (int index = 0; index < 256; ++index)
            {
                payload += "permessage-deflate RFC7692 payload/";
            }
            return payload;
        }();

        /**
         * @brief 造一段伪随机字节（几乎压不动）
         * @return std::string 字节
         */
        std::string makeIncompressiblePayload()
        {
            std::string bytes;
            bytes.reserve(4096);
            unsigned int state = 0x2545F491U;
            for (int index = 0; index < 4096; ++index)
            {
                state = state * 1664525U + 1013904223U;
                bytes.push_back(static_cast<char>((state >> 16) & 0xFF));
            }
            return bytes;
        }
    } // namespace

    /**
     * @brief 对端提供 permessage-deflate 时接受，并回本端选定的参数
     */
    TEST(PerMessageDeflate, AcceptsWhenClientOffersTheExtension)
    {
        const PerMessageDeflateNegotiation negotiation = negotiatePerMessageDeflate("permessage-deflate");

        ASSERT_TRUE(negotiation.accepted) << "对端提供了扩展却没收下";
        EXPECT_NE(negotiation.responseValue.find("permessage-deflate"), std::string::npos);
        // 两条 no_context_takeover 是本端选定的策略：要求每条消息重置上下文
        EXPECT_NE(negotiation.responseValue.find("server_no_context_takeover"), std::string::npos);
        EXPECT_NE(negotiation.responseValue.find("client_no_context_takeover"), std::string::npos);
    }

    /**
     * @brief 多个扩展并存时只挑出 permessage-deflate，参数也要能被跳过
     */
    TEST(PerMessageDeflate, FindsTheExtensionAmongOtherOffers)
    {
        EXPECT_TRUE(negotiatePerMessageDeflate("foo, permessage-deflate; client_max_window_bits, bar").accepted);
        EXPECT_TRUE(negotiatePerMessageDeflate("permessage-deflate;server_max_window_bits=10").accepted);
        // 大小写不敏感：扩展名是 token 语义
        EXPECT_TRUE(negotiatePerMessageDeflate("PerMessage-Deflate").accepted);
    }

    /**
     * @brief 对端没提供该扩展时不接受，也不回任何取值
     */
    TEST(PerMessageDeflate, RejectsWhenNotOffered)
    {
        EXPECT_FALSE(negotiatePerMessageDeflate("").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("foo, bar").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("x-permessage-deflate").accepted) << "撞名的扩展不该被认成目标扩展";
        EXPECT_TRUE(negotiatePerMessageDeflate("foo, bar").responseValue.empty());
    }

    /**
     * @brief 两个窗口参数按对端的声明钳本端，而不是读完就丢
     * @details 旧实现只看扩展名、`;` 之后的参数一个都不解析：对端说「我的解压器只能开 9 位」，
     *          本端照样按 15 位压，线上就是一帧发得出去、对端当场解不开报错收线。
     */
    TEST(PerMessageDeflate, ClampsLocalWindowsToPeerDeclarations)
    {
        // server_max_window_bits = 对端解压器的能力 → 本端压缩位数
        const PerMessageDeflateNegotiation narrowerPeerWindow = negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=10");
        ASSERT_TRUE(narrowerPeerWindow.accepted);
        ASSERT_TRUE(narrowerPeerWindow.window.has_value());
        EXPECT_EQ(narrowerPeerWindow.window->compressBits, 10);
        EXPECT_EQ(narrowerPeerWindow.window->decompressBits, 15) << "对端没提 client_max_window_bits，本端解压窗口按满档";
        EXPECT_NE(narrowerPeerWindow.responseValue.find("server_max_window_bits=10"), std::string::npos) << "选定的位数要回显，对端据此分配它自己的解压窗口";

        // client_max_window_bits = 对端压缩时用的位数 → 本端解压位数（按连接收小窗口就是收小内存）
        const PerMessageDeflateNegotiation narrowerClientWindow = negotiatePerMessageDeflate("permessage-deflate; client_max_window_bits=9");
        ASSERT_TRUE(narrowerClientWindow.window.has_value());
        EXPECT_EQ(narrowerClientWindow.window->decompressBits, 9);
        EXPECT_EQ(narrowerClientWindow.window->compressBits, 15);
        EXPECT_EQ(narrowerClientWindow.responseValue.find("client_max_window_bits"), std::string::npos)
                << "这条不回显：回显一个更低的值等于要求对端改小压缩窗口，而本端不需要那个收益";

        // 不带值的形态（RFC 7692 §7.1.1/§7.1.2）只表示对端能开满 15：不钳也不产生回显
        const PerMessageDeflateNegotiation valuelessOffer = negotiatePerMessageDeflate("permessage-deflate; client_max_window_bits");
        ASSERT_TRUE(valuelessOffer.window.has_value());
        EXPECT_EQ(valuelessOffer.window->decompressBits, 15);
        EXPECT_EQ(valuelessOffer.responseValue.find("server_max_window_bits"), std::string::npos);

        // 同名参数重复出现不在「钳本端」这一族里：本端无从判断对端声明的是哪个配置，
        // RFC 7692 §9.1 给的处置是婉拒整条要约（见 DeclinesOfferWithDuplicatedParameter）

        // 引号形态与大小写：参数名是 token 语义，值可以是 quoted-string（RFC 6455 §9.1 的扩展语法）
        const PerMessageDeflateNegotiation quoted = negotiatePerMessageDeflate("permessage-deflate; SERVER_MAX_WINDOW_BITS=\"12\"");
        ASSERT_TRUE(quoted.window.has_value());
        EXPECT_EQ(quoted.window->compressBits, 12);
    }

    /**
     * @brief 窗口参数无法履约时整条扩展不接受，而不是带着一个对端解不开的窗口开连接
     */
    TEST(PerMessageDeflate, DeclinesUnusableWindowParameters)
    {
        // RFC 7692 只承认 8..15 这八档；越界、带符号、非数字、长到该溢出的都算无法履约
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=7").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=16").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; client_max_window_bits=0").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=-1").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=abc").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=999999999999").accepted) << "位数超长要在累加过程中就判越界，不能让它先溢出";

        const PerMessageDeflateNegotiation declined = negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=7");
        EXPECT_TRUE(declined.responseValue.empty()) << "不接受就不该回任何取值";
        EXPECT_FALSE(declined.window.has_value()) << "不接受就不该留下一份窗口给收发点";

        // 两个端点各自合法
        EXPECT_TRUE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=8").accepted);
        EXPECT_TRUE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=15").accepted);
    }

    /**
     * @brief 钉住 §9.1「服务器必须婉拒」的一条：要约里出现了本扩展没定义的参数名
     * @details 此前不认识的参数被当「与本端无关、忽略即可」放过——那不是 §9.1 给的处置：本端无从知道
     *          对端多出来的那条要求要不要履约。婉拒不等于断连：101 里不回 Sec-WebSocket-Extensions，
     *          对端退回明文收发，连接照常可用。两条 no_context_takeover 是布尔参数，带值即落进
     *          「取值不合法」（§7.1.3/§7.1.4 里它们不带值）。
     */
    TEST(PerMessageDeflate, DeclinesOfferWithUndefinedParameter)
    {
        // 要约方向只定义了四条（§7.1.1–§7.1.4），第五条出现即婉拒
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=12; x_unknown").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; max_chunk_bits=8").accepted);
        // 两条 no_context_takeover 是布尔参数：带值即取值不合法
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; client_no_context_takeover=1").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_no_context_takeover=\"\"").accepted);

        const PerMessageDeflateNegotiation declined = negotiatePerMessageDeflate("permessage-deflate; x_unknown");
        EXPECT_TRUE(declined.responseValue.empty()) << "婉拒就不该回任何取值";
        EXPECT_FALSE(declined.window.has_value()) << "婉拒就不该留下一份窗口给收发点";

        // 正向对照：四条参数各自与组合都收得下——本端认识它们，且本端能履约
        EXPECT_TRUE(negotiatePerMessageDeflate("permessage-deflate; server_no_context_takeover; client_no_context_takeover").accepted);
        EXPECT_TRUE(negotiatePerMessageDeflate("permessage-deflate; server_no_context_takeover; server_max_window_bits=10; client_max_window_bits").accepted);
    }

    /**
     * @brief 钉住 §9.1「服务器必须婉拒」的另一条：同名参数重复出现
     * @details 同名写了两个取值时「取最严的那个」是替对端猜它到底声明了哪个配置，§9.1 给的处置是婉拒；
     *          取值相同也一样，这条要的是「只出现一次」。参数名按 token 语义比对，
     *          因此两种大小写写法仍算同一条参数写了两遍。
     */
    TEST(PerMessageDeflate, DeclinesOfferWithDuplicatedParameter)
    {
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=9; server_max_window_bits=15").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=9; server_max_window_bits=9").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; client_max_window_bits; client_max_window_bits=9").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; client_no_context_takeover; client_no_context_takeover").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("permessage-deflate; SERVER_MAX_WINDOW_BITS=9; server_max_window_bits=15").accepted);

        // 正向对照：四条参数各出现一次时不受影响
        EXPECT_TRUE(negotiatePerMessageDeflate("permessage-deflate; server_max_window_bits=10; client_max_window_bits=12; server_no_context_takeover; client_no_context_takeover")
                            .accepted);
    }

    /**
     * @brief 协商出来的窗口位数必须真的落到压缩参数上，而不是读完就丢
     * @details 正文取「4096 字节压不动的伪随机段重复两次」：满档窗口（32 KiB）能回溯 4096 字节、
     *          把后半几乎吃掉（实测 8192 → 4191），9 位窗口只有 512 字节、够不着那个距离，只能整段重发
     *          （实测 → 8230）。两者差出大半个正文，所以「位数没吃到」等于两个尺寸一样、这条就红。
     *          两次压缩在同一线程上连着做，因此这条同时钉住「复用流遇到位数不同必须重新 init」——
     *          沿用上一条连接的窗口，第二档会退回满档，尺寸差就消失了。
     *          没有把判据写成「越界就解不开」：本机 zlib 的裸 inflate 对更远的回溯是宽容的
     *          （实测窗口 9 也解得开 15 位产出的字节，8192 字节逐字节对得上），而按 RFC 7692 §7.1.2
     *          严格执行的对端会当场报错收线——那一半只有对端测得出，本端只能钉住自己这半。
     */
    TEST(PerMessageDeflate, CompressionWindowChangesTheEmittedBytes)
    {
        const std::string block   = makeIncompressiblePayload(); // 4096 字节、几乎压不动
        const std::string payload = block + block;               // 周期 4096 字节，远超 9 位窗口的 512

        const std::optional<std::string> wide   = deflateWebSocketMessage(payload, kWebSocketDefaultWindowBits);
        const std::optional<std::string> narrow = deflateWebSocketMessage(payload, 9);
        ASSERT_TRUE(wide.has_value()) << "满档压缩失败";
        ASSERT_TRUE(narrow.has_value()) << "9 位窗口压缩失败";
        EXPECT_GT(narrow->size(), wide->size() + payload.size() / 4) << "窗口位数没进到压缩参数里：两条流应当差出整个后半段";

        // 窄窗口产出的字节必须能被同档的解压器逐字节还原（协商两端同档时自洽）
        const std::optional<std::string> restored = inflateWebSocketMessage(*narrow, payload.size() + 1024, 9);
        ASSERT_TRUE(restored.has_value()) << "按对端声明的窗口压完，同档解压却解不开";
        EXPECT_EQ(*restored, payload);

        // 位数越界一律不出字节：宁可这一条不发压缩帧，也不发一段可能没人解得开的字节
        EXPECT_FALSE(deflateWebSocketMessage(payload, 16).has_value());
        EXPECT_FALSE(deflateWebSocketMessage(payload, 7).has_value());
        EXPECT_FALSE(inflateWebSocketMessage(*narrow, payload.size() + 1024, 16).has_value());
    }

    /**
     * @brief 压缩后再解压必须与原消息逐字节一致
     */
    TEST(PerMessageDeflate, RoundTripsMessages)
    {
        const std::string original = "permessage-deflate 往返：中文与 ASCII 混排，含空字节之前的普通文本";

        const std::optional<std::string> compressed = deflateWebSocketMessage(original);
        ASSERT_TRUE(compressed.has_value()) << "压缩失败";

        const std::optional<std::string> restored = inflateWebSocketMessage(*compressed, 64 * 1024);
        ASSERT_TRUE(restored.has_value()) << "解压失败";
        EXPECT_EQ(*restored, original);
    }

    /**
     * @brief 复用流按消息复位：不得把上一条消息的字典带进下一条（no_context_takeover 契约）
     * @details 实现改为复用 thread_local z_stream + 每条 deflateReset/inflateReset。若复位没清干净，
     *          同一条 payload 的第二次压缩会因吃到上一条的字典而字节变短、或与第一次不一致，解压侧同理串消息。
     *          这里把「复用 + 复位」钉成与「每条新建流」逐字节一致，正是这套复用最容易被破坏的不变式。
     */
    TEST(PerMessageDeflate, ReusesStreamAcrossMessagesWithoutContextTakeover)
    {
        const std::string repeated = "复用的 deflate 流不得把上一条的字典带进这一条：A/B/C 混排 + 空字节前的普通文本。";

        const std::optional<std::string> first  = deflateWebSocketMessage(repeated);
        const std::optional<std::string> second = deflateWebSocketMessage(repeated);
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(*first, *second) << "同一条 payload 连续两次压缩必须逐字节一致（证明按消息复位、无上下文接管）";

        // 交错压缩 + 解压多种长度（含空、含不可压），逐条往返一致，验证 inflate 侧复用同样不串
        const std::vector<std::string> payloads{std::string("甲"), std::string(4096, 'x'), std::string(), std::string("tail")};
        for (const std::string &payload: payloads)
        {
            const std::optional<std::string> compressed = deflateWebSocketMessage(payload);
            ASSERT_TRUE(compressed.has_value());
            const std::optional<std::string> restored = inflateWebSocketMessage(*compressed, 64 * 1024);
            ASSERT_TRUE(restored.has_value());
            EXPECT_EQ(*restored, payload);
        }
    }

    /**
     * @brief 空消息也要能往返（线上负载是合法的压缩字节）
     */
    TEST(PerMessageDeflate, RoundTripsEmptyMessage)
    {
        const std::optional<std::string> compressed = deflateWebSocketMessage("");
        ASSERT_TRUE(compressed.has_value());

        const std::optional<std::string> restored = inflateWebSocketMessage(*compressed, 64);
        ASSERT_TRUE(restored.has_value());
        EXPECT_TRUE(restored->empty());
    }

    /**
     * @brief 不可压内容同样往返一致
     */
    TEST(PerMessageDeflate, RoundTripsIncompressiblePayload)
    {
        const std::string payload = makeIncompressiblePayload();

        const std::optional<std::string> compressed = deflateWebSocketMessage(payload);
        ASSERT_TRUE(compressed.has_value());

        const std::optional<std::string> restored = inflateWebSocketMessage(*compressed, payload.size() + 16);
        ASSERT_TRUE(restored.has_value());
        EXPECT_EQ(*restored, payload);
    }

    /**
     * @brief 重复内容确实变小：确认走的是真压缩
     */
    TEST(PerMessageDeflate, ShrinksRepetitivePayload)
    {
        const std::optional<std::string> compressed = deflateWebSocketMessage(kRepetitivePayload);
        ASSERT_TRUE(compressed.has_value());
        EXPECT_LT(compressed->size(), kRepetitivePayload.size() / 10);
        EXPECT_EQ(*inflateWebSocketMessage(*compressed, kRepetitivePayload.size()), kRepetitivePayload);
    }

    /**
     * @brief 解压结果超过上限即拒绝：压缩比可以被放大成 zip bomb
     */
    TEST(PerMessageDeflate, RefusesToInflateBeyondTheOutputLimit)
    {
        const std::optional<std::string> compressed = deflateWebSocketMessage(kRepetitivePayload);
        ASSERT_TRUE(compressed.has_value());

        // 上限比原消息小一个字节：必须判超限而不是把内容全部解出来再截断
        EXPECT_FALSE(inflateWebSocketMessage(*compressed, kRepetitivePayload.size() - 1).has_value()) << "解压输出超过上限却仍然返回了内容";
        // 给足上限就正常
        EXPECT_TRUE(inflateWebSocketMessage(*compressed, kRepetitivePayload.size()).has_value());
    }

    /**
     * @brief 非法字节被拒绝：把随机字节当压缩负载传给解压器
     */
    TEST(PerMessageDeflate, RejectsInvalidCompressedBytes)
    {
        const std::string garbage = makeIncompressiblePayload();
        EXPECT_FALSE(inflateWebSocketMessage(garbage, 1024 * 1024).has_value()) << "随机字节被当成了合法压缩流";
    }
} // namespace AsynGyanis::Net
