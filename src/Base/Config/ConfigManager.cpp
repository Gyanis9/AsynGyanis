#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"

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
            std::array<char, 32> buffer{};
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
         * @return ConfigObject 本层对象
         */
        [[nodiscard]] ConfigObject buildNestedObject(const std::vector<SectionEntry> &entries)
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
                object.insert_or_assign(std::string(name), ConfigValue(buildNestedObject(children)));
            }
            return object;
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
                result.failedFiles.push_back(filePath.string());
                result.errors.push_back("文件不存在：" + filePath.string());
                continue;
            }
            if (!isConfigFile(filePath.string()))
            {
                result.failedFiles.push_back(filePath.string());
                result.errors.push_back("不支持的配置文件格式（应为 .json、.yaml 或 .yml）：" + filePath.string());
                continue;
            }
            if (loadConfigFile(filePath, values, result.errors))
            {
                result.loadedFiles.push_back(filePath.string());
                loadedPaths.push_back(filePath);
            } else
            {
                result.failedFiles.push_back(filePath.string());
            }
        }

        // 至少一个文件成功才提交，全部失败时保留原有配置
        if (!result.loadedFiles.empty())
        {
            // 显式文件列表同样要留下配置目录，否则后续 reload() 与 enableHotReload() 失去依据：
            // 能推导出公共父目录时采用它，否则保留既有取值不覆盖。
            std::filesystem::path directoryToCommit = m_data.load(std::memory_order_acquire)->configDirectory;
            if (const std::filesystem::path derivedDirectory = commonParentDirectory(loadedPaths); !derivedDirectory.empty())
            {
                directoryToCommit = derivedDirectory;
            }
            commitConfigData(std::move(values), result.loadedFiles, directoryToCommit);
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
        if (bool expected = false; !m_hotReloadEnabled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
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

            // 回调以不可变快照发布：写侧是启动监听之前的本线程，读侧是后续的重载线程，
            // release/acquire 保证重载线程一定能看到完整的 std::function 对象
            m_hotReloadCallback.store(std::make_shared<const HotReloadCallback>(std::move(callback)), std::memory_order_release);

            m_fileWatcher->setDebounceInterval(debounceMilliseconds);

            m_fileWatcher->setCallback([this](const std::string_view filePath, const Platform::FileChangeType changeType)
            {
                handleFileChange(filePath, changeType);
            });

            if (!m_fileWatcher->addWatch(currentData->configDirectory.string(), true))
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
        return get<std::string>(key, std::string(defaultValue));
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
        const std::lock_guard writeLock(m_writeMutex);

        // 复制当前快照并更新目标键，原子替换后立即对所有读者生效
        const auto currentData = m_data.load(std::memory_order_acquire);
        const auto newData     = std::make_shared<ConfigData>(*currentData);

        newData->values[std::string(key)] = std::move(value);
        m_data.store(newData, std::memory_order_release);

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

        /// YAML 文档的嵌套/别名展开深度上限：别名在 DOM 里是共享引用，循环别名会让转换无限递归
        constexpr std::size_t kMaximumYamlDepth = 128;

        /// YAML 文档展开后的节点总数上限：别名炸弹（引用同一锚点逐层放大）靠这道预算拦下
        constexpr std::size_t kMaximumYamlNodeCount = 200000;

        /**
         * @brief YAML 文档转 JSON 值模型时的失败（展开超限、不支持的标签、数值越界等）
         * @details 只在本翻译单元内抛出并被 loadConfigFile 捕获后转成中文错误文案，
         *          模块对外从不暴露该类型，因此刻意不并入 Base 的异常层次。
         */
        class YamlConversionException : public std::runtime_error
        {
        public:
            /**
             * @brief 以中文失败原因构造异常
             * @param reason 不含文件名的失败原因
             */
            explicit YamlConversionException(const std::string &reason) : std::runtime_error(reason)
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
         * @throws YamlConversionException 是整数文本但超出 64 位表示范围
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
                    throw YamlConversionException(std::format("整数 '{}' 超出 64 位表示范围（{}）", text, describeYamlMark(node.Mark())));
                }
                magnitude = magnitude * base + static_cast<std::uint64_t>(digit);
            }

            if (negative)
            {
                // 负方向 uint64 只到 INT64_MIN 的量级，再大同样越界
                if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL)
                {
                    throw YamlConversionException(std::format("整数 '{}' 超出 64 位表示范围（{}）", text, describeYamlMark(node.Mark())));
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
         * @throws YamlConversionException 是浮点文本但超出 double 表示范围
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
                throw YamlConversionException(std::format("浮点值 '{}' 超出 double 表示范围", text));
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
         * @throws YamlConversionException 标签不受支持、整数或浮点越界、显式 bool/float 的取值不合法
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
                throw YamlConversionException(std::format("!!bool 的取值 '{}' 不是合法布尔（{}）：只接受 true/false", text, describeYamlMark(node.Mark())));
            }
            if (tag == "tag:yaml.org,2002:int")
            {
                if (const std::optional<ConfigValue> integerValue = parseCoreInteger(text, node))
                {
                    return *integerValue;
                }
                throw YamlConversionException(std::format("!!int 的取值 '{}' 不是合法整数（{}）", text, describeYamlMark(node.Mark())));
            }
            if (tag == "tag:yaml.org,2002:float")
            {
                if (const std::optional<double> floatingValue = parseCoreFloat(text))
                {
                    return ConfigValue(*floatingValue);
                }
                throw YamlConversionException(std::format("!!float 的取值 '{}' 不是合法浮点（{}）", text, describeYamlMark(node.Mark())));
            }
            if (tag != "?")
            {
                throw YamlConversionException(std::format("不支持的 YAML 标签 '{}'（{}）：请改用 !!str/!!int/!!float/!!bool/!!null 或去掉标签", tag,
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
         * @throws YamlConversionException 超过深度或节点数上限、映射键不是标量、映射存在重复键
         */
        [[nodiscard]] ConfigValue yamlNodeToConfigValue(const YAML::Node &node, const std::size_t depth, std::size_t &remainingNodeBudget)
        {
            if (depth > kMaximumYamlDepth)
            {
                throw YamlConversionException(std::format("文档嵌套或别名展开超过 {} 层：若文件里存在循环别名，请先解开锚点引用", kMaximumYamlDepth));
            }
            if (remainingNodeBudget == 0)
            {
                throw YamlConversionException(std::format("文档展开后的节点总数超过上限 {}：引用同一锚点的别名会被逐层复制，请减少别名引用", kMaximumYamlNodeCount));
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
                        throw YamlConversionException(std::format("映射的键必须是标量（{}）：复杂键无法表示为 JSON 对象键", describeYamlMark(entry.first.Mark())));
                    }
                    const std::string key = entry.first.Scalar();
                    if (members.contains(key))
                    {
                        throw YamlConversionException(std::format("映射中存在重复键 '{}'（{}）：请删除重复定义", key, describeYamlMark(entry.first.Mark())));
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
         * @throws YamlConversionException 超过展开或深度上限、含不支持的标签
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
         * @param text 文档文本
         * @return ConfigValue 文档根值
         * @throws nlohmann::json::exception 语法错误（消息自带行列与出错记号）
         */
        [[nodiscard]] ConfigValue parseJsonDocument(const std::string &text)
        {
            // nlohmann 不认 UTF-8 BOM，而 Windows 编辑器常写 BOM：先剥掉再解析
            constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF"};
            if (text.starts_with(kUtf8Bom))
            {
                return ConfigValue::parse(text.substr(kUtf8Bom.size()));
            }
            return ConfigValue::parse(text);
        }

        /**
         * @brief 按文件后缀选择原生解析器解析一份配置文档
         * @details .json 走 nlohmann_json；.yaml/.yml 走 yaml-cpp 再转换成 JSON 值模型。
         * @param text 文档文本
         * @param filePath 文件路径，仅用于挑选解析器
         * @return ConfigValue 文档根值
         * @throws nlohmann::json::exception JSON 语法非法
         * @throws YAML::Exception YAML 语法非法
         * @throws YamlConversionException YAML 文档无法忠实转换为 JSON 值模型
         */
        [[nodiscard]] ConfigValue parseDocumentBySuffix(const std::string &text, const std::filesystem::path &filePath)
        {
            if (isJsonFile(filePath.string()))
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
        return ConfigValue(buildNestedObject(entries));
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

    void ConfigManager::validateRegisteredSchema(const ConfigKeyValueMap &values) const
    {
        ConfigSchema schema;
        {
            const std::lock_guard lock(m_schemaMutex);
            schema = m_schema;
        }
        if (schema.empty())
        {
            return;
        }

        // 针对给定字典逐项校验（不依赖当前快照，供热重载提交路径使用）
        for (const auto &error: runSchemaValidation(values, schema).errors)
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
            result.errors.push_back("配置目录不存在：" + configDirectory.string());
            if (errorCode)
            {
                result.errors.push_back("错误：" + errorCode.message());
            }
            return result;
        }

        if (!std::filesystem::is_directory(configDirectory, errorCode))
        {
            result.success = false;
            result.errors.push_back("路径不是目录：" + configDirectory.string());
            return result;
        }

        // 扫描所有 JSON/YAML 配置文件
        const auto configFiles = scanConfigFiles(configDirectory, recursive);

        if (configFiles.empty())
        {
            // 目录中没有任何配置文件：提交空配置（等价于清空）。
            // 与 commitConfigData 共用同一把写锁——它同样是「整份快照替换」的写者，
            // 不加锁就会与并发的 setValue 互相覆盖
            const std::lock_guard writeLock(m_writeMutex);
            const auto newData       = std::make_shared<ConfigData>();
            newData->configDirectory = configDirectory;
            m_data.store(newData, std::memory_order_release);

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
                result.loadedFiles.push_back(filePath.string());
                for (auto &[key, value]: fileValues)
                {
                    values.insert_or_assign(std::move(key), std::move(value));
                }
            } else
            {
                result.failedFiles.push_back(filePath.string());
            }
        }

        // 全部失败时保留原有配置快照
        if (result.loadedFiles.empty())
        {
            result.success = false;
            return result;
        }

        commitConfigData(std::move(values), result.loadedFiles, configDirectory);
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
            errors.push_back(std::format("文件 '{}' 超过配置文件的体积上限（{} MiB）：请拆分或精简该文件", filePath.string(),
                                         kMaximumConfigFileBytes / (1024ULL * 1024ULL)));
            return false;
        }

        const std::optional<std::string> text = readTextFile(filePath);
        if (!text.has_value())
        {
            errors.push_back("无法打开文件 '" + filePath.string() + "'：文件不存在或不可读");
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

            if (!document.is_object())
            {
                // 沿用 YAML 习惯措辞：数组报 sequence，空值报 null，标量按类型名
                const std::string_view kindName = document.is_array() ? std::string_view{"sequence"}
                                                       : document.is_null() ? std::string_view{"null"}
                                                                            : std::string_view{typeName(document.type())};

                errors.push_back("文件 '" + filePath.string() + "'：根节点必须是映射，实际为 " + std::string(kindName));
                return false;
            }

            std::string prefixBuffer;
            flattenValue(document, prefixBuffer, values);
        } catch (const ConfigValue::exception &exception)
        {
            // 原生库的消息自带行列与出错记号，是定位所需的全部信息，原样带出
            errors.push_back(std::format("解析错误：'{}'：JSON 语法错误：{}", filePath.string(), exception.what()));
            return false;
        } catch (const YAML::Exception &exception)
        {
            errors.push_back(std::format("解析错误：'{}'：YAML 语法错误：{}（{}）", filePath.string(), exception.msg, describeYamlMark(exception.mark)));
            return false;
        } catch (const std::runtime_error &exception)
        {
            // YAML → JSON 值模型转换失败：本文件内抛出的中文原因（含位置）
            errors.push_back(std::format("解析错误：'{}'：{}", filePath.string(), exception.what()));
            return false;
        } catch (const std::exception &exception)
        {
            errors.push_back("加载 '" + filePath.string() + "' 时发生意外错误：" + exception.what());
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
        if (!isConfigFile(filePath))
        {
            return;
        }

        if (changeType != Platform::FileChangeType::Modified && changeType != Platform::FileChangeType::Created && changeType != Platform::FileChangeType::Deleted)
        {
            return;
        }

        // 已有任务在跑：记下「之后还要再来一轮」而不是直接丢弃。重载要读完整份目录，
        // 期间到达的变更（尤其是紧接着那次写入）会落在本轮之后——丢掉它配置就停在旧值，
        // 直到用户下一次改动；接力由当前那轮任务在收尾时完成
        if (bool expected = false; !m_reloadPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            m_reloadDirty.store(true, std::memory_order_release);
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
        const std::lock_guard lock(m_reloadTasksMutex);

        auto        task    = std::make_unique<ReloadTask>();
        ReloadTask *rawTask = task.get();
        task->thread        = std::jthread([this, rawTask]() { runReloadTask(rawTask); });
        m_reloadTasks.push_back(std::move(task));
    }

    void ConfigManager::runReloadTask(ReloadTask *rawTask)
    {
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
            LOG_EXCEPTION(LogLevel::Error, reloadError, "ConfigManager: 热重载任务抛出异常，本轮按失败处理：{}", reloadError.what());
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
                LOG_EXCEPTION(LogLevel::Error, notificationError, "ConfigManager: 通知「本轮重载失败」时回调又抛出异常，已忽略：{}", notificationError.what());
            }
        } catch (...)
        {
            LOG_ERROR_FMT("ConfigManager: 热重载任务抛出非标准异常，本轮按失败处理");
        }

        // 接力：先清 pending 再看 dirty——顺序反过来的话，「清 pending 之后、检查 dirty 之前」
        // 到达的变更会被漏掉。抢不到 pending 说明别的触发者已经接手，它那轮同样会看到 dirty
        m_reloadPending.store(false, std::memory_order_release);
        if (m_reloadDirty.exchange(false, std::memory_order_acq_rel))
        {
            if (bool expected = false; m_reloadPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                startReloadTask();
            }
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

        return loadFromDirectoryImplementation(currentData->configDirectory, true);
    }

    std::vector<std::filesystem::path> ConfigManager::scanConfigFiles(const std::filesystem::path &directory, const bool recursive)
    {
        std::vector<std::filesystem::path> configFiles;

        std::error_code errorCode;
        const auto      collect = [&configFiles](const auto &entry)
        {
            if (entry.is_regular_file() && isConfigFile(entry.path().string()))
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
            }
        }

        // 按文件名排序，保证加载顺序一致
        std::ranges::sort(configFiles);

        return configFiles;
    }

    void ConfigManager::commitConfigData(ConfigKeyValueMap                values,
                                         const std::vector<std::string> & loadedFiles,
                                         const std::filesystem::path &    configDirectory)
    {
        // 与 setValue() 共用同一把写锁：加载/热重载也是「整份快照替换」的写者，
        // 不串行化的话，与并发的 setValue 谁后发布谁生效，先发布的那些键会被整份覆盖掉
        const std::lock_guard writeLock(m_writeMutex);

        const auto newData       = std::make_shared<ConfigData>();
        newData->values          = std::move(values);
        newData->loadedFiles     = loadedFiles;
        newData->configDirectory = configDirectory;

        m_data.store(newData, std::memory_order_release);

        // 提交后自动校验已注册 schema，配置写错时第一时间在日志中暴露
        validateRegisteredSchema(newData->values);
    }
} // namespace AsynGyanis::Base
