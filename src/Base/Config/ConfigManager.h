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
#include "Base/Config/Detail/ReloadRoundGate.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Platform/FileSystem/FileWatcher.h"

#include <atomic>
#include <chrono>
#include <expected>
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
     * @brief 配置管理器
     *
     * @details 从目录递归加载 .json/.yml/.yaml，扁平化成点号路径的键值对，以原子共享指针
     *          做无锁热替换；读取与写入都拿当前快照，加载失败时保留上一份有效配置。
     * @note 单例（Meyers' Singleton）。数值与布尔取值遵循严格口径（见 configValueAs），
     *       不做取整、回绕与跨类型转换。
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
         * @details 按文件名升序逐个装载，后装载的同名键覆盖先装载的（settings.json 覆盖 config.yaml）。
         *          这是本类唯一的优先级规则，靠命名表达「哪份是默认、哪份是覆盖」，不预设任何文件名。
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
         *          该锚点目录此后重扫的递归口径：沿用快照里同一目录已有的取值，只有锚点换成
         *          另一个目录时才按递归起步。
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
         * @details 回调在**重载工作线程**上执行（不是文件监听线程、也不是调用方线程），
         *          其快照在启动监听前发布（release/acquire 配对），因此回调内可安全做耗时处理。
         *          已在监听时重复调用不会重起监视器，但这一轮的回调与防抖间隔照样生效。
         * @param callback 热加载完成后的回调函数；传 nullptr 即不回调（重载照跑，本轮失败改由日志报出）
         * @param debounceMilliseconds 防抖间隔（毫秒），默认 500ms
         * @return bool 成功返回 true；重复调用返回 true；未加载目录或平台不支持返回 false。
         *               返回 false 的每一条路（没有锚点、创建监视器失败、挂目录失败、监视线程起不来、
         *               装配抛异常）都另报一条诊断说明原因——只回 false 让运维无从分辨。
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
         * @note 返回值是配置子树的一份**深拷贝**，拷贝要分配内存，因此本方法不是 noexcept：
         *       写成 noexcept 只会在内存耗尽时把一次可捕获的失败升级成 std::terminate
         */
        [[nodiscard]] std::optional<ConfigValue> getOptional(std::string_view key) const;

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
            throw ConfigValidationException(std::string(key), std::string("配置值取用类型不匹配：期望 ") + configTypeNameOf<ValueType>() + "，实际 " + typeName(value.type()));
        }

        /**
         * @brief 获取指定类型的配置值
         * @param key 配置键
         * @param defaultValue 默认值（键不存在或类型不匹配时返回）
         * @return 配置值或默认值
         * @note 取值遵循严格口径（见 configValueAs）：不做取整、回绕与跨类型转换，
         *       类型对不上按「未配置」处理而不抛出。
         * @note 同样不是 noexcept：命中的那个值要拷出来交给调用方（字符串与容器都是堆对象）。
         *       类型判定本身不拷树——走快照按引用查，判定失败时不必为这次失败深拷整棵子树
         */
        template<typename T>
        T get(const std::string_view key, T &&defaultValue) const
        {
            // 快照在本函数内持强引用，因此下面那份引用只在本次调用期间有效：取的是表里的原对象
            // 而不是 getOptional 的副本（那条通道判定类型之前先把整棵子树拷进 optional，
            // 用一个 getInt 去碰一个 200 成员的表，光这次「类型不符」就要拷两千多个节点）。
            // getString/getText 已按同一口径改成直查
            const auto currentData = m_data.load(std::memory_order_acquire);
            const auto iterator    = currentData->values.find(key);
            if (iterator == currentData->values.end())
            {
                return std::forward<T>(defaultValue);
            }

            if (auto converted = configValueAs<std::decay_t<T>>(iterator->second))
            {
                return std::move(*converted);
            }
            return std::forward<T>(defaultValue);
        }

        /**
         * @brief 读取布尔配置值，缺失或类型不匹配时返回默认值。
         * @param key 配置键。
         * @param defaultValue 默认值。
         * @return bool 配置值或默认值。
         */
        bool getBool(std::string_view key, bool defaultValue = false) const;

        /**
         * @brief 读取整型配置值，缺失或类型不匹配时返回默认值。
         * @param key 配置键。
         * @param defaultValue 默认值。
         * @return int64_t 配置值或默认值。
         */
        int64_t getInt(std::string_view key, int64_t defaultValue = 0) const;

        /**
         * @brief 读取浮点配置值，缺失或类型不匹配时返回默认值。
         * @param key 配置键。
         * @param defaultValue 默认值。
         * @return double 配置值或默认值。
         */
        double getDouble(std::string_view key, double defaultValue = 0.0) const;

        /**
         * @brief 读取字符串配置值，缺失或类型不匹配时返回默认值。
         * @param key 配置键。
         * @param defaultValue 默认值。
         * @return std::string 配置值或默认值。
         */
        std::string getString(std::string_view key, const std::string &defaultValue = "") const;

        /**
         * @brief 宽松字符串读取：字符串原样返回；数字/布尔自动转为文本。
         * @details 用于内容像数字但语义为字符串的键（如纯数字密码），免疫配置类型推断差异。
         * @param key 配置键（点号路径）。
         * @param defaultValue 键缺失时返回的默认值。
         * @return std::string 文本化取值或默认值。
         */
        std::string getText(std::string_view key, const std::string &defaultValue = "") const;

        /**
         * @brief 设置配置值并立即生效（原子替换内存快照）
         * @details 只改内存，**不写回任何文件**：写配置是应用层的事（它知道该写哪个文件、
         *          要不要问过运维），因此本方法的效果不跨进程，重启后回到文件里的取值。
         * @param key 配置键（点号路径，如 server.port）。
         * @param value 配置值。
         * @return bool 成功返回 true；键为空返回 false。
         * @note 该键在已注册 schema 里有约束时，违规按与文件加载同一口径记进日志；
         *       schema 是建议性约束，这里只报告不阻断，写入照常生效
         * @note 写入同样按「后写的说话」定形：把一段表改写成单个值时，段里那些旧键一并让位（反向亦然），
         *       每次取代报一条日志
         * @note 一次写入要复制整份快照，代价随键数线性（容器 Release 实测 256 键约 10 µs，其中定形
         *       扫描不到 1%）：它按启动期/偶发覆盖定价，不要放进逐请求路径
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
         * @details 把 sectionPrefix 下所有键的剩余路径重新聚成嵌套对象，供只认文档结构的消费方
         *          直接使用；返回值不含段名这一层（键是 "port" 而非 "server.port"），段落无键时返回空对象。
         *          段名自己那个值不并进来：两条写入通道都按「后写的说话」定形，一段有键时段名就不会是键。
         * @param sectionPrefix 段名，如 "server"
         * @return ConfigValue 该段自身的对象副本，热重载不会改写已取走的这一份
         * @throws ConfigValidationException 段内同一个名字既配成标量、又是更长键的第一段
         *         （如 server.port 与 server.port.forwarded 并存）：段落结构无法同时表达两者，
         *         宁可报错也不静默丢掉其中一条。定形之后这条属于不变式自检
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

            std::vector<std::string> loadedFiles;                     ///< 成功加载的配置文件路径列表
            std::filesystem::path    configDirectory;                 ///< 配置目录的路径
            bool                     configDirectoryRecursive = true; ///< 该目录当初是按递归加载的；reload() 与热重载按同一口径重扫，否则键集会在无人改文件时变化
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

        std::atomic<std::shared_ptr<ConfigData>> m_data{std::make_shared<ConfigData>()}; ///< 当前有效的配置数据原子指针，支持无锁热替换

        mutable std::shared_mutex m_reloadMutex; ///< 用于配置数据构建过程的读写锁，仅在修改时加写锁

        std::mutex         m_writeMutex;  ///< 串行化 setValue 的「复制—修改—发布」事务，避免并发写者互相覆盖（读者不受影响）
        mutable std::mutex m_schemaMutex; ///< 保护 m_schema 的互斥锁（const 校验方法也需加锁）
        ConfigSchema       m_schema;      ///< 全局 schema（setSchema 注册，提交快照时自动校验）

        // 热加载相关
        /// 启停热加载的控制面锁：m_fileWatcher 是普通 unique_ptr，只能由持锁的写者改。
        /// 它总是最外层的一把（其内才取 m_reloadTasksMutex），因此与监听线程回调之间不构成环
        std::mutex                                            m_hotReloadControlMutex;
        std::unique_ptr<Platform::FileWatcher>                m_fileWatcher;                ///< 文件监控器（用于热加载）
        std::atomic<std::shared_ptr<const HotReloadCallback>> m_hotReloadCallback{nullptr}; ///< 热加载回调快照（enableHotReload 写、重载线程读）
        std::atomic<bool>                                     m_hotReloadEnabled{false};    ///< 热加载功能是否启用（true 启用，false 关闭）
        /// 重载轮次的节流闸门：一轮在跑时来的变更不丢，由世代号差别保证之后必有一轮重读到它。
        /// 不用「pending + dirty 两面旗」——并发收尾时停在中途的那轮会把别人的欠账吃掉，
        /// 配置就永久停在旧值直到用户下一次改动（详见 ReloadRoundGate 的 @details）
        Detail::ReloadRoundGate                  m_reloadGate;
        std::mutex                               m_reloadTasksMutex; ///< 保护 m_reloadTasks 的互斥锁（仅登记/摘取句柄，join 不在锁内做）
        std::vector<std::unique_ptr<ReloadTask>> m_reloadTasks;      ///< 活跃的重载任务（用于析构前 join）

        /**
         * @brief 取出并回收已结束的重载任务
         * @details 锁内只做「摘取」，锁外析构 unique_ptr（其析构会 join 线程）。
         *          这样一次长 reload 阻塞 join 时不会连带挡住其它热加载检查。
         */
        void collectFinishedReloadTasks();

        /**
         * @brief 登记并启动一轮重载任务（调用方需先占到执行权）
         * @details 任务的收尾（交还执行权、按世代差别决定要不要再来一轮）在任务体内。线程起不来时不抛：
         *          调用它的是文件监视线程与重载线程，异常逃出线程函数即 std::terminate；
         *          改由本函数交还执行权，本轮让给下一次事件。
         * @note 只负责登记与启动
         */
        void startReloadTask();

        /**
         * @brief 安排一轮重载：已有任务在跑就只记下变更，由世代号差别保证之后必有一轮覆盖它
         * @details 单个文件变更与「事件溢出、需要重扫整份目录」两条路都汇到这里——
         *          重载本来就是读完整份目录，两者不需要区分动作。
         */
        void scheduleReload();

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
         * @details 只向下展开对象节点；点号前缀用同一个缓冲「追加—递归—回溯」复用，
         *          叶子值直接从文档中移出，省掉一次深拷贝。
         * @param node 当前文档值节点（必须是对象），其叶子值会被移出，故必须是可改写的临时对象
         * @param prefixBuffer 复用的点号前缀缓冲，进入时表示当前层前缀
         * @param values 扁平化结果容器
         * @throws std::runtime_error 某个键名含有分隔符 '.'
         */
        static void flattenValue(ConfigValue &node, std::string &prefixBuffer, ConfigKeyValueMap &values);

        /**
         * @brief 处理文件监听回调并触发后台重载。
         * @param filePath 发生变化的文件路径；changeType 为 NeedsRescan 时是被监视的目录路径，
         *        此时不按扩展名过滤（那条信号要的就是「重读整份目录」）。
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
         * @details 中途出错（子目录不可读等）时交出的是**错误而不是那份扫到一半的清单**：
         *          半份清单被当成全量提交，等于让后面那些文件的键静默从配置里消失。
         * @param directory 待扫描目录。
         * @param recursive 是否递归。
         * @return std::expected<std::vector<std::filesystem::path>, std::string> 成功时是清单
         *         （空清单表示目录里确实没有配置文件）；失败时是中文原因，调用方据此保留旧快照
         */
        [[nodiscard]] static std::expected<std::vector<std::filesystem::path>, std::string> scanConfigFiles(const std::filesystem::path &directory, bool recursive);

        /**
         * @brief 原子提交新的配置快照。
         * @param values 扁平化配置字典。
         * @param loadedFiles 成功加载的文件列表。
         * @param configDirectory 配置目录（每个调用点都给出真实目录，不存在「留空表示不改」这条）。
         * @param configDirectoryRecursive 该目录此后重扫时要不要递归，与本次加载的口径一致。
         */
        void commitConfigData(ConfigKeyValueMap values, const std::vector<std::string> &loadedFiles, const std::filesystem::path &configDirectory, bool configDirectoryRecursive);

        /**
         * @brief 对指定配置字典执行已注册 schema 的校验并记录错误日志。
         * @details 未注册 schema 或没有命中任何约束条目时什么都不做，也不取日志的代价。
         * @param values 扁平化配置字典。
         * @param onlyKey 只判这一个键的约束条目；留空表示判全部（整份快照提交时用）
         */
        void validateRegisteredSchema(const ConfigKeyValueMap &values, std::string_view onlyKey = {}) const;
    };
} // namespace AsynGyanis::Base
