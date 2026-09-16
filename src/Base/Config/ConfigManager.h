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
#include "Base/Exception/ConfigValidationException.h"
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
         *
         * @details 目录下的配置文件按**文件名升序**逐个装载，后装载的同名键覆盖先装载的
         *          （例如 `settings.json` 会覆盖 `config.yaml`，因为 's' 排在 'c' 之后）。
         *          这是本类唯一的优先级规则，靠**命名**表达「哪份是默认、哪份是覆盖」：
         *          想让某个文件生效得更晚，就给它排序更靠后的名字。
         *          这样调用方可以自行决定分层（部署默认 + 本地覆盖），本类不预设任何文件名。
         *
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
         * @tparam T 目标类型（bool、整型、浮点、std::string、ConfigArray、ConfigObject）。
         * @param key 配置键。
         * @return T 配置值。
         * @throws ConfigKeyNotFoundException 键不存在
         * @throws ConfigValidationException 类型不匹配（消息里带键名、期望与实际类型）
         */
        template<typename T>
        T get(const std::string_view key) const
        {
            using ValueType = std::decay_t<T>;

            const ConfigValue value = get(key);
            if (auto converted = configValueAs<ValueType>(value))
            {
                return std::move(*converted);
            }
            throw ConfigValidationException(std::string(key),
                                            std::string("配置值取用类型不匹配：期望 ") + configTypeNameOf<ValueType>() +
                                            "，实际 " + typeName(value.type()));
        }

        /**
         * @brief 获取指定类型的配置值
         * @param key 配置键
         * @param defaultValue 默认值（键不存在或类型不匹配时返回）
         * @return 配置值或默认值
         * @note 取值遵循严格口径（见 configValueAs）：不做取整、回绕与跨类型转换，
         *       类型对不上按「未配置」处理而不抛出。
         */
        template<typename T>
        T get(const std::string_view key, T &&defaultValue) const noexcept
        {
            const auto optionalValue = getOptional(key);
            if (!optionalValue)
            {
                return std::forward<T>(defaultValue);
            }

            if (auto converted = configValueAs<std::decay_t<T> >(*optionalValue))
            {
                return std::move(*converted);
            }
            return std::forward<T>(defaultValue);
        }

        /**
         * @brief 获取配置值，键不存在时抛出异常
         * @tparam T 目标类型。
         * @param key 配置键。
         * @return T 配置值。
         * @throws ConfigKeyNotFoundException 键不存在
         * @throws ConfigValidationException 类型不匹配（消息里带键名、期望与实际类型）
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
         * @brief 设置配置值并立即生效（原子替换内存快照）
         * @details 只改内存，**不写回任何文件**：本类负责读配置，写配置是应用层的事
         *          （它知道该写哪个文件、什么时候写、以及要不要问过运维）。
         *          因此本方法的效果不跨进程，重启后回到文件里的取值。
         * @param key 配置键（点号路径，如 server.port）。
         * @param value 配置值。
         * @return bool 成功返回 true；键为空返回 false。
         */
        bool setValue(std::string_view key, ConfigValue value);

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
         * @brief 取出某一段配置并还原成嵌套对象。
         * @details 内部按键的点号路径扁平存放（见 dump()），本方法把 sectionPrefix 下所有键的
         *          剩余路径重新聚成嵌套对象，供只认文档结构的消费方（如
         *          Net::readHttpServerConfiguration）直接使用。段名本身不是键（get("server")
         *          只会抛「键不存在」），要拿到整段取值只有这一条路径。
         * @param sectionPrefix 段名，如 "server"
         * @return ConfigValue **该段自身**的对象副本（结果里不含段名这一层，即返回值的键是
         *         "port" 而不是 "server.port"）；没有任何键落在该段时返回空对象，消费方可
         *         据此走默认值。返回的是脱离快照的副本，热重载不会改写已取走的这一份
         */
        [[nodiscard]] ConfigValue getSection(std::string_view sectionPrefix) const;

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

        std::mutex         m_writeMutex;       ///< 串行化 setValue 的「复制—修改—发布」事务，避免并发写者互相覆盖（读者不受影响）
        mutable std::mutex m_schemaMutex;      ///< 保护 m_schema 的互斥锁（const 校验方法也需加锁）
        ConfigSchema       m_schema;           ///< 全局 schema（setSchema 注册，提交快照时自动校验）

        // 热加载相关
        std::unique_ptr<Platform::FileWatcher>                 m_fileWatcher;                ///< 文件监控器（用于热加载）
        std::atomic<std::shared_ptr<const HotReloadCallback> > m_hotReloadCallback{nullptr}; ///< 热加载回调快照（enableHotReload 写、重载线程读）
        std::atomic<bool>                                      m_hotReloadEnabled{false};    ///< 热加载功能是否启用（true 启用，false 关闭）
        std::atomic<bool>                                      m_reloadPending{false};       ///< 是否有重载任务正在执行（节流）
        /// 重载进行期间又收到变更：由当前那轮任务在收尾时接力再来一轮。
        /// 直接丢弃会让「重载恰好读到半截文件」变成常态——配置一直停在旧值，直到用户下一次改动
        std::atomic<bool>                                      m_reloadDirty{false};
        std::mutex                                             m_reloadTasksMutex;           ///< 保护 m_reloadTasks 的互斥锁（仅登记/摘取句柄，join 不在锁内做）
        std::vector<std::unique_ptr<ReloadTask> >              m_reloadTasks;                ///< 活跃的重载任务（用于析构前 join）

        /**
         * @brief 取出并回收已结束的重载任务
         * @details 锁内只做「摘取」，锁外析构 unique_ptr（其析构会 join 线程）。
         *          这样一次长 reload 阻塞 join 时不会连带挡住其它热加载检查。
         */
        void collectFinishedReloadTasks();

        /**
         * @brief 登记并启动一轮重载任务（调用方需先占住 m_reloadPending）
         * @note 只负责登记与启动；任务的收尾（清 pending、接力下一轮）在任务体内
         */
        void startReloadTask();

        /**
         * @brief 重载任务体（在后台线程上跑）
         * @param rawTask 本轮任务的完成标记（任务列表持有它的所有权）
         */
        void runReloadTask(ReloadTask *rawTask);

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
         *          类型判定已在解析阶段完成，此处不再二次判定。
         *          点号前缀用同一个缓冲「追加—递归—回溯」复用，避免每层重复拼接父前缀；
         *          叶子值直接从文档中移出，省掉一次深拷贝。
         * @param node 当前文档值节点（必须是对象），其叶子值会被移出，故必须是可改写的临时对象
         * @param prefixBuffer 复用的点号前缀缓冲，进入时表示当前层前缀
         * @param values 扁平化结果容器
         */
        static void flattenValue(ConfigValue &node, std::string &prefixBuffer, ConfigKeyValueMap &values);

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
