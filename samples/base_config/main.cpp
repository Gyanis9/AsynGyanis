// Base 配置子系统示例：多文件与目录加载、点分路径取值、schema 校验、类型不符与缺键、热重载
#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigSchema.h"
#include "Base/Exception/ConfigValidationException.h"
#include "common/SampleSupport.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace AsynGyanis;

namespace
{
/**
 * @brief 给一段演示开一个专属子目录
 * @details 每段演示都必须各用各的目录：热重载与 reload() 重扫的是「锚点目录里的全部配置」，
 *          共用一个目录就等于把前面演示留下的坏文件（broken.yaml 一类）塞进后面演示的加载范围，
 *          表现为「示例明明通过了，却是在一次部分失败的加载上通过的」
 */
std::filesystem::path sampleDirectory(const std::filesystem::path &root, const std::string &name)
{
    const std::filesystem::path directory = root / name;
    std::filesystem::create_directories(directory);
    return directory;
}

    /// 往指定路径写一份文本配置（父目录不存在时一并建出来）
    void writeTextFile(const std::filesystem::path &path, const std::string &text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        output << text;
    }

    void demonstrateLoading(const std::filesystem::path &root)
    {
        const std::filesystem::path directory = sampleDirectory(root, "loading");
        writeTextFile(directory / "app.yaml",
                      "server:\n"
                      "  port: 8080\n"
                      "  host: 0.0.0.0\n"
                      "  workers: 4\n"
                      "feature:\n"
                      "  enable_tls: true\n"
                      "  sample_rate: 0.25\n");
        // 后加载的文件覆盖先加载的：这是「同一份键在两个环境里不同值」的常用形态
        writeTextFile(directory / "override.yaml", "server:\n  port: 9090\n");
        writeTextFile(directory / "nested/deep.json", R"({"logging": {"level": "DEBUG"}})");

        auto &manager = Base::ConfigManager::instance();
        manager.clear();
        const Base::ConfigLoadResult loaded = manager.loadFiles({directory / "app.yaml", directory / "override.yaml"});
        Samples::checklist().check(loaded.success && loaded.loadedFiles.size() == 2, "loadFiles 一次读两份配置并报告读到的文件");
        Samples::checklist().check(manager.getInt("server.port", 0) == 9090, "后加载的文件覆盖同名键");
        Samples::checklist().check(manager.getString("server.host") == "0.0.0.0", "点分路径能取到字符串");
        Samples::checklist().check(manager.getBool("feature.enable_tls", false) &&
                                           manager.getDouble("feature.sample_rate", 0.0) == 0.25,
                                   "getBool 与 getDouble 按类型取值");

        // 目录式加载：递归子目录，把目录里所有受支持的文件一次读进来
        const Base::ConfigLoadResult fromDirectory = manager.loadFromDirectory(directory, true);
        Samples::checklist().check(fromDirectory.success && fromDirectory.loadedFiles.size() >= 3,
                                   "loadFromDirectory 递归读到了子目录里的配置");
        Samples::checklist().check(manager.getString("logging.level") == "DEBUG", "子目录里的 JSON 配置也生效了");

        // reload 只报「成功」不等于真重放了：先把磁盘上的值改掉，再看新值有没有进来。
        // workers 只在基础文件里有值（不被 override 覆盖），改它才看得出「按锚点目录 + 同一递归口径
        // 重扫」；文件条数一起断言，挡掉「重放成别的目录」或「锚点丢了」这类看似成功的空转
        writeTextFile(directory / "app.yaml",
                      "server:\n"
                      "  port: 8080\n"
                      "  host: 0.0.0.0\n"
                      "  workers: 8\n"
                      "feature:\n"
                      "  enable_tls: true\n"
                      "  sample_rate: 0.25\n");
        const Base::ConfigLoadResult reloaded = manager.reload();
        Samples::checklist().check(reloaded.success && reloaded.loadedFiles.size() == 3 &&
                                       manager.getInt("server.workers", 0) == 8,
                                   "reload 按同一批路径重扫，并把磁盘上的新值装了进来");

        // 后一份文件把整段表改写成单个值时，先前摊开的叶子键必须一起让位：否则快照里 server 既是值
        // 又是表，getSection 读不回来，而加载报的是成功。这里不 flush、不重新加载，直接看键表形态
        writeTextFile(directory / "collapse.yaml", "server: 9090\n");
        const Base::ConfigLoadResult collapsed = manager.loadFiles({directory / "app.yaml", directory / "collapse.yaml"});
        Samples::checklist().check(collapsed.success && manager.getInt("server", 0) == 9090 &&
                                       !manager.has("server.port") && !manager.has("server.host") &&
                                       manager.has("feature.enable_tls"),
                                   "后写的文件把一段表改成单个值时，旧的叶子键随之让位（不相干的键留着）");
    }

