#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigDocument.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/System/ProcessInfo.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 从一批配置文件推导配置目录
         * @details 全部文件同处一个目录时才返回它；出现相对路径或跨目录时返回空路径，
         *          表示不猜测一个并不存在的配置目录，由调用方保留既有取值。
         * @param filePaths 成功加载的配置文件路径
         * @return std::filesystem::path 公共父目录，无法判定时为空路径
         */
        [[nodiscard]] std::filesystem::path commonParentDirectory(const std::vector<std::filesystem::path> &filePaths)
        {
            std::filesystem::path commonDirectory;
            for (const auto &filePath: filePaths)
            {
                // 相对路径一律不当锚点：锚点会被 enableHotReload 与之后每一次 reload() 按
                // 当时的当前工作目录重新解析，进程一旦换过工作目录（daemonize、示例或测试改
                // 目录），重读的就不是同一批文件，而配置目录看着「已设置」，失败只报在日志里
                if (!filePath.is_absolute())
                {
                    return {};
                }
                const std::filesystem::path parentDirectory = filePath.parent_path();
                if (parentDirectory.empty())
                {
                    return {};
                }
                if (commonDirectory.empty())
                {
                    commonDirectory = parentDirectory;
                } else if (commonDirectory != parentDirectory)
                {
                    return {};
                }
            }
            return commonDirectory;
        }

        /**
         * @brief 把整数或浮点值写成十进制文本
         * @details 用 std::to_chars 写进栈上缓冲再一次性构造字符串：不受 locale 影响，
         *          也不像 std::to_string/std::ostringstream 那样多次扩容。
         * @tparam Number 算术类型
         * @param value 待文本化的数值
         * @return std::string 数值文本；缓冲区不足时返回空串
         */
        template<typename Number>
        [[nodiscard]] std::string numberToText(const Number value)
        {
            // 64 字节：任何 int64/uint64 的十进制都不超过 20 位，double 的最短往返形式也不到 30 位，
            // 留出一倍余量是为了让「写不下」这条分支在真实数值上根本不出现
            std::array<char, 64> buffer{};
            const auto [out, errorCode] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            if (errorCode != std::errc())
            {
                return {};
            }
            return {buffer.data(), static_cast<std::string::size_type>(out - buffer.data())};
        }

        /// 还原嵌套对象时的一条「剩余点分路径 → 取值」记录
        struct SectionEntry
        {
            std::string_view   remainingPath; ///< 剥掉段前缀后剩下的点分路径
            const ConfigValue *value;         ///< 该键的取值，指向调用方持有的快照
        };

        /**
         * @brief 把「剩余点分路径 → 取值」重新聚成嵌套对象
         * @details 与 flattenValue() 互为逆运算：首段相同的路径归到同一子对象下再递归，
         *          剩余路径里没有点号的即叶子。路径视图指向调用方容器里的字符串，
         *          该容器在整层递归期间一直存活。
         * @param entries 本层的全部条目
         * @param pathContext 本层键的完整前缀，仅用于把冲突点名到真实键（顶层传段名）
         * @return ConfigObject 本层对象
         * @throws ConfigValidationException 同一个名字在本层既配成标量、又是更长键的第一段
         */
        [[nodiscard]] ConfigObject buildNestedObject(const std::vector<SectionEntry> &entries, const std::string &pathContext)
        {
            ConfigObject object;

            // 用有序 map 归组：分组顺序本身不影响语义，但稳定的子对象键序让结果可断言、可阅读
            std::map<std::string_view, std::vector<SectionEntry>> childGroups;
            for (const SectionEntry &entry: entries)
            {
                const std::size_t separator = entry.remainingPath.find('.');
                if (separator == std::string_view::npos)
                {
                    object.insert_or_assign(std::string(entry.remainingPath), *entry.value);
                    continue;
                }
                childGroups[entry.remainingPath.substr(0, separator)].push_back(SectionEntry{entry.remainingPath.substr(separator + 1), entry.value});
            }

            for (const auto &[name, children]: childGroups)
            {
                const std::string groupPath = pathContext.empty() ? std::string(name) : pathContext + '.' + std::string(name);
                // 同一个名字既当标量又当分组时，必须报错而不是让分组悄悄吃掉标量：这种形状真的能
                // 由两份配置文件叠出来（一份给 a.b，另一份给 a.b.c），静默丢弃会让 getSection 与
                // getInt("a.b") 各说一套话
                if (object.contains(name))
                {
                    throw ConfigValidationException(groupPath, std::format("键 '{}' 既配成了标量、又是更长键的第一段（如 '{}.{}'）："
                                                                           "段落还原无法同时表达这两种形状，请把其中一条改成别的名字或换一层段落",
                                                                           groupPath, groupPath,
                                                                           std::string(children.front().remainingPath.substr(0, children.front().remainingPath.find('.')))));
                }
                object.insert_or_assign(std::string(name), ConfigValue(buildNestedObject(children, groupPath)));
            }
            return object;
        }

        /**
         * @brief 把路径转成 UTF-8 文本，供结果列表与诊断文案使用
         * @details 不能就地用 `path::string()`：Windows 上它按本地代码页转换，落在代码页外的字符
         *          直接抛 std::system_error，而本文件的转换点有四处就在 catch 块里——「构造一条报错」
         *          会把整次加载变成抛给调用方的异常。本文件所有对外的字符串通道（loadedFiles、
         *          failedFiles、errors）因此统一按 UTF-8 报，与配置文件内容的文本口径一致。
         * @param path 待报出的路径
         * @return std::string 该路径的 UTF-8 文本
         */
        [[nodiscard]] std::string pathText(const std::filesystem::path &path)
        {
            return AsynGyanis::Platform::FileSystem::utf8FromPath(path);
        }

        /**
         * @brief 取 key 在下一个点号之前的那段前缀（"a.b.c" 从 pos=1 起给出 "a"、"a.b"）
         * @param key 扁平键
         * @param searchFrom 从哪个下标开始找下一个点号
         * @return std::string_view 空前缀（没有点号了）时返回空视图
         */
        [[nodiscard]] std::string_view ancestorAt(const std::string &key, std::size_t &searchFrom) noexcept
        {
            const std::size_t dotPosition = key.find('.', searchFrom);
            if (dotPosition == std::string::npos)
            {
                searchFrom = std::string::npos;
                return {};
            }
            const std::string_view ancestor{key.data(), dotPosition};
            searchFrom = dotPosition + 1U;
            return ancestor;
        }

        /**
         * @brief 把一批新键并进累计表，同时清掉被这批键改掉形态的旧键
         * @details 同一棵文档树展开出的键互不重叠，程序侧一次也只写一个键，因此冲突只出现在
         *          「后来的写入把先前的一段表写成单个值」或反向。让两种形态并存等于把矛盾留给读的人：
         *          getSection() 只看得到 `<段名>.` 前缀下的那些键，段名自己那份值被静默吞掉；而它抛
         *          「既是值又是表」的那条路径又只在段内自撞时才算得出来。这里按「后写的说话」定形。
         *          报告以文本列表返回而不在这里落日志：调用方有的是持写锁的事务，锁内落日志等于把
         *          「写一条日志」的成本转嫁给所有并发写者。
         * @param values 累计表，新键并入其中
         * @param incomingValues 本次写入摊平出来的键（键与值都被搬空）
         * @param sourceLabel 本次写入的可读来源，只用于报错文本
         * @return std::vector<std::string> 每一次取代一条中文说明，无冲突时为空
         */
        [[nodiscard]] std::vector<std::string> mergeFileValues(ConfigKeyValueMap &values, ConfigKeyValueMap &&incomingValues, const std::string_view sourceLabel)
        {
            std::vector<std::string> conflicts;

            // 方向一：新键写在某个旧叶子下面（旧 server 是值、本次写 server.port）→ 淘汰那个旧值。
            // 只按新键往上查祖先，代价是键的深度而不是表的大小；查中即擦，同段的兄弟键因此不会报第二遍
            for (const auto &entry: incomingValues)
            {
                for (std::size_t searchFrom = 0;;)
                {
                    const std::string_view ancestor = ancestorAt(entry.first, searchFrom);
                    if (ancestor.empty())
                    {
                        break;
                    }
                    const auto staleLeaf = values.find(ancestor);
                    if (staleLeaf == values.end())
                    {
                        continue;
                    }
                    conflicts.push_back(std::format("配置键冲突：键 {} 先前是一个值，{} 在它下面写了 {}；同一个键不能既是值又是表，已按后者取值，旧值丢弃", ancestor, sourceLabel,
                                                    entry.first));
                    values.erase(staleLeaf);
                }
            }

            // 方向二：新键把某段旧的表改写成了单个值 → 那段下面每个旧键都要让位。
            // 单次全表扫描（按本次写入而非按键的代价），淘汰按「取代它的那个新键」聚合，一条取代只报一次
            std::vector<std::pair<std::string, std::vector<std::string>>> shadowedLeaves;
            for (auto entry = values.begin(); entry != values.end();)
            {
                std::string_view shadowingLeaf;
                for (std::size_t searchFrom = 0;;)
                {
                    const std::string_view ancestor = ancestorAt(entry->first, searchFrom);
                    if (ancestor.empty())
                    {
                        break;
                    }
                    if (incomingValues.contains(ancestor))
                    {
                        shadowingLeaf = ancestor;
                        break;
                    }
                }
                if (shadowingLeaf.empty())
                {
                    ++entry;
                    continue;
                }

                const auto bucket = std::ranges::find(shadowedLeaves, shadowingLeaf, &std::pair<std::string, std::vector<std::string>>::first);
                if (bucket == shadowedLeaves.end())
                {
                    shadowedLeaves.emplace_back(std::string(shadowingLeaf), std::vector<std::string>{entry->first});
                } else
                {
                    bucket->second.push_back(entry->first);
                }
                entry = values.erase(entry);
            }
            for (const auto &[leaf, droppedKeys]: shadowedLeaves)
            {
                std::string droppedText;
                for (const auto &dropped: droppedKeys)
                {
                    droppedText += droppedText.empty() ? dropped : "、" + dropped;
                }
                conflicts.push_back(std::format("配置键冲突：键 {} 先前摊开成 {} 个键（{}），{} 把它写成了一个值；已按后者取值，先前那 {} 个键丢弃", leaf, droppedKeys.size(),
                                                droppedText, sourceLabel, droppedKeys.size()));
            }

            for (auto &[key, value]: incomingValues)
            {
                values.insert_or_assign(std::move(key), std::move(value));
            }
            return conflicts;
        }

        /**
         * @brief 并一批键并把取代说明逐条落到日志上
         * @param values 累计表
         * @param incomingValues 本次写入摊平出来的键
         * @param sourceLabel 本次写入的可读来源
         */
        void mergeAndReport(ConfigKeyValueMap &values, ConfigKeyValueMap &&incomingValues, const std::string_view sourceLabel)
        {
            for (const std::string &conflict: mergeFileValues(values, std::move(incomingValues), sourceLabel))
            {
                LOG_ERROR(conflict);
            }
        }
    } // namespace

    ConfigManager &ConfigManager::instance() noexcept
    {
        static ConfigManager instance;
        return instance;
    }

    ConfigLoadResult ConfigManager::loadFromDirectory(const std::filesystem::path &configDirectory, const bool recursive)
    {
        return loadFromDirectoryImplementation(configDirectory, recursive);
    }

    ConfigLoadResult ConfigManager::loadFiles(const std::vector<std::filesystem::path> &filePaths)
    {
        ConfigLoadResult result;
        result.timestamp = std::chrono::steady_clock::now();

        if (filePaths.empty())
        {
            result.success = true;
            return result;
        }

        ConfigKeyValueMap                  values;
        std::vector<std::filesystem::path> loadedPaths;
        for (const auto &filePath: filePaths)
        {
            // 目录扫描那条路已全程走 error_code 重载，这里是文件清单路径上最后一处会抛
            // filesystem_error 的查询：路径查不了时同样落进结果清单，而不把异常交给调用方
            std::error_code existsError;
            if (const bool fileExists = std::filesystem::exists(filePath, existsError); existsError || !fileExists)
            {
                if (existsError)
                {
                    result.errors.push_back("无法检查文件是否存在：" + pathText(filePath) + "（" + existsError.message() + "）");
                } else
                {
                    result.errors.push_back("文件不存在：" + pathText(filePath));
                }
                result.failedFiles.push_back(pathText(filePath));
                continue;
            }
            if (!isConfigFile(pathText(filePath)))
            {
                result.failedFiles.push_back(pathText(filePath));
                result.errors.push_back("不支持的配置文件格式（应为 .json、.yaml 或 .yml）：" + pathText(filePath));
                continue;
            }
            // 每份文件先摊进自己那张临时表，成功了才并进来：与按目录加载共用同一份契约
            // （「失败文件里的键从快照中消失」）。直接往累计表里摊的话，解析中途失败时
            // 那份已经展开的半截键会跟着别份文件一起被提交
            ConfigKeyValueMap fileValues;
            if (loadConfigFile(filePath, fileValues, result.errors))
            {
                result.loadedFiles.push_back(pathText(filePath));
                loadedPaths.push_back(filePath);
                mergeAndReport(values, std::move(fileValues), std::format("文件 '{}'", pathText(filePath)));
            } else
            {
                result.failedFiles.push_back(pathText(filePath));
            }
        }

        // 至少一个文件成功才提交，全部失败时保留原有配置
        if (!result.loadedFiles.empty())
        {
            // 显式文件列表同样要留下配置目录，否则后续 reload() 与 enableHotReload() 失去依据：
            // 能推导出公共父目录时采用它，否则保留既有取值不覆盖。
            const auto            previousData      = m_data.load(std::memory_order_acquire);
            std::filesystem::path directoryToCommit = previousData->configDirectory;
            if (const std::filesystem::path derivedDirectory = commonParentDirectory(loadedPaths); !derivedDirectory.empty())
            {
                directoryToCommit = derivedDirectory;
            }
            // 锚点没换目录时沿用快照原有的递归口径：显式列表本身没有「递归与否」，把口径写成 true
            // 等于让同一个目录在无人改文件时换了一副面孔——先 loadFromDirectory(dir, false) 再
            // loadFiles({dir/a.yaml})，随后的 reload() 会凭空多出子目录里的键。
            // 锚点确实是新目录时才取默认值 true（旧口径属于另一棵树，跟不过来）
            const bool recursiveToCommit = directoryToCommit == previousData->configDirectory ? previousData->configDirectoryRecursive : true;
            commitConfigData(std::move(values), result.loadedFiles, directoryToCommit, recursiveToCommit);
        }
        result.success = result.failedFiles.empty() && !result.loadedFiles.empty();
        return result;
    }

    ConfigLoadResult ConfigManager::reload()
    {
        const auto currentData = m_data.load(std::memory_order_acquire);
        if (currentData->configDirectory.empty())
        {
            ConfigLoadResult result;
            result.success = false;
            result.errors.emplace_back("尚未设置配置目录，请先调用 loadFromDirectory 或 loadFiles。");
            return result;
        }
        return doReload();
    }

    bool ConfigManager::enableHotReload(HotReloadCallback callback, const std::chrono::milliseconds debounceMilliseconds)
    {
        // 控制面全程持锁：m_fileWatcher 是普通 unique_ptr，与 disableHotReload 并发读写就是数据竞态
        // （原子量只保护 enabled 这一个布尔，护不住监视器对象本身）
        const std::lock_guard controlLock(m_hotReloadControlMutex);

        // 回调快照在判定「是否已经启用」之前就发布：重复调用也必须把调用方新给的接线换上，
        // 只认第一次那份等于「返回 true 却没接上」。读侧是重载线程，靠这份原子快照配
        // release/acquire 看到完整对象（与启动监听那条同一套发布方式）
        m_hotReloadCallback.store(std::make_shared<const HotReloadCallback>(std::move(callback)), std::memory_order_release);

        if (bool expected = false; !m_hotReloadEnabled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            // 已在监听：防抖间隔跟着这一轮的入参换掉。FileWatcher 里那是原子量，运行中可写
            if (m_fileWatcher)
            {
                m_fileWatcher->setDebounceInterval(debounceMilliseconds);
            }
            return true; // 已经启用
        }

        const auto currentData = m_data.load(std::memory_order_acquire);
        if (currentData->configDirectory.empty())
        {
            // 四条失败路各自报明原因：只回一个 false 等于让运维去猜「是没设锚点、还是监视起不来」，
            // 这两件事的修法完全不同。显式文件列表用相对路径时锚点推不出来，也归在这一条里
            LOG_ERROR_FMT("ConfigManager：enableHotReload 失败，尚未设置配置目录锚点；"
                          "请用 loadFromDirectory 或绝对路径的 loadFiles 加载（相对路径推不出稳定的锚点目录）");
            m_hotReloadEnabled.store(false, std::memory_order_release);
            return false;
        }

        try
        {
            m_fileWatcher = Platform::FileWatcher::create();
            if (!m_fileWatcher)
            {
                LOG_ERROR_FMT("ConfigManager：enableHotReload 失败，FileWatcher::create() 返回空（本平台无法创建文件监视器）");
                m_hotReloadEnabled.store(false, std::memory_order_release);
                return false;
            }

            m_fileWatcher->setDebounceInterval(debounceMilliseconds);

            m_fileWatcher->setCallback([this](const std::string_view filePath, const Platform::FileChangeType changeType) { handleFileChange(filePath, changeType); });

            // 监听范围与 reload 的重扫范围必须同一个口径：只递归挂监听却按非递归重扫，
            // 子目录里的改动会白叫醒一轮重载；反之则子目录的改动根本进不到重扫里。
            // 交给监视器的是 UTF-8 文本：FileWatcher 的接口刻度已写明 UTF-8（Windows 上它的登记键与
            // 事件路径都由宽字符按 CP_UTF8 转出）。这里若换成 `.string()`，中文配置目录会被送去一份
            // 本地代码页的字节，注册那一步按 UTF-8 解不开而静默失败——热重载整条被关掉，现场只看得见
            // 「改了配置没反应」
            if (!m_fileWatcher->addWatch(pathText(currentData->configDirectory), currentData->configDirectoryRecursive))
            {
                LOG_ERROR_FMT("ConfigManager：enableHotReload 失败，给配置目录挂监视没成功：{}（目录可能已被删除、无权限，或系统监视句柄已用尽）",
                              pathText(currentData->configDirectory));
                m_fileWatcher.reset();
                m_hotReloadEnabled.store(false, std::memory_order_release);
                return false;
            }

            if (!m_fileWatcher->start())
            {
                LOG_ERROR_FMT("ConfigManager：enableHotReload 失败，文件监视线程起不来（配置目录 {}）", pathText(currentData->configDirectory));
                m_fileWatcher.reset();
                m_hotReloadEnabled.store(false, std::memory_order_release);
                return false;
            }

            return true;
        } catch (const std::exception &enableError)
        {
            LOG_ERROR_FMT("ConfigManager：enableHotReload 失败，装配监视器时抛出异常：{}", enableError.what());
            m_fileWatcher.reset();
            m_hotReloadEnabled.store(false, std::memory_order_release);
            return false;
        } catch (...)
        {
            LOG_ERROR_FMT("ConfigManager：enableHotReload 失败，装配监视器时抛出非标准异常");
            m_fileWatcher.reset();
            m_hotReloadEnabled.store(false, std::memory_order_release);
            return false;
        }
    }

    void ConfigManager::disableHotReload()
    {
        std::vector<std::unique_ptr<ReloadTask>> pendingTasks;
        {
            // 与 enableHotReload 共用同一把控制面锁，二者对 m_fileWatcher 的读写才互斥；
            // 这把锁总是最外层，监听线程的回调只碰 m_reloadTasksMutex，因此不会成环
            const std::lock_guard controlLock(m_hotReloadControlMutex);
            if (!m_hotReloadEnabled.load(std::memory_order_acquire))
            {
                return;
            }
            {
                const std::lock_guard lock(m_reloadTasksMutex);
                // 先把退出表容量留够：后面的搬移不再分配，唯一分配点排在状态切换之前
                pendingTasks.reserve(m_reloadTasks.size());
                if (!m_hotReloadEnabled.exchange(false, std::memory_order_acq_rel))
                {
                    return;
                }
                for (auto iterator = m_reloadTasks.begin(); iterator != m_reloadTasks.end();)
                {
                    // 重载回调里调 disableHotReload() 是合法用法（「连续几轮失败就别再监视了」），
                    // 而本轮任务此刻正跑在本线程上、finished 要到回调返回之后才置起。把它一起销毁
                    // 就是让 ~jthread 去 join 自己那一条线程：std::jthread 抛
                    // resource_deadlock_would_occur，而异常从析构里出来即 std::terminate。
                    // 原地跳过它，留待下一次 scheduleReload 或本对象析构时回收
                    if (*iterator && (*iterator)->thread.get_id() == std::this_thread::get_id())
                    {
                        ++iterator;
                        continue;
                    }
                    pendingTasks.push_back(std::move(*iterator));
                    iterator = m_reloadTasks.erase(iterator);
                }
            }
            if (m_fileWatcher)
            {
                m_fileWatcher->stop();
                m_fileWatcher.reset();
            }
        }
        // 这里的 join（pendingTasks 析构时隐式完成）是为了等仍活着的重载线程退出：它们可能
        // 还在 doReload() 或用户回调里，不等它们结束就让 disable 返回，调用方紧接着销毁配置
        // 管理器就会让这些线程继续访问已释放的对象（use-after-free）。join 放在控制锁之外：
        // 重载回调里再调 enable/disable 是合法用法，持锁 join 会让回调阻塞在这把非递归互斥量
        // 上，与 join 形成死锁。并发的第二个 disable 可能在本线程 join 完成之前就返回
        // （它看到 enabled 已为 false）
        pendingTasks.clear();
    }

    bool ConfigManager::isHotReloadEnabled() const noexcept
    {
        return m_hotReloadEnabled.load(std::memory_order_acquire);
    }

    ConfigValue ConfigManager::get(const std::string_view key) const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);

        const auto iterator = currentData->values.find(key);
        if (iterator == currentData->values.end())
        {
            throw ConfigKeyNotFoundException(std::string(key));
        }
        return iterator->second;
    }

    std::optional<ConfigValue> ConfigManager::getOptional(const std::string_view key) const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);

        const auto iterator = currentData->values.find(key);
        if (iterator == currentData->values.end())
        {
            return std::nullopt;
        }
        return iterator->second;
    }

    bool ConfigManager::getBool(const std::string_view key, bool defaultValue) const
    {
        return get<bool>(key, std::move(defaultValue));
    }

    int64_t ConfigManager::getInt(const std::string_view key, int64_t defaultValue) const
    {
        return get<int64_t>(key, std::move(defaultValue));
    }

    double ConfigManager::getDouble(const std::string_view key, double defaultValue) const
    {
        return get<double>(key, std::move(defaultValue));
    }

    std::string ConfigManager::getString(const std::string_view key, const std::string &defaultValue) const
    {
        // 按视图直查，命中且类型是字符串时只付一次返回拷贝：走 getOptional 要先把
        // optional<ConfigValue> 里的整棵子树深拷一份（这里只读一个标量），再从中取字符串；
        // 默认值形参是 const 引用，未命中时也只付这一次返回拷贝，不会先把默认值拷一遍。
        // 严格类型口径不变：只有字符串值才算取到，其余回落默认值
        const auto currentData = m_data.load(std::memory_order_acquire);
        const auto iterator    = currentData->values.find(key);
        if (iterator == currentData->values.end() || !iterator->second.is_string())
        {
            return defaultValue;
        }
        return iterator->second.get_ref<const std::string &>();
    }

    std::string ConfigManager::getText(const std::string_view key, const std::string &defaultValue) const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);
        // 直接以 string_view 查找：ConfigKeyValueMap 的透明哈希支持异构查找，
        // 构造临时 std::string 只会白白多一次分配
        const auto iterator = currentData->values.find(key);
        if (iterator == currentData->values.end())
        {
            return defaultValue;
        }

        // 宽松取值：字符串原样；数字/布尔文本化（免疫类型推断差异）。
        // 整数分两条：非负整数落 number_unsigned、负整数落 number_integer，都要覆盖
        switch (const ConfigValue &value = iterator->second; value.type())
        {
            case ConfigValueType::string:
                return value.get_ref<const std::string &>();
            case ConfigValueType::number_integer:
                return numberToText(value.get<std::int64_t>());
            case ConfigValueType::number_unsigned:
                return numberToText(value.get<std::uint64_t>());
            case ConfigValueType::boolean:
                return value.get<bool>() ? "true" : "false";
            case ConfigValueType::number_float:
                return numberToText(value.get<double>());
            default:
                return defaultValue;
        }
    }

    bool ConfigManager::setValue(const std::string_view key, ConfigValue value)
    {
        if (key.empty())
        {
            return false;
        }

        // 写侧串行化：本方法对快照执行「读取 → 复制 → 修改 → 发布」事务，
        // 两个并发写者若同时基于同一份旧快照构造新快照，后发布者会丢掉先发布者的键。
        // 这里只串行化写者之间；读者仍通过 atomic<shared_ptr> 无锁读取快照，不受影响
        std::shared_ptr<ConfigData> newData;
        std::vector<std::string>    shapeConflicts;
        {
            const std::lock_guard writeLock(m_writeMutex);

            // 复制当前快照并更新目标键，原子替换后立即对所有读者生效
            const auto currentData = m_data.load(std::memory_order_acquire);
            newData                = std::make_shared<ConfigData>(*currentData);

            // 与装载路径共用同一套定形规矩：只往快照里塞一个新键会让同一个名字既是值又是表，
            // 而 getSection() 只扫 `<段名>.` 前缀——刚设进去的那个值会被静默吞掉
            ConfigKeyValueMap incoming;
            incoming.emplace(std::string(key), std::move(value));
            shapeConflicts = mergeFileValues(newData->values, std::move(incoming), std::format("setValue(\"{}\")", key));
            // 程序侧写入的键不再是「来自环境变量」；定形冲突淘汰掉的覆盖键也不再计入。
            // 这一步只动拷贝出来的快照，读者仍看旧快照
            newData->environmentOverrideKeys.erase(std::remove_if(newData->environmentOverrideKeys.begin(), newData->environmentOverrideKeys.end(),
                                                                  [&newData, key](const std::string &overrideKey)
                                                                  { return std::string_view(overrideKey) == key || !newData->values.contains(overrideKey); }),
                                                   newData->environmentOverrideKeys.end());
            newData->environmentOverrideCount = newData->environmentOverrideKeys.size();
            m_data.store(newData, std::memory_order_release);
        }

        // 取代说明排在写锁之外，与下面的 schema 校验同一排法：锁内落日志等于把「写一条日志」
        // 的成本转嫁给所有并发写者
        for (const std::string &conflict: shapeConflicts)
        {
            LOG_ERROR(conflict);
        }

        // 校验与日志放在写锁之外，与 commitConfigData 同一排法：按违规做一次 std::format
        // 并走一次 Sink 写入，锁内做这些等于把「落一条日志」的成本转嫁给所有并发的写者。
        // 判的是自己刚发布的那份快照里的这个键，后来者替换快照也不影响这条报告的归属
        validateRegisteredSchema(newData->values, key);

        return true;
    }

    bool ConfigManager::has(const std::string_view key) const noexcept
    {
        const auto currentData = m_data.load(std::memory_order_acquire);
        return currentData->values.contains(key);
    }

    std::vector<std::string> ConfigManager::keys() const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);

        std::vector<std::string> result;
        result.reserve(currentData->values.size());
        for (const auto &key: currentData->values | std::views::keys)
        {
            result.push_back(key);
        }
        std::ranges::sort(result);
        return result;
    }

    ConfigKeyValueMap ConfigManager::dump() const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);
        return currentData->values;
    }

    ConfigValue ConfigManager::getSection(const std::string_view sectionPrefix) const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);

        std::string prefix(sectionPrefix);
        // 末尾必须带上分隔符：否则 "server" 会把 "serverSide.x" 这类同前缀段也捞进来
        prefix.push_back('.');

        std::vector<SectionEntry> entries;
        for (const auto &[key, value]: currentData->values)
        {
            if (!key.starts_with(prefix))
            {
                continue;
            }
            // 视图指向快照里的键，本次调用期间 currentData 一直持有，视图始终有效
            entries.push_back(SectionEntry{std::string_view(key).substr(prefix.size()), &value});
        }

        // 一段都没配属于正常情形：返回空对象让消费方走默认值，而不是抛异常
        if (entries.empty())
        {
            return ConfigValue(ConfigObject{});
        }
        return ConfigValue(buildNestedObject(entries, std::string(sectionPrefix)));
    }

    std::vector<std::string> ConfigManager::loadedFiles() const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);
        return currentData->loadedFiles;
    }

    std::filesystem::path ConfigManager::configDirectory() const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);
        return currentData->configDirectory;
    }

    void ConfigManager::clear()
    {
        // 清空同样是写者：不取锁的话，与并发的 setValue/加载谁后发布谁生效，
        // 一次 clear 可能把刚写进快照的键整份抹掉
        const std::lock_guard writeLock(m_writeMutex);

        const auto newData = std::make_shared<ConfigData>();
        m_data.store(newData, std::memory_order_release);
    }

    std::vector<std::string> ConfigManager::validateRequired(const std::vector<std::string> &requiredKeys) const
    {
        std::vector<std::string> missing;

        const auto currentData = m_data.load(std::memory_order_acquire);
        for (const auto &key: requiredKeys)
        {
            if (!currentData->values.contains(key))
            {
                missing.push_back(key);
            }
        }

        return missing;
    }

    ConfigValidationResult ConfigManager::validateSchema(const ConfigSchema &schema) const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);
        return runSchemaValidation(currentData->values, schema);
    }

    ConfigValidationResult ConfigManager::setSchema(ConfigSchema schema)
    {
        ConfigSchema registeredSchema;
        {
            const std::lock_guard lock(m_schemaMutex);
            m_schema         = std::move(schema);
            registeredSchema = m_schema;
        }

        // 注册后立即对当前快照校验一次，问题以日志形式暴露而不阻断运行
        const auto currentData = m_data.load(std::memory_order_acquire);
        const auto validation  = runSchemaValidation(currentData->values, registeredSchema);
        for (const auto &error: validation.errors)
        {
            LOG_ERROR_FMT("配置 schema 校验失败：{}", error);
        }
        return validation;
    }

    void ConfigManager::validateRegisteredSchema(const ConfigKeyValueMap &values, const std::string_view onlyKey) const
    {
        ConfigSchema targets;
        {
            const std::lock_guard lock(m_schemaMutex);
            if (onlyKey.empty())
            {
                // 整份快照提交：一次复制全表，随后逐条判
                targets = m_schema;
            } else
            {
                // 单键提交：只挑这个键的约束条目。整表复制在这里是白花力气——
                // setValue 一次只改一个键，而「别的必需键还没出现」不该由这次写入重复播报
                for (const auto &entry: m_schema)
                {
                    if (entry.key == onlyKey)
                    {
                        targets.push_back(entry);
                    }
                }
            }
        }
        if (targets.empty())
        {
            return;
        }

        // 针对给定字典逐项校验（不依赖当前快照，提交路径据此判自己刚发布的那一份）
        for (const auto &error: runSchemaValidation(values, targets).errors)
        {
            LOG_ERROR_FMT("配置 schema 校验失败：{}", error);
        }
    }

    ConfigManager::~ConfigManager()
    {
        disableHotReload();
    }

    ConfigLoadResult ConfigManager::loadFromDirectoryImplementation(const std::filesystem::path &configDirectory, const bool recursive)
    {
        ConfigLoadResult result;
        result.timestamp = std::chrono::steady_clock::now();

        std::error_code errorCode;
        if (!std::filesystem::exists(configDirectory, errorCode))
        {
            result.success = false;
            result.errors.push_back("配置目录不存在：" + pathText(configDirectory));
            if (errorCode)
            {
                result.errors.push_back("错误：" + errorCode.message());
            }
            return result;
        }

        if (!std::filesystem::is_directory(configDirectory, errorCode))
        {
            result.success = false;
            result.errors.push_back("路径不是目录：" + pathText(configDirectory));
            return result;
        }

        // 扫描本身可能中途失败（子目录不可读、条目属性取不到）。这时交回的不是一份
        // 少了些文件的清单，而是一次失败：按全量提交会让那些没扫到的文件的键静默消失
        const auto scanned = scanConfigFiles(configDirectory, recursive);
        if (!scanned.has_value())
        {
            result.success = false;
            result.errors.push_back(scanned.error());
            return result;
        }

        const auto &configFiles = *scanned;

        if (configFiles.empty())
        {
            // 目录中没有任何配置文件：提交空配置（等价于清空）。走与其余加载路径同一条提交通道，
            // 这样「快照换成空表」这件事同样会过一次已注册 schema，缺失的必需键不会被静默放过
            commitConfigData({}, {}, configDirectory, recursive);

            result.success = true;
            return result;
        }

        ConfigKeyValueMap values;

        for (const auto &filePath: configFiles)
        {
            // 每个文件先摊进自己那份临时表，成功了才并进来：解析中途失败（例如键里带点号）时，
            // 这个文件已经展开的那半份键绝不能跟着提交——契约是「失败的键从快照中消失」，
            // 而现在同目录还有别的文件成功，那半份配置就会被一起提交，比整份丢弃更危险
            ConfigKeyValueMap fileValues;
            if (loadConfigFile(filePath, fileValues, result.errors))
            {
                result.loadedFiles.push_back(pathText(filePath));
                mergeAndReport(values, std::move(fileValues), std::format("文件 '{}'", pathText(filePath)));
            } else
            {
                result.failedFiles.push_back(pathText(filePath));
            }
        }

        // 全部失败时保留原有配置快照
        if (result.loadedFiles.empty())
        {
            result.success = false;
            return result;
        }

        commitConfigData(std::move(values), result.loadedFiles, configDirectory, recursive);
        result.success = result.failedFiles.empty();
        return result;
    }

    bool ConfigManager::loadConfigFile(const std::filesystem::path &filePath, ConfigKeyValueMap &values, std::vector<std::string> &errors)
    {
        ConfigValue document;
        std::string parseError;
        if (!ConfigDocument::load(filePath, document, parseError))
        {
            errors.push_back(std::move(parseError));
            return false;
        }

        // 整份文档没有内容（只有注释行的 YAML、显式的 null）与空文件等价：不产出配置项，
        // 也不算坏文件。「放一份只写着说明的 overrides.yaml」是常见的发布做法，报它失败
        // 等于逼运维删掉那份说明
        if (document.is_null())
        {
            return true;
        }

        if (!document.is_object())
        {
            // 沿用 YAML 习惯措辞：数组报 sequence，标量按类型名
            const std::string_view kindName = document.is_array() ? std::string_view{"sequence"} : std::string_view{typeName(document.type())};

            errors.push_back("文件 '" + pathText(filePath) + "'：根节点必须是映射，实际为 " + std::string(kindName));
            return false;
        }

        try
        {
            std::string prefixBuffer;
            flattenValue(document, prefixBuffer, values);
        } catch (const std::exception &exception)
        {
            errors.push_back(std::format("解析错误：'{}'：{}", pathText(filePath), exception.what()));
            return false;
        }

        return true;
    }

    void ConfigManager::flattenValue(ConfigValue &node, std::string &prefixBuffer, ConfigKeyValueMap &values)
    {
        // 只展开对象节点：数组与标量整体作为叶子值；叶子值需要被移出节点（见下方 std::move），
        // 因此这里取成员表的可写引用
        if (!node.is_object())
        {
            return;
        }

        for (auto &[key, value]: node.get_ref<ConfigObject &>())
        {
            // 键里出现点号会让扁平路径产生歧义：`"a.b": 1` 与 `a: {b: 1}` 落成同一个键，
            // 后写的一方静默覆盖先写的一方——配置里出现这种键几乎总是笔误。直接拒绝并说清原因，
            // 好过让调用方拿着一份被悄悄改过的配置去跑（抛出由 loadConfigFile 统一收成加载错误）
            if (key.find('.') != std::string::npos)
            {
                throw std::runtime_error("配置键 '" + key +
                                         "' 含有分隔符 '.'：它会与嵌套写法落成同一个扁平路径（例如 a: {b: 1} 落成 a.b），"
                                         "两者互相覆盖且不易察觉；请改用嵌套写法，或去掉键里的点号");
            }

            // 空键段与带点号的键是同一类毛病，只是更隐蔽：递归进空键这一层时前缀仍是空串
            // （下面只在 prefixLength != 0 时补点号），于是子层的键与父层同名的键落成同一个扁平路径，
            // 谁后写谁赢；而只有空键本身时（`{"": 5}`）会往表里塞进一个空键，`has("")` 报有值、
            // `setValue("")` 却拒绝空键，那条记录就此只能读、永远改不动
            if (key.empty())
            {
                throw std::runtime_error("配置键不能为空：空键段会让嵌套与平铺写法塌缩成同一个扁平路径"
                                         "（例如 {\"\": {\"x\": 1}} 与 {\"x\": 2} 都落成 x），两者互相覆盖且不易察觉；"
                                         "请给这一层起个名字");
            }

            // 追加本层键后递归，返回时把缓冲回退到进入本层前的长度：
            // 整棵子树共用同一个前缀缓冲，路径字符只写一次而不是每层重新拼一遍父前缀
            const std::size_t prefixLength = prefixBuffer.size();
            if (prefixLength != 0)
            {
                prefixBuffer.push_back('.');
            }
            prefixBuffer += key;

            if (value.is_object() && !value.empty())
            {
                // 非空嵌套对象：递归展开为点号路径
                flattenValue(value, prefixBuffer, values);
            } else
            {
                // 叶子（标量、数组、空对象）：类型已由解析阶段判定完毕，值直接移出文档
                values.insert_or_assign(prefixBuffer, std::move(value));
            }

            prefixBuffer.resize(prefixLength);
        }
    }


    std::expected<std::vector<std::filesystem::path>, std::string> ConfigManager::scanConfigFiles(const std::filesystem::path &directory, const bool recursive)
    {
        std::vector<std::filesystem::path> configFiles;

        std::error_code errorCode;
        // 单个条目的属性查询也走 error_code 那一份重载：无 ec 的 is_regular_file() 会抛
        // filesystem_error，那会从「加载配置」里逃到调用方手上，而这里要的是一轮失败的重载
        const auto collect = [&configFiles, &errorCode](const auto &entry)
        {
            const bool isRegularFile = entry.is_regular_file(errorCode);
            if (!errorCode && isRegularFile && isConfigFile(pathText(entry.path())))
            {
                configFiles.push_back(entry.path());
            }
        };

        // 范围 for 的 operator++ 不带 error_code，走的是会抛的那一份重载：目录读到一半遇上
        // 不可读的子目录时，filesystem_error 会直接穿过这里逃到调用方。显式 increment(error_code)
        // 把构造与每一次递增的失败都留在 errorCode 里：构造失败时循环条件一开始就不成立，
        // 递增失败则让下一轮条件不成立，两种都交给循环之后那一次判定。
        // 属性查询失败则必须就地跳出：increment(error_code) 成功时会把 errorCode 清掉，
        // 那一处失败若跟着递增走一圈就没人报了
        if (recursive)
        {
            std::filesystem::recursive_directory_iterator       iterator(directory, errorCode);
            const std::filesystem::recursive_directory_iterator end;
            for (; !errorCode && iterator != end; iterator.increment(errorCode))
            {
                collect(*iterator);
                if (errorCode)
                {
                    break;
                }
            }
        } else
        {
            std::filesystem::directory_iterator       iterator(directory, errorCode);
            const std::filesystem::directory_iterator end;
            for (; !errorCode && iterator != end; iterator.increment(errorCode))
            {
                collect(*iterator);
                if (errorCode)
                {
                    break;
                }
            }
        }

        // 循环之外的这一次判定不能省：increment 在最后一个条目之后才失败时，循环因 errorCode
        // 非零而停止，错误只留在 errorCode 里；不在这里认领，这份少了几项的清单就会被当成全量
        if (errorCode)
        {
            return std::unexpected(std::format("扫描配置目录 '{}' 时中断（{}），本轮只扫到 {} 个配置文件："
                                               "不把这份不完整的清单当成全量提交，配置保持原样；"
                                               "请检查该目录及其子目录的读取权限后重试",
                                               pathText(directory), errorCode.message(), configFiles.size()));
        }

        std::ranges::sort(configFiles);
        return configFiles;
    }

    namespace
    {
        /**
         * @brief 只按 ASCII 把大写字母转小写
         * @details 不用 std::tolower：它受 locale 影响，土耳其语环境下 'I' 会折成无点的 ı，
         *          同一个环境变量名在不同机器的语言设置里折出不同的配置键，是查不出来的那类错。
         * @param character 输入字符
         * @return char 转换结果；非 ASCII 大写字母原样返回
         */
        [[nodiscard]] char toAsciiLower(const char character) noexcept
        {
            return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
        }

        /**
         * @brief 把去掉前缀的环境变量名折成配置键
         * @details 规则：`__`（两个连续下划线）是层级分隔符，段内的单个下划线原样保留，整段转 ASCII
         *          小写，于是 `SERVER__PORT` 对应 `server.port`。
         * @param body 去掉前缀之后的变量名
         * @return std::optional<std::string> 配置键；开头/结尾是分隔符这类映射不出合法键的写法返回空
         */
        [[nodiscard]] std::optional<std::string> configKeyFromEnvironmentSuffix(const std::string_view body)
        {
            std::string key;
            key.reserve(body.size());
            for (std::size_t index = 0; index < body.size(); ++index)
            {
                const char character = body[index];
                if (character == '_' && index + 1 < body.size() && body[index + 1] == '_')
                {
                    // 前导与尾随的分隔符都映射不出合法的段名，宁可不认也不造一个空段
                    if (key.empty() || key.back() == '.')
                    {
                        return std::nullopt;
                    }
                    key.push_back('.');
                    ++index;
                    continue;
                }
                key.push_back(toAsciiLower(character));
            }
            if (key.empty() || key.back() == '.')
            {
                return std::nullopt;
            }
            return key;
        }

        /**
         * @brief 按目标类型解释环境变量的文本值
         * @details 目标类型只从「文件里该键已有的类型」或「schema 声明」两处取，本函数不猜类型——
         *          `8080` 该存成整数还是字符串是使用方的语义，不是这一层能定的。对象与数组
         *          解析成 JSON 后还要核对顶层形态：对象项必须是对象、数组项必须是数组，
         *          `42` 这类解析得动但形态不符的取值同样拒绝。
         * @param text 环境变量原文
         * @param type 目标类型（对象与数组按 JSON 解析并核对形态，其余未知类型一律按字符串交出）
         * @param[out] error 转换失败时的中文说明，含原值
         * @return std::optional<ConfigValue> 转换结果；失败时为空
         */
        [[nodiscard]] std::optional<ConfigValue> coerceEnvironmentValue(const std::string_view text, const ConfigValueType type, std::string &error)
        {
            const auto reject = [&text, &error](const std::string &reason) -> std::optional<ConfigValue>
            {
                error = std::format("{}，原值 '{}' 不是合法写法", reason, text);
                return std::nullopt;
            };

            switch (type)
            {
                case ConfigValueType::boolean:
                {
                    std::string lowered;
                    lowered.reserve(text.size());
                    for (const char character: text)
                    {
                        lowered.push_back(toAsciiLower(character));
                    }
                    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on")
                    {
                        return ConfigValue(true);
                    }
                    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off")
                    {
                        return ConfigValue(false);
                    }
                    return reject("布尔项只认 1/0、true/false、yes/no、on/off");
                }
                case ConfigValueType::number_integer:
                {
                    long long parsed = 0;
                    if (const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed); ec == std::errc{} && ptr == text.data() + text.size())
                    {
                        return ConfigValue(parsed);
                    }
                    return reject("取值不是合法整数");
                }
                case ConfigValueType::number_unsigned:
                {
                    unsigned long long parsed = 0;
                    if (const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed); ec == std::errc{} && ptr == text.data() + text.size())
                    {
                        return ConfigValue(parsed);
                    }
                    return reject("取值不是合法非负整数");
                }
                case ConfigValueType::number_float:
                {
                    double parsed = 0.0;
                    if (const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed); ec == std::errc{} && ptr == text.data() + text.size())
                    {
                        return ConfigValue(parsed);
                    }
                    return reject("取值不是合法浮点数");
                }
                case ConfigValueType::object:
                case ConfigValueType::array:
                {
                    // 结构与列表只能按 JSON 收：`ASYN_SERVER__HOSTS=["a","b"]` 这种写法没有别的解释法
                    ConfigValue parsed;
                    try
                    {
                        parsed = ConfigValue::parse(std::string(text));
                    } catch (const std::exception &)
                    {
                        return reject("对象与数组项的值必须是合法 JSON");
                    }
                    // 合法 JSON 不等于形态对得上：`42` 也解析得动，但把数组顶成数字等于偷换类型
                    if (type == ConfigValueType::object && !parsed.is_object())
                    {
                        return reject("对象项的值必须是合法 JSON 对象");
                    }
                    if (type == ConfigValueType::array && !parsed.is_array())
                    {
                        return reject("数组项的值必须是合法 JSON 数组");
                    }
                    return parsed;
                }
                default:
                    return ConfigValue(std::string(text));
            }
        }

        /// 一条待应用的环境变量覆盖：原始变量名（报错点名）、折出的配置键与环境变量原文
        struct EnvironmentOverrideCandidate
        {
            std::string variableName;
            std::string key;
            std::string value;
        };

        /**
         * @brief 扫出带前缀的环境变量并折成候选键；名字映射不出键的逐条报错后跳过
         * @param prefix 环境变量前缀
         * @return std::vector<EnvironmentOverrideCandidate> 按环境枚举顺序排列的候选
         */
        [[nodiscard]] std::vector<EnvironmentOverrideCandidate> collectEnvironmentOverrideCandidates(const std::string &prefix)
        {
            std::vector<EnvironmentOverrideCandidate> candidates;
            for (const auto &[variableName, rawValue]: Platform::ProcessInfo::environmentVariablesWithPrefix(prefix))
            {
                const std::optional<std::string> key = configKeyFromEnvironmentSuffix(std::string_view(variableName).substr(prefix.size()));
                if (!key.has_value())
                {
                    // 名字畸形的那条直接出声：调用方以为自己在覆盖一个键，实际上什么都没发生
                    LOG_ERROR_FMT("ConfigManager：环境变量 '{}' 的名字映射不出合法配置键（'__' 是层级分隔符，不能出现在开头或结尾），本条覆盖被忽略", variableName);
                    continue;
                }
                candidates.push_back({variableName, *key, rawValue});
            }
            return candidates;
        }
    } // namespace

    void ConfigManager::setEnvironmentOverridePrefix(std::string prefix)
    {
        m_environmentPrefix.store(std::make_shared<const std::string>(std::move(prefix)), std::memory_order_release);
    }

    std::string ConfigManager::environmentOverridePrefix() const
    {
        const auto prefixSnapshot = m_environmentPrefix.load(std::memory_order_acquire);
        return prefixSnapshot != nullptr ? *prefixSnapshot : std::string();
    }

    std::size_t ConfigManager::environmentOverrideCount() const
    {
        const auto currentData = m_data.load(std::memory_order_acquire);
        return currentData->environmentOverrideCount;
    }

    std::size_t ConfigManager::applyEnvironmentOverrides(ConfigKeyValueMap &values) const
    {
        std::vector<std::string> appliedKeys;
        return applyEnvironmentOverrides(values, appliedKeys);
    }

    std::size_t ConfigManager::applyEnvironmentOverrides(ConfigKeyValueMap &values, std::vector<std::string> &appliedKeys) const
    {
        appliedKeys.clear();
        const auto prefixSnapshot = m_environmentPrefix.load(std::memory_order_acquire);
        if (prefixSnapshot == nullptr || prefixSnapshot->empty())
        {
            return 0;
        }
        const std::string &prefix = *prefixSnapshot;

        // schema 只取一份局部副本：m_schemaMutex 不归本方法持有，在它的临界区里做整表环境变量扫描
        // 与逐条转换，等于把一次部署侧的批量读取压进所有校验路径的锁里
        ConfigSchema schema;
        {
            const std::lock_guard schemaLock(m_schemaMutex);
            schema = m_schema;
        }

        const std::vector<EnvironmentOverrideCandidate> candidates = collectEnvironmentOverrideCandidates(prefix);
        if (candidates.empty())
        {
            return 0;
        }

        struct Applied
        {
            std::string key;
            ConfigValue value;
        };

        std::vector<Applied>     applied;
        std::vector<std::string> failures;
        applied.reserve(candidates.size());
        for (const auto &candidate: candidates)
        {
            std::optional<ConfigValueType> targetType;
            if (const auto existing = values.find(candidate.key); existing != values.end() && existing->second.type() != ConfigValueType::null)
            {
                // 文件里已有的类型是最可靠的依据：覆盖要顶替同一个键，换了类型读它的人会拿到意外形状
                targetType = existing->second.type();
            } else if (const auto entry = std::ranges::find(schema, candidate.key, &ConfigSchemaEntry::key); entry != schema.end() && entry->expectedType.has_value())
            {
                targetType = entry->expectedType;
            } else
            {
                // 两处都拿不到类型时按字符串交出，不猜。schema 声明过别的类型的话，提交后的校验会把它报出来
                targetType = ConfigValueType::string;
            }

            std::string                error;
            std::optional<ConfigValue> converted = coerceEnvironmentValue(candidate.value, *targetType, error);
            if (!converted.has_value())
            {
                failures.push_back(std::format("配置键 '{}'（按类型 '{}' 解释，来自环境变量 '{}'）：{}", candidate.key, typeName(*targetType), candidate.variableName, error));
                continue;
            }
            applied.push_back({candidate.key, std::move(*converted)});
        }

        if (!failures.empty())
        {
            // 整批不应用：半份覆盖比没有覆盖更难查——文件里的值与部署侧的值会各说一半
            for (const auto &failure: failures)
            {
                LOG_ERROR_FMT("ConfigManager：环境变量覆盖失败，本轮一条都不应用：{}", failure);
            }
            return 0;
        }

        // 排序让互异键之间的冲突有确定结果：键按字典序逐条合入，后写的那条胜出，不再取决于
        // 部署环境给出的变量枚举顺序；折成同一键的多个变量名（POSIX 上仅大小写不同的写法）
        // 之间没有确定先后——std::ranges::sort 不稳定，谁胜出不能靠变量名的拼写推断。
        // 逐条经 mergeFileValues 并进快照，与文件合并共用同一套「标量/表互斥」的定形规则，
        // 覆盖不能绕过它直接写表
        std::ranges::sort(applied, {}, &Applied::key);
        for (auto &entry: applied)
        {
            LOG_DEBUG_FMT("ConfigManager：配置键 '{}' 被环境变量覆盖", entry.key);
            ConfigKeyValueMap singleOverride;
            // 键按副本进表：后面的存活检查还要按它回查快照，搬走就查不成了
            singleOverride.emplace(entry.key, std::move(entry.value));
            for (const std::string &conflict: mergeFileValues(values, std::move(singleOverride), "环境变量覆盖"))
            {
                LOG_ERROR(conflict);
            }
        }

        // 后写的一条覆盖可能把先写的那条删掉（例如 server.port 淘汰掉 server 标量），
        // 因此只认合入后键仍留在快照里的应用条目：逐条回查，折成同一键的多个变量
        // 各留一条；这既不是互异的键数，也不是合入前的应用条数
        for (const Applied &entry: applied)
        {
            if (values.contains(entry.key))
            {
                appliedKeys.push_back(entry.key);
            }
        }
        return appliedKeys.size();
    }

    void ConfigManager::commitConfigData(ConfigKeyValueMap values, const std::vector<std::string> &loadedFiles, const std::filesystem::path &configDirectory,
                                         const bool configDirectoryRecursive)
    {
        // 快照对象与它的字典都在锁外建好：分配与展开一份配置不该让并发写者等着
        const auto newData = std::make_shared<ConfigData>();
        // 环境变量覆盖就加在这一处：首次加载、reload 与热重载走的都是这个出口，于是覆盖不会被下一轮
        // 热重载冲掉，也不需要每个调用点各写一遍（少写一处就是「哪条路忘了叠加」那类分叉）
        std::vector<std::string> environmentOverrideKeys;
        newData->environmentOverrideCount = applyEnvironmentOverrides(values, environmentOverrideKeys);
        newData->environmentOverrideKeys  = std::move(environmentOverrideKeys);
        newData->values                   = std::move(values);
        newData->loadedFiles              = loadedFiles;
        newData->configDirectory          = configDirectory;
        // 递归与否跟着快照一起存：reload() 与热重载据此重扫，口径与这次加载一致
        newData->configDirectoryRecursive = configDirectoryRecursive;

        {
            // 与 setValue() 共用同一把写锁：加载/热重载也是「整份快照替换」的写者，
            // 不串行化的话，与并发的 setValue 谁后发布谁生效，先发布的那些键会被整份覆盖掉
            const std::lock_guard writeLock(m_writeMutex);
            m_data.store(newData, std::memory_order_release);
        }

        // 提交后自动校验已注册 schema，配置写错时第一时间在日志中暴露。
        // 校验刻意放在写锁之外：它按每条违规做一次 std::format 并走一次 Sink 写入，锁内做这些
        // 等于把「落一条日志」的成本转嫁给所有并发的 setValue()。快照已经发布，
        // 校验对着自己手里的 newData 判，与后来者替换快照不冲突
        validateRegisteredSchema(newData->values);
    }
} // namespace AsynGyanis::Base
