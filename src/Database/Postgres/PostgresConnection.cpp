/**
 * @file PostgresConnection.cpp
 * @brief PostgreSQL 数据库连接实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

// windows.h 会把 min / max 定义成函数式宏（libpq 的间接包含链里可能出现它），
// 让 std::numeric_limits<T>::max() 与 std::max/std::min 一律编译不过（C4003/C2589）。
// 必须在任何头文件之前定义 NOMINMAX 才能挡住这对宏：放在文件最顶部是唯一与包含顺序无关的写法。
// 它只影响本翻译单元，本文件全程使用 std:: 限定写法，不依赖这两个宏
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Database/Postgres/PostgresConnection.h"

#include "Database/Dialect/PostgresDialect.h"

#ifdef DATABASE_HAS_POSTGRES

#include "Base/Log/LogMacros.h"

// libpq 是 PostgreSQL 官方客户端库，头文件只在实现文件里包含，对外只暴露 PostgresConnection.h
// 的前置声明。它自带平台网络头的包含顺序，调用方无需先包含 winsock2.h 之类
#include <libpq-fe.h>

// 结果集与取值映射同样只在本分支内包含：桩构建下它们要么没有第三方依赖，要么整体被保护隔开
#include "Database/Postgres/PostgresResult.h"
#include "Database/Postgres/PostgresValueConversion.h"

#endif // DATABASE_HAS_POSTGRES

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 校验命令文本不含 NUL 字节
     *
     * @details libpq 的 PQexec / PQexecParams 只接受「零终止 C 字符串」形式的命令，
     *          内嵌 NUL 会让命令被**静默截断**成前半句：多数情况服务端报语法错误，
     *          但截出来的也可能是恰好合法的一条语句——那就成了「执行了半条 SQL 却不报错」，
     *          比直接失败危险得多。因此在这里本地拦下并说明原因。
     *
     *          本函数不依赖 libpq，因此放在两个分支之外：真实驱动与报错桩共用同一份判定，
     *          桩构建下的行为与真实构建保持一致。
     *
     * @param command 待执行的命令文本
     * @return true 通过校验
     * @return false 含 NUL 字节（调用方需把 lastError 置为中文原因后返回 nullptr）
     */
    [[nodiscard]] bool postgresCommandContainsNul(const std::string_view command)
    {
        return command.find('\0') != std::string_view::npos;
    }

