#include "Database/Redis/RedisConnection.h"

#include "Database/Common/ErrorText.h"
#include "Database/Redis/RedisResult.h"

#ifdef DATABASE_HAS_REDIS

// struct timeval 的定义在两个平台来源不同：Windows 由 winsock2.h（转包 ws2def.h）给出，
// POSIX 则是 <sys/time.h>。统一经 Platform 层取得：它会先置好 WIN32_LEAN_AND_MEAN 与
// NOMINMAX 再包含网络头，因此本文件不必自己分支，也不会被 windows.h 的 max/min 宏污染
#include "Platform/Platform.h"
#if ASYN_PLATFORM_LINUX
#include <sys/time.h>
#endif

// hiredis 是 C 库，只在本实现文件里包含；对外只暴露 RedisConnection.h 里的前置声明
#include <hiredis/hiredis.h>

#include "Database/Redis/RedisReplyText.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#endif // DATABASE_HAS_REDIS

#include <memory>
#include <string_view>

namespace AsynGyanis::Database
{
#ifdef DATABASE_HAS_REDIS

    namespace
    {
        // 用 constexpr 常量取代宏：单位换算的出处集中在此，类型安全且作用域受控
        constexpr int kMillisecondsPerSecond      = 1000; ///< 1 秒等于 1000 毫秒
        constexpr int kMicrosecondsPerMillisecond = 1000; ///< 1 毫秒等于 1000 微秒

        // redisCommandArgv / redisAppendCommandArgv 的 argc 是 int，超过该上限会被静默截断
        constexpr size_t kMaximumArgumentCount = static_cast<size_t>(std::numeric_limits<int>::max());

        // 参数分隔空白，与 redis-cli 的切词行为一致；手写集合而不是 std::isspace，免得引入 locale 依赖
        constexpr std::string_view kWhitespaceCharacters = " \t\n\v\f\r";

        /**
         * @brief 把毫秒换算成 hiredis 需要的「秒 + 微秒」结构
         * @param milliseconds 时长毫秒数，调用方保证为非负
         * @return struct timeval 换算结果
         */
        struct timeval makeTimeval(const int milliseconds)
        {
            struct timeval timeoutValue;

            // 单位换算：整秒部分 = 毫秒 / 1000，剩下的毫秒还得乘 1000 才是微秒数。
            // tv_sec / tv_usec 的具体类型两个平台不一样（Windows 是 LONG，POSIX 是 time_t 与 suseconds_t），
            // 先按 int 算完再 static_cast 到目标字段类型，避免窄化与符号位警告
            timeoutValue.tv_sec  = static_cast<decltype(timeoutValue.tv_sec)>(milliseconds / kMillisecondsPerSecond);
            timeoutValue.tv_usec = static_cast<decltype(timeoutValue.tv_usec)>((milliseconds % kMillisecondsPerSecond) * kMicrosecondsPerMillisecond);
            return timeoutValue;
        }

        /**
         * @brief 按 redis-cli 规则把整行命令切成参数数组
         * @param command 命令文本，例如 SET "my key" "a b"
         * @return std::optional<std::vector<std::string> > 参数数组；没有有效参数或引号未闭合时返回空值
         */
        std::optional<std::vector<std::string> > splitCommandLine(const std::string_view command)
        {
            std::vector<std::string> arguments;
            std::string              currentArgument;

            bool isInsideQuotes     = false; ///< 是否处于引号内：引号里的空白不参与切分
            bool hasCurrentArgument = false; ///< 是否已开始累积参数：区分「引号包住的空参数」与「还没有参数」
            char quoteCharacter     = '\0';  ///< 当前引号的类型，用于配对闭合

            for (size_t index = 0; index < command.size(); ++index)
            {
                const char currentCharacter = command[index];

                if (isInsideQuotes)
                {
                    // 引号内的反斜杠只转义下一个字符；行尾孤立的反斜杠按普通字符收下，不去猜调用方意图
                    if (currentCharacter == '\\' && index + 1 < command.size())
                    {
                        currentArgument.push_back(command[++index]);
                        continue;
                    }

                    // 闭合引号本身不进入参数内容
                    if (currentCharacter == quoteCharacter)
                    {
                        isInsideQuotes = false;
                        continue;
                    }

                    currentArgument.push_back(currentCharacter);
                    continue;
                }

                // 引号外：空白就是分隔符，连续空白与首尾空白都不会产生空参数
                if (kWhitespaceCharacters.contains(currentCharacter))
                {
                    if (hasCurrentArgument)
                    {
                        // 参数收尾：内容所有权交给数组；move 之后立刻 clear，把源串恢复成确定的空串再复用
                        arguments.push_back(std::move(currentArgument));
                        currentArgument.clear();
                        hasCurrentArgument = false;
                    }
                    continue;
                }

                // 引号开启一段字面量，让含空白的单个参数成为可能
                if (currentCharacter == '"' || currentCharacter == '\'')
                {
                    isInsideQuotes     = true;
                    quoteCharacter     = currentCharacter;
                    hasCurrentArgument = true;
                    continue;
                }

                // 引号外的反斜杠转义下一个字符，用来传空格、引号本身等字符
                if (currentCharacter == '\\' && index + 1 < command.size())
                {
                    currentArgument.push_back(command[++index]);
                    hasCurrentArgument = true;
                    continue;
                }

                currentArgument.push_back(currentCharacter);
                hasCurrentArgument = true;
            }

            // 引号没闭合：整条命令判为不合法。宁可不发，也不能把被截断的参数写到服务端
            if (isInsideQuotes)
            {
                return std::nullopt;
            }

            if (hasCurrentArgument)
            {
                arguments.push_back(std::move(currentArgument));
            }

            // 空串或整行只有空白同样不合法：一条零参数的命令在 Redis 侧只会得到 ERR
            if (arguments.empty())
            {
                return std::nullopt;
            }

            return arguments;
        }

