#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/Logger.h"
#include "Platform/FileSystem/AtomicFileWriter.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <sstream>
#include <system_error>

namespace AsynGyanis::Base
{
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

        ConfigKeyValueMap values;
        for (const auto &filePath: filePaths)
        {
            if (!std::filesystem::exists(filePath))
            {
                result.failedFiles.push_back(filePath.string());
                result.errors.push_back("File does not exist: " + filePath.string());
                continue;
            }
            if (!isConfigFile(filePath.string()))
            {
                result.failedFiles.push_back(filePath.string());
                result.errors.push_back("Unsupported config file format (expected .json, .yaml or .yml): " + filePath.string());
                continue;
            }
            if (loadConfigFile(filePath, values, result.errors))
            {
                result.loadedFiles.push_back(filePath.string());
            } else
            {
                result.failedFiles.push_back(filePath.string());
            }
        }

        // 至少一个文件成功才提交，全部失败时保留原有配置
        if (!result.loadedFiles.empty())
        {
            commitConfigData(std::move(values), result.loadedFiles, result.timestamp);
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
            result.errors.emplace_back("No configuration directory set. Call loadFromDirectory first.");
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

            m_hotReloadCallback = std::move(callback);

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

        // 等待所有活跃的重载线程完成，防止 use-after-free
        {
            std::lock_guard lock(m_reloadTasksMutex);
            for (const auto &task: m_reloadTasks)
            {
                if (task && task->thread.joinable())
                {
                    task->thread.join();
                }
            }
            m_reloadTasks.clear();
        }

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
        const auto iterator    = currentData->values.find(std::string(key));
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
                return std::to_string(value.asInt());
            case ConfigValueType::Bool:
                return value.asBool() ? "true" : "false";
            case ConfigValueType::Double:
            {
                std::ostringstream stream;
                stream << value.asDouble();
                return stream.str();
            }
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

        // 复制当前快照并更新目标键，原子替换后立即对所有读者生效
        const auto currentData            = m_data.load(std::memory_order_acquire);
        const auto newData                = std::make_shared<ConfigData>(*currentData);
        newData->values[std::string(key)] = value;
        m_data.store(newData, std::memory_order_release);

        {
            const std::lock_guard lock(m_overrideMutex);
            m_pendingOverrides[std::string(key)] = std::move(value);
        }
        return true;
    }

    namespace
    {
        // -------- settings.json (user override layer) JSON output helpers --------

        /**
         * @brief 标量节点的 JSON 输出类别。
         */
        enum class ScalarKind
        {
            Integer, ///< 整数，裸写
            Float,   ///< 浮点，裸写
            Bool,    ///< 布尔，裸写
            Null,    ///< 空值，裸写
            String,  ///< 字符串，带引号
        };

        /**
         * @brief 推断 YAML 标量节点应归入的 JSON 输出类别。
         * @param node 标量节点。
         * @return ScalarKind 输出类别。
         */
        ScalarKind classifyScalar(const YAML::Node &node)
        {
            const std::string &tag = node.Tag();
            if (tag == "tag:yaml.org,2002:str" || tag == "!")
            {
                return ScalarKind::String;
            }
            if (tag == "tag:yaml.org,2002:int")
                return ScalarKind::Integer;
            if (tag == "tag:yaml.org,2002:float")
                return ScalarKind::Float;
            if (tag == "tag:yaml.org,2002:bool")
                return ScalarKind::Bool;
            if (tag == "tag:yaml.org,2002:null")
                return ScalarKind::Null;

            // Plain scalar: infer type from text (same rule as loading side convertNode)
            const std::string &text = node.Scalar();
            if (text.empty())
            {
                return ScalarKind::String;
            }
            if (text.size() <= 5)
            {
                std::string lower(text.size(), '\0');
                std::ranges::transform(text, lower.begin(),
                                       [](const unsigned char character)
                                       {
                                           return std::tolower(character);
                                       });
                if (lower == "true" || lower == "false" || lower == "yes" || lower == "no" ||
                    lower == "on" || lower == "off")
                {
                    return ScalarKind::Bool;
                }
            }
            const char *begin        = text.data();
            const char *end          = text.data() + text.size();
            int64_t     integerValue = 0;
            if (const auto [first, second] = std::from_chars(begin, end, integerValue);
                second == std::errc() && first == end)
            {
                return ScalarKind::Integer;
            }
            double floatingValue = 0.0;
            if (const auto [first, second] = std::from_chars(begin, end, floatingValue);
                second == std::errc() && first == end && std::isfinite(floatingValue))
            {
                return ScalarKind::Float;
            }
            return ScalarKind::String;
        }

