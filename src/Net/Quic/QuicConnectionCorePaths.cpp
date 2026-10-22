/**
 * @file QuicConnectionCorePaths.cpp
 * @brief QUIC 连接的对端路径跟随、挑战记账与 PATH_RESPONSE 处理
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 连接 ID 跟随、路径状态表、挑战的布置/过期/确认与 PATH_RESPONSE 校验都在这里；
 *          出包、包号空间推进与握手确认留在 QuicConnectionCore.cpp。两侧只通过公开成员与
 *          QuicPathKey/PendingPathChallenge 交互。
 */

#include "Net/Quic/QuicSupport.h" // 必须在任何头文件之前定义 NOMINMAX，与 QuicConnectionCore.cpp 同一约束

#include "Net/Quic/QuicConnectionCore.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using QuicDetail::randomBytes;
        using QuicDetail::saturatingSubtract;

        /// 地址验证之前服务端最多可以回给对端多少倍已收字节（RFC 9000 §8.1）
        constexpr std::size_t kQuicAmplificationFactor = 3;

        /// 「这条路径不受反放大上限约束」的返回值：比任何合法的剩余额度都大，参与取小不影响结论
        constexpr std::size_t kUnlimitedAmplificationByteCount = std::numeric_limits<std::size_t>::max();
    } // namespace

    void QuicConnectionCore::followPeerConnectionId(const QuicPacketHeader &header)
    {
        if (isLocalServer() || m_hasAdoptedPeerConnectionId || !header.isLongHeader || header.sourceConnectionId.empty())
        {
            return;
        }
        // §7.2：客户端只允许把长头报文里的源标识拿来当后续发包的目的标识（短头没有那个字段）。
        // 只跟第一次：服务端此后按 §5.1 可以换标识，跟错一次会把整条连接打成「对端认不出」
        m_hasAdoptedPeerConnectionId = true;
        m_configuration.peerConnectionId.assign(header.sourceConnectionId.begin(), header.sourceConnectionId.end());
        // 本端此前那条「目的标识」是自己挑的初始目的标识，不是对端签发的，因此不进集合；
        // 服务端自报的这一条才是它的序列号 0（§5.1.1）
        m_peerConnectionIds.clear();
        m_peerConnectionIds.push_back(PeerConnectionId{.id = m_configuration.peerConnectionId, .sequence = 0});
    }

    const QuicConnectionCore::PeerPathState *QuicConnectionCore::findPathState(const QuicPathKey &pathKey) const noexcept
    {
        const auto found = std::ranges::find(m_peerPaths, pathKey, &PeerPathState::key);
        return found == m_peerPaths.end() ? nullptr : &*found;
    }

    QuicConnectionCore::PeerPathState *QuicConnectionCore::pathStateFor(const QuicPathKey &pathKey) noexcept
    {
        const auto found = std::ranges::find(m_peerPaths, pathKey, &PeerPathState::key);
        if (found != m_peerPaths.end())
        {
            return &*found;
        }
        if (m_peerPaths.size() >= kMaximumTrackedPeerPaths)
        {
            // 表满：淘汰最老的一条「没在用」的——已验证的、当前发送路径、以及压着候选挑战的验证候选
            // 都还在用（验证期间发包目标留在旧路，候选路径因此不再等于当前发送路径，要单独保护）。
            // 全是在用的就返回空，调用方跳过这次记账：宁可少记一条账，也不能把在用的那条挤掉
            const auto isInUse = [this](const PeerPathState &state)
            {
                if (state.isValidated || state.key == m_sendPathKey)
                {
                    return true;
                }
                return std::ranges::any_of(m_pendingPathChallenges, [&](const PendingPathChallenge &challenge) { return challenge.isCandidate && challenge.key == state.key; });
            };
            const auto evictable = std::ranges::find_if(m_peerPaths, [&](const PeerPathState &state) { return !isInUse(state); });
            if (evictable == m_peerPaths.end())
            {
                return nullptr;
            }
            *evictable = PeerPathState{.key = pathKey};
            return &*evictable;
        }
        return &m_peerPaths.emplace_back(PeerPathState{.key = pathKey});
    }

    void QuicConnectionCore::followPeerPath(const QuicPathKey &pathKey, const PacketNumberSpace space, const std::uint64_t packetNumber, const bool carriesNonProbingFrame,
                                            const Timestamp now)
    {
        if (!m_hasAdoptedPeerPath)
        {
            // 第一条认证过的报文定下这条连接的路径：此前到达的字节可能来路不明，不能拿它们作数。
            // 全零是合法的路径键（单路径用法留空），所以「定没定过」要单独记
            m_hasAdoptedPeerPath = true;
            m_sendPathKey        = pathKey;
            return;
        }
        if (!carriesNonProbingFrame)
        {
            return; // §9.1/§9.3：只有非探测帧才算「对端搬了家」，光打挑战探测不算
        }
        std::optional<std::uint64_t> &largestNonProbing = m_largestNonProbingPacketNumbers[spaceIndex(space)];
        if (largestNonProbing.has_value() && packetNumber <= *largestNonProbing)
        {
            return; // §9.3：发包目标只跟编号最高的那一条非探测包，乱序的旧包不能把它拽回去
        }
        largestNonProbing = packetNumber;
        if (pathKey == m_sendPathKey || !isLocalServer())
        {
            return; // 还在原路上；或本端是客户端——§9 的丢弃门在 handlePacket 里已经拦了陌生地址
        }

        const QuicPathKey previousPathKey = m_sendPathKey;
        if (const PeerPathState *const newPathState = findPathState(pathKey); newPathState != nullptr && newPathState->isValidated)
        {
            // 这条地址以前验过，§9.3 的 MAY 允许跳过重验：立刻切过去，候选验证都用不上了
            m_sendPathKey      = pathKey;
            m_validatedPathKey = pathKey;
            static_cast<void>(std::erase_if(m_pendingPathChallenges, [](const PendingPathChallenge &challenge) { return challenge.isCandidate; }));
        } else if (std::ranges::none_of(m_pendingPathChallenges, [&](const PendingPathChallenge &challenge) { return challenge.key == pathKey && challenge.isCandidate; }))
        {
            // §9.3 的 MUST：验证完成之前不许向新地址发非探测帧。发包目标因此留在旧路，ACK、DATA 这些
            // 都还走旧路；同一条路径已有候选挑战时不再重武装，否则每来一个包就重置一次挑战数据与计时
            armPathChallenge(pathKey, true, now);
        }
        // §9.3.3：疑似迁移时必须在原来那条活动路径上也验一次——那条挑战会把对端的真实流量引回来，
        // 离路劫持的包走得比真包快也照样会被拽回去
        if (const PeerPathState *const previousState = findPathState(previousPathKey); previousState != nullptr && previousState->isValidated)
        {
            armPathChallenge(previousPathKey, false, now);
        }
    }

    void QuicConnectionCore::armPathChallenge(const QuicPathKey &pathKey, const bool isCandidate, const Timestamp now)
    {
        // §8.2.1 的 MUST：每次挑战都用不可预测数据——可预测的话，谁抢答都算验证通过，等于没验
        const std::vector<std::uint8_t> random = randomBytes(kQuicPathValidationDataByteLength);
        if (random.size() != kQuicPathValidationDataByteLength)
        {
            return; // 随机数不可用：不发、不验证，绝不用可预测数据顶上
        }
        PendingPathChallenge challenge;
        challenge.key         = pathKey;
        challenge.isCandidate = isCandidate;
        std::copy(random.begin(), random.end(), challenge.data.begin());
        // §8.2.4 的推荐值：3×max(当前 PTO, 按 kInitialRtt 算的 PTO)。留得宽是有意的——挑战或回应
        // 丢一两条都不该把验证判死，这段时间里换一份新数据重发（§9.4 末段）
        challenge.abandonDeadline    = now + std::max(probeTimeoutPeriod(), m_recovery.initialProbeTimeout()) * 3;
        challenge.retransmitDeadline = now + probeTimeoutPeriod();
        // 同一条路径上只留最新的一条；候选至多一条（能走到这里就说明旧的候选已经让了位）
        static_cast<void>(
                std::erase_if(m_pendingPathChallenges, [&](const PendingPathChallenge &existing) { return existing.key == pathKey || (isCandidate && existing.isCandidate); }));
        if (m_pendingPathChallenges.size() >= kMaximumPendingPathChallenges)
        {
            // 表满先丢非候选的（复验那种丢了也无害）；还满就这条不发
            static_cast<void>(std::erase_if(m_pendingPathChallenges, [](const PendingPathChallenge &existing) { return !existing.isCandidate; }));
            if (m_pendingPathChallenges.size() >= kMaximumPendingPathChallenges)
            {
                return;
            }
        }
        m_pendingPathChallenges.push_back(challenge);
    }

    void QuicConnectionCore::abandonPathValidation(const QuicPathKey &pathKey)
    {
        if (m_configuration.onPeerPathAbandoned)
        {
            m_configuration.onPeerPathAbandoned(pathKey);
        }
        if (!m_hasValidatedPeerPath)
        {
            // §9.3.2 末段：一个已验证地址都没有，就没有可退的地方——静默收口，连 CLOSE 都不发
            m_phase               = QuicConnectionPhase::Closing;
            m_localCloseErrorCode = std::nullopt;
            m_localCloseReasonPhrase.clear();
            m_outboundDatagrams.clear();
            return;
        }
        // §9.3.2：退回上一个已验证地址。验证期间发包目标一直留在旧路，这一步此刻是幂等的；保留它
        // 作为放弃路径的统一出口。对端若真搬了家，它会从新地址再说话、再触发一轮跟随与验证
        m_sendPathKey = m_validatedPathKey;
    }

    void QuicConnectionCore::expirePathChallenges(const Timestamp now)
    {
        if (m_pendingPathChallenges.empty())
        {
            return;
        }
        std::vector<PendingPathChallenge> survivors;
        survivors.reserve(m_pendingPathChallenges.size());
        for (PendingPathChallenge challenge: m_pendingPathChallenges)
        {
            if (now >= challenge.abandonDeadline)
            {
                if (challenge.isCandidate)
                {
                    abandonPathValidation(challenge.key); // §8.2.4 到期放弃 → §9.3.2 退回
                }
                continue; // 复验那条（旧路 / MTU）到期只是销账，不影响连接
            }
            if (challenge.isCandidate && now >= challenge.retransmitDeadline)
            {
                if (!challenge.isSendPending)
                {
                    // §13.3 对 PATH_CHALLENGE 的要求是「换一份新数据重发」：同一份重发没有意义（对端可能
                    // 已经回过、也可能永远回不来）；§9.4 末段给的动作也正是「发一条新挑战并把计时拉长」
                    if (const std::vector<std::uint8_t> fresh = randomBytes(kQuicPathValidationDataByteLength); fresh.size() == kQuicPathValidationDataByteLength)
                    {
                        std::copy(fresh.begin(), fresh.end(), challenge.data.begin());
                        challenge.isSendPending = true;
                    }
                }
                // 计时无论如何都要往后推：还压着没发出去（窗口或额度不够）时也一样，否则这个时刻
                // 永远留在过去，外壳的闹钟会一拍接一拍地空转
                challenge.retransmitDeadline = now + probeTimeoutPeriod();
            }
            survivors.push_back(challenge);
        }
        m_pendingPathChallenges = std::move(survivors);
    }

    void QuicConnectionCore::confirmPeerPathValidated(const QuicPathKey &pathKey, const bool wasExpanded, const Timestamp now)
    {
        if (PeerPathState *const state = pathStateFor(pathKey); state != nullptr)
        {
            state->isValidated = true;
        }
        m_hasValidatedPeerPath = true;
        m_validatedPathKey     = pathKey;
        // §9.4 的 MUST：确认对端对新地址的所有权之后，立刻把拥塞控制器与 RTT 估算重置回初值。
        // 「只换了端口」那一档规范允许保留旧状态（NAT 重绑最常见的正是这种），但那要拿两个地址
        // 的 IP 比对——核心刻意不认识地址，这半条 MAY 就不做：一律重置仍是规范下侧合规的写法
        m_congestion->resetToInitialState();
        m_recovery.resetForPathChange();
        if (m_configuration.onPeerPathValidated)
        {
            m_configuration.onPeerPathValidated(pathKey);
        }
        // §8.2.3 末段：第一轮挑战没能扩到 1200（额度不够）的话，路径通了但 MTU 还没验过，
        // 必须再验一轮扩充的；这一刻路径已验证，反放大上限已经不在
        if (!wasExpanded)
        {
            armPathChallenge(pathKey, false, now);
        }
    }

    void QuicConnectionCore::queuePathResponse(const QuicPathKey &pathKey, const std::array<std::uint8_t, kQuicPathValidationDataByteLength> &data)
    {
        if (m_pendingPathResponses.size() >= kMaximumPendingPathResponses)
        {
            return; // 防滥用：对端在一个包里塞满挑战就等于拿本端当放大器（aioquic 为同一理由设了同量级的上限）
        }
        m_pendingPathResponses.push_back(PendingPathResponse{.key = pathKey, .data = data});
    }

    void QuicConnectionCore::handlePathResponseFrame(const QuicPathResponseFrame &frame, const Timestamp now)
    {
        const auto pending = std::ranges::find_if(m_pendingPathChallenges, [&](const PendingPathChallenge &challenge) { return challenge.data == frame.data; });
        if (pending == m_pendingPathChallenges.end())
        {
            // 对不上任何一条在途挑战：忽略。可能来得太晚，也可能属于别的连接，两种都不构成对端违规
            // （§8.2.3 只写了「对上了」意味着什么）
            return;
        }
        const PendingPathChallenge matched = *pending;
        m_pendingPathChallenges.erase(pending);
        if (matched.isCandidate)
        {
            // 先确认——含 §9.4 的拥塞与 RTT 重置、以及 §8.2.3 末段的补验——再切发包目标：验证完成
            // 之前 ACK、DATA 一直留在旧路，这里是唯一允许开始向新地址发非探测帧的时刻（§9.3 的 MUST）
            confirmPeerPathValidated(matched.key, matched.wasExpanded, now);
            m_sendPathKey = matched.key;
        }
        // 非候选（旧路径复验 / MTU 复验）对上了什么都不做：那条路径本来就是已验证的
    }

    std::size_t QuicConnectionCore::amplificationLimitForPath(const QuicPathKey &pathKey) const noexcept
    {
        std::size_t remaining = kUnlimitedAmplificationByteCount;
        if (isLocalServer() && !m_isAddressValidated)
        {
            // §8.1 的握手期总额度：回出去的字节总数不许超过已收的三倍
            remaining = saturatingSubtract(kQuicAmplificationFactor * m_receivedByteCount, m_sentByteCount);
        }
        if (isLocalServer() && !isUnspecifiedQuicPathKey(pathKey))
        {
            if (const PeerPathState *const state = findPathState(pathKey); state != nullptr && !state->isValidated)
            {
                // §9.3.1 的逐路径额度：只认从这条路径收到的字节
                remaining = std::min(remaining, saturatingSubtract(kQuicAmplificationFactor * state->receivedByteCount, state->sentByteCount));
            }
        }
        return remaining;
    }
} // namespace AsynGyanis::Net