        /**
         * @brief 为 hiredis 的 argv 接口准备「指针数组 + 长度数组」两个平行数组
         * @tparam ArgumentRange 任何可区间遍历、元素有 data() 与 size() 的容器：std::vector<std::string>
         *         （自己解析出来的参数）、std::vector<std::string_view> 与 std::span<const std::string_view>
         *         （调用方给的参数视图）都满足，因此三条来源共用这一份实现，不必为视图先落一份 owning 副本
         * @param argumentValues 参数值列表，整个调用期间必须保持存活且不再被修改
         * @param argumentPointers 出参：每个参数的首地址
         * @param argumentLengths 出参：每个参数的字节长度
         */
        template<typename ArgumentRange>
        void buildArgumentViews(const ArgumentRange &argumentValues, std::vector<const char *> &argumentPointers, std::vector<size_t> &argumentLengths)
        {
            argumentPointers.clear();
            argumentLengths.clear();
            argumentPointers.reserve(argumentValues.size());
            argumentLengths.reserve(argumentValues.size());

            // 长度数组一并给出，Redis 的批量字符串按长度取值，'\0' 因此能安全穿过协议；
            // 指针指向参数自身的缓冲，所以参数表在本函数返回后到调用结束之间不能变
            for (const auto &argumentValue: argumentValues)
            {
                argumentPointers.push_back(argumentValue.data());
                argumentLengths.push_back(argumentValue.size());
            }
        }

        /**
         * @brief 一次命令的参数视图表：窄命令用栈上数组，宽命令才落堆
         * @details hiredis 的 argv 接口要的是「指针数组 + 长度数组」两个连续序列。常见命令只有
         *          两三个参数，为它们各取一块堆内存等于每条命令两次 malloc/free；只有宽命令
         *          （MGET 上千个键）才退回 vector。两条路径都只借调用方的缓冲，不复制参数内容。
         *          管道那一侧仍用可复用的 vector（buildArgumentViews）：那里的暂存表一次分配摊给整批。
         * @tparam ArgumentRange 与 buildArgumentViews 同一约束
         */
        template<typename ArgumentRange>
        class ArgumentViewTable
        {
        public:
            /// 栈上能容下的参数个数：取 8 覆盖 SET/GET/EXPIRE/LPUSH 这类命令的常见宽度，
            /// 两个数组各 8 槽合计 192 字节，摊在栈帧上可以接受
            static constexpr std::size_t kInlineArgumentCapacity = 8;

            /**
             * @brief 从参数列表建表：先定容量去向，再一次性填入两个平行数组
             * @param argumentValues 参数值列表，必须比本对象活得久（表里只有指向它们的指针）
             */
            explicit ArgumentViewTable(const ArgumentRange &argumentValues) :
                m_argumentCount(argumentValues.size())
            {
                if (m_argumentCount <= kInlineArgumentCapacity)
                {
                    fill(m_inlinePointers, m_inlineLengths, argumentValues);
                    return;
                }

                m_heapPointers.resize(m_argumentCount);
                m_heapLengths.resize(m_argumentCount);
                fill(m_heapPointers, m_heapLengths, argumentValues);
            }

            ArgumentViewTable(const ArgumentViewTable &) = delete;

            ArgumentViewTable &operator=(const ArgumentViewTable &) = delete;

            /**
             * @brief 参数首地址数组，可直接交给 redisCommandArgv
             * @return const char ** 长度为 count() 的连续数组
             */
            [[nodiscard]] const char **pointers() noexcept
            {
                return m_argumentCount <= kInlineArgumentCapacity ? m_inlinePointers.data() : m_heapPointers.data();
            }

            /**
             * @brief 参数字节长度数组，可直接交给 redisCommandArgv
             * @return const size_t * 长度为 count() 的连续数组
             */
            [[nodiscard]] const size_t *lengths() noexcept
            {
                return m_argumentCount <= kInlineArgumentCapacity ? m_inlineLengths.data() : m_heapLengths.data();
            }

            /**
             * @brief 参数个数
             * @return std::size_t 与构造时传入的列表长度一致
             */
            [[nodiscard]] std::size_t count() const noexcept
            {
                return m_argumentCount;
            }

        private:
            /**
             * @brief 把参数逐条落成「指针 + 长度」两个平行数组
             * @tparam PointerRange 承接指针的容器（栈数组或 vector）
             * @tparam LengthRange 承接长度的容器
             * @param pointers 目标指针数组，槽位须已就位
             * @param lengths 目标长度数组，槽位须已就位
             * @param argumentValues 参数值列表
             */
            template<typename PointerRange, typename LengthRange>
            static void fill(PointerRange &pointers, LengthRange &lengths, const ArgumentRange &argumentValues)
            {
                std::size_t index = 0;
                for (const auto &argumentValue: argumentValues)
                {
                    pointers[index] = argumentValue.data();
                    lengths[index]  = argumentValue.size();
                    ++index;
                }
            }

