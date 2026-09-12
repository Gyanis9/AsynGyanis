/**
 * @file HttpResponse.h
 * @brief HTTP 响应构建器与序列化器
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTP 响应类，用于构建并序列化 HTTP/1.1 响应消息
     *
     * @details 支持设置状态码、头部、正文，toString() 生成可直接写入 socket 的报文；
     *          另提供 ok()、notFound()、serverError() 等常用工厂方法。
     *          一个响应对象只服务一条请求：需要复用时先 reset()。
     *
     * @note 头部存储模型（本次重构修正）：
     *       @li 权威记录 m_headerFields —— 按「设置顺序」保存每一条头部，
     *           toString() 就按这个顺序逐条输出，因此报文头部顺序稳定可复现，
     *           多条 Set-Cookie 也保持先后次序（旧实现遍历 unordered_map，顺序跨次运行漂移）；
     *       @li 单值视图 m_headers —— 名到值的映射，每个名字恰有一条，可重复头部保留首次值，
     *           供既有的 headers() 接口使用；逐条取值请用 headerValues()；
     *       @li 头部名一律转小写存储（HTTP 头部名大小写不敏感，RFC 9110 §5.1），
     *           旧实现为容纳多条 Set-Cookie 而造的 "set-cookie_1" 伪键已彻底移除——
     *           那不是合法头部名，会被原样发到线上。
     * @warning 头部值会被原样写入报文，调用方不得传入含 CR/LF 的内容，否则构成响应拆分注入。
     *          正文与状态码由本类自行序列化，不受此限。
     */
    class HttpResponse
    {
    public:
        /**
         * @brief 构造一个默认响应（状态码 200 OK，无正文）。
         */
        HttpResponse();

        /**
         * @brief 设置 HTTP 状态码。
         * @param code 状态码，如 200、404、500
         * @note 本方法不校验取值范围；序列化时按十进制原样写出，
         *       非三位数状态码会产出对 RFC 9110 而言不合规、但多数实现仍能读的状态行
         */
        void setStatus(int code);

        /**
         * @brief 获取当前状态码。
         * @return 状态码
         */
        [[nodiscard]] int status() const;

        /**
         * @brief 设置一个 HTTP 头部字段。
         *
         * @details 头部名转小写后入库。两类语义：
         *          @li 普通头部：同名已存在则就地覆盖其值，条目位置保持在首次设置处，
         *              顺序不因改写而改变；
         *          @li 可重复头部（当前只有 set-cookie）：每次调用都新增一条独立头部，
         *              序列化时逐条输出 "Set-Cookie: ..."，先设先发。
         *          旧实现在第二条分支上写的是 set-cookie_1 这类带后缀的键名，属非法头部名，已修正。
         * @param name  头部字段名（如 "Content-Type"），大小写不敏感
         * @param value 头部字段值（如 "text/html"）
         * @return true 已写入
         * @return false 参数非法，响应未被改动
         * @note 值里出现 CR、LF 或 NUL 一律拒绝：头部以 CRLF 定界，放行就等于让调用方
         *       （常常是把用户输入写进 Location/X-Header 的业务代码）提前结束头部块，
         *       即 HTTP 响应拆分。非法头部名同理拒收。
         * @note 204 与 1xx 响应不应携带 content-length：本方法不会自动补，
         *       调用方显式设置的也不会在序列化时被抹掉，需要自行避免。
         */
        bool setHeader(const std::string &name, const std::string &value);

        /**
         * @brief 获取指定名称的 HTTP 头部值。
         * @param name 头部字段名，大小写不敏感
         * @return 命中时返回该名字的单值（可重复头部为首条）；未命中返回空 optional
         * @see headerValues() 需要逐条取值时使用
         */
        [[nodiscard]] std::optional<std::string> getHeader(const std::string &name) const;

        /**
         * @brief 获取指定名称的全部头部值，按设置顺序返回。
         * @details 主要给 Set-Cookie 这类可重复头部使用：setHeader 每调一次就多条一项。
         * @param name 头部字段名，大小写不敏感
         * @return 值列表；名字不存在时为空列表
         */
        [[nodiscard]] std::vector<std::string> headerValues(const std::string &name) const;

        /**
         * @brief 获取所有头部字段的单值视图。
         * @return 名到值的 unordered_map 引用，键为小写头部名；可重复头部在此只有首条值。
         *         序列化顺序不看这张表，一律按权威记录的插入顺序输出
         */
        [[nodiscard]] const std::unordered_map<std::string, std::string> &headers() const;

        /**
         * @brief 设置响应正文，覆盖已有内容。
         * @param body 正文字符串视图（内容会被复制存储）
         */
        void setBody(std::string_view body);

        /**
         * @brief 获取响应正文。
         * @return 正文字符串视图，视图生命周期跟随本响应对象
         */
        [[nodiscard]] std::string_view body() const;

        /**
         * @brief 设置 HTTP 协议版本（默认 "HTTP/1.1"），用于状态行序列化。
         * @param version 版本字符串，按原文写入状态行开头
         */
        void setHttpVersion(std::string version);

        /**
         * @brief 将响应序列化为 HTTP 格式的字符串。
         *
         * @details 输出结构：状态行 + 头部块 + 空白行 + 正文，行分隔符一律 CRLF。
         *          头部块按设置顺序逐条输出；随后按需补两条自动头部：
         *          @li 正文非空且未设置 content-type → 补 "content-type: text/plain"；
         *          @li 未设置 content-length → 按正文实际字节数补一条。
         *          补出的自动头部统一为小写名，排在调用方自设头部之后。
         * @return 完整的 HTTP 响应字符串
         * @note 返回串的长度即上线字节数，调用方直接整块发送即可
         */
        [[nodiscard]] std::string toString() const;

        /**
         * @brief 只序列化响应头部（状态行 + 头部块 + 空白行），不含正文
         *
         * @details 配合 body() 使用，可把「头部块 + 正文」作为两段交给聚合写一次提交，
         *          省掉把正文拼进头部块的那次整体拷贝（大正文与文件响应最明显）。
         *          补齐规则与 toString() 完全一致：正文非空且未设 content-type 时补
         *          text/plain，未设 content-length 时按正文实际字节数补一条（1xx/204 不补）。
         * @return 响应头部块字符串，长度不包含正文
         * @note 与 toString() 拼接后的结果逐字节相同：toString() 就是「本函数 + 正文」，
         *       差别只在于正文是否需要额外一份拷贝
         */
        [[nodiscard]] std::string serializeHead() const;

        /**
         * @brief 创建一个 200 OK 响应。
         * @param body 响应正文
         * @return HttpResponse 对象，已带 content-type: text/plain
         */
        static HttpResponse ok(std::string body);

        /**
         * @brief 创建一个 404 Not Found 响应。
         * @return HttpResponse 对象，正文为 "Not Found"
         */
        static HttpResponse notFound();

        /**
         * @brief 创建一个 500 Internal Server Error 响应。
         * @param message 错误描述正文，默认为空串
         * @return HttpResponse 对象，已带 content-type: text/plain
         */
        static HttpResponse serverError(std::string message = "");

        /**
         * @brief 重置响应对象到初始状态（状态码 200、版本 HTTP/1.1，清空头部和正文）。
         * @details 两条头部存储一起清空，保持「视图与权威记录一致」的不变式；
         *          复用响应对象时必须先调用本方法，否则上一轮的 Set-Cookie 会残留。
         */
        void reset();

    private:
        /**
         * @brief 单条头部字段的权威记录
         */
        struct HeaderField
        {
            std::string name;  ///< 已归一化为小写的头部名
            std::string value; ///< 头部值原文，序列化时逐字写出
        };

        using HeaderFieldList = std::vector<HeaderField>; ///< 头部记录的有序容器类型

        /**
         * @brief 根据状态码获取标准原因短语。
         * @param code 状态码
         * @return 原因短语字符串（如 "OK"、"Not Found"）；未收录的状态码返回空串，
         *         此时状态行原因为空（RFC 9110 允许）
         */
        static const char *statusMessage(int code);

        /**
         * @brief 把头部名就地改写为小写
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
         * @brief 判断头部名是否允许在同一报文里出现多条
         * @param canonicalName 已归一化（小写）的头部名
         * @return true 表示该头部逐条上线，不做覆盖
         */
        static bool isRepeatableHeaderName(std::string_view canonicalName);

        /**
         * @brief 在权威记录中按名字线性查找首个同名条目
         * @param canonicalName 已归一化（小写）的头部名
         * @return 指向首个同名条目的迭代器，未命中时等于 m_headerFields.end()
         */
        HeaderFieldList::iterator findHeaderField(const std::string &canonicalName);

        /**
         * @brief 该状态码的响应是否不允许携带正文（RFC 9110 §6.3：1xx、204、304）
         * @return true 表示序列化时不得输出正文
         */
        [[nodiscard]] bool carriesNoContent() const noexcept;

        /**
         * @brief 该状态码是否不得自动补 content-length
         * @details 与 carriesNoContent() 刻意不同：304 不允许带正文，但明确允许携带
         *          content-length（RFC 7230），因此只有 1xx 与 204 需要跳过自动补缺
         * @return true 表示不补 content-length
         */
        [[nodiscard]] bool mustNotDeclareContentLength() const noexcept;

        /**
         * @brief 计算头部块（状态行 + 头部 + 空白行）的预留长度，不含正文
         * @return std::size_t 预留字节数
         */
        [[nodiscard]] std::size_t headReserveLength() const;

        /**
         * @brief 把头部块追加到目标串
         * @param result 目标串（调用方已按 headReserveLength 预留容量）
         */
        void appendHead(std::string &result) const;

        int m_status{200};                                     ///< HTTP 状态码，默认 200
        std::string m_httpVersion{"HTTP/1.1"};                 ///< HTTP 版本，默认 1.1
        HeaderFieldList m_headerFields;                        ///< 头部权威记录，按设置顺序保存，决定序列化顺序
        std::unordered_map<std::string, std::string> m_headers; ///< 头部单值视图，供 headers()/getHeader() 使用
        std::string m_body;                                    ///< 响应正文
    };
} // namespace AsynGyanis::Net
