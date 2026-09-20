/**
 * @file HttpResponse.h
 * @brief HTTP 响应构建器与序列化器
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Platform/IO/MemoryMappedFile.h"

#include <cstddef>
#include <functional>
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
     * @note 头部存储模型：
     *       @li 权威记录按「设置顺序」保存每条头部，toString() 就按它逐条输出，
     *           因此报文头部顺序稳定可复现，多条 Set-Cookie 也保持先后次序；
     *       @li 单值视图是名到值的映射（可重复头部只留首条），供 headers()/getHeader()
     *           使用，逐条取值请用 headerValues()；**首次查询时才由权威记录建出**（写入只把视图标脏），
     *           HTTP/1.1 的序列化从不读它，因此那条路径不再为每个响应维护一张映射表；
     *           头部名一律转小写存储（RFC 9110 §5.1）。
     * @warning 头部值会被原样写入报文，调用方不得传入含 CR/LF 的内容，否则构成响应拆分注入。
     *          正文与状态码由本类自行序列化，不受此限。
     *
     * @note 流式模式（startChunkedResponse() + writeChunk()）与整块正文互斥：进入之后正文只能
     *       逐段写出，头部按 transfer-encoding: chunked 序列化且不写 content-length。
     *       头部不在 startChunkedResponse() 里发出（那是同步接口，发不出去字节），而是随首段正文
     *       一起上线，因此状态码与头部必须在首次 writeChunk() 之前定稿。
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
         * @details 头部名按小写形态入库。普通头部同名即就地覆盖值、条目位置不变；
         *          可重复头部（当前只有 set-cookie）每次调用新增一条独立头部，先设先发。
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
        bool setHeader(std::string_view name, std::string_view value);

        /**
         * @brief 获取指定名称的 HTTP 头部值。
         * @param name 头部字段名，大小写不敏感
         * @return 命中时返回该名字的单值（可重复头部为首条）；未命中返回空 optional
         * @see headerValues() 需要逐条取值时使用
         */
        [[nodiscard]] std::optional<std::string> getHeader(std::string_view name) const;

        /**
         * @brief 判断指定名称的头部取值里是否出现了某个逗号分隔的 token（RFC 9110 §5.6.1）
         * @details 例如 `Connection: keep-alive, Upgrade` 含 "upgrade" 而不含 "close"。
         *          判定在存储内部逐段完成，既不拷贝取值也不构造值列表；保活判定每条响应都要跑，
         *          而它只需要一个布尔结果。
         * @param name 头部字段名，大小写不敏感
         * @param expectedToken 待查找的 token，大小写不敏感
         * @return true 至少一条取值列出了该 token
         * @see headerValues() 需要逐条取值时使用
         */
        [[nodiscard]] bool hasHeaderValueToken(std::string_view name, std::string_view expectedToken) const;

        /**
         * @brief 获取指定名称的全部头部值，按设置顺序返回。
         * @details 主要给 Set-Cookie 这类可重复头部使用：setHeader 每调一次就多条一项。
         * @param name 头部字段名，大小写不敏感
         * @return 值列表；名字不存在时为空列表
         */
        [[nodiscard]] std::vector<std::string> headerValues(std::string_view name) const;

        /**
         * @brief 只发流式响应的头部、不发正文（HEAD 请求用）
         * @details HEAD 的响应「只有头部、没有正文」：对端在头部之后的第一个空行就认为消息结束
         *          （RFC 9112 §6.1）。分块帧与终止块都算正文，发出去会被对端当成下一条报文的开头，
         *          因此一并不发；头部照发（它描述的正是 GET 会返回什么）。会话在派发前按请求方法设置，
         *          重置（reset）时清除
         */
        void suppressStreamingBody() noexcept;

        /**
         * @brief 流式响应的正文是否被抑制（见 suppressStreamingBody）
         * @return true 只发头部、不发正文段
         */
        [[nodiscard]] bool isStreamingBodySuppressed() const noexcept;

        /**
         * @brief 获取所有头部字段的单值视图。
         * @return 名到值的 unordered_map 引用，键为小写头部名；可重复头部在此只有首条值。
         *         序列化顺序不看这张表，一律按权威记录的插入顺序输出
         */
        [[nodiscard]] const std::unordered_map<std::string, std::string> &headers() const;

        /**
         * @brief 设置响应正文，覆盖已有内容。
         * @param body 正文字符串视图（内容会被复制存储）
         * @throws Base::LogicException 响应已进入流式模式：整块正文与流式模式互斥，
         *         正文只能由 writeChunk() 逐段写出
         * @note 与 setMappedBody() 互斥：调用本函数会丢弃已映射的文件
         * @note 调用方已显式设过的 content-length 原样保留（HEAD 与静态文件服务靠「先声明
         *       长度、不读正文」省一次整文件 IO）。替换既有正文的中间件要自己清这条头，
         *       否则发出「头部说 5 字节、正文几千字节」的报文
         */
        void setBody(std::string_view body);

        /**
         * @brief 用「内存映射的文件」当正文：整份文件不复制进堆，直接以映射视图参与发送
         *
         * @details 静态文件响应的正文动辄几十 KiB 到几十 MiB，先读进堆再发等于白白多一次
         *          等量拷贝与分配。映射之后正文就是文件页的视图，发送时直接引用（本响应
         *          持有映射的所有权，因此映射在发送期间一定有效）。
         * @param mappedFile 已映射好的文件；无效对象会被当作空正文
         * @throws Base::LogicException 响应已进入流式模式：整块正文与流式模式互斥，
         *         正文只能由 writeChunk() 逐段写出
         * @note 与 setBody() 互斥：调用本函数会丢弃已存下的堆正文
         * @see Platform::MemoryMappedFile
         */
        void setMappedBody(Platform::MemoryMappedFile mappedFile);

        /**
         * @brief 用「内存映射文件里的一个区间」当正文，供 206 区间响应零拷贝引用
         *
         * @details 区间视图由 [offset, offset + length) 界定，body() 与序列化都只看到这一段，
         *          发送时仍指向文件页，因此 206 不必为切片额外拷一份堆内存。
         * @param mappedFile 已映射好的文件
         * @param offset 区间起始偏移，单位字节
         * @param length 区间长度，单位字节；0 表示空正文
         * @throws Base::InvalidArgumentException 区间超出映射范围（offset 或 length 越界）。
         *         越界属于调用方的用法错误，拒绝静默钳制——那会让 content-length 与实际
         *         字节数悄悄不一致
         * @throws Base::LogicException 响应已进入流式模式：整块正文与流式模式互斥，
         *         正文只能由 writeChunk() 逐段写出
         * @note 与 setBody() 互斥：调用本函数会丢弃已存下的堆正文
         * @see Platform::MemoryMappedFile
         */
        void setMappedBody(Platform::MemoryMappedFile mappedFile, std::size_t offset, std::size_t length);

        /**
         * @brief 获取响应正文。
         * @return 正文字符串视图，视图生命周期跟随本响应对象（映射正文时指向文件映射）
         */
        [[nodiscard]] std::string_view body() const;

        /**
         * @brief 释放映射正文（解除映射并关闭其持有的文件句柄），头部与状态码不受影响
         * @details 正文在响应发出之后就不再需要，而空闲的 keep-alive 连接会一直持有响应对象
         *          直到下一条报文派发时才复位：静态文件响应因此可能长时间占着映射与句柄。
         *          会话在发送完成后调用本方法提前归还，占用的文件资源只覆盖真正的发送期间。
         * @note 调用后 bodyView() 变为空视图；reset() 与 setBody() 也会做同样的释放
         */
        void releaseMappedBody() noexcept;