            std::size_t                                       m_argumentCount;                 ///< 参数个数，决定走栈还是走堆
            std::array<const char *, kInlineArgumentCapacity> m_inlinePointers{};               ///< 栈上指针数组
            std::array<size_t, kInlineArgumentCapacity>       m_inlineLengths{};                ///< 栈上长度数组
            std::vector<const char *>                         m_heapPointers{};                 ///< 宽命令的指针数组，窄命令下不分配
            std::vector<size_t>                               m_heapLengths{};                  ///< 宽命令的长度数组，窄命令下不分配
        };
    } // namespace

    RedisConnection::RedisConnection(const ConnectionConfig &configuration)
    {
        // 基类的 m_configuration 是唯一真值来源；构造阶段不做任何 IO，
        // 否则「构造一个连接对象」这件事就带上了失败语义
        m_configuration = configuration;
    }

    bool RedisConnection::connect()
    {
        // 已连接时直接返回，满足基类对 connect() 幂等的要求：
        // 重复 redisConnectWithTimeout 会拿到第二个上下文，前一个就此泄漏
        if (m_isConnected)
        {
            return true;
        }

        // 清掉上一轮的失败文本，避免成功路径上 lastError() 仍报旧错
        m_lastError.clear();

        // 空主机交给 hiredis 只会得到一条难懂的底层错误，这里前置拦一道
        if (m_configuration.host.empty())
        {
            m_lastError = "Redis 主机地址未配置";
            return false;
        }

        // 基类的 connectTimeout() 换算成「秒 + 微秒」交给 redisConnectWithTimeout。
        // 该接口只有内存分配失败才返回 nullptr，网络/拒绝连接类失败会返回带 err 的有效上下文，
        // 两条失败路径都必须先摘错误文本再释放上下文——顺序反了就是在读已释放内存
        const int connectionTimeoutMilliseconds = connectTimeout();
        m_redisContext = connectionTimeoutMilliseconds > 0
                                 ? redisConnectWithTimeout(m_configuration.host.c_str(),
                                                           static_cast<int>(m_configuration.port),
                                                           makeTimeval(connectionTimeoutMilliseconds))
                                 // 非正值是「不设连接超时」（与 setQueryTimeout、MySQL 驱动同一口径）：
                                 // 折算成 {0, -1000} 交给 select() 只会得到一次无效或零窗口的等待，
                                 // 对着活着的服务器也报连接失败。redisConnect 这条路径不带等待窗口
                                 : redisConnect(m_configuration.host.c_str(), static_cast<int>(m_configuration.port));
        if (m_redisContext == nullptr)
        {
            // redisConnectWithTimeout 只在内存分配失败时返回空（网络类失败会给出带 err 的上下文），
            // 文案据此给出唯一可行动作，不要罗列与实现不符的「无法访问」
            m_lastError = "创建 Redis 连接上下文失败：客户端库内存分配失败（" + m_configuration.host + ":" +
                          std::to_string(m_configuration.port) + "）——请检查进程内存后重试";
            return false;
        }

        if (m_redisContext->err != 0)
        {
            // captureError 已经把 errstr 拷进 m_lastError，才可以让 disconnect 释放上下文
            captureError("连接 Redis 服务失败");
            disconnect();
            return false;
        }

        // 建连成功后立刻把 queryTimeout() 应用到上下文（不应用则命令可以无限阻塞），
        // 放在认证与 SELECT 之前，让这两步同样处在读写超时的保护之下
        if (!applyQueryTimeout())
        {
            disconnect();
            return false;
        }

        // password 非空才发 AUTH：Redis 的 AUTH 只接受 1 或 2 个参数，
        // 只配了 userName 没配 password 时发出去必然是条错误命令，不如不发
        if (!m_configuration.password.empty())
        {
            // 各留一份可变副本：hiredis 的 %b 按 char* + size_t 取参，
            // 直接传 m_configuration 的 const 视图与其形参类型不严格兼容
            std::string userName = m_configuration.userName;
            std::string password = m_configuration.password;

            // 用 %b（指针 + 长度）而不是 %s：密码里出现 '%' 时不会被当成格式说明符去取并不存在的参数
            // 而格式化接口会越界读甚至直接崩溃，内嵌 '\0' 也能完整送出。
            // userName 非空时走 Redis 6+ 的 ACL 两参数形式 AUTH userName password
            void *rawAuthenticationReply = nullptr;
            if (userName.empty())
            {
                rawAuthenticationReply = redisCommand(m_redisContext, "AUTH %b", password.data(), password.size());
            } else
            {
                rawAuthenticationReply = redisCommand(m_redisContext, "AUTH %b %b",
                                                      userName.data(), userName.size(),
                                                      password.data(), password.size());
            }

            auto *authenticationReply = static_cast<redisReply *>(rawAuthenticationReply);
            if (authenticationReply == nullptr)
            {
                captureError("认证 Redis 服务失败");
                disconnect();
                return false;
            }

            if (authenticationReply->type == REDIS_REPLY_ERROR)
            {
                m_lastError = "Redis 认证失败：" + copyReplyText(authenticationReply) + "（请检查 userName 与 password 配置）";

                // 先释放回复再断开：redisFree 不会替调用方回收已交出的 reply
                freeReplyObject(authenticationReply);
                disconnect();
                return false;
            }

            freeReplyObject(authenticationReply);
        }

        // ConnectionConfig::database 对 Redis 的解释是键空间编号：非空就在建连后 SELECT，
        // 否则这个配置字段会被静默忽略
        m_configuredKeySpaceIndex = 0;
        m_currentKeySpaceIndex    = 0;
        if (!m_configuration.database.empty())
        {
            // 十进制解析：解析失败、留有余文（如 "3abc"）或负值都不猜测、不回退到 0 号库，
            // 静默回退会把命令写进错误的键空间，那比直接失败危险得多
            int                          keySpaceIndex = 0;
            const char *                 parseBegin    = m_configuration.database.data();
            const char *                 parseEnd      = parseBegin + m_configuration.database.size();
            const std::from_chars_result parseResult   = std::from_chars(parseBegin, parseEnd, keySpaceIndex);
            if (parseResult.ec != std::errc() || parseResult.ptr != parseEnd || keySpaceIndex < 0)
            {
                m_lastError = "Redis 键空间编号配置非法：" + m_configuration.database + "，必须是十进制非负整数";
                disconnect();
                return false;
            }

            const std::string keySpaceText = std::to_string(keySpaceIndex);
            if (const std::vector<std::string_view> selectArguments{std::string_view("SELECT"), keySpaceText};
                executeArguments(std::span<const std::string_view>(selectArguments)) == nullptr)
            {
                // executeArguments 已把服务端原文或传输层原因写进 m_lastError，
                // 这里只补一句上下文，说明失败发生在连接初始化阶段
                m_lastError = "选择 Redis 键空间 " + keySpaceText + " 失败：" + m_lastError;
                disconnect();
                return false;
            }

            // 会话确实停在这个库上了，才把它记成「配置要求」与「当前所在」：
            // 归还时按这两者的差决定要不要补一次 SELECT
            m_configuredKeySpaceIndex = keySpaceIndex;
            m_currentKeySpaceIndex    = keySpaceIndex;
        }

        // 全部步骤走通才置位：中途任何失败都不会让 isConnected() 读到「已连接」的中间态
        m_isConnected = true;
        return true;
    }

