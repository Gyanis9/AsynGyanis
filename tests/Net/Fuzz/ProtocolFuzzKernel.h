/**
 * @file ProtocolFuzzKernel.h
 * @brief 协议解码器的属性化模糊内核：可复现随机源、按目标造输入、以及一组「任意输入都必须成立」的不变量
 * @author Gyanis
 * @date 2026-09-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Net::Fuzz
{
    /**
     * @brief 被模糊的目标解码器
     *
     * @details 新增一个解码器就往这里加一项：随机驱动用例与 libFuzzer 入口都只跟着这一个枚举扩，
     *          不会出现「CI 跑了三个目标、本地用例只覆盖两个」的错位
     */
    enum class Target : std::uint8_t
    {
        Http1Request,      ///< HTTP/1.x 请求解析器（RFC 9112 §5/§6/§7.1：TCP 上最先被外部打到的一段，含分块正文与流水线）
        WebSocketFrame,    ///< WebSocket 增量帧解码器（RFC 6455 + 服务端侧的掩码/控制帧约束）
        Http2Frame,        ///< HTTP/2 增量帧解码器（RFC 7540 §4 + 本端上限）
        Http3Frame,        ///< HTTP/3 帧读取器（RFC 9114 §7 + varint 帧头 + 单帧上限）
        HpackBlock,        ///< HPACK 头块解码器（RFC 7541 + 动态表与头列表上限）
        QuicPacket,        ///< QUIC 报文头解码器（RFC 9000 §17；UDP 上最先被外部打到的一段，不需要任何密钥）
        QuicFrameSequence, ///< QUIC 帧序列解码器（§12.4/§19；解出的帧持有指向载荷的视图，视图越界即野指针）
        QuicParameters,    ///< QUIC 传输参数解码器（§7.3；来自对端 TLS 扩展，未识别项必须忽略）
        Count,             ///< 哨兵：目标总数，用于遍历，不是可解码的目标
    };

    /**
     * @brief 自带种子的线性同余发生器：固定种子保证失败可复现
     * @details 不用 std::random_device / std::mt19937：前者的输出不可复现（失败无法重放），后者状态大、
     *          播种成本高，而这里要的只是「同一种子 → 同一串字节」。各档的 gtest 用例与 libFuzzer 入口
     *          共用本类，避免各写一份而某天行为不一致。
     */
    class DeterministicRandom
    {
    public:
        explicit DeterministicRandom(const std::uint64_t seed) noexcept : m_state(seed)
        {
        }

        /// @brief 取下一个 32 位随机数
        [[nodiscard]] std::uint32_t next() noexcept
        {
            m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<std::uint32_t>(m_state >> 32U);
        }

        /// @brief 取 [0, bound) 内的随机数；bound 为 0 时返回 0
        [[nodiscard]] std::size_t nextBelow(const std::size_t bound) noexcept
        {
            return bound == 0 ? 0U : static_cast<std::size_t>(next()) % bound;
        }

    private:
        std::uint64_t m_state; ///< 当前状态
    };

    /**
     * @brief 把字节串渲染成可读文本，供失败信息用（不可打印字节转成 \\xNN）
     * @param text 原始字节
     * @return std::string 可安全打进日志与断言消息的文本
     */
    [[nodiscard]] std::string toEscapedText(std::string_view text);

    /**
     * @brief 为指定目标生成一份输入
     * @param target 目标解码器
     * @param random 随机源
     * @param maximumLength 输入长度上限（字节）
     * @return std::string 输入字节
     * @details **纯随机字节打不到解码器深处**（一个合法的 9 字节 h2 帧头随机撞出来的概率约 2^-64），
     *          因此这里先按该协议的结构拼出一个「像样的骨架」（帧头/操作码/长度前缀取合法或接近合法的值，
     *          长度字段偶尔与实际负载不符），再随机化负载与尾部字节。这样才有变异空间。
     */
    [[nodiscard]] std::string makeInput(Target target, DeterministicRandom &random, std::size_t maximumLength);

    /**
     * @brief 一轮驱动的产出统计，用于证明模糊本身没有退化成空转
     * @details 一套属性化模糊最坏的失效不是「没找到 bug」，而是**什么都没喂进去**：输入生成器退化、
     *          解码器提前返回、驱动少喂一段，都会让用例永远全绿。因此调用方把各轮的统计累计起来
     *          断言「既产出过帧、也判过错」，让空转变成红。
     */
    struct RunStats
    {
        std::size_t producedFrameCount{0}; ///< 本轮解出的帧数（HPACK 记解出的字段数）
        bool        isErrorEnd{false};     ///< 本轮是否以「拒绝」收场
    };

    /**
     * @brief 把一份输入喂给指定目标，并检查全部不变量
     * @param target 目标解码器
     * @param input 输入字节
     * @param stats 可选输出参数：本轮的产出统计（解出的帧/字段数、是否以拒绝收场），供调用方累计成防空转断言
     * @return std::string 空串表示全部成立；否则是第一条失败原因（不含输入，转义文本由调用方拼）
     *
     * @details 检查的六条不变量（每一条都是某个真实缺陷的形状，不是「别崩」而已）：
     *          - I1 结论域：解码器只能给出「还要数据 / 产出一帧 / 出错」三类结论，越界即失败；
     *          - I2 消费守恒与推进：单次消费的字节数不超过本次喂入；「还要数据」却不消费、
     *            或整体不推进（驱动步数触到上限）都算违例——后者会让模糊器比被测物先垮；
     *          - I3 分片无关：同一串字节整体喂与一次一字节喂，产出的帧序列与终态必须逐个相同
     *            （增量解码器最容易在字节边界上出错，本仓历史缺陷里就有一类是切在中间时的状态错位）；
     *          - I4 粘滞错误：进入错误态后继续喂任何字节，仍是同一错误且一个字节都不再消费；
     *          - I5 复位可用：在**同一个**解码器上 reset() 之后喂一份合法输入，必须能正常解出
     *            （新建一个对象会恰好绕过「粘滞没清干净」这一类缺陷，所以这里刻意复用）；
     *          - I6 不越权产出：错误态下不得交出任何帧，失败也不得把半截产出留在调用方的输出参数里。
     *
     *          各目标的适用面不同，按接口形状各自取用（这一列就是「哪条判据真的在跑」的清单）：
     *          HTTP/1 的请求解析器与 WebSocket / HTTP/2 同为 parse/takeFrame 形状的增量解码器，
     *          I1～I6 全查；差别只在「产出一条报文」之后必须像服务端那样 reset() 再继续喂剩余字节
     *          （流水线：一个字节都不吃地停在 Complete 是本层的契约，不是违例）；
     *          WebSocket 与 HTTP/2 是 parse/takeFrame 形状的增量解码器，I1～I6 全查；
     *          HTTP/3 的读取器是 feed/nextFrame 形状，没有「本次消费多少」的出口，故 I2 退成
     *          「步数上限 + 帧序不变」、I4 退成「判错之后的复查仍须报错」；
     *          HPACK 是一次性整块解码，没有分片语义，因此 I3 换成**确定性**（同一输入两个独立
     *          解码器必须给出逐个字段相同的结果），I4 落在 decode 的粘滞入口上，I6 是「失败必须清空输出」；
     *          QUIC 那三档（报文头 / 帧序列 / 传输参数）都是一次性 `std::expected` 出口的自由函数，
     *          没有分片与复位可言，因此 I3 同样换成确定性双跑，另外钉三条它们特有的：
     *          **成功产物的自洽**（长头标志与首字节高位一致、连接标识不超 20 字节、保留位确被挡下、
     *          帧序列两路口径逐帧相同）、**视图不越出载荷**（解出的帧指向载荷内部，指到外面就是野指针，
     *          ASan 只在真去读时才报，所以这里当场核偏移与长度）、**失败不交半截产出**（出参版必须清空）。
     */
    [[nodiscard]] std::string checkInvariants(Target target, const std::string &input, RunStats *stats = nullptr);

    /**
     * @brief 各目标在**本进程内**被真正解码过多少次
     * @details 计数点在 `checkInvariants` 里，因此 libFuzzer 入口与 gtest 的随机驱动共用同一份账：
     *          这份账回答的是「每一档解码器是不是都在被推」——`Target` 加了新项而没人走到它，
     *          外面完全看不出来（作业照常绿）。模糊侧在退出时把它打成一行机器可读的读数，
     *          gtest 侧有一条用例按目标逐个走一遍并核对增量
     * @return std::array<std::uint64_t, Target::Count> 下标即 `Target` 的序号
     */
    [[nodiscard]] std::array<std::uint64_t, static_cast<std::size_t>(Target::Count)> targetCallCounts();

    /**
     * @brief 该目标的「合法输入」样本，用于 I5（复位后可用）
     * @param target 目标解码器
     * @return std::string 至少能完整解出一帧的字节
     */
    [[nodiscard]] std::string validInput(Target target);

    /// @brief 目标的可读名字，用于用例名与失败信息
    [[nodiscard]] std::string_view targetName(Target target) noexcept;
} // namespace AsynGyanis::Net::Fuzz
