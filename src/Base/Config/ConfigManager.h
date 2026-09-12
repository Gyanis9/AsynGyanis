/**
 * @file ConfigManager.h
 * @brief 配置管理器核心接口
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigKeyValueMap.h"
#include "Base/Config/ConfigLoadResult.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Config/ConfigValidationResult.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Format/Value/FormatValue.h"
#include "Platform/FileSystem/FileWatcher.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置管理器类
     *
     * 配置管理器，提供以下核心功能：
     *   - 从指定目录递归加载所有 .json/.yml/.yaml 配置文件
     *   - 扁平化存储配置键值对
     *   - 线程安全的读写访问（atomic shared_ptr 无锁热替换）
     *   - 热加载支持（监听配置文件变更，自动重载）
     *   - 类型安全的配置值访问
     *
     * 使用单例模式（Meyers' Singleton）确保全局唯一实例。
     * 加载失败时保留上一次的有效配置快照。
     *
     * 使用示例：
     * @code
     *   // 初始化
     *   auto& configuration = ConfigManager::instance();
     *   auto result = configuration.loadFromDirectory("./config");
     *   if (!result) {
     *       for (const auto& error : result.errors) {
     *           LOG_ERROR(error);
     *       }
     *       return -1;
     *   }
     *
     *   // 启用热加载
     *   configuration.enableHotReload([](const ConfigLoadResult& reloadResult) {
     *       LOG_INFO("Config reloaded, {} files", reloadResult.loadedFiles.size());
     *   });
     *
     *   // 读取配置
     *   auto port = configuration.get<int64_t>("server.port", 8080);
     *   auto host = configuration.get<std::string>("server.host", "0.0.0.0");
     *   auto debug = configuration.get<bool>("debug.enabled", false);
     *
     *   // 检查键是否存在
     *   if (configuration.has("database.url")) {
     *       auto url = configuration.get<std::string>("database.url");
     *   }
     * @endcode
     */
    class ConfigManager
    {
    public:
        /**
         * @brief 获取配置管理器单例。
         * @return ConfigManager& 单例引用。
         */
        static ConfigManager &instance() noexcept;

        ConfigManager(const ConfigManager &) = delete;

        ConfigManager &operator=(const ConfigManager &) = delete;

        ConfigManager(ConfigManager &&) = delete;

        ConfigManager &operator=(ConfigManager &&) = delete;

        /**
         * @brief 从目录加载 JSON/YAML 配置文件。
         * @param configDirectory 配置目录。
         * @param recursive 是否递归扫描子目录。
         * @return ConfigLoadResult 加载结果。
         */
        ConfigLoadResult loadFromDirectory(const std::filesystem::path &configDirectory, bool recursive = true);

        /**
         * @brief 加载指定文件列表中的配置（支持 .json/.yaml/.yml）。
         * @details 至少一个文件成功时提交合并结果；全部失败时保留原配置。
         *          文件全部同处一个目录时顺带把该目录记为配置目录，使后续 reload() 与
         *          enableHotReload() 有据可依；跨目录或含相对路径时保留既有配置目录不覆盖。
         * @param filePaths 配置文件路径列表。
         * @return ConfigLoadResult 加载结果。
         */
        ConfigLoadResult loadFiles(const std::vector<std::filesystem::path> &filePaths);

        /**
         * @brief 使用当前目录配置执行一次重载。
         * @return ConfigLoadResult 重载结果。
         */
        ConfigLoadResult reload();

        /**
         * @brief 启用热加载（监听配置文件变更，自动重载）
         * @details 回调在**重载工作线程**（不是文件监听线程、也不是调用方线程）中执行，
         *          因此回调内可以安全地做耗时处理，但不应再回调用方持有的非线程安全状态。
         *          回调以原子共享指针快照方式持有：写入发生在 enableHotReload（启动监听之前），
         *          读取发生在重载线程，二者有明确的 acquire/release 同步。
         * @param callback 热加载完成后的回调函数
         * @param debounceMilliseconds 防抖间隔（毫秒），默认 500ms
         * @return bool 成功返回 true；重复调用返回 true；未加载目录或平台不支持返回 false
         */
        bool enableHotReload(HotReloadCallback callback = nullptr, std::chrono::milliseconds debounceMilliseconds = std::chrono::milliseconds(500));

        /**
         * @brief 关闭热重载监听并释放监听资源。
         */
        void disableHotReload();

        /**
         * @brief 查询热重载当前是否启用。
         * @return bool 启用返回 true。
         */
        [[nodiscard]] bool isHotReloadEnabled() const noexcept;

        /**
         * @brief 按键获取配置值，键不存在时抛出异常。
         * @param key 配置键。
         * @return ConfigValue 配置值副本。
         * @throws ConfigKeyNotFoundException 键不存在时抛出。
         */
        ConfigValue get(std::string_view key) const;

        /**
         * @brief 安全获取配置值。
         * @param key 配置键。
         * @return std::optional<ConfigValue> 键存在时返回值，否则为空。
         */
        [[nodiscard]] std::optional<ConfigValue> getOptional(std::string_view key) const noexcept;

        /**
         * @brief 获取指定类型的配置值，键不存在或类型不匹配时抛出异常。
         * @tparam T 目标类型。
         * @param key 配置键。
         * @return T 配置值。
         * @throws ConfigKeyNotFoundException 键不存在
         * @throws ValueAccessError 类型不匹配（由 FormatValue::as<T>() 抛出，异常里带键名与期望/实际类型）
         */
        template<typename T>
        T get(const std::string_view key) const
        {
            return get(key).template as<T>();
        }

        /**
         * @brief 获取指定类型的配置值
         * @param key 配置键
         * @param defaultValue 默认值（键不存在或类型不匹配时返回）
         * @return 配置值或默认值
         */
        template<typename T>
        T get(const std::string_view key, T &&defaultValue) const noexcept
        {
            const auto optionalValue = getOptional(key);
            if (!optionalValue)
            {
                return std::forward<T>(defaultValue);
            }

            auto typed = optionalValue->get<std::decay_t<T> >();
            return typed.value_or(std::forward<T>(defaultValue));
        }

        /**
         * @brief 获取配置值，键不存在时抛出异常
         * @tparam T 目标类型。
         * @param key 配置键。
         * @return T 配置值。
         * @throws ConfigKeyNotFoundException 键不存在
         * @throws ValueAccessError 类型不匹配（由 FormatValue::as<T>() 抛出，异常里带键名与期望/实际类型）
         */
        template<typename T>
        T getRequired(const std::string_view key) const
        {
            return get<T>(key);
        }

        /**
         * @brief 读取布尔配置值，缺失或类型不匹配时返回默认值。
         * @param key 配置键。
         * @param defaultValue 默认值。
         * @return bool 配置值或默认值。
         */
        bool getBool(std::string_view key, bool defaultValue = false) const noexcept;

        /**
         * @brief 读取整型配置值，缺失或类型不匹配时返回默认值。
         * @param key 配置键。
         * @param defaultValue 默认值。
         * @return int64_t 配置值或默认值。
         */
        int64_t getInt(std::string_view key, int64_t defaultValue = 0) const noexcept;

        /**
         * @brief 读取浮点配置值，缺失或类型不匹配时返回默认值。
         * @param key 配置键。
         * @param defaultValue 默认值。
         * @return double 配置值或默认值。
         */
        double getDouble(std::string_view key, double defaultValue = 0.0) const noexcept;

        /**
         * @brief 读取字符串配置值，缺失或类型不匹配时返回默认值。
         * @param key 配置键。
         * @param defaultValue 默认值。
         * @return std::string 配置值或默认值。
         */
        std::string getString(std::string_view key, const std::string &defaultValue = "") const;

        /**
         * @brief 宽松字符串读取：字符串原样返回；数字/布尔自动转为文本。
         * @details 用于内容像数字但语义为字符串的键（如纯数字密码），
         *          免疫配置类型推断差异导致的取空问题。
         * @param key 配置键（点号路径）。
         * @param defaultValue 键缺失时返回的默认值。
         * @return std::string 文本化取值或默认值。
         */
        std::string getText(std::string_view key, const std::string &defaultValue = "") const;

        /**
         * @brief 设置配置值并立即生效（原子替换内存快照）。
         * @details 修改同时记入待持久化集合，调用 saveOverrides() 后写入用户覆盖层 settings.json。
         * @param key 配置键（点号路径，如 server.port）。
         * @param value 配置值。
         * @return bool 成功返回 true。
         */
        bool setValue(std::string_view key, ConfigValue value);

        /**
         * @brief 把 setValue 累积的待保存修改合并进覆盖层 settings.json
         *
         * @details 只改覆盖层，不动部署默认文件（config.yaml / config.json 保持只读）：
         *          覆盖层里既有的其它键原样保留，本次修改覆盖同名键，最后经原子写整份替换，
         *          避免中断留下半截文件。存储为 JSON，类型原生自描述。
         *
         * @return true 修改已落盘，**或无待保存内容**（此时不触碰文件）
         * @return false 尚无配置目录（未通过 loadFromDirectory/loadFiles 装载过），或写文件失败
         * @note 保存成功后待保存集合中对应的键会被清空，因此重复调用是幂等的
         */
        bool saveOverrides();

        /**
         * @brief 设置配置值并立即持久化（setValue + saveOverrides 的组合）。
         * @param key 配置键（点号路径）。
         * @param value 配置值。
         * @return bool 内存修改与持久化均成功返回 true。
         */
        bool setAndPersist(std::string_view key, ConfigValue value);

        /**
         * @brief 按 schema 校验当前配置快照。
         * @param schema 约束条目列表。
         * @return ConfigValidationResult 校验结果，errors 逐条描述问题键。
         */
        [[nodiscard]] ConfigValidationResult validateSchema(const ConfigSchema &schema) const;

        /**
         * @brief 注册全局 schema，后续加载/热重载提交时自动校验并记录错误日志。
         * @details 传入空 schema 可取消注册；注册时立即对当前快照校验一次并记日志。
         * @param schema 约束条目列表。
         * @return ConfigValidationResult 当前快照的校验结果。
         */
        ConfigValidationResult setSchema(ConfigSchema schema);

        /**
         * @brief 检查配置键是否存在。
         * @param key 配置键。
         * @return bool 存在返回 true。
         */
        [[nodiscard]] bool has(std::string_view key) const noexcept;

        /**
         * @brief 返回所有配置键并按字典序排序。
         * @return std::vector<std::string> 配置键列表。
         */
        [[nodiscard]] std::vector<std::string> keys() const;

        /**
         * @brief 导出当前配置快照。
         * @return ConfigKeyValueMap 配置字典副本。
         */
        [[nodiscard]] ConfigKeyValueMap dump() const;

        /**
         * @brief 获取当前已加载文件列表。
         * @return std::vector<std::string> 文件路径列表。
         */
        [[nodiscard]] std::vector<std::string> loadedFiles() const;

        /**
         * @brief 获取当前配置目录。
         * @return std::filesystem::path 配置目录路径。
         */
        [[nodiscard]] std::filesystem::path configDirectory() const;

        /**
         * @brief 清空所有配置数据。
         */
        void clear();

        /**
         * @brief 校验必需配置键是否都已存在。
         * @param requiredKeys 必需键列表。
         * @return std::vector<std::string> 缺失键列表。
         */
        std::vector<std::string> validateRequired(const std::vector<std::string> &requiredKeys) const;

    private:
        /**
         * @brief 私有构造函数，配合 instance() 约束单例入口。
         */
        ConfigManager() = default;

        /**
         * @brief 析构时自动关闭热重载监听。
         */
        ~ConfigManager();

        /**
         * @brief 内部配置数据容器
         * @details 使用原子共享指针（atomic shared_ptr）实现无锁热替换。
         */
        struct ConfigData
        {
            ConfigKeyValueMap values; ///< 配置键值对映射表（支持 string_view 异质查找）

            std::vector<std::string>              loadedFiles;     ///< 成功加载的配置文件路径列表
            std::filesystem::path                 configDirectory; ///< 配置目录的路径
            std::chrono::steady_clock::time_point loadTime;        ///< 配置加载完成的时间戳（单调时钟）
        };

        /**
         * @brief 热重载任务及其完成标记（用于回收线程资源）。
         * @note 成员声明顺序即析构顺序的逆序：这里让 jthread 先于 finished 析构，
         *       于是「join 等待线程退出」发生在 finished 被销毁之前——否则运行中的
         *       线程会写已析构的 finished（虽为平凡析构，仍属生命周期错误）。
         */
        struct ReloadTask
        {
            std::atomic<bool> finished{false}; ///< 任务是否已结束
            std::jthread      thread;          ///< 后台重载线程（析构时自动 join）
        };

        std::atomic<std::shared_ptr<ConfigData> > m_data{std::make_shared<ConfigData>()}; ///< 当前有效的配置数据原子指针，支持无锁热替换

        mutable std::shared_mutex m_reloadMutex; ///< 用于配置数据构建过程的读写锁，仅在修改时加写锁

        mutable std::mutex m_overrideMutex;    ///< 保护待持久化覆盖集的互斥锁
        ConfigKeyValueMap  m_pendingOverrides; ///< 待写入 settings.json 的修改集合（setValue 累积，saveOverrides 清空）
        std::mutex         m_writeMutex;       ///< 串行化 setValue 的「复制—修改—发布」事务，避免并发写者互相覆盖（读者不受影响）
        mutable std::mutex m_schemaMutex;      ///< 保护 m_schema 的互斥锁（const 校验方法也需加锁）
        ConfigSchema       m_schema;           ///< 全局 schema（setSchema 注册，提交快照时自动校验）

        // 热加载相关
        std::unique_ptr<Platform::FileWatcher>                 m_fileWatcher;                ///< 文件监控器（用于热加载）
        std::atomic<std::shared_ptr<const HotReloadCallback> > m_hotReloadCallback{nullptr}; ///< 热加载回调快照（enableHotReload 写、重载线程读）
        std::atomic<bool>                                      m_hotReloadEnabled{false};    ///< 热加载功能是否启用（true 启用，false 关闭）
        std::atomic<bool>                                      m_reloadPending{false};       ///< 是否有重载任务正在执行（节流）
        std::mutex                                             m_reloadTasksMutex;           ///< 保护 m_reloadTasks 的互斥锁（仅登记/摘取句柄，join 不在锁内做）
        std::vector<std::unique_ptr<ReloadTask> >              m_reloadTasks;                ///< 活跃的重载任务（用于析构前 join）

        /**
         * @brief 取出并回收已结束的重载任务
         * @details 锁内只做「摘取」，锁外析构 unique_ptr（其析构会 join 线程）。
         *          这样一次长 reload 阻塞 join 时不会连带挡住其它热加载检查。
         */
        void collectFinishedReloadTasks();

        /**
         * @brief 目录加载的内部实现，负责扫描、解析并原子替换配置快照。
         * @param configDirectory 配置目录。
         * @param recursive 是否递归扫描。
         * @return ConfigLoadResult 加载结果。
         */
        ConfigLoadResult loadFromDirectoryImplementation(const std::filesystem::path &configDirectory, bool recursive);

        /**
         * @brief 读取并扁平化单个 JSON/YAML 配置文件到配置字典。
         * @param filePath 配置文件路径。
         * @param values 目标配置字典。
         * @param errors 错误信息收集容器。
         * @return bool 加载成功返回 true。
         */
        static bool loadConfigFile(const std::filesystem::path &filePath, ConfigKeyValueMap &values, std::vector<std::string> &errors);

        /**
         * @brief 递归扁平化文档值，将嵌套键转换为点号路径。
         * @details 只向下展开对象节点；数组与标量作为叶子值存入，
         *          类型推断已在解析器内完成，此处不再二次判定。
         *          点号前缀用同一个缓冲「追加—递归—回溯」复用，避免每层重复拼接父前缀；
         *          叶子值直接从文档中移出，省掉一次深拷贝。
         * @param node 当前文档值节点（必须是对象），其叶子值会被移出，故必须是可改写的临时对象
         * @param prefixBuffer 复用的点号前缀缓冲，进入时表示当前层前缀
         * @param values 扁平化结果容器
         */
        static void flattenValue(FormatValue &node, std::string &prefixBuffer, ConfigKeyValueMap &values);

        /**
         * @brief 处理文件监听回调并触发后台重载。
         * @param filePath 发生变化的文件路径。
         * @param changeType 文件变更类型。
         */
        void handleFileChange(std::string_view filePath, Platform::FileChangeType changeType);

        /**
         * @brief 在互斥保护下执行一次目录重载。
         * @return ConfigLoadResult 重载结果。
         */
        ConfigLoadResult doReload();

        /**
         * @brief 扫描目录中的 JSON/YAML 配置文件并按路径排序。
         * @param directory 待扫描目录。
         * @param recursive 是否递归。
         * @return std::vector<std::filesystem::path> 配置文件路径列表。
         */
        static std::vector<std::filesystem::path> scanConfigFiles(const std::filesystem::path &directory, bool recursive);

        /**
         * @brief 原子提交新的配置快照。
         * @param values 扁平化配置字典。
         * @param loadedFiles 成功加载的文件列表。
         * @param timestamp 加载时间戳。
         * @param configDirectory 配置目录（为空时保留原目录）。
         */
        void commitConfigData(ConfigKeyValueMap                     values,
                              const std::vector<std::string> &      loadedFiles,
                              std::chrono::steady_clock::time_point timestamp,
                              const std::filesystem::path &         configDirectory = {});

        /**
         * @brief 对指定配置字典执行已注册 schema 的校验并记录错误日志。
         * @param values 扁平化配置字典。
         */
        void validateRegisteredSchema(const ConfigKeyValueMap &values) const;
    };
} // namespace AsynGyanis::Base
