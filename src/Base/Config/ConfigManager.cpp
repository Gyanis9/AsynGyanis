#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"
#include "Base/Format/Json/JsonParser.h"
#include "Base/Format/Json/JsonWriter.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/Yaml/YamlParser.h"
#include "Platform/FileSystem/AtomicFileWriter.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 从一批配置文件推导配置目录
         * @details 只有全部文件同处一个目录时才据此设定配置目录；出现相对路径或跨目录时
         *          返回空路径，表示不猜测一个并不存在的配置目录，由调用方保留既有取值。
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
         * @details 用 std::to_chars 写进栈上缓冲再一次性构造字符串：既不做本地化，
         *          也不像 std::to_string/std::ostringstream 那样依赖流状态或多次扩容。
         * @tparam Number 算术类型
         * @param value 待文本化的数值
         * @return std::string 数值文本；极端情况下（缓冲区溢出）返回空串
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
            commitConfigData(std::move(values), result.loadedFiles, result.timestamp, directoryToCommit);
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

    std::optional<ConfigValue> ConfigManager::getOptional(const std::string_view key) const noexcept
    {
        const auto currentData = m_data.load(std::memory_order_acquire);

        const auto iterator = currentData->values.find(key);
        if (iterator == currentData->values.end())
        {
            return std::nullopt;
        }
        return iterator->second;
    }

    bool ConfigManager::getBool(const std::string_view key, bool defaultValue) const noexcept
    {
        return get<bool>(key, std::move(defaultValue));
    }

    int64_t ConfigManager::getInt(const std::string_view key, int64_t defaultValue) const noexcept
    {
        return get<int64_t>(key, std::move(defaultValue));
    }

    double ConfigManager::getDouble(const std::string_view key, double defaultValue) const noexcept
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

        // 宽松取值：字符串原样；数字/布尔文本化（免疫类型推断差异）
        switch (const ConfigValue &value = iterator->second; value.type())
        {
            case ConfigValueType::String:
                return value.asString();
            case ConfigValueType::Int:
                return numberToText(value.asInt());
            case ConfigValueType::Bool:
                return value.asBool() ? "true" : "false";
            case ConfigValueType::Double:
                return numberToText(value.asDouble());
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

        // 键字符串只构造一次：快照与待持久化集合共用同一份文本
        const std::string ownedKey(key);
        newData->values[ownedKey] = value;
        m_data.store(newData, std::memory_order_release);

        {
            const std::lock_guard lock(m_overrideMutex);
            m_pendingOverrides[ownedKey] = std::move(value);
        }
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

        /**
         * @brief 按文件后缀选择解析器解析一份配置文档
         * @details .json 走 JsonParser，其余（.yaml/.yml）走 YamlParser。
         *          类型推断由解析器在读取时一次完成，配置侧不再二次判定。
         * @param text 文档文本
         * @param filePath 文件路径，仅用于挑选后缀
         * @return FormatValue 文档根值
         * @throws FormatError 语法非法或使用了不支持的 YAML 特性
         */
        [[nodiscard]] FormatValue parseDocumentBySuffix(const std::string_view text, const std::filesystem::path &filePath)
        {
            return isJsonFile(filePath.string()) ? JsonParser::parse(text) : YamlParser::parse(text);
        }

        /**
         * @brief 把一份扁平文档解析成覆盖层键值表
         * @details 解析失败或根节点不是对象时返回空表：覆盖层保存宁可丢弃损坏的旧内容，
         *          也不让整次写盘失败并丢掉本次有效修改。
         * @param text 文档文本
         * @param filePath 文件路径，仅用于挑选解析器
         * @return FormatValueObject 顶层键值表
         */
        [[nodiscard]] FormatValueObject parseFlatMembers(const std::string_view text, const std::filesystem::path &filePath)
        {
            try
            {
                FormatValue document = parseDocumentBySuffix(text, filePath);
                if (const auto members = std::get_if<FormatValueObject>(&document.variant()); members != nullptr)
                {
                    return std::move(*members);
                }
            } catch (const FormatError &)
            {
            }
            return {};
        }
    } // namespace

    bool ConfigManager::saveOverrides()
    {
        ConfigKeyValueMap pending;
        {
            const std::lock_guard lock(m_overrideMutex);
            if (m_pendingOverrides.empty())
            {
                return true;
            }
            pending = m_pendingOverrides;
        }

        const auto currentData = m_data.load(std::memory_order_acquire);
        if (currentData->configDirectory.empty())
        {
            return false;
        }

        const std::filesystem::path targetPath = currentData->configDirectory / "settings.json";
        const std::filesystem::path legacyPath = currentData->configDirectory / "ui.yaml";

        // 既有覆盖层内容先并入；损坏或根节点不是对象时按空表处理，不阻塞本次保存
        FormatValueObject members;
        if (const std::optional<std::string> existingText = readTextFile(targetPath); existingText.has_value())
        {
            members = parseFlatMembers(*existingText, targetPath);
        }

        // 历史 ui.yaml 覆盖残留：只并入标量项（扁平点号键），随后删除旧文件完成迁移
        if (const std::optional<std::string> legacyText = readTextFile(legacyPath); legacyText.has_value())
        {
            for (auto &[key, value]: parseFlatMembers(*legacyText, legacyPath))
            {
                if (value.type() != FormatValueType::Object && value.type() != FormatValueType::Array)
                {
                    members.insert_or_assign(key, std::move(value));
                }
            }

            std::error_code ignored;
            std::filesystem::remove(legacyPath, ignored);
        }

        // 本次界面修改最后写入，同名旧值以本次为准
        for (const auto &[key, value]: pending)
        {
            members.insert_or_assign(key, value);
        }

        // 类型原生序列化：字符串带引号、整数与浮点裸写，缩进两空格便于人工编辑
        const std::string json = JsonWriter::write(FormatValue(std::move(members)), true) + "\n";

        // 断电安全：经原子写替换，避免中断留下半截 settings.json
        std::string writeError;
        if (!Platform::AtomicFileWriter::writeText(targetPath, json, {}, &writeError))
        {
            LOG_ERROR_FMT("保存用户设置失败：{}", writeError);
            return false;
        }

        {
            const std::lock_guard lock(m_overrideMutex);
            for (const auto &key: pending | std::views::keys)
            {
                m_pendingOverrides.erase(key);
            }
        }
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

    bool ConfigManager::setAndPersist(const std::string_view key, ConfigValue value)
    {
        if (!setValue(key, std::move(value)))
        {
            return false;
        }
        return saveOverrides();
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

    // ============================================================================
    // 内部加载实现
    // ============================================================================

    ConfigLoadResult ConfigManager::loadFromDirectoryImplementation(const std::filesystem::path &configDirectory, const bool recursive)
    {
        ConfigLoadResult result;
        result.timestamp = std::chrono::steady_clock::now();

        // 检查目录是否存在
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
            // 目录中没有任何配置文件：提交空配置（等价于清空）
            const auto newData       = std::make_shared<ConfigData>();
            newData->configDirectory = configDirectory;
            newData->loadTime        = result.timestamp;
            m_data.store(newData, std::memory_order_release);

            result.success = true;
            return result;
        }

        ConfigKeyValueMap values;

        for (const auto &filePath: configFiles)
        {
            if (loadConfigFile(filePath, values, result.errors))
            {
                result.loadedFiles.push_back(filePath.string());
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

        commitConfigData(std::move(values), result.loadedFiles, result.timestamp, configDirectory);
        result.success = result.failedFiles.empty();
        return result;
    }

    bool ConfigManager::loadConfigFile(const std::filesystem::path &filePath, ConfigKeyValueMap &values, std::vector<std::string> &errors)
    {
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
            FormatValue document = parseDocumentBySuffix(*text, filePath);

            if (document.type() != FormatValueType::Object)
            {
                // 沿用 YAML 习惯措辞：数组报 sequence，保持既有错误文案与用例一致
                const std::string_view kindName = document.type() == FormatValueType::Array
                                                      ? std::string_view{"sequence"}
                                                      : document.type() == FormatValueType::Null
                                                      ? std::string_view{"null"}
                                                      : std::string_view{typeName(document.type())};

                errors.push_back("文件 '" + filePath.string() + "'：根节点必须是映射，实际为 " + std::string(kindName));
                return false;
            }

            std::string prefixBuffer;
            flattenValue(document, prefixBuffer, values);
        } catch (const FormatError &exception)
        {
            errors.push_back("解析错误：'" + filePath.string() + "'：" + exception.reason() + "（" +
                             exception.position().describe() + "）");
            return false;
        } catch (const std::exception &exception)
        {
            errors.push_back("加载 '" + filePath.string() + "' 时发生意外错误：" + exception.what());
            return false;
        }

        return true;
    }

    void ConfigManager::flattenValue(FormatValue &node, std::string &prefixBuffer, ConfigKeyValueMap &values)
    {
        // 用可写 variant 取成员表：FormatValue 只暴露常量访问器，
        // 而叶子值需要被移出节点（见下方 std::move），因此必须拿到可写引用
        auto *members = std::get_if<FormatValueObject>(&node.variant());
        if (members == nullptr)
        {
            return;
        }

        for (auto &[key, value]: *members)
        {
            // 追加本层键后递归，返回时把缓冲回退到进入本层前的长度：
            // 整棵子树共用同一个前缀缓冲，路径字符只写一次而不是每层重新拼一遍父前缀
            const std::size_t prefixLength = prefixBuffer.size();
            if (prefixLength != 0)
            {
                prefixBuffer.push_back('.');
            }
            prefixBuffer += key;

            const auto *nestedMembers = std::get_if<FormatValueObject>(&value.variant());
            if (nestedMembers != nullptr && !nestedMembers->empty())
            {
                // 非空嵌套对象：递归展开为点号路径
                flattenValue(value, prefixBuffer, values);
            } else
            {
                // 叶子（标量、数组、空对象）：类型已由解析器判定完毕，值直接移出文档
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

        if (bool expected = false; !m_reloadPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return;
        }

        {
            const std::lock_guard lock(m_reloadTasksMutex);

            auto        task    = std::make_unique<ReloadTask>();
            ReloadTask *rawTask = task.get();
            task->thread        = std::jthread([this, rawTask]()
            {
                if (!m_hotReloadEnabled.load(std::memory_order_acquire))
                {
                    m_reloadPending.store(false, std::memory_order_release);
                    rawTask->finished.store(true, std::memory_order_release);
                    return;
                }
                const auto result = doReload();

                // 回调快照：本线程（重载工作线程）读到的是 enableHotReload 发布的那一份
                if (const auto callback = m_hotReloadCallback.load(std::memory_order_acquire); callback && *callback)
                {
                    (*callback)(result);
                }

                m_reloadPending.store(false, std::memory_order_release);
                rawTask->finished.store(true, std::memory_order_release);
            });
            m_reloadTasks.push_back(std::move(task));
        }
        // 清理已完成任务并回收线程资源，防止任务列表无限增长。
        // join 在锁外执行：持 m_reloadTasksMutex 期间 join 会把「一次长 reload 的等待」
        // 变成对所有后续热加载检查的阻塞
        collectFinishedReloadTasks();
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

    void ConfigManager::commitConfigData(ConfigKeyValueMap                           values,
                                         const std::vector<std::string> &            loadedFiles,
                                         const std::chrono::steady_clock::time_point timestamp,
                                         const std::filesystem::path &               configDirectory)
    {
        const auto newData       = std::make_shared<ConfigData>();
        newData->values          = std::move(values);
        newData->loadedFiles     = loadedFiles;
        newData->configDirectory = configDirectory;
        newData->loadTime        = timestamp;

        m_data.store(newData, std::memory_order_release);

        // 提交后自动校验已注册 schema，配置写错时第一时间在日志中暴露
        validateRegisteredSchema(newData->values);
    }
} // namespace AsynGyanis::Base