    void RedisConnection::disconnect()
    {
        // 既没连过也没有残留上下文时是安全的空操作，析构函数会无条件调用本方法
        if (!m_isConnected && m_redisContext == nullptr)
        {
            return;
        }

        // 先落状态再关句柄：关闭过程中若有回调读 isConnected()，也应看到「已断开」
        m_isConnected = false;

        if (m_redisContext != nullptr)
        {
            // redisFree 会一并释放内部缓冲；调用方交出的 redisReply 不属于上下文，需自行释放
            redisFree(m_redisContext);
            m_redisContext = nullptr;
        }

        // 管道缓冲区一律丢弃：这些命令没发出去或没读回来，重连后继续发送
        // 会把它们插进另一条会话中间，造成服务端无法预期的批量写入
        m_pipelineCommands.clear();

        // 服务端侧的事务与监视状态随会话一起消失：新连接上没有残留 MULTI 要清、也没有键被盯着，
        // 记账必须跟着归零，否则重连后的第一次归还白发一条 DISCARD / UNWATCH
        m_isInTransaction = false;
        m_isWatchingKeys  = false;

        // 会话模式与库位也随会话一起没了：下一次 connect() 会按配置重设这两格
        m_isSessionModeChanged = false;
        m_currentKeySpaceIndex = m_configuredKeySpaceIndex;
    }

    bool RedisConnection::isConnected() const
    {
        // 双判据：m_isConnected 是逻辑状态，上下文非空才是物理事实；
        // 两者不一致说明有路径漏置位，一律按未连接处理更安全。
        // 这里刻意不发 PING：一次往返的代价对高频命令不可接受，链路失活由命令失败路径发现
        return m_isConnected && m_redisContext != nullptr;
    }

    std::unique_ptr<DatabaseResult> RedisConnection::execute(const std::string_view command)
    {
        // 每次调用都是独立尝试：先清空错误，成功调用不会残留上一轮的失败文本
        m_lastError.clear();

        if (!isConnected())
        {
            m_lastError = "未连接到 Redis，命令未执行";
            return nullptr;
        }

        // 整行命令必须先切词再交给 argv 接口：直接交给格式化接口会连参数一起压成一个元素
        // （hiredis 的 %s 不认宽度说明符），服务端收到的是 "GET mykey" 这样一条非法命令
        const std::optional<std::vector<std::string> > argumentValues = splitCommandLine(command);
        if (!argumentValues.has_value())
        {
            m_lastError = "Redis 命令不合法（内容为空或引号未闭合）：" + std::string(command);
            return nullptr;
        }

        // 切词得到的那批 std::string 才是数据的持有者；这里只叠一层视图，不再逐条复制内容
        const std::vector<std::string_view> argumentViews(argumentValues->begin(), argumentValues->end());
        return executeArguments(std::span<const std::string_view>(argumentViews));
    }

