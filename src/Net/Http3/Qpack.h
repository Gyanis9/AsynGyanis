/**
 * @file Qpack.h
 * @brief QPACK 头压缩（RFC 9204）：动态表、编码器与解码器（含阻塞流与增量指令解析）
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 与 HPACK 的关键差异是「表更新」与「头块」走两条独立的流，因此对端可能先拿到头块、后拿到
 *          它引用的动态表项：这类流必须能挂起（blocked）并在指令补齐后续解（§2.2.1）。整数与 Huffman
 *          码沿用 RFC 7541 的表示（§4.1.1、§4.1.2 明确复用），故基本表示直接调 Net/Http2/Hpack.h 的助手，
 *          但静态表、索引空间与编号全部按 RFC 9204 重建（见 QpackStaticTable.h）。
 */

#pragma once

#include "Net/Http2/Hpack.h"
#include "Net/Http3/Http3Error.h"
#include "Net/Http3/QpackStaticTable.h"

#include <cstdint>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Net
{
    // ============================================================================
    // QPACK 头块压缩（RFC 9204）
    //
    // 一层只管压缩状态，不碰流：编码器产出「头块字节 + 编码器流指令字节」两段，解码器吃「编码器流指令
    // 字节」并在交付头块后回吐「解码器流指令字节」，四条字节流都由会话层负责写到对应的 QUIC 流上。
    // ============================================================================

    /// 动态表每一项的固定开销（RFC 9204 §3.2.1 的算式：名长 + 值长 + 32 字节）
    inline constexpr std::size_t kQpackEntryOverheadByteCount = 32;

    /// 前缀整数可表示的最大取值（RFC 9204 §4.1.1 要求实现能解到 62 位；再大按 §7.4 判错）
    inline constexpr std::uint64_t kQpackMaximumIntegerValue = (std::uint64_t{1} << 62) - 1;

    /// 单个字符串字面量声明长度的上限（RFC 9204 §7.4 要求实现自设名/值长度上限）：超出即判错，
    /// 避免为一个永远凑不齐的超长字面量无界等待后续字节
    inline constexpr std::size_t kQpackMaximumStringLengthByteCount = 1ull << 20;

    /// 编码器为「留出可淘汰余量」而放弃直接引用的目标空间比例分母（§2.1.1.1 的固定余量启发式）
    inline constexpr std::size_t kQpackDrainingFreeSpaceDivisor = 2;

    /**
     * @brief 动态表里查不到匹配项时的哨兵取值（绝对索引 0 是合法项，不能用 0 表示「没有」）
     */
    inline constexpr std::uint64_t kQpackNoAbsoluteIndex = static_cast<std::uint64_t>(-1);

    /**
     * @brief QPACK 失败的类别，按「该用哪个线上错误码收场」划分
     *
     * @details 头块解不开只作废那一条流（对端还能继续用同一个压缩上下文），而编码器流/解码器流上的
     *          指令非法意味着两端动态表已经不同步，必须作废整条连接：这是三类码分开注册的意义
     *          （RFC 9204 §6）。
     * @note 新增类别一律追加在末尾；映射见 toHttp3ErrorCode()，上层只看 kind 不匹配文案。
     */
    enum class QpackErrorKind
    {
        DecompressionFailed,   ///< 头块本身解不开：索引越界、引用的项已淘汰、Required Insert Count 不合法、Required Insert Count 超过本端阻塞上限（RFC 9204 §2.2.1、§2.2.3、§4.5、§7.4）
        EncoderStreamError,    ///< 编码器流指令非法：未知指令、动态表项大于容量、引用已淘汰项、容量超过对端上限（RFC 9204 §3.2.2、§3.2.3、§4.3）
        DecoderStreamError,    ///< 解码器流指令非法：重复或无据的 Section Ack、Increment 为 0 或超出本端已发计数（RFC 9204 §4.4.1、§4.4.3）
        FieldSectionTooLarge,  ///< 解出的头块超过本端 SETTINGS_MAX_FIELD_SECTION_SIZE：属对端过量负载（RFC 9114 §4.2.2、§10.5.1）
        InvalidLocalState,     ///< 本端用法或状态不自洽，未产生任何线上字节（如要求的容量大于对端上限）
    };

    /**
     * @brief 一次 QPACK 失败的完整说明
     */
    struct QpackError
    {
        QpackErrorKind kind{QpackErrorKind::InvalidLocalState}; ///< 失败类别：上层据此选上线错误码，不去匹配文案
        std::string message;                                    ///< 中文原因，含可定位坐标（哪个索引、哪条指令、第几个字段行）与 RFC 章节号
    };

    /**
     * @brief 把 QPACK 失败类别映射成上线的 HTTP/3 错误码
     * @param errorKind QPACK 失败类别
     * @return Http3ErrorCode DecompressionFailed→DecompressionFailed、EncoderStreamError→EncoderStreamError、
     *         DecoderStreamError→DecoderStreamError、FieldSectionTooLarge→ExcessiveLoad、InvalidLocalState→InternalError
     */
    [[nodiscard]] Http3ErrorCode toHttp3ErrorCode(QpackErrorKind errorKind) noexcept;

    /**
     * @brief 一条字段行（RFC 9204 §1.1 的 field line）
     */
    struct QpackHeaderField
    {
        std::string name;  ///< 字段名，按字节原样存取（大小写、NUL 都不在本层解释）
        std::string value; ///< 字段值，按字节原样存取
    };

    /**
     * @brief 头块解码结果：解出来了，还是因动态表未就绪而挂起
     *
     * @details Blocked 不是错误（§2.2.1 的正常路径）：此时不产出字段行，本端保留原始字节，等
     *          feedEncoderStream() 报告该流解除阻塞后再按 resumeBlockedFieldSection() 续解。
     */
    enum class QpackFieldSectionDecodeStatus
    {
        Decoded, ///< 头块解完，字段行已按序产出
        Blocked, ///< Required Insert Count 大于本端 Insert Count，头块挂起等待编码器流补齐（§2.2.1）
    };

    /**
     * @brief QPACK 动态表（RFC 9204 §3.2）：FIFO 存放字段行，按绝对索引寻址
     *
     * @details 绝对索引一旦分配就在项的整个生命周期内不变（§3.2.4），淘汰不改变它，也不让累计插入计数
     *          回退；因此「表里现在有哪些项」与「已经插入了多少项」是两条独立的线，索引换算全部走
     *          insertCount / droppedCount 两个计数。
     */
    class QpackDynamicTable
    {
    public:
        /**
         * @brief 淘汰许可判据：绝对索引 -> 该项此刻能否被淘汰
         * @details 编码器侧要求「未被未确认头块引用且已被对端确认收到」才可淘汰（§2.1.1、§2.2.2.2）；
         *          解码器侧无条件允许，因为淘汰节奏由对端指令决定（§3.2.2）。
         */
        using EvictionPredicate = std::function<bool(std::uint64_t absoluteIndex)>;

        /**
         * @brief 建表
         * @param capacityByteCount 表容量上限，单位字节；0 是合法状态（禁用动态表，§3.2.2）
         */
        explicit QpackDynamicTable(std::size_t capacityByteCount = 0) noexcept;

        /**
         * @brief 设置容量并按需从表尾淘汰（RFC 9204 §3.2.2）
         * @param capacityByteCount 新容量，单位字节
         * @param evictionPermitted 淘汰许可判据；默认允许淘汰任何项
         * @return true 已把表缩到新容量之内
         * @return false 需要淘汰的项里有不被允许的，容量与表内容保持调用前一致
         */
        [[nodiscard]] bool setCapacityByteCount(std::size_t capacityByteCount, const EvictionPredicate &evictionPermitted = nullptr);

        /**
         * @brief 插入一项，必要时先淘汰表尾（RFC 9204 §3.2.2）
         * @param field 待插入的字段行，按移动收下
         * @param evictionPermitted 淘汰许可判据；默认允许淘汰任何项
         * @return std::optional<std::uint64_t> 新项的绝对索引；未插入时为空，原因有二：单项大于容量
         *         （§3.2.2 判错），或需要淘汰不被允许的项（§2.1.1 禁止）
         */
        [[nodiscard]] std::optional<std::uint64_t> insert(QpackHeaderField field, const EvictionPredicate &evictionPermitted = nullptr);

        /**
         * @brief 按绝对索引取项（RFC 9204 §3.2.4）
         * @param absoluteIndex 绝对索引
         * @param field 输出参数：取到的字段行（按值拷贝），仅在返回 true 时有效
         * @return true 命中
         * @return false 该项已被淘汰或尚未插入
         */
        [[nodiscard]] bool tryGetEntryByAbsoluteIndex(std::uint64_t absoluteIndex, QpackHeaderField &field) const;

        /**
         * @brief 按「相对表首」的相对索引取项（RFC 9204 §3.2.5 中编码器指令的语境）
         * @param relativeIndex 相对索引：0 是最新插入的一项，随插入过程移动
         * @param field 输出参数：取到的字段行，仅在返回 true 时有效
         * @return true 命中
         * @return false 越过表尾（项已淘汰）或表为空
         */
        [[nodiscard]] bool tryGetEntryByRelativeIndexFromInsertionPoint(std::uint64_t relativeIndex, QpackHeaderField &field) const;

        /**
         * @brief 按「名 + 值」找最新匹配项（编码器选表示用）
         * @param name 字段名，按字节比较
         * @param value 字段值，按字节比较
         * @return std::uint64_t 命中项的绝对索引；未命中返回 kQpackNoAbsoluteIndex
         */
        [[nodiscard]] std::uint64_t findMatchingEntry(std::string_view name, std::string_view value) const;

        /**
         * @brief 只按字段名找最新匹配项（编码器取「带索引名的字面量」用）
         * @param name 字段名，按字节比较
         * @return std::uint64_t 命中项的绝对索引；未命中返回 kQpackNoAbsoluteIndex
         */
        [[nodiscard]] std::uint64_t findNameEntry(std::string_view name) const;

        /// 清空表内容；累计插入计数不回退，绝对索引空间与对端保持一致（RFC 9204 §3.2.4）
        void clearEntries() noexcept;

        /**
         * @brief 算一项占的大小（RFC 9204 §3.2.1）
         * @param field 待算的字段行
         * @return std::size_t 名长 + 值长 + 32，单位字节；按未做 Huffman 编码的长度算
         */
        [[nodiscard]] static std::size_t entrySizeByteCountOf(const QpackHeaderField &field) noexcept;

        [[nodiscard]] std::size_t capacityByteCount() const noexcept;    ///< 当前容量上限，单位字节
        [[nodiscard]] std::size_t sizeByteCount() const noexcept;        ///< 当前表大小，单位字节（§3.2.1 算式）
        [[nodiscard]] std::size_t entryCount() const noexcept;           ///< 当前项数
        [[nodiscard]] std::uint64_t insertCount() const noexcept;        ///< Insert Count：累计插入数，含已淘汰项（§1.1）
        [[nodiscard]] std::uint64_t droppedEntryCount() const noexcept;  ///< Dropping Point：最小可用绝对索引（§3.2.5 的 d）

        /**
         * @brief 取 MaxEntries：容量所能容纳的最多项数（RFC 9204 §4.5.1.1）
         * @param maximumTableCapacityByteCount 对端 SETTINGS 公布的容量上限，单位字节
         * @return std::uint64_t floor(容量 / 32)；上限小于 32 时为 0，此时 Required Insert Count 只允许编码 0
         */
        [[nodiscard]] static std::uint64_t maximumEntryCount(std::size_t maximumTableCapacityByteCount) noexcept;

        [[nodiscard]] const std::deque<QpackHeaderField> &entries() const noexcept; ///< 表内容，下标 0 是最新插入的一项

    private:
        std::deque<QpackHeaderField> m_entries;             ///< 表内容，下标 0 是最新插入项，淘汰只从表尾开始
        std::size_t m_capacityByteCount{0};                 ///< 当前容量上限，单位字节
        std::size_t m_sizeByteCount{0};                     ///< 当前表大小，单位字节
        std::uint64_t m_insertCount{0};                     ///< 累计插入数（绝对索引即插入时的该计数值）
    };

    /**
     * @brief 本端编码头块（= 本端写自己的动态表，对端负责解码）
     *
     * @details 按 RFC 9204 附录 C 的单遍算法编码：先取 Base 快照（本段开始时的 Insert Count），逐字段行
     *          在「静态表精确命中 → 动态表精确命中 → 插入并引用 → 带索引名/双字面量」里选表示，
     *          Required Insert Count 取所有被引用绝对索引的最大值 + 1（§2.1.2），最后回填前缀。
     *
     * @note 编码侧不产出 Huffman 字面量、不产出 N 位（never-indexed）表示：两者都是体积/策略优化，
     *       不影响可解性，与本片验收标准「编出的字节能被解回」一致。
     * @warning 动态表状态是本端与对端解码器共享的上下文：同一条连接上不要换用另一个实例，容量变化也只
     *          能通过 setMaximumTableCapacityByteCount() 走，否则对端索引会指向不同项（§3.2）。
     */
    class QpackEncoder
    {
    public:
        /**
         * @brief 构造编码器
         * @param peerMaximumTableCapacityByteCount 对端 SETTINGS_QPACK_MAX_TABLE_CAPACITY：本端能设的容量上限（§3.2.3）
         * @param peerMaximumBlockedStreamCount 对端 SETTINGS_QPACK_BLOCKED_STREAMS：本端可冒险阻塞的流数上限（§2.1.2）
         * @param localTableCapacityByteCount 本端想用的动态表容量，单位字节；不得超过对端上限，超了不静默
         *        收窄，而在任何产出字节的调用里以 InvalidLocalState 失败
         * @note 容量非 0 时，Set Dynamic Table Capacity 指令要等第一次产出字节才写进编码器流（§4.3.1）；
         *       对端上限为 0 时本端不得插入也不得发任何编码器流指令（§3.2.3）
         */
        QpackEncoder(std::size_t peerMaximumTableCapacityByteCount, std::size_t peerMaximumBlockedStreamCount,
                     std::size_t localTableCapacityByteCount);

        /**
         * @brief 析构函数：动态表与未确认头块的记账都是按值容器，无额外资源需要回收
         */
        ~QpackEncoder() = default;

        // 禁拷贝：复制一份会让动态表与「已发出未确认」的记账各自推进，插入索引随即在两边指向不同条目
        QpackEncoder(const QpackEncoder &) = delete;
        QpackEncoder &operator=(const QpackEncoder &) = delete;

        /**
         * @brief 调整动态表容量，并产出 Set Dynamic Table Capacity 指令（RFC 9204 §4.3.1、§3.2.2）
         * @param capacityByteCount 新容量，单位字节；0 表示把表清空到不可插入，之后仍可再调回来
         * @param encoderStreamBytes 输出参数：本次要追加到编码器流的字节，进入调用时先清空
         * @return std::expected<void, QpackError> 成功；失败时未写任何字节——超出对端上限判
         *         InvalidLocalState，会淘汰仍被未确认头块引用的项也判 InvalidLocalState（§4.3.1 禁止）
         */
        [[nodiscard]] std::expected<void, QpackError> setMaximumTableCapacityByteCount(std::size_t capacityByteCount,
                                                                                       std::string &encoderStreamBytes);

        /**
         * @brief 编码一段头块（含 §4.5.1 的两字段前缀）
         * @param streamId 承载该头块的 HTTP 流标识，仅用于阻塞与引用登记的记账（§2.1.2）
         * @param fieldLines 待编码的字段行，按给定顺序输出表示（§2.1 要求保序）
         * @param headerBlock 输出参数：完整编码段（前缀 + 字段行表示），可直接放进 HEADERS/CONTINUATION 净负载；进入调用时先清空
         * @param encoderStreamBytes 输出参数：本次产生的编码器流指令（容量变更、插入、Duplicate），进入调用时先清空
         * @return std::expected<void, QpackError> 成功；失败时两个输出均为空串，不留半截字节
         * @warning 两段输出必须按「编码器流指令在前、头块在后」的顺序写出：头块引用的表项若还没随指令到达
         *          对端，对端就得为该流挂起等待（§2.2.1）；同一条头块的这两段字节应落在同一个 flush 周期内
         *          送出，§2.1.3 还要求整条指令的流控额度已可用才写。
         */
        [[nodiscard]] std::expected<void, QpackError> encodeFieldSection(std::uint64_t streamId,
                                                                        std::span<const QpackHeaderField> fieldLines,
                                                                        std::string &headerBlock,
                                                                        std::string &encoderStreamBytes);

        /**
         * @brief 增量吃掉对端解码器流的字节（Section Ack / Stream Cancellation / Insert Count Increment）
         * @param bytes 新到达的字节，按「指针 + 长度」取，可含任意二进制
         * @return std::expected<std::size_t, QpackError> 本趟消费的字节数；剩余不足一条指令的尾巴留在
         *         内部缓冲里等下趟，故返回值可以小于 bytes.size()。指令非法时返回 DecoderStreamError，
         *         此时本端已放弃该连接上的压缩上下文，上层须按 §6 作废连接
         * @note 每收到一条 Section Ack 即推进「对端已知插入计数」并释放该流对动态表项的引用（§2.1.4、§2.2.2.1）
         */
        [[nodiscard]] std::expected<std::size_t, QpackError> feedDecoderStream(std::span<const std::uint8_t> bytes);

        /**
         * @brief 本端放弃了某条流：释放该流上的引用，并按 §4.4.2 产出 Stream Cancellation
         * @param streamId 被放弃的 HTTP 流标识
         * @param decoderStreamBytes 输出参数：本次要追加到解码器流的字节，进入调用时先清空
         * @note 调用时机：本端在该流上主动 RESET、或对端 RESET 之后本端不再读它的头块。容量为 0 时按
         *       §2.2.2.2 可以省略该指令，本实现仍照发，以便对端尽早释放引用
         */
        void noteStreamAbandoned(std::uint64_t streamId, std::string &decoderStreamBytes);

        /**
         * @brief 是否有已发出、但对端可能还没解开的头块
         * @return true 至少一条流处于「可能阻塞」状态：调度方若已无编码器流带宽，先停发新头块
         */
        [[nodiscard]] bool hasBlockedStreams() const noexcept;

        /**
         * @brief 处于「可能阻塞」状态的流数
         * @return std::size_t 计数上界恒为构造时传入的 peerMaximumBlockedStreamCount（§2.1.2 的 MUST）
         */
        [[nodiscard]] std::size_t blockedStreamCount() const noexcept;

        [[nodiscard]] std::size_t tableCapacityByteCount() const noexcept;   ///< 本端生效的表容量，单位字节
        [[nodiscard]] std::size_t dynamicTableSizeByteCount() const noexcept;///< 当前表大小，单位字节（§3.2.1 算式）
        [[nodiscard]] std::uint64_t insertCount() const noexcept;            ///< 本端累计插入数
        [[nodiscard]] std::uint64_t knownReceivedInsertCount() const noexcept;///< 对端已确认收到的插入数（§2.1.4）
        [[nodiscard]] const std::deque<QpackHeaderField> &dynamicTableEntries() const noexcept; ///< 表内容，下标 0 最新

    private:
        /**
         * @brief 一段未确认头块的记账，Section Ack 按「同流最早一段」弹掉（§2.2.2.1）
         */
        struct PendingFieldSection
        {
            std::uint64_t requiredInsertCount{0};       ///< 该段声明的 Required Insert Count，0 表示不需要 Ack
            std::vector<std::uint64_t> referencedAbsoluteIndices; ///< 该段引用的动态表绝对索引
            bool risksBlocking{false};                  ///< 发出时是否可能让对端阻塞（计入 BLOCKED_STREAMS）
        };

        /**
         * @brief 扣掉一段头块对动态表项的引用（Section Ack 或流取消时调用，§2.2.2.2）
         * @param pendingSection 被确认或被取消的那一段记账
         */
        void releasePendingSectionReferences(const PendingFieldSection &pendingSection) noexcept;

        /**
         * @brief 把某条流计入阻塞上限的段数减一，减到 0 时该流不再占名额
         * @param streamId 流标识
         */
        void subtractBlockingSectionCount(std::uint64_t streamId) noexcept;

        /**
         * @brief 放弃一条流：释放它全部未确认头块的引用并撤掉其阻塞名额（§4.4.2）
         * @param streamId 流标识
         */
        void cancelStreamReferences(std::uint64_t streamId) noexcept;

        /**
         * @brief 已知接收计数前进后，重算哪些段不再可能让对端阻塞（§2.1.2 的名额归还）
         */
        void refreshBlockingState() noexcept;

        /**
         * @brief 找到某条流上最早一段待确认的头块（§2.2.2.1 的「最早未确认」）
         * @param streamId 收到 Section Ack 的流标识
         * @return std::map<std::uint64_t, std::deque<PendingFieldSection>>::iterator 命中则该队列非空；
         *         该流没有待确认的段时返回末尾迭代器
         */
        [[nodiscard]] std::map<std::uint64_t, std::deque<PendingFieldSection>>::iterator findEarliestAwaitingAcknowledgement(
            std::uint64_t streamId) noexcept;

        /**
         * @brief 判据：绝对索引为 absoluteIndex 的表项此刻能否被淘汰（§2.1.1、§2.2.2.2）
         * @param absoluteIndex 待判定的绝对索引
         * @return true 已被对端确认收到且无未确认引用
         */
        [[nodiscard]] bool isEntryEvictable(std::uint64_t absoluteIndex) const;

        /**
         * @brief 按 §2.1.1.1 的固定余量启发式推进 draining 索引，使较旧的项不再被直接引用
         */
        void refreshDrainingAbsoluteIndex() noexcept;

        std::size_t m_peerMaximumTableCapacityByteCount{0};   ///< 对端 SETTINGS_QPACK_MAX_TABLE_CAPACITY（也用于 §4.5.1.1 的取模）
        std::size_t m_peerMaximumBlockedStreamCount{0};       ///< 对端 SETTINGS_QPACK_BLOCKED_STREAMS
        std::size_t m_tableCapacityByteCount{0};              ///< 本端要求的表容量，单位字节
        bool m_hasPendingCapacityInstruction{false};          ///< 是否需要把当前容量作为第一条指令写到编码器流
        QpackDynamicTable m_dynamicTable;                     ///< 本端动态表，与对端解码器同步演进

        std::unordered_map<std::uint64_t, std::size_t> m_entryReferenceCount;   ///< 绝对索引 -> 未确认头块对它的引用数
        std::map<std::uint64_t, std::deque<PendingFieldSection>> m_pendingSectionsByStreamId; ///< 每条流未确认的头块，队首最早
        std::map<std::uint64_t, std::size_t> m_blockingSectionCountByStreamId;  ///< 每条流仍可能阻塞的头块数
        std::uint64_t m_knownReceivedInsertCount{0};                            ///< 对端已确认的插入数（§2.1.4）
        std::uint64_t m_drainingAbsoluteIndex{0};                                ///< 本端不再直接引用的最小绝对索引（§2.1.1.1）

        std::string m_decoderStreamBuffer;                     ///< 解码器流上未凑齐一条指令的残留字节
    };

    /**
     * @brief 本端解码器的自我约束，取本端在 SETTINGS 里公布的三个值
     *
     * @note maximumFieldSectionSizeByteCount 为 0 表示不限（RFC 9114 §7.2.4.1 的默认取值），与
     *       HpackDecoderLimits 里「0 表示什么都不放过」的约定相反；另两项为 0 是合法状态（禁用动态表）。
     */
    struct QpackDecoderSettings
    {
        std::size_t maximumTableCapacityByteCount{0};   ///< 本端 SETTINGS_QPACK_MAX_TABLE_CAPACITY：对端可设的容量上限（§3.2.3）
        std::size_t maximumBlockedStreamCount{0};       ///< 本端 SETTINGS_QPACK_BLOCKED_STREAMS：本端承诺支持的阻塞流数（§2.1.2）
        std::size_t maximumFieldSectionSizeByteCount{0};///< 本端 SETTINGS_MAX_FIELD_SECTION_SIZE，单位字节；0 为不限
    };

    /**
     * @brief 本端解码头块（= 本端按对端要求维护解码侧动态表状态，并回吐解码器流指令）
     *
     * @details 头块与表更新来自两条独立流，因此解码顺序不保证与编码顺序一致：本类用
     *          「Required Insert Count 与本端 Insert Count 比较」判定能否立刻解（§2.2.1），解不开的按流
     *          挂起原始字节，等编码器流补齐后续解，并且只在整段头块交付上层之后才发 Section Ack（§4.4.1）。
     *
     * @warning 入参是一个**完整**的编码段：HEADERS 与 CONTINUATION 的片段由会话层按序拼好后一次喂入。
     * @warning 失败即状态不再可信：DynamicTable 与对端不一致后必须按 §6 作废连接（头块类失败可只重置该
     *          流），本类不提供 reset()，实例应随连接一起销毁。
     */
    class QpackDecoder
    {
    public:
        /**
         * @brief 构造解码器
         * @param settings 本端公布的自我约束；动态表容量初始为 0，等对端发 Set Dynamic Table Capacity（§3.2.2）
         */
        explicit QpackDecoder(QpackDecoderSettings settings);

        /**
         * @brief 析构函数：解码侧动态表与挂起头块的记录都是按值容器，无额外资源需要回收
         */
        ~QpackDecoder() = default;

        // 禁拷贝：解码器持有对端编码器流的解析进度与解码侧动态表，复制一份会让两边进度分叉，
        // 该发的 Section Ack 与插入数告知也会各回吐一遍
        QpackDecoder(const QpackDecoder &) = delete;
        QpackDecoder &operator=(const QpackDecoder &) = delete;

        /**
         * @brief 增量吃掉对端编码器流的字节（容量变更 / 两种插入 / Duplicate）
         * @param bytes 新到达的字节，按「指针 + 长度」取
         * @param unblockedStreamIds 输出参数：本次因表补齐而可以续解的流标识，进入调用时先清空；调用方须对
         *        每个标识调一次 resumeBlockedFieldSection()（§2.2.1）
         * @param decoderStreamBytes 输出参数：本次建议追加到解码器流的字节（当前实现只可能是 Insert Count
         *        Increment），进入调用时先清空
         * @return std::expected<std::size_t, QpackError> 本趟消费的字节数；剩余部分留在内部缓冲里等下趟，
         *         故可以小于 bytes.size()。指令非法时返回 EncoderStreamError（§6：连接作废）
         */
        [[nodiscard]] std::expected<std::size_t, QpackError> feedEncoderStream(std::span<const std::uint8_t> bytes,
                                                                               std::vector<std::uint64_t> &unblockedStreamIds,
                                                                               std::string &decoderStreamBytes);

        /**
         * @brief 解一段头块：前缀（Required Insert Count + Delta Base）加字段行表示
         * @param streamId 承载该头块的 HTTP 流标识，用于挂起与 Section Ack 的按流记账
         * @param encodedFieldSection 完整编码段字节
         * @param fields 输出参数：解出的字段行按序存放，进入调用时先清空；Blocked 与失败时为空
         * @param decoderStreamBytes 输出参数：本次可追加到解码器流的字节；**Section Ack 不在这里发**，
         *        要等整段头块交付上层后由 noteFieldSectionDelivered() 产出（§4.4.1）
         * @return std::expected<QpackFieldSectionDecodeStatus, QpackError> Decoded 表示 fields 可用；
         *         Blocked 表示已挂起（超过本端承诺的阻塞流数则按 §2.1.2 判 DecompressionFailed）；
         *         失败时错误类别区分头块类（DecompressionFailed）与本端策略（FieldSectionTooLarge）
         */
        [[nodiscard]] std::expected<QpackFieldSectionDecodeStatus, QpackError>
        decodeFieldSection(std::uint64_t streamId, std::span<const std::uint8_t> encodedFieldSection,
                           std::vector<QpackHeaderField> &fields, std::string &decoderStreamBytes);

        /**
         * @brief 续解一条已挂起的流上最早的那段头块
         * @param streamId feedEncoderStream() 报出来的已解除阻塞的流标识
         * @param fields 输出参数：解出的字段行按序存放，进入调用时先清空
         * @param decoderStreamBytes 输出参数：本次可追加到解码器流的字节，同 decodeFieldSection()
         * @return std::expected<QpackFieldSectionDecodeStatus, QpackError> Decoded 表示 fields 可用且挂起记录
         *         已消除；Blocked 表示仍不够解（挂起记录原样留着，等下一次表补齐）；引用的项在此期间被
         *         淘汰或前缀本身非法时返回 DecompressionFailed（§2.2.3）
         */
        [[nodiscard]] std::expected<QpackFieldSectionDecodeStatus, QpackError>
        resumeBlockedFieldSection(std::uint64_t streamId, std::vector<QpackHeaderField> &fields,
                                  std::string &decoderStreamBytes);

        /**
         * @brief 告知本层「整段头块已交给上层处理」，据此产出 Section Ack（RFC 9204 §4.4.1）
         * @param streamId 该头块所在流；按 §2.2.2.1 只确认该流上最早一段 Required Insert Count 非 0 的头块
         * @param decoderStreamBytes 输出参数：追加的字节（Section Ack，并视需要补一条 Insert Count Increment），
         *        进入调用时先清空
         * @return std::expected<void, QpackError> 无可确认的头块时返回 DecoderStreamError（说明上层与本层
         *         的记账不同步，线上会表现为对端收到无据的 Ack）
         * @warning 解码成功不等于可以 Ack：本方法必须在字段行交给上层之后调用，提前调用会让对端把尚未被
         *          本端消费的表项判为可淘汰（§4.4.1 的措辞是「After processing」）
         */
        [[nodiscard]] std::expected<void, QpackError> noteFieldSectionDelivered(std::uint64_t streamId,
                                                                               std::string &decoderStreamBytes);

        /**
         * @brief 把「本端已收到但还没告诉对端」的插入数作为 Insert Count Increment 写出（RFC 9204 §4.4.3）
         * @param decoderStreamBytes 输出参数：追加的字节，进入调用时不清空（可与别的指令拼成一趟）
         * @return std::size_t 追加的字节数；没有待告知的增量时为 0 且不写任何字节
         * @note 同一计数不会被重复告知：Increment 恒等于「已处理插入数 − 已知接收插入计数」，发出后两者即相等
         */
        [[nodiscard]] std::size_t emitInsertCountIncrement(std::string &decoderStreamBytes);

        /**
         * @brief 对端在该流上 RESET，或本端放弃继续读它的头块
         * @param streamId 被放弃的 HTTP 流标识
         * @param decoderStreamBytes 输出参数：追加的 Stream Cancellation 字节（§4.4.2），进入调用时先清空
         * @note 挂起中的头块被丢弃但不算解码失败：该流上 Required Insert Count 里那段引用不再有人等，
         *       已解出但尚未交付的 Section Ack 待办也一并作废。对没有挂起记录的流照发该指令：§4.4.2 未
         *       禁止，且能让对端尽早释放引用
         */
        void noteStreamAbandoned(std::uint64_t streamId, std::string &decoderStreamBytes);

        [[nodiscard]] std::size_t blockedStreamCount() const noexcept;             ///< 当前挂起的流数，上界为本端公布的阻塞流数
        [[nodiscard]] bool hasBlockedStreams() const noexcept;                     ///< 是否有流在等编码器流补齐
        [[nodiscard]] std::size_t tableCapacityByteCount() const noexcept;         ///< 对端设定的当前容量，单位字节
        [[nodiscard]] std::size_t dynamicTableSizeByteCount() const noexcept;      ///< 当前表大小，单位字节
        [[nodiscard]] std::uint64_t insertCount() const noexcept;                  ///< 本端已处理的插入数（§2.2.1 的比较基准）
        [[nodiscard]] std::uint64_t knownReceivedInsertCount() const noexcept;     ///< 已告诉对端的插入数（§2.1.4）
        [[nodiscard]] const std::deque<QpackHeaderField> &dynamicTableEntries() const noexcept; ///< 表内容，下标 0 最新

    private:
        /**
         * @brief 一条挂起的头块
         */
        struct BlockedFieldSection
        {
            std::uint64_t requiredInsertCount{0}; ///< 该段声明的 Required Insert Count，用于判定解除阻塞
            std::string encodedFieldSection;      ///< 原始编码段字节，解除阻塞后据此续解
        };

        /**
         * @brief 解一个字段行表示并把它收下
         * @details 写进本解码器的字段行落点（`beginDecodedFieldLine()`），槽位与其中的串跨段复用。
         * @param streamId 所在流，仅用于报错定位
         * @param section 整段编码段
         * @param cursor [in,out] 字节游标，成功时推进到该表示之后
         * @param baseValue 该段的 Base（§3.2.5/§3.2.6 的换算基准）
         * @param requiredInsertCount 该段的 Required Insert Count，用于 §2.2.3 的越界判定
         * @param maximumReferencedAbsoluteIndex [in,out] 本段引用到的最大绝对索引，用于核对 RIC 取值
         * @return std::expected<void, QpackError> 成功；字节不够与表示非法都按 DecompressionFailed 返回
         */
        [[nodiscard]] std::expected<void, QpackError>
        decodeFieldLineRepresentation(std::uint64_t streamId, std::span<const std::uint8_t> section, std::size_t &cursor,
                                      std::uint64_t baseValue, std::uint64_t requiredInsertCount,
                                      std::uint64_t &maximumReferencedAbsoluteIndex);

        /**
         * @brief 解一段头块到本解码器的字段行落点里，不动调用方交出的缓冲
         * @details 拆出来是为了让「交付」只有一处：解成功才把落点逐条改写进调用方的 vector，
         *          失败与被挂起都让它保持空，调用方看到的形状与逐条 append 的写法完全一致。
         * @param streamId 承载该头块的流标识，仅用于报错定位与挂起登记
         * @param encodedFieldSection 编码段字节
         * @param decoderStreamBytes 输出参数：本次可追加到解码器流的字节，进入调用时先清空
         * @return std::expected<QpackFieldSectionDecodeStatus, QpackError> 与公开入口同一口径
         */
        [[nodiscard]] std::expected<QpackFieldSectionDecodeStatus, QpackError>
        decodeFieldSectionIntoScratch(std::uint64_t streamId, std::span<const std::uint8_t> encodedFieldSection,
                                      std::string &decoderStreamBytes);

        /**
         * @brief 开始解一段头块
         * @details 字段行游标归零，已建好的槽与其中的串一概原地留着。
         */
        void restartFieldLineScratch() noexcept;

        /**
         * @brief 取本段下一条字段行的写入槽
         * @details 名与值先清空，各自已要到的容量留着复用；落点不够用时才新建一槽。
         * @return QpackHeaderField& 指向本段该条字段行的落点
         */
        [[nodiscard]] QpackHeaderField &beginDecodedFieldLine();

        /**
         * @brief 把本段解出的字段行逐条改写进调用方的缓冲
         * @details 两侧串都按赋值改写：交换或移动会把调用方缓冲的容量带走，下一段又从头长。
         * @param fields 输出参数：交付后的字段行，条数与本段解出的一致
         */
        void deliverDecodedFieldLines(std::vector<QpackHeaderField> &fields) const;

        /**
         * @brief 把动态表绝对索引换算成该索引对应的项
         * @param absoluteIndex 待取项的绝对索引
         * @param requiredInsertCount 该段的 Required Insert Count（§2.2.3 要求引用严格小于它）
         * @param field 输出参数：取到的字段行
         * @return std::expected<void, QpackError> 成功；越界或已淘汰按 DecompressionFailed 返回
         */
        [[nodiscard]] std::expected<void, QpackError> resolveDynamicReference(std::uint64_t absoluteIndex,
                                                                             std::uint64_t requiredInsertCount,
                                                                             QpackHeaderField &field) const;

        /**
         * @brief 挂起一条流并在超过本端承诺的阻塞流数时判错（§2.1.2 的 MUST）
         * @param streamId 流标识
         * @param section 原始编码段字节
         * @param requiredInsertCount 该段的 Required Insert Count
         * @return std::expected<void, QpackError> 已挂起；超限返回 DecompressionFailed
         */
        [[nodiscard]] std::expected<void, QpackError> blockStream(std::uint64_t streamId, std::span<const std::uint8_t> section,
                                                                  std::uint64_t requiredInsertCount);

        /**
         * @brief 按 §4.5.1.1 的算法把前缀里的 Encoded Insert Count 还原成 Required Insert Count
         * @param encodedInsertCount 前缀里 8 位前缀整数解出的值
         * @return std::expected<std::uint64_t, QpackError> 还原值；不可能由合规编码器产生时按 DecompressionFailed 返回
         */
        [[nodiscard]] std::expected<std::uint64_t, QpackError> decodeRequiredInsertCount(std::uint64_t encodedInsertCount) const;

        /**
         * @brief 有「本端已处理但还没告诉对端」的插入时，追加一条 Insert Count Increment（§4.4.3）
         * @param decoderStreamBytes 输出参数：按序追加的字节，不清空
         * @note 每次追加都带上「已处理插入数 − 已知接收插入计数」这一正向增量，并立即把两者拉平，
         *       因此同一个计数不会被重复告知对端，也不会出现增量为 0 的指令（§4.4.3 判错的那种）
         */
        void appendInsertCountIncrementIfPending(std::string &decoderStreamBytes);

        /**
         * @brief 抹掉一条流的全部挂起记录（续解成功或流被放弃时调用）
         * @param streamId 流标识
         */
        void eraseBlockedSection(std::uint64_t streamId) noexcept;

        QpackDecoderSettings m_settings{};            ///< 构造时按值落定的本端约束，没有中途更换的入口
        QpackDynamicTable m_dynamicTable;             ///< 解码侧动态表，随对端编码器流指令演进（§3.2）
        std::uint64_t m_knownReceivedInsertCount{0};  ///< 已经告诉对端的插入数（§2.1.4）
        std::map<std::uint64_t, std::deque<BlockedFieldSection>> m_blockedSectionsByStreamId; ///< 按流挂起的头块，队首最早
        std::map<std::uint64_t, std::deque<std::uint64_t>> m_unacknowledgedRequiredInsertCountsByStreamId; ///< 已解出、待 Ack 的 RIC

        std::string m_encoderStreamBuffer;                    ///< 编码器流上未凑齐一条指令的残留字节
        std::vector<QpackHeaderField> m_fieldLineScratch{};   ///< 字段行的复用落点，高水位常驻：槽与其中的串跨段留着
        std::size_t m_fieldLineCount{0};                      ///< 本段已写入落点的条数，超出部分是上一段的残留
    };
} // namespace AsynGyanis::Net
