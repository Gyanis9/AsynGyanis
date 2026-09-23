#include "Base/Log/Sinks/RollingFileSink.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/System/PlatformTime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 按时间滚动时，同一周期备份名冲突的最大试探次数
        constexpr int kMaximumSuffixCollisions = 1000;

        /// 一天的秒数，用于推算下一个整日边界
        constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;

        /// 一小时的秒数，用于推算下一个整点边界
        constexpr std::int64_t kSecondsPerHour = 60 * 60;

        /// 活动文件打不开之后的重试间隔：让出锁与系统调用的同时保证故障自愈，取值与
        /// FileSink 那条「重新打开该文件后恢复」的指引相称——占用类故障通常在秒级内消失
        constexpr std::chrono::seconds kReopenRetryInterval{1};

        /**
         * @brief 清理时使用的备份条目：路径与预先读好的时间戳
         */
        struct BackupEntry
        {
            std::filesystem::path           path;      ///< 备份文件路径
            std::filesystem::file_time_type writeTime; ///< 预先取好的最后写入时间
        };

        /// 路径的字符串刻度：Windows 上是 wstring，POSIX 上是 string。文件名一律按这个刻度拼与比，
        /// 换成窄串会经过本地代码页——落在代码页外的字符要么变成 '?'（写到改了名的文件上），
        /// 要么直接抛出（清理与滚动路径上多一次不该有的失败）
        using PathText = std::filesystem::path::string_type;

        /// 路径刻度下的字符串视图：目录扫描里免掉逐条临时串
        using PathView = std::basic_string_view<PathText::value_type>;

        /**
         * @brief 把只含 ASCII 的文本换成路径刻度
         * @details 序号与时间后缀都由数字、点、连字符、下划线组成，宽窄两种刻度逐位等同，
         *          因此这条转换不经过任何编码，也就不会失败。
         * @param text 只含 ASCII 的文本
         * @return PathText 路径刻度下的同一段文本
         */
        [[nodiscard]] PathText pathTextFromAscii(const std::string &text)
        {
            return PathText(text.begin(), text.end());
        }

        /**
         * @brief 把基础文件名切成「主名 + 含点号扩展名」两段
         * @details 走 path 自己的 stem/extension 口径，且活动名、备份名、清理识别三处共用这一处：
         *          两处口径不一致就会认不出自己产出的备份，max_backup 形同虚设。
         * @param baseFilename 基础文件名
         * @return std::pair<PathText, PathText> 主名与扩展名（无扩展名时第二段为空）
         */
        [[nodiscard]] std::pair<PathText, PathText> splitBaseFilename(const std::filesystem::path &baseFilename)
        {
            return {baseFilename.stem().native(), baseFilename.extension().native()};
        }

        /**
         * @brief 按「主名.中段.扩展名」拼出活动文件名或备份文件名
         * @param namePart 主名（路径刻度）
         * @param middle 只含 ASCII 的中段：大小策略的序号、周期策略的时间后缀（可再带冲突序号）
         * @param extensionPart 含点号的扩展名（路径刻度），可为空
         * @return PathText 不含目录的文件名
         */
        [[nodiscard]] PathText joinDottedName(const PathText &namePart, const std::string &middle, const PathText &extensionPart)
        {
            PathText filename = namePart;
            filename += pathTextFromAscii("." + middle);
            filename += extensionPart;
            return filename;
        }

        /**
         * @brief 判断备份名中段的字符是否只可能是「序号或时间戳」
         * @param character 中段的单个字符
         * @return true 属于数字、点、连字符、下划线之一
         */
        [[nodiscard]] bool isBackupMiddleCharacter(const PathText::value_type character) noexcept
        {
            return (character >= '0' && character <= '9') || character == '.' || character == '-' || character == '_';
        }

        /**
         * @brief 按大小滚动前把已有备份整体向后顺移一位，为空出 1 号位
         * @param directory 日志目录
         * @param namePart 去掉扩展名的基础文件名
         * @param extensionPart 含点号的扩展名，无扩展名时为空
         * @param maximumBackupFiles 允许保留的备份数量上限
         * @note 必须从最大序号倒序移动，正序会把后一个备份直接覆盖
         */
        void rotateSizeBackups(const std::filesystem::path &directory, const PathText &namePart, const PathText &extensionPart,
                               const std::size_t maximumBackupFiles)
        {
            // 序号一路用 std::size_t 走到底：转成 int 会在上限以上回绕成负数，于是整个顺移循环
            // 一步不跑，后面的 rename 直接把 1 号备份盖掉——保留 N 份配置实际只剩 1 份
            const std::size_t highestIndex = maximumBackupFiles == 0 ? 1U : maximumBackupFiles;
            for (std::size_t index = highestIndex; index >= 1U; --index)
            {
                std::error_code errorCode;
                const std::filesystem::path sourcePath = directory / joinDottedName(namePart, std::to_string(index), extensionPart);
                if (!std::filesystem::exists(sourcePath, errorCode) || errorCode)
                {
                    continue;
                }
                // 顺移失败时保持原文件不动，后续 cleanupOldFiles 仍会按上限收敛
                const std::filesystem::path targetPath = directory / joinDottedName(namePart, std::to_string(index + 1), extensionPart);
                std::filesystem::rename(sourcePath, targetPath, errorCode);
            }
        }
    } // namespace

    RollingFileSink::RollingFileSink(std::filesystem::path baseFilename,
                                     std::filesystem::path directory,
                                     const RollingPolicy   policy,
                                     const size_t          maximumSizeBytes,
                                     const size_t          maximumBackupFiles) :
        // 备份数上限自行钳制：它同时决定每次滚动要探测多少个序号，配置侧虽已夹过一道，
        // 但本类是公开可构造的，不能把「不会被卡死」的责任推给调用方
        // 只取文件名段（构造参数的注释对此已有承诺）：带目录段的 base_filename 会把活动文件写到
        // 那个目录里去（operator/ 遇到绝对路径的右操作数会整体替换左操作数），而备份与清理都在
        // m_directory 下按「主名.序号.扩展名」找——两半不在同一目录时滚动照样发生，备份却永远清不掉
        m_baseFilename(baseFilename.filename()), m_directory(std::move(directory)), m_policy(policy), m_maximumSizeBytes(maximumSizeBytes),
        m_maximumBackupFiles(std::min(maximumBackupFiles, kMaximumBackupFileCount))
    {
        // error_code 重载：目录创建失败时不让 std::filesystem_error 从构造路径逃逸，
        // 随后的 FileSink 打开文件会失败并抛出带路径的中文异常，定位信息更准确
        std::error_code directoryError;
        std::filesystem::create_directories(m_directory, directoryError);

        if (m_policy == RollingPolicy::Daily || m_policy == RollingPolicy::Hourly)
        {
            m_currentSuffix      = generateTimestampSuffix();
            m_nextPeriodBoundary = nextPeriodBoundary(std::time(nullptr));
        }
        reopenActiveFile();
    }

    RollingFileSink::~RollingFileSink()
    {
        RollingFileSink::flush();
    }

    void RollingFileSink::write(const LogEvent &event)
    {
        std::lock_guard lock(m_mutex);
        // 活动文件为空只可能来自上一次重开失败（目标被杀软/备份代理短暂独占、磁盘写满、网络盘
        // 失联）。不在这里补一次重开就再没有触发点：按大小的滚动判据要求活动文件非空，于是本
        // Sink 会一声不响地永久停产。限流到每秒一次；仍开不开照旧抛出，由 Logger 的 Sink
        // 异常上报路径出声，故障期间的丢弃量因此可见而不是不可见
        if (!m_currentSink)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < m_nextReopenAttempt)
            {
                return;
            }
            m_nextReopenAttempt = now + kReopenRetryInterval;
            reopenActiveFile();
        }
        checkAndRoll();
        // 用本 Sink 自己的 formatter 把版式渲进复用的行缓冲，再交给活动文件的落盘与加锁逻辑：
        // 格式化器造一个结果串要取两次堆，而这里连字节累计都只用返回的长度，无需每行 flush + stat
        m_lineBuffer.clear();
        formatEventInto(m_lineBuffer, event);
        m_bytesInCurrentFile += m_currentSink->writeLine(m_lineBuffer);
    }

    void RollingFileSink::flush()
    {
        std::lock_guard lock(m_mutex);
        if (m_currentSink)
        {
            m_currentSink->flush();
        }
    }

    void RollingFileSink::checkAndRoll()
    {
        bool shouldRoll = false;
        if (m_policy == RollingPolicy::Daily || m_policy == RollingPolicy::Hourly)
        {
            // 每行只做一次 time_t 比较；只有跨过周期边界才做本地时间转换与后缀格式化。
            // 用后缀字符串是否变化作为二次判据：夏令时切换等情况下边界可能估算偏差一小时，
            // 此时后缀不变即不滚动，并把边界推到下一个周期，自然收敛
            if (const std::time_t now = std::time(nullptr); now >= m_nextPeriodBoundary)
            {
                m_nextPeriodBoundary = nextPeriodBoundary(now);
                if (const std::string newSuffix = generateTimestampSuffix(); newSuffix != m_currentSuffix)
                {
                    shouldRoll      = true;
                    m_currentSuffix = newSuffix;
                }
            }
        }
        if (m_policy == RollingPolicy::Size && m_currentSink && m_bytesInCurrentFile >= m_maximumSizeBytes)
        {
            shouldRoll = true;
        }
        if (shouldRoll)
        {
            // 先关闭当前文件（Windows 不允许重命名打开中的文件）
            m_currentSink.reset();
            const auto currentPath = getCurrentFilename();
            if (std::error_code existsError; std::filesystem::exists(currentPath, existsError) && !existsError)
            {
                const auto [namePart, extensionPart] = splitBaseFilename(m_baseFilename);
                PathText   backupName;
                if (m_policy == RollingPolicy::Size)
                {
                    // 已有备份整体向后顺移一位，空出 .1，避免序号耗尽后覆盖最旧备份
                    rotateSizeBackups(m_directory, namePart, extensionPart, m_maximumBackupFiles);
                    backupName = joinDottedName(namePart, "1", extensionPart);
                } else
                {
                    // 同一周期内已有备份（例如进程重启后再次滚动）时追加序号，不覆盖历史内容。
                    // 候选名逐个问到底：旧写法在数到第 kMaximumSuffixCollisions 个之前退出，那一个
                    // 的名字从没被验过就被拿去改名，把「用满了」变成悄悄覆盖。探测一律用 error_code
                    // 重载——这条路径跑在 Sink 的写线程上，抛出来只会让本轮滚动半途而废（活动文件已
                    // 关、重开被跳过），而探测失败真要变成改名失败时，下面那条 rename 的诊断会接手
                    std::error_code probeError;
                    bool          isTargetFree = false;
                    for (int candidateIndex = 1; candidateIndex <= kMaximumSuffixCollisions; ++candidateIndex)
                    {
                        // 第 1 个候选是不带序号的名字，其后依次追加 .2、.3 ……与旧的取名口径逐字相同
                        backupName = candidateIndex == 1
                                         ? joinDottedName(namePart, m_currentSuffix, extensionPart)
                                         : joinDottedName(namePart, m_currentSuffix + "." + std::to_string(candidateIndex),
                                                         extensionPart);
                        const bool isTaken = std::filesystem::exists(m_directory / backupName, probeError);
                        if (probeError)
                        {
                            // 问不出「有没有」就不必再往后猜：后面的候选同样问不出，而本轮滚动必须落定一个名字
                            break;
                        }
                        if (!isTaken)
                        {
                            isTargetFree = true;
                            break;
                        }
                    }
                    if (!isTargetFree && !probeError)
                    {
                        // 全被占：这是崩溃循环里同一周期反复滚动的现场，覆盖掉的那一段历史不会再有第二份
                        std::cerr << "RollingFileSink：同一周期的备份名已用满 " << kMaximumSuffixCollisions
                                << " 个，本次滚动覆盖 " << Platform::FileSystem::utf8FromPath(m_directory / backupName)
                                << "，那一段日志丢失" << '\n';
                    }
                }

                std::error_code renameError;
                std::filesystem::rename(currentPath, m_directory / backupName, renameError);
                if (renameError)
                {
                    // 这里的故障只能写 std::cerr：本 Sink 自己就是日志的出口，拿根日志器报这一条等于
                    // 让 write() 递归回自己。清空当前文件是既定的兜底——不清则滚动条件恒成立，
                    // 活动日志会在原地无限增长；但「日志被清掉了」这件事必须出声，否则现场只看得到
                    // 文件在反复变空
                    std::cerr << "RollingFileSink：滚动失败（" << Platform::FileSystem::utf8FromPath(currentPath) << " 改名为 "
                            << Platform::FileSystem::utf8FromPath(m_directory / backupName) << "）：" << renameError.message()
                            << "；已清空当前文件以免滚动条件恒成立，这一段日志随之丢弃" << '\n';
                    std::error_code truncateError;
                    std::filesystem::resize_file(currentPath, 0, truncateError);
                    if (truncateError)
                    {
                        // 连清空也没做到：滚动条件会一直成立，每条日志都要再付一次失败滚动的代价
                        std::cerr << "RollingFileSink：清空当前文件也失败：" << truncateError.message()
                                << "；滚动条件持续成立，每条日志都会重试一次失败的滚动" << '\n';
                    }
                }
            }
            reopenActiveFile();
            cleanupOldFiles();
        }
    }

    void RollingFileSink::reopenActiveFile()
    {
        const auto currentPath = getCurrentFilename();
        m_currentSink          = std::make_unique<FileSink>(currentPath);

        // 追加模式下目标文件可能已存在（同一周期的活动文件、进程重启后的续写），
        // 累计字节数必须从真实大小起算，否则按大小滚动会推迟到超过阈值一倍以上
        std::error_code      sizeError;
        const std::uintmax_t existingSize = std::filesystem::file_size(currentPath, sizeError);
        m_bytesInCurrentFile              = sizeError ? 0 : existingSize;
    }

    std::filesystem::path RollingFileSink::getCurrentFilename() const
    {
        if (m_policy == RollingPolicy::Size)
        {
            return m_directory / m_baseFilename;
        }
        // 周期策略把时间后缀插在扩展名之前（app.log -> app.2026-09-22.log），与备份名同一套拼法，
        // 否则清理那侧按前缀认不出自己的产物
        const auto [namePart, extensionPart] = splitBaseFilename(m_baseFilename);
        return m_directory / joinDottedName(namePart, m_currentSuffix, extensionPart);
    }

    std::string RollingFileSink::generateTimestampSuffix() const
    {
        const auto    now       = std::chrono::system_clock::now();
        const auto    timeValue = std::chrono::system_clock::to_time_t(now);
        const std::tm localTime = AsynGyanis::Platform::PlatformTime::localTime(timeValue);
        if (m_policy == RollingPolicy::Daily)
        {
            return std::format("{:04d}-{:02d}-{:02d}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday);
        }
        return std::format("{:04d}-{:02d}-{:02d}_{:02d}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour);
    }

    std::time_t RollingFileSink::nextPeriodBoundary(const std::time_t timeValue) const noexcept
    {
        const std::tm      localTime     = AsynGyanis::Platform::PlatformTime::localTime(timeValue);
        const bool         isDailyPolicy = m_policy == RollingPolicy::Daily;
        const std::int64_t periodSeconds = isDailyPolicy ? kSecondsPerDay : kSecondsPerHour;

        // 当前周期内已过的秒数：整日策略看时分秒，整点策略只看分秒
        const std::int64_t elapsedSeconds = isDailyPolicy
                                                ? static_cast<std::int64_t>(localTime.tm_hour) * kSecondsPerHour +
                                                  localTime.tm_min * 60 + localTime.tm_sec
                                                : static_cast<std::int64_t>(localTime.tm_min) * 60 + localTime.tm_sec;

        // elapsedSeconds == 0 时结果恰为 timeValue + periodSeconds，因此边界恒严格晚于当前时刻，
        // 同一周期内不会重复触发格式化
        return timeValue + static_cast<std::time_t>(periodSeconds - elapsedSeconds);
    }

    void RollingFileSink::cleanupOldFiles() const
    {
        // m_maximumBackupFiles == 0 表示不保留任何备份，备份列表仍需要构建并全部清理
        std::vector<BackupEntry> backupFiles;
        const auto               [namePart, extensionPart] = splitBaseFilename(m_baseFilename);
        const PathText           activeName                = getCurrentFilename().filename().native();

        // 前缀只构造一次，比较用 view：逐目录项拼临时串会把整目录扫描变成分配热点
        const PathText backupPrefix     = namePart + pathTextFromAscii(".");
        const PathView backupPrefixView{backupPrefix};
        const PathView extensionView{extensionPart};

        for (std::error_code errorCode; const auto &entry: std::filesystem::directory_iterator(m_directory, errorCode))
        {
            if (errorCode)
            {
                break;
            }
            const PathText filename     = entry.path().filename().native();
            const PathView filenameView{filename};
            // 备份名只有两种形态：`name.N.ext`（大小策略的序号备份）与 `name.<时间戳>[.N].ext`
            // （周期策略，时间戳形如 2026-09-16 或 2026-09-16_07，本身带连字符与下划线）。
            // 因此中间那段只允许数字、点、连字符与下划线：只按前缀匹配会把 app.audit.log 这类
            // 同前缀的无关文件也扫进删除区间，那是数据丢失；而不认 `-`/`_` 会让周期备份
            // 永远清不掉——max_backup 形同虚设，日志目录无界增长
            const bool hasBackupPrefix = filename != activeName && filenameView.starts_with(backupPrefixView) &&
                                         filenameView.ends_with(extensionView);
            if (!hasBackupPrefix)
            {
                continue;
            }
            // 前缀与后缀合起来可能比文件名本身还长：大小策略留下的 app.1.log 交给按天策略清理时
            // 就是这种形状（长度 7 小于前缀 4 加后缀 4）。先挡掉再算中段，否则下面的长度减法
            // 会回绕成天量、读到串尾之外
            if (filename.size() < backupPrefixView.size() + extensionView.size())
            {
                continue;
            }
            const PathView middlePart{filename.data() + backupPrefixView.size(),
                                      filename.size() - backupPrefixView.size() - extensionView.size()};
            const bool     isBackupName = !middlePart.empty() && std::ranges::all_of(middlePart, isBackupMiddleCharacter);
            if (isBackupName)
            {
                // 时间戳在排序前一次性读好：比较器里再调 last_write_time 会在出错时抛异常，
                // 而 std::ranges::sort 的比较器抛出是未定义行为
                std::error_code timeError;
                const auto      writeTime = std::filesystem::last_write_time(entry.path(), timeError);
                backupFiles.push_back(BackupEntry{.path = entry.path(), .writeTime = timeError ? std::filesystem::file_time_type::min() : writeTime});
            }
        }
        if (backupFiles.size() > m_maximumBackupFiles)
        {
            // 取不到时间戳的条目标记为 file_time_type::min()，排序时视为最旧、优先被清理
            std::ranges::sort(backupFiles,
                              [](const BackupEntry &left, const BackupEntry &right)
                              {
                                  return left.writeTime > right.writeTime;
                              });
            for (size_t index = m_maximumBackupFiles; index < backupFiles.size(); ++index)
            {
                std::error_code removeErrorCode;
                std::filesystem::remove(backupFiles[index].path, removeErrorCode);
            }
        }
    }
} // namespace AsynGyanis::Base
