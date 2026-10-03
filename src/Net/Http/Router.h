/**
 * @file Router.h
 * @brief URL 路由器，按 HTTP 方法与路径模式分发请求并串联中间件
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Middleware.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    // 下面两块用 /// 而不是 /** */：块注释里出现 "/static/*" 会被 Doxygen 当成嵌套注释的开头，
    // 那个 */ 被它当成闭合符吃掉之后，本文件后面的文档就不抽取了
    /// @brief 通配路由捕获到的剩余路径存放在哪个路由参数里
    ///
    /// @details 注册 "/static/*" 时，"/static/css/main.css" 捕获到的 "css/main.css"
    ///          会以本常量作为参数名写入 HttpRequest，handler 用 `request.param(kWildcardParameterName)` 取。
    ///          之所以用 "*" 这个名字：它与模式里写下的通配符字面一致，读者在 handler 里一眼能对上，
    ///          而且路由参数名允许出现 ":id" 这类模板写法取不到的字符，不会与具名参数撞车。
    inline constexpr std::string_view kWildcardParameterName = "*";

    /// @brief HTTP 路由器，按方法与路径模式匹配请求并执行对应处理函数，同时串起中间件管道。
    ///
    /// @details 匹配分两级，先精确后模式：
    ///          @li 一级：精确路径哈希索引（不含 ':' 与 '*' 的模式都进这张表），一次哈希 + 极短的候选扫描；
    ///          @li 二级：参数化（"/user/:id"）与通配（"/static/*"、"*"）路由按注册顺序线性扫描。
    ///
    /// @note **优先级规则（本类唯一的仲裁口径，注册侧请按此规划）**
    ///       @li 登记了虚拟主机时，**先按 Host 选表**，下面这些路径优先级只在选中的那一张表里成立；
    ///           选中的主机没有这条路径就是 404，不会回落去问根表（见 virtualHost()）；
    ///       @li 精确路径永远优先于模式路径，与注册先后无关；
    ///       @li 同一层内（同一条精确路径的多个方法绑定之间、模式路由彼此之间）**先注册者优先**，
    ///           指定方法条目与 any() 条目之间也只看注册先后，不做「谁更具体谁优先」的特殊仲裁；
    ///       @li 以完全相同的 (方法集合, 路径) 再次注册会**就地替换**处理函数且不改变注册位置，
    ///           因此重复调用是幂等的（HttpServer::staticFileDir() 就依赖这一点）。
    ///
    /// @note **方法与 405/404 的判定**
    ///       @li 请求方法命中某条路由且该路由允许此方法 → 执行；
    ///       @li HEAD 复用 GET（RFC 9110 §9.1）：显式 head() 与 any() 都没命中时，按 GET 的匹配
    ///           路径（精确表与模式路由，优先级与 GET 完全一致）执行它的处理器。因此「只注册了
    ///           get()」的路径收到 HEAD 会得到 200 而不是 405；
    ///       @li 路径命中但方法不被任何候选路由允许 → 405，并给出 Allow 头：列出显式注册的方法，
    ///           并在 GET 被允许时补上隐含可用的 HEAD（RFC 9110 §9.1 的 GET/HEAD 复用使资源实际
    ///           支持 HEAD（§9.3.2），§15.5.6 要求 Allow 如实交代支持集合）；
    ///       @li 路径根本没有命中 → 404；
    ///       @li HttpMethod::UNKNOWN（CONNECT、TRACE、M-SEARCH 等未收录方法）**不参与业务匹配**，
    ///           连 any() 注册的通配方法路由也不会放行它，只按上面两条产出 404/405。
    ///           把它兼任「通配方法」就等于让任何未收录方法蹭上兜底路由，405 防线随之失效。
    ///
    /// @see any(), MiddlewarePipeline
    class ASYN_NET_API Router
    {
    public:
        /**
         * @brief 路由处理函数类型。
         */
        using Handler = std::function<Core::Task<>(HttpRequest &, HttpResponse &)>;

        /**
         * @brief 构造一个空路由器：没有路由、没有中间件。
         */
        Router() = default;

        // 共享形态（dllexport）会把隐式特殊成员全部实例化一遍，而 m_virtualHosts 是
        // unordered_map<string, unique_ptr<Router>>：它的拷贝赋值在实例化期就编不过（MSVC C2679），
        // 静态形态下没人用拷贝所以从未暴露。显式声明拷贝为删除、移动保持可用，两形态行为一致。
        Router(const Router &) = delete;

        Router &operator=(const Router &) = delete;

        Router(Router &&) = default;

        Router &operator=(Router &&) = default;

        /**
         * @brief 注册 GET 路由。
         * @param path 路径模式：字面路径、":name" 参数段，或以 '*' 结尾的前缀通配
         * @param handler 处理函数，所有权转移给路由器
         * @note 同 (方法, 路径) 重复注册为就地替换；与既有路由的先后关系见类注释的优先级规则
         * @note 该路径上的 HEAD 请求在没有显式 head() 注册时由本处理函数应答（RFC 9110 §9.1）
         */
        void get(const std::string &path, Handler handler);

        /**
         * @brief 注册 POST 路由。
         * @param path 路径模式
         * @param handler 处理函数，所有权转移给路由器
         */
        void post(const std::string &path, Handler handler);

        /**
         * @brief 注册 PUT 路由。
         * @param path 路径模式
         * @param handler 处理函数，所有权转移给路由器
         */
        void put(const std::string &path, Handler handler);

        /**
         * @brief 注册「流式正文」POST 路由：头部收齐即派发，正文经 HttpRequest::bodyStream() 边收边读
         *
         * @details 与 post() 的处理器类型相同，区别只在**派发时机**与正文交付方式：
         *          @li 普通路由等整条请求（含正文）收齐后才派发，正文经 body() 一次性读取；
         *          @li 流式路由在头部收齐、正文仍在收取时就派发，处理器用
         *              `co_await request.bodyStream()->readNext()` 按到达批次取正文——
         *              不调用时连接不再读入，慢业务因此天然形成背压，大上传也不会在内存里
         *              整块驻留（缓冲上界是一批网络字节，而非正文全长）。
         *
         * @param path 路径模式
         * @param handler 处理函数；读正文请用 `request.bodyStream()`，不要依赖 body()
         * @note 追加语义：处理器返回时若正文仍未读完，会话会把剩余字节排空后复用连接；
         *       排空失败或正文中途解析出错则收口连接（不按可复用处理）
         * @note 与 WebSocket 升级不兼容：流式路由里登记升级会被按 500 拒绝
         * @note 中间件对两类路由都生效；横切逻辑若需要看正文，请同样经 bodyStream() 读
         * @see HttpRequestBody, HttpRequest::bodyStream()
         */
        void postStreaming(const std::string &path, Handler handler);

        /**
         * @brief 注册「流式正文」PUT 路由，语义同 postStreaming()
         * @param path 路径模式
         * @param handler 处理函数
         * @see postStreaming()
         */
        void putStreaming(const std::string &path, Handler handler);

        /**
         * @brief 注册 DELETE 路由。
         * @param path 路径模式
         * @param handler 处理函数，所有权转移给路由器
         * @note 方法名为 del() 而非 delete()：后者是 C++ 关键字，无法用作成员函数名
         */
        void del(const std::string &path, Handler handler);

        /**
         * @brief 注册 PATCH 路由。
         * @param path 路径模式
         * @param handler 处理函数，所有权转移给路由器
         */
        void patch(const std::string &path, Handler handler);

        /**
         * @brief 注册 HEAD 路由。
         * @param path 路径模式
         * @param handler 处理函数，所有权转移给路由器
         * @note 显式注册的 HEAD 路由优先于 GET：同一条路径上既注册了 head() 也有 get() 时，
         *       按优先级规则先命中的那条先被选中，注册了 head() 就一定不会走到 GET 复用上
         */
        void head(const std::string &path, Handler handler);

        /**
         * @brief 注册 OPTIONS 路由（用于非跨域的方法探测）。
         * @param path 路径模式
         * @param handler 处理函数，所有权转移给路由器
         * @note CORS 预检请求由 corsMiddleware 直接应答，不会走到这里；注册了本路由也不代表能收到预检
         */
        void options(const std::string &path, Handler handler);

        /**
         * @brief 注册「任意已知方法」都能命中的路由。
         * @param path 路径模式，字面路径与模式路径皆可（"*" 即全捕获兜底路由的常用写法）
         * @param handler 处理函数，所有权转移给路由器
         *
         * @details 显式的通配方法入口：放行的方法集合是 HttpMethod 里除 UNKNOWN 之外的全部枚举值；
         *          UNKNOWN（CONNECT/TRACE 等未收录方法）一律不放行。
         * @note 与专属方法路由之间没有「谁更具体」的仲裁：同一层内先注册者优先。
         *       希望 "/api/:id" 这类专属路由压过 any("/api/:id")，就先把专属路由注册出去。
         */
        void any(const std::string &path, Handler handler);

        /**
         * @brief 取（没有则建）一个按 Host 选中的虚拟主机路由表
         *
         * @details 名字里带 `*.` 的是通配站点：`*.example.com` 收 `a.example.com` 与 `a.b.example.com`，
         *          但**不收** `example.com` 本身。选站仲裁只有一条规则——精确名优先于通配名，
         *          通配名之间取后缀最长者（`*.co.example.com` 赢过 `*.example.com`），与注册先后无关。
         *          比对键的归一化见 `normalizeHostComparisonKey()`：去端口、折小写、去结尾的根点，
         *          因此 `"API.Example.COM:443"`、`"api.example.com."` 与 `"api.example.com"` 是同一个站点。
         *
         * @param hostName 主机名，可带 `*.` 前缀；大小写与端口不参与比对
         * @return 该主机的路由表引用，在它上面照常 get()/post()/addMiddleware()
         * @throws Base::InvalidArgumentException 名字为空、只有 `*.`，或在**虚拟主机表上**再登记虚拟主机
         *         （选站只做一层：多层语义要扯清「谁先套谁」，那是能被规则本身回答的问题，不该留给你猜）
         * @note 命中的主机**只**在本表里找路由：根表上的路由（含 `staticFileDir()` 挂的那条 `any("*")`）
         *       不参与，也不与主机表的路由比先后。要让某个主机有静态目录，就在它自己上面注册
         * @note 根路由的中间件对所有主机都生效（它在最外层），各主机自己的中间件只对本主机生效（在内层）。
         *       鉴权、CORS、访问日志这类横切逻辑因此不会因为加了个主机而被绕过
         * @note 没带 Host（或 Host 归一化后为空）的请求一律走根表：h1 的 HTTP/1.0 式请求与直连 IP 的
         *       探活请求都属此类，把它们判 404 会让健康检查先炸
         * @see hasVirtualHosts(), route()
         */
        Router &virtualHost(const std::string &hostName);

        /**
         * @brief 本表是否登记过虚拟主机
         * @return true 至少有一个虚拟主机
         * @note 会话与路由内部用它决定「要不要为本次请求算一次主机键」：没登记虚拟主机时，
         *       单站点服务一条请求都不多付那次字符串归一化
         */
        [[nodiscard]] bool hasVirtualHosts() const noexcept;

        /**
         * @brief 添加全局中间件，将应用于所有路由。
         * @param middleware 中间件函数，所有权转移给路由器
         */
        void addMiddleware(MiddlewareFunc middleware);

        /**
         * @brief 路由入口：匹配方法与路径，跑完中间件管道并填充响应。
         * @param request  HTTP 请求对象；命中模式路由时其路由参数会被本函数填充
         * @param response HTTP 响应对象；命中时由中间件与处理函数填充，未命中时被重置后填 404/405
         * @return Core::Task<> 协程任务，处理链结束后返回
         *
         * @details 本函数保证「一定写完响应」：要么由业务 handler 写，要么由这里写 404/405，
         *          因此调用方（会话循环）不需要再判断响应是否被填过。HEAD 请求在显式 head()、
         *          any() 与 GET 都没命中时会按 GET 再匹配一遍（RFC 9110 §9.1）。
         * @details 登记过虚拟主机时，本函数先按请求的 Host 选站（见 virtualHost()）：选中主机的请求
         *          在「根中间件 → 该主机中间件 → 该主机路由」这条链上跑，未选中的请求照旧只走根表。
         * @note 未命中且没走虚拟主机时会先 reset()：前面中间件已经落下的头部不会残留到错误响应里。
         *       命中虚拟主机时这一步跳过——外层根中间件此刻已经写好了 CORS/日志头部，里层无权清场；
         *       「上一条报文的残留」由会话在每轮派发前的一次复位保证
         * @note HEAD 响应的正文不在本函数里剥：头部必须按完整正文序列化才能与 GET 逐字节一致，
         *       剥正文由会话的发送路径负责
         */
        Core::Task<> route(HttpRequest &request, HttpResponse &response);

        /**
         * @brief 查询「按给定方法、URI 与 Host 命中的路由是不是流式注册的」
         *
         * @details 供会话在头部收齐、正文未收完时判定派发时机：命中的路由是流式注册的，
         *          就提前派发（正文经 HttpRequestBody 边收边读）；否则等整条请求收齐再走
         *          route()。匹配规则与 route() 完全一致（先选站、精确优先、先到先得、UNKNOWN 不放行），
         *          只是不执行任何处理函数、也不产出 404/405。
         *
         * @param method 请求方法
         * @param uri 请求 URI 原文（本函数内部按 route() 同一口径截取路径部分）
         * @param authority 请求的 Host 原文（h1 的 host 头部、h2/h3 的 :authority 补齐值），
         *                  没有主机信息时传空视图
         * @return true 命中且该条路由为流式注册；路径未命中或命中的是普通路由时为 false
         * @note 主机参数不能省：某主机注册了流式路由而根表同路径是普通路由时，只查根表会把请求
         *       按普通路径派发——正文被整份缓冲后流就交不出字节了，那是一条看着像「业务卡住」的错
         * @note 仅 POST/PUT 可能返回 true：流式注册只经 postStreaming()/putStreaming() 两个方法绑定的
         *       入口产生，其余方法（含 GET/HEAD）在方法层即不可能命中流式路由，直接返回 false 不扫表
         * @see postStreaming(), putStreaming(), virtualHost()
         */
        [[nodiscard]] bool hasStreamingRoute(HttpMethod method, std::string_view uri, std::string_view authority) const;

    private:
        /// 路由参数的临时收集容器：整条路由命中后才一次性提交给请求
        using PathParameters = std::unordered_map<std::string, std::string>;

        /**
         * @brief 模式路由条目（参数化与通配路径），并缓存预解析结果
         */
        struct PatternRoute
        {
            /**
             * @brief 构造一条模式路由并预解析路径
             * @param routeMethod 绑定的 HTTP 方法，仅当 isAnyMethod 为 false 时有意义
             * @param matchAnyMethod 是否为 any() 注册的「任意方法」路由
             * @param routePattern 注册时给出的路径模式原文，所有权转移给本条目
             * @param routeHandler 处理函数，所有权转移给本条目
             * @param isStreaming 本条路由是否按流式接口注册，存进 streaming 供流式判定读取
             */
            PatternRoute(HttpMethod routeMethod, bool matchAnyMethod, std::string routePattern, Handler routeHandler, bool isStreaming = false);

            /**
             * @brief 把路径模式拆成段：普通段原样、":name" 段以 ':' 前缀保留，通配形态记入 isWildcard 与 wildcardPrefix
             * @param routePattern 路径模式原文
             */
            void precomputeSegments(const std::string &routePattern);

            HttpMethod               method{HttpMethod::GET}; ///< 绑定的方法；isAnyMethod 为 true 时该字段不参与判定
            bool                     isAnyMethod{false};      ///< 是否为任意方法路由（显式通配，与 UNKNOWN 无关）
            std::string              pattern;                 ///< 模式原文，用于替换判等与诊断输出
            std::vector<std::string> segments;                ///< 预解析的逐段模式，已去掉分隔用的 '/'
            std::string              wildcardPrefix;          ///< 通配路由的目录前缀（含结尾 '/'），非通配路由为空
            bool                     isWildcard{false};       ///< 是否以 '*' 结尾的前缀通配路由
            bool                     streaming{false};        ///< 是否流式正文路由（见 postStreaming()）
            Handler                  handler;                 ///< 业务处理函数，路由期间只按引用使用，不拷贝
        };

        /**
         * @brief 精确路径上的一条绑定：方法（或任意方法）到处理函数
         */
        struct ExactRoute
        {
            HttpMethod method{HttpMethod::GET}; ///< 绑定的方法；isAnyMethod 为 true 时不参与判定
            bool       isAnyMethod{false};      ///< 是否为 any() 注册的任意方法条目
            bool       streaming{false};        ///< 是否流式正文路由（见 postStreaming()）
            Handler    handler;                 ///< 业务处理函数
        };

        /**
         * @brief 判断一条模式路由的路径是否命中请求路径
         * @param route 待判定的模式路由
         * @param requestPath 请求路径（未解码的原文）
         * @param collectedParameters 收参数的落点：匹配到的 ":name" 与通配剩余路径，仅在本函数返回
         *        true 时应被采信；传 nullptr 表示只要命中判定、不收集（流式路由探测那条路），
         *        免去为通配剩余路径与每个 ":name" 段各造一份 std::string，也免去为「反正要丢掉」
         *        的调用方构造一张空哈希表（MSVC 上那张空表就要两次堆分配）
         * @return true 路径命中
         * @return false 路径不命中（此时 collectedParameters 里的残留应被丢弃）
         */
        static bool matchesPattern(const PatternRoute &route, std::string_view requestPath, PathParameters *collectedParameters);

        /**
         * @brief 判断路径是否为精确路径（不含 ":param" 与 '*' 模式字符）
         * @param path 路径模式
         * @return true 可进精确索引
         * @return false 需进模式列表
         */
        static bool isLiteralPath(std::string_view path);

        /**
         * @brief 把枚举方法名转成 Allow 头里的写法（ASCII）
         * @param method HTTP 方法
         * @return 指向静态字符串的视图；UNKNOWN 返回空视图（不参与 Allow 列表）
         */
        static std::string_view methodName(HttpMethod method);

        /**
         * @brief 把路由注册到对应索引：同 (方法集合, 路径) 就地替换，否则按注册顺序追加
         * @param method 绑定的方法
         * @param isAnyMethod 是否为任意方法路由
         * @param path 路径模式
         * @param handler 处理函数
         * @param streaming 是否流式正文路由（见 postStreaming()）
         */
        void addRoute(HttpMethod method, bool isAnyMethod, const std::string &path, Handler handler, bool streaming = false);

        /**
         * @brief 按与方法匹配同一套规则，判断命中的路由是否为流式注册
         * @param matchMethod 参与匹配的方法（HEAD 复用 GET 时调用方传 GET）
         * @param requestPath 请求路径（已截去查询串）
         * @return true 命中且该条路由为流式注册
         */
        [[nodiscard]] bool matchedRouteIsStreaming(HttpMethod matchMethod, std::string_view requestPath) const;

        /**
         * @brief 一次性提交匹配到的路由参数（先到先得规则在这里落地）
         * @param request 待写入的请求对象
         * @param collectedParameters 本条路由攒下的参数
         */
        static void commitPathParameters(HttpRequest &request, const PathParameters &collectedParameters);

        /**
         * @brief 未命中时写入 404/405 响应
         * @param request 请求对象，用于取协议版本与方法
         * @param response 响应对象，进入本函数即被重置
         * @param isMethodNotAllowed true 表示路径命中但方法不允许，回 405 并带 Allow
         * @param allowedMethods 405 时填入 Allow 头的方法集合文本
         */
        static void writeNotFoundOrNotAllowed(const HttpRequest &request, HttpResponse &response, bool isMethodNotAllowed, const std::string &allowedMethods);

        /**
         * @brief 路由后的响应收尾：按 HTTP 语义对 204/304 清空正文
         * @param response 响应对象；204 与 304 的正文在此被清空
         * @note HEAD 的正文刻意保留：会话要按完整正文序列化头部才能拿到与 GET 一致的一份头部，
         *       剥正文由发送路径负责（见 Detail::httpKeepAliveLoop）
         */
        static void finalizeResponse(HttpResponse &response);

        /**
         * @brief 把「已经在主机的表上完成匹配」的终点包进该主机自己的中间件管道
         *
         * @details 命中虚拟主机时派发要跑两层：根表的中间件在外（横切逻辑对所有站点一视同仁），
         *          主机的中间件在内。匹配本身在 route() 里就按主机的表做完了，这里只多套一层管道，
         *          因此这条额外的协程只在真用了虚拟主机时才有——单站点的派发形状与开销一字未改。
         *
         * @param hostTable 被选中的主机表
         * @param request 本次请求
         * @param response 本次响应
         * @param terminalHandler 最里层的终点（命中的处理函数，或 404/405 的写入）
         * @return TerminalHandler 交回给根管道的那个终末回调
         */
        [[nodiscard]] static TerminalHandler wrapWithHostPipeline(Router *hostTable, HttpRequest &request, HttpResponse &response, const TerminalHandler &terminalHandler);

        /**
         * @brief 按请求的 Host 选出该由哪张表服务
         * @param request 待派发的请求（读它的 host 头部，零拷贝）
         * @return 命中的主机表指针；本表没登记虚拟主机、或该请求不落在任何主机上时为空指针（交回根表）
         */
        Router *selectVirtualHostTable(const HttpRequest &request);

        /**
         * @brief 在已归一化的主机键里选一张表
         * @details const 与非 const 两个版本共用同一份判定（见 Router.cpp 的 selectVirtualHostRow）：
         *          派发与「流式派发判定」两条路必须选中同一张表，两份实现迟早会漂
         * @param hostKey 归一化后的主机名（见 normalizeHostComparisonKey()）
         * @return 命中的主机表指针；无匹配时为空指针
         */
        Router *findHostTable(const std::string &hostKey);

        /// 同上，供只读路径（流式派发判定）使用
        const Router *findHostTable(const std::string &hostKey) const;

        /// 一级索引：字面路径 → 该路径上的方法绑定候选（通常 1~2 条，先到先得）
        /**
         * @brief 字符串视图的透明哈希，让「按路径查精确路由」不必先把视图变成 std::string
         *
         * @details 请求路径以视图形式在路由内部流转（request.path() 返回视图），而 unordered_map
         *          默认的哈希只认 key_type：没有 is_transparent，find(string_view) 会先构造一个
         *          临时 std::string，等于把省下的那次路径串拷贝又原样还回去。等值比较也要换成
         *          透明的 std::equal_to<>，两者缺一，异构查找就不成立。
         */
        struct TransparentStringHash
        {
            using is_transparent = void; ///< 开启 unordered_map 的异构查找

            /**
             * @brief 计算视图的哈希
             * @param text 待哈希的视图
             * @return std::size_t 哈希值（对同一文本与 std::hash<std::string> 一致）
             */
            [[nodiscard]] std::size_t operator()(const std::string_view text) const noexcept
            {
                return std::hash<std::string_view>{}(text);
            }
        };

        /// 精确路由表：路径 → 该路径上的全部候选（哈希与相等比较都支持 string_view）
        using ExactRouteTable = std::unordered_map<std::string, std::vector<ExactRoute>, TransparentStringHash, std::equal_to<>>;

        ExactRouteTable m_exactRoutes; ///< 精确路由表（异构查找免去每请求一次路径串拷贝）

        /// 二级索引：参数化/通配路由列表，按注册顺序线性扫描，先注册者优先
        std::vector<PatternRoute> m_patternRoutes;

        MiddlewarePipeline m_pipeline; ///< 全局中间件管道

        /// 虚拟主机表：归一化主机名 → 该主机的路由表（键里带 `*.` 的是通配站点）
        std::unordered_map<std::string, std::unique_ptr<Router>> m_virtualHosts;

        /// 通配站点计数：为 0 时选站只查精确键，不必把整张表扫一遍
        std::size_t m_wildcardVirtualHostCount{0};

        /// 本表是不是一张虚拟主机表（由 virtualHost() 造出来的那批）：它自己不再选站
        bool m_isVirtualHostTable{false};
    };
} // namespace AsynGyanis::Net