    std::unique_ptr<DatabaseResult> RedisConnection::executeCommand(const std::vector<std::string_view> &arguments)
    {
        m_lastError.clear();

        if (arguments.empty())
        {
            m_lastError = "Redis 命令参数为空";
            return nullptr;
        }

        if (!isConnected())
        {
            m_lastError = "未连接到 Redis，命令未执行";
            return nullptr;
        }

        // 直接把视图交给 argv 接口：它要的就是「指针 + 长度」，内嵌 '\0' 靠长度而不是终止符穿过协议。
        // 这里落成一份 vector<std::string> 只会为每个参数多取一次堆块，而 arguments 在整个调用期间都活着
        return executeArguments(std::span<const std::string_view>(arguments));
    }

    bool RedisConnection::pipelineCommand(const std::string_view command)
    {
        m_lastError.clear();

        // 切词提前到登记阶段：命令文本非法（引号未闭合、整行没有有效参数）当场反馈，
        // 不必等到 flush 时才发现「N 条里有一条是坏的」
        const std::optional<std::vector<std::string> > argumentValues = splitCommandLine(command);
        if (!argumentValues.has_value())
        {
            m_lastError = "Redis 管道命令不合法（内容为空或引号未闭合）：" + std::string(command);
            return false;
        }

        // 只登记不发送：管道全部的收益都来自 flush 时把多次往返压成一次
        m_pipelineCommands.push_back(std::move(*argumentValues));
        return true;
    }

    std::vector<std::unique_ptr<DatabaseResult> > RedisConnection::flushPipeline()
    {
        m_lastError.clear();

        std::vector<std::unique_ptr<DatabaseResult> > results;
        if (m_pipelineCommands.empty())
        {
            // 空管道不是错误，也不触碰连接状态：直接交出空列表
            return results;
        }

        const size_t registeredCommandCount = m_pipelineCommands.size();
        results.reserve(registeredCommandCount);

        // 整批登记先搬到局部：会话记账要按回复来更新（见下面的读回复循环），而那里只能按同一条
        // 顺序回看命令名。搬到局部也让「已交给协议流的命令一律不重放」在异常路径上自动成立
        std::vector<std::vector<std::string> > registeredCommands;
        registeredCommands.swap(m_pipelineCommands);

        if (!isConnected())
        {
            m_lastError = "未连接到 Redis，" + std::to_string(registeredCommandCount) + " 条管道命令均未发送";
            return results;
        }

        // 第一阶段：把命令逐条 append 进 hiredis 的输出缓冲，到这一步才真正开始发送。
        // 用 argv 接口而不是把命令文本当格式串传进去，否则参数里的 '%' 同样会被解释成格式说明符
        // 这对暂存表跨条复用：buildArgumentViews 进来先 clear()，容量因此逐条继承，
        // 一条长管道不再为每条命令各取两个堆块（管线的全部收益来自批量，这里的常数按条数放大）
        std::vector<const char *> argumentPointers;
        std::vector<size_t>       argumentLengths;
        size_t appendedCommandCount = 0;
        for (const std::vector<std::string> &commandArguments: registeredCommands)
        {
            buildArgumentViews(commandArguments, argumentPointers, argumentLengths);

            if (redisAppendCommandArgv(m_redisContext, static_cast<int>(argumentPointers.size()),
                                       argumentPointers.data(), argumentLengths.data()) != REDIS_OK)
            {
                // 先摘 errstr 再断开：redisFree 之后那就是已释放内存
                captureError("发送 Redis 管道命令失败");
                disconnect();
                return results;
            }

            ++appendedCommandCount;
        }

        // 第二阶段：按「已发出的条数」读回复。Redis 严格按请求顺序回包，
        // 因此 results[i] 与登记顺序的第 i 条命令一一对应
        for (size_t round = 0; round < appendedCommandCount; ++round)
        {
            void *rawReplyPointer = nullptr;
            if (redisGetReply(m_redisContext, &rawReplyPointer) != REDIS_OK)
            {
                // 传输层失败之后回复流的位置已无法对齐，剩余命令的归属无从判断；
                // hiredis 在失败时也可能交出半截回复，非空就得释放，否则直接泄漏
                if (rawReplyPointer != nullptr)
                {
                    freeReplyObject(rawReplyPointer);
                }

                captureError("读取 Redis 管道回复失败");
                m_lastError += "：仅取回 " + std::to_string(results.size()) + " / " + std::to_string(appendedCommandCount) + " 条回复";
                disconnect();

                // 只交出已经取到的前缀：元素数量少于登记数量即代表有命令没回来
                return results;
            }

            auto *serverReply = static_cast<redisReply *>(rawReplyPointer);
            if (serverReply == nullptr)
            {
                // hiredis 约定 REDIS_OK 时不会给出空回复；真遇到就是协议层异常。
                // 这里宁可就地放弃也不把 nullptr 塞进结果列表：空指针交给调用方解引用会直接崩溃
                m_lastError = "读取 Redis 管道回复失败：服务端回复为空，仅取回 " + std::to_string(results.size()) + " / " + std::to_string(appendedCommandCount) + " 条回复";
                disconnect();
                return results;
            }

            // 与单命令路径同一份记账，且同样以回复为准：管道里的 MULTI / EXEC 也会留下（或了结）连接级状态，
            // 漏记的话这条连接带着未了结的事务回池，下一个借用者的写全部被静默排队；
            // 而在 append 那一刻记账会把「被服务端退回的 EXEC」当成已了结，那句 WATCH 就跟着泄漏下去
            noteSessionCommand(registeredCommands[round].front(), serverReply->type != REDIS_REPLY_ERROR);

            // 所有权移交：此后由 RedisResult 析构释放。
            // error 类型的回复刻意保留成结果集而不是报错中断——一条命令失败不该让整批管道作废，
            // 调用方用 RedisResult::isError() 逐条定位，这正是管道路径与单命令路径的差异所在
            results.push_back(std::make_unique<RedisResult>(serverReply));
        }

        return results;
    }

