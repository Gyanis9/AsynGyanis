/**
 * @file CommonTestSupport.h
 * @brief 跨模块共用的测试夹具：临时目录、轮询等待、标准流改挂、栈帧符号判定与本地时刻折算
 * @author Gyanis
 * @date 2026-09-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本头不依赖任何模块的生产代码，供 Base / Core / Platform 等模块的测试目标包含。
 *          各模块的 TestSupport 命名空间以 using 转发暴露这些名字，既有调用点无需改动；
 *          此前 Base 与 Platform 各存一份近乎相同的实现，现只有这一处定义。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::TestSupport
{
    /// 等待类断言的统一上限：正常耗时都在毫秒级，给足余量但不许无界等待
    inline constexpr std::chrono::milliseconds kWaitTimeout{5000};

    /**
     * @brief 把某个标准流临时改挂到给定缓冲，析构时还原原缓冲
     * @details 生产代码里有一条不成文的口径：日志子系统自己的故障只能写 std::cerr（拿根日志器报
     *          自己的故障会递归）。要断言这类诊断就只能把标准流接进内存缓冲。
     */
    class ScopedStreamRedirect
    {
    public:
        /**
         * @brief 改挂流缓冲
         * @param stream 被改挂的标准流（std::cerr 等）
         * @param buffer 临时缓冲，须活过本对象
         */
        ScopedStreamRedirect(std::ostream &stream, std::streambuf *buffer) : m_stream(stream), m_original(stream.rdbuf(buffer))
        {
        }

        /**
         * @brief 析构时还原原缓冲，避免影响其它用例
         */
        ~ScopedStreamRedirect()
        {
            m_stream.rdbuf(m_original);
        }

        ScopedStreamRedirect(const ScopedStreamRedirect &) = delete;

        ScopedStreamRedirect &operator=(const ScopedStreamRedirect &) = delete;

    private:
        std::ostream   &m_stream;   ///< 被改挂的标准流
        std::streambuf *m_original; ///< 原缓冲，析构时还原
    };

    /**
     * @brief 临时目录夹具
     *
     * @details 在系统临时目录下创建唯一命名的目录，析构时递归删除。
     *          文件 Sink、配置目录扫描与文件监视等用例都依赖一个可写的空目录。
     */
    class TemporaryDirectory
    {
    public:
        /**
         * @brief 创建临时目录
         * @param namePrefix 便于定位问题的用途前缀，如 "FileSink"
         */
        explicit TemporaryDirectory(const std::string &namePrefix)
        {
            static std::atomic<unsigned int> sequenceCounter{0};

            // 目录名必须**跨进程**唯一：ctest 会把每个用例作为独立进程并行拉起，而 steady_clock 的读数
            // 是全系统共享的、序号计数器又是每个进程各自从 0 开始，两者相加仍可能撞名（撞名时两个进程
            // 会共用同一个目录，先结束的那个 remove_all 会把另一个的用例文件删掉）。因此以
            // create_directories 是否**真的新建了目录**为准，撞了就换个盐重试——不依赖时钟精度
            std::error_code error;
            while (true)
            {
                const std::string salt = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(sequenceCounter.fetch_add(1));
                m_path                 = std::filesystem::temp_directory_path() / ("AsynGyanis_Test_" + namePrefix + "_" + salt);
                if (std::filesystem::create_directories(m_path, error))
                {
                    break;
                }
                if (error)
                {
                    // 真出错（权限等）不再重试：保持原语义，让用例自己去失败并暴露环境问题
                    break;
                }
            }
        }

        /**
         * @brief 析构并递归删除临时目录；删除失败时向 stderr 报告原因
         */
        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
            // 删除失败必须说出来：静默失败会让「句柄没释放」这类问题以临时目录在 %TEMP% 里
            // 悄悄堆积的形式潜伏（曾有 2000+ 个目录累积数天无人察觉）
            if (error)
            {
                std::fprintf(stderr, "[测试支持] 临时目录删除失败（可能仍被打开的文件占用）：%s — %s\n", m_path.string().c_str(), error.message().c_str());
            }
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;

        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        /**
         * @brief 获取临时目录路径
         * @return const std::filesystem::path& 目录绝对路径
         */
        [[nodiscard]] const std::filesystem::path &path() const noexcept
        {
            return m_path;
        }

        /**
         * @brief 在临时目录内写入文本文件
         * @param fileName 相对文件名
         * @param content 文件内容
         * @return true 写入成功
         */
        bool writeFile(const std::string &fileName, const std::string &content) const
        {
            std::ofstream file(m_path / fileName, std::ios::out | std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }
            file << content;
            return file.good();
        }

        /**
         * @brief 在临时目录内写入**二进制**文件，字节按原样落盘
         * @param fileName 相对文件名
         * @param content 要写的字节（可含 0x00 与 0x0A）
         * @return true 写入成功
         * @details 与 writeFile() 分开而不是加个模式参数：文本模式在 Windows 上会把 0x0A 翻成 CRLF，
         *          凡断言按字节数或内容逐字节比对的夹具（密钥、镜像、DER 编码）用了它就会静默改变长度，
         *          而那正是这类用例要钉住的东西
         */
        bool writeBinaryFile(const std::string &fileName, const std::string &content) const
        {
            std::ofstream file(m_path / fileName, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            file.close();
            return !file.fail();
        }

        /**
         * @brief 在临时目录的子路径下写入文本文件
         * @param relativePath 相对于临时目录的路径，缺失的父目录会被创建
         * @param content 文件内容
         * @return true 写入成功
         */
        bool writeNestedFile(const std::string &relativePath, const std::string &content) const
        {
            const std::filesystem::path targetPath = m_path / relativePath;

            std::error_code error;
            std::filesystem::create_directories(targetPath.parent_path(), error);

            std::ofstream file(targetPath, std::ios::out | std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }
            file << content;
            return file.good();
        }

    private:
        std::filesystem::path m_path; ///< 临时目录绝对路径
    };

    /**
     * @brief 造一段内容确定的字节串，供需要「同名不同内容」或「不同名同内容」的夹具使用
     * @param seed 种子：同 seed 得到同一份，异 seed 得到互不相同的
     * @param length 字节数
     * @return std::string 原始字节（按值返回，可含 0x00）
     * @details 不用随机数：用例要可复现——随机内容会让「两份夹具恰好相同」这种极端情形把对照组变成
     *          偶发失败，而失败也无法在同一台机器上重放。写文件请配
     *          TemporaryDirectory::writeBinaryFile()，文本模式会改动 0x0A。
     */
    inline std::string makeBytePattern(const unsigned int seed, const std::size_t length)
    {
        std::string bytes(length, '\0');
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            bytes[index] = static_cast<char>(static_cast<unsigned char>((index * 7U + seed * 31U + 11U) & 0xFFU));
        }
        return bytes;
    }

    /**
     * @brief 在时限内轮询等待条件成立（避免固定 sleep 造成的偶发失败）
     * @tparam Predicate 可调用且返回 bool 的类型
     * @param predicate 待轮询的条件
     * @param timeout 超时上限，默认 kWaitTimeout
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    bool waitForCondition(Predicate predicate, const std::chrono::milliseconds timeout = kWaitTimeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    /**
     * @brief 以毫秒整数给出超时上限的重载
     * @details int 不会隐式转成 std::chrono::milliseconds（duration 的 rep 构造是 explicit 的），
     *          但各模块的既有调用点大量直接传毫秒整数，这里提供同名重载避免调用方被迫包一层。
     * @tparam Predicate 可调用且返回 bool 的类型
     * @param predicate 待轮询的条件
     * @param timeoutMilliseconds 最长等待毫秒数
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    bool waitForCondition(Predicate predicate, const int timeoutMilliseconds)
    {
        return waitForCondition(std::move(predicate), std::chrono::milliseconds(timeoutMilliseconds));
    }

    /**
     * @brief 判断调用栈文本里的帧是否已被符号解析出源文件位置
     * @details 没有调试信息时 MSVC 只给「模块名+0x偏移」、GCC 给「??」，文本照样非空——
     *          因此「文本为空」不能当作「符号不可用」的判据，只能看有没有出现源文件名。
     * @param stackTraceText 栈帧文本（Base::formatStackTrace() 的产出）
     * @return true 至少有一帧带出了源文件名
     */
    [[nodiscard]] inline bool hasResolvedStackTraceFrames(const std::string_view stackTraceText) noexcept
    {
        // 两家工具链的行号写法不同：MSVC 是 file.cpp(12)，GCC 是 file.cpp:12；头文件里的内联帧同理
        for (const std::string_view fileExtension: {".cpp", ".cc", ".hpp", ".h(", ".h:"})
        {
            if (stackTraceText.find(fileExtension) != std::string_view::npos)
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 把本地挂钟字段折成一个时刻
     * @details 断言日志时间戳的用例要拿固定文本比对整行版式，而文本随机器时区而变，因此固定的
     *          是本地字段而非 epoch 偏移：折出的时刻渲染回去就是同一串文本，与时区无关。
     *          时刻类型取 system_clock，与日志事件用的同一个时钟。
     * @param year 公元年
     * @param month 月（1-12）
     * @param dayOfMonth 日（1-31）
     * @param hour 时（0-23），建议取正午前后以避开夏令时的切换窗口
     * @param minute 分（0-59）
     * @param second 秒（0-59）
     * @param millisecond 毫秒（0-999）
     * @return std::chrono::system_clock::time_point 对应的时刻；折算失败时给出明显不匹配的时刻，由断言报出来
     */
    [[nodiscard]] inline std::chrono::system_clock::time_point makeLocalMoment(const int year, const int month, const int dayOfMonth, const int hour, const int minute,
                                                                               const int second, const int millisecond)
    {
        std::tm calendarTime{};
        calendarTime.tm_year = year - 1900;
        calendarTime.tm_mon  = month - 1;
        calendarTime.tm_mday = dayOfMonth;
        calendarTime.tm_hour = hour;
        calendarTime.tm_min  = minute;
        calendarTime.tm_sec  = second;
        // 交还给 libc 判定夏令时：写死 0/1 会在有夏令时的时区折出偏移一小时的另一刻
        calendarTime.tm_isdst = -1;

        // mktime 会顺手 tzset，而 glibc 的时区缓存是「换掉上一份内部缓冲」（多线程并发首入时
        // 表现为 libc 内部一次 free 与写竞争，TSan 报在 libc 帧上）。POSIX 不承诺 tzset 线程安全，
        // 所以这条只能由调用侧回避；生产代码不在这条路径上（PlatformTime 自己折日历并缓存），
        // 故只在测试助手里串起来：一把进程级锁把各用例的折算排开，换来「TSan 零告警」这个判据可用
        static std::mutex                 calendarFoldMutex;
        const std::lock_guard<std::mutex> foldLock(calendarFoldMutex);

        return std::chrono::system_clock::from_time_t(std::mktime(&calendarTime)) + std::chrono::milliseconds(millisecond);
    }

    /**
     * @brief 读一个环境变量，把「没设」与「设成空串」分开报
     * @details MSVC 在 /W4 /WX 下把 std::getenv 判为弃用（C4996 直接升级成错误），因此 Windows 侧走
     *          _dupenv_s；这不是平台差异而是 CRT 差异，故按编译器判定而非按 ASYN_PLATFORM_WIN32。
     *          两种实现都立即拷贝，调用方不保留指向环境块的指针
     * @param variableName 环境变量名
     * @return std::optional<std::string> 设过则返回值（可能为空串），未设返回 nullopt
     */
    [[nodiscard]] inline std::optional<std::string> readEnvironmentVariable(const char *const variableName)
    {
#if defined(_MSC_VER)
        char  *rawValue      = nullptr;
        size_t valueCapacity = 0;
        if (::_dupenv_s(&rawValue, &valueCapacity, variableName) != 0 || rawValue == nullptr)
        {
            return std::nullopt;
        }
        std::string variableValue(rawValue);
        std::free(rawValue);
        return variableValue;
#else
        const char *const rawValue = std::getenv(variableName);
        if (rawValue == nullptr)
        {
            return std::nullopt;
        }
        return std::string(rawValue);
#endif
    }
} // namespace AsynGyanis::TestSupport
