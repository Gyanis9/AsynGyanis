#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"
#include "Platform/FileSystem/FileSystem.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
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
            const auto           [out, errorCode] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
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
            std::map<std::string_view, std::vector<SectionEntry> > childGroups;
            for (const SectionEntry &entry: entries)
            {
                const std::size_t separator = entry.remainingPath.find('.');
                if (separator == std::string_view::npos)
                {
                    object.insert_or_assign(std::string(entry.remainingPath), *entry.value);
                    continue;
                }
                childGroups[entry.remainingPath.substr(0, separator)].push_back(
                        SectionEntry{entry.remainingPath.substr(separator + 1), entry.value});
            }

            for (const auto &[name, children]: childGroups)
            {
                const std::string groupPath = pathContext.empty() ? std::string(name)
                                                                  : pathContext + '.' + std::string(name);
                // 同一个名字既当标量又当分组时，必须报错而不是让分组悄悄吃掉标量：这种形状真的能
                // 由两份配置文件叠出来（一份给 a.b，另一份给 a.b.c），静默丢弃会让 getSection 与
                // getInt("a.b") 各说一套话
                if (object.contains(name))
                {
                    throw ConfigValidationException(groupPath,
                                                    std::format("键 '{}' 既配成了标量、又是更长键的第一段（如 '{}.{}'）："
                                                                "段落还原无法同时表达这两种形状，请把其中一条改成别的名字或换一层段落",
                                                                groupPath, groupPath, std::string(children.front().remainingPath.substr(0, children.front().remainingPath.find('.')))));
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
            if (!std::filesystem::exists(filePath))
            {
                result.failedFiles.push_back(pathText(filePath));
                result.errors.push_back("文件不存在：" + pathText(filePath));
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
                for (auto &[key, value]: fileValues)
                {
                    values.insert_or_assign(std::move(key), std::move(value));
                }
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
            const auto previousData = m_data.load(std::memory_order_acquire);
            std::filesystem::path directoryToCommit = previousData->configDirectory;
            if (const std::filesystem::path derivedDirectory = commonParentDirectory(loadedPaths); !derivedDirectory.empty())
            {
                directoryToCommit = derivedDirectory;
            }
            // 锚点没换目录时沿用快照原有的递归口径：显式列表本身没有「递归与否」，把口径写成 true
            // 等于让同一个目录在无人改文件时换了一副面孔——先 loadFromDirectory(dir, false) 再
            // loadFiles({dir/a.yaml})，随后的 reload() 会凭空多出子目录里的键。
            // 锚点确实是新目录时才取默认值 true（旧口径属于另一棵树，跟不过来）
            const bool recursiveToCommit = directoryToCommit == previousData->configDirectory
                                               ? previousData->configDirectoryRecursive
                                               : true;
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
            m_hotReloadEnabled.store(false, std::memory_order_release);
            return false;
        }

        try
        {
            m_fileWatcher = Platform::FileWatcher::create();
            if (!m_fileWatcher)
            {
                m_hotReloadEnabled.store(false, std::memory_order_release);
                return false;
            }

            m_fileWatcher->setDebounceInterval(debounceMilliseconds);

            m_fileWatcher->setCallback([this](const std::string_view filePath, const Platform::FileChangeType changeType)
            {
                handleFileChange(filePath, changeType);
            });

            // 监听范围与 reload 的重扫范围必须同一个口径：只递归挂监听却按非递归重扫，
            // 子目录里的改动会白叫醒一轮重载；反之则子目录的改动根本进不到重扫里。
            // 交给监视器的是 UTF-8 文本：FileWatcher 的接口刻度已写明 UTF-8（Windows 上它的登记键与
            // 事件路径都由宽字符按 CP_UTF8 转出）。这里若换成 `.string()`，中文配置目录会被送去一份
            // 本地代码页的字节，注册那一步按 UTF-8 解不开而静默失败——热重载整条被关掉，现场只看得见
            // 「改了配置没反应」
            if (!m_fileWatcher->addWatch(pathText(currentData->configDirectory), currentData->configDirectoryRecursive))
            {
                m_fileWatcher.reset();
                m_hotReloadEnabled.store(false, std::memory_order_release);
                return false;
            }

            if (!m_fileWatcher->start())
            {
                m_fileWatcher.reset();
                m_hotReloadEnabled.store(false, std::memory_order_release);
                return false;
            }

            return true;
        } catch (...)
        {
            m_fileWatcher.reset();
            m_hotReloadEnabled.store(false, std::memory_order_release);
            return false;
        }
    }

    void ConfigManager::disableHotReload()
    {
        // 与 enableHotReload 共用同一把控制面锁，二者对 m_fileWatcher 的读写才互斥；
        // 这把锁总是最外层，监听线程的回调只碰 m_reloadTasksMutex，因此不会成环
        const std::lock_guard controlLock(m_hotReloadControlMutex);

        if (!m_hotReloadEnabled.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        // 等待所有活跃的重载线程完成，防止 use-after-free。
        // 锁内只摘取句柄，join 放到锁外：join 可能等到一次完整 reload 结束，
        // 持锁 join 会让文件监听线程后续的热加载检查全部排队等待
        std::vector<std::unique_ptr<ReloadTask> > pendingTasks;
        {
            const std::lock_guard lock(m_reloadTasksMutex);
            pendingTasks.swap(m_reloadTasks);
            // 重载回调里调 disableHotReload() 是合法用法（「连续几轮失败就别再监视了」），
            // 而本轮任务此刻正跑在本线程上、finished 要到回调返回之后才置起。把它一起销毁
            // 就是让 ~jthread 去 join 自己那一条线程：std::jthread 抛
            // resource_deadlock_would_occur，而异常从析构里出来即 std::terminate。
            // 留下它，本轮收尾后由 collectFinishedReloadTasks 正常回收
            for (auto &task: pendingTasks)
            {
                if (task && task->thread.get_id() == std::this_thread::get_id())
                {
                    m_reloadTasks.push_back(std::move(task));
                }
            }
        }
        pendingTasks.clear();

        if (m_fileWatcher)
        {
            m_fileWatcher->stop();
            m_fileWatcher.reset();
        }
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
        // 与 getText 同一口径按视图直查：走 get<std::string> 要先把整份 ConfigValue 深拷进
        // optional、再从那份副本里拷一次字符串，而进入时形参 defaultValue 已经按值构造过一份
        // （命中时那次分配纯属白花）。严格类型口径不变：只有字符串值才算取到，其余回落默认值
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
        const auto iterator    = currentData->values.find(key);
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
        {
            const std::lock_guard writeLock(m_writeMutex);

            // 复制当前快照并更新目标键，原子替换后立即对所有读者生效
            const auto currentData = m_data.load(std::memory_order_acquire);
            newData                = std::make_shared<ConfigData>(*currentData);

            newData->values[std::string(key)] = std::move(value);
            m_data.store(newData, std::memory_order_release);
        }

        // 校验与日志放在写锁之外，与 commitConfigData 同一排法：按违规做一次 std::format
        // 并走一次 Sink 写入，锁内做这些等于把「落一条日志」的成本转嫁给所有并发的写者。
        // 判的是自己刚发布的那份快照里的这个键，后来者替换快照也不影响这条报告的归属
        validateRegisteredSchema(newData->values, key);

        return true;
    }

    namespace
    {
        /**
         * @brief 读取文本文件全部内容
         * @param filePath 目标文件路径
         * @return std::optional<std::string> 成功返回内容，无法打开返回 std::nullopt
         */
        [[nodiscard]] std::optional<std::string> readTextFile(const std::filesystem::path &filePath)
        {
            std::ifstream file(filePath, std::ios::in | std::ios::binary);
            if (!file.is_open())
            {
                return std::nullopt;
            }

            // 先按文件大小预留容量：istreambuf_iterator 逐字符追加会触发多次重新分配与搬移。
            // 取不到大小（管道、特殊文件）时保持按需增长，不影响正确性
            std::string     content;
            std::error_code sizeError;
            if (const std::uintmax_t fileSize = std::filesystem::file_size(filePath, sizeError); !sizeError)
            {
                content.reserve(static_cast<std::string::size_type>(fileSize));
            }

            content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

            // BOM 在这里就剥掉，而不是留给 JSON 解析器：只带 BOM 的文件（Windows 记事本把空文件
            // 存成 UTF-8 就是这个字节串）会先过「空文件」判定——三个非空白字节判不出空，
            // 于是带着空正文去解析，被报成「JSON 语法错误」。同一个文件在 YAML 侧被当空文档接受、
            // 真·空文件也被接受，只有 JSON+BOM 这一格被拒，而且每次热重载都重复报这个幽灵错误
            constexpr std::string_view kUtf8ByteOrderMark = "\xEF\xBB\xBF";
            if (content.starts_with(kUtf8ByteOrderMark))
            {
                content.erase(0, kUtf8ByteOrderMark.size());
            }
            return content;
        }

        /**
         * @brief 判断文本是否只由空白字符组成
         * @param text 待判定文本
         * @return true 为空或全空白
         */
        [[nodiscard]] bool isBlankText(std::string_view text) noexcept
        {
            return std::ranges::all_of(text,
                                       [](const unsigned char character)
                                       {
                                           return std::isspace(character) != 0;
                                       });
        }

        /// 单个配置文件的体积上限（64 MiB）：配置文件是人工产物，超出这个规模基本可判定为误传或
        /// 攻击载荷，在解析前拦下，免得解析器为超大文本做巨量分配
        constexpr std::uintmax_t kMaximumConfigFileBytes = 64ULL * 1024ULL * 1024ULL;

        /// 文档的嵌套深度上限（JSON 与 YAML 同一口径）。YAML 侧：别名在 DOM 里是共享引用，
        /// 循环别名会让转换无限递归；JSON 侧：nlohmann 的解析器是状态机、不会先崩在解析上，
        /// 会崩的是 DOM 的递归析构与我们自己摊平嵌套结构的递归
        constexpr std::size_t kMaximumDocumentDepth = 128;

        /// YAML 文档展开后的节点总数上限：别名炸弹（引用同一锚点逐层放大）靠这道预算拦下
        constexpr std::size_t kMaximumYamlNodeCount = 200000;

        /**
         * @brief 读取或转换配置文档时的失败（嵌套超限、不支持的标签、数值越界等）
         * @details 只在本翻译单元内抛出并被 loadConfigFile 捕获后转成中文错误文案，
         *          模块对外从不暴露该类型，因此刻意不并入 Base 的异常层次。
         */
        class DocumentConversionException : public std::runtime_error
        {
        public:
            /**
             * @brief 以中文失败原因构造异常
             * @param reason 不含文件名的失败原因
             */
            explicit DocumentConversionException(const std::string &reason) : std::runtime_error(reason)
            {
            }
        };

        /**
         * @brief 把库的位置标记格式化为可读文本
         * @param mark yaml-cpp 的位置标记
         * @return std::string 形如「第 N 行，第 M 列」；标记无效时返回「位置未知」
         */
        [[nodiscard]] std::string describeYamlMark(const YAML::Mark &mark)
        {
            if (mark.is_null())
            {
                return "位置未知";
            }
            // 库的行列从 0 起算，报给用户时统一转成从 1 起算
            return std::format("第 {} 行，第 {} 列", mark.line + 1, mark.column + 1);
        }

        /**
         * @brief 按核心 schema 识别布尔文本
         * @param text 标量文本
         * @return std::optional<bool> 不是布尔文本时返回空；只认 true/false 的三种大小写组合
         */
        [[nodiscard]] std::optional<bool> parseCoreBoolean(const std::string &text) noexcept
        {
            if (text == "true" || text == "True" || text == "TRUE")
            {
                return true;
            }
            if (text == "false" || text == "False" || text == "FALSE")
            {
                return false;
            }
            return std::nullopt;
        }

        /**
         * @brief 按核心 schema 识别整数文本（十进制、0o 八进制、0x 十六进制，可带正负号）
         * @details 非负整数与 JSON 侧口径一致地落无符号数（原生解析把非负整数放进 number_unsigned）。
         * @param text 标量文本
         * @param node 所在节点，用于错误位置
         * @return std::optional<ConfigValue> 不是整数文本时返回空
         * @throws DocumentConversionException 是整数文本但超出 64 位表示范围
         */
        [[nodiscard]] std::optional<ConfigValue> parseCoreInteger(const std::string &text, const YAML::Node &node)
        {
            std::size_t position = 0;
            bool        negative = false;
            if (position < text.size() && (text[position] == '+' || text[position] == '-'))
            {
                negative = text[position] == '-';
                ++position;
            }

            int base = 10;
            if (text.compare(position, 2, "0o") == 0)
            {
                base = 8;
                position += 2;
            } else if (text.compare(position, 2, "0x") == 0)
            {
                base = 16;
                position += 2;
            }
            if (position >= text.size())
            {
                return std::nullopt;
            }

            std::uint64_t magnitude = 0;
            for (; position < text.size(); ++position)
            {
                const char character = text[position];
                int        digit     = -1;
                if (character >= '0' && character <= '9')
                {
                    digit = character - '0';
                } else if (base == 16 && character >= 'a' && character <= 'f')
                {
                    digit = character - 'a' + 10;
                } else if (base == 16 && character >= 'A' && character <= 'F')
                {
                    digit = character - 'A' + 10;
                }
                if (digit < 0 || digit >= base)
                {
                    // 出现非数字字符（如 1.2.3、0xZZ）：整体不是整数
                    return std::nullopt;
                }
                // 先按无符号累加，溢出即越界：静默回绕会得到看似正常的错误数值
                if (magnitude > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(digit)) / static_cast<std::uint64_t>(base))
                {
                    throw DocumentConversionException(std::format("整数 '{}' 超出 64 位表示范围（{}）", text, describeYamlMark(node.Mark())));
                }
                magnitude = magnitude * base + static_cast<std::uint64_t>(digit);
            }

            if (negative)
            {
                // 负方向 uint64 只到 INT64_MIN 的量级，再大同样越界
                if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL)
                {
                    throw DocumentConversionException(std::format("整数 '{}' 超出 64 位表示范围（{}）", text, describeYamlMark(node.Mark())));
                }
                if (magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL)
                {
                    return ConfigValue(std::numeric_limits<std::int64_t>::min());
                }
                return ConfigValue(-static_cast<std::int64_t>(magnitude));
            }
            return ConfigValue(magnitude);
        }

        /**
         * @brief 按核心 schema 识别浮点文本
         * @details 接受十进制小数/指数形式与 .inf/.nan 系列；裸 inf/nan 按 schema 属字符串，
         *          而库的解析会接受它们，因此先按字符集挡掉再做严格的全串解析。
         * @param text 标量文本
         * @return std::optional<double> 不是浮点文本时返回空
         * @throws DocumentConversionException 是浮点文本但超出 double 表示范围
         */
        [[nodiscard]] std::optional<double> parseCoreFloat(const std::string &text)
        {
            std::string_view signlessText = text;
            bool             negative     = false;
            if (!signlessText.empty() && (signlessText.front() == '+' || signlessText.front() == '-'))
            {
                negative = signlessText.front() == '-';
                signlessText.remove_prefix(1);
            }

            if (signlessText == ".inf" || signlessText == ".Inf" || signlessText == ".INF")
            {
                return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            }
            if (signlessText == ".nan" || signlessText == ".NaN" || signlessText == ".NAN")
            {
                return std::numeric_limits<double>::quiet_NaN();
            }

            // 字符集先挡一道：库也接受 "inf"/"nan"/"0x1p3" 这类写法，但它们不是核心 schema 的浮点
            bool hasDigit = false;
            for (const char character: signlessText)
            {
                if (character >= '0' && character <= '9')
                {
                    hasDigit = true;
                } else if (character != '.' && character != '-' && character != 'e' && character != 'E')
                {
                    return std::nullopt;
                }
            }
            if (!hasDigit)
            {
                return std::nullopt;
            }

            std::string_view numericText = text;
            // 前导 '+'（YAML 允许）不是标准库解析所认的写法，剥掉；'-' 保留给库处理
            if (!numericText.empty() && numericText.front() == '+')
            {
                numericText.remove_prefix(1);
            }

            double     parsedValue               = 0.0;
            const auto [stopPosition, errorCode] = std::from_chars(numericText.data(), numericText.data() + numericText.size(), parsedValue);
            if (errorCode == std::errc::result_out_of_range)
            {
                throw DocumentConversionException(std::format("浮点值 '{}' 超出 double 表示范围", text));
            }
            if (errorCode != std::errc() || stopPosition != numericText.data() + numericText.size())
            {
                // 形态不合（如 1.2.3、1e）：不是浮点，按字符串原样保留
                return std::nullopt;
            }
            return parsedValue;
        }

        /**
         * @brief 把 YAML 标量转换为 JSON 标量
         * @details 引号标量（标签 "!"）一律按字符串处理；普通标量（标签 "?"）按 YAML 1.2
         *          核心 schema 识别 true/false、整数与浮点，其余原样作字符串；显式标签只认
         *          核心 schema 的 str/int/float/bool/null，其余（如 !!binary、!!timestamp）
         *          明确报错而不是静默丢成字符串。
         * @param node 标量节点
         * @return ConfigValue 转换结果
         * @throws DocumentConversionException 标签不受支持、整数或浮点越界、显式 bool/float 的取值不合法
         */
        [[nodiscard]] ConfigValue yamlScalarToConfigValue(const YAML::Node &node)
        {
            const std::string &tag  = node.Tag();
            const std::string &text = node.Scalar();

            if (tag == "!" || tag == "tag:yaml.org,2002:str")
            {
                return ConfigValue(text);
            }
            if (tag == "tag:yaml.org,2002:null")
            {
                return ConfigValue(nullptr);
            }
            if (tag == "tag:yaml.org,2002:bool")
            {
                if (const std::optional<bool> booleanValue = parseCoreBoolean(text))
                {
                    return ConfigValue(*booleanValue);
                }
                throw DocumentConversionException(std::format("!!bool 的取值 '{}' 不是合法布尔（{}）：只接受 true/false", text, describeYamlMark(node.Mark())));
            }
            if (tag == "tag:yaml.org,2002:int")
            {
                if (const std::optional<ConfigValue> integerValue = parseCoreInteger(text, node))
                {
                    return *integerValue;
                }
                throw DocumentConversionException(std::format("!!int 的取值 '{}' 不是合法整数（{}）", text, describeYamlMark(node.Mark())));
            }
            if (tag == "tag:yaml.org,2002:float")
            {
                if (const std::optional<double> floatingValue = parseCoreFloat(text))
                {
                    return ConfigValue(*floatingValue);
                }
                throw DocumentConversionException(std::format("!!float 的取值 '{}' 不是合法浮点（{}）", text, describeYamlMark(node.Mark())));
            }
            if (tag != "?")
            {
                throw DocumentConversionException(std::format("不支持的 YAML 标签 '{}'（{}）：请改用 !!str/!!int/!!float/!!bool/!!null 或去掉标签", tag,
                                                           describeYamlMark(node.Mark())));
            }

            // 普通标量：核心 schema 的类型判定，顺序为 bool → int → float → string
            if (const std::optional<bool> booleanValue = parseCoreBoolean(text))
            {
                return ConfigValue(*booleanValue);
            }
            if (const std::optional<ConfigValue> integerValue = parseCoreInteger(text, node))
            {
                return *integerValue;
            }
            if (const std::optional<double> floatingValue = parseCoreFloat(text))
            {
                return ConfigValue(*floatingValue);
            }
            return ConfigValue(text);
        }

        /**
         * @brief 递归把 yaml-cpp 节点转换为 JSON 值
         * @details 别名在 yaml-cpp 的 DOM 里是共享引用、在 JSON 里只能深拷贝，故用深度与节点总数
         *          两道上限拦住循环别名与别名炸弹；重复键在 DOM 里保留为两份，这里明确报错
         *          而不让后者静默覆盖前者。
         * @param node 当前节点
         * @param depth 当前嵌套深度（根为 0）
         * @param remainingNodeBudget 剩余可转换节点数，逐节点扣减
         * @return ConfigValue 转换结果
         * @throws DocumentConversionException 超过深度或节点数上限、映射键不是标量、映射存在重复键
         */
        [[nodiscard]] ConfigValue yamlNodeToConfigValue(const YAML::Node &node, const std::size_t depth, std::size_t &remainingNodeBudget)
        {
            if (depth > kMaximumDocumentDepth)
            {
                throw DocumentConversionException(std::format("文档嵌套或别名展开超过 {} 层：若文件里存在循环别名，请先解开锚点引用", kMaximumDocumentDepth));
            }
            if (remainingNodeBudget == 0)
            {
                throw DocumentConversionException(std::format("文档展开后的节点总数超过上限 {}：引用同一锚点的别名会被逐层复制，请减少别名引用", kMaximumYamlNodeCount));
            }
            --remainingNodeBudget;

            // 空值（null）与未定义节点都按 null 处理：「键存在但没有取值」是合法形态
            if (!node.IsDefined() || node.IsNull())
            {
                return ConfigValue(nullptr);
            }

            if (node.IsSequence())
            {
                ConfigValue elements = ConfigValue::array();
                for (const YAML::Node &element: node)
                {
                    elements.push_back(yamlNodeToConfigValue(element, depth + 1, remainingNodeBudget));
                }
                return elements;
            }

            if (node.IsMap())
            {
                ConfigValue members = ConfigValue::object();
                for (const auto &entry: node)
                {
                    if (!entry.first.IsScalar())
                    {
                        throw DocumentConversionException(std::format("映射的键必须是标量（{}）：复杂键无法表示为 JSON 对象键", describeYamlMark(entry.first.Mark())));
                    }
                    const std::string key = entry.first.Scalar();
                    if (members.contains(key))
                    {
                        throw DocumentConversionException(std::format("映射中存在重复键 '{}'（{}）：请删除重复定义", key, describeYamlMark(entry.first.Mark())));
                    }
                    members[key] = yamlNodeToConfigValue(entry.second, depth + 1, remainingNodeBudget);
                }
                return members;
            }

            // 到这里只剩标量：未定义与 null 已在前面处理
            return yamlScalarToConfigValue(node);
        }

        /**
         * @brief 解析 YAML 文本并转换为 JSON 值模型
         * @details 只取首份文档（配置文件不做多文档语义）；空输入与空文档返回 null，
         *          由根节点类型校验给出「根节点必须是映射」。
         * @param text 文档文本
         * @return ConfigValue 文档根值
         * @throws YAML::Exception YAML 语法非法
         * @throws DocumentConversionException 超过展开或深度上限、含不支持的标签
         */
        [[nodiscard]] ConfigValue yamlDocumentToConfigValue(const std::string &text)
        {
            const YAML::Node document = YAML::Load(text);
            if (!document.IsDefined() || document.IsNull())
            {
                return ConfigValue(nullptr);
            }

            std::size_t remainingNodeBudget = kMaximumYamlNodeCount;
            return yamlNodeToConfigValue(document, 0, remainingNodeBudget);
        }

        /**
         * @brief 解析 JSON 文本（先剥掉可选的 UTF-8 BOM）
         * @details 深度闸门挂在解析回调上：nlohmann 的解析器自己是状态机，超限的是随后
         *          那个几万层的 DOM 的递归析构，以及我们把嵌套结构摊平成点分键的那趟递归，
         *          所以在建 DOM 的途中就抛出，交回调用方的是「一轮失败的加载」而不是崩溃。
         * @param text 文档文本
         * @return ConfigValue 文档根值
         * @throws DocumentConversionException 嵌套超过 kMaximumDocumentDepth 层
         * @throws nlohmann::json::exception 语法错误（消息自带行列与出错记号）
         */
        [[nodiscard]] ConfigValue parseJsonDocument(const std::string &text)
        {
            const auto depthGuard = [](const int depth, const ConfigValue::parse_event_t, ConfigValue &)
            {
                if (static_cast<std::size_t>(depth) > kMaximumDocumentDepth)
                {
                    throw DocumentConversionException(std::format("文档嵌套超过 {} 层：正常配置不会这么深，多半是误传或机器生成的文件",
                                                                  kMaximumDocumentDepth));
                }
                return true;
            };

            // nlohmann 不认 UTF-8 BOM，而 Windows 编辑器常写 BOM：先剥掉再解析
            constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF"};
            if (text.starts_with(kUtf8Bom))
            {
                return ConfigValue::parse(text.substr(kUtf8Bom.size()), depthGuard);
            }
            return ConfigValue::parse(text, depthGuard);
        }

        /**
         * @brief 按文件后缀选择原生解析器解析一份配置文档
         * @details .json 走 nlohmann_json；.yaml/.yml 走 yaml-cpp 再转换成 JSON 值模型。
         * @param text 文档文本
         * @param filePath 文件路径，仅用于挑选解析器
         * @return ConfigValue 文档根值
         * @throws nlohmann::json::exception JSON 语法非法
         * @throws YAML::Exception YAML 语法非法
         * @throws DocumentConversionException YAML 文档无法忠实转换为 JSON 值模型
         */
        [[nodiscard]] ConfigValue parseDocumentBySuffix(const std::string &text, const std::filesystem::path &filePath)
        {
            if (isJsonFile(pathText(filePath)))
            {
                return parseJsonDocument(text);
            }
            return yamlDocumentToConfigValue(text);
        }

    } // namespace

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

        // 扫描本身可能中途失败（子目录不可读、条目属性取不到）。这时交回的**不是**一份
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
                for (auto &[key, value]: fileValues)
                {
                    values.insert_or_assign(std::move(key), std::move(value));
                }
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
        // 先按文件大小拦下超大文件：解析器要为全文分配内存，读进来再判就晚了
        std::error_code      sizeError;
        const std::uintmax_t fileSize = std::filesystem::file_size(filePath, sizeError);
        if (!sizeError && fileSize > kMaximumConfigFileBytes)
        {
            errors.push_back(std::format("文件 '{}' 超过配置文件的体积上限（{} MiB）：请拆分或精简该文件", pathText(filePath),
                                         kMaximumConfigFileBytes / (1024ULL * 1024ULL)));
            return false;
        }

        const std::optional<std::string> text = readTextFile(filePath);
        if (!text.has_value())
        {
            errors.push_back("无法打开文件 '" + pathText(filePath) + "'：文件不存在或不可读");
            return false;
        }

        if (isBlankText(*text))
        {
            // 空文件：不报错、不产出配置项
            return true;
        }

        try
        {
            // 文档必须可改写：叶子值会被移出以便省掉一次深拷贝（文档本身是本次解析的临时产物）
            ConfigValue document = parseDocumentBySuffix(*text, filePath);

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
                const std::string_view kindName = document.is_array() ? std::string_view{"sequence"}
                                                     : std::string_view{typeName(document.type())};

                errors.push_back("文件 '" + pathText(filePath) + "'：根节点必须是映射，实际为 " + std::string(kindName));
                return false;
            }

            std::string prefixBuffer;
            flattenValue(document, prefixBuffer, values);
        } catch (const ConfigValue::exception &exception)
        {
            // 原生库的消息自带行列与出错记号，是定位所需的全部信息，原样带出
            errors.push_back(std::format("解析错误：'{}'：JSON 语法错误：{}", pathText(filePath), exception.what()));
            return false;
        } catch (const YAML::Exception &exception)
        {
            errors.push_back(std::format("解析错误：'{}'：YAML 语法错误：{}（{}）", pathText(filePath), exception.msg, describeYamlMark(exception.mark)));
            return false;
        } catch (const std::runtime_error &exception)
        {
            // YAML → JSON 值模型转换失败：本文件内抛出的中文原因（含位置）
            errors.push_back(std::format("解析错误：'{}'：{}", pathText(filePath), exception.what()));
            return false;
        } catch (const std::exception &exception)
        {
            errors.push_back("加载 '" + pathText(filePath) + "' 时发生意外错误：" + exception.what());
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

    void ConfigManager::handleFileChange(const std::string_view filePath, const Platform::FileChangeType changeType)
    {
        // 这条路径跑在文件监视线程上，而监视器派发改动时不拦回调的异常：从这里抛出去
        // 就是逃出线程函数 → std::terminate 把整个进程带走。单个事件失败只该丢掉这一个事件
        // （下一次改动还会再来），因此整段收口在这里
        try
        {
            // 重扫信号走的是**目录**路径，按扩展名的那道过滤会把它整个滤掉——内核丢事件时本要的正是
            // 「把整份目录重读一遍」，因此它在过滤之前先接下来，与单个文件变更走同一条重载路径
            if (changeType == Platform::FileChangeType::NeedsRescan)
            {
                scheduleReload();
                return;
            }

            if (!isConfigFile(filePath))
            {
                return;
            }

            // Moved 也要收：Windows 把「改名走开」报成 Moved（源路径），Linux 的同一条动作被映射成 Deleted。
            // 只收三种时，Windows 上「把 config.yaml 改名挪走」这个下线动作一条通知都不算数——旧名被类型
            // 判据滤掉、新名因不是配置后缀被上一条滤掉，配置停在已经消失的那份上直到下次改动
            if (changeType != Platform::FileChangeType::Modified && changeType != Platform::FileChangeType::Created && changeType != Platform::FileChangeType::Deleted && changeType != Platform::FileChangeType::Moved)
            {
                return;
            }

            scheduleReload();
        } catch (const std::exception &changeError)
        {
            LOG_ERROR_EXCEPTION(changeError, "ConfigManager: 处理文件变更事件时抛出异常，本事件已丢弃：{}", changeError.what());
        } catch (...)
        {
            LOG_ERROR_FMT("ConfigManager: 处理文件变更事件时抛出非标准异常，本事件已丢弃");
        }
    }

    /**
     * @brief 安排一轮重载：已在跑就只记下这次变更，由那一轮的收尾接力
     */
    void ConfigManager::scheduleReload()
    {
        // 先把「又要重读」记进世代号，再去抢执行权：抢不到时正在跑的那一轮收尾会比对自己开始时的
        // 快照，看到差别就再来一轮。反过来（先抢后记）可能让那一轮刚好比对在这次记录之前，
        // 这次变更就没人接力了
        m_reloadGate.noteChanged();

        // 已有任务在跑：不重复起轮，交给它那轮的接力。重载要读完整份目录，期间到达的变更
        // （尤其是紧接着那次写入）会落在本轮之后——丢掉它配置就停在旧值，直到用户下一次改动
        if (!m_reloadGate.claimRound())
        {
            return;
        }

        startReloadTask();

        // 清理已完成任务并回收线程资源，防止任务列表无限增长。
        // join 在锁外执行：持 m_reloadTasksMutex 期间 join 会把「一次长 reload 的等待」
        // 变成对所有后续热加载检查的阻塞
        collectFinishedReloadTasks();
    }

    void ConfigManager::startReloadTask()
    {
        // 起线程是这条路上唯一会失败的分配（建任务节点、开线程栈、线程数触到系统上限）。
        // 让它抛出去等于把异常送到调用方的线程函数外——监视线程与重载线程都会 std::terminate。
        // 这里的收口方式是「本轮不跑，把活留给下一次事件」
        try
        {
            const std::lock_guard lock(m_reloadTasksMutex);

            auto        task    = std::make_unique<ReloadTask>();
            ReloadTask *rawTask = task.get();
            task->thread        = std::jthread([this, rawTask]() { runReloadTask(rawTask); });
            m_reloadTasks.push_back(std::move(task));
            return;
        } catch (const std::exception &startError)
        {
            LOG_ERROR_EXCEPTION(startError, "ConfigManager: 启动热重载任务失败，本轮重载让给下一次事件：{}", startError.what());
        } catch (...)
        {
            LOG_ERROR_FMT("ConfigManager: 启动热重载任务失败（非标准异常），本轮重载让给下一次事件");
        }

        // 起不来时把刚抢到的执行权交还：本轮就此作罢，等下一次事件重新起轮。
        // 这里刻意不做接力（releaseRound 会在有新变更时再起一轮）——线程本来就起不来，
        // 接力只会在这条失败路径上原地递归
        m_reloadGate.finishRunning();
    }

    void ConfigManager::runReloadTask(ReloadTask *rawTask)
    {
        // 本轮开始前的世代快照：收尾时与当前值比对，不同就说明「本轮跑完之后或跑的过程中」
        // 又有变更落盘，那笔变更本轮没读到，必须再来一轮
        const std::uint64_t observedGeneration = m_reloadGate.observedGeneration();
        // 任务体的顶层兜底：doReload() 与用户回调都可能抛（解析失败、类型不符、用户代码）。
        // 让异常逃出线程函数就是 std::terminate 把整个进程带走——这里收口成一次「失败的重载」，
        // 并照常通知回调，免得调用方以为配置已经刷新
        try
        {
            if (!m_hotReloadEnabled.load(std::memory_order_acquire))
            {
                // 热重载已关闭：本轮不跑，收尾在下面统一做
            } else
            {
                const ConfigLoadResult result = doReload();

                // 回调快照：本线程（重载工作线程）读到的是 enableHotReload 发布的那一份
                if (const auto callback = m_hotReloadCallback.load(std::memory_order_acquire); callback && *callback)
                {
                    (*callback)(result);
                }
            }
        } catch (const std::exception &reloadError)
        {
            LOG_ERROR_EXCEPTION(reloadError, "ConfigManager: 热重载任务抛出异常，本轮按失败处理：{}", reloadError.what());
            // 再通知回调一次「本轮失败」：不通知的话调用方会以为配置已经刷新。
            // 这一次通知同样可能抛（抛的那个回调就是它），因此单独兜一层——再逃出去还是 terminate
            try
            {
                if (const auto callback = m_hotReloadCallback.load(std::memory_order_acquire); callback && *callback)
                {
                    ConfigLoadResult failedResult;
                    failedResult.success = false;
                    failedResult.errors.push_back(std::string("热重载任务抛出异常：") + reloadError.what());
                    (*callback)(failedResult);
                }
            } catch (const std::exception &notificationError)
            {
                LOG_ERROR_EXCEPTION(notificationError, "ConfigManager: 通知「本轮重载失败」时回调又抛出异常，已忽略：{}", notificationError.what());
            }
        } catch (...)
        {
            LOG_ERROR_FMT("ConfigManager: 热重载任务抛出非标准异常，本轮按失败处理");
        }

        // 接力：先交还执行权，再比对自己那份世代快照。顺序不能反——反了会让「交还之后、
        // 比对之前」到达的变更没人接力。比对而不是清标记，是因为收尾可以并发：一轮交还执行权
        // 之后它的收尾代码还在跑，另一轮已经起好并跑完，此时若共用一面脏标记，停在中间那一格
        // 的旧轮次会把别人记下的欠账吃掉，而它自己又抢不到接力权——那笔变更再没人重读
        if (m_reloadGate.releaseRound(observedGeneration))
        {
            startReloadTask();
        }
        rawTask->finished.store(true, std::memory_order_release);
    }

    void ConfigManager::collectFinishedReloadTasks()
    {
        std::vector<std::unique_ptr<ReloadTask> > finishedTasks;
        {
            const std::lock_guard lock(m_reloadTasksMutex);
            for (auto iterator = m_reloadTasks.begin(); iterator != m_reloadTasks.end();)
            {
                if ((*iterator)->finished.load(std::memory_order_acquire))
                {
                    // 锁内只摘取句柄：unique_ptr 移出后立即析构会在锁外 join
                    finishedTasks.push_back(std::move(*iterator));
                    iterator = m_reloadTasks.erase(iterator);
                } else
                {
                    ++iterator;
                }
            }
        }
        // 析构已结束任务的 jthread（此时线程必然已退出，join 立即返回）
        finishedTasks.clear();
    }

    ConfigLoadResult ConfigManager::doReload()
    {
        std::unique_lock lock(m_reloadMutex);

        const auto currentData = m_data.load(std::memory_order_acquire);
        if (currentData->configDirectory.empty())
        {
            ConfigLoadResult result;
            result.success = false;
            result.errors.emplace_back("尚未设置配置目录，请先调用 loadFromDirectory 或 loadFiles。");
            return result;
        }

        // 递归口径跟着快照走：调用方当初以 loadFromDirectory(dir, false) 建起来的配置，
        // 若在 reload() 时被强行改成递归，就会凭空多出子目录里的键——没人改过文件却换了配置
        return loadFromDirectoryImplementation(currentData->configDirectory, currentData->configDirectoryRecursive);
    }

    std::expected<std::vector<std::filesystem::path>, std::string> ConfigManager::scanConfigFiles(const std::filesystem::path &directory, const bool recursive)
    {
        std::vector<std::filesystem::path> configFiles;

        std::error_code errorCode;
        // 单个条目的属性查询也走 error_code 那一份重载：无 ec 的 is_regular_file() 会抛
        // filesystem_error，那会从「加载配置」里逃到调用方手上，而这里要的是一轮失败的重载
        const auto      collect = [&configFiles, &errorCode](const auto &entry)
        {
            const bool isRegularFile = entry.is_regular_file(errorCode);
            if (!errorCode && isRegularFile && isConfigFile(pathText(entry.path())))
            {
                configFiles.push_back(entry.path());
            }
        };

        if (recursive)
        {
            for (const auto &entry: std::filesystem::recursive_directory_iterator(directory, errorCode))
            {
                if (errorCode)
                {
                    break;
                }
                collect(entry);
                if (errorCode)
                {
                    break;
                }
            }
        } else
        {
            for (const auto &entry: std::filesystem::directory_iterator(directory, errorCode))
            {
                if (errorCode)
                {
                    break;
                }
                collect(entry);
                if (errorCode)
                {
                    break;
                }
            }
        }

        // 循环之外的这一次判定不能省：迭代器在**最后一个条目之后**才出错时，循环体内的检查
        // 永远不会再跑到，错误就这么留在 errorCode 里没人认领
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

    void ConfigManager::commitConfigData(ConfigKeyValueMap                values,
                                         const std::vector<std::string> & loadedFiles,
                                         const std::filesystem::path &    configDirectory,
                                         const bool                       configDirectoryRecursive)
    {
        // 快照对象与它的字典都在锁外建好：分配与展开一份配置不该让并发写者等着
        const auto newData       = std::make_shared<ConfigData>();
        newData->values          = std::move(values);
        newData->loadedFiles     = loadedFiles;
        newData->configDirectory = configDirectory;
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