        /**
         * @brief 待输出的标量值（类别 + 文本形式）。
         */
        struct ScalarValue
        {
            ScalarKind  kind{ScalarKind::String}; ///< JSON 输出类别
            std::string text;                     ///< 已格式化好的值文本
        };

        /**
         * @brief 将配置值转换为带类别的标量文本。
         * @param value 配置值。
         * @return ScalarValue 输出类别与文本。
         */
        ScalarValue toScalarValue(const ConfigValue &value)
        {
            ScalarValue result;
            switch (value.type())
            {
                case ConfigValueType::Bool:
                    result.kind = ScalarKind::Bool;
                    result.text = value.asBool() ? "true" : "false";
                    break;
                case ConfigValueType::Int:
                    result.kind = ScalarKind::Integer;
                    result.text = std::to_string(value.asInt());
                    break;
                case ConfigValueType::Double:
                {
                    result.kind = ScalarKind::Float;
                    std::ostringstream stream;
                    stream << value.asDouble();
                    result.text = stream.str();
                    if (result.text.find('.') == std::string::npos &&
                        result.text.find('e') == std::string::npos &&
                        result.text.find('E') == std::string::npos)
                    {
                        result.text += ".0";
                    }
                    break;
                }
                case ConfigValueType::String:
                    result.text = value.asString();
                    break;
                default:
                    result.kind = ScalarKind::Null;
                    break;
            }
            return result;
        }

        /**
         * @brief 转义 JSON 字符串内容。
         * @param text 原始文本。
         * @return std::string 不含外层引号的转义结果。
         */
        std::string jsonEscape(const std::string &text)
        {
            std::string escaped;
            escaped.reserve(text.size() + 8);
            for (const char character: text)
            {
                switch (character)
                {
                    case '"':
                        escaped += "\\\"";
                        break;
                    case '\\':
                        escaped += "\\\\";
                        break;
                    case '\b':
                        escaped += "\\b";
                        break;
                    case '\f':
                        escaped += "\\f";
                        break;
                    case '\n':
                        escaped += "\\n";
                        break;
                    case '\r':
                        escaped += "\\r";
                        break;
                    case '\t':
                        escaped += "\\t";
                        break;
                    default:
                        if (static_cast<unsigned char>(character) < 0x20)
                        {
                            escaped += std::format("\\u{:04x}", static_cast<unsigned int>(character));
                        } else
                        {
                            escaped += character;
                        }
                        break;
                }
            }
            return escaped;
        }

        /**
         * @brief 将标量渲染为 JSON 片段。
         * @param scalar 带类别的标量文本。
         * @return std::string JSON 值片段。
         */
        std::string jsonScalar(const ScalarValue &scalar)
        {
            switch (scalar.kind)
            {
                case ScalarKind::Null:
                    return "null";
                case ScalarKind::String:
                    return "\"" + jsonEscape(scalar.text) + "\"";
                default:
                    return scalar.text;
            }
        }

        /**
         * @brief 递归将配置值渲染为 JSON 片段
         * @details 标量沿用 jsonScalar 的分类输出（数字样字符串仍带引号）；数组与对象
         *          展开为 JSON 容器，避免界面写入的列表/字典落盘时被降级成 null。
         * @param value 待序列化的配置值
         * @return std::string JSON 值片段
         */
        std::string configValueToJson(const ConfigValue &value)
        {
            switch (value.type())
            {
                case ConfigValueType::Array:
                {
                    std::string output = "[";
                    for (const auto &element: value.asArray())
                    {
                        if (output.size() > 1)
                        {
                            output += ", ";
                        }
                        output += configValueToJson(element);
                    }
                    return output + "]";
                }
                case ConfigValueType::Object:
                {
                    std::string output = "{";
                    for (const auto &[key, element]: value.asObject())
                    {
                        if (output.size() > 1)
                        {
                            output += ", ";
                        }
                        output += "\"" + jsonEscape(key) + "\": " + configValueToJson(element);
                    }
                    return output + "}";
                }
                default:
                    return jsonScalar(toScalarValue(value));
            }
        }

