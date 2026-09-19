/**
 * @file Http3Connection.h
 * @brief 一条 QUIC 连接上的 HTTP/3 协议状态机：流分类、控制流、帧交错规则、QPACK 接线与待发字节调度
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 位置与 HTTP/2 侧的 `Http2Connection` 对应：只管协议，不管业务，也不碰 socket 与事件循环。
 *          传输层以「流号 + 字节段」喂入、以同样的形状取走待发字节，因此本类对底下是 ngtcp2 还是
 *          自研状态机一无所知。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Net/Http3/Http3Error.h"
#include "Net/Http3/Http3Frame.h"
#include "Net/Http3/Http3HeaderValidation.h"
#include "Net/Http3/Qpack.h"

namespace AsynGyanis::Net
{
    /**
     * @brief 一条连接上的 HTTP/3 协议状态机
     *
     * @details 本端负责的四件事：把对端字节按流分类并解帧、按 §7.2 的交错规则与 §4 的头部规则判定
     *          合法性、把响应编成帧与 QPACK 头块、以及把攒下的待发字节按流轮转交出去。
     * @note 所有回调都在调用方所在线程上同步触发；本类不是线程安全的，一条连接一个实例。
     */
    class Http3Connection
    {
    public:
        /// 开一条本端发起的单向流并返回流号；失败返回 -1（控制流与两条 QPACK 流在构造时各开一条）
        using StreamOpener = std::function<std::int64_t()>;

        /// 一条流上待发字节的出口：endStream 为真表示这段之后本端在该流上收尾
        using StreamWriter = std::function<void(std::int64_t streamId, std::span<const std::uint8_t> data, bool endStream)>;

        /// 把已消费的字节归还给 QUIC 的接收窗口（本端在这条流上还可以再收多少）
        using StreamCrediter = std::function<void(std::int64_t streamId, std::size_t consumedByteCount)>;

        /**
         * @brief 交给上层的通知集合
         * @details 只传平类型（字符串视图与字节视图），帧结构与 QPACK 类型一律不外泄，
         *          这样上层换实现时不必跟着重编译。视图只在回调期间有效，要留就自己拷。
         */
        struct Callbacks
        {
            /// 解出一个头字段（含伪头，原样给出）：参数为流号、名、值
            std::function<void(std::int64_t streamId, std::string_view name, std::string_view value)> onHeaderField;

            /// 一个头块解完并已通过判定：上层此刻可以按方法/路径决定要不要提前派发
            std::function<void(std::int64_t streamId, bool isTrailers)> onHeaderBlockReceived;

            /// 收到请求正文的一段字节：参数为流号与字节视图
            std::function<void(std::int64_t streamId, std::span<const std::uint8_t> bytes)> onBodyBytes;

            /// 对端在该流上 END_STREAM：请求收全
            std::function<void(std::int64_t streamId)> onRequestEnded;

            /// 一条流的两侧都已结束：上层可以丢掉该流的状态
            std::function<void(std::int64_t streamId)> onStreamClosed;

            /// 本端判定该流出错并已放弃：参数为流号与线上错误码
            std::function<void(std::int64_t streamId, Http3ErrorCode errorCode)> onStreamReset;

            /**
             * @brief 收到的请求头部畸形（RFC 9114 §4.1.2）
             * @details 与 onStreamReset 分开：规范允许服务端在重置之前先回一个错误响应，而回什么状态码
             *          属业务层的事（本类不知道 400/431 的区别）。上层据此作答并结束该流；本类随后
             *          把这条流标记为已答复，不再产生任何事件。
             * @param streamId 出错的流
             * @param reason 中文原因，含命中的规则
             */
            std::function<void(std::int64_t streamId, std::string_view reason)> onMalformedRequest;

            /// 连接级收口：对端 GOAWAY、关键流被关或协议错误。参数为线上错误码与中文原因
            std::function<void(Http3ErrorCode errorCode, std::string_view reason)> onConnectionClosed;
        };

        /**
         * @brief 本端在 SETTINGS 里公布的能力
         */
        struct LocalSettings
        {
            std::size_t qpackMaximumTableCapacityByteCount{4096}; ///< 本端解码侧动态表容量上限（RFC 9204 §5.1）
            std::size_t qpackMaximumBlockedStreamCount{100};      ///< 本端允许同时阻塞的头块数（RFC 9204 §5.2）
            std::size_t maximumFieldSectionSizeByteCount{64U * 1024U}; ///< 本端愿收的最大头段字节数（RFC 9114 §4.2.2）
            std::size_t maximumFrameByteCount{8U * 1024U * 1024U};     ///< 单个帧能缓冲的上限，见 @warning
            bool isExtendedConnectEnabled{true};                  ///< 是否支持 RFC 9220 的扩展 CONNECT
        };

        /**
         * @brief 建立协议状态机并开出三条本端单向流
         * @param opener 单向流的开流口；为空即无法建立（isUsable() 为假）
         * @param writer 流数据出口；为空同上
         * @param crediter 接收窗口归还口；可为空（空表示不归还，正文一大就会用光窗口）
         * @param callbacks 通知集合；缺哪一项就不发哪一项通知，不因此失败
         * @param settings 本端能力，写进控制流的 SETTINGS 帧
         * @note 开流失败不抛异常：一条连接建不起 h3 不该把服务端拖垮，上层按 isUsable() 降级
         */
        Http3Connection(StreamOpener opener, StreamWriter writer, StreamCrediter crediter, Callbacks callbacks,
                        LocalSettings settings = {});

        ~Http3Connection() = default;

        Http3Connection(const Http3Connection &) = delete;
        Http3Connection &operator=(const Http3Connection &) = delete;

        /// 三条本端单向流是否都开出来并绑好了
        [[nodiscard]] bool isUsable() const noexcept;

        /// 连接是否已作废：作废之后除了取走最后的待发字节与销毁，没有别的合法动作
        [[nodiscard]] bool isBroken() const noexcept;

        /// 作废原因对应的线上错误码；未作废时为 H3_NO_ERROR
        [[nodiscard]] Http3ErrorCode connectionErrorCode() const noexcept;

        /// 作废原因的中文文本，供日志与 QUIC 关闭帧使用
        [[nodiscard]] const std::string &connectionErrorReason() const noexcept;

        /**
         * @brief 把对端在某条流上送来的字节交给协议层
         * @param streamId 流号（含对端发起的双向流与单向流）
         * @param data 本段字节
         * @param isEndStream 对端在这段之后收尾
         * @note 除 DATA 载荷之外的字节在这里就地归还接收额度：那部分是帧头或被丢弃的帧，
         *       本端确实消费掉了。DATA 载荷由 onBodyBytes 的接收方决定何时归还
         */
        void consumeStreamData(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /**
         * @brief 承载层报来「对端重置或停止了这条流」
         * @param streamId 被取消的流
         * @note 本类看不到 QUIC 层的 RESET_STREAM/STOP_SENDING，不告知就会留下流状态与挂起的头块
         */
        void noteStreamCancelledByPeer(std::int64_t streamId);

        /**
         * @brief 提交响应头
         * @param streamId 承载该响应的流（对端发起的双向流）
         * @param fieldLines 头字段，含 :status；伪头必须在最前（判定在 QPACK 编码之前做）
         * @param isEndOfStream true 表示这条响应没有正文，交完头就收尾
         * @return 成功返回空；失败返回错误，其类别即要写回 QUIC 的线上错误码
         */
        [[nodiscard]] std::expected<void, QpackError> submitResponseHead(std::int64_t streamId,
                                                                        const std::vector<QpackHeaderField> &fieldLines,
                                                                        bool isEndOfStream);

        /**
         * @brief 追加一段响应正文（编成 DATA 帧排进该流的待发队列）
         * @param streamId 流号
         * @param bytes 正文字节；空字节段只在 isEndStream 为真时有意义
         * @param isEndStream true 表示正文到此为止
         * @return 成功返回已收下的字节数；失败返回错误
         */
        [[nodiscard]] std::expected<std::size_t, QpackError> appendResponseBody(std::int64_t streamId,
                                                                                std::span<const std::uint8_t> bytes,
                                                                                bool isEndStream);

        /**
         * @brief 该流上还有多少字节没交给传输层
         * @param streamId 流号
         * @return 待发字节数；流不存在时为 0
         * @note 上层的有界缓冲闸门口径就用它：本端交完给传输层即视为已排空，重传由传输层负责
         */
        [[nodiscard]] std::size_t pendingOutputByteCount(std::int64_t streamId) const noexcept;

        /// 本端在某条流上是否已经收尾（尾字节的 endStream 已交出）
        [[nodiscard]] bool isLocalStreamFinished(std::int64_t streamId) const noexcept;

        /**
         * @brief 主动重置一条流：把待发字节丢掉并交给传输层去发 RESET_STREAM
         * @param streamId 流号
         * @param errorCode 线上错误码
         */
        void resetStream(std::int64_t streamId, Http3ErrorCode errorCode);

        /**
         * @brief 把各流攒下的待发字节按轮转交出去
         * @note 一次最多搬 kMaximumFlushRounds 轮，避免一条连接上转太久饿死别的连接
         */
        void flush();

        /// 对端在 SETTINGS 里公布的动态表容量，本端编码器据此决定能插多少
        [[nodiscard]] std::size_t peerTableCapacityByteCount() const noexcept;

        /// 对端是否声明支持扩展 CONNECT（RFC 9220 §3.2.1）
        [[nodiscard]] bool isExtendedConnectPermitted() const noexcept;

    private:
        /// 对端发起的单向流按类型前缀落到哪种角色上
        enum class PeerStreamKind
        {
            Unknown,      ///< 还没见到类型字节
            Control,      ///< 对端控制流
            QpackEncoder, ///< 对端编码器流：喂给本端解码器
            QpackDecoder, ///< 对端解码器流：喂给本端编码器
            Ignored,      ///< 未知类型：按 RFC 9114 §6.2.1 丢弃后续字节
        };

        /// 一条对端流的帧交错状态（控制流与请求流共用同一形状）
        struct StreamState
        {
            std::optional<Http3FrameReader> reader;          ///< 该流的帧读取器，上限按本端配置建
            std::unique_ptr<Http3HeaderValidator> validator; ///< 该流的消息头判定器：跨头段与尾段共用一份，才认得出「尾段必须在头段之后」
            std::uint64_t fedByteCount{0};                   ///< 交给该流读取器的字节总数
            std::uint64_t creditedByteCount{0};              ///< 已归还接收窗口的字节数，不含 DATA 载荷
            std::uint64_t receivedBodyByteCount{0};          ///< 已交出的 DATA 总长，与 content-length 比对
            std::uint64_t declaredContentLengthByteCount{0}; ///< 头段声明的正文长度
            bool hasContentLengthDeclaration{false};         ///< 头段是否声明了 content-length
            bool isHeaderSectionSeen{false};                 ///< 是否已收到过头段（DATA 必须排在它之后）
            bool isHeadRejected{false};                      ///< 头段已被判畸形并交回会话作答，后续字节只看不再解释
            bool isTrailersSeen{false};                      ///< 尾段只允许一个
            bool isBodyStarted{false};                       ///< 是否已收到 DATA：再来的头段就是尾段
            bool isPeerFinished{false};                      ///< 对端已 END_STREAM
            bool isLocalFinished{false};                     ///< 本端已收尾
            bool isAbandoned{false};                         ///< 已因错误重置，不再产生任何事件
        };

        /// 一条流的待发字节
        struct OutboundStream
        {
            std::string bytes{};     ///< 尚未交给传输层的字节
            bool isEndStream{false}; ///< 交完这些字节本端就在该流上收尾
        };

        /// 归类一条对端单向流：类型前缀可能跨多次交付，凑齐一个变长整数之后才分派
        void consumePeerUnidirectionalTypePrefix(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /// 处理对端控制流上的字节
        void consumeControlStreamBytes(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /// 处理请求流上的字节
        void consumeRequestStream(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /// 处理对端 QPACK 编码器流上的字节
        void consumeQpackEncoderStreamBytes(std::span<const std::uint8_t> data);

        /// 解出一个完整帧之后按流类型与交错规则处置；失败时已被处置，调用方只需停手
        std::expected<void, QpackError> handleRequestFrame(std::int64_t streamId, StreamState &state, const Http3Frame &frame);

        /// 处置对端控制流上的一个帧
        std::expected<void, QpackError> handleControlFrame(const Http3Frame &frame);

        /// 把一个解完的头段先整体判定、再逐字段交给上层
        [[nodiscard]] bool deliverFieldSection(std::int64_t streamId, StreamState &state,
                                               const std::vector<QpackHeaderField> &fields, bool isTrailers);

        /// 编码器流补齐了内容之后续解某个挂起流上的头段
        void deliverResumedFieldSection(std::int64_t streamId);

        /// 对端收尾之后按 content-length 与实际正文字数对账，并给出「请求收全」的通知
        void finishRequestStreamIfEnded(std::int64_t streamId, StreamState &state);

        /// 按读取器的进度归还接收额度；dataPayloadByteCount 是本次交出的 DATA 载荷，不在归还之列
        void creditConsumedBytes(std::int64_t streamId, StreamState &state, std::size_t dataPayloadByteCount);

        /// 把 QPACK 两侧产出的指令字节排进对应单向流的待发队列
        void queueQpackInstructions(std::string_view encoderBytes, std::string_view decoderBytes);

        /// 把「本端已处理但还没告知」的插入数以 Insert Count Increment 排进解码器流（RFC 9204 §4.4.3）
        void emitDecoderStreamIncrements();

        /// 把字节排进某条流的待发队列
        void queueOutboundBytes(std::int64_t streamId, std::string_view bytes, bool isEndStream);

        /// 应用对端 SETTINGS 里与 QPACK 有关的能力
        void applyPeerSettings(const Http3SettingsFrame &settingsFrame);

        /// 记下一条流的状态；不存在则建
        StreamState &streamStateFor(std::int64_t streamId);

        /// 头段畸形：交给上层决定回什么错误响应（RFC 9114 §4.1.2 允许先答再重置）
        void rejectRequestHead(std::int64_t streamId, StreamState &state, std::string_view reason);

        /// 本端刚把某条流的收尾字节交出去：记状态并在两侧都完时发关闭通知
        void noteLocallyFinishedStream(std::int64_t streamId);

        /// 两侧都完或已放弃时发关闭通知并摘状态
        void closeStreamIfDone(std::int64_t streamId);

        /// 作废连接：只记第一次的原因
        void breakConnection(Http3ErrorCode errorCode, std::string_view reason);

        /// 以流错误处置一条流：丢掉待发字节、标记放弃并通知上层（状态本身留到安全点回收）
        void failStream(std::int64_t streamId, Http3ErrorCode errorCode, std::string_view reason);

        /// 在不再持有该流引用的位置回收已放弃的流状态
        void pruneAbandonedStream(std::int64_t streamId);

        static constexpr std::size_t kMaximumFlushRounds = 64; ///< 一次 flush 最多搬多少段

        StreamOpener m_streamOpener;
        StreamWriter m_streamWriter;
        StreamCrediter m_streamCrediter;
        Callbacks m_callbacks;
        LocalSettings m_localSettings;

        std::optional<QpackEncoder> m_qpackEncoder; ///< 本端编码器：写自己的动态表，受对端公布容量约束
        std::optional<QpackDecoder> m_qpackDecoder; ///< 本端解码器：受本端公布的容量约束

        std::int64_t m_localControlStreamId{-1};  ///< 本端控制流号
        std::int64_t m_localEncoderStreamId{-1};  ///< 本端 QPACK 编码器流号
        std::int64_t m_localDecoderStreamId{-1};  ///< 本端 QPACK 解码器流号
        std::int64_t m_peerControlStreamId{-1};   ///< 对端控制流号
        std::int64_t m_peerEncoderStreamId{-1};   ///< 对端编码器流号
        std::int64_t m_peerDecoderStreamId{-1};   ///< 对端解码器流号
        std::map<std::int64_t, PeerStreamKind> m_peerStreamKinds;   ///< 已归类的对端单向流
        std::map<std::int64_t, std::string> m_peerStreamTypeBuffers; ///< 类型前缀还没收全的对端单向流的残留字节
        std::uint64_t m_peerMaximumBlockedStreamCount{0};            ///< 对端允许阻塞的头块数，编码器据此决定

        std::map<std::int64_t, StreamState> m_streams;     ///< 对端流（请求流与控制流）的状态
        std::map<std::int64_t, OutboundStream> m_outbound; ///< 各流的待发字节
        std::deque<std::int64_t> m_outboundOrder;          ///< 待发字节的轮转顺序，防止单条流独占一次 flush

        bool m_isPeerSettingsReceived{false}; ///< 对端控制流上是否已出现过 SETTINGS 帧
        std::uint64_t m_maximumPushId{0};     ///< 对端允许的最大推送标识；本服务端不推送，只记录
        bool m_isUsable{false};
        bool m_isBroken{false};
        Http3ErrorCode m_connectionErrorCode{Http3ErrorCode::NoError};
        std::string m_connectionErrorReason;
    };
} // namespace AsynGyanis::Net