    void demonstrateValueAccess(const std::filesystem::path &root)
    {
        const std::filesystem::path directory = sampleDirectory(root, "typed");
        writeTextFile(directory / "typed.yaml",
                      "limits:\n"
                      "  maximum_body: 1048576\n"
                      "  ratio: 1.5\n"
                      "  label: heavy\n");
        auto &manager = Base::ConfigManager::instance();
        manager.clear();
        static_cast<void>(manager.loadFiles({directory / "typed.yaml"}));

        Samples::checklist().check(manager.get<std::int64_t>("limits.maximum_body") == 1048576, "模板版 get 按声明类型取值");
        Samples::checklist().check(!manager.getOptional("limits.absent").has_value(), "缺键时 getOptional 给空而不是抛异常");
        Samples::checklist().check(manager.get("limits.absent", std::string{"fallback"}) == "fallback",
                                   "带默认值的 get 在缺键时回落默认值");

        bool isThrownForWrongType = false;
        try
        {
            // 类型不符要当场抛：静默回落等于配置错了却看不出来
            static_cast<void>(manager.get<bool>("limits.label"));
        } catch (const Base::ConfigValidationException &validationException)
        {
            isThrownForWrongType = true;
            LOG_INFO_FMT("按预期抛出的类型不匹配说明：{}", validationException.what());
        }
        Samples::checklist().check(isThrownForWrongType, "取用类型不符时抛 ConfigValidationException");

        // 「全部」与顺序都要断言：非空在任何一次成功加载后都成立，什么都证不出来
        const std::vector<std::string> loadedKeys = manager.keys();
        Samples::checklist().check(loadedKeys == std::vector<std::string>{"limits.label", "limits.maximum_body", "limits.ratio"},
                                   "keys() 列出已加载的全部键，且按字典序");
        const Base::ConfigValue section = manager.getSection("limits");
        Samples::checklist().check(section.is_object() && section.size() == 3, "getSection 把扁平键还原成嵌套对象");

        const std::vector<std::string> missing = manager.validateRequired({"limits.label", "limits.nope"});
        Samples::checklist().check(missing.size() == 1 && missing.front() == "limits.nope",
                                   "validateRequired 只列出缺失的那几个键");
    }

    void demonstrateSchemaValidation(const std::filesystem::path &root)
    {
        const std::filesystem::path directory = sampleDirectory(root, "schema");
        auto &manager = Base::ConfigManager::instance();

        const Base::ConfigSchema schema = {
                {"server.port", Base::ConfigValueType::number_integer, true, 1.0, 65535.0},
                {"server.host", Base::ConfigValueType::string, true, {}, {}},
                {"server.workers", Base::ConfigValueType::number_integer, false, 1.0, 128.0},
        };
        // 校验只对着「当前已加载的配置」判：先装一份合规的，再看破坏版
        writeTextFile(directory / "schema-good.yaml",
                      "server:\n  port: 8080\n  host: 0.0.0.0\n  workers: 4\n");
        manager.clear();
        static_cast<void>(manager.loadFiles({directory / "schema-good.yaml"}));
        const Base::ConfigValidationResult passed = manager.validateSchema(schema);
        for (const auto &error: passed.errors)
        {
            LOG_INFO_FMT("本该通过的校验却报了：{}", error);
        }
        Samples::checklist().check(passed.valid && passed.errors.empty(), "满足 schema 时校验通过");

        // 三处同时破坏：缺必需键（host）、类型不符（workers）、数值越界（port）
        writeTextFile(directory / "schema-bad.yaml", "server:\n  port: 70000\n  workers: not-a-number\n");
        manager.clear();
        static_cast<void>(manager.loadFiles({directory / "schema-bad.yaml"}));
        const Base::ConfigValidationResult failed = manager.validateSchema(schema);
        Samples::checklist().check(!failed.valid && failed.errors.size() >= 3,
                                   "缺键、类型不符、越界三类问题都被 schema 校验挑出来");
        for (const auto &error: failed.errors)
        {
            LOG_INFO_FMT("schema 报告：{}", error);
        }
    }