        /**
         * @brief 递归将 YAML 节点追加为 JSON 文本。
         * @param output 输出缓冲。
         * @param node 当前 YAML 节点。
         */
        void appendJsonValue(std::string &output, const YAML::Node &node)
        {
            if (!node || node.IsNull())
            {
                output += "null";
                return;
            }
            switch (node.Type())
            {
                case YAML::NodeType::Map:
                {
                    output     += '{';
                    bool first = true;
                    for (auto iterator = node.begin(); iterator != node.end(); ++iterator)
                    {
                        if (!first)
                            output += ',';
                        first  = false;
                        output += "\"" + jsonEscape(iterator->first.as<std::string>()) + "\":";
                        appendJsonValue(output, iterator->second);
                    }
                    output += '}';
                    break;
                }
                case YAML::NodeType::Sequence:
                {
                    output     += '[';
                    bool first = true;
                    for (const auto &child: node)
                    {
                        if (!first)
                            output += ',';
                        first = false;
                        appendJsonValue(output, child);
                    }
                    output += ']';
                    break;
                }
                case YAML::NodeType::Scalar:
                    output += jsonScalar({.kind = classifyScalar(node), .text = node.Scalar()});
                    break;
                default:
                    output += "null";
                    break;
            }
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

        YAML::Node root;
        if (std::error_code errorCode; std::filesystem::exists(targetPath, errorCode))
        {
            try
            {
                root = YAML::LoadFile(targetPath.string());
            } catch (const std::exception &)
            {
                root = YAML::Node();
            }
        }
        if (!root.IsMap())
        {
            root = YAML::Node(YAML::NodeType::Map);
        }

        // 界面修改（扁平点号键）：JSON 顶层键可含点，读取端按“键已含点”原样使用。
        // 注意：pending 值不写入 YAML 节点（节点会丢失引号类型信息），序列化时按 ConfigValue 类型直接输出
        if (std::error_code errorCode; std::filesystem::exists(legacyPath, errorCode))
        {
            try
            {
                if (const YAML::Node legacy = YAML::LoadFile(legacyPath.string()); legacy.IsMap())
                {
                    for (auto iterator = legacy.begin(); iterator != legacy.end(); ++iterator)
                    {
                        if (iterator->second.IsScalar())
                        {
                            root[iterator->first.as<std::string>()] = iterator->second;
                        }
                    }
                }
            } catch (const std::exception &)
            {
            }
            std::error_code ignored;
            std::filesystem::remove(legacyPath, ignored);
        }

        // JSON 序列化（类型原生：字符串带引号、数字/布尔/空值裸写）
        std::string json         = "{\n";
        bool        first        = true;
        const auto  emitKeyValue = [&json, &first](const std::string &key, const std::string &valueJson)
        {
            if (!first)
                json += ",\n";
            first = false;
            json  += "  \"" + jsonEscape(key) + "\": " + valueJson;
        };
        for (auto iterator = root.begin(); iterator != root.end(); ++iterator)
        {
            const auto key = iterator->first.as<std::string>();
            if (const auto pendingValue = pending.find(key); pendingValue != pending.end())
            {
                emitKeyValue(key, configValueToJson(pendingValue->second));
            } else
            {
                std::string valueJson;
                appendJsonValue(valueJson, iterator->second);
                emitKeyValue(key, valueJson);
            }
        }
        // 追加尚未存在于 settings.json 的新修改键
        for (const auto &[key, value]: pending)
        {
            if (root[key].IsDefined())
            {
                continue;
            }
            emitKeyValue(key, configValueToJson(value));
        }
        json += "\n}\n";

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
            result.errors.push_back("Configuration directory does not exist: " + configDirectory.string());
            if (errorCode)
            {
                result.errors.push_back("Error: " + errorCode.message());
            }
            return result;
        }

        if (!std::filesystem::is_directory(configDirectory, errorCode))
        {
            result.success = false;
            result.errors.push_back("Path is not a directory: " + configDirectory.string());
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
        try
        {
            // yaml-cpp 兼容 YAML 1.2 核心模式，JSON 是 YAML 的子集，
            // 因此 JSON 与 YAML 共用同一解析路径。
            const YAML::Node root = YAML::LoadFile(filePath.string());

            if (root.IsNull())
            {
                // 空文件，不报错、无配置项
                return true;
            }

            if (!root.IsMap())
            {
                errors.push_back("File '" + filePath.string() +
                                 "': root node must be a map, got " +
                                 std::string(root.Type() == YAML::NodeType::Sequence ? "sequence" : root.Type() == YAML::NodeType::Scalar ? "scalar" : "null"));
                return false;
            }

            flattenNode(root, "", values);
            return true;
        } catch (const YAML::ParserException &exception)
        {
            errors.push_back("Parse error in '" + filePath.string() +
                             "': " + exception.what() + " at line " + std::to_string(exception.mark.line + 1) +
                             ", column " + std::to_string(exception.mark.column + 1));
        } catch (const YAML::BadFile &exception)
        {
            errors.push_back("Cannot open file '" + filePath.string() + "': " + exception.what());
        } catch (const std::exception &exception)
        {
            errors.push_back("Unexpected error loading '" + filePath.string() + "': " + exception.what());
        }
        return false;
    }

