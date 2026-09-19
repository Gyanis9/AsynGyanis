/**
 * @file Http3Frame.h
 * @brief HTTP/3 帧（RFC 9114 §7.2）与单向流类型前缀（§6.2）的字节编解码
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本层只按 §7.1/§7.2 的字段布局编与解，不做协议策略判断：帧该出现在哪条流、SETTINGS 是否
 *          重复、推送 ID 是否越界、保留帧类型是否违规，一律由连接层按 §8.1 选错误码。未知帧类型与
 *          未知设置项按 §9「必须忽略」原样交出，不当成解码失败。
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace AsynGyanis::Net
{
    /// §7.2.8（帧类型）、§6.2.3（流类型）、§7.2.4.1（设置项）三处保留区共用同一个式子 0x1f * N + 0x21 里的步长
    inline constexpr std::uint64_t kHttp3ReservedExtensionIdentifierStride = 0x1f;

    /// 上式的起始值 N=0 时的取值，也是保留区的最小值
    inline constexpr std::uint64_t kHttp3ReservedExtensionIdentifierBase = 0x21;

    /**
     * @brief 判断某个标识取值是否落在「留给未知类型」的保留区 0x1f * N + 0x21
     * @details 三处保留区（§7.2.8 帧类型、§6.2.3 流类型、§7.2.4.1 设置项）用的是同一个式子，因此共用本判断。
     *          规范要它们存在的目的就是验证「未知类型必须被忽略」这条，收到时**不得**赋予任何含义。
     * @note 别与 §7.2.8 另一类保留值混淆：0x02、0x06、0x08、0x09 是从 HTTP/2 继承下来的保留帧类型，
     *       它们的处置是「MUST NOT 发送，收到按 H3_FRAME_UNEXPECTED」，不落在本式子里。
     * @param value 帧类型 / 流类型 / 设置项标识的线上取值
     * @return true 表示取值形如 0x1f * N + 0x21（0x21、0x40、0x5f ...）
     */
    [[nodiscard]] inline constexpr bool isHttp3ReservedExtensionIdentifier(const std::uint64_t value) noexcept
    {
        // 式子等价于「减掉起始值后是步长的整数倍」，先挡掉小于起始值的取值避免无符号回绕
        return value >= kHttp3ReservedExtensionIdentifierBase &&
               (value - kHttp3ReservedExtensionIdentifierBase) % kHttp3ReservedExtensionIdentifierStride == 0;
    }

    /**
     * @brief 帧类型取值（RFC 9114 表 2 与各小节）
     *
     * @details 只列本层**认识布局**的类型。0x02/0x06/0x08/0x09（HTTP/2 里用过、HTTP/3 无对应帧）与
     *          0x1f * N + 0x21 一族都在 §7.2.8 里被保留，取值仍然原样落进 Http3UnknownFrame 交上层处置；
     *          新增成员一律追加在末尾。
     */
    enum class Http3FrameType : std::uint64_t
    {
        Data        = 0x00, ///< DATA：请求或响应的内容字节（§7.2.1）
        Headers     = 0x01, ///< HEADERS：QPACK 编码后的头字段段（§7.2.2）
        CancelPush  = 0x03, ///< CANCEL_PUSH：撤销一个尚未送达的推送承诺（§7.2.3）
        Settings    = 0x04, ///< SETTINGS：连接级参数（§7.2.4）
        PushPromise = 0x05, ///< PUSH_PROMISE：服务端承诺一个推送（§7.2.5）
        GoAway      = 0x07, ///< GOAWAY：优雅收场，声明不再接受的标识（§7.2.6）
        MaxPushId   = 0x0d, ///< MAX_PUSH_ID：客户端给可允许的最大推送 ID（§7.2.7）
    };

    /**
     * @brief 单向流开头的流类型取值（§6.2 图 1）
     *
     * @details 流类型是单向流第一个字段，本身就是一个变长整数（§6.2）。本端只认这四值；其余取值按
     *          §6.2 与 §9 属「未知流类型」——接收侧**必须**要么中止读该流、要么丢弃后续数据且不再处理，
     *          中止读时 SHOULD 用 H3_STREAM_CREATION_ERROR 或保留错误码（§8.1），且**不得**把未知流类型
     *          当成连接级错误。§6.2.3 另留了 0x1f * N + 0x21 一族作保留流类型，发送侧可干净收尾也可重置它。
     */
    enum class Http3StreamType : std::uint64_t
    {
        Control       = 0x00, ///< 控制流：承载 SETTINGS 等控制帧，每端恰好一条（§6.2.1）
        Push          = 0x01, ///< 推送流：类型后紧跟一个 Push ID 变长整数，再跟帧（§6.2.2 图 2）
        QpackEncoder  = 0x02, ///< QPACK 编码器流：无帧结构，直接是指令序列（RFC 9204 §4.2）
        QpackDecoder  = 0x03, ///< QPACK 解码器流：无帧结构，直接是指令序列（RFC 9204 §4.2）
    };

    /**
     * @brief 本层认识的设置项标识（§7.2.4.1 与 RFC 9204 §5）
     *
     * @details 其余取值一律按 §7.2.4「必须忽略不认识的参数」处理成未知设置项：例如扩展定义过的
     *          SETTINGS_H3_DATAGRAM（0x33）与 0x1f * N + 0x21 一族的保留设置项，本层都不赋予含义，
     *          原样落在 Http3SettingsFrame 的 unknownSettings 里。注意 0x02～0x05 是「HTTP/2 里定义过、
     *          HTTP/3 无对应设置」的保留标识，§7.2.4.1 要求收到即判 H3_SETTINGS_ERROR——那是连接层的
     *          判定，本层同样只是把它们塞进未知项列表。
     */
    enum class Http3SettingId : std::uint64_t
    {
        QpackMaxTableCapacity = 0x01, ///< SETTINGS_QPACK_MAX_TABLE_CAPACITY，默认 0（RFC 9204 §5）
        MaxFieldSectionSize   = 0x06, ///< SETTINGS_MAX_FIELD_SECTION_SIZE，默认不限（§7.2.4.1）
        QpackBlockedStreams   = 0x07, ///< SETTINGS_QPACK_BLOCKED_STREAMS，默认 0（RFC 9204 §5；草案时期曾占 0x02，终稿是 0x07）
        EnableConnectProtocol = 0x08, ///< SETTINGS_ENABLE_CONNECT_PROTOCOL，扩展 CONNECT 的开关（RFC 9220 §3.2.1）
    };

    /**
     * @brief 设置项标识的线上取值到枚举的映射
     * @details 连接层把手上的整数并进本端设置表时用的就是这一档，与解码侧保持同一张认表。
     * @param identifierValue 线上取值
     * @return std::optional<Http3SettingId> 认识的取值返回对应枚举；未知或保留取值返回 nullopt，
     *         调用方按「忽略该项」处理，不当成错误
     */
    [[nodiscard]] std::optional<Http3SettingId> http3SettingIdFromValue(const std::uint64_t identifierValue) noexcept;

    /**
     * @brief 帧解码失败的类别
     *
     * @details 分类依据是「怎么回才对端有用」，与 QuicDecodeErrorKind/Http2FrameErrorKind 同一套判据：
     *          文案会改，类别才是契约。新增类别一律追加在末尾。
     */
    enum class Http3FrameErrorKind
    {
        Malformed,    ///< 载荷与 §7.2 的字段布局不合（字段越出声明长度、声明长度盖不住必填字段、SETTINGS 参数残缺）：连接层按 §7.1 回 H3_FRAME_ERROR
        LimitExceeded, ///< 声明长度或已缓冲字节数突破本端上限：本端资源策略而非对端违规，连接层可选 H3_EXCESSIVE_LOAD 或直接关流
    };

    /**
     * @brief 一次帧编解码失败的完整说明
     */
    struct Http3FrameError
    {
        Http3FrameErrorKind kind{Http3FrameErrorKind::Malformed}; ///< 失败类别：上层据此选上线错误码，不去匹配文案
        std::string message;                                     ///< 中文可操作文案，含帧类型、声明长度与实收字节数和对应 RFC 章节
    };

    // ============================================================================
    // 各帧的字段结构：只放该帧自己的字段，载荷一律是指向入参缓冲的视图
    // ============================================================================

    /**
     * @brief 逐字节比较两段载荷视图
     * @details `std::span` 标准里就没给 `==`，因此带视图成员的帧结构体一律用它比：直接 `= default`
     *          会让 `operator==` 被静默删掉，直到有人比整条 `std::variant` 时才暴露成「没有可用的 ==」。
     * @param left 左段载荷，可为空
     * @param right 右段载荷，可为空
     * @return true 表示长度相等且逐字节相同；两个空视图相等
     */
    [[nodiscard]] inline constexpr bool http3PayloadBytesEqual(const std::span<const std::uint8_t> left,
                                                              const std::span<const std::uint8_t> right) noexcept
    {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }

    /// DATA 帧（§7.2.1 图 4）：Type + Length + Data
    struct Http3DataFrame
    {
        std::span<const std::uint8_t> payload{}; ///< Data 字段，长度任意（含 0 长度）；不做内容解释

        [[nodiscard]] bool operator==(const Http3DataFrame &other) const noexcept
        {
            return http3PayloadBytesEqual(payload, other.payload);
        }
    };

    /// HEADERS 帧（§7.2.2 图 5）：Type + Length + Encoded Field Section
    struct Http3HeadersFrame
    {
        std::span<const std::uint8_t> encodedFieldSection{}; ///< QPACK 编码后的头字段段，本层不解释语义

        [[nodiscard]] bool operator==(const Http3HeadersFrame &other) const noexcept
        {
            return http3PayloadBytesEqual(encodedFieldSection, other.encodedFieldSection);
        }
    };

    /// 一条本层不认识的设置项（§7.2.4「必须忽略不认识的参数」），原样留着以便再编出去
    struct Http3UnknownSetting
    {
        std::uint64_t identifier{}; ///< 线上标识取值，可能是扩展定义的，也可能是 §7.2.4.1 的保留标识
        std::uint64_t value{};      ///< 线上值，含义对本层不透明

        [[nodiscard]] bool operator==(const Http3UnknownSetting &) const = default;
    };

    /**
     * @brief SETTINGS 帧（§7.2.4 图 7）：Type + Length + Setting(...) ...
     *
     * @details 载荷是零或多条「标识 + 值」的变长整数对。本层按 §7.2.4 只做布局拆解，不判语义：
     *          同一个标识出现两次照样两条都收（规范写着「MUST NOT 重复」「接收方**可以**判 H3_SETTINGS_ERROR」，
     *          判不判是连接层的事），也不拒绝空载荷（零个参数是合法的）。
     * @note 编码侧先写 settings 再写 unknownSettings：线上参数顺序无语义，逐字节回显对端原文不是本层职责。
     */
    struct Http3SettingsFrame
    {
        std::vector<std::pair<Http3SettingId, std::uint64_t>> settings{}; ///< 本层认识 settings 表里的项，按线上出现顺序
        std::vector<Http3UnknownSetting> unknownSettings{};               ///< 其余项，一条都不丢，可原样再编出去

        [[nodiscard]] bool operator==(const Http3SettingsFrame &) const = default;
    };

    /// CANCEL_PUSH 帧（§7.2.3 图 6）：载荷恰好一个 Push ID 变长整数
    struct Http3CancelPushFrame
    {
        std::uint64_t pushId{}; ///< 被撤销的推送标识；是否「超出当前允许范围」由连接层判（§7.2.3 的 H3_ID_ERROR）

        [[nodiscard]] bool operator==(const Http3CancelPushFrame &) const = default;
    };

    /// PUSH_PROMISE 帧（§7.2.5 图 8）：Type + Length + Push ID + Encoded Field Section
    struct Http3PushPromiseFrame
    {
        std::uint64_t pushId{};                                        ///< 承诺的推送标识
        std::span<const std::uint8_t> encodedFieldSection{};           ///< 被承诺请求的 QPACK 头字段段

        [[nodiscard]] bool operator==(const Http3PushPromiseFrame &other) const noexcept
        {
            return pushId == other.pushId && http3PayloadBytesEqual(encodedFieldSection, other.encodedFieldSection);
        }
    };

    /**
     * @brief GOAWAY 帧（§7.2.6 图 9）：载荷恰好一个变长整数
     *
     * @details 终稿的字段名就是「Stream ID/Push ID」，同一个位置按方向承载两种量：服务端发的是
     *          客户端发起的双向流号，客户端发的是 Push ID（§7.2.6、§5.2）。语义是**该标识及其以上**的
     *          请求或推送都被发送方拒绝，所以它既是「最早不再接受的标识」也是「可能已被处理的上界」。
     * @note 取值是否属于合法流号（服务端方向必须是客户端发起的双向流）与「后发的 GOAWAY 标识不得变大」
     *       都由连接层判，违规按 §7.2.6/§5.2 回 H3_ID_ERROR。
     */
    struct Http3GoAwayFrame
    {
        std::uint64_t streamIdOrPushId{}; ///< 线上 Stream ID/Push ID 字段原值

        [[nodiscard]] bool operator==(const Http3GoAwayFrame &) const = default;
    };

    /// MAX_PUSH_ID 帧（§7.2.7 图 10）：载荷恰好一个 Push ID 变长整数
    struct Http3MaxPushIdFrame
    {
        std::uint64_t pushId{}; ///< 服务端可用推送标识的上界；只能升不能降（§7.2.7）

        [[nodiscard]] bool operator==(const Http3MaxPushIdFrame &) const = default;
    };

    /**
     * @brief 本层不认识的帧：§9 要求「未知或不受支持的取值必须忽略」，于是载荷原样交出
     *
     * @details 承载三类取值：扩展自定义的帧类型、§7.2.8 的 0x1f * N + 0x21 保留族、以及
     *          §7.2.8 从 HTTP/2 继承下来的保留帧类型（0x02/0x06/0x08/0x09）。前两类本层**不报错**，
     *          第三类要连接层按 §7.2.8 判 H3_FRAME_UNEXPECTED——差别只在数值落在哪个区段，
     *          用 isHttp3ReservedExtensionIdentifier() 即可区分，故不额外建类型。
     */
    struct Http3UnknownFrame
    {
        std::uint64_t frameType{};              ///< 线上帧类型原值，本层不认识
        std::span<const std::uint8_t> payload{}; ///< 载荷原文，本层不解释

        [[nodiscard]] bool operator==(const Http3UnknownFrame &other) const noexcept
        {
            return frameType == other.frameType && http3PayloadBytesEqual(payload, other.payload);
        }
    };

    /**
     * @brief 一帧的联合类型
     *
     * @details 与 QuicFrame 同档：每种帧字段互不相干，摊平会造出一堆对该类型无意义的空位。
     * @warning 载荷类字段（DATA/HEADERS/PUSH_PROMISE/未知帧）是**指向解码时那段缓冲的视图**，
     *          不是拷贝。一次性解码入口要求缓冲活到帧用完；增量解码器见 Http3FrameReader 的说明。
     */
    using Http3Frame = std::variant<Http3DataFrame, Http3HeadersFrame, Http3CancelPushFrame, Http3SettingsFrame,
                                    Http3PushPromiseFrame, Http3GoAwayFrame, Http3MaxPushIdFrame, Http3UnknownFrame>;

    /**
     * @brief 取帧的线上类型值
     * @param frame 帧
     * @return std::uint64_t §7.2 各小节的类型值；Http3UnknownFrame 返回它自带的原值，可直接写进错误帧或日志
     */
    [[nodiscard]] std::uint64_t http3FrameTypeValue(const Http3Frame &frame) noexcept;

    /**
     * @brief 取帧类型的规范标识名，用于错误文案与日志
     * @param frameTypeValue 帧类型的线上取值
     * @return std::string_view 大写下划线的规范名（如 `HEADERS`）；不认识或保留的取值返回「未知帧类型」，
     *         调用方自己把数值拼进去——对端可以发任何 62 位内的整数，照实记下比含糊带过有用
     */
    [[nodiscard]] std::string_view http3FrameTypeName(const std::uint64_t frameTypeValue) noexcept;

    /**
     * @brief 把一帧按 §7.1 的布局追写到缓冲末尾
     * @details 写出顺序是 Type、Length、载荷；Length 恒按**实际写出的载荷字节数**填，不接受调用方给的长度，
     *          因此 §7.1 要求的「长度自洽」在编码侧不可能被写坏。变长整数一律用最短档位（§7.2.4.1 保留区
     *          之外也没有更宽的要求）。
     * @param bytes 目标缓冲，二进制安全
     * @param frame 帧
     * @throws Base::InvalidArgumentException 用法错误：某个字段值超过 kQuicMaximumIntegerValue（2^62-1），
     *         在协议里没有合法编码
     */
    void appendHttp3Frame(std::string &bytes, const Http3Frame &frame);

    /**
     * @brief 追写单向流开头的流类型前缀（§6.2 图 1）
     * @details 只写类型这一个变长整数。推送流还要紧跟一个 Push ID（§6.2.2 图 2），本层不代拼：
     *          调用方直接再用 appendQuicVariableLengthInteger 追写即可。未知流类型不在此列——
     *          本端不会发不认识的类型，要发保留流类型（§6.2.3）时把它的数值当流类型写进去就行。
     * @param bytes 目标缓冲，二进制安全
     * @param streamType 流类型取值
     */
    void appendHttp3StreamTypeHeader(std::string &bytes, const Http3StreamType streamType);

    /**
     * @brief HTTP/3 帧的增量解码器：喂进来的字节边界完全任意
     *
     * @details 内部缓冲按帧为单位推进：帧头（Type + Length 两个变长整数）收齐才能知道整帧多长，
     *          载荷收齐才解一帧并交出。因此一帧可以横跨任意多次 feed()，一次 feed() 也可以横跨任意
     *          多帧（含只到一半的尾帧）——剩余字节留在缓冲里等下一次。跨帧语义（该不该出现在这条流上、
     *          SETTINGS 是否重复、流类型是什么）一概不做，留给连接层。
     * @note 缓冲的增长被两处闸门钉死：帧的声明长度不得超过上限（否则一收到帧头就判超限，不会干等），
     *        单次 feed() 的块也不得超过上限。于是缓冲最坏情况只是「一个半成品帧 + 一次喂入的整块」，
     *        即以两倍的量封顶，不会无限增长。
     *
     * @note 未知帧类型、未知设置项按 §9 原样交出，不判错；本层唯一的错误是「布局不合 §7.2」与「超本端上限」。
     * @note 变长整数**允许非最短编码**：RFC 9000 §16 末段那条「唯一要求最短编码」的例外管的是 QUIC 自己的
     *       Frame Type 字段（§12.4），HTTP/3 的帧类型、长度与流类型都不在其列，§7.1 只要求嵌套长度自洽
     *       （见 §10.8），所以宽档位照样解成同一个值。
     * @warning 交出的帧里载荷字段指向**本对象内部缓冲**。下一次 feed() 可能让缓冲扩容搬家、nextFrame()
     *          会把已消费的前缀挪走，两者都会让上一帧的视图失效；要把某帧留到之后用，当场把载荷拷走。
     * @warning 失败是粘滞的：一旦返回过错误，缓冲里的字节已无法解释，后续调用一律重复同一个错误，
     *          直到 reset() 为止。
     */
    class Http3FrameReader
    {
    public:
        /**
         * @brief 构造解码器
         * @param maximumFrameByteCount 单帧「帧头 + 载荷」的总长上限（字节），同时用作内部缓冲的封顶依据：
         *        声明长度超过它的帧当场判超限，单次喂入超过它的块也判超限（见类说明里的两道闸门）
         * @throws Base::InvalidArgumentException 用法错误：上限为 0，那样连一个最小的空帧都容不下
         */
        explicit Http3FrameReader(std::size_t maximumFrameByteCount);

        /**
         * @brief 析构函数：成员都是按值的标准容器，无额外资源需要回收。
         */
        ~Http3FrameReader() = default;

        // 禁拷贝：解码器持有「解到第几字节」的游标与内部缓冲，复制一份会让两半字节的归属当场分叉
        Http3FrameReader(const Http3FrameReader &) = delete;

        Http3FrameReader &operator=(const Http3FrameReader &) = delete;

        /**
         * @brief 把新到的一段字节追加到内部缓冲
         * @details 本函数只负责收下并缓冲，不解释内容；解帧是 nextFrame() 的事。
         * @param newBytes 新到的字节，按「指针 + 长度」取，可含任意二进制
         * @return 成功返回空的 expected
         * @return 失败返回 `Http3FrameError`：类别是 `LimitExceeded`（本次块长超过单帧上限），
         *         或解码器已进入粘滞错误态而重复报出旧错
         * @warning 之前交出的帧的载荷视图指向本对象的缓冲，本次追加可能让它们失效
         */
        [[nodiscard]] std::expected<void, Http3FrameError> feed(std::span<const std::uint8_t> newBytes);

        /**
         * @brief 取出缓冲里下一个完整帧
         * @details 帧头没收齐、或载荷还差一字节，都算「还没到」而不是「读坏了」：此时不消费任何字节，
         *          等下一次 feed() 补上。解出帧后已消费的前缀会在**下一次**调用时才被挪走，
         *          所以本帧的视图在下次调用前一直有效。
         * @return 成功返回一帧；缓冲里没有完整帧时返回空 optional（不是错误）
         * @return 失败返回 `Http3FrameError`：载荷字段越出声明长度或声明长度盖不住必填字段为 `Malformed`，
         *         声明长度本身超上限为 `LimitExceeded`；此后解码器进入粘滞错误态，需 reset() 才能继续
         */
        [[nodiscard]] std::expected<std::optional<Http3Frame>, Http3FrameError> nextFrame();

        /**
         * @brief 丢弃缓冲里的一切，回到初态
         * @details 换连接、或连接层决定「这条流到此为止」时用；错误态也只有在这里才被清掉。
         */
        void reset() noexcept;

        /**
         * @brief 取尚未被 nextFrame() 消费的残余字节数
         * @details 这是「流干净收尾时最后一帧是否被截断」的唯一观测点：§7.1 要求收流时残余必须为 0，
         *          否则就是「最后一个帧是半截的」，连接层据此回 H3_FRAME_ERROR。
         * @return std::size_t 残余字节数；0 表示缓冲里没有半成品
         */
        [[nodiscard]] std::size_t pendingByteCount() const noexcept;

    private:
        /**
         * @brief 把一条失败记成粘滞错误并返回它
         * @param kind 失败类别
         * @param message 中文可操作文案
         * @return Http3FrameError 原样返回，方便调用方直接 std::unexpected(...)
         */
        [[nodiscard]] Http3FrameError latchError(Http3FrameErrorKind kind, std::string message);

        /// 内部缓冲，含已解出但还没被挪走的帧
        std::vector<std::uint8_t> m_buffer{};

        /// m_buffer 里下一帧的起始位置：挪走前缀的动作推迟到下一次调用，以免当场把刚交出的视图抽掉
        std::size_t m_consumedByteOffset{0};

        /// 单帧「帧头 + 载荷」的总长上限，也是喂入块长的上限
        std::size_t m_maximumFrameByteCount{0};

        /// 粘滞错误：有值即解码器已作废，只重复报同一个错
        std::optional<Http3FrameError> m_stickyError{};
    };
} // namespace AsynGyanis::Net