#ifdef DATABASE_HAS_POSTGRES

    namespace
    {
        // 用 constexpr 常量取代宏：单位换算与协议上限的出处集中在此，类型安全且作用域受控
        constexpr int kMillisecondsPerSecond = 1000; ///< 1 秒等于 1000 毫秒，毫秒→秒换算的唯一依据

        /// libpq 的 connect_timeout 是整秒，不足 1 秒的超时会被截成 0（含义是无限等待），因此取 1 秒为下限
        constexpr int kMinimumConnectTimeoutSeconds = 1;

        /// 连接字符集：UTF8 才能保证中文列与中文错误消息按同一套编码往返
        constexpr const char *kConnectionCharacterSet = "UTF8";

        /**
         * @brief 把链路 TLS 策略档位翻译成 libpq 的 sslmode 取值
         * @details 五档一一对应（命名本就对齐 MySQL 的 ssl-mode）：Disabled→"disable"、
         *          Preferred→"prefer"、Required→"require"、VerifyCa→"verify-ca"、
         *          VerifyIdentity→"verify-full"。libpq 自己还有一个 "allow" 档（先明文、失败再试 TLS），
         *          本库没有对应策略，永远不会下发。
         * @param mode 链路 TLS 策略档位
         * @return const char * libpq sslmode 取值字面量
         */
        [[nodiscard]] const char *tlsModeToLibpqSslMode(const DatabaseTlsPolicy::Mode mode) noexcept
        {
            switch (mode)
            {
                case DatabaseTlsPolicy::Mode::Disabled:
                    return "disable";
                case DatabaseTlsPolicy::Mode::Preferred:
                    return "prefer";
                case DatabaseTlsPolicy::Mode::Required:
                    return "require";
                case DatabaseTlsPolicy::Mode::VerifyCa:
                    return "verify-ca";
                case DatabaseTlsPolicy::Mode::VerifyIdentity:
                    return "verify-full";
            }
            return "prefer";
        }

        /// libpq 把服务端版本号编成 major * 10000 + minor * 100 + patch（17.11 → 170011）
        constexpr int kPostgresMajorVersionDivisor = 10000;

        /// 次版本号在编号里占两位（minor * 100），取模 100 即得次版本号
        constexpr int kPostgresMinorVersionDivisor = 100;

        /**
         * @brief 语句是否适合进命名语句缓存（数据语句）
         * @details 只认 SELECT/INSERT/UPDATE/DELETE/MERGE/VALUES/WITH 打头的语句。事务控制、SET 与 DDL
         *          每次执行都便宜，且把它们塞进命名池只会占额度；更重要的是，这条判据让
         *          「写错前缀的语句」退回 PQexecParams 的既有语义，而不是被 PQprepare 拒掉
         * @param command 语句原文（前导空白与注释会被跳过）
         * @return true 可以预处理并缓存
         */
        [[nodiscard]] bool isPreparableDataStatement(const std::string_view command) noexcept
        {
            std::size_t cursor = 0;
            while (cursor < command.size() && (command[cursor] == ' ' || command[cursor] == '\t' || command[cursor] == '\r' || command[cursor] == '\n'))
            {
                ++cursor;
            }
            // 跳过前导注释：`--` 到行尾，`/* ... */` 到配对的结束符。跳不过去就按「不可预处理」退回旧路径
            for (;;)
            {
                if (cursor + 1U < command.size() && command[cursor] == '-' && command[cursor + 1U] == '-')
                {
                    const std::size_t lineEnd = command.find('\n', cursor + 2U);
                    cursor                    = lineEnd == std::string_view::npos ? command.size() : lineEnd + 1U;
                } else if (cursor + 1U < command.size() && command[cursor] == '/' && command[cursor + 1U] == '*')
                {
                    const std::size_t blockEnd = command.find("*/", cursor + 2U);
                    if (blockEnd == std::string_view::npos)
                    {
                        return false;
                    }
                    cursor = blockEnd + 2U;
                } else
                {
                    break;
                }
                while (cursor < command.size() && (command[cursor] == ' ' || command[cursor] == '\t' || command[cursor] == '\r' || command[cursor] == '\n'))
                {
                    ++cursor;
                }
            }

            constexpr std::array<std::string_view, 7> kPreparableKeywords{"SELECT", "INSERT", "UPDATE", "DELETE", "MERGE", "VALUES", "WITH"};
            for (const std::string_view keyword: kPreparableKeywords)
            {
                if (command.size() - cursor < keyword.size())
                {
                    continue;
                }
                bool isSameKeyword = true;
                for (std::size_t index = 0; index < keyword.size(); ++index)
                {
                    const char character = command[cursor + index];
                    const char upperCase = (character >= 'a' && character <= 'z') ? static_cast<char>(character - 'a' + 'A') : character;
                    if (upperCase != keyword[index])
                    {
                        isSameKeyword = false;
                        break;
                    }
                }
                if (isSameKeyword)
                {
                    // 关键字后面必须是分隔符：`SELECTED` 这样的标识符不该被当 SELECT
                    const char following = cursor + keyword.size() < command.size() ? command[cursor + keyword.size()] : ' ';
                    if (following == ' ' || following == '\t' || following == '\r' || following == '\n' || following == '(')
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /**
         * @brief 把基类的毫秒超时换算成 libpq 的 connect_timeout 秒数
         * @param milliseconds 时长毫秒数，来自基类的 connectTimeout()
         * @return int 秒数；非正值返回 0，含义是「不超时」（由调用方省略该关键字）
         */
        [[nodiscard]] int toLibrarySeconds(const int milliseconds)
        {
            // 非正值不走换算：libpq 对 0 或负数（以及省略该关键字）都解释为「无限等待」，
            // 与 MySQL 侧「非正值折算成不超时」的约定一致
            if (milliseconds <= 0)
            {
                return 0;
            }

            // 加常数前先钳一下，避免极值输入让 int 加法溢出（有符号溢出是未定义行为）
            const int maximumSafeMilliseconds = std::numeric_limits<int>::max() - (kMillisecondsPerSecond - 1);
            const int boundedMilliseconds     = milliseconds > maximumSafeMilliseconds ? maximumSafeMilliseconds : milliseconds;

            // 单位换算取向上取整：libpq 只吃整秒，若向下取整，500 毫秒会被截成 0——
            // 而 0 的语义恰好相反（永不超时），一个「更短的超时」变成了「没有超时」
            const int seconds = (boundedMilliseconds + kMillisecondsPerSecond - 1) / kMillisecondsPerSecond;
            return seconds > kMinimumConnectTimeoutSeconds ? seconds : kMinimumConnectTimeoutSeconds;
        }

        /**
         * @brief 把 libpq 交出的错误文本整理成可直接拼进中文原因的形式
         * @details 必须立刻拷贝：PQerrorMessage / PQresultErrorMessage 返回的指针只到下一次
         *          使用同一对象（或 PQclear / PQfinish 释放它）之前有效。libpq 的文本惯例上
         *          以换行结尾，直接把换行拼进中文句子会让日志出现断行，因此统一裁掉行尾换行。
         * @param rawMessage libpq 给出的 C 字符串，可为空指针
         * @return std::string 去掉行尾换行后的文本；入参为空指针时返回空串
         */
        [[nodiscard]] std::string trimmedLibraryMessage(const char *rawMessage)
        {
            std::string message(rawMessage != nullptr ? rawMessage : "");

            // 只裁行尾：文本中间的换行是服务端多行提示的一部分（如 DETAIL / HINT 行），保留有助于定位
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
            {
                message.pop_back();
            }

            return message;
        }

        /**
         * @brief PGresult 的自定义释放器，用来罩住「已取出、尚未交给结果集」这段所有权真空
         */
        struct ResultReleaser
        {
            /**
             * @brief 释放结果集句柄
             * @param ownedResult 待释放的 PGresult，可为空指针
             */
            void operator()(PGresult *ownedResult) const noexcept
            {
                // 判空后再释放：不依赖第三方库对空指针的容忍度
                if (ownedResult != nullptr)
                {
                    PQclear(ownedResult);
                }
            }
        };

        /**
         * @brief 一个待下发的连接参数（libpq 的关键字/取值对）
         */
        struct ConnectionParameter
        {
            const char *keyword; ///< 关键字名，例如 "host"，取自 libpq 的常量字符串，生命周期长于本结构
            std::string text;    ///< 取值文本，由本结构持有，保证交给 PQconnectdbParams 的指针有效
        };

        /**
         * @brief 把连接配置摊成 libpq 的关键字/取值对序列
         * @details 空取值一律**省略**而不是原样送出：在 libpq 里「省略」表示用默认规则（用户名取操作
         *          系统账号、口令走 PGPASSWORD 或 ~/.pgpass、库名取用户名），而「送出空串」表示
         *          「值就是空」，服务端会直接回 database "" does not exist 一类错误。
         *          TLS 五档显式下发（含 disable）：不下发时客户端库按自己的默认档走，「这条链路
         *          有没有加密」就成了随环境漂移的隐变量，把决定权收回配置面。
         * @param configuration 连接配置（唯一真值来源）
         * @param connectionTimeoutSeconds 已换算为整秒的建连超时；非正表示不下发（与省略同义）
         * @return std::vector<ConnectionParameter> 参数序列；取值文本由返回的结构持有，
         *         调用方须让它活到 PQconnectdbParams 返回为止
         */
        [[nodiscard]] std::vector<ConnectionParameter> collectConnectionParameters(const ConnectionConfig &configuration, const int connectionTimeoutSeconds)
        {
            std::vector<ConnectionParameter> connectionParameters;

            connectionParameters.push_back(ConnectionParameter{.keyword = "host", .text = configuration.host});

            // port 为 0 表示「配置未指定端口」：省略该关键字让 libpq 回落到编译期默认端口 5432，
            // 而不是把 0 当成端口号送出去（服务端只会回一句语法错误）
            if (configuration.port != 0)
            {
                connectionParameters.push_back(ConnectionParameter{.keyword = "port", .text = std::to_string(configuration.port)});
            }

            if (!configuration.userName.empty())
            {
                connectionParameters.push_back(ConnectionParameter{.keyword = "user", .text = configuration.userName});
            }

            if (!configuration.password.empty())
            {
                // 口令按字节原样送出：libpq 的参数化接口不需要也不允许值里有任何转义
                connectionParameters.push_back(ConnectionParameter{.keyword = "password", .text = configuration.password});
            }

            if (!configuration.database.empty())
            {
                connectionParameters.push_back(ConnectionParameter{.keyword = "dbname", .text = configuration.database});
            }

            connectionParameters.push_back(ConnectionParameter{.keyword = "sslmode", .text = tlsModeToLibpqSslMode(configuration.tls.mode)});

            if (!configuration.tls.certificateAuthorityFile.empty())
            {
                // sslrootcert 是 TLS 校验链的信任锚（也是 verify-ca / verify-full 的判据来源）
                connectionParameters.push_back(ConnectionParameter{.keyword = "sslrootcert", .text = configuration.tls.certificateAuthorityFile});
            }

            if (!configuration.tls.clientCertificateFile.empty())
            {
                // 证书与私钥由 ConnectionConfig::validate() 保证成对给出，这里按 libpq 的关键字名逐个下发
                connectionParameters.push_back(ConnectionParameter{.keyword = "sslcert", .text = configuration.tls.clientCertificateFile});
                connectionParameters.push_back(ConnectionParameter{.keyword = "sslkey", .text = configuration.tls.clientPrivateKeyFile});
            }

            if (connectionTimeoutSeconds > 0)
            {
                // 只在正数时下发：0 与「省略」在 libpq 里同义（无限等待），显式送出只会多一个无意义的键
                connectionParameters.push_back(ConnectionParameter{.keyword = "connect_timeout", .text = std::to_string(connectionTimeoutSeconds)});
            }

            return connectionParameters;
        }

        /// 由连接参数序列生成的两组指针（两个以空指针收尾的平行数组）
        struct ConnectionParameterPointers
        {
            std::vector<const char *> keywords; ///< 关键字指针数组，末尾一个空指针
            std::vector<const char *> values;   ///< 取值指针数组，末尾一个空指针
        };

        /**
         * @brief 由参数序列生成两个以空指针收尾的平行数组
         * @details libpq 靠空指针判断参数列表结束：漏掉它就会越界读取后面的栈内容。数组元素是取值
         *          文本的视图，**源序列必须活到 PQconnectdbParams 返回为止**——不要把本函数直接
         *          套在 collectConnectionParameters(...) 的临时量上。
         * @param parameters 参数序列（须活到使用完）
         * @return ConnectionParameterPointers 两组指针
         */
        [[nodiscard]] ConnectionParameterPointers pointAtConnectionParameters(const std::vector<ConnectionParameter> &parameters)
        {
            ConnectionParameterPointers pointers;
            // 取指针这一步与「收集」分离是有意的：收集期容器的扩容会搬移 std::string 元素，
            // SSO 短字符串的内部缓冲随之失效，边收集边取的指针就成了悬垂指针
            // 末尾各留一个空指针终止符，多留一位保证下面的 push_back 不会触发扩容
            pointers.keywords.reserve(parameters.size() + 1U);
            pointers.values.reserve(parameters.size() + 1U);

            for (const ConnectionParameter &connectionParameter: parameters)
            {
                pointers.keywords.push_back(connectionParameter.keyword);
                pointers.values.push_back(connectionParameter.text.c_str());
            }

            pointers.keywords.push_back(nullptr);
            pointers.values.push_back(nullptr);
            return pointers;
        }

        /**
         * @brief 把实参逐个转成可绑定的文本参数；容器类型当场拒绝
         * @details 容器的正确用法是展开成多个标量参数（如 IN 列表，方言已负责展开），而不是当成单个
         *          参数绑定：静默绑成 NULL 会让调用方以为条件生效了，属于最难排查的一类静默错误。
         *          参数序号按 1 开始计数，与语句里的 $n 编号一致，调用方可直接对照语句定位。
         * @param parameters 全部实参
         * @param textParameters 输出：转换结果；搬进容器后不再改动，交给 libpq 的指针在整个执行期间有效
         * @return std::optional<std::string> 有值即失败原因（调用方写入 lastError()）
         */
        [[nodiscard]] std::optional<std::string> convertParametersToText(const std::span<const DatabaseValue>        parameters,
                                                                         std::vector<Detail::PostgresTextParameter> &textParameters)
        {
            textParameters.clear();
            textParameters.reserve(parameters.size());

            for (std::size_t index = 0; index < parameters.size(); ++index)
            {
                std::optional<Detail::PostgresTextParameter> convertedParameter = Detail::toPostgresTextParameter(parameters[index]);
                if (!convertedParameter.has_value())
                {
                    return "参数化查询不支持容器类型的参数（第 " + std::to_string(index + 1U) + " 个参数，类型 " + std::string(databaseValueTypeName(parameters[index])) +
                           "）：请改用不带参数的 execute()，或先把容器自行序列化成一个标量文本参数后重试";
                }

                textParameters.push_back(std::move(*convertedParameter));
            }
            return std::nullopt;
        }

        /**
         * @brief 参数文本的 NUL 字节本地判定
         * @details PostgreSQL 的文本类型（TEXT / VARCHAR / JSON / 字符类型）在编码层面就不允许 NUL：
         *          服务端会以「invalid byte sequence for encoding "UTF8": 0x00」这类报文拒绝整条
         *          语句，调用方看到的只是一个与编码有关的报错，很难定位到「某个字符串里混进了 '\0'」。
         *          本驱动本就用「指针 + 长度」送出参数，'\0' 不会被截断，是服务端按类型拒绝——要做
         *          字节级二进制存储应使用 BYTEA 列，并按十六进制文本（'\x…'，纯 ASCII、不含 NUL）
         *          作为普通文本参数送出。
         * @param parameters 全部实参
         * @return std::optional<std::string> 有值即失败原因（调用方写入 lastError()）
         */
        [[nodiscard]] std::optional<std::string> rejectNulByteParameters(const std::span<const DatabaseValue> parameters)
        {
            for (std::size_t index = 0; index < parameters.size(); ++index)
            {
                const auto *textValue = std::get_if<std::string>(&parameters[index]);
                if (textValue == nullptr || textValue->find('\0') == std::string::npos)
                {
                    continue;
                }

                return "参数化查询的文本参数不支持 NUL 字节（第 " + std::to_string(index + 1U) +
                       " 个参数）：PostgreSQL 的文本类型（TEXT / VARCHAR 等）无法存储 NUL 字节。"
                       "需要承载任意二进制请改用 BYTEA 列，并把取值按十六进制文本（形如 \\x48656c6c6f）"
                       "作为普通参数送出；若该字符串只是意外含 NUL，请在调用方清理掉";
            }
            return std::nullopt;
        }

        /**
         * @brief 组 PQexecParams 的两个平行数组（取值指针与长度）
         * @details 长度数组非空时 libpq 按「指针 + 长度」取字节，不依赖零终止符，因此内嵌 '\0' 的
         *          文本与空串都能被精确表达（只给 C 字符串会截断前者，省掉长度数组则连后者都表达不出来）。
         *          SQL NULL 只能靠空指针表达：送空串会让 "IS NULL" 不再成立，NULL 与空串也会被混淆。
         *          长度形参是 int，文本超出即失败——宁可当场失败也不送出被截断的数据。
         * @param textParameters 已转换好的参数（须活到 PQexecParams 返回为止，数组元素是它的视图）
         * @param valuePointers 输出：取值指针数组
         * @param valueLengths 输出：长度数组（与指针数组平行）
         * @return std::optional<std::string> 有值即失败原因（调用方写入 lastError()）
         */
        [[nodiscard]] std::optional<std::string> buildParameterArrays(const std::vector<Detail::PostgresTextParameter> &textParameters, std::vector<const char *> &valuePointers,
                                                                      std::vector<int> &valueLengths)
        {
            valuePointers.clear();
            valueLengths.clear();
            valuePointers.reserve(textParameters.size());
            valueLengths.reserve(textParameters.size());

            for (const Detail::PostgresTextParameter &textParameter: textParameters)
            {
                if (textParameter.isNull)
                {
                    // SQL NULL 只能靠空指针表达：送空串会让 "IS NULL" 不再成立，NULL 与空串也会被混淆
                    valuePointers.push_back(nullptr);
                    valueLengths.push_back(0);
                    continue;
                }

                // 长度形参是 int：超出即会被窄化成另一个长度，宁可当场失败也不送出被截断的数据
                if (textParameter.text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    return "文本参数过长：" + std::to_string(textParameter.text.size()) + " 字节，超出 PostgreSQL 单参数上限";
                }

                // data() 而非 c_str()：长度由下面的数组单独给出，beyond 零终止符的字节同样会被取用
                valuePointers.push_back(textParameter.text.data());
                valueLengths.push_back(static_cast<int>(textParameter.text.size()));
            }
            return std::nullopt;
        }
    } // namespace

    PostgresConnection::PostgresConnection(const ConnectionConfig &configuration)
    {
        // 基类的 m_configuration 是唯一真值来源；构造阶段既不建连也不下发任何参数，
        // 否则「构造一个连接对象」这件事就带上了失败语义，超时设置也会被冻结在构造那一刻
        m_configuration = configuration;
    }

    bool PostgresConnection::connect()
    {
        // 已连接时直接返回，满足基类对 connect() 幂等的要求：
        // 在同一句柄上再次握手相当于隐式重连，会丢掉事务与会话变量等全部会话状态
        if (m_isConnected)
        {
            return true;
        }

        // 清掉上一轮的失败文本，避免成功路径上 lastError() 仍报旧错
        m_lastError.clear();

        // 空主机交给 libpq 只会得到一条难懂的底层错误（而且不同版本行为还不一致），这里前置拦一道
        if (m_configuration.host.empty())
        {
            m_lastError = "PostgreSQL 主机地址未配置";
            return false;
        }

        // 链路 TLS 策略自洽性同理：VerifyCa 缺 CA 这类配置错误要在拨号之前点名
        if (const std::optional<std::string> tlsReason = m_configuration.tls.validate(); tlsReason.has_value())
        {
            m_lastError = "连接 PostgreSQL 服务失败：" + *tlsReason;
            return false;
        }

        // 建连超时每次建连都重新读取：外部 setter 因此在下一次 connect() 上生效。
        // 参数序列必须落在具名局部量上：下面生成的指针数组存的是它的取值文本的视图，
        // 直接套在临时量上（pointAtConnectionParameters(collectConnectionParameters(...))）
        // 会让那些指针在整条表达式结束时指向已销毁的字符串
        const std::vector<ConnectionParameter> connectionParameters = collectConnectionParameters(m_configuration, toLibrarySeconds(connectTimeout()));
        const ConnectionParameterPointers      connectionPointers   = pointAtConnectionParameters(connectionParameters);

        // expand_dbname 传 0：不允许把 dbname 的取值当成连接串再展开一次。本驱动的取值全部来自
        // ConnectionConfig 的普通字段，展开只会让「库名里恰好含有 = 与空格」这类输入变成对连接
        // 参数的改写，属于多余且危险的语义
        constexpr int expandDatabaseName = 0;
        m_connection                     = PQconnectdbParams(connectionPointers.keywords.data(), connectionPointers.values.data(), expandDatabaseName);

        // PQconnectdbParams 只在内存分配失败时返回空指针，此时连接句柄根本不存在，
        // 也就没有 PQfinish 要做的事，直接给出可读原因即可
        if (m_connection == nullptr)
        {
            m_lastError = "PostgreSQL 驱动：连接失败：libpq 返回空指针（PQconnectdbParams 内存分配失败）";
            return false;
        }

        if (PQstatus(m_connection) != CONNECTION_OK)
        {
            // 先摘文本再断开：PQerrorMessage 的缓冲归句柄所有，PQfinish 之后即失效，顺序反了就读悬垂指针
            captureError("连接失败");
            disconnect();
            return false;
        }

        // 客户端编码在建连后立即协商为 UTF8：libpq 默认跟随服务端/操作系统的编码，
        // 在 SQL_ASCII 之类的服务端上中文列会变成乱码、错误消息也可能无法按 UTF-8 解读。
        // 协商失败判整次连接失败——静默带着错误编码继续用，只会在更晚的地方以乱码形态暴露
        if (PQsetClientEncoding(m_connection, kConnectionCharacterSet) != 0)
        {
            captureError("设置客户端字符集失败");
            disconnect();
            return false;
        }

        // 全部步骤走通才置位：中途任何一步失败都不会让 isConnected() 读到「已连接」的中间态
        m_isConnected = true;

        // 语句时限只能在握手之后下发（它是一条普通语句）。设不上不该把一次成功的连接判成失败：
        // 会话本身可用，原因留在 lastError() 里；但若这条语句把连接本身弄断了（握手刚完就失联），
        // 那就不是一条可用会话，如实判失败（与 MySqlConnection 的判定同款）
        if (!applyStatementTimeout() && !isConnected())
        {
            return false;
        }

        return true;
    }

    void PostgresConnection::disconnect()
    {
        // 既没连过也没有残留句柄时是安全的空操作，析构函数会无条件调用本方法
        if (!m_isConnected && m_connection == nullptr)
        {
            return;
        }

        // 先落状态再关句柄：关闭过程中若有回调读 isConnected()，也应看到「已断开」。
        // 这里不清 m_lastError——失败路径上先写好的原因不能被断开动作抹掉
        m_isConnected = false;

        if (m_connection == nullptr)
        {
            return;
        }

        // PQfinish 释放句柄内部的全部缓冲，其中就包括 PQerrorMessage 指向的那一份，
        // 因此所有错误文本都必须在本行之前取走（connect()/execute()/takeResult() 的失败路径都遵守
        // 这一顺序）。建连失败时同样要走到这里：失败会话占用的内存同样只有 PQfinish 才能回收
        clearStatementCache();
        PQfinish(m_connection);
        m_connection = nullptr;
    }

    bool PostgresConnection::isConnected() const
    {
        // 三重判据：m_isConnected 是逻辑状态，句柄非空是物理事实，PQstatus 才是链路的真实状态。
        // 前两者不一致说明有路径漏置位，一律按未连接处理；PQstatus 读的是 libpq 本地维护的状态位
        // （不发任何网络往返），对端单方面关掉连接时它会变成 CONNECTION_BAD，
        // 因此这里能发现「标志位仍为真、链路其实已死」的情形
        return m_isConnected && m_connection != nullptr && PQstatus(m_connection) == CONNECTION_OK;
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::execute(const std::string_view command)
    {
        // 每次调用都是独立尝试：先清空错误，成功调用不会残留上一轮的失败文本
        m_lastError.clear();

        // 命令为空是调用方错误，本地即刻判掉，不向服务端发送任何字节
        if (command.empty())
        {
            m_lastError = "数据库命令为空";
            return nullptr;
        }

        // 内嵌 NUL 会让 libpq 把命令截断成前半句（见 postgresCommandContainsNul 的说明），
        // 同样放在连接状态之前：这是纯本地判定，且失败原因比「未连接」更值得优先报出来
        if (postgresCommandContainsNul(command))
        {
            m_lastError = "数据库命令含 NUL 字节：libpq 按零终止 C 字符串解析命令，内嵌 NUL 会把命令"
                          "静默截断成前半句（可能执行出一条不完整的语句而不报错），请先清理命令文本";
            return nullptr;
        }

        if (!isConnected())
        {
            m_lastError = "未连接到 PostgreSQL 数据库，命令未执行";
            return nullptr;
        }

        // 在途未清流：回复流停在中途，任何新语句都会与它交错。在**发送之前**就拒——语句一条都不发，
        // 因此这类拒绝不改变任何服务端状态，可以安全重试；连事务控制语句也不放行（流与连接绑定）
        if (m_openStreamCount != 0)
        {
            m_lastError = "这条连接上还有未消费完的流式结果：流式结果与产出它的连接绑定，在它读完或析构之前，"
                          "本连接不能再发任何语句（含事务控制）。请先把它读完或释放，再执行新命令";
            return nullptr;
        }

        // PQexecParams 的命令文本同样是零终止 C 字符串，而 std::string_view 不保证末尾有 '\0'
        // （它可能是另一个更长缓冲的切片），直接交出 data() 会读到越界内存，因此必须先拷成 std::string
        const std::string commandText(command);

        // 无参数路径也走扩展查询协议（零参数）：简单查询协议（PQexec）会把文本里的**多条语句全部执行**
        // 而只返回最后一条的结果，而扩展协议在解析阶段整次拒绝多语句——这正是 MySQL 侧「握手不开
        // CLIENT_MULTI_STATEMENTS」的同一安全姿态，把「拼接出的第二条语句被静默执行」这道门关上。
        // 事务语句、DDL 与 SET 都是单条 utility statement，扩展协议照常执行（参数格式与结果格式的
        // 空指针含义见带参数的重载）
        PGresult *rawResult = PQexecParams(m_connection, commandText.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0);

        // 状态校验、错误采集、结果集构造与断链复位全部收敛在 takeResult 里，两条 execute 路径因此不会漂移
        return takeResult(rawResult, "执行 SQL 命令失败");
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::execute(const std::string_view command, const std::span<const DatabaseValue> parameters)
    {
        // 每次调用都是独立尝试：先清空错误，成功调用不会残留上一轮的失败文本
        m_lastError.clear();

        // 参数能否绑定是纯本地判定（不向服务端发任何字节），因此放在连接状态之前：越早给出
        // 越省一次网络往返，也让这两条判据不需要服务端即可验证。
        // 这是与 MySqlConnection（先判连接、再判参数）的有意差异
        std::vector<Detail::PostgresTextParameter> textParameters;
        if (std::optional<std::string> reason = convertParametersToText(parameters, textParameters); reason.has_value())
        {
            m_lastError = std::move(*reason);
            return nullptr;
        }
        if (std::optional<std::string> reason = rejectNulByteParameters(parameters); reason.has_value())
        {
            m_lastError = std::move(*reason);
            return nullptr;
        }

        if (command.empty())
        {
            m_lastError = "数据库命令为空";
            return nullptr;
        }

        // 与不带参数的重载同一条判定：PQexecParams 的命令文本同样是零终止 C 字符串
        if (postgresCommandContainsNul(command))
        {
            m_lastError = "数据库命令含 NUL 字节：libpq 按零终止 C 字符串解析命令，内嵌 NUL 会把命令"
                          "静默截断成前半句（可能执行出一条不完整的语句而不报错），请先清理命令文本";
            return nullptr;
        }

        if (!isConnected())
        {
            m_lastError = "未连接到 PostgreSQL 数据库，命令未执行";
            return nullptr;
        }

        // 在途未清流：回复流停在中途，任何新语句都会与它交错。在**发送之前**就拒——语句一条都不发，
        // 因此这类拒绝不改变任何服务端状态，可以安全重试；连事务控制语句也不放行（流与连接绑定）
        if (m_openStreamCount != 0)
        {
            m_lastError = "这条连接上还有未消费完的流式结果：流式结果与产出它的连接绑定，在它读完或析构之前，"
                          "本连接不能再发任何语句（含事务控制）。请先把它读完或释放，再执行新命令";
            return nullptr;
        }

        // PQexecParams 的 nParams 形参是 int：参数个数超出 int 上限时会被静默窄化成一个错误的
        // 数字，进而让服务端按错误的个数解码后续数据，宁可当场失败也不送出畸形报文
        // （PostgresDialect 的协议上限 65535 远低于此，这里只兜住绕过方言直接调用的极端情形）
        if (parameters.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            m_lastError = "参数个数过多：" + std::to_string(parameters.size()) + " 个，超出 PostgreSQL 扩展查询协议可表达的范围";
            return nullptr;
        }

        // 同 PQexec：PQexecParams 的命令文本也是零终止 C 字符串，必须交出副本的 c_str()。
        // 语句文本原样送达，不做任何 "?" → "$n" 改写——PostgresDialect 生成的 $1 $2 就是
        // 扩展查询协议要求的写法，parameters[i] 天然对应第 i+1 个占位符
        const std::string commandText(command);

        // paramValues 与 paramLengths 是两个平行数组（元素是 textParameters 里取值文本的视图，
        // textParameters 在本调用期间不再改动；参数过长的判定在 buildParameterArrays 里）
        std::vector<const char *> valuePointers;
        std::vector<int>          valueLengths;
        if (std::optional<std::string> reason = buildParameterArrays(textParameters, valuePointers, valueLengths); reason.has_value())
        {
            m_lastError = std::move(*reason);
            return nullptr;
        }

        // paramTypes 传空指针：让服务端从语句上下文推断每个参数的类型（$1 出现在 WHERE id = $1
        // 里就推断成 id 的类型），这是扩展查询协议的正规用法，也避免客户端另建一份类型表；
        // paramFormats 传空指针：全部参数按文本格式送出，不引入二进制编码这一层；
        // resultFormat 传 0：结果同样按文本格式取回，与 convertColumnText 的按文本解析一一对应。
        // 零参数时不交出数组（空 vector 的 data() 没有可读元素），nParams 为 0 时 libpq 也不会读它。
        // 数据语句走命名语句缓存：首次 PQprepare，之后 PQexecPrepared；控制语句仍走 PQexecParams
        const int parameterCount = static_cast<int>(parameters.size());
        PGresult *rawResult      = nullptr;
        if (isPreparableDataStatement(commandText))
        {
            const std::optional<std::string> statementName = preparedStatementName(commandText, parameterCount);
            if (!statementName.has_value())
            {
                return nullptr;
            }
            rawResult = PQexecPrepared(m_connection, statementName->c_str(), parameterCount, valuePointers.empty() ? nullptr : valuePointers.data(),
                                       valueLengths.empty() ? nullptr : valueLengths.data(), nullptr, 0);
        } else
        {
            rawResult = PQexecParams(m_connection, commandText.c_str(), parameterCount, nullptr, valuePointers.empty() ? nullptr : valuePointers.data(),
                                     valueLengths.empty() ? nullptr : valueLengths.data(), nullptr, 0);
        }

        return takeResult(rawResult, "执行带参数的 SQL 命令失败");
    }

    std::optional<std::string> PostgresConnection::preparedStatementName(const std::string &commandText, const int parameterCount)
    {
        const auto cached = m_statementCache.find(commandText);
        if (cached != m_statementCache.end())
        {
            cached->second.lastUseStamp = ++m_statementCacheUseStamp;
            ++m_statementCacheHits;
            return cached->second.name;
        }

        if (m_statementCache.size() >= kMaximumCachedStatements)
        {
            evictLeastRecentlyUsedStatement();
        }

        const std::string statementName = "asyn_stmt_" + std::to_string(++m_preparedStatementSerial);
        PGresult         *prepared      = PQprepare(m_connection, statementName.c_str(), commandText.c_str(), parameterCount, nullptr);
        if (prepared == nullptr)
        {
            captureError("准备 SQL 语句失败");
            return std::nullopt;
        }
        if (PQresultStatus(prepared) != PGRES_COMMAND_OK)
        {
            captureResultError(prepared, "准备 SQL 语句失败");
            PQclear(prepared);
            if (PQstatus(m_connection) == CONNECTION_BAD)
            {
                disconnect();
            }
            return std::nullopt;
        }
        PQclear(prepared);

        m_statementCache.emplace(commandText, CachedStatement{statementName, ++m_statementCacheUseStamp});
        return statementName;
    }

    void PostgresConnection::evictLeastRecentlyUsedStatement() noexcept
    {
        if (m_statementCache.empty())
        {
            return;
        }
        auto oldest = m_statementCache.begin();
        for (auto candidate = std::next(m_statementCache.begin()); candidate != m_statementCache.end(); ++candidate)
        {
            if (candidate->second.lastUseStamp < oldest->second.lastUseStamp)
            {
                oldest = candidate;
            }
        }

        // 逐出时把服务端那份也放手：名字是本类生成的 ASCII 标识符，不需要引用
        const std::string deallocateStatement = "DEALLOCATE " + oldest->second.name;
        if (PGresult *deallocated = PQexecParams(m_connection, deallocateStatement.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0); deallocated != nullptr)
        {
            PQclear(deallocated);
        }
        m_statementCache.erase(oldest);
    }

    void PostgresConnection::clearStatementCache() noexcept
    {
        // 只清本地表：调用点是 DISCARD ALL 之前与断链时，服务端那侧由这两条路径各自收掉
        m_statementCache.clear();
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::executeStreaming(const std::string_view command)
    {
        // 每次调用都是独立尝试：先清空错误，成功调用不会残留上一轮的失败文本
        m_lastError.clear();

        // 本地校验（纯判定，不向服务端发任何字节）先于发送：顺序与两条 execute 一致
        if (command.empty())
        {
            m_lastError = "数据库命令为空";
            return nullptr;
        }

        if (postgresCommandContainsNul(command))
        {
            m_lastError = "数据库命令含 NUL 字节：libpq 按零终止 C 字符串解析命令，内嵌 NUL 会把命令"
                          "静默截断成前半句（可能执行出一条不完整的语句而不报错），请先清理命令文本";
            return nullptr;
        }

        if (!isConnected())
        {
            m_lastError = "未连接到 PostgreSQL 数据库，命令未执行";
            return nullptr;
        }

        // 在途未清流：在同一条连接上再开一条流会让两股回复交错，发送前就拒（含本方法自己）
        if (m_openStreamCount != 0)
        {
            m_lastError = "这条连接上还有未消费完的流式结果：流式结果与产出它的连接绑定，在它读完或析构之前，"
                          "本连接不能再发任何语句（含事务控制）。请先把它读完或释放，再执行新命令";
            return nullptr;
        }

        const std::string commandText(command);

        // 与两条 execute 同一协议（扩展查询）：多语句在解析阶段被服务端整次拒绝
        if (PQsendQueryParams(m_connection, commandText.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0) == 0)
        {
            captureError("发送流式查询失败");
            return nullptr;
        }

        // 发送之后的工序（单行模式、首块分派、开流计数）与带参入口共用一份
        return openSingleRowStream();
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::executeStreaming(const std::string_view command, const std::span<const DatabaseValue> parameters)
    {
        // 每次调用都是独立尝试：先清空错误，成功调用不会残留上一轮的失败文本
        m_lastError.clear();

        // 参数的两条本地判定（容器、NUL）放在连接状态之前：与 execute(带参) 同一顺序、同一批助手
        std::vector<Detail::PostgresTextParameter> textParameters;
        if (std::optional<std::string> reason = convertParametersToText(parameters, textParameters); reason.has_value())
        {
            m_lastError = std::move(*reason);
            return nullptr;
        }
        if (std::optional<std::string> reason = rejectNulByteParameters(parameters); reason.has_value())
        {
            m_lastError = std::move(*reason);
            return nullptr;
        }

        // 命令级本地校验（空、NUL）与连接/在途流判定：与另三条执行入口逐条同款
        if (command.empty())
        {
            m_lastError = "数据库命令为空";
            return nullptr;
        }

        if (postgresCommandContainsNul(command))
        {
            m_lastError = "数据库命令含 NUL 字节：libpq 按零终止 C 字符串解析命令，内嵌 NUL 会把命令"
                          "静默截断成前半句（可能执行出一条不完整的语句而不报错），请先清理命令文本";
            return nullptr;
        }

        if (!isConnected())
        {
            m_lastError = "未连接到 PostgreSQL 数据库，命令未执行";
            return nullptr;
        }

        if (m_openStreamCount != 0)
        {
            m_lastError = "这条连接上还有未消费完的流式结果：流式结果与产出它的连接绑定，在它读完或析构之前，"
                          "本连接不能再发任何语句（含事务控制）。请先把它读完或释放，再执行新命令";
            return nullptr;
        }

        if (parameters.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            m_lastError = "参数个数过多：" + std::to_string(parameters.size()) + " 个，超出 PostgreSQL 扩展查询协议可表达的范围";
            return nullptr;
        }

        const std::string commandText(command);

        // paramValues 与 paramLengths 两个平行数组的构造与 execute(带参) 共用同一个助手；
        // 数组元素是 textParameters 里取值文本的视图，活到本函数返回为止（libpq 在 send 时复制）
        std::vector<const char *> valuePointers;
        std::vector<int>          valueLengths;
        if (std::optional<std::string> reason = buildParameterArrays(textParameters, valuePointers, valueLengths); reason.has_value())
        {
            m_lastError = std::move(*reason);
            return nullptr;
        }

        // 与两条 execute 同一协议（扩展查询：多语句在解析阶段被服务端整次拒绝）；之后的单行模式
        // 工序与不带参的流式档逐条相同（顺序、错误处置、收尾口径都一致）
        if (PQsendQueryParams(m_connection, commandText.c_str(), static_cast<int>(parameters.size()), nullptr, valuePointers.empty() ? nullptr : valuePointers.data(),
                              valueLengths.empty() ? nullptr : valueLengths.data(), nullptr, 0) == 0)
        {
            captureError("发送流式查询失败");
            return nullptr;
        }

        // 发送之后的工序与不带参入口共用一份
        return openSingleRowStream();
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::openSingleRowStream()
    {
        // 单行模式必须在「已发送、还没取任何结果」的窗口里打开：这是 libpq 的硬性顺序
        if (PQsetSingleRowMode(m_connection) == 0)
        {
            // 进不了单行模式（正常路径只在连接状态不对时发生）：查询已经发出，必须把回复收干
            // 再失败——残留的回复会让下一条语句读到本次查询的行，协议就此错位
            while (PGresult *pendingChunk = PQgetResult(m_connection))
            {
                PQclear(pendingChunk);
            }
            captureError("切换到单行取数模式失败");
            return nullptr;
        }

        // 先取首块：它要么是第一行（PGRES_SINGLE_TUPLE），要么直接是收尾块（0 行的 PGRES_TUPLES_OK）
        PGresult *firstChunk = PQgetResult(m_connection);
        if (firstChunk == nullptr)
        {
            captureError("读取流式查询的首块失败");
            return nullptr;
        }

        const ExecStatusType firstChunkStatus = PQresultStatus(firstChunk);

        // 空结果：没有「流」可言，交一份普通的预读空结果集（列元数据与行数快照在它的构造里就绪）
        if (firstChunkStatus == PGRES_TUPLES_OK)
        {
            return takeResult(firstChunk, "执行 SQL 命令失败");
        }

        if (firstChunkStatus != PGRES_SINGLE_TUPLE)
        {
            // 首块即失败（语法错、表不存在等）：错误文本与 SQLSTATE 挂在这一块上，按与 execute
            // 失败路径同一口径采集，链路已断则顺手断开
            captureResultError(firstChunk, "执行流式 SQL 命令失败");
            PQclear(firstChunk);
            if (PQstatus(m_connection) == CONNECTION_BAD)
            {
                disconnect();
            }
            return nullptr;
        }

        // 开流成功：计数 +1 表示「这条连接在流读完或结果析构之前不可发新语句」，
        // 归位由结果集在读完、中断或析构收干时经 noteStreamClosed() 完成
        ++m_openStreamCount;
        return PostgresResult::forStreaming(m_connection, this, firstChunk);
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::takeResult(PGresult *const rawResult, const std::string_view description)
    {
        // PQexec / PQexecParams 只在内存不足等极少数情形返回空指针，此时连接自身的错误状态仍可读
        if (rawResult == nullptr)
        {
            captureError(description);
            return nullptr;
        }

        const ExecStatusType resultStatus = PQresultStatus(rawResult);

        // 只有这两种状态代表命令被服务端接受：TUPLES_OK 是查询、COMMAND_OK 是写语句/DDL/事务语句。
        // 其余状态（PGRES_FATAL_ERROR、PGRES_BAD_RESPONSE、PGRES_COPY_* 等）一律算失败，
        // 不做任何「部分成功」的宽容处理
        if (resultStatus != PGRES_TUPLES_OK && resultStatus != PGRES_COMMAND_OK)
        {
            // 先摘文本再 PQclear：PQresultErrorMessage 指向的缓冲归结果集所有，PQclear 之后即失效
            captureResultError(rawResult, description);
            PQclear(rawResult);

            // 链路已断（服务端关掉连接、空闲连接被服务端清理）时该句柄再也无法复用，
            // 顺手断开并释放，让 isConnected() 与后续 execute() 一致地看到「未连接」，调用方重连即可
            if (PQstatus(m_connection) == CONNECTION_BAD)
            {
                disconnect();
            }

            return nullptr;
        }

        // 用带自定义释放器的 unique_ptr 罩住「构造结果集之前」这段所有权真空：
        // make_unique 若因内存分配失败抛异常，PGresult 会由守卫释放而不是像裸指针那样泄漏
        std::unique_ptr<PGresult, ResultReleaser> guardedResult{rawResult};
        std::unique_ptr<PostgresResult>           result = std::make_unique<PostgresResult>(guardedResult.get());

        // 构造成功，所有权正式移交结果集：此后再由 PostgresResult 的析构负责 PQclear
        guardedResult.release();
        return result;
    }

    std::string PostgresConnection::serverVersion() const
    {
        // 服务端版本必须持有已建立连接的句柄才有值（与 SQLite 那种进程内引擎不同），
        // 未连接时返回空串，与 MySqlConnection::serverVersion() 保持一致口径
        if (!isConnected())
        {
            return {};
        }

        // 优先用握手阶段服务端主动送来的 server_version：它是本地缓存，不产生任何网络往返
        const char *rawVersion = PQparameterStatus(m_connection, "server_version");
        if (rawVersion != nullptr)
        {
            const std::string fullVersion(rawVersion);

            // 只取第一个空格之前的版本号本身：server_version 形如 "17.11 (Debian 17.11-1.pgdg120+1)"，
            // 括号里是发行版的打包信息，与「服务端版本」无关，裁掉后调用方拿到的文本可直接参与比较或打印
            const std::size_t firstSpace  = fullVersion.find(' ');
            const std::string versionText = firstSpace == std::string::npos ? fullVersion : fullVersion.substr(0, firstSpace);
            if (!versionText.empty())
            {
                return versionText;
            }
            // 取值存在但为空串（理论上不可达）时继续走下面的数字拼装兜底，而不是返回空串
        }

        const int numericVersion = PQserverVersion(m_connection);
        if (numericVersion <= 0)
        {
            // 两种取值手段都拿不到版本号时返回空串，而不是编造一个数字冒充版本
            return {};
        }

        // 按 libpq 的编号规则逆推：major * 10000 + minor * 100 + patch，
        // 拆出主次版本得到与 server_version 同口径的 "17.11"
        const int majorVersion = numericVersion / kPostgresMajorVersionDivisor;
        const int minorVersion = (numericVersion / kPostgresMinorVersionDivisor) % kPostgresMinorVersionDivisor;
        return std::to_string(majorVersion) + "." + std::to_string(minorVersion);
    }

    bool PostgresConnection::applyStatementTimeout()
    {
        const int timeoutMilliseconds = queryTimeout();

        // statement_timeout 的单位与基类一致（毫秒），不做换算；非正值下发 0，含义是服务端不设限——
        // 不下发会把上一次设过的值继续留着，让「改成不设时限」变成一句空话。
        // 这个数来自 queryTimeout() 这个 int，不是外部文本，因此直接拼进语句不引入注入面
        const std::string statementText = "SET statement_timeout = " + std::to_string(timeoutMilliseconds > 0 ? timeoutMilliseconds : 0);

        if (execute(statementText) != nullptr)
        {
            return true;
        }

        // execute() 已把服务端原文写进 lastError（例如服务端不让改该参数）。再补一句这条会话少了哪道界：
        // 调用方要能看出「语句时限没生效」，而不是把一条没有时限的会话当成受控会话
        m_lastError += "；该会话的语句时限未能下发（statement_timeout）";
        return false;
    }

    void PostgresConnection::applyQueryTimeoutNow() noexcept
    {
        // 未连接时无处可发：connect() 会在握手成功后按最新值下发一次
        if (!isConnected())
        {
            return;
        }

        // 失败必须出声：原因写进 m_lastError 只有「紧接着问 lastError() 的调用方」看得见，而连接池
        // 在归还路径上调用这里之后不会再问——那道时限没下发就等于下一位借用者拿到一条比配置更宽的
        // 连接，两边都不报错。日志是这条路径上唯一还留得住的读数
        if (!applyStatementTimeout())
        {
            LOG_WARN_FMT("PostgresConnection: 命令超时下发失败，这条连接的语句时限按不上（原因：{}）", lastError());
        }
    }

    bool PostgresConnection::resetSessionState() noexcept
    {
        // 未连接：没有会话可复位
        if (!isConnected())
        {
            return true;
        }

        // 在途未清流直接判脏（见 m_openStreamCount）：回复流停在中途，连 ROLLBACK 都发不出去。
        // 一步不做、当场回 false，由池归还路径按「没复位干净」丢弃这条连接——它已经不可复用，
        // 交回池只会让下一个借用者的第一条语句撞上协议错位
        if (m_openStreamCount != 0)
        {
            m_lastError = "这条连接上还有未消费完的流式结果，会话无法复位：流式结果必须读完或析构，连接才能回到可复用状态";
            return false;
        }

        // rollback 与 execute 都会构造 std::string（内存分配失败即抛），而本方法按接口约定是 noexcept：
        // 不接住就是 terminate，因此显式吞掉，并把「没复位干净」如实交回池（池据此丢弃这条连接，
        // 而不是让下一位借用者接着上一笔事务或上一套会话变量执行语句）
        try
        {
            // 事务开着时 DISCARD ALL 会被服务端拒（cannot run inside a transaction block），先滚掉。
            // PQtransactionStatus 读的是 libpq 本地维护的状态位（服务端在应答里自报），不是网络探活；
            // INERROR（失败事务）同样要滚——它同样把连接占在「事务没结束」的状态里
            const PGTransactionStatusType transactionStatus = PQtransactionStatus(m_connection);
            if (transactionStatus == PQTRANS_INTRANS || transactionStatus == PQTRANS_INERROR)
            {
                if (execute("ROLLBACK") == nullptr)
                {
                    return false;
                }
            }

            // DISCARD ALL 把会话里一切可变状态复位：SET 变量、PREPARE 的语句、临时表、游标、
            // LISTEN 注册等——上一个借用者绕过本类入口手工留下的东西同样收得掉。
            // 本地语句缓存在它之前清空：DISCARD ALL 之后那些命名语句已经不存在
            clearStatementCache();
            if (execute("DISCARD ALL") == nullptr)
            {
                return false;
            }

            // DISCARD ALL 把 statement_timeout 一并复位成默认（不限），因此在这里按当前取值重下发：
            // 池的归还顺序是「先本方法、后 restoreQueryTimeoutBaseline」，而后者只在借用者改过取值时
            // 才动手，不在这里重下发就会把一条「比配置更宽」的会话交出去
            return applyStatementTimeout();
        } catch (...)
        {
            return false;
        }
    }

    void PostgresConnection::captureError(const std::string_view description)
    {
        // PQerrorMessage 只反映句柄上最后一次调用的结果，且返回指针只到下一次使用该句柄的 API
        // 调用之前有效，必须立刻拷进 std::string；句柄为空时不能调用它
        if (m_connection == nullptr)
        {
            m_lastError = "PostgreSQL 驱动：" + std::string(description) + "：连接句柄未初始化";
            return;
        }

        std::string libraryMessage = trimmedLibraryMessage(PQerrorMessage(m_connection));
        if (libraryMessage.empty())
        {
            // libpq 的已知退化情形：状态不对但错误缓冲为空，给出可读兜底而不是留下空原因
            libraryMessage = "libpq 未给出原因";
        }

        // 连接级错误没有 SQLSTATE 可取（协议层就把连接失败与查询失败分开了），
        // 因此这类原因只有中文说明与 libpq 原文两项
        m_lastError = "PostgreSQL 驱动：" + std::string(description) + "：" + libraryMessage;
    }

    void PostgresConnection::captureResultError(PGresult *const result, const std::string_view description)
    {
        // 句柄为空时不能调用 PQresultErrorMessage，退回连接级采集（正常路径不可达）
        if (result == nullptr)
        {
            captureError(description);
            return;
        }

        std::string resultMessage = trimmedLibraryMessage(PQresultErrorMessage(result));
        if (resultMessage.empty())
        {
            // 命令本身成功但状态不在白名单内时，PQresultErrorMessage 可能给出空串；
            // 此时退回连接级错误文本，保证 m_lastError 永远有可读原因
            resultMessage = m_connection != nullptr ? trimmedLibraryMessage(PQerrorMessage(m_connection)) : std::string{};
        }

        if (resultMessage.empty())
        {
            resultMessage = "服务端未给出原因";
        }

        // SQLSTATE 是排查服务端拒绝原因的关键（23505 唯一键冲突、57014 查询被取消、42P01 表不存在…），
        // libpq 的错误文本里不带它，因此单独取出拼进原因；取不到时这一项直接省略
        const char       *rawSqlState = PQresultErrorField(result, PG_DIAG_SQLSTATE);
        const std::string sqlState    = rawSqlState != nullptr ? std::string(rawSqlState) : std::string{};

        // 文本与原生码成对写入（见 ErrorRecord）：libpq 的原生码就是 SQLSTATE 文本，没有数字形式，
        // 打包规则见 postgresSqlStateNativeCode()，调用方据此判重试（QueryExecutionException 的白名单里
        // 有 40P01 死锁与 40001 序列化失败）。取不到 SQLSTATE 时落只写文本的路径，码按「未知」处理
        if (sqlState.empty())
        {
            m_lastError = "PostgreSQL 驱动：" + std::string(description) + "：" + resultMessage;
            return;
        }

        m_lastError.assignNative("PostgreSQL 驱动：" + std::string(description) + "：" + resultMessage + "（SQLSTATE " + sqlState + "）", postgresSqlStateNativeCode(sqlState));
    }

#else // DATABASE_HAS_POSTGRES —— 桩实现：CMake 未找到 libpq 时编译，所有入口明确失败

    namespace
    {
        // 桩构建的统一失败原因：每个入口都把它写成看得见的错误，调用方不会把「什么都没做」当成成功
        constexpr const char *kMissingDriverError = "当前构建未编译 PostgreSQL 驱动（缺少 libpq）：装上 libpq 后重新执行 CMake configure 即可启用";
    } // namespace

    PostgresConnection::PostgresConnection(const ConnectionConfig &configuration)
    {
        // 桩同样只登记配置，不做任何分配与 IO。构造本身是成功的，
        // 因此不提前占用 lastError()——缺失驱动的提示由各入口在真正要用时给出
        m_configuration = configuration;
    }

    bool PostgresConnection::connect()
    {
        m_lastError = kMissingDriverError;
        return false;
    }

    void PostgresConnection::disconnect()
    {
        // 没有句柄可释放，只把状态归位，保证析构路径无条件调用本方法是安全的
        m_isConnected = false;
    }

    bool PostgresConnection::isConnected() const
    {
        return false;
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::execute(const std::string_view)
    {
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::execute(const std::string_view, const std::span<const DatabaseValue>)
    {
        // 参数化路径与不带参数的路径在桩里没有区别：连客户端库都没有，既无法预处理也无法绑定参数。
        // 明确报出「驱动缺失」而不是基类默认的「暂不支持参数化查询」，
        // 否则使用者会以为问题出在「这个驱动没实现参数绑定」而不是「当前构建没编译驱动」
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::executeStreaming(const std::string_view)
    {
        // 与两条 execute 同一判据：桩里报的是「当前构建没编译驱动」，而不是基类默认的
        // 「该驱动不支持流式结果集」
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::executeStreaming(const std::string_view, const std::span<const DatabaseValue>)
    {
        // 带参流式同样按「没编译驱动」报，而不是基类默认的「不支持这条路径」
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    std::string PostgresConnection::serverVersion() const
    {
        // 桩里没有客户端库可问，返回空串（含义与「未连接」一致，调用方本就连不上）
        return {};
    }

    std::unique_ptr<DatabaseResult> PostgresConnection::takeResult(PGresult *const, const std::string_view)
    {
        // 桩构建里 connect() 必失败，execute() 在任何网络动作之前就返回，正常路径走不到这里
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    void PostgresConnection::captureError(const std::string_view)
    {
        // 桩构建里没有句柄可采集，一律按「驱动缺失」定性
        m_lastError = kMissingDriverError;
    }

    void PostgresConnection::captureResultError(PGresult *const, const std::string_view)
    {
        // 同 captureError：没有结果集可采集，按「驱动缺失」定性
        m_lastError = kMissingDriverError;
    }

    bool PostgresConnection::applyStatementTimeout()
    {
        // 桩构建里没有句柄可下发，按「驱动缺失」定性（正常路径到不了：connect() 必失败）
        m_lastError = kMissingDriverError;
        return false;
    }

    void PostgresConnection::applyQueryTimeoutNow() noexcept
    {
        // 桩里没有会话可改：连不上服务端，调用方重连的尝试同样会以「驱动缺失」失败
    }

    bool PostgresConnection::resetSessionState() noexcept
    {
        // 桩连接从不建立，没有需要复位的会话
        return true;
    }

#endif // DATABASE_HAS_POSTGRES

    // ------------------------------------------------------------------------
    // 以下定义与是否编译 libpq 无关，两种构建配置共用
    // ------------------------------------------------------------------------

    PostgresConnection::~PostgresConnection()
    {
        // RAII 收尾：析构阶段虚表已回到本类，直接调用 disconnect() 而不经虚接口，
        // 保证无论调用方是否显式断开都不会漏掉 PQfinish。
        // 注意虚析构只能在类内首次声明处 = default，本函数是类外定义，必须给出函数体
        disconnect();
    }

    DatabaseType PostgresConnection::databaseType() const
    {
        return DatabaseType::PostgreSql;
    }

    void PostgresConnection::noteStreamClosed() noexcept
    {
        // 两处调用（流读完/中断、结果析构收干）可能先后都到：幂等地递减，不重复超发
        if (m_openStreamCount != 0)
        {
            --m_openStreamCount;
        }
    }

    bool PostgresConnection::beginTransaction()
    {
        // 语句文本取自方言而不是写死在这里：同一件事（开启事务）若有两份语句源，
        // 就会随方言演进而漂移，与 Transaction 走方言的路径产生行为差异
        const PostgresDialect dialect;
        // 事务语句没有返回列，execute() 非空即代表 BEGIN 已被服务端接受
        return execute(dialect.beginTransactionStatement()) != nullptr;
    }

    bool PostgresConnection::commit()
    {
        const PostgresDialect dialect;
        return execute(dialect.commitStatement()) != nullptr;
    }

    bool PostgresConnection::rollback()
    {
        const PostgresDialect dialect;
        // 没有活动事务时服务端会给出警告级提示（并非错误），此时命令仍然是成功的
        return execute(dialect.rollbackStatement()) != nullptr;
    }

} // namespace AsynGyanis::Database
