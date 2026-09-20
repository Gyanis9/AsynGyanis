/**
 * @file HttpRequest.h
 * @brief 解析后的 HTTP 请求数据对象
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/Http/HttpMethod.h"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 判定 Expect 头部是否在等 `100-continue`（RFC 9110 §10.1.1）
     *
     * @details 取值是逗号分隔的 token 列表（大小写不敏感），因此 `100-Continue`、`100-continue, foo`
     *          都算命中。h1 的解析器与 h2 的会话共用这一份判定：同一条规范语义只留一个出处，
     *          两处不会各自漂移。
     * @param expectHeaderValue Expect 头部的值；对端没给这个头时传空串
     * @return true 对端声明了 100-continue：本端应在读正文之前先回一个 100（或不等正文就回最终状态）
     */
    [[nodiscard]] bool isContinueExpected(std::string_view expectHeaderValue) noexcept;

    class HttpRequestBody;

    /**
     * @brief HTTP 请求数据对象
     *
     * @details 存储解析后的 HTTP 请求内容：方法、URI、版本、头部、正文与路由参数。
     *          本对象由 HttpParser 逐字段填充，本身不做任何 IO，也不校验报文合法性。
     *
     * @note 头部存储见 HttpHeaderFieldStore：按到达顺序的权威记录 + 按需重建的单值视图
     *       （普通头部按 RFC 7230 §3.2.2 用 ", " 合并、可重复头部保留首条）。
     *       头部名一律转小写存储（RFC 9110 §5.1 大小写不敏感），查询侧同样归一化。
     */
    class HttpRequest
    {
    public:
        /**
         * @brief 默认构造函数，创建一个空请求对象。
         */
        HttpRequest() = default;

        /**
         * @brief 将字符串转换为 HttpMethod 枚举。
         * @param method HTTP 方法原文，区分大小写，须为 "GET"、"POST" 这类大写形式
         * @return 对应的 HttpMethod 枚举值；不在收录表内（含小写形式）返回 HttpMethod::UNKNOWN
         */
        static HttpMethod methodFromString(std::string_view method);

        /**
         * @brief 设置 HTTP 请求方法。
         * @param method HTTP 方法枚举值
         */
        void setMethod(HttpMethod method);

        /**
         * @brief 获取 HTTP 请求方法。
         * @return HttpMethod 枚举值；未经解析器填充时为 HttpMethod::UNKNOWN
         */
        [[nodiscard]] HttpMethod method() const;

        /**
         * @brief 设置请求 URI（请求行中的原始 URI，含查询串）。
         * @param uri URI 字符串
         */
        void setUri(std::string uri);

        /**
         * @brief 获取原始 URI。
         * @return URI 字符串引用
         */
        [[nodiscard]] const std::string &uri() const;

        /**
         * @brief 设置 HTTP 版本（如 "HTTP/1.1"）。
         * @param version 版本字符串，按解析器读到的原文保存
         */
        void setHttpVersion(std::string version);

        /**
         * @brief 获取 HTTP 版本。
         * @return 版本字符串引用
         */
        [[nodiscard]] const std::string &httpVersion() const;

        /**
         * @brief 追加一条 HTTP 头部字段。
         * @details 头部名转小写后入库。可重复头部（当前只有 set-cookie）每条独立留档；
         *          其余头部同名再次到达时，值以 ", " 追加到已有条目上，且条目位置不变。
         * @param key   头部字段名，大小写不敏感
         * @param value 头部字段值，原样保存不做裁剪
         * @see headerValues(), headers()
         */
        void addHeader(std::string key, std::string value);

        /**
         * @brief 获取指定名称的 HTTP 头部值。
         * @param key 头部字段名，大小写不敏感（内部统一按小写存储与查找）
         * @return 命中时返回该名字的单个值：普通头部为 ", " 合并后的完整值，
         *         可重复头部为首条的值；未命中时返回空 optional
         * @see headerValues() 需要逐条取值时使用
         */
        [[nodiscard]] std::optional<std::string> getHeader(const std::string &key) const;

        /**
         * @brief 获取指定名称的全部头部值，按线上到达顺序返回。
         * @details 用于 Set-Cookie 这类禁止合并的头部，以及确实收到多条同名普通头部的场景。
         * @param key 头部字段名，大小写不敏感
         * @return 值列表；名字不存在时为空列表
         */
        [[nodiscard]] std::vector<std::string> headerValues(const std::string &key) const;

        /**
         * @brief 取指定名称的首条头部值（原样，不参与同名多条的 ", " 合并）
         * @details 与 headerValues() 的首元素同值，但不为「只要一个值」构造整列值列表。
         *          链路 id 这类同名多条各表一个来源的头部，要的就是首条原值。
         * @param key 头部字段名，大小写不敏感
         * @return std::optional<std::string> 首条值；名字不存在时为空
         */
        [[nodiscard]] std::optional<std::string> firstHeaderValue(std::string_view key) const;

        /**
         * @brief 判断指定名称的头部取值里是否出现了某个逗号分隔的 token（RFC 9110 §5.6.1）
         * @details 例如 `Connection: keep-alive, Upgrade` 含 "upgrade" 而不含 "close"。
         *          判定在存储内部逐段完成，既不拷贝取值也不构造值列表：Connection/Upgrade
         *          这类判定每条请求都要跑几遍，而调用方只需要一个布尔结果。
         * @param key 头部字段名，大小写不敏感
         * @param expectedToken 待查找的 token，大小写不敏感
         * @return true 至少一条取值列出了该 token
         */
        [[nodiscard]] bool hasHeaderValueToken(std::string_view key, std::string_view expectedToken) const;

        /**
         * @brief 获取所有头部字段的单值视图。
         * @return 名到值的 unordered_map 引用，键为小写头部名；
         *         可重复头部在此只有一条（首次出现的值），逐条取值请用 headerValues()
         */
        [[nodiscard]] const std::unordered_map<std::string, std::string> &headers() const;

        /**
         * @brief 设置请求正文，覆盖已有内容。
         * @param body 正文内容
         */
        void setBody(std::string body);

        /**
         * @brief 向请求正文尾部追加一段字节。
         * @param data   数据起始指针，调用方保证非空且可读
         * @param length 追加长度，单位字节
         * @note 不做上限校验，资源上限由 HttpParser 把关
         */
        void appendBody(const char *data, size_t length);

        /**
         * @brief 获取请求正文（字符串视图）。
         * @return 正文内容的 string_view，视图生命周期跟随本请求对象
         */
        [[nodiscard]] std::string_view body() const;

        /**
         * @brief 获取本请求的正文流（普通与流式派发都可读）
         *
         * @details 会话在连接建立时装配一次，因此对 h1 的每一条请求都非空。两种派发下的行为：
         *          @li 普通路由在请求收齐后派发：流把已缓冲的全部正文作为一段交出、随后 EOF；
         *              此时 body() 同样给出全量正文（两者等价，流是为统一写法提供的）；
         *          @li 流式路由（Router::postStreaming()/putStreaming() 注册）在头部收齐即派发：
         *              正文边收边交、且处理器不拉取时连接不再读入（背压）；此时 body() 只含
         *              「已收但尚未经流交付」的残余字节，正文的权威来源是流。
         *
         * @return HttpRequestBody* 正文流指针；h2 会话与直接构造的请求对象上可能为空，取用前判空
         * @see HttpRequestBody, Router::postStreaming()
         */
        [[nodiscard]] HttpRequestBody *bodyStream() const noexcept;

        /**
         * @brief 装配正文流（会话内部使用，按连接调用一次）
         * @param bodyStream 正文流对象；生命周期由会话保证覆盖整条连接（跨请求复用同一对象）
         * @note 指针不参与 reset()：它属于连接而不是单条报文，解析器复位不应把它清掉
         */
        void setBodyStream(HttpRequestBody *bodyStream) noexcept;

        /**
         * @brief 设置本次请求的 request-id
         *
         * @details 由会话在请求收齐、进入业务之前落定（见 detail::httpKeepAliveLoop()）：
         *          客户端自带合法的 x-request-id 就沿用，否则用服务器侧生成器发一个。
         *          业务、中间件与日志因此都从一处读到同一个值。
         *
         * @param requestId 本次请求的标识；传空串表示不采集（例如该会话没有生成器）
         * @note 它是**可观测性标识，不是安全令牌**：不参与鉴权，也不要求不可预测
         */
        void setRequestId(std::string requestId);

        /**
         * @brief 获取本次请求的 request-id
         * @return 标识文本的视图，生命周期跟随本请求对象；会话未设置时为空视图
         * @see setRequestId(), HttpRequestIdGenerator
         */
        [[nodiscard]] std::string_view requestId() const noexcept;

        /**
         * @brief 从 URI 中提取路径部分（'?' 之前的内容，不含查询参数）。
         * @return 路径视图；URI 为空时返回空视图
         * @note 返回的是原始文本，未做百分号解码
         * @note 返回**视图**而不是副本：它指向本对象持有的 URI，生命周期跟随本请求对象。
         *       路由这类每请求都要看路径的地方因此不必再付一次字符串拷贝
         */
        [[nodiscard]] std::string_view path() const;

        /**
         * @brief 解析 URI 中的查询参数（'?' 之后的 key=value 串）。
         *
         * @details 先按 '&' 拆成一个个分对，每对再按「第一个 '='」拆成键与值，键值都做百分号
         *          解码（'+' 按空格处理，序列规则见 percentDecode()）。无 '?' 或其后为空 → 空表；
         *          无值键与没有 '=' 的分对 → 值为空串；键为空的分对（"?&a=1"、"?=v"）直接跳过；
         *          值中的 '=' 整体算作值（"?q=a=b" 得 "a=b"）；重复键后出现的覆盖先出现的。
         * @return 查询参数的 unordered_map，键值均为解码后的文本
         */
        [[nodiscard]] std::unordered_map<std::string, std::string> queryParams() const;

        /**
         * @brief 设置路由参数（路径中的 ":id" 之类占位符匹配到的值）。
         * @param key   参数名
         * @param value 参数值，同名覆盖
         */
        void setParam(std::string key, std::string value);

        /**
         * @brief 获取路由参数值。
         * @param key 参数名，区分大小写（与路由模板中的写法一致，不做归一化）
         * @return 参数值的 optional，若不存在则为空
         */
        [[nodiscard]] std::optional<std::string> param(const std::string &key) const;

        /**
         * @brief 重置请求对象，清空所有字段（方法、URI、头部、正文、参数等）。
         * @details 取消信号一并重建：旧的 stop_source 可能已被超时中间件触发，
         *          复用连接时必须让下一条请求拿到一个未被取消的令牌。
         */
        void reset();

        // ====================================================================
        // 协作式取消支持（用于超时中间件等场景）
        // ====================================================================

        /**
         * @brief 获取与本次请求关联的取消令牌。
         * @details 中间件（如 timeoutMiddleware）可设置 stop_source，业务处理器定期检查
         *          token.stop_requested() 以实现协作式取消，避免超时后继续浪费资源。
         * @return 当前取消源的令牌快照；每次调用返回的令牌共享同一个状态源
         */
        [[nodiscard]] std::stop_token cancelToken() const noexcept;

        /**
         * @brief 获取内部 stop_source 的可写引用，供中间件设置取消信号。
         * @return stop_source 可变引用
         */
        [[nodiscard]] std::stop_source &cancelSource() noexcept;

        /**
         * @brief 请求取消本次处理（由中间件在超时等条件下调用）。
         * @return true 本次调用发出了新的取消信号；false 早已被取消过，重复调用不会二次通知
         */
        bool requestCancel() const;

    private:
        /**
         * @brief URL 百分号解码
         * @details 宽松解码，永不报错："%XX" → 对应字节、'+' → 空格（表单编码约定），其余字符原样保留；
         *          非法或残缺的序列（"%"、"%4"、"%4G"）连同百分号按原文保留，不吞字符也不报错。不报错是
         *          因为查询串来自外部输入，解码失败不足以否定整个请求。
         * @param source 待解码文本
         * @return 解码后的文本
         */
        static std::string percentDecode(std::string_view source);

        HttpMethod m_method{HttpMethod::UNKNOWN};                  ///< HTTP 方法
        std::string m_uri;                                         ///< 原始 URI，含查询串
        std::string m_httpVersion;                                 ///< HTTP 版本原文
        HttpHeaderFieldStore m_headerStore;                        ///< 头部存储：权威记录 + 按需重建的单值视图（见该类注释）
        std::string m_body;                                        ///< 消息正文
        HttpRequestBody *m_bodyStream{nullptr};                    ///< 正文流（按连接装配，见 bodyStream()；不随 reset() 清除）
        std::string m_requestId;                                   ///< 本次请求的可观测性标识，由会话在业务之前落定（见 setRequestId()）
        std::unordered_map<std::string, std::string> m_params;     ///< 路由参数
        mutable std::stop_source m_cancelSource;                   ///< 协作式取消源：被触发过才在 reset() 里重建，未触发则跨请求沿用（省掉每请求一次分配）
    };
} // namespace AsynGyanis::Net