    bool RedisConnection::selectDatabase(const int index)
    {
        // 负编号会让服务端回一句难懂的 ERR，前置校验把问题直接指回调用方
        if (index < 0)
        {
            m_lastError = "Redis 键空间编号非法：" + std::to_string(index) + "，必须为非负整数";
            return false;
        }

        const std::string keySpaceText = std::to_string(index);

        // 走 executeCommand：非 error 回复才算成功（executeArguments 已把 error 转成 nullptr 与原因）
        if (executeCommand({std::string_view("SELECT"), keySpaceText}) == nullptr)
        {
            return false;
        }

        // 服务端认了这个编号才记账：resetSessionState() 按「当前所在 ≠ 配置要求」决定要不要 SELECT 回去，
        // 没换成功就记成换了，等于把库位不明的连接交给下一个借用者
        m_currentKeySpaceIndex = index;
        return true;
    }

    void RedisConnection::captureError(const std::string_view description)
    {
        // errstr 是 redisContext 结构体内的固定数组，随上下文一起释放：
        // 所有调用点都必须「先摘文本、再 redisFree」，顺序反了读到的就是悬垂内存
        if (m_redisContext == nullptr)
        {
            m_lastError = std::string(description) + "：Redis 连接上下文尚未建立";
            return;
        }

        // 少数底层错误不会填 errstr，留一个兜底文本，免得调用方只看到前缀和错误码
        const std::string_view reasonText = (m_redisContext->errstr[0] != '\0') ? std::string_view(m_redisContext->errstr) : std::string_view("未给出原因的协议或系统错误");

        // 带上 hiredis 的 err 码：只有中文文本时排查具体的超时与 DNS 失败仍需要原始数字
        m_lastError = composeNativeErrorText(description, reasonText, "未给出原因的协议或系统错误", m_redisContext->err);
    }

    bool RedisConnection::applyQueryTimeout()
    {
        const int timeoutMilliseconds = queryTimeout();

        // 非正值按「不设收发超时」处理：hiredis 对全零 timeval 的约定正是清除已设置的超时，
        // 而把负数塞进 tv_sec 在各平台上的行为并不一致。正数的毫秒→(tv_sec, tv_usec) 换算见 makeTimeval
        struct timeval timeoutValue{};
        if (timeoutMilliseconds > 0)
        {
            timeoutValue = makeTimeval(timeoutMilliseconds);
        }

        if (redisSetTimeout(m_redisContext, timeoutValue) == REDIS_OK)
        {
            return true;
        }

        // 设不上超时就不算连接成功：一条没有上界的阻塞命令能把整个调用线程挂死
        captureError("设置 Redis 命令超时失败");
        return false;
    }

    void RedisConnection::applyQueryTimeoutNow() noexcept
    {
        // 未连接时无处可设：值留在基类里，connect() 会按最新值配置上下文
        if (m_redisContext == nullptr)
        {
            return;
        }

        // 中途设不上只留原因、不断开这条连接：它仍能按上一次的超时继续用，
        // 而 connect() 那条路把「设不上超时」判成连接失败是因为那里还没有任何保护
        static_cast<void>(applyQueryTimeout());
    }

    std::unique_ptr<DatabaseResult> RedisConnection::executeArguments(const std::span<const std::string_view> argumentValues)
    {
        // 前置条件由调用方保证上下文有效；这里再兜一次，任何路径都不会把空句柄交给 hiredis
        if (m_redisContext == nullptr)
        {
            m_lastError = "未连接到 Redis，命令未执行";
            return nullptr;
        }

        if (argumentValues.empty())
        {
            m_lastError = "Redis 命令参数为空";
            return nullptr;
        }

        // argc 是 int，超出上限会被截断成另一条命令，宁可报错也不发半截
        if (argumentValues.size() > kMaximumArgumentCount)
        {
            m_lastError = "Redis 命令参数过多：" + std::to_string(argumentValues.size()) + " 个，超出协议上限";
            return nullptr;
        }

        // 参数表落在栈上数组里（超出 8 个才退回堆）：单条命令不再为两个平行数组各取一次内存
        ArgumentViewTable<std::span<const std::string_view>> argumentViews(argumentValues);

        // 走 argv 接口而非格式化接口：参数内容里的 '%' 永远不会被解释成格式说明符，
        // '\0' 也按长度完整传递——这是 hiredis 唯一的二进制安全发送路径
        void *rawReplyPointer = redisCommandArgv(m_redisContext, static_cast<int>(argumentViews.count()),
                                                 argumentViews.pointers(), argumentViews.lengths());
        auto *serverReply = static_cast<redisReply *>(rawReplyPointer);
        if (serverReply == nullptr)
        {
            // 传输层失败：上下文已被标记为错误，回复流位置不可知，这条连接不能再用于发命令。
            // 顺序仍是先摘 errstr、再由 disconnect 释放上下文并复位状态
            captureError("执行 Redis 命令失败");
            disconnect();
            return nullptr;
        }

        // 记账按「服务端有没有认这条命令」更新：这六个命令的 error 回复一律表示状态未变，
        // 把它当成「已了结」就会把真实存在的 WATCH 当成已撤销，那句监视接着毒害下一个借用者。
        // 传输层失败在上面已经断开并清零，走不到这里
        noteSessionCommand(argumentValues.front(), serverReply->type != REDIS_REPLY_ERROR);

        if (serverReply->type == REDIS_REPLY_ERROR)
        {
            // 服务端明确回了 error：按基类「失败返回 nullptr，原因见 lastError()」处理，
            // 让只判空指针的调用方也能发现问题，而不是把一条报错当成正常结果读
            m_lastError = "Redis 服务器返回错误：" + copyReplyText(serverReply);

            // 所有权还在本函数手里，交给结果集之前就得自己释放
            freeReplyObject(serverReply);
            return nullptr;
        }

        // 所有权移交：此后由 RedisResult 析构释放，本函数不再触碰 serverReply
        return std::make_unique<RedisResult>(serverReply);
    }

#else // DATABASE_HAS_REDIS —— 桩实现：未找到 hiredis 时编译，所有入口明确失败

