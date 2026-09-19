/**
 * @file Http3HeaderValidation.h
 * @brief HTTP/3 消息头部的合法性判定：伪头归位、字段名字符集、连接特定字段、Content-Length 一致性
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 判据全部取自 RFC 9114 §4.1.2/§4.2/§4.3/§4.3.1/§4.3.2 与 RFC 9220 §3.2，按原文一律从严：
 *          规范明写「MUST be treated as malformed」的这里都判失败，不做「宽容收下」的降级。
 */

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "Net/Http3/Http3Error.h"

namespace AsynGyanis::Net
{
    /**
     * @brief 被判定的是请求头部还是响应头部
     *
     * @details 两者允许的伪头集合不同（RFC 9114 §4.3.1 与 §4.3.2），混在一起判会漏掉「响应里出现
     *          :method」这类跨方向的非法用法。
     */
    enum class Http3MessageKind
    {
        Request,  ///< 请求头部：允许 :method、:scheme、:authority、:path，扩展 CONNECT 另允许 :protocol
        Response, ///< 响应头部：只允许 :status
    };

    /**
     * @brief 头部判定的失败类别
     *
     * @details 取值一律对应 RFC 9114 §4.1.2 列出的某一条「malformed」，因此对上的线上错误码同一个：
     *          流错误 H3_MESSAGE_ERROR。分类只为日志与埋点能指明是哪条规则，不为了换错误码。
     *          新增成员只能追加在末尾。
     */
    enum class Http3HeaderErrorKind
    {
        EmptyFieldName,               ///< 字段名为空：无法构成 RFC 9110 §5.1 的 field-name
        UppercaseFieldName,           ///< 字段名含大写：RFC 9114 §4.2 要求编码前转小写，带大写即畸形
        IllegalFieldNameCharacter,    ///< 字段名含 tchar 之外的字符：RFC 9110 §5.1
        IllegalFieldValueCharacter,   ///< 字段值含控制字符（含 CR/LF/NUL）：会构成响应拆分，只允许 HTAB 与可见字符
        ConnectionFieldProhibited,    ///< 出现连接特定字段：RFC 9114 §4.2 明确禁止（TE 除外）
        TeValueNotAllowed,            ///< TE 出现了但值不是 trailers：RFC 9114 §4.2 的例外只给 trailers
        PseudoHeaderAfterField,       ///< 伪头出现在普通字段之后：RFC 9114 §4.3
        PseudoHeaderInTrailers,       ///< 尾段里出现伪头：RFC 9114 §4.3
        UndefinedPseudoHeader,        ///< 未定义的伪头：RFC 9114 §4.3
        DuplicatePseudoHeader,        ///< 同一伪头出现两次：RFC 9114 §4.3.1 要求「恰好一个」
        MissingPseudoHeader,          ///< 必填伪头缺失：RFC 9114 §4.3.1/§4.3.2
        ProhibitedPseudoForMethod,    ///< CONNECT 带上了不属于它的伪头，或非 CONNECT 缺 :scheme/:path
        EmptyPath,                    ///< :path 为空：RFC 9114 §4.3.1 对 http/https 明确禁止
        EmptyAuthority,               ///< :authority 或 host 为空：RFC 9114 §4.3.1
        InvalidStatusValue,           ///< :status 不是三位十进制：RFC 9114 §4.3.2
        ContentLengthInvalid,         ///< content-length 不是非负十进制整数：RFC 9110 §8.6
        ContentLengthConflict,        ///< 多个 content-length 取值不一致：RFC 9110 §8.6
        AuthorityConflict,            ///< :authority 与 host 取值冲突，或 host 重复且不一致：RFC 9114 §4.3.1
        ProhibitedFieldInTrailers,    ///< 尾段里出现 content-length/host/连接特定字段：RFC 9110 §6.5
        InvalidMessageSequence,       ///< 头块序列非法（同一条消息里出现第二个非尾段头块）：RFC 9114 §4.1
    };

    /**
     * @brief 一次头部判定的完整说明
     */
    struct Http3HeaderError
    {
        Http3HeaderErrorKind kind{Http3HeaderErrorKind::UndefinedPseudoHeader}; ///< 失败类别：上层按 §4.1.2 一律回 H3_MESSAGE_ERROR
        std::string message;                                                    ///< 中文原因，含字段名实际值与对应的 RFC 章节
    };

    /**
     * @brief 把一个头部判定失败映射成线上错误码
     * @param errorKind 失败类别
     * @return 恒为 H3_MESSAGE_ERROR（RFC 9114 §4.1.2 把这一类统一归到该码），留成函数是为了上层读代码时
     *         不必去猜「哪条规则该回哪个码」
     */
    [[nodiscard]] Http3ErrorCode toHttp3ErrorCode(Http3HeaderErrorKind errorKind) noexcept;