#if !ASYN_PLATFORM_WIN32
        /**
         * @brief 映射正文的零拷贝发送描述：文件描述符 + 文件内区间
         */
        struct ZeroCopyBody
        {
            int         fileDescriptor{-1}; ///< 承载正文的文件描述符；-1 表示本响应没有可零拷贝发送的正文
            std::size_t offset{0};          ///< 区间在文件中的起始偏移，单位字节
            std::size_t length{0};          ///< 区间字节数
        };

        /**
         * @brief 取映射正文的零拷贝发送描述（sendfile 用）
         * @return std::optional<ZeroCopyBody> 有映射正文且长度非零时给出描述；堆正文、空正文、
         *         流式响应都返回空，调用方据此退回普通发送路径
         * @note 只给描述、不转移所有权：文件描述符随本对象存活，调用方须在响应对象存活期间使用
         */
        [[nodiscard]] std::optional<ZeroCopyBody> zeroCopyBody() const noexcept;
#endif

        /**
         * @brief 流式发送回调：把一段字节写到本响应所属的连接
         *
         * @details 由会话在路由之前装配（HttpResponse 自己不认识 socket）。false = 本段未发出、连接不可
         *          再用、调用方应停止写入，两种来源：本侧已收口（此前必已记录过收口原因，本次不再记）与
         *          传输失败（对端关闭或复位、描述符被关闭、等可写期间被关闭，本次记一条中文日志）。
         *          失败一律折成 false 不抛异常；回调是协程，写不下时挂起等待可写而不是丢弃这段字节。
         */
        using ChunkSender = std::function<Core::Task<bool>(std::string_view)>;

        /**
         * @brief 装配流式发送回调（由会话在路由之前调用）
         * @param chunkSender 发送回调；传空 std::function 表示清除装配
         * @note 回调绑定的是**连接**而不是某一条报文，因此 reset() 不清理它；未装配时
         *       writeChunk() 会直接报错，绝不把正文段悄悄丢掉
         */
        void setChunkSender(ChunkSender chunkSender);

        /**
         * @brief 进入流式响应模式：正文此后只能由 writeChunk() 逐段写出
         *
         * @details 头部按 transfer-encoding: chunked 序列化（RFC 9112 §6），不写 content-length；
         *          先设下的 content-length 会被删掉（两者不得并存）。状态码与头部须在首次
         *          writeChunk() 之前定稿：头部随首段正文一起上线，上线之后改不了。
         * @param statusCode 响应状态码
         * @throws Base::LogicException 状态码不允许携带正文（1xx、204、304，RFC 9110 §6.3）、
         *         已经设过整块正文、或流式头部已经上线
         * @warning 分块传输是 HTTP/1.1 的机制：在 HTTP/1.0 请求的响应上使用会产出对端无法解析的报文。
         *          另按 RFC 9110 §9.3.2，HEAD 的响应不得带正文，其处理函数不应进入流式模式。
         * @see writeChunk()
         */
        void startChunkedResponse(int statusCode);

        /**
         * @brief 把一段正文按 chunked 帧立即写出
         *
         * @details 帧格式为 `<十六进制的长度>\r\n<数据>\r\n`（RFC 9112 §7.1），经会话装配的回调
         *          直接写连接，不在内存里攒整块正文；首段之前先把头部推出去。
         * @param data 本次写出的正文段，按「指针 + 长度」取，可以是任意字节
         * @return true 已写出（data 为空时同样为 true，表示无需写出）
         * @return false 本段未发出、连接不可再用、**调用方应停止继续写并收手**；来源有两种：
         *         **本侧已收口**（此前必已记录过收口原因，本次不新增日志）与**传输失败**（对端关闭、
         *         对端复位、描述符被关闭、等可写期间被关闭，会话本次记一条中文日志）。该失败不抛异常；
         *         「false 且日志里没有新记录」通常意味着本侧已收口，排查请回看收口那一刻的日志
         * @throws Base::LogicException 用法错误，仍抛异常：当前不在流式模式（请先调用
         *         startChunkedResponse()），或响应对象未装配发送回调（只对会话交给处理器的响应对象有效）
         * @note data 指向的字节必须活到本次 co_await 结束：协程到首次 resume 才读入参
         * @note data 为空时不发送并直接返回 true：零长度块是终止块的语义（RFC 9112 §7.1），
         *       发一个空段既无信息又会被对端当成消息结束
         * @note 每次写出之前会把连接的空闲截止时间按 HttpServerLimits::writeTimeout 刷新，
         *       因此相邻两段之间超过该时限仍会被清扫协程当成慢消费者收口：长间隔的流（如 SSE
         *       心跳）要在期限内补一段数据，或把 writeTimeout 配大、配 0（关闭该项保护）
         * @see startChunkedResponse()
         */
        Core::Task<bool> writeChunk(std::string_view data);

        /**
         * @brief 本响应是否处于流式模式。
         * @return true 已由 startChunkedResponse() 进入流式模式
         */
        [[nodiscard]] bool isChunkedResponse() const noexcept;

        /**
         * @brief 流式头部是否已随首段正文上线
         *
         * @details 会话据此判断收尾时该做什么：头部未上线还能整份重写（例如换成 500），
         *          已上线则只能补终止块。
         * @return true 状态行与头部已经在对端手里，此后只能写正文段与终止块
         */
        [[nodiscard]] bool hasSentChunkedHead() const noexcept;

        /**
         * @brief 登记「本次请求要把这条连接升级成 WebSocket」（RFC 6455 §4）
         *
         * @details 只登记意图，不做任何 IO：会话在此后的循环里校验升级请求、把 101 报文原样写出，
         *          然后把连接交给该处理器。处理器一直运行到它返回或连接收口，期间会话不再回到
         *          HTTP 事务循环——一条连接要么是 HTTP，要么是 WebSocket。
         * @param handler 升级成功后的业务处理器，所有权转移给本响应
         * @throws Base::InvalidArgumentException handler 为空
         * @throws Base::LogicException 已设过整块正文或已进入流式模式：升级应答由握手模块逐字节生成，
         *         与 HttpResponse 的正文/分块序列化互斥
         * @note 校验可能失败（请求并不构成合法握手），此时会话回 400 而不会调用处理器
         * @note reset() 会连同处理器一起清掉：复用的响应对象不会把升级意图带到下一条报文
         * @see isWebSocketUpgradeRequested(), webSocketHandler()
         */
        void upgradeToWebSocket(WebSocketHandler handler);

        /**
         * @brief 本次响应是否登记了 WebSocket 升级。
         * @return true 已由 upgradeToWebSocket() 登记，会话应尝试升级而不是按响应序列化应答
         */
        [[nodiscard]] bool isWebSocketUpgradeRequested() const noexcept;

        /**
         * @brief 取已登记的升级处理器。
         * @return 处理器的常引用；未登记时为空 std::function
         * @note 引用跟随本响应的生命周期；会话在升级成功后才取用它
         */
        [[nodiscard]] const WebSocketHandler &webSocketHandler() const noexcept;

        /**
         * @brief 设置 HTTP 协议版本（默认 "HTTP/1.1"），用于状态行序列化。
         * @param version 版本字符串，按原文写入状态行开头
         */
        void setHttpVersion(std::string version);

        /**
         * @brief 将响应序列化为 HTTP 格式的字符串。
         *
         * @details 输出结构：状态行 + 头部块 + 空白行 + 正文，行分隔符一律 CRLF；头部块按设置顺序输出，
         *          随后按需补三条自动头部：未设 date 时补一条当前时刻的 IMF-fixdate；正文非空且未设
         *          content-type 时补 text/plain；未设 content-length 时按正文实际字节数补一条。
         *          自动补出的头部为小写名，排在自设头部之后。
         * @return 完整的 HTTP 响应字符串
         * @note 返回串的长度即上线字节数，调用方直接整块发送即可
         */
        [[nodiscard]] std::string toString() const;

        /**
         * @brief 只序列化响应头部（状态行 + 头部块 + 空白行），不含正文
         *
         * @details 配合 body() 使用，可把「头部块 + 正文」作为两段交给聚合写一次提交，
         *          省掉把正文拼进头部块的那次整体拷贝（大正文与文件响应最明显）。
         *          补齐规则与 toString() 完全一致。
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
         * @details 两条头部存储一起清空，保持「视图与权威记录一致」的不变式；流式模式标记与
         *          WebSocket 升级意图一并复位（否则下一条报文会被按分块定界、或被当成升级请求）；
         *          发送回调刻意不清，它绑定连接而不是本条报文。复用响应对象时必须先调用本方法，
         *          否则上一轮的 Set-Cookie 会残留。
         */
        void reset();

        /**
         * @brief 该状态码的响应是否不允许携带正文（RFC 9110 §6.3：1xx、204、304）
         * @param statusCode 待判定的状态码
         * @return true 表示该状态码的响应不得有正文，因此不能进入流式模式
         */
        [[nodiscard]] static bool isBodylessStatusCode(int statusCode) noexcept;

        /**
         * @brief 该状态码的响应是否不允许携带正文（RFC 9110 §6.3：1xx、204、304）
         * @return true 表示序列化时不得输出正文
         */
        [[nodiscard]] bool carriesNoContent() const noexcept;

    private:
        using HeaderField = HttpHeaderFieldStore::HeaderField; ///< 头部记录（存储内部类型）

        /**
         * @brief 根据状态码获取标准原因短语。
         * @param code 状态码
         * @return 原因短语字符串（如 "OK"、"Not Found"）；未收录的状态码返回空串，
         *         此时状态行原因为空（RFC 9110 允许）
         */
        static const char *statusMessage(int code);

        /**
         * @brief 删除指定名字的全部条目
         * @param name 头部名，大小写不敏感；不存在时为空操作
         */
        void removeHeaderField(std::string_view name);

        /**
         * @brief 该状态码是否不得自动补 content-length
         * @details 与 carriesNoContent() 刻意不同：304 允许携带 content-length，但只允许取
         *          「同一请求的 200 会发出的正文长度」（RFC 9112 §6.2）——自动补出来的是 0，
         *          正好违反该条 MUST NOT。因此 1xx / 204 / 304 一律不补，要带就由知道真实长度的
         *          调用方显式设置
         * @return true 表示不补 content-length
         */
        [[nodiscard]] bool mustNotDeclareContentLength() const noexcept;

        /**
         * @brief 取当前正文的视图，与正文的来源（堆串或文件映射）无关
         * @return std::string_view 映射正文时指向文件页，堆正文时指向 m_body；无正文时为空视图
         * @note 序列化、补 content-length、对外 body() 都经此一处取值，
         *       只要两条正文存储的互斥不变式被 setBody/setMappedBody/reset 维持住，
         *       调用方就看不到区别
         */
        [[nodiscard]] std::string_view bodyView() const noexcept;

        /**
         * @brief 取自动补出的 date 头部值，首次调用时按当前时刻生成并缓存
         * @details 缓存保证同一响应的多次序列化给出逐字一致的 date：serializeHead() 与 toString()
         *          若各自取一次 now()，跨秒的两次调用会产出不同的头部字节。文本本身取自
         *          `currentHttpDateText()` 的按秒缓存，这一层保证的是「同一响应内不变」。
         * @return 自动生成的 IMF-fixdate 文本
         */
        [[nodiscard]] std::string_view autoDateText() const;

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
        HttpHeaderFieldStore m_headerStore; ///< 头部存储：权威记录 + 按需重建的单值视图（见该类注释）
        bool m_isStreamingBodySuppressed{false};               ///< HEAD 请求：流式响应只发头部、不发正文段
        std::string m_body;                                    ///< 响应正文（堆存储），与 m_mappedBody 互斥
        Platform::MemoryMappedFile m_mappedBody;               ///< 响应正文（文件映射），持有映射所有权，保证发送期间映射有效
        std::size_t m_mappedBodyOffset{0};                     ///< 映射正文的起始偏移，单位为字节（整份文件时为 0）
        std::size_t m_mappedBodyLength{0};                     ///< 映射正文的长度，单位为字节（决定 bodyView 与 content-length）
        bool m_isChunked{false};                               ///< 是否处于流式响应模式：正文由 writeChunk 逐段写出，头部按 chunked 序列化
        bool m_hasSentChunkedHead{false};                      ///< 流式头部是否已随首段正文上线；上线之后状态码与头部都改不了
        ChunkSender m_chunkSender;                             ///< 流式发送回调，由会话装配；空表示这条响应没有可写的连接
        WebSocketHandler m_webSocketHandler;                   ///< 升级成功后的业务处理器；空表示本次没有登记升级
        bool m_isWebSocketUpgradeRequested{false};              ///< 是否登记了 WebSocket 升级；会话据此走升级分支而不是序列化应答
        mutable std::string m_autoDateValue;                   ///< 自动补出的 date 值，首次序列化时生成并缓存；空串表示尚未生成
    };
} // namespace AsynGyanis::Net
