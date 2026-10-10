#include "Net/Http/RequestBodySpool.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Net/Http/HttpRequestBody.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/System/ProcessInfo.h"

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 进程内的落盘文件计数器
         * @details 与 AtomicFileWriter 同一套路：进程号在同一台机器上同时存活的进程之间唯一，
         *          再加这一格把同一进程内的多份上传也分开。少了计数器，两份并发上传会算出
         *          同一个临时名并互相夹写——那比失败更难归因
         * @return std::uint64_t 单调递增的序号
         */
        std::uint64_t nextSpoolSequence() noexcept
        {
            static std::atomic<std::uint64_t> counter{0};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief 关掉流并尽力删掉文件，交回「有没有删掉」
         * @param state 待收口的文件状态
         * @return true 文件已不在（删成功，或本来就没建出来）
         */
        bool discardSpoolFile(const std::shared_ptr<Detail::SpoolFileState> &state) noexcept
        {
            if (state == nullptr || state->path.empty())
            {
                return true;
            }
            if (state->stream.is_open())
            {
                state->stream.close();
            }
            std::error_code removalError;
            std::filesystem::remove(state->path, removalError);
            return !removalError;
        }
    } // namespace

    Core::Task<std::expected<RequestBodySpool, std::string>> RequestBodySpool::capture(Core::EventLoop &completionLoop, Core::AsyncExecutor &writerExecutor, HttpRequestBody &body,
                                                                                       RequestBodySpoolOptions options)
    {
        if (options.maximumByteCount == 0)
        {
            // 0 不是「不限」——那两个字留给别的字段说。这里当成用法错误拒掉，
            // 免得一次误读把「任何正文都落不下去」表现成磁盘问题
            co_return std::unexpected("落盘上限不能为 0：那既不是「不限」也没有可落的正文");
        }

        std::error_code       directoryError;
        std::filesystem::path directory = options.directory;
        if (directory.empty())
        {
            // 系统临时目录读不出来是可能的（容器里 TMPDIR 指向不存在的路径），这时不猜一个位置，
            // 让调用方显式给 directory——猜错的落点会把上传写进一个没人回收的目录
            directory = std::filesystem::temp_directory_path(directoryError);
            if (directoryError)
            {
                co_return std::unexpected("定不下落盘目录：系统临时目录读不出来（错误码 " + std::to_string(directoryError.value()) + "），请显式给 directory");
            }
        }

        auto state = std::make_shared<Detail::SpoolFileState>();
        // 名字按「前缀 + 进程号 + 进程内计数器」拼，同目录内两份并发上传不会算出同一个名字；
        // 打开时直接把 path 交给流，不走 path.string() 那次代码页往返（Windows 上非 ASCII 的
        // 临时目录名会在那一步抛或变形，而本层的失败通道是值而不是异常）
        state->path = directory / (options.filePrefix + std::to_string(Platform::ProcessInfo::currentProcessId()) + "-" + std::to_string(nextSpoolSequence()));
        state->stream.open(state->path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!state->stream.is_open())
        {
            co_return std::unexpected("打不开落盘文件：" + Platform::FileSystem::utf8FromPath(state->path));
        }

        while (co_await body.readNext())
        {
            // 先拷一段再提交：chunk() 的视图只有效到下一次 readNext()，而工作线程可能在恢复投递之前还没跑完
            const std::string chunk(body.chunk());
            // 减法在「已写的那一侧」做：writtenByteCount 恒不超过上限，右边不会为负
            if (chunk.size() > options.maximumByteCount - state->writtenByteCount)
            {
                static_cast<void>(discardSpoolFile(state));
                co_return std::unexpected("正文超过落盘上限 " + std::to_string(options.maximumByteCount) + " 字节");
            }

            const std::size_t writtenLength = co_await writerExecutor.submit<std::size_t>(completionLoop,
                                                                                          [state, chunk]
                                                                                          {
                                                                                              state->stream.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                                                                                              state->stream.flush();
                                                                                              return state->stream.good() ? chunk.size() : static_cast<std::size_t>(0);
                                                                                          });
            if (writtenLength != chunk.size())
            {
                // 半截段落不进第二次重试（文件位置已经不确定了），删掉整份重来是唯一干净的选择
                static_cast<void>(discardSpoolFile(state));
                co_return std::unexpected("写盘只落了 " + std::to_string(writtenLength) + "/" + std::to_string(chunk.size()) + " 字节（磁盘满、目录被摘或 IO 故障）");
            }
            state->writtenByteCount += writtenLength;
        }

        if (body.isTruncated())
        {
            // 交出去的每一份都要能被当成完整的那一份用；长度不足的正文按错误处理（RFC 9110 §6.6）
            static_cast<void>(discardSpoolFile(state));
            co_return std::unexpected("正文没收齐就终止了（对端断开、流被重置或中途解析失败），不落半份");
        }

        state->stream.close();
        co_return RequestBodySpool{std::move(state)};
    }

    const std::filesystem::path &RequestBodySpool::path() const noexcept
    {
        return m_state->path;
    }

    std::uintmax_t RequestBodySpool::byteCount() const noexcept
    {
        return m_state->writtenByteCount;
    }

    void RequestBodySpool::release() noexcept
    {
        m_state->isReleased = true;
    }

    RequestBodySpool::~RequestBodySpool()
    {
        if (m_state != nullptr && !m_state->isReleased)
        {
            static_cast<void>(discardSpoolFile(m_state));
        }
    }
} // namespace AsynGyanis::Net