    void ConfigManager::flattenNode(const YAML::Node &node, const std::string &prefix, ConfigKeyValueMap &values)
    {
        if (!node.IsMap())
        {
            return;
        }

        for (const auto &keyValue: node)
        {
            const auto key = keyValue.first.as<std::string>();

            const std::string fullKey = prefix.empty() ? key : prefix + "." + key;
            if (const YAML::Node &valueNode = keyValue.second; valueNode.IsMap())
            {
                if (valueNode.size() == 0)
                {
                    // 空对象保留为独立的空 ConfigValue，避免数据丢失
                    values[fullKey] = convertNode(valueNode);
                } else
                {
                    // 非空嵌套对象：递归展开
                    flattenNode(valueNode, fullKey, values);
                }
            } else
            {
                // 叶子节点：转换为 ConfigValue 并存储
                values[fullKey] = convertNode(valueNode);
            }
        }
    }

    ConfigValue ConfigManager::convertNode(const YAML::Node &node)
    {
        if (node.IsNull())
        {
            return ConfigValue(nullptr);
        }
        if (node.IsScalar())
        {
            const auto scalar = node.as<std::string>();

            // 显式字符串标签（!!str）或带引号/块样式（yaml-cpp 中 tag 为 "!"）一律按字符串处理
            // （裸标量 tag 为 "?"，走后续的类型推断）
            if (const std::string &tag = node.Tag(); tag == "tag:yaml.org,2002:str" || tag == "!")
            {
                return ConfigValue(scalar);
            }

            // 空字符串边界：避免被误判为整数 0
            if (scalar.empty())
            {
                return ConfigValue(scalar);
            }

            // 布尔值检测：true/false, yes/no, on/off (大小写不敏感，YAML 1.1 兼容)
            if (scalar.size() <= 5)
            {
                std::string lower(scalar.size(), '\0');
                std::ranges::transform(scalar, lower.begin(),
                                       [](const unsigned char character)
                                       {
                                           return std::tolower(character);
                                       });
                if (lower == "true" || lower == "false" || lower == "yes" || lower == "no" ||
                    lower == "on" || lower == "off")
                {
                    return ConfigValue(lower == "true" || lower == "yes" || lower == "on");
                }
            }

            // 整数检测（from_chars 严格全量匹配，不跳过空白）
            {
                const char *begin        = scalar.data();
                const char *end          = scalar.data() + scalar.size();
                int64_t     integerValue = 0;
                if (const auto [first, second] = std::from_chars(begin, end, integerValue);
                    second == std::errc() && first == end)
                {
                    return ConfigValue(integerValue);
                }
            }

            // 浮点数检测（拒绝无穷/NaN 与超出 double 范围的值）
            {
                const char *begin         = scalar.data();
                const char *end           = scalar.data() + scalar.size();
                double      floatingValue = 0.0;
                if (const auto [first, second] = std::from_chars(begin, end, floatingValue);
                    second == std::errc() && first == end && std::isfinite(floatingValue))
                {
                    return ConfigValue(floatingValue);
                }
            }

            return ConfigValue(scalar);
        }
        if (node.IsSequence())
        {
            ConfigArray array;
            array.reserve(node.size());
            for (const auto &item: node)
            {
                array.push_back(convertNode(item));
            }
            return ConfigValue(std::move(array));
        }
        if (node.IsMap())
        {
            ConfigObject object;
            for (const auto &keyValue: node)
            {
                object[keyValue.first.as<std::string>()] = convertNode(keyValue.second);
            }
            return ConfigValue(std::move(object));
        }

        return ConfigValue(nullptr);
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

        std::lock_guard lock(m_reloadTasksMutex);

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

            if (m_hotReloadCallback)
            {
                m_hotReloadCallback(result);
            }

            m_reloadPending.store(false, std::memory_order_release);
            rawTask->finished.store(true, std::memory_order_release);
        });
        m_reloadTasks.push_back(std::move(task));

        // 清理已完成任务并回收线程资源，防止任务列表无限增长
        for (auto iterator = m_reloadTasks.begin(); iterator != m_reloadTasks.end();)
        {
            if ((*iterator)->finished.load(std::memory_order_acquire))
            {
                if ((*iterator)->thread.joinable())
                {
                    (*iterator)->thread.join();
                }
                iterator = m_reloadTasks.erase(iterator);
            } else
            {
                ++iterator;
            }
        }
    }

    ConfigLoadResult ConfigManager::doReload()
    {
        std::unique_lock lock(m_reloadMutex);

        const auto currentData = m_data.load(std::memory_order_acquire);
        if (currentData->configDirectory.empty())
        {
            ConfigLoadResult result;
            result.success = false;
            result.errors.emplace_back("No configuration directory set. Call loadFromDirectory first.");
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
