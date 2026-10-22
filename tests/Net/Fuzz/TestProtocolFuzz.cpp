// 自研协议解码器的属性化随机模糊：每一档解码器 × 各自那组不变量，固定种子保证失败可复现。
// 与 libFuzzer 入口（FuzzTargets.cpp）共用同一份内核，因此「本地跑绿」与「CI 持续模糊」判的是同一件事。
//
// 粘滞、复位与「失败不留半截产出」这几条判据已按突变法证过非恒绿（2026-09-24，各自临时改坏实现后转红、
// 复原后转绿；每条都报出了轮次、种子与转义输入）：
//   · WebSocket I4/I6「错误态没粘住，或还在消费字节」——去掉 parse 开头的 Failed 早退
//   · WebSocket I5「reset() 后合法输入解不出帧」——让 reset() 漏清 m_stage（留在 Failed）
//   · Http2  I4/I6 同上——把 Failed 的结论洗成 NeedMore（顺带一条事实：改成「重回帧头阶段」会让解码器
//     以 std::array 越界的硬断言收场，说明这条驱动确实喂到了状态机深处，而不是在门口打转）
//   · Http3  I4「复查 nextFrame 不再报错」——把粘滞错误改回「还差字节」
//   · Hpack  I6「decode 失败却留下了字段」——去掉失败路径上的 headerFields.clear()
// 两处口径要记下：① 只把 h3 的早退条件改成「几乎不成立」并不会转红，因为同样的坏字节会被重新判成
// 同一个错——这条钉的是「不再报错」这个可观测面，不是内部那个标记位；② HPACK 的 I5 与其它三条同形状，
// 没有单独再突变一次。
//
// 两档「吃不可信输入」的入口另按同一方法证过（2026-10-19，同样先转红再复原）：
//   · LogEscaping K1「折出的正文里有裸控制字节」——把可打印下界从 0x20 放宽到 0x0A，让裸换行过关。
//     这一改同时转红三条：本档随机轮、本档的已知形状自证、以及桩机构那族敌意 detail 的用例
//   · ConfigValueText「值与它自己写出的文本再解回来的值不等」——把含某键的对象写成只剩那一键。
//     教训记这条：只比两次文本的版本对这一改完全不响（那份文本自洽），所以判据加了「值也要相等」那一半；
//     反过来，给正文加一个尾空格这种改法两轮下来同形，本档判不了它，也不该判它
#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogEscaping.h"
#include "CommonTestSupport.h"
#include "Fuzz/ProtocolFuzzKernel.h"
#include "Net/Http/Auth/JwtVerifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace AsynGyanis::Net::Fuzz
{
    namespace
    {
        /// 每个目标的随机轮数默认值。每轮都含「整体喂 + 逐字节喂」两路驱动与一次复位探针。
        /// 实测（Debug+ASan，本机）：400 轮时各档合计 0.84 秒，2 万轮合计 15 秒（目标数会变，条数不写死）——
        /// 常驻防线取 400 轮图的是「每次都跑得完」，要加压就设 ASYN_FUZZ_ROUNDS（判同一批不变量，
        /// 且输入序列是默认跑的前缀）。上限 20 万轮：再多是 CI 超时而不是模糊。
        /// 另一条来自这里的教训：驱动不推进时，2 万轮能把进程撑到 4.49 GB 常驻（同一段字节被反复重喂），
        /// maximumDriveSteps 那道闸就是为这种「模糊器比被测物先垮」而加的
        constexpr int kDefaultRandomRoundCount = 400;

        /// 轮数上限：按 2 万轮 15 秒实测，20 万轮约 2.5 分钟（全部目标合计），够一次深度跑；
        /// 再多就是把作业变成超时，而不是把覆盖变深
        constexpr int kMaximumRandomRoundCount = 200000;

        /// 防空转断言生效所需的最小轮数。那两条断言判的是「整串输入的整体性质」（既解出过帧、也判过次错），
        /// 一两个样本不足以说明生成器退化——第 0 轮的输入恰好解不出帧是正常现象，不是缺陷。
        /// 因此低于这个轮数就明说不判，而不是把钳制下限抬到恰好能过的那个数
        constexpr int kAntiIdleMinimumRoundCount = 100;

        /**
         * @brief 取本轮实际用的轮数：ASYN_FUZZ_ROUNDS 覆盖默认值
         * @details 常驻跑 400 轮、临时加压用 `ASYN_FUZZ_ROUNDS=20000 ./TestNet --gtest_filter='ProtocolFuzz.*'`，
         *          两条路判的是同一批不变量——所以「本地撞出来的」与「CI 拦下来的」不会各说各话。
         *          变量缺失、空、含非数字、超出上限都回落到默认值或饱和：这是加压旋钮，不是被测契约，
         *          填错不该让用例变红（也因此不走 std::stoul——它对全数字但超范围的串会抛异常）。
         *          把轮数调到 kAntiIdleMinimumRoundCount 以下时，防空转那两条断言按下面的说明不判
         * @return int 生效轮数
         */
        int randomRoundCount()
        {
            const std::optional<std::string> overrideText = AsynGyanis::TestSupport::readEnvironmentVariable("ASYN_FUZZ_ROUNDS");
            if (!overrideText.has_value() || overrideText->empty())
            {
                return kDefaultRandomRoundCount;
            }

            std::uint64_t accumulated = 0;
            for (const char digit: *overrideText)
            {
                if (digit < '0' || digit > '9')
                {
                    return kDefaultRandomRoundCount;
                }
                // 饱和累加：再大的数字也只是「跑到上限」，不会被回绕成一个小值而悄悄少跑
                accumulated = std::min<std::uint64_t>(accumulated * 10U + static_cast<std::uint64_t>(digit - '0'), static_cast<std::uint64_t>(kMaximumRandomRoundCount));
            }
            return accumulated == 0U ? kDefaultRandomRoundCount : static_cast<int>(accumulated);
        }

        /// 单份输入的长度上限
        constexpr std::size_t kMaximumInputLength = 512;

        /// 各目标的固定种子：失败信息里带轮次与轮数，凭种子就能在同一台机器上重放同一串输入。
        /// 随机源是单调流，因此加压跑（ASYN_FUZZ_ROUNDS 调大）的前 N 轮与默认跑完全同序——
        /// 默认跑绿而加压跑红的轮次，把前面的输入原样搬进用例即可常驻
        constexpr std::uint64_t kSeedHttp1          = 20261010ULL;
        constexpr std::uint64_t kSeedWebSocket      = 20260924ULL;
        constexpr std::uint64_t kSeedHttp2          = 20260925ULL;
        constexpr std::uint64_t kSeedHttp3          = 20260926ULL;
        constexpr std::uint64_t kSeedHpack          = 20260927ULL;
        constexpr std::uint64_t kSeedQuicPacket     = 20261001ULL;
        constexpr std::uint64_t kSeedQuicFrames     = 20261002ULL;
        constexpr std::uint64_t kSeedQuicParameters = 20261003ULL;
        constexpr std::uint64_t kSeedQpack          = 20261012ULL;
        constexpr std::uint64_t kSeedPerMessage     = 20261013ULL;
        constexpr std::uint64_t kSeedHttpResponse   = 20261014ULL;
        constexpr std::uint64_t kSeedJwtCompact     = 20261018ULL;
        constexpr std::uint64_t kSeedConfigValue    = 20261019ULL;
        constexpr std::uint64_t kSeedLogEscaping    = 20261020ULL;

        /**
         * @brief 对某个目标跑一轮随机模糊
         * @param target 目标解码器
         * @param seed 随机源种子
         */
        void runRandomRounds(const Target target, const std::uint64_t seed)
        {
            DeterministicRandom random(seed);
            std::size_t         producedFrameCount      = 0;
            std::size_t         rejectedRoundCount      = 0;
            std::size_t         cookieDecodeHitCount    = 0;
            std::size_t         formDecodeHitCount      = 0;
            std::size_t         multipartDecodeHitCount = 0;
            std::size_t         jsonDecodeHitCount      = 0;
            const int           roundCount              = randomRoundCount();

            for (int round = 0; round < roundCount; ++round)
            {
                const std::string input = makeInput(target, random, kMaximumInputLength);
                RunStats          stats;
                const std::string violation = checkInvariants(target, input, &stats);
                if (!violation.empty())
                {
                    FAIL() << targetName(target) << " 第 " << round << " 轮违例：" << violation << "，输入（转义）=" << toEscapedText(input) << "，种子=" << seed
                           << "，轮数=" << roundCount;
                }
                producedFrameCount += stats.producedFrameCount;
                rejectedRoundCount += stats.isErrorEnd ? 1U : 0U;
                cookieDecodeHitCount += stats.cookieDecodeHitCount;
                formDecodeHitCount += stats.formDecodeHitCount;
                multipartDecodeHitCount += stats.multipartDecodeHitCount;
                jsonDecodeHitCount += stats.jsonDecodeHitCount;
            }

            // 防空转：既要真解出过帧，也要真判过错。只有前者说明输入太规整（打不到拒绝分支），
            // 只有后者说明输入生成器坏了（一路喂垃圾）——两种都让这套用例失去意义。
            // 低于 kAntiIdleMinimumRoundCount 时不判：那是整体性质，几轮样本撑不起结论
            if (roundCount >= kAntiIdleMinimumRoundCount)
            {
                EXPECT_GT(producedFrameCount, 0U) << targetName(target) << " 全程没解出过一帧：输入生成器或驱动退化";
                EXPECT_GT(rejectedRoundCount, 0U) << targetName(target) << " 全程没判过一次错：输入太规整，拒绝分支没被覆盖";
            }

            // HTTP/1 独有的一格：四个「已链进闭包」的解码器（Cookie / 表单 / multipart / JSON）必须真被
            // 推到过——只补调用而不产触发头时这四个数恒为 0，F-6b 的四个调用就等于空转
            if (target == Target::Http1Request && roundCount >= kAntiIdleMinimumRoundCount)
            {
                EXPECT_GT(cookieDecodeHitCount, 0U) << "全程没解出过一条 Cookie：生成器没产 Cookie 触发头，四个解码器的调用等于空转";
                EXPECT_GT(formDecodeHitCount, 0U) << "全程没解出过一个表单字段：urlencoded 的触发头或正文没配上";
                EXPECT_GT(multipartDecodeHitCount, 0U) << "全程没解出过一段 multipart：边界名与正文的成对关系断了";
                EXPECT_GT(jsonDecodeHitCount, 0U) << "全程没解出过一份 JSON 正文：content-type 或正文没配上";
            }
        }

        /**
         * @brief 对某个目标跑截断矩阵：合法输入的每一个前缀都必须「可解或还要数据」，不得崩、不得产出半截帧
         * @details 增量解码器的历史缺陷集中在截断点上（切在掩码键中间、帧头中间、长度域中间），
         *          前缀是打中这些位置最经济的方式
         */
        void runTruncationMatrix(const Target target)
        {
            const std::string valid = validInput(target);
            ASSERT_FALSE(valid.empty()) << targetName(target) << " 的合法输入样本为空，本用例无从判定";

            for (std::size_t length = 0; length <= valid.size(); ++length)
            {
                const std::string truncated = valid.substr(0, length);
                const std::string violation = checkInvariants(target, truncated);
                EXPECT_TRUE(violation.empty()) << targetName(target) << " 截断到 " << length << " 字节时违例：" << violation << "，输入（转义）=" << toEscapedText(truncated);
            }
        }
    } // namespace

    /**
     * @brief HTTP/1 请求解析器的不变量轮：TCP 上最先被外部打到的一段
     * @details 其余几档要么是「升级之后」要么换了一套分帧，只有这一段直接吃客户端的首字节，而且它的
     *          拒绝面是分层的（414/431/400/413 各归一类）。此前它不在模糊面里——CI 的 protocol-fuzz
     *          作业等于把最热的攻击面只交给了人写的用例。
     */
    TEST(ProtocolFuzz, Http1RequestParserKeepsInvariants)
    {
        runRandomRounds(Target::Http1Request, kSeedHttp1);
        runTruncationMatrix(Target::Http1Request);
    }

    TEST(ProtocolFuzz, WebSocketFrameDecoderKeepsInvariants)
    {
        runRandomRounds(Target::WebSocketFrame, kSeedWebSocket);
        runTruncationMatrix(Target::WebSocketFrame);
    }

    TEST(ProtocolFuzz, Http2FrameDecoderKeepsInvariants)
    {
        runRandomRounds(Target::Http2Frame, kSeedHttp2);
        runTruncationMatrix(Target::Http2Frame);
    }

    TEST(ProtocolFuzz, Http3FrameReaderKeepsInvariants)
    {
        runRandomRounds(Target::Http3Frame, kSeedHttp3);
        runTruncationMatrix(Target::Http3Frame);
    }

    TEST(ProtocolFuzz, HpackDecoderKeepsInvariants)
    {
        runRandomRounds(Target::HpackBlock, kSeedHpack);
        runTruncationMatrix(Target::HpackBlock);
    }

    TEST(ProtocolFuzz, QuicPacketHeaderDecoderKeepsInvariants)
    {
        runRandomRounds(Target::QuicPacket, kSeedQuicPacket);
        runTruncationMatrix(Target::QuicPacket);
    }

    TEST(ProtocolFuzz, QuicFrameDecoderKeepsInvariants)
    {
        runRandomRounds(Target::QuicFrameSequence, kSeedQuicFrames);
        runTruncationMatrix(Target::QuicFrameSequence);
    }

    TEST(ProtocolFuzz, QuicTransportParametersDecoderKeepsInvariants)
    {
        runRandomRounds(Target::QuicParameters, kSeedQuicParameters);
        runTruncationMatrix(Target::QuicParameters);
    }

    TEST(ProtocolFuzz, QpackDecoderKeepsInvariants)
    {
        runRandomRounds(Target::QpackBlock, kSeedQpack);
        runTruncationMatrix(Target::QpackBlock);
    }

    TEST(ProtocolFuzz, PerMessageDeflateKeepsInvariants)
    {
        runRandomRounds(Target::PerMessageDeflate, kSeedPerMessage);
        runTruncationMatrix(Target::PerMessageDeflate);
    }

    TEST(ProtocolFuzz, HttpResponseParserKeepsInvariants)
    {
        runRandomRounds(Target::HttpResponse, kSeedHttpResponse);
        runTruncationMatrix(Target::HttpResponse);
    }

    TEST(ProtocolFuzz, JwtCompactKeepsInvariants)
    {
        runRandomRounds(Target::JwtCompact, kSeedJwtCompact);
        runTruncationMatrix(Target::JwtCompact);
    }

    /**
     * @brief 配置/回复 JSON 档的不变量轮：一切远端与运维喂进来的文本进值模型的那一道门
     * @details 这一档吃的是配置文件、ACME 机构的正文、JWKS 正文与 HTTP 的 JSON 请求体——四路的共同点
     *          是「字节来自别人」。判据不要求它一定解得出东西，只要求解得出的那份必须能稳定地再走一圈回来。
     */
    TEST(ProtocolFuzz, ConfigValueTextKeepsInvariants)
    {
        runRandomRounds(Target::ConfigValueText, kSeedConfigValue);
        runTruncationMatrix(Target::ConfigValueText);
    }

    /**
     * @brief 折日志这一档的不变量轮：远端自由文本进本进程那一行之前的最后一道闸
     * @details 这一档不是解码器，而是一道「不许把控制字节带进日志」的折法：ACME 机构 problem document
     *          的 detail、其它实现的错误串、头部缺陷原文都从它过。它出错的方式不是崩，而是让一条记录在
     *          采集端变成两条、让行首被覆盖、或者让运维的终端收到一段转义序列——所以判据按字节逐个核。
     */
    TEST(ProtocolFuzz, LogEscapingKeepsInvariants)
    {
        runRandomRounds(Target::LogEscaping, kSeedLogEscaping);
        runTruncationMatrix(Target::LogEscaping);
    }

    /**
     * @brief 自证这一档会响：四份已知畸形令牌必须按契约被拒，而不是崩、也不是被宽容收下
     * @details 新加一档最怕两种假绿：生成器永远造不出能进到深处的输入（档在空转），
     *          或解析器把畸形输入当合法（真缺陷被吞）。前者由 EveryTargetDecodesAndIsCounted
     *          与上面的随机轮次管，这条管后者——每一格都对着 RFC 7519 §4 / base64url 无填充
     *          这条硬约定写死期望。
     */
    TEST(ProtocolFuzz, JwtCompactRejectsKnownMalformedTokens)
    {
        // 段数不对：两段
        EXPECT_FALSE(parseJwt("aGVsbG8.").has_value());
        // 填充符不属于 JWT 的字母表（'=' 是 base64 的，RFC 7519 明确不用）
        EXPECT_FALSE(parseJwt("aGVsbG8=.aGVsbG8=c2ln").has_value());
        // 头段解出来不是 JSON（"hello"），形状对但内容不合法
        EXPECT_FALSE(parseJwt("aGVsbG8.aGVsbG8.c2ln").has_value());
        // 签名段掺一个 '+'（base64 而非 base64url）
        EXPECT_FALSE(parseJwt(base64UrlEncode(R"({"alg":"RS256"})") + "." + base64UrlEncode(R"({"exp":1}")") + ".ab+c").has_value());

        // 反面对照：同一份生成路径下合法的令牌必须解得开，且声明校验真的产出过原因
        const std::string              goodHeader  = base64UrlEncode(R"({"alg":"RS256","typ":"JWT"})");
        const std::string              goodPayload = base64UrlEncode(R"({"iss":"签发者-甲","aud":"受众-乙","exp":4000000000})");
        const std::string              goodToken   = goodHeader + "." + goodPayload + "." + base64UrlEncode("signature-bytes");
        const std::optional<ParsedJwt> parsed      = parseJwt(goodToken);
        ASSERT_TRUE(parsed.has_value()) << "合法令牌被自己的生成路径拒了，那这一档根本什么都没测到";

        const std::chrono::system_clock::time_point now{std::chrono::seconds{1000000000}};
        EXPECT_TRUE(validateJwtClaims(parsed->payloadJson, JwtClaimExpectations{.issuer = "别人", .audience = ""}, now).has_value()) << "期望的 iss 不命中时校验不该放行";
        EXPECT_FALSE(validateJwtClaims(parsed->payloadJson, JwtClaimExpectations{.issuer = "签发者-甲", .audience = "受众-乙"}, now).has_value())
                << "期望全命中时校验不该给原因（exp 在 2096 年之前，nbf 未给出）";

        RunStats stats;
        EXPECT_TRUE(checkInvariants(Target::JwtCompact, goodToken, &stats).empty());
        EXPECT_GT(stats.producedFrameCount, 0U) << "合法令牌没被记成产出：这一档的账在 CI 上会是零";
    }

    /**
     * @brief 自证 ConfigValueText 这一档会响：畸形正文按契约拒掉，而不许把合法出口报成违例
     * @details 这一档最怕两种假绿：生成器永远造不出能进到解析器深处的输入，或者判据把实现自己的合法出口
     *          当成缺陷。第一种由上面的随机轮与 EveryTargetDecodesAndIsCounted 管；这一条管后者——
     *          `1e999` 是 libFuzzer 几秒内就会撞出来的形状，它必须按「语法不合法」被拒（实测 nlohmann 不把
     *          溢出的数字折成 ±Inf 收下），而判据那边要把它记成一次普通拒绝。判据里还留着「解析收下却
     *          序列化不出」那一格的放行：今天从 parse 走不到它，但真走到的那天它也不该是一条永远的红。
     */
    TEST(ProtocolFuzz, ConfigValueTextRejectsKnownMalformedDocuments)
    {
        // 语法不合法：一律「交回空」，不许抛也不许崩（parseConfigValue 走 allow_exceptions=false 的入口）
        for (const std::string_view malformed:
             {std::string_view{R"({"a":})"}, std::string_view{R"({)"}, std::string_view{R"([1,])"}, std::string_view{R"({"a":1,})"}, std::string_view{R"(NaN)"},
              std::string_view{R"({"a":"未闭合)"}, std::string_view{"\xFF\x41", 2}, std::string_view{R"(1e999x)"}, std::string_view{R"({"重复键":1,"重复键":2,"尾})"}})
        {
            EXPECT_FALSE(Base::parseConfigValue(malformed).has_value()) << toEscapedText(malformed);
            EXPECT_TRUE(checkInvariants(Target::ConfigValueText, std::string{malformed}).empty()) << "畸形文本本该按拒绝收场，而不是违例：" << toEscapedText(malformed);
        }

        // 非法 UTF-8 的正文（上面那格 \xFF\x41）与残缺文档都归「拒」；这里核的是另一格：注释只在宽松档进
        static constexpr std::string_view kCommented = R"(// 说明
{"a": 1})";
        EXPECT_FALSE(Base::parseConfigValue(kCommented).has_value()) << "HTTP 正文那一路按 RFC 8259 不吃注释";
        EXPECT_TRUE(Base::parseConfigValue(kCommented, true).has_value()) << "配置文件那一路要吃注释";

        // 收下却不可表示那一格走不到：溢出的数字在 parse 就被判成语法错，而不是折成 ±Inf 收进值模型
        EXPECT_FALSE(Base::parseConfigValue("1e999").has_value()) << "1e999 本该被拒；若哪天它被折成 ±Inf 收下，值模型就得按「不可表示」那一格记账";
        RunStats overflowStats;
        EXPECT_TRUE(checkInvariants(Target::ConfigValueText, "1e999", &overflowStats).empty()) << "被拒的输入不该判成违例";
        EXPECT_TRUE(overflowStats.isErrorEnd) << "这一格没记进「以拒绝收场」的账，防空转读数看不见它";

        // 这一档的裁判要先自证：「值也要相等」那一半若恒真，静默变形就永远看不见。
        // bool 与整数在值模型里必须是两个值——它们各自的文本都稳定，正是只比两次文本会漏的那一类
        const std::optional<Base::ConfigValue> asBool = Base::parseConfigValue("true");
        const std::optional<Base::ConfigValue> asInt  = Base::parseConfigValue("1");
        ASSERT_TRUE(asBool.has_value() && asInt.has_value());
        EXPECT_NE(*asBool, *asInt) << "值模型把 bool 与整数判成相等，那一半判据是恒真";

        // 反面对照：自洽文档要真的走通一圈并记账，否则上面那批「都别判违例」的断言等于恒真
        RunStats goodStats;
        EXPECT_TRUE(checkInvariants(Target::ConfigValueText, validInput(Target::ConfigValueText), &goodStats).empty());
        EXPECT_GT(goodStats.producedFrameCount, 0U) << "自洽文档没被记成产出：这一档的账在 CI 上会是零";
    }

    /**
     * @brief 自证 LogEscaping 这一档会响：三种「一行日志被拆开」的形状都必须变成可见转义
     * @details 折法失效的表现不是崩，而是日志看着完全正常而缺陷已经发生，所以这里既核折文本身，
     *          也核本档四条判据真的能认出这些形状（K1 那条裸控制字节的判据若在空转，随机轮也一样全绿）。
     *          形状用 ASCII 造：高位字节（含合法 UTF-8 的中文）也一律被折成 \xNN，那是折法写明的取舍。
     */
    TEST(ProtocolFuzz, LogEscapingFoldsKnownHostileShapes)
    {
        for (const char hostile: {'\n', '\r', '\x1B', '\x00', '\x7F'})
        {
            const std::string withControl = std::string("authority says") + hostile + "tail";
            const std::string folded      = Base::escapeForLog(withControl);
            EXPECT_EQ(folded.find(hostile), std::string::npos) << "控制字节原样进了折文：落到日志就是两条记录或一次终端转义";
            EXPECT_NE(folded.find("\\x"), std::string::npos) << "换成了可见转义才对，整段丢掉也是缺陷";

            RunStats stats;
            EXPECT_TRUE(checkInvariants(Target::LogEscaping, withControl, &stats).empty()) << "本档把自己的合法产出判成违例";
            EXPECT_GT(stats.producedFrameCount, 0U);
        }

        // 截断标记不许骗人：恰好等于上限时不贴，超一个字节才贴（贴错方向就是把「原文到此为止」当成事实）
        EXPECT_EQ(Base::escapeForLog("xxxxx", 5), "xxxxx");
        EXPECT_EQ(Base::escapeForLog("xxxxxx", 5), "xxxxx…（已截断）");
        // NUL 要折成字面的 \x00：按 C 字符串取日志的采集器会把后半截静默丢掉，原样带过去等于什么都没报
        EXPECT_EQ(Base::escapeForLog(std::string("ab") + '\0' + "cd"), std::string{R"(ab\x00cd)"});
    }

    /**
     * @brief 钉住：枚举里每一个目标都真的被解码到，且各自的账都记上了
     * @details 上面那批用例是「一个目标一条」的手写清单——`Target` 加了新项而没人给它写分支时，
     *          其余各条照旧全绿，新目标却在暗处一条都不跑（`Target` 的注释担心的正是这种错位）。
     *          这条用例按枚举遍历，因此新增目标会自动被纳入：合法输入必须解出东西
     *          （`producedFrameCount > 0`；HPACK 记的是字段数），否则就是「走通了却没解码」。
     *          同时核对进程内计数：漏 `default` 分支时会返回「无违例」，只看返回值发现不了
     */
    TEST(ProtocolFuzz, EveryTargetDecodesAndIsCounted)
    {
        for (std::size_t index = 0; index < static_cast<std::size_t>(Target::Count); ++index)
        {
            const auto target = static_cast<Target>(index);
            const auto name   = std::string(targetName(target));
            const auto before = targetCallCounts()[index];

            RunStats          stats;
            const std::string violation = checkInvariants(target, validInput(target), &stats);
            EXPECT_TRUE(violation.empty()) << name << "：合法输入被判违例：" << violation;
            EXPECT_GT(stats.producedFrameCount, 0U) << name << "：合法输入什么都没解出来——这个目标多半没有实现分支";
            EXPECT_EQ(targetCallCounts()[index], before + 1U) << name << "：调用没记进分目标的账，CI 那行读数会漏掉它";
        }
    }
} // namespace AsynGyanis::Net::Fuzz
