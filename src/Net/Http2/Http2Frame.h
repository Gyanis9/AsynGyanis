/**
 * @file Http2Frame.h
 * @brief HTTP/2 帧层（RFC 7540 §4 帧格式、§6 帧定义、§7 错误码）：帧头与各类型负载的编解码、增量帧解码器
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
#include <vector>

namespace AsynGyanis::Net
{
    // ============================================================================
    // HTTP/2 帧编解码（RFC 7540 §4/§6）
    //
    // 本层只提供协议机制（帧头与各类型负载的编解码、增量帧解码器）：谁调用编码器、谁把解码器接到
    // 连接读取循环上由上层负责——Http2Connection 解释帧并把要回的帧交给上层写出，Http2Session 负责
    // 真正读写 TLS 通道。帧层只判**单帧**自身的合法性（长度、标志、padding、优先级字段），不维护
    // 流状态、不做流控记账、不拼头块——那些要看跨帧状态，属于上层与 HPACK 层。
    // ============================================================================

    /**
     * @brief HTTP/2 帧类型（RFC 7540 §6）
     *
     * @note 新增取值一律追加在末尾：这些数值直接对应线上帧头的第二个字节，改动既有取值等于
     *       改变协议本身；未定义的类型不属于本枚举（解码器按 §4.1 要求忽略并原样交出）。
     */
    enum class Http2FrameType : std::uint8_t
    {
        Data         = 0x0, ///< DATA：流上的应用数据
        Headers      = 0x1, ///< HEADERS：头块片段，可带优先级字段与 padding
        Priority     = 0x2, ///< PRIORITY：流优先级声明（负载固定 5 字节）
        RstStream    = 0x3, ///< RST_STREAM：立即终止一条流（负载固定 4 字节错误码）
        Settings     = 0x4, ///< SETTINGS：连接级配置参数（负载为 6 字节参数的序列）
        PushPromise  = 0x5, ///< PUSH_PROMISE：服务端推送预告（本片不解释其负载，原样交出）
        Ping         = 0x6, ///< PING：链路探测与往返测量（负载固定 8 字节）
        GoAway       = 0x7, ///< GOAWAY：连接关闭通告
        WindowUpdate = 0x8, ///< WINDOW_UPDATE：流控窗口增量（负载固定 4 字节）
        Continuation = 0x9  ///< CONTINUATION：头块片段的续帧
    };

    /**
     * @brief HTTP/2 错误码（RFC 7540 §7）
     *
     * @note 新增取值一律追加在末尾；GOAWAY 与 RST_STREAM 都会把这些数值带上线，改动既有取值
     *       等于改变协议本身。
     */
    enum class Http2ErrorCode : std::uint32_t
    {
        NoError            = 0x0, ///< 无错误：优雅关闭或正常终止
        ProtocolError      = 0x1, ///< 协议违规：帧的顺序、形态或字段取值违反规范
        InternalError      = 0x2, ///< 本端内部错误：与对端行为无关
        FlowControlError   = 0x3, ///< 流控违规：窗口被突破或增量越界
        SettingsTimeout    = 0x4, ///< 对端未在合理时间内确认 SETTINGS
        StreamClosed       = 0x5, ///< 对端在已关闭的流上发了帧
        FrameSizeError     = 0x6, ///< 帧尺寸违规：超出通告上限或与类型要求的固定长度不符
        RefusedStream      = 0x7, ///< 本端拒绝接受这条流
        Cancel             = 0x8, ///< 发送方不再需要这条流
        CompressionError   = 0x9, ///< 头块压缩上下文出错，无法继续解码
        ConnectError       = 0xa, ///< 仅用于 CONNECT 方法建立的隧道出错
        EnhanceYourCalm    = 0xb, ///< 对端行为可能造成过量负载（本层用于本地资源上限被突破）
        InadequateSecurity = 0xc, ///< 传输层安全等级不满足要求
        Http11Required     = 0xd  ///< 要求改用 HTTP/1.1 重试本次请求
    };

    /// 帧头固定 9 字节：24 位长度 + 8 位类型 + 8 位标志 + 31 位流号（RFC 7540 §4.1）
    inline constexpr std::size_t kHttp2FrameHeaderByteCount = 9;

    /// 帧头长度域是 24 位，可表示的最大负载长度（RFC 7540 §4.1）
    inline constexpr std::uint32_t kHttp2MaximumFramePayloadByteCount = 0xFFFFFF;

    /// SETTINGS_MAX_FRAME_SIZE 的默认值，也是合法区间的下界（RFC 7540 §6.5.2）
    inline constexpr std::uint32_t kHttp2DefaultMaximumFrameSize = 16384;

    /// SETTINGS_MAX_FRAME_SIZE 的合法上界，等于 24 位长度域的满值（RFC 7540 §6.5.2）
    inline constexpr std::uint32_t kHttp2MaximumMaximumFrameSize = kHttp2MaximumFramePayloadByteCount;

    /// 流号字段是 31 位，最高位 R 必须为 0（RFC 7540 §4.1）
    inline constexpr std::uint32_t kHttp2MaximumStreamId = 0x7FFFFFFF;

    /// 流控窗口的初值（RFC 7540 §6.9.2）：连接级窗口恒以它为初值，流级初值随 SETTINGS_INITIAL_WINDOW_SIZE 变化
    inline constexpr std::uint32_t kHttp2InitialWindowSizeByteCount = 65535;

    /// 流控窗口的上限（RFC 7540 §6.9.2）：任何窗口超过 2^31-1 一律判 FLOW_CONTROL_ERROR，两条方向都适用
    inline constexpr std::uint32_t kHttp2MaximumWindowSizeByteCount = 0x7FFFFFFFU;

    /// DATA / HEADERS 的 END_STREAM 标志（RFC 7540 §6.1、§6.2）
    inline constexpr std::uint8_t kHttp2FlagEndStream = 0x1;

    /// SETTINGS / PING 的 ACK 标志（RFC 7540 §6.5、§6.7）
    inline constexpr std::uint8_t kHttp2FlagAcknowledge = 0x1;

    /// HEADERS / CONTINUATION / PUSH_PROMISE 的 END_HEADERS 标志（RFC 7540 §6.2、§6.10）
    inline constexpr std::uint8_t kHttp2FlagEndHeaders = 0x4;

    /// DATA / HEADERS / PUSH_PROMISE 的 PADDED 标志（RFC 7540 §6.1）
    inline constexpr std::uint8_t kHttp2FlagPadded = 0x8;

    /// HEADERS 的 PRIORITY 标志：负载里多出 5 字节优先级字段（RFC 7540 §6.2）
    inline constexpr std::uint8_t kHttp2FlagPriority = 0x20;

    /**
     * @brief 帧解码失败的类别
     *
     * @details 分类依据是「用哪个错误码回对端」：ProtocolError 与 FrameSizeError 直接取自 RFC 7540 §7
     *          的同名错误码，LimitExceeded 是本端资源上限被突破（不是对端违规），按规范建议回
     *          ENHANCE_YOUR_CALM，映射关系见 toHttp2ErrorCode()。
     * @note 上层据 kind 决定策略而不匹配文案——文案会改，分类是契约；新增类别一律追加在末尾。
     */
    enum class Http2FrameErrorKind
    {
        None,           ///< 尚未失败
        ProtocolError,  ///< 对端违反 RFC 7540 的帧层规则（R 位非 0、padding 越界、依赖自身流号、增量 0 等）
        FrameSizeError, ///< 帧尺寸违规（负载超本端通告上限，或与类型要求的固定长度不符）
        LimitExceeded   ///< 本端资源上限被突破（累计读取字节数），与对端违规区分开
    };

    /**
     * @brief 把帧层失败类别映射成上线的 HTTP/2 错误码
     * @param errorKind 帧层失败类别
     * @return Http2ErrorCode 对应错误码：ProtocolError→PROTOCOL_ERROR、FrameSizeError→FRAME_SIZE_ERROR、
     *         LimitExceeded→ENHANCE_YOUR_CALM（RFC 7540 §7 对「对端可能造成过量负载」的建议取值）、
     *         None→NO_ERROR
     */
    [[nodiscard]] Http2ErrorCode toHttp2ErrorCode(Http2FrameErrorKind errorKind) noexcept;

    /**
     * @brief 取帧类型的中文名，用于错误文案与日志
     * @param frameType 帧类型
     * @return std::string_view 中文名；未定义取值返回「未知类型」
     */
    [[nodiscard]] std::string_view http2FrameTypeName(Http2FrameType frameType) noexcept;

    /**
     * @brief 取错误码的中文名，用于错误文案与日志
     * @param errorCode 错误码
     * @return std::string_view 中文名；未定义取值返回「未定义错误码」
     */
    [[nodiscard]] std::string_view http2ErrorCodeName(Http2ErrorCode errorCode) noexcept;

    /**
     * @brief HTTP/2 帧头（RFC 7540 §4.1）
     */
    struct Http2FrameHeader
    {
        std::uint32_t payloadLength{0};                 ///< 24 位负载长度，不含 9 字节帧头
        Http2FrameType type{Http2FrameType::Data};      ///< 帧类型
        std::uint8_t flags{0};                          ///< 8 位标志，含义随类型变化，未定义位置位时按 §4.1 忽略
        std::uint32_t streamId{0};                      ///< 31 位流号；0 表示连接级帧
    };

    /**
     * @brief 流的优先级字段（RFC 7540 §6.3）
     */
    struct Http2Priority
    {
        bool isExclusive{false};         ///< E 位：独占标记（§5.3.1）
        std::uint32_t streamDependency{0}; ///< 31 位依赖的父流号；等于本帧流号即违反 §5.3.1
        std::uint8_t weight{0};          ///< 线上权重取值 0..255，**实际权重是它加一**（§5.3.2，区间 1..256）
    };

    /**
     * @brief 解码器交出的一帧：帧头 + 已剥掉帧级字段的净负载
     *
     * @details 净负载的含义按类型区分：DATA 是去掉 padding 后的应用数据，HEADERS 是去掉 padding 与
     *          优先级字段后的头块片段，CONTINUATION 与其余类型即线上负载原文。padding 与优先级字段
     *          由帧层就地剥掉（只有它同时拿得到标志与整段负载），上层不必再按标志重算偏移。
     * @note PUSH_PROMISE 本片不解释（服务端不会收到它，投递方向是服务端到客户端），其负载含 promised
     *       流号与 padding，整段原样交出；CONTINUATION 本身没有 padding，两种类型的固定字段都不由本层剥离。
     */
    struct Http2Frame
    {
        Http2FrameHeader header;         ///< 帧头原文（payloadLength 是线上声明的长度，含 padding）
        std::string payload;             ///< 净负载，见结构体说明
        bool hasPriority{false};         ///< 本帧是否带优先级字段（HEADERS 置 PRIORITY 位、或 PRIORITY 帧）
        Http2Priority priority{};        ///< 优先级字段，hasPriority 为 true 时有效
    };

    // ============================================================================
    // 帧头编解码
    // ============================================================================

    /**
     * @brief 编码 9 字节帧头
     * @param header 帧头；payloadLength 由调用方给出（本函数不改写它）
     * @return std::string 9 字节帧头（大端序）
     * @throws Base::InvalidArgumentException 用法错误：payloadLength 超出 24 位、streamId 超出 31 位
     *         （R 位必须为 0），或 type 不是 RFC 7540 §6 定义过的取值
     */
    [[nodiscard]] std::string encodeHttp2FrameHeader(const Http2FrameHeader &header);

    /**
     * @brief 解码 9 字节帧头（独立入口，帧解码器内部走的是同一套校验）
     * @details 只校验帧头**自身**能判的字段：字节数收满 9 个、流号的 R 位为 0。是否超出本端通告的
     *          SETTINGS_MAX_FRAME_SIZE 属于接收策略，由 Http2FrameDecoder 判。
     * @param bytes 帧头字节，至少 kHttp2FrameHeaderByteCount 字节
     * @param header 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功；未定义的类型值照样收下（RFC 7540 §4.1 要求忽略未知类型）
     * @return false bytes 不足 9 字节，或 R 位非 0
     * @warning RFC 7540 §4.1 允许接收侧忽略 R 位，本实现按要求从严判错：放行保留位会让「收到的是什么」
     *          依赖对端是否在用未定义的扩展，且这些帧在本端无法被正确解释
     */
    [[nodiscard]] bool decodeHttp2FrameHeader(std::string_view bytes, Http2FrameHeader &header, std::string *errorText = nullptr);

    /**
     * @brief 编码一帧：帧头（长度按负载实际大小重算）+ 负载
     * @param type 帧类型
     * @param flags 标志位
     * @param streamId 流号；连接级帧传 0
     * @param payload 负载字节，按「指针 + 长度」取，可含 NUL 与任意二进制
     * @return std::string 完整帧字节，可直接写入连接
     * @throws Base::InvalidArgumentException 用法错误：负载超过 24 位长度域、流号超出 31 位，或类型未定义
     */
    [[nodiscard]] std::string encodeHttp2Frame(Http2FrameType type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload);

    /**
     * @brief 把一帧直接拼进给定缓冲的末尾（载荷已在手时用它，别再造临时帧串）
     * @details 帧的布局与校验只在本函数一处定义，`encodeHttp2Frame()` 是它的「交出一份新串」便利入口。
     *          出站 DATA 帧的载荷本就是连接待发缓冲里的一段，先拼进临时串再搬过去等于整段白拷一遍。
     * @param bytes 目标缓冲，只能追加；抛错时一字节未加
     * @param type 帧类型
     * @param flags 标志位
     * @param streamId 流号；连接级帧传 0
     * @param payload 负载字节，本函数同步拷完才返回，不留视图
     * @throws Base::InvalidArgumentException 用法错误：负载超过 24 位长度域、流号超出 31 位，或类型未定义
     */
    void appendHttp2Frame(std::string &bytes, Http2FrameType type, std::uint8_t flags, std::uint32_t streamId,
                          std::string_view payload);

    /**
     * @brief 把一帧 HEADERS 直接拼进给定缓冲末尾（头块片段已在手时用它）
     * @details 与 `encodeHttp2HeadersFrame()` 的差别只在负载不必先拷成片段、帧也不先攒进临时串：
     *          连接发的头块本就躺在自己的缓冲里。带优先级字段的形态没有对应的视图出口（发送侧
     *          从不带那个字段），要走 `encodeHttp2HeadersFrame()`。
     * @param bytes 目标缓冲，只能追加；抛错时一字节未加
     * @param headerBlockFragment 头块片段（HPACK 编码后的字节）
     * @param endStream END_STREAM：该流正文随本帧结束
     * @param endHeaders END_HEADERS：头块在本帧内结束，后面没有 CONTINUATION
     * @param streamId 目标流号，必须非 0
     * @throws Base::InvalidArgumentException 用法错误：流号为 0，或片段长度超出 24 位长度域
     */
    void appendHttp2HeadersFrame(std::string &bytes, std::string_view headerBlockFragment, bool endStream, bool endHeaders,
                                 std::uint32_t streamId);

    // ============================================================================
    // 具名负载结构体（RFC 7540 §6.x）
    // ============================================================================

    /**
     * @brief SETTINGS 的一个参数（RFC 7540 §6.5.1）
     */
    struct Http2Setting
    {
        std::uint16_t identifier{0}; ///< 16 位参数标识，取值见 Http2SettingIdentifier
        std::uint32_t value{0};      ///< 32 位参数取值
    };

    /**
     * @brief SETTINGS 的 6 个具名参数标识（RFC 7540 §6.5.2）
     *
     * @note 新增取值一律追加在末尾；未知标识不判错，由上层忽略（§6.5.2 明确要求）。
     */
    enum class Http2SettingIdentifier : std::uint16_t
    {
        HeaderTableSize      = 0x1, ///< 本端解码头块所用的动态表上限，初值 4096 字节
        EnablePush           = 0x2, ///< 是否允许服务端推送，初值 1；取值只能是 0 或 1
        MaxConcurrentStreams = 0x3, ///< 本端允许的对端并发流上限，初值不限
        InitialWindowSize    = 0x4, ///< 流的初始流控窗口，初值 65535 字节
        MaxFrameSize         = 0x5, ///< 本端愿意接收的最大帧负载，初值 16384 字节
        MaxHeaderListSize    = 0x6, ///< 本端愿意接收的头列表大小（§6.5.2 的算式），初值不限
        EnableConnectProtocol = 0x8 ///< 是否接受带 :protocol 的扩展 CONNECT（RFC 8441）：1 表示接受；取值只能是 0 或 1
    };

    /**
     * @brief SETTINGS 帧负载（RFC 7540 §6.5）
     */
    struct Http2SettingsPayload
    {
        bool isAcknowledgement{false};          ///< ACK 标志：置位时负载必须为空（§6.5）
        std::vector<Http2Setting> parameters;   ///< 参数按到达顺序排列，含未知标识（§6.5.2 要求忽略而非判错）
    };

    /**
     * @brief PING 帧负载（RFC 7540 §6.7）
     */
    struct Http2PingPayload
    {
        bool isAcknowledgement{false};          ///< ACK 标志：置位表示这是对端 PING 的回声
        std::array<std::uint8_t, 8> opaqueData{}; ///< 8 字节不透明数据，收到后必须原样回送（§6.7）
    };

    /**
     * @brief GOAWAY 帧负载（RFC 7540 §6.8）
     */
    struct Http2GoAwayPayload
    {
        std::uint32_t lastStreamId{0};                      ///< 31 位「最后处理的流号」，0 表示一条都没处理
        Http2ErrorCode errorCode{Http2ErrorCode::NoError};  ///< 关闭原因
        std::string debugData;                              ///< 额外调试数据（可为空，不参与协议判定）
    };

    /**
     * @brief RST_STREAM 帧负载（RFC 7540 §6.4）
     */
    struct Http2RstStreamPayload
    {
        Http2ErrorCode errorCode{Http2ErrorCode::NoError}; ///< 终止流的原因
    };

    /**
     * @brief WINDOW_UPDATE 帧负载（RFC 7540 §6.9）
     */
    struct Http2WindowUpdatePayload
    {
        std::uint32_t windowSizeIncrement{0}; ///< 31 位窗口增量，取值必须非 0（§6.9）
    };

    /**
     * @brief DATA 帧负载（RFC 7540 §6.1）
     */
    struct Http2DataPayload
    {
        bool endStream{false}; ///< END_STREAM 标志：这是本流最后一个数据帧
        std::string data;      ///< 应用数据（padding 已由帧层剥掉）
    };

    /**
     * @brief HEADERS 帧负载（RFC 7540 §6.2）
     */
    struct Http2HeadersPayload
    {
        bool endStream{false};            ///< END_STREAM 标志：头块之后的正文到此为止
        bool endHeaders{false};           ///< END_HEADERS 标志：头块在本帧内结束，后面没有 CONTINUATION
        bool hasPriority{false};          ///< 是否带优先级字段
        Http2Priority priority{};         ///< 优先级字段，hasPriority 为 true 时有效
        std::string headerBlockFragment;  ///< 头块片段（padding 与优先级字段已剥掉），交给 HPACK 解码
    };

    /**
     * @brief CONTINUATION 帧负载（RFC 7540 §6.10）
     */
    struct Http2ContinuationPayload
    {
        bool endHeaders{false};           ///< END_HEADERS 标志：头块在本帧内结束
        std::string headerBlockFragment;  ///< 头块片段，必须紧接在同一条头块的前一片段之后
    };

    // ============================================================================
    // 具名负载编码：产出的都是**完整帧**，标志与流号按 RFC 约束一并落定
    // ============================================================================

    /**
     * @brief 编码 SETTINGS 帧（流号恒为 0，ACK 置位时负载为空）
     * @param payload 负载结构体
     * @return std::string 完整帧字节
     * @throws Base::InvalidArgumentException 用法错误：ACK 置位却带参数（§6.5 要求 ACK 帧负载为空）
     */
    [[nodiscard]] std::string encodeHttp2SettingsFrame(const Http2SettingsPayload &payload);

    /**
     * @brief 编码 PING 帧（流号恒为 0）
     * @param payload 负载结构体
     * @return std::string 完整帧字节
     */
    [[nodiscard]] std::string encodeHttp2PingFrame(const Http2PingPayload &payload);

    /**
     * @brief 编码 GOAWAY 帧（流号恒为 0）
     * @param payload 负载结构体
     * @return std::string 完整帧字节
     */
    [[nodiscard]] std::string encodeHttp2GoAwayFrame(const Http2GoAwayPayload &payload);

    /**
     * @brief 编码 RST_STREAM 帧
     * @param payload 负载结构体
     * @param streamId 目标流号，必须非 0（§6.4 要求 RST_STREAM 关联到一条流）
     * @return std::string 完整帧字节
     * @throws Base::InvalidArgumentException 用法错误：streamId 为 0
     */
    [[nodiscard]] std::string encodeHttp2RstStreamFrame(const Http2RstStreamPayload &payload, std::uint32_t streamId);

    /**
     * @brief 编码 WINDOW_UPDATE 帧
     * @param payload 负载结构体
     * @param streamId 目标流号；0 表示连接级窗口
     * @return std::string 完整帧字节
     * @throws Base::InvalidArgumentException 用法错误：增量为 0（§6.9 要求增量必须非 0，对端收到必然
     *         判错，与其发出去被断连不如在本地拒绝），或增量超出 31 位
     */
    [[nodiscard]] std::string encodeHttp2WindowUpdateFrame(const Http2WindowUpdatePayload &payload, std::uint32_t streamId);

    /**
     * @brief 编码 DATA 帧（不产生 padding：服务端发出的数据帧不填充）
     * @param payload 负载结构体
     * @param streamId 目标流号，必须非 0（§6.1 要求 DATA 关联到一条流）
     * @return std::string 完整帧字节
     * @throws Base::InvalidArgumentException 用法错误：streamId 为 0
     */
    [[nodiscard]] std::string encodeHttp2DataFrame(const Http2DataPayload &payload, std::uint32_t streamId);

    /**
     * @brief 编码 DATA 帧（负载按「指针 + 长度」取，不经过负载结构体）
     * @param data 应用数据
     * @param endStream 是否置 END_STREAM
     * @param streamId 目标流号，必须非 0（§6.1 要求 DATA 关联到一条流）
     * @return std::string 完整帧字节
     * @throws Base::InvalidArgumentException 用法错误：streamId 为 0
     * @note 发送路径用这个重载：待发缓冲里的字节可以直接按视图交给编码器，
     *       不必先拷进 Http2DataPayload::data 再由编码器拷进帧缓冲（正文一大就是整段白拷一次）
     */
    [[nodiscard]] std::string encodeHttp2DataFrame(std::string_view data, bool endStream, std::uint32_t streamId);

    /**
     * @brief 编码 HEADERS 帧（不产生 padding；hasPriority 为 true 时写出 5 字节优先级字段）
     * @param payload 负载结构体
     * @param streamId 目标流号，必须非 0（§6.2 要求 HEADERS 关联到一条流）
     * @return std::string 完整帧字节
     * @throws Base::InvalidArgumentException 用法错误：streamId 为 0，或优先级字段依赖自身流号（§5.3.1）
     */
    [[nodiscard]] std::string encodeHttp2HeadersFrame(const Http2HeadersPayload &payload, std::uint32_t streamId);

    /**
     * @brief 编码 CONTINUATION 帧
     * @param payload 负载结构体
     * @param streamId 目标流号，必须非 0
     * @return std::string 完整帧字节
     * @throws Base::InvalidArgumentException 用法错误：streamId 为 0
     */
    [[nodiscard]] std::string encodeHttp2ContinuationFrame(const Http2ContinuationPayload &payload, std::uint32_t streamId);

    /**
     * @brief 编码 PRIORITY 帧（负载固定 5 字节）
     * @param priority 优先级字段
     * @param streamId 目标流号，必须非 0（§6.3 要求 PRIORITY 关联到一条流）
     * @return std::string 完整帧字节
     * @throws Base::InvalidArgumentException 用法错误：streamId 为 0，或依赖的父流号等于自身流号
     */
    [[nodiscard]] std::string encodeHttp2PriorityFrame(const Http2Priority &priority, std::uint32_t streamId);

    // ============================================================================
    // 具名负载解析：入参是帧层交出的帧，负载已是净负载（padding 与优先级字段不在里面）
    // ============================================================================

    /**
     * @brief 解析 SETTINGS 帧
     * @details 未知参数标识一律原样收进 parameters 而不判错（RFC 7540 §6.5.2 明确要求忽略），
     *          取具名参数用 tryGetHttp2Setting()。ACK 帧的 parameters 为空。
     * @param frame 帧层交出的帧
     * @param payload 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功
     * @return false 帧类型不是 SETTINGS，或负载长度不是 6 的整数倍
     */
    [[nodiscard]] bool parseHttp2SettingsPayload(const Http2Frame &frame, Http2SettingsPayload &payload, std::string *errorText = nullptr);

    /**
     * @brief 从 SETTINGS 参数里取一个具名参数的取值
     * @details 同一标识重复出现时以**最后一次**为准（RFC 7540 §6.5：参数按出现顺序处理，值取最后见到的）。
     * @param payload 已解析的 SETTINGS 帧
     * @param identifier 目标参数标识
     * @param value 输出参数：参数取值，仅在返回 true 时有效
     * @return true 对端带了该参数
     * @return false 对端没带该参数（保持默认值），不是错误
     */
    [[nodiscard]] bool tryGetHttp2Setting(const Http2SettingsPayload &payload, Http2SettingIdentifier identifier, std::uint32_t &value) noexcept;

    /**
     * @brief 解析 PING 帧
     * @param frame 帧层交出的帧
     * @param payload 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功
     * @return false 帧类型不是 PING，或负载不是 8 字节
     */
    [[nodiscard]] bool parseHttp2PingPayload(const Http2Frame &frame, Http2PingPayload &payload, std::string *errorText = nullptr);

    /**
     * @brief 解析 GOAWAY 帧
     * @param frame 帧层交出的帧
     * @param payload 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功
     * @return false 帧类型不是 GOAWAY，或负载不足 8 字节
     */
    [[nodiscard]] bool parseHttp2GoAwayPayload(const Http2Frame &frame, Http2GoAwayPayload &payload, std::string *errorText = nullptr);

    /**
     * @brief 解析 RST_STREAM 帧
     * @param frame 帧层交出的帧
     * @param payload 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功
     * @return false 帧类型不是 RST_STREAM，或负载不是 4 字节
     */
    [[nodiscard]] bool parseHttp2RstStreamPayload(const Http2Frame &frame, Http2RstStreamPayload &payload, std::string *errorText = nullptr);

    /**
     * @brief 解析 WINDOW_UPDATE 帧
     * @param frame 帧层交出的帧
     * @param payload 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功
     * @return false 帧类型不是 WINDOW_UPDATE，或负载不是 4 字节
     */
    [[nodiscard]] bool parseHttp2WindowUpdatePayload(const Http2Frame &frame, Http2WindowUpdatePayload &payload,
                                                     std::string *errorText = nullptr);

    /**
     * @brief 解析 DATA 帧
     * @param frame 帧层交出的帧
     * @param payload 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功
     * @return false 帧类型不是 DATA
     */
    [[nodiscard]] bool parseHttp2DataPayload(const Http2Frame &frame, Http2DataPayload &payload, std::string *errorText = nullptr);

    /**
     * @brief 解析 HEADERS 帧
     * @param frame 帧层交出的帧
     * @param payload 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功
     * @return false 帧类型不是 HEADERS
     */
    [[nodiscard]] bool parseHttp2HeadersPayload(const Http2Frame &frame, Http2HeadersPayload &payload, std::string *errorText = nullptr);

    /**
     * @brief 解析 CONTINUATION 帧
     * @param frame 帧层交出的帧
     * @param payload 输出参数：解析结果，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解析成功
     * @return false 帧类型不是 CONTINUATION
     */
    [[nodiscard]] bool parseHttp2ContinuationPayload(const Http2Frame &frame, Http2ContinuationPayload &payload,
                                                     std::string *errorText = nullptr);

    // ============================================================================
    // 增量帧解码器
    // ============================================================================

    /**
     * @brief 帧解码器的接收上限（本端策略，随 SETTINGS 一并通告给对端）
     */
    struct Http2FrameLimits
    {
        /// 单帧负载上限，对应本端通告的 SETTINGS_MAX_FRAME_SIZE；合法区间 [16384, 16777215]（RFC 7540 §6.5.2），
        /// 超出即长度违规（FRAME_SIZE_ERROR）。默认值即规范的初始值 16384
        std::size_t maximumFrameSizeByteCount{kHttp2DefaultMaximumFrameSize};

        /// 解码器自构造或上次 reset() 以来累计消费字节数的上限，0 表示不限：它是本端防「长连接上无限
        /// 读下去」的兜底闸门，触发时按 ENHANCE_YOUR_CALM 回对端（§7 对过量负载的建议取值）
        std::size_t maximumTotalConsumedByteCount{0};
    };

    /**
     * @brief 一次 Http2FrameDecoder::parse() 调用的结论状态
     *
     * @details 判定顺序固定为「先看是否已产出帧 → 再看是否已失败 → 其余为需要更多数据」，与
     *          WebSocketDecodeStatus 同构且三态互斥穷尽。
     * @note Frame 是唯一的完成证据：取走之前再喂数据一字节不吃；Error 粘滞（除非 reset()），
     *       其中「资源上限」这一子类由 errorKind() 区分。
     */
    enum class Http2FrameDecodeStatus
    {
        NeedMore, ///< 数据不足，需要继续读取网络字节后再次调用 parse()
        Frame,    ///< 一帧已就绪，此刻才允许调用 takeFrame()
        Error     ///< 对端违反 RFC 7540 或突破本端上限，解码器进入粘滞错误态，调用方应按错误码收场
    };

    /**
     * @brief HTTP/2 帧的增量解码器
     *
     * @details 逐字节推进，任意字节边界都能切开续上（含切在 9 字节帧头中间、负载中间）。帧头收齐即
     *          校验并在负载到齐后剥掉 padding 与优先级字段，takeFrame() 交出的是**已解释过帧级字段**的
     *          单帧；跨帧语义（流状态、流控、头块拼接）一概不做，留给会话层。
     *
     * @note 未知帧类型（不在 Http2FrameType 里的取值）按 §4.1 必须忽略：解码器原样交出该帧，由上层
     *       丢弃——判错会让「未来新增的帧类型」把整条连接打死，而规范要求的是跳过。
     * @note 从严之处：帧头 R 位非 0 判错（§4.1 允许接收侧忽略，见 decodeHttp2FrameHeader() 的说明）；
     *       类型要求的固定长度不符时按 §6.x 判 FRAME_SIZE_ERROR，不留给自己在会话层再判一遍。
     * @warning 负载按「指针 + 长度」处理，可含 NUL 与任意二进制；GOAWAY 的调试数据、PING 的不透明
     *          数据都不在本层解释语义。
     */
    class Http2FrameDecoder
    {
    public:
        /**
         * @brief 构造解码器：取一份接收上限配置，其余状态为初态，可直接开始解码
         * @param limits 接收上限配置；默认值对应规范的初始 SETTINGS 取值
         * @throws Base::InvalidArgumentException 配置错误：maximumFrameSizeByteCount 不在
         *         [16384, 16777215] 内，此时本端等于在通告一个非法的 SETTINGS_MAX_FRAME_SIZE
         */
        explicit Http2FrameDecoder(Http2FrameLimits limits = {});

        /**
         * @brief 析构函数：成员都是按值的标准容器，无额外资源需要回收。
         */
        ~Http2FrameDecoder() = default;

        // 禁拷贝：解码器内部持有「解码到一半」的状态与已产出待取走的帧，
        // 复制一份会把这个状态静默分叉成两份，而两边的字节流并不相同
        Http2FrameDecoder(const Http2FrameDecoder &) = delete;

        Http2FrameDecoder &operator=(const Http2FrameDecoder &) = delete;

        /**
         * @brief 喂入一段字节，推进解码
         *
         * @details 状态判定三步互斥且穷尽：已有产出待取走 → 直接返回 Frame 且一字节不吃；已失败 →
         *          返回 Error 且一字节不吃；否则逐字节推进，一产出帧就停下——剩下的字节属于下一帧，
         *          由调用方按 consumedByteCount() 挪掉后重新喂入。
         * @param data 数据起始指针，调用方保证可读
         * @param length 数据长度，单位字节
         * @retval Http2FrameDecodeStatus::Frame 一帧已就绪，等待 takeFrame() 取走
         * @retval Http2FrameDecodeStatus::NeedMore 本段字节已全部消费，帧还没收齐
         * @retval Http2FrameDecodeStatus::Error 帧非法或突破本端上限，粘滞到 reset() 为止
         * @see takeFrame(), consumedByteCount(), errorKind(), errorMessage()
         */
        Http2FrameDecodeStatus parse(const char *data, std::size_t length);

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
         * @return Http2Frame 产出的帧，负载按移动移交（不额外拷贝）
         * @throws Base::LogicException 用法错误：当前没有待取走的帧（parse() 未返回过 Frame）
         */
        [[nodiscard]] Http2Frame takeFrame();

        /**
         * @brief 重置解码器状态，以便接着解码下一段字节流
         * @details 清空半成品帧、已产出待取走的帧、粘滞错误标记与累计读取计数，回到初态。
         */
        void reset();

        /**
         * @brief 检查解码器是否处于错误状态。
         * @return true 表示发生过错误（含突破本端上限），false 表示无错误
         */
        [[nodiscard]] bool hasError() const;

        /**
         * @brief 获取本次失败的类别
         * @details 上层据它决定回哪个错误码（见 toHttp2ErrorCode()），不必去匹配错误文案
         * @return Http2FrameErrorKind 失败类别；未失败时为 None
         */
        [[nodiscard]] Http2FrameErrorKind errorKind() const;

        /**
         * @brief 获取错误信息描述（若有）。
         * @return 面向使用者的中文错误文本；无错误时为空串
         */
        [[nodiscard]] std::string errorMessage() const;

        /**
         * @brief 取回自构造或上次 reset() 以来累计消费的字节数
         * @details 只在配置了 maximumTotalConsumedByteCount 时有消费方（会话据此判断是否该换连接），
         *          它同时也是排查「对端在读什么」的观测点。
         * @return std::size_t 累计消费字节数
         */
        [[nodiscard]] std::size_t totalConsumedByteCount() const;

    private:
        /**
         * @brief 解码所处的阶段
         */
        enum class Stage
        {
            FrameHeader, ///< 正在收 9 字节帧头
            Payload,     ///< 正在收负载
            Failed       ///< 已失败：错误粘滞到 reset()
        };

        /**
         * @brief 校验并落定一帧的帧头（含各类型对长度与流号的固定要求）
         * @param headerBytes 刚收齐的 9 字节帧头
         * @return true 合法
         */
        [[nodiscard]] bool acceptFrameHeader(const std::array<std::uint8_t, kHttp2FrameHeaderByteCount> &headerBytes);

        /**
         * @brief 按帧头校验帧的形态：类型对长度与流号的固定要求
         * @details RFC 7540 §6.x 对 SETTINGS / PING / RST_STREAM / WINDOW_UPDATE / GOAWAY / PRIORITY 的
         *          负载长度与流号各有硬性要求，不符即 FRAME_SIZE_ERROR 或 PROTOCOL_ERROR；未定义类型按
         *          §4.1 忽略，不在这里判错。
         * @param header 已校验过帧头字段的帧头
         * @return true 形态合法
         */
        [[nodiscard]] bool acceptFrameShape(const Http2FrameHeader &header);

        /**
         * @brief 负载收齐：剥掉 padding 与优先级字段、补齐标志解释，产出一帧
         */
        void completeFrame();

        /**
         * @brief 统一的失败记录：置粘滞错误态并补上中文前缀
         * @param errorKind 失败类别（决定回哪个错误码）
         * @param reason 中文失败详情（不含前缀）
         */
        void recordFailure(Http2FrameErrorKind errorKind, std::string reason);

        /**
         * @brief 解析当前帧的优先级字段（HEADERS 置 PRIORITY 位或 PRIORITY 帧）
         * @param bytes 优先级字段的 5 字节
         * @return true 合法（依赖的父流号不等于本帧流号，满足 §5.3.1）
         */
        [[nodiscard]] bool acceptPriorityField(std::string_view bytes);

        /**
         * @brief 清空单帧的解析暂存（容器只清内容、保留容量，供下一帧复用）
         */
        void clearFrameScratch() noexcept;

        Http2FrameLimits m_limits{}; ///< 构造时按值落定的接收上限，没有中途更换的入口
        Stage m_stage{Stage::FrameHeader}; ///< 当前阶段

        std::array<std::uint8_t, kHttp2FrameHeaderByteCount> m_headerBytes{}; ///< 帧头字节暂存
        std::size_t m_headerBytesSeen{0};                                      ///< 已收帧头字节数

        Http2FrameHeader m_currentHeader{}; ///< 当前帧的帧头
        std::string m_payloadBuffer;        ///< 当前帧的负载暂存（收齐后按需就地剥离）
        std::size_t m_payloadBytesSeen{0};  ///< 当前帧已收负载字节数
        bool m_isCurrentFramePriority{false}; ///< 当前帧是否带优先级字段（HEADERS 置位或 PRIORITY 帧）
        Http2Priority m_currentPriority{};    ///< 当前帧的优先级字段

        Http2Frame m_pendingFrame;  ///< 已产出待取走的帧
        bool m_hasPendingFrame{false}; ///< 是否已有产出待取走

        bool m_hasError{false};                        ///< 是否已发生解码错误
        Http2FrameErrorKind m_errorKind{Http2FrameErrorKind::None}; ///< 失败类别（决定上层回哪个错误码）
        std::string m_errorMessage;                    ///< 面向使用者的中文错误描述
        std::size_t m_consumedByteCount{0};            ///< 最近一次 parse() 实际消费的字节数
        std::size_t m_totalConsumedByteCount{0};       ///< 自构造或 reset() 以来的累计消费字节数
    };
} // namespace AsynGyanis::Net
