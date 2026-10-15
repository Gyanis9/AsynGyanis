/**
 * @file RedisConnection.h
 * @brief Redis 键值存储连接实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/DatabaseConnection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// hiredis 的 redisContext 与 redisReply 都是全局作用域的 C 结构体，前置声明集中写在本头的全局作用域：
// 只有 .cpp 才包含 <hiredis/hiredis.h>，避免第三方 C 头顺着包含链传染给所有使用方。
// RedisResult.h 通过包含本头复用这两行声明，不得重复声明。
struct redisContext;
struct redisReply;

namespace AsynGyanis::Core
{
    class TlsContext;
}

namespace AsynGyanis::Database
{
    /**
     * @brief 服务端主动推来的一条订阅回复
     *
     * @details Redis 把「订阅确认」与「消息」都写成三或四个元素的数组，靠第一个元素区分：
     *          message / pmessage / subscribe / unsubscribe / psubscribe / punsubscribe
     *          （RESP3 的 smessage/ssubscribe 同理，本结构按形状收下）。
     *          认不出的 kind 原样留在 kind 里、不猜也不丢：服务端将来加新形态时，调用方仍然看得见，
     *          比在这里静默过滤掉要好排查。
     */
    struct ASYN_DATABASE_API RedisPushReply
    {
        std::string  kind;                 ///< 回复类型：message / pmessage / subscribe / unsubscribe ...
        std::string  channel;              ///< 消息所在频道；pmessage 时是实际命中的那个频道
        std::string  pattern;              ///< 仅 pmessage 有值：命中的模式
        std::string  payload;              ///< 消息正文；订阅类回复没有这一段，留空
        std::int64_t subscriptionCount{0}; ///< 订阅类回复里的当前订阅数；消息类为 0
    };

    /**
     * @brief 一条键空间通知（keyspace notifications）拆出来的两半
     *
     * @details 服务端有两种频道形状，两半内容正好互换，只按频道名分不出来：
     *          @li `__keyspace@<库>__:<键>` —— 正文是事件名；
     *          @li `__keyevent@<库>__:<事件>` —— 正文是被改动的键。
     *          本结构按「键 / 事件」两个语义字段给出，调用方不必自己记哪种形状。
     */
    struct ASYN_DATABASE_API RedisKeyspaceNotification
    {
        std::int64_t database{0};       ///< 通知来自哪个键空间
        bool         isKeyEvent{false}; ///< true 表示走的是 __keyevent__（正文是键），false 是 __keyspace__（正文是事件）
        std::string  key;               ///< 被改动的键
        std::string  event;             ///< 事件名（set / del / expired / evicted ...）
    };

    /**
     * @brief 一个键的存活状态（`PTTL` 的三档答案，见 RedisConnection::keyLifetime）
     *
     * @details 服务端在这里回的是**一个整数三种含义**：`-1` 是「键在，但没设过期」，`-2` 是
     *          「键不存在」，其余是剩余毫秒。把两个负数折成同一个「没有值」就丢掉了排查现场最需要的
     *          那一条信息（键到底还在不在），而折成一个布尔又分不出「永不过期」与「已经没了」。
     *          因此这里按三档如实建模，调用方不必再记那两个哨兵值。
     * @note 「问不成」（未连接、服务端报错）不由本结构表达——那条走返回值的空值与 lastError()，
     *       否则「键不存在」与「链路坏了」会共用同一个形状。
     */
    struct ASYN_DATABASE_API RedisKeyLifetime
    {
        bool                                     keyExists{false}; ///< 键是否存在（服务端的 -2 即「不存在」）
        std::optional<std::chrono::milliseconds> remainingTime{};  ///< 剩余时长；键在而没设过期时为空

        /**
         * @brief 这个键会不会自己消失
         * @return true 设了过期且键还在；false 键不存在，或键在但永不过期
         */
        [[nodiscard]] bool hasExpiry() const noexcept
        {
            return keyExists && remainingTime.has_value();
        }
    };

    /**
     * @brief 一页 `SCAN` 的结果（游标 + 这一页扫到的键）
     *
     * @details SCAN 的契约是「游标续扫」：服务端回一份两元素数组，第一个是**下一页**的游标
     *          （回到 "0" 表示扫完），第二个是这一页的键。游标是**字符串**而不是整数——服务端可能
     *          编码自己的迭代状态，把它当数字解析会静默截断，因此这里原样透传。
     * @note 一次调用只发一条命令、只走一页：把「循环到扫完」也做进本方法就等于替调用方决定要不要
     *       防一次无界遍历（大键空间上那会让一条连接被占住 arbitrarily 久），因此这里只交一页。
     *       游标可能是**重复**或**空**的——SCAN 只保证「游标为 0 之前每个至少存在过的键会被给出一次」，
     *       键在扫描期间被删除或新建都不保证；这是协议的限制，不是本类的缺陷（见 Redis 的 SCAN 文档）。
     */
    struct ASYN_DATABASE_API RedisScanPage
    {
        std::string              nextCursor; ///< 下一页游标；回到 "0" 表示这一轮扫完了
        std::vector<std::string> keys;       ///< 这一页的键名（按字节原样交出，可能为空）

        /**
         * @brief 这一轮迭代是否已经回到起点
         * @return true 服务端给出的游标是 "0"；false 还要拿 nextCursor 继续扫
         */
        [[nodiscard]] bool isFinished() const noexcept
        {
            return nextCursor == "0";
        }
    };

    /**
     * @brief Redis 键值存储连接
     *
     * @details 可选编译：未取得 hiredis 时编译为报错桩（各执行入口把「当前构建未编译 Redis 驱动」写入
     *          lastError()）。命令一律走 hiredis 的 argv 接口，参数按「指针 + 长度」传递，因此 '%' 不是
     *          格式串、内嵌 '\0' 不被截断；execute() 的整行命令按 redis-cli 规则切词后送出。queryTimeout()
     *          在建连后由 redisSetTimeout 应用到上下文，因此 AUTH 与初始 SELECT 也在读写超时保护内；
     *          连接存续期间改这个值同样当场生效，不必断开重连。
     *
     * @note **Redis Cluster 不做（这是决定，不是待办）**：集群要在本层之上再起一整层拓扑——slot 路由、
     *       MOVED/ASK 重定向、拓扑刷新与故障切换时的重试，成本与收益不匹配（多键命令还会因跨 slot
     *       直接失败，那等于把「哪些命令能用」再切一刀）。主从这一现实需求由 Sentinel 覆盖：
     *       `resolveSentinelMasterAddress()` 给出主库地址后照常建连即可（见 RedisSentinel.h）。
     *       真要做集群时的落点是「在 `RedisConnection` 之外加一层路由连接（持有 N 条到各节点的连接
     *       并按 slot 分派）」，而不是把这些逻辑塞进本类。
     *
     * @warning 管道命令登记后不立即发送，flushPipeline() 之前不会有任何网络往返；
     *          中途的传输层失败会丢弃尚未读回的回复并断开连接，Redis 侧无法回滚已执行的命令。
     * @warning 订阅要走本类给出的那组入口（subscribe()/psubscribe()/readPushReply()），不要拿
     *          execute() 直接发 SUBSCRIBE：本类按「一条命令一次读回复」的模型执行，一旦对端切到推送
     *          模式，后续回复就会与命令错位。那组入口把「读干确认回复」与「按形状解推送」都做了，
     *          并且把这条连接标成订阅形态。发出这类命令不会被 execute() 拦下（那等于替调用方决定用途），
     *          但连接归还时会直接断开而不是带着错位的回复流回池。
     */
    class ASYN_DATABASE_API RedisConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 使用配置构造 Redis 连接，此阶段不发生任何网络交互
         * @param configuration 连接配置；host/port 用于建连，password/userName 用于认证，
         *                      database 作为键空间编号在 connect() 内被 SELECT
         */
        explicit RedisConnection(const ConnectionConfig &configuration);

        /**
         * @brief 析构时自动断开连接，释放 hiredis 上下文
         */
        ~RedisConnection() override;

        // 上下文所有权唯一：拷贝会出现两个对象 redisFree 同一个 redisContext；
        // 移动会让源对象析构时再次 redisFree（同样的句柄已交给目标对象），
        // 因此拷贝与移动一律禁止（基类同样已删除，这里显式写清意图）。
        RedisConnection(const RedisConnection &) = delete;

        RedisConnection &operator=(const RedisConnection &) = delete;

        RedisConnection(RedisConnection &&) = delete;

        RedisConnection &operator=(RedisConnection &&) = delete;

        /**
         * @brief 连接 Redis 服务并完成协议协商、认证与键空间选择
         * @details 重写 DatabaseConnection::connect()：已连接时直接返回 true 保持幂等（重复
         *          redisConnectWithTimeout 会泄漏前一个上下文）；建连成功后用 redisSetTimeout 应用
         *          queryTimeout()；password 非空时先走二进制安全的 AUTH；配置成 RESP3 时随后发一条
         *          `HELLO 3`（谈不成即建连失败，不静默回落 RESP2，理由见 .cpp 那一段）；database 非空时
         *          最后 SELECT。任一步失败即断开；全部成功后 m_isConnected 才置位。其余与基类一致；
         *          桩构建下直接返回 false。
         * @return true 连接已建立（含协议协商、认证与键空间选择）
         * @return false 任一环节失败，原因见 lastError()
         * @note host 为空视为配置错误，直接失败而不是交给 hiredis 报出难懂的底层错误
         * @note 建连这一路的 HELLO 不进会话记账（它说的是「这条会话按配置就停在 RESP3 上」，不是某个
         *       借用者中途改的形态）；连接建立之后再发 HELLO 仍按「退不回去的模式」处置，归还时断开
         */
        bool connect() override;

        /**
         * @brief 断开连接并释放 hiredis 上下文
         * @details 重写 DatabaseConnection::disconnect()：与基类的差异是会一并丢弃管道缓冲区——
         *          这些命令尚未发送或尚未读回，重连后继续发送会把它们插入到另一条会话中间，
         *          造成服务端状态无法预期的批量写入。
         *          未连接且无残留上下文时是安全的空操作，析构函数会无条件调用本方法。
         * @note 已交出的 RedisResult 不依赖本连接，断开后仍可正常读取
         */
        void disconnect() override;

        /**
         * @brief 判断连接是否可用
         * @details 重写 DatabaseConnection::isConnected()：不做 PING 之类的活性探测（一次往返的代价对
         *          高频命令不可接受），只同时校验 m_isConnected 与 hiredis 上下文非空；链路被对端单方面
         *          断开时这里仍返回 true，真实失效由下一条命令的失败路径发现并断开。其余与基类一致。
         * @return true 已连接且上下文有效
         */
        [[nodiscard]] bool isConnected() const override;

        // 引入基类的全部 execute 重载：本类声明了名为 execute 的成员，按 C++ 名字查找规则
        // 会隐藏基类的同名重载，加上这一行后通过具体对象也能调用两个 execute 与它们的包装。
        using DatabaseConnection::execute;

        /**
         * @brief 以「命令文本 + 参数」的形式执行一条 Redis 命令（参数化入口的 Redis 实现）
         * @details 基类这一档原本只回一句「该驱动暂不支持参数化查询（Redis）」——而 Redis 的命令行
         *          本来就是参数数组，只是没有 SQL 那种占位符。这条通道接上之后
         *          `execute("SET", {DatabaseValue("k"), DatabaseValue("v")})` 与
         *          `executeCommand({"SET", "k", "v"})` 是同一句话，按驱动接口写的应用（以及
         *          `executeChecked()` 那条带回路的包装）在 Redis 上不再是死路。
         * @param command 命令文本：命令名，可带内联参数。**文本里出现 `?` 这个记号即拒绝**——
         *        Redis 没有占位符语法，静默把它当成一个键发出去，比当场报错难查得多
         * @param parameters 逐个转成文本后追加在命令之后，经 argv 接口发送（内嵌 '\0' 与空白都不丢）
         * @return std::unique_ptr<DatabaseResult> 结果集；参数不可表达、命令带占位符、未连接或
         *        服务端报错时返回 nullptr，原因见 lastError()
         * @note 形状判据排在连接检查**之前**：一条本来就不合法的命令不该等到网络状态才知道
         * @see argumentText() 取值到文本的口径
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command, std::span<const DatabaseValue> parameters) override;

        /**
         * @brief 把一个数据库值转成 Redis 的参数文本
         * @details 单独交出来是为了让这条口径可被直接判定，不需要真连上一台服务端。
         *          口径：Int64 与 Double 按十进制文本（Double 用最短可往返表示）；Bool 按本仓 ORM
         *          写侧的口径成 "1"/"0"；Text 与 Blob 原样交字节；NULL（monostate）与复合类型
         *          （List/Hash）拒发——它们没有「一个参数」的对应物，猜一个形状等于静默改写调用方的意思
         * @param value 待转换的值
         * @param errorText 不可表达时写入的中文原因（成功时不动它）
         * @return std::optional<std::string> 参数文本；不可表达时为空
         */
        [[nodiscard]] static std::optional<std::string> argumentText(const DatabaseValue &value, std::string &errorText);

        /**
         * @brief 执行一条 Redis 命令
         * @details 重写 DatabaseConnection::execute()：每次调用开头清空 m_lastError；整行命令按 redis-cli
         *          规则切词后走 argv 接口（参数含空格必须加引号，'%' 不再有格式串含义）；引号未闭合或整行无
         *          有效参数时判定为命令不合法、直接失败且不发送任何字节；服务端 error 回复返回 nullptr 并把
         *          原文写入 lastError()；传输层失败顺带断开连接。其余与基类一致。
         * @param command 命令文本，例如 "SET mykey myvalue"
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command) override;

        /**
         * @brief 获取数据库类型
         * @details 重写 DatabaseConnection::databaseType()：恒定返回 DatabaseType::Redis，桩构建下同样返回本类型。
         * @return DatabaseType DatabaseType::Redis
         */
        [[nodiscard]] DatabaseType databaseType() const override;

        /**
         * @brief 以参数数组形式执行一条 Redis 命令
         * @details 与 execute() 共用同一条发送路径，区别在于参数已经切分好，
         *          因此不需要引号转义，也不必担心参数里含空白：
         *          executeCommand({"SET", "my key", "a\nb"}) 会正确发出三个参数。
         *          参数以「指针 + 长度」交给 hiredis，内嵌 '\0' 不丢失。
         * @param arguments 参数列表，第一个元素为命令名，其余为其参数
         * @return std::unique_ptr<DatabaseResult> 结果集；参数为空、未连接或命令失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> executeCommand(const std::vector<std::string_view> &arguments);

        /**
         * @brief 登记一条管道命令（不立即发送）
         * @details 命令在此处切词并登记进缓冲区，真正的网络往返发生在 flushPipeline()。
         *          批量管道能把 N 次往返压成一次，是 Redis 上最重要的吞吐优化。
         *          不另设「是否处于管道模式」标志：缓冲区非空即代表有命令待发，flush 之后一律为空。
         * @param command 命令文本，切词规则与 execute() 一致
         * @return true 命令已登记
         * @return false 命令不合法（内容为空或引号未闭合），不会登记半个参数，原因见 lastError()
         * @note 未连接时允许登记（此处无 IO），由 flushPipeline() 统一报错并丢弃
         */
        bool pipelineCommand(std::string_view command);

        /**
         * @brief 归还连接池时丢掉残留的会话状态：未发送的管道命令、服务端留着的 MULTI 与 WATCH、
         *        被 SELECT 移走的键空间（无论走不走 selectDatabase()），以及退不出去的模式
         * @details 管道残留命令会被下一个借用者的 flushPipeline() 代发，回复按下标错位且毫无报错；
         *          MULTI 与 WATCH 留在服务端一侧，本地清缓冲清不掉它——留着时下一个借用者的写命令全部
         *          被排进别人的事务、服务端逐条回 +QUEUED，看着像执行成功却一条都没落库。
         *          键空间同理是会话级的：本连接被借去 SELECT 3 之后，不还回配置里那个库，
         *          下一位按配置以为自己停在 15 号库，写进去的键却在 3 号库。
         *          池在归还时统一调用本方法（见 DatabaseConnection::resetSessionState）
         * @note 只在按命令名记的账说「确有残留」时才发清理命令，干净连接不额外付一次往返
         * @note MONITOR / 订阅 / HELLO / AUTH / CLIENT 的几条子命令（REPLY、TRACKING、NO-EVICT、NO-TOUCH）
         *       之后本类退不回原来的会话形态，此时直接断开：池会丢掉这条不健康的连接并另起一条，
         *       比让它带着错位的回复流、别人的 ACL 身份或别人的淘汰保护回池便宜。完整名单与分档理由
         *       见 .cpp 里 noteSessionCommand() 那张按命令名分档的表
         * @return 会话是否已干净：断开式收口（退不回一问一答的那几种模式）交回 false——它同样算
         *         「复位没干净」，池据此记进会话复位失败并丢弃这条连接；本来就干净时交 true；
         *         清理命令发不出去又没走到断开时也交 false
         */
        bool resetSessionState() noexcept override;

        /**
         * @brief 借出前探针：发一条 PING 并读回它的回复
         * @details 真花一次往返，因此认得出「服务端重启过、这条连接已被它丢掉」那一格——
         *          那是 isConnected() 只看本端标志时看不出来的
         * @return true 收到 +PONG；false 发送或读取失败（原因见 lastError()）
         */
        [[nodiscard]] bool probeAlive() noexcept override;

        /**
         * @brief 一次性发送全部已登记的管道命令并读回回复
         * @details 先逐条 append（此时才开始发送），再按已发出的条数读回复，因此返回顺序与登记顺序严格
         *          一致；返回的元素永不为 nullptr。服务端 error 回复被原样封装成 RedisResult 返回，由调用方
         *          用 isError() 定位哪一条失败（连接级 lastError() 表达不了「N 条中的第几条」）。传输层错误时
         *          记录原因、丢弃未读回的命令并断开连接，返回已取到的前缀，已登记的命令一律不重放到新连接。
         * @return std::vector<std::unique_ptr<DatabaseResult> > 与已发送命令一一对应（截断后）的结果集列表
         */
        [[nodiscard]] std::vector<std::unique_ptr<DatabaseResult>> flushPipeline();

        /**
         * @brief 切换键空间（数据库编号）
         * @details 等价于 executeCommand({"SELECT", 编号})。Redis 默认有 16 个键空间（0..15），
         *          服务端也可用 databases 配置项改数量：本方法只校验非负，
         *          超出范围的编号由服务端报错，并按 executeCommand 的失败路径如实返回 false。
         *          切换是连接级会话状态：本对象由池共享时，归还那一刻会 SELECT 回配置里的编号
         *          （见 resetSessionState()），下一个借用者拿到的仍是配置里那个库。
         * @param index 键空间编号，必须为非负整数
         * @return true 切换成功（服务端回了非 error 回复）
         * @return false 编号非法、未连接或服务端拒绝，原因见 lastError()
         */
        bool selectDatabase(int index);

        /**
         * @brief 问一个键还能活多久（`PTTL`，毫秒精度）
         * @details 存在性、有没有设过期、还剩多久这三档在服务端是**一个整数的三种含义**
         *          （`-2` 不存在 / `-1` 存在但永不过期 / 其余为剩余毫秒），本方法把它们交回
         *          `RedisKeyLifetime` 的三个不同形状，调用方不必再记那两个哨兵值。
         *          为什么用 PTTL 而不是 TTL：后者只有秒精度，`1500 毫秒` 会被服务端报成 `1`，
         *          而用它做「还剩多久就该续期」的判断时那个误差正好落在决策点上。
         * @param key 键名（按字节送出，含空白与内嵌 '\0' 都不截断）
         * @return std::optional<RedisKeyLifetime> 三档之一的存活状态
         * @return std::nullopt 问不成——未连接、服务端报错或链路失败，原因见 lastError()
         * @note 「键不存在」不是失败，因此它有值；空值只表示没问到。这两件事混成一个空返回值，
         *       现场就分不清「这个键没人写过」与「这条连接根本没连上」
         */
        [[nodiscard]] std::optional<RedisKeyLifetime> keyLifetime(std::string_view key);

        /**
         * @brief 扫一页键（`SCAN`，游标续扫，一次一条命令）
         * @details 为什么不做「扫到完」的那一层包装：那等于替调用方决定一次无界遍历要不要设上限，
         *          而大键空间上这一条命令序列能把连接占住很久。本方法只走一页，续不续、走几页、
         *          要不要在页与页之间让出连接，都留在调用方这一侧（`RedisScanPage::isFinished()`
         *          就是那个停表）。游标按**字符串**原样透传：它是服务端的迭代状态，当数字解析会静默截断。
         *          SCAN 只保证「回到 0 之前每个至少存在过一次的键会被给出一次」——同一页里可能重复，
         *          扫描期间新建或删除的键不保证出现，这是协议的限制。
         * @param cursor 本次迭代的游标；首轮传 "0"，之后传上一页的 nextCursor
         * @param pattern 键名模式（Glob 风格，如 `session:*`）；空表示不筛
         * @param count 服务端这一页想拿到多少个键的**提示**；为空时不带 COUNT 参数。
         *        它是提示不是上限：服务端可以回更多或更少，真拿到几个键要看返回的 keys
         * @return std::optional<RedisScanPage> 这一页的游标与键
         * @return std::nullopt 游标为空串、未连接、服务端报错或回复形状不对，原因见 lastError()
         */
        [[nodiscard]] std::optional<RedisScanPage> scanPage(std::string_view cursor = "0", std::string_view pattern = {}, std::optional<std::size_t> count = {});

        /**
         * @brief 获取底层 redisContext 句柄，供需要直接使用 hiredis 的高级场景使用
         * @warning 句柄所有权始终属于本连接，调用方不得 redisFree，
         *          也不得在连接销毁后继续使用；拿它去发命令会绕过本类的错误与超时处理，
         *          **也绕过会话状态的记账**（`noteSessionCommand()` 只看走本类的那条路）：从这里发
         *          `SELECT`／`AUTH`／`CLIENT REPLY OFF`／`SUBSCRIBE` 都不会被记下来，归还时因此不会
         *          退回或断开——下一个借用者接的就是这条被改过会话的连接。要发这类命令请走本类的公开入口
         * @return redisContext* 未连接时为 nullptr；桩构建下恒为 nullptr
         */
        [[nodiscard]] redisContext *nativeHandle() const noexcept
        {
            return m_redisContext;
        }

        // ============================================================================
        // 订阅与推送消费：本类的「一条命令一条回复」模型在这里换成「持续读推送」
        // ============================================================================

        /**
         * @brief 订阅若干频道，并把这条连接切到推送形态
         * @details 走的是同一条 argv 发送路径，因此频道名二进制安全（含空格与内嵌 '\0' 都不丢）。
         *          服务端为**每个频道**回一条 subscribe 确认，本方法把它们读干后才返回——留着不读，
         *          第一份确认就会被下一个 readPushReply() 当成消息交出去。
         * @param channels 频道名列表；为空时不发送任何字节并直接返回 false（那是调用方的错，
         *                 不该让服务端去回一个「SUBSCRIBE 需要至少一个参数」的错话）
         * @return true 命令已发出且确认回复收齐
         * @return false 频道列表为空、未连接、这条连接配置成 RESP3 档，或收发失败，原因见 lastError()
         * @note 订阅是连接级会话状态：这条连接不能再交回连接池。归还时 resetSessionState() 会直接
         *       断开它（回复流已与命令错位，留着比丢掉便宜）
         * @note 配置成 RESP3 时**在发送之前就拒**：RESP3 把订阅确认写成推送，本类按「一条命令一条
         *       回复」执行，那条确认收不到（实测等满读写超时才报 recv timeout）。档位与理由见
         *       ConnectionConfig::redisProtocol 与 .cpp 里 kResp3PushPathRefusal 那段取证
         */
        bool subscribe(std::span<const std::string_view> channels);

        /**
         * @brief 按模式订阅（PSUBSCRIBE），确认回复同样在本方法内收干
         * @param patterns 模式列表（Glob 风格，如 `__keyspace@0__:*`），语义与限制同 subscribe()
         * @return true 命令已发出且确认回复收齐
         * @return false 模式列表为空、未连接、这条连接配置成 RESP3 档，或收发失败，原因见 lastError()
         */
        bool psubscribe(std::span<const std::string_view> patterns);

        /**
         * @brief 退掉全部频道与模式订阅，把这条连接送回「一条命令一条回复」的形态
         * @details 不带参数的 UNSUBSCRIBE / PUNSUBSCRIBE 会为**每个当前订阅**回一条确认，本方法按
         *          自己记下的条数把它们收干（服务端回复里那个整数是两类合计的剩余订阅数，分不出
         *          单一类退完没有，所以条数只能本地记）。收干之后这条连接能再发普通命令并拿回
         *          正常回复——用例 UnsubscribeAllRestoresTheCommandReplyShape 钉的就是这件事。
         * @note 但**不要指望它被池留下**：经历过推送形态，这条连接上的 m_isSessionModeChanged 一直是真，
         *       归还时池照旧把它换掉。那一位是 HELLO / MONITOR / 订阅三类共用的「本类退不回去」标记，
         *       在这里顺手清掉就会把前两类的账一起抹了——宁可多换一条连接。
         * @return true 两类订阅都已退干净（本来就没订阅也算成功，且不发任何命令）
         * @return false 未连接或收发失败，原因见 lastError()；此时订阅状态不确定，调用方应断开重连
         */
        bool unsubscribeAll();

        /**
         * @brief 读一条服务端推来的回复，最多等 waitTimeout
         * @details 本驱动是同步的，因此「等」就落在调用线程上：要么由调用方放到工作线程上跑，
         *          要么给一个有限时长。等超时**不算失败**——返回空值、连接保持可用，这是持续消费
         *          的正常节奏（否则每轮空等都得重连）。
         * @param waitTimeout 本次等待上限；非正值表示不设本次超时，按连接当前的 queryTimeout() 等
         * @return std::optional<RedisPushReply> 收到一条推送；超时时返回空值
         * @return std::nullopt 也用于「读坏了」的情况——那种情形 lastError() 非空且连接已断开，
         *         两种空值靠 lastError() 区分（推送本身可以载荷为空，所以不拿空字符串表达失败）
         * @note 这条连接配置成 RESP3 档时立即回空值并把原因写进 lastError()：RESP3 的推送在
         *       hiredis 未接推送回调时会被就地丢弃，等下去只会读满本次时限，然后把「档位不支持」
         *       报成「对端这一轮没发」
         */
        [[nodiscard]] std::optional<RedisPushReply> readPushReply(std::chrono::milliseconds waitTimeout);

        /**
         * @brief 这条连接当前是否停在订阅形态
         * @return true 至少有一条频道或模式订阅未退；此时不要把它交回池
         */
        [[nodiscard]] bool isSubscribing() const noexcept
        {
            return !m_channelSubscriptions.empty() || !m_patternSubscriptions.empty();
        }

        /**
         * @brief 把一条键空间通知的频道与正文拆成「键 / 事件」两个语义字段
         * @details 两种频道形状的两半正好互换（见 RedisKeyspaceNotification），这件事容易记反，
         *          因此放在库里而不是让每个使用方各抄一遍。
         * @param channel 推送的频道名，形如 `__keyspace@3__:user:42` 或 `__keyevent@0__:set`
         * @param payload 推送的正文（另一种内容）
         * @return std::optional<RedisKeyspaceNotification> 认得出形状时给出拆好的两半
         * @return std::nullopt 频道不是这两种形状（前缀不对、缺分隔符、库号不是十进制整数）
         */
        [[nodiscard]] static std::optional<RedisKeyspaceNotification> parseKeyspaceNotification(std::string_view channel, std::string_view payload);

    protected:
        /**
         * @brief 把新的 queryTimeout() 立刻落到已建立的上下文上
         * @details 重写 DatabaseConnection::applyQueryTimeoutNow()：redisSetTimeout 改的是上下文里的收发
         *          超时并顺手对套接字下 setsockopt，对已经连上的会话同样有效，因此借到连接的调用方改完
         *          下一条命令即受新值约束，不必先断开。未连接与降级桩（句柄恒为空）时空操作，
         *          connect() 自会按最新值配置。设不上超时只把原因留在 lastError()，不改变连接状态。
         */
        void applyQueryTimeoutNow() noexcept override;

    private:
        /**
         * @brief 采集 hiredis 上下文的 errstr 与 err 码并写入 m_lastError
         * @param description 面向使用者的中文动作说明，例如「执行 Redis 命令失败」
         */
        void captureError(std::string_view description);

        /**
         * @brief 把 queryTimeout() 应用到已建立的上下文
         * @return true 已应用（含「非正值表示取消超时」这一合法情形）
         * @return false redisSetTimeout 失败，m_lastError 已记录原因
         */
        bool applyQueryTimeout();

        /**
         * @brief 明文建连（redisConnectWithTimeout / redisConnect 两条路），失败原因进 m_lastError
         * @return true 连接已建立且上下文完好；false 失败（句柄已按失败纪律收掉）
         */
        [[nodiscard]] bool openPlainSocket();

        /**
         * @brief 建立传输：明文建连，档位要求 TLS 时握手；Preferred 谈不成弃连重拨回落明文
         * @return true 通道就绪（此时尚未认证）；false 失败，原因进 m_lastError
         */
        [[nodiscard]] bool establishTransport();

        /**
         * @brief 按配置协商 RESP 协议版本：只在 Resp3 档发一条 `HELLO 3`，其余档位一个字节都不发
         * @details 抽成独立一步的理由：`connect()` 的骨架要看得见「传输 → 超时 → 认证 → 协议 → 键空间」
         *          这五格，而不是其中一格摊成二十几行内联。
         *          这一条**不走** `executeArguments`：那条路会给 `noteSessionCommand` 记账，而建连时的
         *          HELLO 说的是「这条会话按配置就停在 RESP3 上」，不是某个借用者中途改的形态——记了账
         *          就等于把池里每条连接都标成「退不回去」，归还时一律断开。借用者自己发 HELLO 的姿态不变
         * @return true 档位无需协商，或服务端确认切到 RESP3
         * @return false 服务端拒绝或链路失败，原因进 m_lastError（调用方负责断开）
         */
        [[nodiscard]] bool negotiateProtocol();

        /**
         * @brief 在已建立的明文连接上做链路 TLS：阻塞握手 + 把 hiredis 的读/写/关闭钩子换到 SSL 上
         * @details 失败原因进 m_lastError。此刻尚未换钩子，调用方按档位决定弃连（Required/Verify*）
         *          还是弃掉重拨回落明文（Preferred）
         * @return true 握手完成、认证与后续命令都在密文里走
         */
        [[nodiscard]] bool secureSocketWithTls();

        /**
         * @brief 用已切分好的参数数组发送一条命令并封装结果（execute/executeCommand 的共同路径）
         * @param argumentValues 参数数组，第一个元素为命令名，调用期间必须保持存活且不被修改。
         *        按视图接收：hiredis 的 argv 接口要的就是「指针 + 长度」，把参数落回 std::string
         *        只会为每个参数多取一次堆块，而内嵌 '\0' 照样靠长度安全穿过
         * @return std::unique_ptr<DatabaseResult> 结果集；传输失败或服务端报错时返回 nullptr
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> executeArguments(std::span<const std::string_view> argumentValues);

        /**
         * @brief 按送出的命令名与它的回复维护「服务端还替这条连接留着什么状态」的记账
         * @details MULTI 与 WATCH 是连接级状态，命令被服务端收下即生效，一直留到 EXEC / DISCARD / RESET
         *          为止；本方法在两条发送路径上各调一次，resetSessionState() 据此决定要不要发清理命令。
         *          六个名字的 error 回复一律意味着服务端什么都没做（EXEC/DISCARD 报「without MULTI」、
         *          WATCH 报「inside MULTI」），因此记账保持原样——据此才漏不掉「被拒的 EXEC 之后仍挂着的
         *          WATCH」。HELLO / MONITOR / 订阅三类改的是回复的形态或流向，本类退不回去，只记一个标记。
         * @param commandName 命令的第一个参数（命令名），Redis 的命令名不区分大小写
         * @param firstArgument 命令的第二个参数，没有则传空串：CLIENT 这一类要按子命令分档，
         *        不能把 CLIENT GETNAME 与 CLIENT REPLY OFF 一并当成「退不回去的模式」
         * @param isAccepted 服务端给出了非 error 回复；false 表示这条命令被原样退回，未改变任何状态
         */
        void noteSessionCommand(std::string_view commandName, std::string_view firstArgument, bool isAccepted) noexcept;

        /**
         * @brief 发出 SUBSCRIBE / PSUBSCRIBE，并把服务端为每个目标回的确认证干
         * @details subscribe() 与 psubscribe() 只差命令名与一处文案，发送、记账与「确认必须收干」
         *          三件事完全相同，因此共用本助手（两条各抄一遍的话，「确认没读干净」这个坑会漏修一次）
         * @param commandName 命令名（"SUBSCRIBE" / "PSUBSCRIBE"）
         * @param targets 频道名或模式名列表，非空由调用方保证（空列表在本方法里直接判失败）
         * @return true 命令已发出且 targets.size() 条确认全部收干（第一条是这条命令本身的回复，
         *         由发送路径顺手收走；剩下几条在本方法里读干）
         * @return false 未连接、收发失败或确认数对不上，原因见 lastError()
         */
        bool startSubscription(std::string_view commandName, std::span<const std::string_view> targets);

        redisContext *m_redisContext{nullptr}; ///< hiredis 连接上下文，本对象独占所有权，未连接时为 nullptr
        /// 链路 TLS 上下文（CA 与客户端证书装在这上面）；档位为 Disabled 或未连过时为空。
        /// SSL 会话本体挂在上下文的 privctx 上、随 redisFree 释放；这份上下文要活到会话释放之后
        std::unique_ptr<Core::TlsContext> m_tlsContext;

        // 之所以在登记时就切词而不是原样缓存命令文本：命令文本的合法性错误能在 pipelineCommand()
        // 当场反馈，不必等到 flush 时才发现「N 条里有一条引号没闭合」
        std::vector<std::vector<std::string>> m_pipelineCommands; ///< 管道命令缓冲区，元素是已切词好的参数数组

        bool m_isInTransaction{false}; ///< 服务端是否停在 MULTI 里：归还时发 DISCARD，否则下一个借用者的写全被排队
        bool m_isWatchingKeys{false};  ///< 服务端是否留着 WATCH 监视：归还时发 UNWATCH，否则别人的键改动会让他人的 EXEC 判成冲突

        int  m_configuredKeySpaceIndex{0};  ///< 配置里那个键空间编号，即一条新会话应当停在的库
        int  m_currentKeySpaceIndex{0};     ///< 本会话实际所在的键空间编号，与上面不等时归还前 SELECT 回去
        bool m_isSessionModeChanged{false}; ///< 是否进入了退不回去的会话模式（MONITOR/订阅/HELLO/AUTH/CLIENT 的几条子命令）：归还时断开这条连接

        // 两类订阅各记各的**去重名单**：服务端为一条订阅只记一份，无参数的 UNSUBSCRIBE / PUNSUBSCRIBE
        // 因此按名单条数回确认（本机 redis 实测：`SUBSCRIBE a a` 回两条 subscribe 确认但计数都是 1，
        // 随后无参数 UNSUBSCRIBE 只回一条）。原先记的是「发出去的目标条数」，重复目标会让账比真订阅多，
        // unsubscribeAll() 就去等一批服务端根本不会发的确认——白等满一次确认时限再把连接判成不确定。
        // 确认里那个整数是两类合计的剩余订阅数，光看回复分不出「这一类退完了没」，所以仍按类分开记。
        // 订阅期间这条连接归调用方独占，本地记的名单与服务端一致
        std::set<std::string> m_channelSubscriptions; ///< 当前活跃的频道订阅名单（去重）
        std::set<std::string> m_patternSubscriptions; ///< 当前活跃的模式订阅名单（去重）
    };

    namespace Detail
    {
        /**
         * @brief 把「毫秒」折成 hiredis 超时所需的「秒 + 剩余微秒」，先钳到 int 能表达的格
         *
         * @details 折成 `timeval` 之前必须钳：`std::chrono::milliseconds::rep` 是 64 位，而这套
         *          「秒 + 微秒」的换算是按 int 做的——2^31 毫秒（约 24.85 天）以上会窄化成负数。
         *          更要紧的是调用方那句「取值大于 0 才算自定义等待」用的是**未窄化**的取值，于是
         *          负数照样被交给 `redisSetTimeout`：把负数塞进 `tv_sec` 在各平台上的行为并不一致
         *          （`applyQueryTimeout` 的注释里写着同一条），轻则立刻返回「超时」让消费循环空转，
         *          重则这一次读取根本没有时限。
         * @param milliseconds 时长毫秒数；负数按 0（= 不设超时）处理
         * @return std::pair<std::int64_t, std::int64_t> 秒数与剩余的微秒数，两者都非负
         */
        [[nodiscard]] ASYN_DATABASE_API std::pair<std::int64_t, std::int64_t> makeTimeoutParts(std::int64_t milliseconds) noexcept;
    } // namespace Detail

} // namespace AsynGyanis::Database
