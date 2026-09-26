/**
 * @file RedisConnection.h
 * @brief Redis 键值存储连接实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseConnection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// hiredis 的 redisContext 与 redisReply 都是全局作用域的 C 结构体，前置声明集中写在本头的全局作用域：
// 只有 .cpp 才包含 <hiredis/hiredis.h>，避免第三方 C 头顺着包含链传染给所有使用方。
// RedisResult.h 通过包含本头复用这两行声明，不得重复声明。
struct redisContext;
struct redisReply;

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
    struct RedisPushReply
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
    struct RedisKeyspaceNotification
    {
        std::int64_t database{0};       ///< 通知来自哪个键空间
        bool         isKeyEvent{false}; ///< true 表示走的是 __keyevent__（正文是键），false 是 __keyspace__（正文是事件）
        std::string  key;               ///< 被改动的键
        std::string  event;             ///< 事件名（set / del / expired / evicted ...）
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
     * @warning 管道命令登记后不立即发送，flushPipeline() 之前不会有任何网络往返；
     *          中途的传输层失败会丢弃尚未读回的回复并断开连接，Redis 侧无法回滚已执行的命令。
     * @warning 订阅要走本类给出的那组入口（subscribe()/psubscribe()/readPushReply()），不要拿
     *          execute() 直接发 SUBSCRIBE：本类按「一条命令一次读回复」的模型执行，一旦对端切到推送
     *          模式，后续回复就会与命令错位。那组入口把「读干确认回复」与「按形状解推送」都做了，
     *          并且把这条连接标成订阅形态。发出这类命令不会被 execute() 拦下（那等于替调用方决定用途），
     *          但连接归还时会直接断开而不是带着错位的回复流回池。
     */
    class RedisConnection : public DatabaseConnection
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
         * @brief 连接 Redis 服务并完成认证与键空间选择
         * @details 重写 DatabaseConnection::connect()：已连接时直接返回 true 保持幂等（重复
         *          redisConnectWithTimeout 会泄漏前一个上下文）；建连成功后用 redisSetTimeout 应用
         *          queryTimeout()；password / database 非空时依次发送二进制安全的 AUTH 与 SELECT，任一步
         *          失败即断开；全部成功后 m_isConnected 才置位。其余与基类一致；桩构建下直接返回 false。
         * @return true 连接已建立（含认证与键空间选择）
         * @return false 任一环节失败，原因见 lastError()
         * @note host 为空视为配置错误，直接失败而不是交给 hiredis 报出难懂的底层错误
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
        // 会隐藏基类的同名重载，加上这一行后通过具体对象也能调用参数化版本。
        // Redis 不是 SQL 数据库，参数化版本由基类默认实现返回中文错误提示，
        // 需要按参数发送命令请改用 executeCommand()
        using DatabaseConnection::execute;

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
         *        被 selectDatabase() 移走的键空间，以及退不出去的模式
         * @details 管道残留命令会被下一个借用者的 flushPipeline() 代发，回复按下标错位且毫无报错；
         *          MULTI 与 WATCH 留在服务端一侧，本地清缓冲清不掉它——留着时下一个借用者的写命令全部
         *          被排进别人的事务、服务端逐条回 +QUEUED，看着像执行成功却一条都没落库。
         *          键空间同理是会话级的：本连接被借去 SELECT 3 之后，不还回配置里那个库，
         *          下一位按配置以为自己停在 15 号库，写进去的键却在 3 号库。
         *          池在归还时统一调用本方法（见 DatabaseConnection::resetSessionState）
         * @note 只在按命令名记的账说「确有残留」时才发清理命令，干净连接不额外付一次往返
         * @note MONITOR / 订阅 / HELLO 之后本类退不回「一条命令一条回复」的形态，此时直接断开：
         *       池会丢掉这条不健康的连接并另起一条，比让它带着错位的回复流回池便宜
         */
        void resetSessionState() noexcept override;

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
         * @brief 获取底层 redisContext 句柄，供需要直接使用 hiredis 的高级场景使用
         * @warning 句柄所有权始终属于本连接，调用方不得 redisFree，
         *          也不得在连接销毁后继续使用；拿它去发命令会绕过本类的错误与超时处理
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
         * @return false 频道列表为空、未连接或收发失败，原因见 lastError()
         * @note 订阅是连接级会话状态：这条连接不能再交回连接池。归还时 resetSessionState() 会直接
         *       断开它（回复流已与命令错位，留着比丢掉便宜）
         */
        bool subscribe(std::span<const std::string_view> channels);

        /**
         * @brief 按模式订阅（PSUBSCRIBE），确认回复同样在本方法内收干
         * @param patterns 模式列表（Glob 风格，如 `__keyspace@0__:*`），语义与限制同 subscribe()
         * @return true 命令已发出且确认回复收齐
         * @return false 模式列表为空、未连接或收发失败，原因见 lastError()
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
         */
        [[nodiscard]] std::optional<RedisPushReply> readPushReply(std::chrono::milliseconds waitTimeout);

        /**
         * @brief 这条连接当前是否停在订阅形态
         * @return true 至少有一条频道或模式订阅未退；此时不要把它交回池
         */
        [[nodiscard]] bool isSubscribing() const noexcept
        {
            return m_channelSubscriptionCount + m_patternSubscriptionCount > 0;
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
         * @param isAccepted 服务端给出了非 error 回复；false 表示这条命令被原样退回，未改变任何状态
         */
        void noteSessionCommand(std::string_view commandName, bool isAccepted) noexcept;

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

        // 之所以在登记时就切词而不是原样缓存命令文本：命令文本的合法性错误能在 pipelineCommand()
        // 当场反馈，不必等到 flush 时才发现「N 条里有一条引号没闭合」
        std::vector<std::vector<std::string>> m_pipelineCommands; ///< 管道命令缓冲区，元素是已切词好的参数数组

        bool m_isInTransaction{false}; ///< 服务端是否停在 MULTI 里：归还时发 DISCARD，否则下一个借用者的写全被排队
        bool m_isWatchingKeys{false};  ///< 服务端是否留着 WATCH 监视：归还时发 UNWATCH，否则别人的键改动会让他人的 EXEC 判成冲突

        int  m_configuredKeySpaceIndex{0};  ///< 配置里那个键空间编号，即一条新会话应当停在的库
        int  m_currentKeySpaceIndex{0};     ///< 本会话实际所在的键空间编号，与上面不等时归还前 SELECT 回去
        bool m_isSessionModeChanged{false}; ///< 是否进入了退不回去的会话模式（MONITOR/订阅/HELLO）：归还时断开这条连接

        // 两类订阅各记各的条数：服务端 UNSUBSCRIBE / PUNSUBSCRIBE 的确认里那个整数是**两类合计**
        // 的剩余订阅数，光看回复分不出「这一类退完了没」，因此按类记账才知道每条命令该收几条确认。
        // 订阅期间这条连接归调用方独占，本地记的数与服务端一致
        std::size_t m_channelSubscriptionCount{0}; ///< 当前活跃的频道订阅条数
        std::size_t m_patternSubscriptionCount{0}; ///< 当前活跃的模式订阅条数
    };

} // namespace AsynGyanis::Database