    namespace
    {
        // 桩构建的统一失败原因：每个入口都把它写成看得见的错误，
        // 避免调用方把「什么都没做」当成成功
        constexpr const char *kMissingDriverError = "当前构建未编译 Redis 驱动（缺少 hiredis）";
    } // namespace

    RedisConnection::RedisConnection(const ConnectionConfig &configuration)
    {
        // 桩同样只登记配置，不做任何 IO。构造本身是成功的，
        // 因此不提前占用 lastError()——缺失驱动的提示由各入口在真正要用时给出
        m_configuration = configuration;
    }

    bool RedisConnection::connect()
    {
        m_lastError = kMissingDriverError;
        return false;
    }

    void RedisConnection::disconnect()
    {
        // 没有上下文可释放，只把状态与缓冲区归位，保证析构路径调用本方法是安全的
        m_pipelineCommands.clear();
        m_isConnected = false;

        // 会话记账同样归零：桩里永远连不上，留着标记会让 resetSessionState 去发一条注定失败的清理命令
        m_isInTransaction = false;
        m_isWatchingKeys  = false;
    }

    bool RedisConnection::isConnected() const
    {
        return false;
    }

    std::unique_ptr<DatabaseResult> RedisConnection::execute(const std::string_view)
    {
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    std::unique_ptr<DatabaseResult> RedisConnection::executeCommand(const std::vector<std::string_view> &)
    {
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    bool RedisConnection::pipelineCommand(const std::string_view)
    {
        // 桩里连登记都没有意义：命令永远不会被发出，直接返回 false 让调用方知道没接受
        m_lastError = kMissingDriverError;
        return false;
    }

    std::vector<std::unique_ptr<DatabaseResult> > RedisConnection::flushPipeline()
    {
        m_lastError = kMissingDriverError;
        return {};
    }

    bool RedisConnection::selectDatabase(const int)
    {
        m_lastError = kMissingDriverError;
        return false;
    }

    void RedisConnection::applyQueryTimeoutNow() noexcept
    {
        // 桩里没有上下文可设，也不该报错：基类 setter 只是记下取值并通知驱动，真正的失败要在 connect()
        // 上报（那里返回 false）。留空实现同时保证两种构建配置都能链上头文件里那句 override 声明
    }

#endif // DATABASE_HAS_REDIS

    // ------------------------------------------------------------------------
    // 以下定义与是否编译 hiredis 无关，两种构建配置共用
    // ------------------------------------------------------------------------

    namespace
    {
        /**
         * @brief 把一个字符折成小写，只管 ASCII 那一段
         * @details 不用 std::tolower：它按当前 locale 折叠，非英语 locale 下 'I' 之类会折出与 ASCII
         *          不同的结果（本仓库的解析路径已为此吃过一次亏），而 Redis 的命令名只可能是 ASCII
         * @param character 待折叠的字符
         * @return char 折叠结果；非大写 ASCII 原样返回
         */
        constexpr char foldAsciiToLower(const char character) noexcept
        {
            return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
        }

        /**
         * @brief 判断命令名是否就是给定名字（ASCII 大小写不敏感）
         * @param commandName 命令名，取自参数的第一个元素
         * @param lowerCaseName 期望名字，必须已经全小写
         * @return true 两者按大小写不敏感规则相等
         */
        constexpr bool commandNameMatches(const std::string_view commandName, const std::string_view lowerCaseName) noexcept
        {
            if (commandName.size() != lowerCaseName.size())
            {
                return false;
            }

            for (size_t index = 0; index < commandName.size(); ++index)
            {
                if (foldAsciiToLower(commandName[index]) != lowerCaseName[index])
                {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    void RedisConnection::noteSessionCommand(const std::string_view commandName, const bool isAccepted) noexcept
    {
        // 只认留下连接级状态的命令名。先按首字母分叉，其余命令（GET/SET/MGET…）一次字符串比较都不做
        switch (commandName.empty() ? '\0' : foldAsciiToLower(commandName.front()))
        {
            case 'm':
                // MULTI：从这一刻起本连接的命令全部排队，直到 EXEC 或 DISCARD。
                // MONITOR 与它首字母相同，走另一条分支：那条命令把连接变成持续推送，本类读不回「一条命令一条回复」
                if (isAccepted && commandNameMatches(commandName, "monitor"))
                {
                    m_isSessionModeChanged = true;
                }
                else if (isAccepted && commandNameMatches(commandName, "multi"))
                {
                    m_isInTransaction = true;
                }
                return;

            case 'e':
            case 'd':
            case 'r':
                // EXEC / DISCARD / RESET 都会把事务与监视一并了结。被服务端退回时（EXEC/DISCARD
                // without MULTI）这条命令等于没执行，记账必须原样留着——留着才会在归还时补上 UNWATCH
                if (isAccepted && (commandNameMatches(commandName, "exec") || commandNameMatches(commandName, "discard")
                                   || commandNameMatches(commandName, "reset")))
                {
                    m_isInTransaction = false;
                    m_isWatchingKeys  = false;
                }
                return;

            case 'w':
                // WATCH 在 MULTI 之外也能单独发出，并且一直有效到事务了结为止
                if (isAccepted && commandNameMatches(commandName, "watch"))
                {
                    m_isWatchingKeys = true;
                }
                return;

            case 'u':
                // UNWATCH 只撤监视，不碰事务
                if (isAccepted && commandNameMatches(commandName, "unwatch"))
                {
                    m_isWatchingKeys = false;
                }
                return;

            case 'h':
            case 's':
                // HELLO 换掉回复的形态，订阅三个把连接切到推送模式：本类按
                // 「一条命令一条回复」读，退不回去也就无法再替下一个借用者保证读到的就是它那条命令的回复
                if (isAccepted && (commandNameMatches(commandName, "hello") || commandNameMatches(commandName, "subscribe")
                                   || commandNameMatches(commandName, "psubscribe")
                                   || commandNameMatches(commandName, "ssubscribe")))
                {
                    m_isSessionModeChanged = true;
                }
                return;

            default:
                // 其余命令不改变连接级状态：在 MULTI 里排队的写命令尤其不能在这里被当成「事务结束了」
                return;
        }
    }

    void RedisConnection::resetSessionState() noexcept
    {
        // 管道是「登记到 flush 之间」的会话状态：这条连接要交给下一个借用者了，残留命令必须丢掉。
        // 留着的话会被下一位的 flushPipeline() 代发，回复按下标错位且毫无报错
        m_pipelineCommands.clear();

        // 账先取走再归零：本方法要幂等，且清理命令发不出去（链路已断）时也不该留下「还欠一条 DISCARD」
        const bool wasInTransaction          = m_isInTransaction;
        const bool wasWatchingKeys           = m_isWatchingKeys;
        const bool needsFreshSession         = m_isSessionModeChanged;
        const int  keySpaceIndexBeforeReturn = m_currentKeySpaceIndex;
        m_isInTransaction     = false;
        m_isWatchingKeys      = false;
        m_isSessionModeChanged = false;
        m_currentKeySpaceIndex = m_configuredKeySpaceIndex;

        // 桩构建与未连接都在这里止步：没有会话可复位，也就不必为一条发不出去的命令报错
        if (!isConnected())
        {
            return;
        }

        // MONITOR / 订阅 / HELLO 之后本类退不回「一条命令一次回复」：这条连接读到的下一段字节不属于
        // 下一个借用者。断开比把错位的回复流交出去便宜——池看到 isConnected() 为假就会另起一条
        if (needsFreshSession)
        {
            disconnect();
            return;
        }

        try
        {
            if (wasInTransaction)
            {
                // DISCARD 同时撤掉监视，因此事务还在时不必再补一条 UNWATCH
                [[maybe_unused]] const std::unique_ptr<DatabaseResult> discarded = executeCommand({std::string_view("DISCARD")});
            }
            else if (wasWatchingKeys)
            {
                // 只 WATCH 过、没进 MULTI 时 DISCARD 会被服务端判成错误（DISCARD without MULTI），
                // 而那句错误回复并不撤监视，因此这里必须发 UNWATCH
                [[maybe_unused]] const std::unique_ptr<DatabaseResult> unwatched = executeCommand({std::string_view("UNWATCH")});
            }

            // 键空间同样是会话状态：被借去 SELECT 过就得还回配置里那个编号，否则下一位照配置以为
            // 自己停在 15 号库，写进去的键却落在别人的库里，还一句报错都没有。
            // 走 selectDatabase() 而不是另发一条 SELECT：换库的含义（编号校验、成功才算换了）只有一处定义
            if (keySpaceIndexBeforeReturn != m_configuredKeySpaceIndex && !selectDatabase(m_configuredKeySpaceIndex))
            {
                // 还不回去的连接不能当成「已复位」交出去：链路可能已断（失败路径里已断开），
                // 也可能是服务端不认这个编号。两种都让池另起一条，别把库位不明的连接给下一位
                disconnect();
            }
        } catch (...)
        {
            // 归还路径绝不抛出：本方法按基类约定是 noexcept，最坏情况是连接带着未复位的会话状态回池
        }
    }

    RedisConnection::~RedisConnection()
    {
        // RAII 收尾：析构阶段虚表已回到本类，直接调用 disconnect() 而不经虚接口，
        // 保证无论调用方是否显式断开都不会漏掉 redisFree
        RedisConnection::disconnect();
    }

    DatabaseType RedisConnection::databaseType() const
    {
        return DatabaseType::Redis;
    }

} // namespace AsynGyanis::Database