    /**
     * @brief 一条消息头部的累积判定器
     *
     * @details 逐字段喂进来（QPACK 解出一个交一个），内部记下伪头归位结果与顺序状态；头块收齐后
     *          调 endHeaderBlock() 补判「必填项齐不齐」。判定器不拷贝普通字段的值，只留调用方
     *          随后要用的伪头与 content-length。
     */
    class Http3HeaderValidator
    {
    public:
        /**
         * @brief 建一个判定器
         * @param messageKind 判定请求头还是响应头
         * @param isExtendedConnectPermitted true 表示本会话允许扩展 CONNECT（对端 SETTINGS 已给出
         *        ENABLE_CONNECT_PROTOCOL=1）：此时 :protocol 才是合法伪头
         */
        explicit Http3HeaderValidator(Http3MessageKind messageKind, bool isExtendedConnectPermitted = false) noexcept;

        /**
         * @brief 开始一个新的头块
         * @param isTrailers true 表示这是消息主体之后的尾段（trailer section）
         * @return 成功返回空；失败返回非法的头块序列（响应头的 :status 段之后不允许再来一个非尾段头块）
         * @note 一个头块结束后必须先调 endHeaderBlock() 才能再 beginHeaderBlock()
         */
        [[nodiscard]] std::expected<void, Http3HeaderError> beginHeaderBlock(bool isTrailers) noexcept;

        /**
         * @brief 喂进一个头字段
         * @param name 字段名或伪头名（伪头以 ':' 开头），按原样字节给出，不做大小写折叠
         * @param value 字段值，按原样字节给出
         * @return 成功返回空；失败返回具体的判定结果
         */
        [[nodiscard]] std::expected<void, Http3HeaderError> onHeaderField(std::string_view name, std::string_view value);

        /**
         * @brief 头块收齐：补判必填伪头与 authority/host 一致性
         * @return 成功返回空；失败返回缺失或冲突的具体原因
         * @note 尾段（trailer section）没有必填项，本函数只复核「尾段里不得有伪头与被禁字段」
         */
        [[nodiscard]] std::expected<void, Http3HeaderError> endHeaderBlock() noexcept;

        /// 已归位的 :method 原文；未收到时为空串
        [[nodiscard]] const std::string &methodText() const noexcept;

        /// 已归位的 :path 原文；未收到时为空串
        [[nodiscard]] const std::string &pathText() const noexcept;

        /// 已归位的 :authority 原文；未收到时为空串
        [[nodiscard]] const std::string &authorityText() const noexcept;

        /// 已归位的 :scheme 原文；未收到时为空串
        [[nodiscard]] const std::string &schemeText() const noexcept;

        /// 已归位的 :protocol 原文（RFC 9220 扩展 CONNECT）；未收到时为空串
        [[nodiscard]] const std::string &protocolText() const noexcept;

        /// 响应状态码：只有响应判定器收到 :status 后才有值
        [[nodiscard]] std::optional<int> statusCode() const noexcept;

        /// 本次消息是否显式带了 host 字段（服务端据此决定要不要用 :authority 补）
        [[nodiscard]] bool hasHostHeader() const noexcept;

        /// 本次消息声明的正文长度；未声明时无值
        [[nodiscard]] std::optional<std::uint64_t> contentLengthByteCount() const noexcept;

    private:
        /// 校验字段名的字符集与大小写（RFC 9110 §5.1、RFC 9114 §4.2）
        [[nodiscard]] std::expected<void, Http3HeaderError> validateFieldName(std::string_view name) const;

        /// 校验字段值的字符集：控制字符除 HTAB 一律拒（CR/LF 是响应拆分的入口）
        [[nodiscard]] std::expected<void, Http3HeaderError> validateFieldValue(std::string_view value) const;

        /// 归位并校验一个伪头
        [[nodiscard]] std::expected<void, Http3HeaderError> onPseudoHeader(std::string_view name, std::string_view value);

        /// 归位并校验一个普通字段
        [[nodiscard]] std::expected<void, Http3HeaderError> onRegularField(std::string_view name, std::string_view value);

        Http3MessageKind m_messageKind{Http3MessageKind::Request};   ///< 判定方向
        bool m_isExtendedConnectPermitted{false};                    ///< 本会话是否允许 :protocol
        bool m_isTrailersSection{false};                             ///< 当前头块是不是尾段
        bool m_isHeadSectionDone{false};                             ///< 响应头块是否已收过一次（防第二个 :status 段）
        bool m_sawRegularField{false};                               ///< 本头块内是否已出现过普通字段：伪头必须排在它之前
        bool m_hasHostHeader{false};                                 ///< 是否收到过 host 字段
        std::string m_hostText;                                      ///< host 字段原文：与 :authority 同时存在时要求逐字一致
        std::string m_methodText;                                    ///< :method 原文
        std::string m_pathText;                                      ///< :path 原文
        std::string m_authorityText;                                 ///< :authority 原文
        std::string m_schemeText;                                    ///< :scheme 原文
        std::string m_protocolText;                                  ///< :protocol 原文
        std::optional<int> m_statusCode;                             ///< :status 数值
        std::optional<std::uint64_t> m_contentLengthByteCount;       ///< content-length 数值，多次出现要求取值一致
    };
} // namespace AsynGyanis::Net
