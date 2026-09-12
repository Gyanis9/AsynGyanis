/**
 * @file HttpRequest.h
 * @brief 解析后的 HTTP 请求数据对象
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

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
     * @brief HTTP 请求数据对象
     *
     * @details 存储解析后的 HTTP 请求内容：方法、URI、版本、头部、正文与路由参数。
     *          本对象由 HttpParser 逐字段填充，本身不做任何 IO，也不校验报文合法性。
     *
     * @note 头部存储模型（本次重构修正，旧实现用 "set-cookie_1" 这类带后缀的伪键容纳多值，
     *       那是对 HTTP 语法的曲解）：
     *       @li 权威记录 m_headerFields —— 按「线上到达顺序」每条头部占一项，可重复头部各占一项，
     *           是 headerValues() 的数据来源；
     *       @li 单值视图 m_headers —— 名到值的映射，每个名字恰有一条：普通头部按
     *           RFC 7230 §3.2.2 的收件人规则用 ", " 合并，可重复头部保留首次出现的值。
     *           headers()/getHeader() 这两个既有接口就读这张表，语义与旧版一致。
     *       @li 头部名一律转小写存储。HTTP 头部名大小写不敏感（RFC 9110 §5.1），
     *           归一化后 Content-Type 与 content-type 命中同一条，查询侧同样归一化。
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
         * @brief 从 URI 中提取路径部分（'?' 之前的内容，不含查询参数）。
         * @return 路径字符串；URI 为空时返回空串
         * @note 返回的是原始文本，未做百分号解码
         */
        [[nodiscard]] std::string path() const;

        /**
         * @brief 解析 URI 中的查询参数（'?' 之后的 key=value 串）。
         *
         * @details 切分规则：先按 '&' 拆成一个个分对，每个分对再按「第一个 '='」拆成键与值，
         *          键与值都做百分号解码。行为约定：
         *          @li 无 '?' 或 '?' 后为空 → 返回空表；
         *          @li 无值键（"?a&b=1" 里的 a）→ 值为空串；
         *          @li 分对没有 '='（"?flag"）→ 值为空串；
         *          @li 键为空的分对（"?&a=1"、"?=v"）→ 直接跳过，不产生空键；
         *          @li 值中出现 '='（"?q=a=b"）→ 从第一个 '=' 之后整体算作值，即值为 "a=b"；
         *          @li 重复键（"?a=1&a=2"）→ 后出现的覆盖先出现的，与旧实现一致；
         *          @li '+' 按空格处理（表单编码约定），百分号序列见 percentDecode()。
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
         * @brief 单条头部字段的线上原样记录
         */
        struct HeaderField
        {
            std::string name;  ///< 已归一化为小写的头部名
            std::string value; ///< 头部值原文
        };

        using HeaderFieldList = std::vector<HeaderField>; ///< 头部记录的有序容器类型

        /**
         * @brief 把头部名就地改写为小写
         * @details 归一化规则的唯一出处，toCanonicalHeaderName() 也复用它，
         *          避免写入侧与查询侧哪天被改成两套规则。
         * @param name 待改写的字符串，按 ASCII 表处理，不受 locale 影响
         */
        static void lowercaseInPlace(std::string &name);

        /**
         * @brief 把头部名归一化成内部存储形式（小写）
         * @param name 原始头部名
         * @return 归一化后的头部名
         */
        static std::string toCanonicalHeaderName(std::string_view name);

        /**
         * @brief 按需重建单值视图
         *
         * @details 单值视图（名 → 合并后的值）只在真正被查询时才建：绝大多数请求路径
         *          （路由、回显、静态文件之外的处理）从不读它，为它们维护一份哈希表
         *          等于每请求白付若干次节点分配。合并规则只有这一处实现：
         *          @li 普通头部同名多条 → 用 ", " 合并（RFC 7230 §3.2.2 的收件人规则）；
         *          @li 可重复头部（set-cookie）→ 保留首条，其余靠 headerValues() 逐条取。
         */
        void rebuildSingleValueView() const;

        /**
         * @brief 判断头部名是否允许在同一报文里出现多条
         * @param canonicalName 已归一化（小写）的头部名
         * @return true 表示该头部禁止合并，必须逐条保留
         */
        static bool isRepeatableHeaderName(std::string_view canonicalName);

        /**
         * @brief URL 百分号解码
         * @details 行为约定（宽松解码，永不报错）：
         *          @li "%XX"（XX 为两位十六进制，大小写均可）→ 对应字节；
         *              序列正好位于文本末尾（如 "?q=%41"）时照常解码，越界判据是
         *              「剩余长度不小于一个完整转义序列」而非「下标再加二仍小于总长」；
         *          @li '+' → 空格，这是表单编码的既有约定；
         *          @li 其余字符原样保留；
         *          @li 非法或残缺的百分号序列（"%"、"%4"、"%%"、"%4G"）→
         *              百分号及其后字符一律按原文保留，不吞字符也不报错。
         *          之所以不报错：查询串来自外部输入，解码失败不足以否定整个请求，
         *          把原文交给业务处理器判断比抛异常更符合中间件的容错预期。
         * @param source 待解码文本
         * @return 解码后的文本
         */
        static std::string percentDecode(std::string_view source);

        HttpMethod m_method{HttpMethod::UNKNOWN};                  ///< HTTP 方法
        std::string m_uri;                                         ///< 原始 URI，含查询串
        std::string m_httpVersion;                                 ///< HTTP 版本原文
        HeaderFieldList m_headerFields;                            ///< 头部权威记录，按线上到达顺序保存
        mutable std::unordered_map<std::string, std::string> m_headers; ///< 头部单值视图，供 headers()/getHeader() 使用：首次查询时才由权威记录建出
        mutable bool m_isSingleValueViewStale{true};               ///< 单值视图是否已过期（新增头部或重置后置位，查询前重建）
        std::string m_body;                                        ///< 消息正文
        std::unordered_map<std::string, std::string> m_params;     ///< 路由参数
        mutable std::stop_source m_cancelSource;                   ///< 协作式取消源：被触发过才在 reset() 里重建，未触发则跨请求沿用（省掉每请求一次分配）
    };
} // namespace AsynGyanis::Net