    void demonstrateFailurePaths(const std::filesystem::path &root)
    {
        // 这一段刻意留下语法坏掉的文件，就更不能与别的演示共用目录
        const std::filesystem::path directory = sampleDirectory(root, "failures");
        auto &manager = Base::ConfigManager::instance();

        // 文件不存在：加载整体失败并给出原因，而不是留下一份「看起来加载过」的空配置
        manager.clear();
        const Base::ConfigLoadResult missingFile = manager.loadFiles({directory / "absent.yaml"});
        Samples::checklist().check(!missingFile.success && !missingFile.failedFiles.empty() && !missingFile.errors.empty(),
                                   "加载不存在的文件时如实报告失败与原因");

        writeTextFile(directory / "broken.yaml", "server:\n  port: 1\n   bad-indent: [unclosed\n");
        manager.clear();
        const Base::ConfigLoadResult broken = manager.loadFiles({directory / "broken.yaml"});
        // 说「不会装进半份配置」就得看配置本体：只看 success 时，「解析了一半就报错但把半份装进去」
        // 照样过。上一步刚 clear 过，所以键表为空才是这条说法的证据
        Samples::checklist().check(!broken.success && manager.keys().empty(),
                                   "语法坏掉的 YAML 被拒，不会装进半份配置");
    }

    void demonstrateHotReload(const std::filesystem::path &root)
    {
        const std::filesystem::path directory = sampleDirectory(root, "hot");
        const auto path = directory / "hot.yaml";
        writeTextFile(path, "runtime:\n  flag: off\n");

        auto &manager = Base::ConfigManager::instance();
        manager.clear();
        static_cast<void>(manager.loadFiles({path}));

        std::atomic<bool> isCallbackFired{false};
        std::atomic<bool> isReloadCompletelySuccessful{false};
        // 回调里只置标记：热重载回调在监视线程上跑，重活都不该在这里做
        if (!manager.enableHotReload(
                    [&](const Base::ConfigLoadResult &result)
                    {
                        // success 为假意味着这一轮有文件没读进来——值可能已经换上去了，
                        // 但配置只是半份。只断言「响过」会把这种部分失败当成热重载正常工作
                        isReloadCompletelySuccessful.store(result.success, std::memory_order_release);
                        isCallbackFired.store(true, std::memory_order_release);
                    }))
        {
            Samples::checklist().check(false, "enableHotReload 应当能装上文件监视");
            return;
        }
        Samples::checklist().check(manager.getString("runtime.flag") == "off", "热重载前读到的是初始配置");

        writeTextFile(path, "runtime:\n  flag: on\n");
        // 两条判据一起等，一条都不许单等：那一轮是先提交快照、后置回调标记，只盯「值变了」就可能
        // 抢在回调落地前读到 false；只盯「回调过了」又可能被一轮早于本次写入的收尾满足。
        // 先后与轮询窗口都是用例造不出来的条件，按规范不能赌调度
        const bool isReloaded = Samples::waitUntil(
                [&manager, &isCallbackFired]
                {
                    return isCallbackFired.load(std::memory_order_acquire) && manager.getString("runtime.flag") == "on";
                },
                std::chrono::seconds{8});
        Samples::checklist().check(isReloaded, "改动文件后热重载把新值装了进来");
        Samples::checklist().check(isCallbackFired.load(std::memory_order_acquire), "热重载回调被调用过");
        Samples::checklist().check(isReloadCompletelySuccessful.load(std::memory_order_acquire),
                                   "热重载那一轮把目录里的每个配置文件都读进来了（没有部分失败）");
        manager.disableHotReload();
    }
}

int main()
{
    Samples::setupConsoleLogging();

    const auto directory = std::filesystem::temp_directory_path() /
                           ("asyn-sample-base-config-" + std::to_string(Platform::ProcessInfo::currentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    LOG_INFO("=== Base 配置子系统示例开始 ===");
    demonstrateLoading(directory);
    demonstrateValueAccess(directory);
    demonstrateSchemaValidation(directory);
    demonstrateFailurePaths(directory);
    demonstrateHotReload(directory);

    Base::ConfigManager::instance().disableHotReload();
    Base::ConfigManager::instance().clear();
    std::filesystem::remove_all(directory);

    return Samples::finishSample("base_config");
}
