#include "Net/Http3/Http3Session.h"

#include "Base/Log/LogMacros.h"

#include <chrono>
#include <cstring>

#include <nghttp3/nghttp3.h>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 当前单调时钟的纳秒读数（nghttp3 的时间戳口径与 ngtcp2 一致：单调、纳秒）
        nghttp3_tstamp currentTimestamp() noexcept
        {
            return static_cast<nghttp3_tstamp>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        /// 收到请求正文分片：正文语义在下一个切片里接业务时处理
        int receiveDataCallback(nghttp3_conn *, std::int64_t, const std::uint8_t *, std::size_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 收到一个头字段：头字段的收集与映射在下一个切片里接业务时处理
        int receiveHeaderCallback(nghttp3_conn *, std::int64_t, std::int32_t, nghttp3_rcbuf *, nghttp3_rcbuf *, std::uint8_t, void *,
                                  void *) noexcept
        {
            return 0;
        }

        /// 一个头块开始
        int beginHeadersCallback(nghttp3_conn *, std::int64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 一个头块结束
        int endHeadersCallback(nghttp3_conn *, std::int64_t, int, void *, void *) noexcept
        {
            return 0;
        }

        /// 一条流的接收侧关闭：对服务端来说就是「请求收全了」。本切片只到传输绑定，
        /// 业务处理（映射到 Router 并回响应）是下一个切片，因此这里只留一条可追的轨迹
        int endStreamCallback(nghttp3_conn *, const std::int64_t streamId, void *, void *) noexcept
        {
            LOG_DEBUG_FMT("Http3Session: 流 {} 上的请求已收全（HTTP/3 业务映射尚未接入，本次不产生响应）", streamId);
            return 0;
        }

        /// 一条流关闭
        int streamCloseCallback(nghttp3_conn *, std::int64_t, std::uint64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 被流间同步挡住的字节终于被消费了
        int deferredConsumeCallback(nghttp3_conn *, std::int64_t, std::size_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 本端发出去的流数据被对端确认
        int acknowledgedStreamDataCallback(nghttp3_conn *, std::int64_t, std::uint64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 对端要求本端停止发送：本切片还没有待发的响应体，无需动作
        int stopSendingCallback(nghttp3_conn *, std::int64_t, std::uint64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// nghttp3 要求本端重置一条流
        int resetStreamCallback(nghttp3_conn *, std::int64_t, std::uint64_t, void *, void *) noexcept
        {
            return 0;
        }

        /// 对端发起连接级收口
        int shutdownCallback(nghttp3_conn *, std::int64_t, void *) noexcept
        {
            return 0;
        }

        /// 收到对端的 SETTINGS
        int receiveSettingsCallback(nghttp3_conn *, const nghttp3_settings *, void *) noexcept
        {
            return 0;
        }

        /**
         * @brief 填好 nghttp3 的回调表
         * @details HTTP/2 侧的帧状态机是手写的，这里不重复那套：h3 的帧与 QPACK 全交给 nghttp3，
         *          本表只把它的通知接住。当前切片只做传输绑定，因此这些回调先按「收下即消费」返回 0
         *          （返回值非 0 会被 nghttp3 当成致命错误），业务语义在下一个切片里填。
         * @return nghttp3_callbacks 回调表
         */
        nghttp3_callbacks makeCallbacks() noexcept
        {
            nghttp3_callbacks callbacks{};
            callbacks.acked_stream_data = acknowledgedStreamDataCallback;
            callbacks.stream_close      = streamCloseCallback;
            callbacks.recv_data         = receiveDataCallback;
            callbacks.deferred_consume  = deferredConsumeCallback;
            callbacks.begin_headers     = beginHeadersCallback;
            callbacks.recv_header       = receiveHeaderCallback;
            callbacks.end_headers       = endHeadersCallback;
            callbacks.end_stream        = endStreamCallback;
            callbacks.stop_sending      = stopSendingCallback;
            callbacks.reset_stream      = resetStreamCallback;
            callbacks.shutdown          = shutdownCallback;
            callbacks.recv_settings     = receiveSettingsCallback;
            return callbacks;
        }
    } // namespace

    Http3Session::Http3Session(StreamOpener opener, StreamWriter writer) : m_writer(std::move(writer))
    {
        if (!opener || !m_writer)
        {
            LOG_ERROR("Http3Session: 缺少单向流的开流口或流数据出口，HTTP/3 会话不可用");
            return;
        }

        nghttp3_settings settings;
        nghttp3_settings_default(&settings);

        const nghttp3_callbacks callbacks = makeCallbacks();
        if (nghttp3_conn_server_new(&m_connection, &callbacks, &settings, nullptr, this) != 0)
        {
            m_connection = nullptr;
            LOG_ERROR("Http3Session: nghttp3 服务端会话创建失败，HTTP/3 会话不可用");
            return;
        }

        // 三条本端发起的单向流：控制流、QPACK 编码流、QPACK 解码流。流号由传输层开出来
        // （ngtcp2 不认识这三条流的话，往它们上面写数据会被 STREAM_NOT_FOUND 拒掉）
        const std::int64_t controlStreamId      = opener();
        const std::int64_t qpackEncoderStreamId = opener();
        const std::int64_t qpackDecoderStreamId = opener();
        if (controlStreamId < 0 || qpackEncoderStreamId < 0 || qpackDecoderStreamId < 0)
        {
            LOG_WARN("Http3Session: 开本端单向流失败，HTTP/3 会话不可用");
            return;
        }

        // 绑定顺序按 nghttp3 的接口来：控制流单独绑，两条 QPACK 流一次绑（编码流在前）
        if (nghttp3_conn_bind_control_stream(m_connection, controlStreamId) != 0 ||
            nghttp3_conn_bind_qpack_streams(m_connection, qpackEncoderStreamId, qpackDecoderStreamId) != 0)
        {
            LOG_WARN("Http3Session: 控制流或 QPACK 流绑定失败，HTTP/3 会话不可用");
            return;
        }

        m_isUsable = true;
        LOG_DEBUG_FMT("Http3Session: HTTP/3 会话已建立（控制流 {}、QPACK 编码流 {}、解码流 {}）", controlStreamId, qpackEncoderStreamId,
                      qpackDecoderStreamId);
    }

    Http3Session::~Http3Session()
    {
        if (m_connection != nullptr)
        {
            nghttp3_conn_del(m_connection);
            m_connection = nullptr;
        }
    }

    bool Http3Session::isUsable() const noexcept
    {
        return m_isUsable && !m_isBroken;
    }

    bool Http3Session::isBroken() const noexcept
    {
        return m_isBroken;
    }

    void Http3Session::onStreamData(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
    {
        if (m_connection == nullptr || m_isBroken)
        {
            return;
        }

        // 返回的「已消费字节数」才是可以还给 QUIC 的流控额度；DATA 帧里的应用数据不算在内，
        // 那部分由 recv_data 回调给出
        const nghttp3_ssize consumedLength =
                nghttp3_conn_read_stream2(m_connection, streamId, data.data(), data.size(), isEndStream ? 1 : 0, currentTimestamp());
        if (consumedLength < 0)
        {
            markBroken(static_cast<int>(consumedLength), "读入流数据");
            return;
        }

        // 对端的数据可能解锁了本端待发的东西（比如 QPACK 动态表更新后头块才能编码）
        flushPendingStreamData();
    }

    void Http3Session::flushPendingStreamData()
    {
        if (m_connection == nullptr || m_isBroken)
        {
            return;
        }

        for (std::size_t writeIndex = 0; writeIndex < kMaximumWritesPerFlush; ++writeIndex)
        {
            // nghttp3_vec 与 ngtcp2_vec 布局一致（都是 {指针, 长度}），因此这里顺手就能递给传输层
            nghttp3_vec vectors[kMaximumDataVectors]{};
            std::int64_t      streamId    = -1;
            int               isFinal     = 0;
            const nghttp3_ssize vectorCount =
                    nghttp3_conn_writev_stream(m_connection, &streamId, &isFinal, vectors, kMaximumDataVectors);
            if (vectorCount < 0)
            {
                markBroken(static_cast<int>(vectorCount), "取待发流数据");
                return;
            }
            if (vectorCount == 0 && streamId == -1)
            {
                // 没有待发字节，也没有要收尾的流：本轮搬完了
                return;
            }

            // nghttp3 给的是分片数组，传输层的出口一次只收一段连续字节，先拼起来。
            // 拼好的字节在交出时必须是有效的，所以用成员缓冲而不是临时对象
            std::size_t totalLength = 0;
            for (nghttp3_ssize vectorIndex = 0; vectorIndex < vectorCount; ++vectorIndex)
            {
                totalLength += vectors[vectorIndex].len;
            }
            m_pendingBytes.resize(totalLength);
            std::size_t offset = 0;
            for (nghttp3_ssize vectorIndex = 0; vectorIndex < vectorCount; ++vectorIndex)
            {
                if (vectors[vectorIndex].len == 0)
                {
                    continue;
                }
                std::memcpy(m_pendingBytes.data() + offset, vectors[vectorIndex].base, vectors[vectorIndex].len);
                offset += vectors[vectorIndex].len;
            }

            m_writer(streamId, m_pendingBytes, isFinal != 0);
            // 字节已被 QUIC 收下，回告 nghttp3 实际收下的长度（它按这个推进写窗口）。
            // 「只收尾、不带数据」时长度是 0，这一次调用同样不能省
            nghttp3_conn_add_write_offset(m_connection, streamId, totalLength);
        }
    }

    void Http3Session::markBroken(const int errorCode, const char *const what)
    {
        m_isBroken = true;
        LOG_WARN_FMT("Http3Session: {}时 nghttp3 报错（{}），HTTP/3 会话作废", what, nghttp3_strerror(errorCode));
    }
} // namespace AsynGyanis::Net
