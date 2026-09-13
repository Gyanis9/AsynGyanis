/**
 * @file WebSocketFrame.h
 * @brief WebSocket（RFC 6455 §5）帧的编码与增量解码
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    // ============================================================================
    // WebSocket 帧编解码（RFC 6455 §5）
    //
    // 尚未接入会话：帧编解码只提供协议机制，谁调用编码器（握手成功后的发送路径）与谁把解码器
    // 接到连接读取循环上都还没做，因此引入本文件不会改变任何既有 HTTP 会话的行为。
    // ============================================================================

    /**
     * @brief WebSocket 帧的操作码（RFC 6455 §5.2）
     *
     * @details 0x0-0x2 是数据帧，0x8-0xA 是控制帧；0x3-0x7 与 0xB-0xF 保留未用，收到即判错。
     * @note 新增取值一律追加在末尾：这些数值直接对应线上帧首字节的低 4 位，改动既有取值
     *       等于改变协议本身。
     */
    enum class WebSocketOpCode : std::uint8_t
    {
        Continuation = 0x0, ///< 继续帧：分片消息的后续片段，不单独构成一条消息
        Text         = 0x1, ///< 文本帧：负载是 UTF-8 文本
        Binary       = 0x2, ///< 二进制帧：负载是任意字节
        Close        = 0x8, ///< 关闭帧：发起关闭握手，负载可选地含两字节状态码与原因
        Ping         = 0x9, ///< 心跳帧：对端应以 Pong 回应
        Pong         = 0xA  ///< 心跳应答帧
    };

    /**
     * @brief 一个完整的 WebSocket 帧
     *
     * @details 编码器的入参形态与解码器的产出形态共用同一个结构：编码时 isFinal 由调用方决定，
     *          解码时它恒为 true（见 WebSocketFrameDecoder）。
     */
    struct WebSocketFrame
    {
        WebSocketOpCode opCode{WebSocketOpCode::Text}; ///< 操作码；解码器只交出数据消息与完整控制帧，不会是 Continuation
        bool isFinal{true};                            ///< 是否消息末帧；解码器重组后才交付，因此恒为 true
        std::string payload;                           ///< 负载：Text 为文本，Binary 为任意字节，控制帧不超过 125 字节
    };

    /// 控制帧负载上限 125 字节（RFC 6455 §5.5）：控制帧要能塞进一个 IP 分片，故不随消息体积增长
    inline constexpr std::size_t kWebSocketMaximumControlPayloadLength = 125;

    /**
     * @brief 把一帧编码成线上的字节序列（服务端 → 客户端方向）
     *
     * @details 服务端发出的帧一律不置掩码位（RFC 6455 §5.1 只要求客户端加掩码）；负载长度按
     *          7 / 16 / 64 位三档编码，64 位档的最高位必须为 0（RFC 6455 §5.2）。
     * @param opCode 操作码
     * @param payload 负载字节，按「指针 + 长度」取，可以含 NUL 与任意二进制
     * @param isFinal 是否末帧；数据帧分片时后续片段要传 false 并改用 Continuation
     * @return std::string 完整帧字节（首字节 + 长度 + 无掩码负载），可直接写入连接
     * @throws Base::InvalidArgumentException 用法错误：控制帧负载超 125 字节、控制帧要求分片
     *         （isFinal 为 false），或 opCode 不是 RFC 6455 定义过的取值
     * @note 文本帧负载的 UTF-8 合法性不在本层校验：编码器只保证帧格式，内容语义由上层负责
     */
    [[nodiscard]] std::string encodeWebSocketFrame(WebSocketOpCode opCode, std::string_view payload, bool isFinal = true);

    /**
     * @brief 一次 WebSocketFrameDecoder::parse() 调用的结论状态
     *
     * @details 判定顺序固定为「先看是否已产出帧 → 再看是否已失败 → 其余为需要更多数据」，
     *          与 HttpParser 的 ParseStatus 同构且三态互斥穷尽。
     * @note Frame 是唯一的完成证据：取走之前再喂数据一字节不吃；Error 粘滞（除非 reset()），
     *       其中「超限」这一子类由 WebSocketFrameDecoder::isLimitExceeded() 区分。
     */
    enum class WebSocketDecodeStatus
    {
        NeedMore, ///< 数据不足，需要继续读取网络字节后再次调用 parse()
        Frame,    ///< 一帧（或一条重组后的消息）已就绪，此刻才允许调用 takeFrame()
        Error     ///< 对端违反 RFC 6455，解码器进入粘滞错误态，调用方应关闭连接
    };

    /**
     * @brief 服务端侧的 WebSocket 帧增量解码器
     *
     * @details 逐字节推进，任意字节边界都能切开续上（含切在掩码键中间、扩展长度中间、负载中间）。
     *          产出的是**完整消息**：分片消息在此重组后才交付，因此 takeFrame() 交出的帧 opCode
     *          绝不是 Continuation、isFinal 恒为 true；控制帧各自单独成帧。
     *
     * @note 服务端收到的帧必须带掩码（RFC 6455 §5.1），未掩码一律判错：掩码是这条连接上防止
     *       中间设施按 HTTP 报文缓存并重放帧内容的唯一防线。掩码按 4 字节循环异或解除。
     * @note 本实现不协商任何扩展，因此 RSV1/RSV2/RSV3 任一置位即判错。
     * @note 从严之处：RFC 6455 §5.4 允许控制帧插在分片消息中间，本实现拒绝——消息既然在此重组，
     *       放行插帧就会让「取帧顺序」与「消息到达顺序」不再是同一件事。
     *
     * @warning 负载按「指针 + 长度」处理，可含 NUL 与任意字节；文本帧的 UTF-8 合法性、
     *          Close 帧的状态码与原因文本都不在本层校验，由上层（会话）负责。
     */
    class WebSocketFrameDecoder
    {
    public:
        /// 单帧负载上限 1 MiB：单帧体量按「能一次放进内存也不拖垮服务端」取，更大的数据请用分片消息
        static constexpr std::size_t kMaximumFramePayloadLength = 1ull * 1024 * 1024;

        /// 分片消息重组后的总上限 8 MiB：与 HttpParser 的请求体上限同档，防止用无限分片撑爆内存
        static constexpr std::size_t kMaximumMessagePayloadLength = 8ull * 1024 * 1024;

        /**
         * @brief 构造解码器：全部状态为初态，可直接开始解码。
         */
        WebSocketFrameDecoder() = default;

        /**
         * @brief 析构函数：成员都是按值的标准容器，无额外资源需要回收。
         */
        ~WebSocketFrameDecoder() = default;

        // 禁拷贝：解码器内部持有「解码到一半」的状态与已产出待取走的帧，
        // 复制一份会把这个状态静默分叉成两份，而两边的字节流并不相同
        WebSocketFrameDecoder(const WebSocketFrameDecoder &) = delete;

        WebSocketFrameDecoder &operator=(const WebSocketFrameDecoder &) = delete;

        /**
         * @brief 喂入一段字节，推进解码
         *
         * @details 状态判定三步互斥且穷尽：已有产出待取走 → 直接返回 Frame 且一字节不吃；
         *          已失败 → 返回 Error 且一字节不吃；否则逐字节推进，一产出帧就停下——
         *          剩下的字节属于下一帧，由调用方按 consumedByteCount() 挪掉后重新喂入。
         * @param data 数据起始指针，调用方保证可读
         * @param length 数据长度，单位字节
         * @retval WebSocketDecodeStatus::Frame 一帧已就绪，等待 takeFrame() 取走
         * @retval WebSocketDecodeStatus::NeedMore 本段字节已全部消费，帧还没收齐
         * @retval WebSocketDecodeStatus::Error 对端违反 RFC 6455，粘滞到 reset() 为止
         * @see takeFrame(), consumedByteCount(), errorMessage(), isLimitExceeded()
         */
        WebSocketDecodeStatus parse(const char *data, std::size_t length);

        /**
         * @brief 取回最近一次 parse() 实际消费的字节数
         *
         * @details 这是「本帧到哪里结束」的唯一出处：返回 Frame 时，喂进去的数据里前
         *          consumedByteCount() 个字节属于这一帧，排在后面的属于下一帧（本次调用一个都没吃）。
         * @return std::size_t 已消费字节数；错误态、或已产出帧未取走时再次 parse() 时为 0
         */
        [[nodiscard]] std::size_t consumedByteCount() const;

        /**
         * @brief 取走已产出的帧，并清除待取走标记
         *
         * @details 取走后解码器才能接受下一次 parse()：产出与取走之间的数据一律不消费，
         *          否则剩下的字节会被当成下一帧的开头，把已产出的帧改坏。
         * @return WebSocketFrame 产出的帧，负载按移动移交（不额外拷贝）
         * @throws Base::LogicException 用法错误：当前没有待取走的帧（parse() 未返回过 Frame）
         */
        [[nodiscard]] WebSocketFrame takeFrame();

        /**
         * @brief 重置解码器状态，以便接着解码下一段字节流
         * @details 清空半成品帧、已产出待取走的帧与粘滞错误标记，回到初态。
         */
        void reset();

        /**
         * @brief 检查解码器是否处于错误状态。
         * @return true 表示发生过错误（含超出资源上限），false 表示无错误
         */
        [[nodiscard]] bool hasError() const;

        /**
         * @brief 检查本次错误是否由资源上限触发。
         * @details 与 hasError() 联合使用可从 Error 里分出「超限」这一子类：超限说明帧格式合法、
         *          只是体量越界，上层可据此回 1009（消息过大）而不是 1002（协议错误）。
         * @return true 表示错误由单帧上限或消息总上限造成
         */
        [[nodiscard]] bool isLimitExceeded() const;

        /**
         * @brief 获取错误信息描述（若有）。
         * @return 面向使用者的中文错误文本；无错误时为空串
         */
        [[nodiscard]] std::string errorMessage() const;

    private:
        /**
         * @brief 解码所处的阶段
         */
        enum class Stage
        {
            FirstByte,      ///< 正在收帧首字节（FIN、RSV 与操作码）
            LengthFirstByte,///< 正在收第二个字节（掩码位与 7 位长度）
            ExtendedLength, ///< 正在收 16 位或 64 位扩展长度（大端）
            MaskKey,        ///< 正在收 4 字节掩码键
            Payload,        ///< 正在收负载
            Failed          ///< 已失败：错误粘滞到 reset()
        };

        /**
         * @brief 校验帧首字节并进入长度阶段
         * @details RSV 位、操作码、控制帧不得分片，以及分片状态是否与操作码自洽都在这里判。
         * @param firstByte 帧首字节
         * @return true 合法
         */
        [[nodiscard]] bool acceptFirstByte(std::uint8_t firstByte);

        /**
         * @brief 校验第二个字节并定下负载长度
         * @param secondByte 帧的第二字节（掩码位 + 7 位长度）
         * @return true 合法
         */
        [[nodiscard]] bool acceptLengthFirstByte(std::uint8_t secondByte);

        /**
         * @brief 按定下来的负载长度校验单帧上限与消息总上限
         * @param payloadLength 本帧声明的负载长度，单位字节
         * @return true 未超限
         */
        [[nodiscard]] bool acceptPayloadLength(std::uint64_t payloadLength);

        /**
         * @brief 一帧的负载收齐：据此交付消息或继续累积分片
         */
        void completeFrame();

        /**
         * @brief 统一的失败记录：置粘滞错误态并补上中文前缀
         * @param isLimitExceeded 本次失败是否由资源上限触发
         * @param reason 中文失败详情（不含前缀）
         */
        void recordFailure(bool isLimitExceeded, std::string reason);

        /**
         * @brief 清空单帧的解析暂存（容器只清内容、保留容量，供下一帧复用）
         */
        void clearFrameScratch() noexcept;

        Stage m_stage{Stage::FirstByte};              ///< 当前阶段
        std::uint8_t m_opCodeValue{0};                ///< 本帧操作码的原始取值（长度校验要靠它区分控制帧）
        bool m_isFinal{true};                         ///< 本帧的 FIN 位
        std::uint64_t m_payloadLength{0};             ///< 本帧声明的负载长度，单位字节
        std::size_t m_extendedLengthByteCount{0};     ///< 扩展长度还需读的字节数（16 位档 2、64 位档 8）
        std::size_t m_extendedLengthBytesSeen{0};     ///< 扩展长度已读字节数
        std::array<std::uint8_t, 4> m_maskKey{};      ///< 本帧的 4 字节掩码键
        std::size_t m_maskKeyBytesSeen{0};            ///< 掩码键已读字节数
        std::size_t m_framePayloadBytesSeen{0};       ///< 本帧已收负载字节数（掩码按它循环取值）

        /// 负载落点：未分片时是本帧负载，分片消息进行中时是「已重组的部分」，
        /// 因此重组不需要第二份缓冲，也不会多一次拷贝
        std::string m_payloadBuffer;

        bool m_isFragmentedMessageInProgress{false};   ///< 是否正处在一条分片消息中间
        std::uint8_t m_fragmentedMessageOpCodeValue{0};///< 分片消息首帧的操作码，重组后作为整条消息的操作码
        WebSocketFrame m_pendingFrame;                 ///< 已产出待取走的帧
        bool m_hasPendingFrame{false};                 ///< 是否已有产出待取走

        bool m_hasError{false};                        ///< 是否已发生解码错误
        bool m_isLimitExceeded{false};                 ///< 本次失败是否由资源上限触发
        std::string m_errorMessage;                    ///< 面向使用者的中文错误描述
        std::size_t m_consumedByteCount{0};            ///< 最近一次 parse() 实际消费的字节数
    };
} // namespace AsynGyanis::Net
