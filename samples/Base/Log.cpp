// Base 日志子系统示例：控制台/文件/滚动/异步 Sink、四种格式化器、级别与注册表、异常带栈、配置驱动装配
#include "Base/Config/ConfigManager.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/Formatters/JsonFormatter.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerConfigLoader.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/AsyncSink.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Sinks/FileSink.h"
#include "Base/Log/Sinks/RollingFileSink.h"
#include "common/SampleSupport.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace AsynGyanis;

namespace
{
    /**
     * @brief 数一个文件里有多少行（空文件返回 0，文件不存在返回 -1）
     * @param path 目标文件
     * @return int 行数；读不到即 -1，由调用方判成失败
     */
    int countLines(const std::filesystem::path &path)
    {
        std::ifstream input(path);
        if (!input)
        {
            return -1;
        }
        int lineCount = 0;
        for (std::string line; std::getline(input, line);)
        {
            ++lineCount;
        }
        return lineCount;
    }

    /// @return std::string 文件全文；读不到即空串
    std::string readWholeFile(const std::filesystem::path &path)
    {
        std::ifstream input(path);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    /**
     * @brief 造一种格式化器：按名字取，示例里三种走同一段断言
     * @param formatterName 格式化器名（JsonFormatter / DefaultFormatter / ColorFormatter）
     * @return std::unique_ptr<Base::LogFormatter> 对应的格式化器实例
     */
    std::unique_ptr<Base::LogFormatter> makeFormatter(const std::string &formatterName)
    {
        if (formatterName == "JsonFormatter")
        {
            return std::make_unique<Base::JsonFormatter>();
        }
        if (formatterName == "ColorFormatter")
        {
            return std::make_unique<Base::ColorFormatter>();
        }
        return std::make_unique<Base::DefaultFormatter>();
    }

    /**
     * @brief 逐个验证三种格式化器：都往各自的文件里写一行，再按预期片段核对
     * @param directory 临时目录
     */
    /**
     * @brief 造一个只往指定文件写的记录器，并把它挂进注册表
     * @param name 记录器名
     * @param path 目标文件
     * @param truncate true 表示每次运行都从头覆盖（示例反复跑，不覆盖会一直累加）
     * @return Base::Logger & 注册表里的记录器引用
     */
    Base::Logger &makeFileLogger(const std::string &name, const std::filesystem::path &path, const bool truncate, const std::string &formatterName = {})
    {
        auto &logger = Base::LoggerRegistry::instance().getLogger(name);
        logger.clearSinks();
        auto sink = std::make_unique<Base::FileSink>(path, truncate);
        if (!formatterName.empty())
        {
            // 格式化器挂在 Sink 上：同一个 logger 换 sink 就换了输出形态
            sink->setFormatter(makeFormatter(formatterName));
        }
        logger.addSink(std::move(sink));
        logger.setLevel(Base::LogLevel::Trace);
        return logger;
    }

    void demonstrateRegistry()
    {
        auto &networkLogger = Base::LoggerRegistry::instance().getLogger("sample.net");
        networkLogger.setLevel(Base::LogLevel::Warn);

        // 级别判定要在真正写之前就能问出来：调用方靠它跳过昂贵的格式化
        Samples::checklist().check(networkLogger.shouldLog(Base::LogLevel::Error) && !networkLogger.shouldLog(Base::LogLevel::Info), "具名 logger 按级别过滤（Warn 以上才写）");
        Samples::checklist().check(Base::LoggerRegistry::instance().loggerLevel("sample.net").has_value(), "注册表能问回某个 logger 的级别");

        LOG_LOGGER_WARN_FMT(networkLogger, "网络子系统示例：{} 次重试", 3);

        // 遍历与名单：运维界面靠这两样把全部 logger 列出来调级别
        std::size_t visitedCount = 0;
        Base::LoggerRegistry::instance().forEachLogger([&visitedCount](Base::Logger &) { ++visitedCount; });
        Samples::checklist().check(visitedCount >= 2 && !Base::LoggerRegistry::instance().getLoggerNames().empty(), "forEachLogger 与 getLoggerNames 能看到已登记的 logger");

        Base::LoggerRegistry::instance().setGlobalLevel(Base::LogLevel::Info);
        // 只问根 logger 证不出「扇出到全部 logger」：根本来就是 setupConsoleLogging 给的 Info，
        // 把 setGlobalLevel 换成空函数也照样过。sample.net 在开头被显式设成 Warn，
        // 它被改回 Info 才是这条 API 的说法本身
        Samples::checklist().check(Base::LoggerRegistry::instance().getRootLogger().getLevel() == Base::LogLevel::Info &&
                                           Base::LoggerRegistry::instance().loggerLevel("sample.net") == Base::LogLevel::Info,
                                   "setGlobalLevel 扇出到全部 logger（含显式设过级别的具名 logger）");
    }

    void demonstrateFileSink(const std::filesystem::path &directory)
    {
        const auto path   = directory / "plain.log";
        auto      &logger = makeFileLogger("sample.file", path, true);
        LOG_LOGGER_INFO_FMT(logger, "第一条落盘记录");
        static_cast<void>(logger.flush());

        // writeLine 是 Sink 自己的旁路：不走格式化，用于把已经拼好的文本直接落盘（崩溃兜底用）
        auto sink = std::make_unique<Base::FileSink>(path, false);
        static_cast<void>(sink->writeLine("绕过格式化器的一行"));
        sink->flush();

        Samples::checklist().check(countLines(path) == 2, "FileSink 写入两条：一条走 logger、一条走 writeLine");

        // reopen 换文件：日志切到另一个路径，旧句柄要关掉
        const auto reopenedPath = directory / "plain-2.log";
        sink->reopen(reopenedPath);
        static_cast<void>(sink->writeLine("换文件后的一行"));
        sink->flush();
        sink.reset();
        Samples::checklist().check(countLines(reopenedPath) == 1 && countLines(path) == 2, "FileSink::reopen 之后写到新路径，旧文件不再增长");

        // Fatal 这一条不靠调用方记得 flush：打完就 abort 的路径上，留在流缓冲里的最后一条等于没打。
        // 判据是直接重开文件读它——既不 flush 也不销毁 logger，读得到才算真落到了磁盘上。
        // 放在最后一步：它会让 plain.log 多出一行，夹在上面的「旧文件不再增长」中间就把那条判据改了
        LOG_LOGGER_FATAL_FMT(logger, "崩溃前的最后一条记录");
        Samples::checklist().check(readWholeFile(path).find("崩溃前的最后一条记录") != std::string::npos, "LOG_FATAL 无需显式 flush 就已落盘（崩溃前最后一条不会留在缓冲里）");
    }

    void demonstrateRollingSink(const std::filesystem::path &directory)
    {
        const auto rollingDirectory = directory / "rolling";
        std::filesystem::create_directories(rollingDirectory);

        auto &logger = Base::LoggerRegistry::instance().getLogger("sample.rolling");
        logger.clearSinks();
        // 上限 1 KiB 配 200 字节的正文：几十条就够触发一次滚动
        logger.addSink(std::make_unique<Base::RollingFileSink>("app.log", rollingDirectory, Base::RollingPolicy::Size, 1024, 3));
        logger.setLevel(Base::LogLevel::Info);
        for (int index = 0; index < 60; ++index)
        {
            LOG_LOGGER_INFO_FMT(logger, "滚动示例第 {} 条：{}", index, std::string(200, 'x'));
        }
        static_cast<void>(logger.flush());

        // 滚动过就会出现带序号的备份文件；只配 size 策略时按大小切
        std::size_t rolledFileCount = 0;
        for (const auto &entry: std::filesystem::directory_iterator(rollingDirectory))
        {
            if (entry.is_regular_file())
            {
                ++rolledFileCount;
            }
        }
        Samples::checklist().check(rolledFileCount >= 2, "RollingFileSink 按大小滚出了备份文件");

        // 按天与按小时只走构造与写入路径：时间策略要跨到次日/次时才切，示例不等那个时刻。
        // 两种策略各用一份自己的文件，证据才分得清是谁写出来的（同名共用一份文件时，
        // 第二种策略即使整块没写也照样能看见前一种留下的文件）
        const auto writesUnderTimePolicy = [&](const Base::RollingPolicy policy, const std::string &fileBase, const std::string &nameStem)
        {
            auto timeLogger = std::make_unique<Base::Logger>("sample.time-rolling");
            timeLogger->addSink(std::make_unique<Base::RollingFileSink>(fileBase, rollingDirectory, policy, 1024, 1));
            LOG_LOGGER_INFO_FMT(*timeLogger, "时间策略下的写入");
            static_cast<void>(timeLogger->flush());

            for (const auto &entry: std::filesystem::directory_iterator(rollingDirectory))
            {
                // 时间策略的**当前**文件就把周期段插在扩展名之前（`timed-daily.2026-09-21.log`，
                // 见 RollingFileSink::getCurrentFilename），所以按不带扩展名的主干认
                std::error_code sizeError;
                if (!entry.is_regular_file() || !entry.path().filename().string().starts_with(nameStem))
                {
                    continue;
                }
                if (const std::uintmax_t writtenBytes = std::filesystem::file_size(entry, sizeError); !sizeError && writtenBytes > 0)
                {
                    return true;
                }
            }
            return false;
        };

        Samples::checklist().check(writesUnderTimePolicy(Base::RollingPolicy::Daily, "timed-daily.log", "timed-daily"), "按天滚动的 sink 把一条日志真的写到了磁盘上");
        Samples::checklist().check(writesUnderTimePolicy(Base::RollingPolicy::Hourly, "timed-hourly.log", "timed-hourly"), "按小时滚动的 sink 把一条日志真的写到了磁盘上");
    }

    /// @return Base::LogEvent 一条 Info 级、只带正文的事件（直接喂 Sink 时用）
    Base::LogEvent makeInfoEvent(std::string message)
    {
        Base::LogEvent event;
        event.level   = Base::LogLevel::Info;
        event.message = std::move(message);
        return event;
    }

    void demonstrateAsyncSink(const std::filesystem::path &directory)
    {
        const auto blockingPath = directory / "async-block.log";
        {
            auto            innerSink = std::make_unique<Base::FileSink>(blockingPath, true);
            Base::AsyncSink async(std::move(innerSink), 8, Base::AsyncSink::OverflowPolicy::Block);
            for (int index = 0; index < 50; ++index)
            {
                async.write(makeInfoEvent("异步阻塞策略下的一条记录"));
            }
            async.flush();
        }
        // Block 策略不丢事件：写满队列时会等 worker 腾位置
        Samples::checklist().check(countLines(blockingPath) == 50, "AsyncSink(Block) 把 50 条全部落盘，一条不丢");

        const auto droppingPath = directory / "async-drop.log";
        {
            Base::AsyncSink async(std::make_unique<Base::FileSink>(droppingPath, true), 2, Base::AsyncSink::OverflowPolicy::DropOldest);
            for (int index = 0; index < 50; ++index)
            {
                async.write(makeInfoEvent("异步丢旧策略下的第 " + std::to_string(index) + " 条记录"));
            }
            async.stop();
            // 旧的判据是「0 < 落盘条数 <= 50」——上限由构造保证、下限只要没全丢就成立，丢没丢都过。
            // 换成正反两条：落盘 + 丢弃必须正好等于提交条数（计数不撒谎），且留在盘上的必须是
            // 最新那条（DropOldest 的定义是丢最旧的）。不要求「确实丢了」：那取决于 worker 抢不抢得到
            // 时间片，两种结果下这两条都成立，所以它们不会因为调度运气而假红
            // 记账一律走有符号：countLines 读不到文件时如实给 -1，接成 std::size_t 就变成一个巨大的
            // 正数，守恒式两边因此可以在「文件根本没读出来」时凑巧相等
            const std::int64_t landedLineCount = countLines(droppingPath);
            const std::int64_t droppedCount    = static_cast<std::int64_t>(async.droppedEventCount());
            const std::string  landedText      = readWholeFile(droppingPath);
            Samples::checklist().check(landedLineCount + droppedCount == 50 && landedText.find("第 49 条记录") != std::string::npos,
                                       "AsyncSink(DropOldest) 的丢弃计数与落盘条数对得上账，留下的是最新那条");
        }
    }

    void demonstrateFormatters(const std::filesystem::path &directory)
    {
        // 断言只查「含预期片段」：颜色码与时区随平台不同，逐字节比会假红。
        // 但每例的片段必须只有该格式化器会产出：三例原先都拿 "INFO" 当判据，
        // 颜色版不发 ANSI、或 JSON 把等级写成别的值，全都照样过
        const std::vector<std::pair<std::string, std::string>> cases = {
                {"JsonFormatter", R"("level":"INFO")"},
                // 纯文本版式里等级是补齐后的 "[INFO ]"，紧跟的方括号里就是 logger 名
                {"DefaultFormatter", "[INFO ] [sample.DefaultFormatter]"},
                // 彩色版式把 ANSI 前导插在等级两侧，纯文本里没有这串
                {"ColorFormatter", "\x1b["},
        };
        for (const auto &[formatterName, expectedFragment]: cases)
        {
            const auto path   = directory / (formatterName + ".log");
            auto      &logger = makeFileLogger("sample." + formatterName, path, true, formatterName);
            LOG_LOGGER_INFO_FMT(logger, "格式化器示例：{}", 42);
            static_cast<void>(logger.flush());

            const std::string text = readWholeFile(path);
            Samples::checklist().check(!text.empty() && text.find(expectedFragment) != std::string::npos, formatterName + " 产出的行里带着预期字段");
        }

        // 非法 UTF-8 的处置是「整条不写」而不是写出一串看着合法、内容已被改写的字节——采集端拿到
        // 静默变形的日志比少一条更难查。判据一正一反：坏的那条连标记都不该出现，紧随其后的正常行
        // 照样落得下去，且这条失败不抛给调用方（日志写不出去不该带走业务流程）
        // 文件与 logger 名单独一套：复用上面那一步的 JsonFormatter.log 会把它的正文截掉，
        // 事后翻日志的人就分不清哪个文件是哪一步留下的
        const auto    invalidUtf8Path   = directory / "JsonFormatterInvalidUtf8.log";
        Base::Logger &invalidUtf8Logger = makeFileLogger("sample.JsonFormatterInvalidUtf8", invalidUtf8Path, true, "JsonFormatter");
        std::string   brokenMessage     = "带非法字节的正文 ";
        brokenMessage.push_back('\xFF');
        bool threwIntoCaller = false;
        try
        {
            invalidUtf8Logger.log(Base::LogLevel::Info, brokenMessage);
            invalidUtf8Logger.log(Base::LogLevel::Info, "坏消息之后的正常一行");
        } catch (...)
        {
            threwIntoCaller = true;
        }
        static_cast<void>(invalidUtf8Logger.flush());

        const std::string invalidUtf8Text = readWholeFile(invalidUtf8Path);
        Samples::checklist().check(!threwIntoCaller && invalidUtf8Text.find("带非法字节的正文") == std::string::npos &&
                                           invalidUtf8Text.find("坏消息之后的正常一行") != std::string::npos,
                                   "含非法 UTF-8 的那条整条不写也不抛给调用方，后面的正常行照常落盘");
    }

    void demonstrateExceptionLogging(const std::filesystem::path &directory)
    {
        const auto path   = directory / "exception.log";
        auto      &logger = makeFileLogger("sample.exception", path, true);
        try
        {
            throw Base::InvalidArgumentException("端口号超出可用范围");
        } catch (const std::exception &caught)
        {
            // 这一条把「异常 + 调用栈」一起写下去：栈在构造时捕获，格式化在 Sink 侧才做
            LOG_LOGGER_ERROR_EXCEPTION(logger, caught, "示例里的参数错误。原因：{}", caught.what());
        }
        logger.logWithStackTrace(Base::LogLevel::Error, "主动带栈的一条记录");
        static_cast<void>(logger.flush());

        const std::string text = readWholeFile(path);
        Samples::checklist().check(text.find("端口号超出可用范围") != std::string::npos, "LOG_ERROR_EXCEPTION 把异常原文写进了日志");
        Samples::checklist().check(text.find("demonstrateExceptionLogging") != std::string::npos || text.find("stack") != std::string::npos || text.find('#') != std::string::npos,
                                   "带栈的记录里能看到调用栈的帧（或栈标记）");
    }

    void demonstrateConfigDrivenSetup(const std::filesystem::path &directory)
    {
        // 用真实加载器读一份 YAML：装配出来的 sink 与级别都该按文件生效
        const auto configurationPath = directory / "logging.yaml";
        {
            std::ofstream output(configurationPath);
            output << "logging:\n"
                   << "  global_level: INFO\n"
                   << "  loggers:\n"
                   << "    root:\n"
                   << "      level: WARN\n"
                   << "      sinks:\n"
                   << "        - type: file\n"
                   << "          path: configured.log\n"
                   << "          truncate: true\n"
                   << "        - type: rolling_file\n"
                   << "          base_filename: configured-rolling.log\n"
                   << "          directory: configured-rolling\n"
                   << "          policy: size\n"
                   << "          max_size_mb: 1\n"
                   << "          max_backup: 2\n";
        }
        Base::ConfigManager::instance().clear();
        const bool isLoaded = Base::ConfigManager::instance().loadFiles({configurationPath}).success;
        Samples::checklist().check(isLoaded, "ConfigManager 能把日志配置读进来");
        if (!isLoaded)
        {
            return;
        }

        Base::LoggerConfigLoader::loadFromConfig("logging", directory);
        auto &root = Base::LoggerRegistry::instance().getRootLogger();
        // 此刻 root 已经是「文件 sink + WARN 起」：示例自己的后续输出都会落进文件里。先把结论取成
        // 局部量，恢复控制台之后再断言，否则这几步在终端上凭空消失
        const bool isLevelApplied = root.getLevel() == Base::LogLevel::Warn;
        LOG_ERROR("配置驱动装配之后的一条错误日志");
        static_cast<void>(root.flush());
        const bool isFileSinkApplied = countLines(directory / "configured.log") == 1;
        // 第二个 sink 也得有人验：rolling_file 分支自己建目录、并把 base_filename 拼到 directory
        // 之下，只查 file sink 等于这条装配链从头到尾没人看过
        const bool isRollingSinkApplied = countLines(directory / "configured-rolling" / "configured-rolling.log") == 1;
        root.clearSinks();
        Samples::setupConsoleLogging();
        Samples::checklist().check(isLevelApplied, "配置里的 root.level 真的生效了");
        Samples::checklist().check(isFileSinkApplied, "配置里的 file sink 确实收到了这条日志");
        Samples::checklist().check(isRollingSinkApplied, "配置里的 rolling_file sink 也收到了同一条日志");
    }

    void demonstrateLifetime(const std::filesystem::path &directory)
    {
        const auto path = directory / "retired.log";
        {
            auto scoped = std::make_unique<Base::Logger>("sample.retired");
            scoped->addSink(std::make_unique<Base::FileSink>(path, true));
            LOG_LOGGER_INFO_FMT(*scoped, "登记前的一条记录");
            Base::LoggerRegistry::instance().registerLogger(std::move(scoped));
        }
        Samples::checklist().check(Base::LoggerRegistry::instance().loggerLevel("sample.retired").has_value(), "registerLogger 之后能从注册表取到它");

        // 先把裸引用留住：注册表给出的是引用，退休表护住的正是「注销时调用方还攥着它」这一刻
        Base::Logger &retained = Base::LoggerRegistry::instance().getLogger("sample.retired");
        static_cast<void>(retained.flush());
        const std::size_t lineCountBeforeRemoval = countLines(path);

        Base::LoggerRegistry::instance().unregisterLogger("sample.retired");
        // 注销当场把 Sink 交还（退休表只留日志器外壳），所以这个引用还能安全调用，但写入不再落盘。
        // 只断言「删得掉」在 POSIX 上等于没有断言——那里文件开着也能删，必须再看落盘结果
        LOG_LOGGER_INFO_FMT(retained, "注销后的一条记录");
        static_cast<void>(retained.flush());
        Samples::checklist().check(countLines(path) == lineCountBeforeRemoval, "注销后攥着的引用仍可用，但 Sink 已随注销交还、不再落盘");

        std::error_code removalError;
        std::filesystem::remove(path, removalError);
        Samples::checklist().check(!removalError, "unregisterLogger 当场释放了文件句柄（Windows 上否则删不掉）");
        Base::LoggerRegistry::instance().purgeRetiredLoggers();
    }
} // namespace

int main()
{
    Samples::setupConsoleLogging();

    const auto directory = std::filesystem::temp_directory_path() / ("asyn-sample-base-log-" + std::to_string(Platform::ProcessInfo::currentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    LOG_INFO("=== Base 日志子系统示例开始 ===");
    demonstrateRegistry();
    demonstrateFileSink(directory);
    demonstrateRollingSink(directory);
    demonstrateAsyncSink(directory);
    demonstrateFormatters(directory);
    demonstrateExceptionLogging(directory);
    demonstrateConfigDrivenSetup(directory);
    // 上一步把 root 换成了「文件 sink + WARN 起」：不把控制台装回来，后面的步骤与结论就只剩
    // std::cout 那一行，看着像程序没跑完
    demonstrateLifetime(directory);

    // 收尾时把注册表清干净并释放句柄，临时目录才能删掉（Windows 上打开着的文件删不掉）
    Base::LoggerRegistry::instance().clear();
    Base::LoggerRegistry::instance().purgeRetiredLoggers();
    Base::ConfigManager::instance().clear();
    std::filesystem::remove_all(directory);

    return Samples::finishSample("base_log");
}
