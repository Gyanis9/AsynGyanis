/**
 * @file Http2Connection.h
 * @brief HTTP/2 连接层状态机（RFC 7540 §3.5/§4/§5/§6/§8.1）：前奏与 SETTINGS 协商、头块拼接与请求语义校验、流状态与并发上限、响应发送与发送方向流控记账
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Frame.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    // ============================================================================
    // HTTP/2 连接层（RFC 7540 §3.5/§4/§5/§6/§8.1）
    //
    // 本层是纯状态机：不绑 socket、不接 HttpServer、不碰 ALPN。字节进 feedBytes()、字节出
    // takeOutgoingBytes()，请求与正文按事件交出；所有入口都不阻塞、不 co_await——等对端的响应
    // （窗口、SETTINGS ACK、PING）不由本层等待，驱动顺序见 Http2Connection 的类说明。
    // 帧层与 HPACK 层各管一段（单帧合法性、头块压缩），跨帧的流状态、窗口与语义校验都在本层。
    // ============================================================================

    /// 客户端前奏的 24 字节（RFC 7540 §3.5）：服务端必须先原样收到它，之后的字节才按帧解释
    inline constexpr std::string_view kHttp2ConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    /// 流控窗口的初值（RFC 7540 §6.9.2）：连接级窗口恒以它为初值，流级初值随 SETTINGS_INITIAL_WINDOW_SIZE 变化
    inline constexpr std::uint32_t kHttp2InitialWindowSizeByteCount = 65535;

    /// 流控窗口的上限（RFC 7540 §6.9.2）：任何窗口超过 2^31-1 一律判 FLOW_CONTROL_ERROR
    inline constexpr std::uint32_t kHttp2MaximumWindowSizeByteCount = 0x7FFFFFFFU;

    /**
     * @brief 一条流的状态（RFC 7540 §5.1 的状态机，本端不实现推送故没有 reserved 两态）
     *
     * @note 新增取值一律追加在末尾；本枚举会出现在上层的观测与日志里。
     */
    enum class Http2StreamState
    {
        Idle,              ///< 未开启：对端发出该流号的 HEADERS 才离开此态
        Open,              ///< 双向可收发
        HalfClosedRemote,  ///< 对端已 END_STREAM：本端仍可发响应，对端不得再发 DATA/HEADERS
        HalfClosedLocal,   ///< 本端已 END_STREAM：本端不再发正文，对端还能发它的正文
        Closed             ///< 已终止：RST_STREAM（任一端）或双向 END_STREAM
    };

    /**
     * @brief 整条连接的状态
     *
     * @note 新增取值一律追加在末尾。
     */
    enum class Http2ConnectionState
    {
        AwaitingPreface,   ///< 还在收 24 字节前奏，此刻不解释任何帧
        AwaitingSettings,  ///< 前奏已收齐、初始 SETTINGS 已发出，等对端自己的 SETTINGS
        Open,              ///< 协商完成：新流、正文、响应都可处理
        Closing,           ///< 关闭中：本端已发收尾 GOAWAY 或已收到对端 GOAWAY，不再受理新流，既有流继续收发
        Failed             ///< 违反协议或突破本端上限：粘滞错误态，按 errorCode() 收场
    };

    /**
     * @brief 一次 Http2Connection::feedBytes() 调用的结论
     *
     * @note 与 Http2FrameDecodeStatus 的区别：本层没有「一帧就绪」这一态——帧、请求、正文、待发
     *       字节都由 take*() 取走，feedBytes() 只回答「还要不要继续喂」与「是不是已经失败」。
     * @note 新增取值一律追加在末尾。
     */
    enum class Http2ConnectionFeedStatus
    {
        NeedMore, ///< 本段字节已消费完：可以继续读网络字节后再次喂入（不代表没有产出）
        Failed    ///< 连接进入粘滞失败态：写出 takeOutgoingBytes() 后按 errorCode() 收场
    };

    /**
     * @brief 一次响应发送（响应头或响应正文）的结论
     *
     * @details 响应发送的失败分两类，调用方要采取的处置完全不同：该流已被对端取消或关闭（停掉这一条流、
     *          连接继续服务其它流）与连接不可用（写出待发字节后收口整条连接）。用法错误单列，见 Rejected。
     * @note 新增取值一律追加在末尾。
     */
    enum class Http2ResponseSendStatus
    {
        Sent,                  ///< 已排入待发字节或该流的发送队列（窗口不足时留在队列里，等对端 WINDOW_UPDATE 后由本层续发）
        StreamNotWritable,     ///< 该流已不可写响应：不在账本里、已终止（对端 RST_STREAM 或双向 END_STREAM）；本条流不再需要响应，连接继续服务其它流
        ConnectionUnavailable, ///< 连接尚未完成协商或已失败：整条连接不可再用，调用方应写出待发字节后收口
        Rejected               ///< 本次调用参数或时序不合规（状态码越界、头名非法、已安排 END_STREAM 后又追加正文）：本条流因用法错误无法应答
    };

    /**
     * @brief 连接层配置：本端通告的 SETTINGS 参数与几项本地资源上限
     *
     * @details 每项后面的括号是取值来源：规范初始值取自 RFC 7540 §6.5.2，本端策略项是本实现选定的
     *          有限值（规范对这两项不设初值，等于「不限」，那会让对端能无限消耗本端内存）。
     * @note 构造 Http2Connection 时按值取走一份，没有运行期更换的入口：上限若中途变紧，同一条流的
     *       前后两段会按不同尺子判定。
     */
    struct Http2ConnectionConfiguration
    {
        std::uint32_t headerTableSize{kHpackDefaultDynamicTableSizeByteCount}; ///< SETTINGS_HEADER_TABLE_SIZE（§6.5.2 初值 4096），同时是本端 HPACK 解码器动态表的上限
        std::uint32_t enablePush{0};                        ///< SETTINGS_ENABLE_PUSH（§6.5.2 初值 1）：本片不实现推送，取 0 如实告知对端不必预留
        std::uint32_t maximumConcurrentStreams{100};        ///< SETTINGS_MAX_CONCURRENT_STREAMS（§6.5.2 初值不限）：本端策略上限，超出回 REFUSED_STREAM
        std::uint32_t initialWindowSize{kHttp2InitialWindowSizeByteCount}; ///< SETTINGS_INITIAL_WINDOW_SIZE（§6.5.2 初值 65535）：本端允许对端每条流先发的字节数
        std::uint32_t maximumFrameSize{kHttp2DefaultMaximumFrameSize};     ///< SETTINGS_MAX_FRAME_SIZE（§6.5.2 初值 16384，合法区间 [16384, 16777215]）：本端可接收的单帧负载上限
        std::uint32_t maximumHeaderListSize{16U * 1024U};   ///< SETTINGS_MAX_HEADER_LIST_SIZE（§6.5.2 初值不限）：本端策略上限，算式按 §6.5.2 的「名长 + 值长 + 32」
        std::size_t maximumHeaderBlockByteCount{16U * 1024U}; ///< 本端策略：单个头块（HEADERS 与其 CONTINUATION 片段之和）的压缩后字节上限，防对端用无限 CONTINUATION 撑内存
        std::size_t maximumTotalConsumedByteCount{0};       ///< 本端策略：本连接累计消费字节上限，0 表示不限；开着时超过即按 ENHANCE_YOUR_CALM 收场
    };

    /**
     * @brief 连接层交出的一个请求（RFC 7540 §8.1.2）
     *
     * @details 只装 HTTP/2 语义：四个伪头各自成字段、普通头部按到达顺序原样保留（可重复头部不合并），
     *          因此未知方法原文与 :scheme 都不会在交接中丢失。接线层再用一个显式映射把它转成
     *          HttpRequest（:authority → host、:path → uri、「HTTP/2」→ 版本），两边语义都不被改写。
     */
    struct Http2Request
    {
        /**
         * @brief 取一条普通头部的值，按到达顺序取第一条命中的
         * @param name 头名，小写（本层已按 §8.1.2 校验过大小写）
         * @return 指向值的指针；没有该头部时返回 nullptr
         * @note 返回的是本对象内部字段的地址，随本对象一并失效
         */
        [[nodiscard]] const std::string *findHeaderValue(std::string_view name) const noexcept;

        std::uint32_t streamId{0};                  ///< 该请求所属的流号，回响应时按它定位
        std::string method;                         ///< :method 原文（未知方法原样保留）
        std::string scheme;                         ///< :scheme 原文；CONNECT 请求为空
        std::string path;                           ///< :path 原文；CONNECT 请求为空
        std::string authority;                      ///< :authority 原文；对端没带时为空
        std::vector<HpackHeaderField> headerFields; ///< 普通头部，按到达顺序，名已校验为小写
        bool hasBody{false};                        ///< 请求头未带 END_STREAM：正文会随 takeReceivedData() 交出
    };

    /**
     * @brief 从对端收到的一段正文（DATA 帧的应用数据）
     */
    struct Http2ReceivedData
    {
        std::uint32_t streamId{0}; ///< 数据所属的流号
        std::string data;          ///< 应用数据；空串表示零长 DATA 帧（§6.1 允许，常见于带 END_STREAM 的收尾帧）
        bool endStream{false};     ///< 对端在这片数据上置了 END_STREAM：该流的对端方向到此为止
        std::size_t flowControlByteCount{0}; ///< 本片占用的流控字节数：DATA 帧负载原长（含 padding，§6.9.1 要求 padding 也计入）
    };

    /**
     * @brief HTTP/2 连接层状态机（服务端角色）
     *
     * @details 一个实例持有这条连接的全部协议状态。上层按「读事件 → feedBytes() → takeOutgoingBytes()
     *          写出 → takeRequests()/takeReceivedData() 交给业务 → 业务调 sendResponse*() → 再
     *          takeOutgoingBytes()」驱动：本层既不阻塞也不 co_await，窗口不足的数据留在发送队列里，
     *          收到 WINDOW_UPDATE 后由本层在同一入口内续发，上层只需在每次入口调用后再取一次字节。
     *          接收方向则由上层回报消费量（creditReceivedData()），窗口更新帧由本层排进待发字节。
     *
     * @note 流状态判定表（RFC 7540 §5.1、§5.1.1）：从未开启（idle）的流只接受 HEADERS 与 PRIORITY，
     *       其余帧（DATA/WINDOW_UPDATE/RST_STREAM/CONTINUATION）判连接错误 PROTOCOL_ERROR；新流号必须
     *       为奇数且严格大于所有已用过的流号，偶数或倒退一律连接错误 PROTOCOL_ERROR。
     * @note 已终止的流分两种处置：本端或对端 RST_STREAM 掉的流，其上的帧一律忽略（对端可能还没看到
     *       RST_STREAM，回敬 RST 只会变成风暴，§5.1「closed」段允许忽略）；双向 END_STREAM 正常终止的
     *       流，只有 WINDOW_UPDATE/RST_STREAM/PRIORITY 可忽略（§5.1 明确要求），其余按连接错误
     *       STREAM_CLOSED 收场。
     * @note 半关（remote）的流再收到 DATA 或 HEADERS 是流错误 STREAM_CLOSED：RST_STREAM 该流，连接继续
     *       服务其它流；请求语义不合规（伪头、连接特定头、头名大写等）同样是流错误 PROTOCOL_ERROR。
     *
     * @warning 连接进入失败态后不再消费任何字节：调用方必须写出 takeOutgoingBytes() 里的 GOAWAY（错误码
     *          即 errorCode()）后收场，继续喂字节只会一直得到 Failed。
     * @warning 接收方向流控由调用方驱动：takeReceivedData() 交出正文后，调用方必须对每片调用
     *          creditReceivedData()（按 flowControlByteCount 报量），否则对端的发送窗口耗尽后会
     *          停在半途等窗口，大请求永远收不完。本层不做尾部头块的内容解释、不建优先级树、
     *          不实现服务端推送；超过窗口的 DATA 按 §6.9.1 判连接错误 FLOW_CONTROL_ERROR。
     */
    class Http2Connection
    {
    public:
        /**
         * @brief 构造连接状态机：本端取一份配置，协议状态回到「等前奏」
         * @param configuration 连接层配置；默认值对应规范的初始 SETTINGS 取值与本端策略上限
         * @throws Base::InvalidArgumentException 配置错误：enablePush 不是 0/1，或 maximumFrameSize
         *         不在 [16384, 16777215] 内——后者会被本端通告成一个非法的 SETTINGS_MAX_FRAME_SIZE
         */
        explicit Http2Connection(Http2ConnectionConfiguration configuration = {});

        /**
         * @brief 析构函数：成员都是按值的标准容器，无额外资源需要回收。
         */
        ~Http2Connection() = default;

        // 禁拷贝：本对象持有 HPACK 编解码器与流账本，复制一份会让两份动态表各自演进，
        // 后续索引在两边指向不同条目，且两条连接的字节流并不相同
        Http2Connection(const Http2Connection &) = delete;

        Http2Connection &operator=(const Http2Connection &) = delete;

        /**
         * @brief 喂入一段网络字节，推进状态机
         *
         * @details 前奏按字节比对（不满 24 字节就是 NeedMore），之后交给帧解码器逐帧处理；每一帧都会被
         *          就地解释——协商、拼头块、校验请求、记账窗口，并把要回的帧追加进待发字节。
         * @param data 数据起始指针；length 为 0 时可以是空指针
         * @param length 数据长度，单位字节
         * @return Http2ConnectionFeedStatus NeedMore 表示本段已消费完（产出用 take*() 取），
         *         Failed 表示连接已进入粘滞失败态（本段剩余字节不再消费）
         * @note 同一个入口处理任意切分方式：逐字节喂与一次性喂对协议状态的影响完全一致
         */
        Http2ConnectionFeedStatus feedBytes(const char *data, std::size_t length);

        /**
         * @brief 取走待发字节（含初始 SETTINGS、ACK、RST_STREAM、GOAWAY、响应头与正文）
         * @return std::string 自上次取走以来累积的全部字节，按生成顺序拼接；没有待发字节时为空串
         * @note 取走即清空；上层在每次 feedBytes() 与每个响应入口调用之后都应当调用它一次
         */
        [[nodiscard]] std::string takeOutgoingBytes();

        /**
         * @brief 取走已校验通过的请求
         * @return std::vector<Http2Request> 按到达顺序排列的请求；没有新请求时为空
         * @note 取走即清空。请求在头块的 END_HEADERS 收齐、语义校验通过后才出现；同一流后续的正文
         *       走 takeReceivedData()
         */
        [[nodiscard]] std::vector<Http2Request> takeRequests();

        /**
         * @brief 取走已收到的正文片段
         * @return std::vector<Http2ReceivedData> 按到达顺序排列的片段；没有新片段时为空
         * @note 取走即清空。零长 DATA 帧同样会出现（可能只为了带 END_STREAM）
         * @note 取走只是「交付」，不代表消费：调用方处理完每片之后必须调 creditReceivedData()
         *       把窗口还回去，见该方法的说明
         */
        [[nodiscard]] std::vector<Http2ReceivedData> takeReceivedData();

        /**
         * @brief 报告已消费的对端正文，按量把接收窗口还回去
         *
         * @details 调用方消费掉 takeReceivedData() 交出的片段后调用它（每片一次），本层据此把连接级
         *          与流级接收窗口补回（RFC 7540 §6.9.1），窗口因此不会随请求体量单调耗尽。
         *          为了不逐帧回敬，累计未还的字节达到该窗口初始值的一半才发一次 WINDOW_UPDATE。
         *
         * @param streamId 正文所属的流号，取自 Http2ReceivedData::streamId
         * @param byteCount 本次消费的字节数，取 Http2ReceivedData::flowControlByteCount（含 padding）
         * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 已记入接收窗口（可能还没到发 WINDOW_UPDATE 的阈值）
         * @return false 用法错误，没有改动任何状态：连接已失败或已进入失败态前不可用，或
         *         byteCount 超过窗口上限 2^31-1（原因写在 errorText 里）
         * @note 该流已经终止（对端 RST 或双向 END_STREAM）时只还连接级窗口：对已终止流的流级
         *       WINDOW_UPDATE 会被对端按 §5.1 忽略，发了也没有意义
         */
        [[nodiscard]] bool creditReceivedData(std::uint32_t streamId, std::size_t byteCount, std::string *errorText = nullptr);

        /**
         * @brief 本端发起收尾：发一个 NO_ERROR 的 GOAWAY，通告此后不再受理新流
         *
         * @details 通告里带的是本端已处理的最大对端流号（§6.8）：该号之前的流本端照旧做完，之后新开的
         *          流一律回 RST_STREAM REFUSED_STREAM，对端据此可以放心在新连接上重试。
         *          本层不因这次通告关闭连接，也不等待既有流——收口时机由调用方决定。
         * @param reason 中文原因，写进 GOAWAY 的调试数据，供对端排查
         * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 已把 GOAWAY 排进待发字节，连接转入 Closing
         * @return false 没有写入任何字节：连接尚未完成协商（还在等前奏或对端 SETTINGS）、已经在关闭中、
         *         或已进入失败态（连接错误的 GOAWAY 已经发过一次），原因写在 errorText 里
         */
        [[nodiscard]] bool sendGoAway(std::string_view reason, std::string *errorText = nullptr);

        /**
         * @brief 本端主动按连接错误收口：把带 errorCode 的 GOAWAY 排进待发字节并转入失败态
         *
         * @details 用于本端判定连接不能继续的情形（例如对端在约定时限内没有 ACK 本端 SETTINGS，
         *          RFC 7540 §6.5.3 的 SETTINGS_TIMEOUT）。与收尾通告 sendGoAway() 的区别在错误码与
         *          连接去向：本方法带非 NO_ERROR 的错误码，并把连接置为粘滞失败态（此后不再解释字节）。
         * @param errorCode 要回给对端的错误码；不能取 NoError（§7 的 NO_ERROR 属收尾通告，请改用 sendGoAway()）
         * @param reason 中文原因，同时是 GOAWAY 的调试数据
         * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 已把 GOAWAY 排进待发字节，连接转入 Failed
         * @return false 没有写入任何字节：errorCode 是 NoError，或连接已经失败过（GOAWAY 只发一次）
         */
        [[nodiscard]] bool failConnection(Http2ErrorCode errorCode, std::string_view reason, std::string *errorText = nullptr);

        /**
         * @brief 请对端无错地中止某条流的发送：发 RST_STREAM(NO_ERROR) 并终止该流
         *
         * @details 用于「响应已经完整发出、不再需要请求正文」的情形——最典型的是请求正文超限后的 413：
         *          RFC 9113 §8.1 明确允许服务端在发完完整响应后，用一个 NO_ERROR 的 RST_STREAM
         *          请对端中止发送请求正文，本端因此不必把剩余的字节白收一遍再丢掉。与 failStream()
         *          的区别只在错误码与语义：那条路是「这条流出错了」，本条是「这条流不需要了」。
         * @param streamId 目标流号
         * @param reason 中文原因，只写进 lastStreamErrorMessage() 供排查（不发给对端）
         * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 已把 RST_STREAM 排进待发字节并终止该流
         * @return false 没有写入任何字节：连接状态不允许收发、流不在账本里或已经终止
         */
        [[nodiscard]] bool abortStream(std::uint32_t streamId, std::string_view reason, std::string *errorText = nullptr);

        /**
         * @brief 本端初始 SETTINGS 是否还没被对端 ACK
         * @details 对端在约定时限内一直不回 ACK 属连接错误（RFC 7540 §6.5.3 的 SETTINGS_TIMEOUT）；
         *          上层据此把连接的空闲截止时间切到握手期专项限额，超时即收口。
         * @return true 本端已发出 SETTINGS 且还没收到它的 ACK
         * @return false 初始 SETTINGS 还没发（未收齐前奏），或已经被 ACK
         */
        [[nodiscard]] bool hasSettingsAwaitingAcknowledgement() const noexcept;

        /**
         * @brief 取最近一次发出 SETTINGS 的时刻
         * @return std::chrono::steady_clock::time_point 发 SETTINGS 的时刻；从没发过时返回默认时刻
         *         （steady_clock 的起点，对任何「现在」都为过去），供上层算 SETTINGS 超时
         * @note 用 steady_clock 而非系统时钟：超时判定不能因为系统时间被改写而提前或滞后
         */
        [[nodiscard]] std::chrono::steady_clock::time_point lastSettingsSentTime() const noexcept;

        /**
         * @brief 在某条流上发响应头
         *
         * @details 头列表按「:status 在最前（§8.1.2.1 伪头必须先于普通头部）+ 调用方给的字段」交给
         *          HpackEncoder 编码，再按对端通告的 MAX_FRAME_SIZE 切成 HEADERS 与若干 CONTINUATION
         *          （§4.3、§6.10）；endStream 置位时 END_STREAM 落在 HEADERS 上，该流随即进入半关（local）。
         * @param streamId 目标流号，必须是本连接账本里仍可发响应的流
         * @param statusCode 响应状态码，取值 100..999
         * @param headerFields 除 :status 外的响应头，名必须全小写且不含连接特定头（§8.1.2）
         * @param endStream 响应是否到此结束（无正文）
         * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
         * @return Http2ResponseSendStatus Sent 已排入待发字节（窗口不影响头块，HEADERS 不受流控）；
         *         StreamNotWritable 该流已被对端取消或已终止（停掉这条流即可，连接继续）；
         *         ConnectionUnavailable 连接尚未协商完成或已失败；Rejected 状态码或头名不合规
         * @note 返回非 Sent 时不写入任何字节；调用方按返回值决定是只停这条流还是收口整条连接，
         *       不要只看 errorText（原因文本只供日志与排查）
         */
        [[nodiscard]] Http2ResponseSendStatus sendResponseHeaders(std::uint32_t streamId, std::uint32_t statusCode,
                                                                  const std::vector<HpackHeaderField> &headerFields, bool endStream,
                                                                  std::string *errorText = nullptr);

        /**
         * @brief 在某条流上发响应正文
         *
         * @details 字节先进该流的发送队列，再按「连接级窗口、流级窗口、对端 MAX_FRAME_SIZE」三者取小
         *          尽量出帧（§5.2.2、§6.9）；窗口不足的部分留在队列里，等对端 WINDOW_UPDATE 进来后由
         *          feedBytes() 续发。endStream 置位时 END_STREAM 只落在队列排空后的最后一帧上，因此
         *          窗口不足时不会提前把流半关掉。
         * @param streamId 目标流号，必须是本端仍可发正文的流（Open 或 HalfClosedRemote）
         * @param data 正文片段，按「指针 + 长度」取，可含 NUL 与任意二进制
         * @param endStream 本片之后本端不再发正文（本片可能因窗口不足尚未出帧）
         * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
         * @return Http2ResponseSendStatus Sent 已排入待发字节或发送队列（可能一帧都没出，需等 WINDOW_UPDATE）；
         *         StreamNotWritable 该流已被对端取消或已终止（停掉这条流即可，连接继续）；
         *         ConnectionUnavailable 连接尚未协商完成或已失败；Rejected 本端已收尾或已安排 END_STREAM
         * @note 返回非 Sent 时不改动任何状态；调用方按返回值决定是只停这条流还是收口整条连接
         */
        [[nodiscard]] Http2ResponseSendStatus sendResponseData(std::uint32_t streamId, std::string_view data, bool endStream,
                                                               std::string *errorText = nullptr);

        /**
         * @brief 取当前连接状态
         * @return Http2ConnectionState 连接状态；失败态之后恒为 Failed
         */
        [[nodiscard]] Http2ConnectionState state() const noexcept;

        /**
         * @brief 判断连接是否已经失败
         * @return true 表示发生过连接错误（不可恢复，须按 errorCode() 终止连接）
         */
        [[nodiscard]] bool hasFailed() const noexcept;

        /**
         * @brief 取本次失败的错误码
         * @details 与写进 GOAWAY 的错误码是同一个值（流级错误不进这里：那类错误只 RST_STREAM 单条流）
         * @return Http2ErrorCode 错误码；未失败时为 NoError
         */
        [[nodiscard]] Http2ErrorCode errorCode() const noexcept;

        /**
         * @brief 取本次失败的中文原因
         * @return 面向使用者的错误文本；未失败时为空串
         */
        [[nodiscard]] std::string errorMessage() const;

        /**
         * @brief 取最近一次流级错误的中文原因
         * @details 流级错误（RST_STREAM）不回给调用方任何返回值，原因只在这里可见，供日志与排查使用
         * @return 中文原因；还没发生过流级错误时为空串
         */
        [[nodiscard]] std::string lastStreamErrorMessage() const;

        /**
         * @brief 查一条流的状态
         * @param streamId 流号
         * @param streamState 输出参数：该流的状态，仅在返回 true 时有效
         * @return true 账本里还有这条流（含刚终止、仍在「忽略窗口」内的流）
         * @return false 本端从没见过这个流号，或它已终止很久（记录已被上限挤掉）
         */
        [[nodiscard]] bool tryGetStreamState(std::uint32_t streamId, Http2StreamState &streamState) const noexcept;

        /**
         * @brief 取当前活动流数
         * @return std::size_t Open 与两个半关状态的流数（按 §5.1.2 的算式，正是并发上限的判据）
         */
        [[nodiscard]] std::size_t openStreamCount() const noexcept;

        /**
         * @brief 查对端在 SETTINGS 里通告过的参数
         * @param identifier 目标参数标识
         * @param value 输出参数：参数取值，仅在返回 true 时有效
         * @return true 对端通告过该参数
         * @return false 对端没带该参数（按规范的初始值处理），不是错误
         */
        [[nodiscard]] bool tryGetPeerSetting(Http2SettingIdentifier identifier, std::uint32_t &value) const noexcept;

        /**
         * @brief 取对端发来的 GOAWAY
         * @param payload 输出参数：GOAWAY 负载，仅在返回 true 时有效
         * @return true 对端发过 GOAWAY（连接已进入 Closing：不再受理新流）
         * @return false 还没收到过 GOAWAY
         */
        [[nodiscard]] bool tryGetPeerGoAway(Http2GoAwayPayload &payload) const;

    private:
        /**
         * @brief 账本里的一条流：含已终止的流，用于区分「忽略」与「判错」
         */
        struct StreamRecord
        {
            std::uint32_t streamId{0};                                        ///< 流号
            Http2StreamState state{Http2StreamState::Idle};                   ///< 当前状态
            bool wasTerminatedByReset{false};                                 ///< 终止方式：true 表示 RST_STREAM（任一端），false 表示双向 END_STREAM
            std::int64_t sendWindowByteCount{kHttp2InitialWindowSizeByteCount}; ///< 本端可发送的流级窗口，可为负（§6.9.2 要求允许并等 WINDOW_UPDATE 救回来）
            std::string pendingData;                                          ///< 窗口不足时排队的正文
            bool isEndStreamPending{false};                                   ///< 队列排空后是否还要补一个 END_STREAM
            std::int64_t receiveWindowByteCount{kHttp2InitialWindowSizeByteCount}; ///< 本端已通告的流级接收窗口：对端还能发的字节数，扣成负数即 FLOW_CONTROL_ERROR
            std::size_t pendingReceiveCreditByteCount{0};                     ///< 已消费、还没用 WINDOW_UPDATE 还回去的字节数
        };

        /**
         * @brief 头块拼接的用途：解完这批字段后该怎么用
         *
         * @details 无论哪种用途都要把字节交给 HpackDecoder：头块里的「带增量索引」表示会改动动态表，
         *          跳过一次解码就与对端编码器的表永久错位。
         */
        enum class HeaderBlockPurpose
        {
            Request,  ///< 新请求：过了校验就交出请求对象
            Trailers, ///< 尾部头块：只校验语法（§8.1.2.1 禁止伪头），字段有意丢弃
            Discard   ///< 已拒绝或已终止的流：解完即丢，只为让动态表与对端同步
        };

        /**
         * @brief 发初始 SETTINGS：把配置里的六项参数按 §6.5.2 的参数标识顺序写出
         */
        void sendInitialSettings();

        /**
         * @brief 处理一帧
         * @param frame 帧层交出的帧，负载按移动收下
         * @return true 连接可以继续；false 连接已进入失败态，调用方应停止消费字节
         */
        [[nodiscard]] bool handleFrame(Http2Frame frame);

        /**
         * @brief 处理 DATA 帧：落到活动流上就是正文，其余按流状态判定表处置
         */
        [[nodiscard]] bool handleDataFrame(const Http2Frame &frame);

        /**
         * @brief 处理 HEADERS 帧：新流、尾部头块、或按流状态判定表处置
         */
        [[nodiscard]] bool handleHeadersFrame(const Http2Frame &frame);

        /**
         * @brief 处理 PRIORITY 帧：本片不建优先级树，只校验流号奇偶后忽略
         */
        [[nodiscard]] bool handlePriorityFrame(const Http2Frame &frame);

        /**
         * @brief 处理 RST_STREAM 帧：关掉该流并丢弃其待发数据
         */
        [[nodiscard]] bool handleRstStreamFrame(const Http2Frame &frame);

        /**
         * @brief 处理 SETTINGS 帧：ACK 只允许匹配一次，非 ACK 则记账并回 ACK
         */
        [[nodiscard]] bool handleSettingsFrame(const Http2Frame &frame);

        /**
         * @brief 处理 PING 帧：非 ACK 必须按 §6.7 原样回声，未请求过的 ACK 忽略
         */
        [[nodiscard]] bool handlePingFrame(const Http2Frame &frame);

        /**
         * @brief 处理 GOAWAY 帧：记下对端通告并转入 Closing
         */
        [[nodiscard]] bool handleGoAwayFrame(const Http2Frame &frame);

        /**
         * @brief 处理 WINDOW_UPDATE 帧：连接级与流级窗口各自记账，溢出即 FLOW_CONTROL_ERROR
         */
        [[nodiscard]] bool handleWindowUpdateFrame(const Http2Frame &frame);

        /**
         * @brief 处理 CONTINUATION 帧：续进当前头块，END_HEADERS 时收尾
         */
        [[nodiscard]] bool handleContinuationFrame(const Http2Frame &frame);

        /**
         * @brief 开始拼接一个头块（HEADERS 帧到达时调用），END_HEADERS 已置位则当场收尾
         * @param streamId 该头块所属的流
         * @param purpose 解完这批字段后的用途
         * @param endStream HEADERS 是否带了 END_STREAM
         * @param firstFragment 本帧携带的头块片段
         * @param endHeaders HEADERS 是否带了 END_HEADERS
         * @return true 连接可以继续
         */
        [[nodiscard]] bool beginHeaderBlock(std::uint32_t streamId, HeaderBlockPurpose purpose, bool endStream,
                                            std::string_view firstFragment, bool endHeaders);

        /**
         * @brief 把一段头块片段追加进拼接缓冲，并判本端字节上限
         */
        [[nodiscard]] bool appendHeaderBlockFragment(std::string_view fragment);

        /**
         * @brief 头块拼完：交 HpackDecoder 解码，再按用途校验与交出
         */
        [[nodiscard]] bool finishHeaderBlock();

        /**
         * @brief 校验并落定一个请求：伪头齐全、顺序、重复、未知伪头、连接特定头、头名全小写
         * @param headerFields 解码后的头列表（按到达顺序）
         * @param request 输出参数：通过校验的请求对象（进入调用时先清空）
         * @param errorText 输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 头列表是一个合法的请求
         */
        [[nodiscard]] static bool acceptRequestHeaderFields(const std::vector<HpackHeaderField> &headerFields, Http2Request &request,
                                                            std::string *errorText);

        /**
         * @brief 校验尾部头块的语法：禁止伪头与连接特定头、头名必须全小写
         * @param headerFields 解码后的头列表（按到达顺序）
         * @param errorText 输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 头列表是一个合法的尾部头块
         */
        [[nodiscard]] static bool acceptTrailerHeaderFields(const std::vector<HpackHeaderField> &headerFields, std::string *errorText);

        /**
         * @brief 按 §8.1.2 校验一条待发响应头的头名（token、全小写、非连接特定头）与头值字节
         * @param name 头名（调用方给的名字，不做大小写归一化）
         * @param value 头值
         * @param errorText 输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 可以发送
         */
        [[nodiscard]] static bool acceptResponseHeaderField(std::string_view name, std::string_view value, std::string *errorText);

        /**
         * @brief 把对端的 SETTINGS 参数逐个记账并落到本端行为上
         * @param payload 已解析的 SETTINGS 帧（非 ACK）
         * @return true 参数全部合法；false 连接已失败（取值非法或窗口溢出）
         */
        [[nodiscard]] bool applyPeerSettings(const Http2SettingsPayload &payload);

        /**
         * @brief 开一条新流：状态 Open，发送窗口取对端通告的初值
         */
        void openStream(std::uint32_t streamId);

        /**
         * @brief 记下对端的 END_STREAM：Open 转半关（remote），半关（local）则整条终止
         */
        void noteRemoteEndStream(StreamRecord &stream);

        /**
         * @brief 记下本端的 END_STREAM：Open 转半关（local），半关（remote）则整条终止
         */
        void noteLocalEndStream(StreamRecord &stream);

        /**
         * @brief 把流置为 Closed 并记进终止窗口（同一流号之后的帧据此判「忽略」还是「判错」）
         * @param stream 待终止的流
         * @param wasTerminatedByReset true 表示由 RST_STREAM 终止（任一端），false 表示双向 END_STREAM
         */
        void terminateStream(StreamRecord &stream, bool wasTerminatedByReset);

        /**
         * @brief 把一条已终止的流号记进终止窗口，超出上限时挤掉最旧的记录
         * @param streamId 已终止的流号
         */
        void rememberTerminatedStream(std::uint32_t streamId);

        /**
         * @brief 拒绝一条新流：回 RST_STREAM REFUSED_STREAM，并把它记成「已用过且已终止」
         * @param streamId 被拒绝的流号
         * @param reason 中文原因，写进 lastStreamErrorMessage() 供上层排查
         */
        void refuseNewStream(std::uint32_t streamId, std::string reason);

        /**
         * @brief 判一条已终止流上的帧该忽略还是该判错
         * @param stream 已终止的流记录
         * @param frameType 落在它上面的帧类型
         * @return true 忽略本帧；false 按连接错误 STREAM_CLOSED 收场
         */
        [[nodiscard]] static bool isIgnorableFrameOnTerminatedStream(const StreamRecord &stream, Http2FrameType frameType);

        /**
         * @brief 校验流号奇偶：本端不推送，偶数流号只可能属于服务端的对端，收到即意外流号
         * @param streamId 待校验的流号（非 0 由帧层保证）
         * @param errorText 输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 是合法的对端流号（奇数）
         */
        [[nodiscard]] static bool acceptPeerStreamIdParity(std::uint32_t streamId, std::string *errorText);

        /**
         * @brief 递增连接级发送窗口，溢出即 FLOW_CONTROL_ERROR（§6.9.1）
         * @param increment 对端通告的增量（非 0 由帧层保证）
         * @return true 已递增；false 连接已失败
         */
        [[nodiscard]] bool increaseConnectionSendWindow(std::uint32_t increment);

        /**
         * @brief 递增流级发送窗口，溢出即 FLOW_CONTROL_ERROR（§6.9.1）
         * @param stream 目标流
         * @param increment 对端通告的增量（非 0 由帧层保证）
         * @return true 已递增；false 连接已失败
         */
        [[nodiscard]] bool increaseStreamSendWindow(StreamRecord &stream, std::uint32_t increment);

        /**
         * @brief 把消费掉的字节还给连接级接收窗口，够阈值就发一个 WINDOW_UPDATE
         * @param byteCount 本次消费的字节数
         */
        void creditConnectionReceiveWindow(std::size_t byteCount);

        /**
         * @brief 把消费掉的字节还给某条流的接收窗口，够阈值就发一个 WINDOW_UPDATE
         * @param stream 目标流；传空指针或已终止的流时只记账，不发流级帧
         * @param byteCount 本次消费的字节数
         */
        void creditStreamReceiveWindow(StreamRecord *stream, std::size_t byteCount);

        /**
         * @brief 判累计未还的字节够不够发一次 WINDOW_UPDATE
         * @param pendingByteCount 该窗口上累计未还的字节数
         * @param advertisedWindowByteCount 该窗口对本端通告的初始值
         * @return true 达到阈值，应当发一次 WINDOW_UPDATE
         */
        [[nodiscard]] static bool isReceiveCreditWorthFlushing(std::size_t pendingByteCount, std::uint32_t advertisedWindowByteCount) noexcept;

        /**
         * @brief 尽量把一条流的待发正文按窗口与分片上限发出去
         * @param stream 目标流；队列排空且有待发的 END_STREAM 时补一个零长 DATA 帧收尾
         */
        void pumpSendQueue(StreamRecord &stream);

        /**
         * @brief 对账本里所有活动流各跑一次 pumpSendQueue()（窗口或分片上限变大后调用）
         */
        void pumpAllSendQueues();

        /**
         * @brief 发一个头块：按对端 MAX_FRAME_SIZE 切成 HEADERS + 若干 CONTINUATION
         * @param streamId 目标流
         * @param headerBlock 已编码的头块
         * @param endStream END_STREAM 是否落在 HEADERS 上
         */
        void emitHeaderBlock(std::uint32_t streamId, const std::string &headerBlock, bool endStream);

        /**
         * @brief 把一帧追加进待发字节
         * @param frameBytes 完整帧字节，按移动收下
         */
        void appendOutgoing(std::string frameBytes);

        /**
         * @brief 取对端通告的 MAX_FRAME_SIZE
         * @return std::uint32_t 对端通告值；对端还没通告时取规范的初始值 16384（§6.5.2）
         */
        [[nodiscard]] std::uint32_t peerMaximumFrameSize() const noexcept;

        /**
         * @brief 取对端通告的 SETTINGS_INITIAL_WINDOW_SIZE
         * @return std::uint32_t 对端通告值；对端还没通告时取规范的初始值 65535
         */
        [[nodiscard]] std::uint32_t peerInitialWindowSize() const noexcept;

        /**
         * @brief 在账本里按流号找流
         * @param streamId 流号
         * @return 流记录的指针；不在账本里时返回 nullptr
         */
        [[nodiscard]] StreamRecord *findStream(std::uint32_t streamId) noexcept;

        /**
         * @brief 按流号找一条仍在收发（非 Closed）的流
         * @param streamId 流号
         * @return 流记录的指针；不在账本里或已终止时返回 nullptr
         */
        [[nodiscard]] StreamRecord *findActiveStream(std::uint32_t streamId) noexcept;

        /**
         * @brief 统一记一次连接级失败：置粘滞失败态并把原因写进 GOAWAY
         * @param errorCode 要回给对端的错误码
         * @param reason 中文原因（同时是 GOAWAY 的调试数据）
         */
        void fail(Http2ErrorCode errorCode, std::string reason);

        /**
         * @brief 统一记一次流级失败：回 RST_STREAM 并终止该流，连接继续
         * @param stream 目标流
         * @param errorCode 要回给对端的错误码
         * @param reason 中文原因，记进 lastStreamErrorMessage()
         */
        void failStream(StreamRecord &stream, Http2ErrorCode errorCode, std::string reason);

        Http2ConnectionConfiguration m_configuration{};              ///< 构造时按值落定的配置，没有运行期更换的入口
        Http2ConnectionState m_state{Http2ConnectionState::AwaitingPreface}; ///< 连接状态
        std::size_t m_prefaceByteCount{0};                            ///< 已收到的前奏字节数
        std::size_t m_outstandingSettingsCount{0};                    ///< 本端已发出、还没被 ACK 的 SETTINGS 数（ACK 只允许匹配一次）
        std::chrono::steady_clock::time_point m_lastSettingsSentTime{}; ///< 最近一次发出 SETTINGS 的时刻，供上层算 SETTINGS_TIMEOUT
        Http2ErrorCode m_errorCode{Http2ErrorCode::NoError};          ///< 连接级失败的错误码（也是 GOAWAY 里带上的码）
        std::string m_errorMessage;                                   ///< 连接级失败的中文原因
        std::string m_lastStreamErrorMessage;                         ///< 最近一次流级错误的中文原因

        Http2FrameDecoder m_frameDecoder;                             ///< 帧解码器：单帧合法性由它把关
        HpackDecoder m_hpackDecoder;                                  ///< 请求方向的头块解码器，动态表与对端编码器同步演进
        HpackEncoder m_encoder;                                       ///< 响应方向的头块编码器，动态表与对端解码器同步演进

        bool m_isAssemblingHeaderBlock{false};                        ///< 是否正在拼一个头块（期间只收同流 CONTINUATION）
        std::uint32_t m_pendingHeaderStreamId{0};                     ///< 正在拼的头块所属的流号
        HeaderBlockPurpose m_pendingHeaderPurpose{HeaderBlockPurpose::Request}; ///< 正在拼的头块的用途
        bool m_pendingHeaderEndStream{false};                         ///< 正在拼的头块是否带 END_STREAM
        std::string m_pendingHeaderBlock;                             ///< 正在拼的头块字节

        std::map<std::uint32_t, StreamRecord> m_streams;              ///< 流账本：含刚终止的流
        std::deque<std::uint32_t> m_terminatedStreamIds;              ///< 终止顺序，用于给账本里已终止的记录设上限
        std::size_t m_openStreamCount{0};                             ///< Open 与两个半关状态的流数（并发上限的判据）
        std::uint32_t m_highestPeerStreamId{0};                       ///< 对端已用过的最大流号：判「严格递增」与 GOAWAY 的 last-stream-id
        std::int64_t m_connectionSendWindowByteCount{kHttp2InitialWindowSizeByteCount}; ///< 连接级发送窗口（只受 WINDOW_UPDATE 影响，§6.9.2）
        std::int64_t m_connectionReceiveWindowByteCount{kHttp2InitialWindowSizeByteCount}; ///< 本端已通告的连接级接收窗口：对端还能发的字节数，扣成负数即 FLOW_CONTROL_ERROR
        std::size_t m_pendingConnectionReceiveCreditByteCount{0};     ///< 已消费、还没用 WINDOW_UPDATE 还回去的连接级字节数
        std::map<std::uint16_t, std::uint32_t> m_peerSettings;        ///< 对端 SETTINGS 记账（标识 → 取值，重复出现以来值为准）
        bool m_hasPeerGoAway{false};                                  ///< 是否已收到对端 GOAWAY
        Http2GoAwayPayload m_peerGoAway{};                            ///< 对端 GOAWAY 的负载

        std::string m_outgoingBytes;                                  ///< 待发字节：由 takeOutgoingBytes() 一次取走
        std::vector<Http2Request> m_pendingRequests;                  ///< 已校验通过、等着被取走的请求
        std::vector<Http2ReceivedData> m_pendingReceivedData;         ///< 已收到、等着被取走的正文片段
    };
} // namespace AsynGyanis::Net
