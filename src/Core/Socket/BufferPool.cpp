#include "Core/Socket/BufferPool.h"

namespace AsynGyanis::Core
{
    BufferPool::BufferPool(const size_t bufferSize, const size_t bufferCount) :
        m_bufferSize(bufferSize), m_bufferCount(bufferCount),
        m_memory(bufferSize * bufferCount), m_freeList(bufferCount), m_isOccupied(bufferCount, 0)
    {
        for (size_t i = 0; i < bufferCount; ++i)
        {
            // 逆序压栈：索引 0 落在栈顶，于是栈式弹出的次序是 0、1、2…，
            // 与内存布局正序一致（若正序压栈，首次分配会从最大索引倒着发）
            m_freeList[i] = static_cast<int>(bufferCount - 1 - i);
        }
        m_freeTop = bufferCount;
    }

    BufferPool::~BufferPool() = default;

    int BufferPool::acquire()
    {
        // 空闲栈为空属于「本次没有空闲缓冲」这一正常否定结果，用 -1 作哨兵；
        // 不能拿 0 当哨兵——0 是合法索引，否则调用方无法区分「拿到 0 号块」与「分配失败」
        if (m_freeTop == 0)
            return -1;

        // 空闲栈后进先出：刚释放的块最先被复用，命中缓存；先弹栈再置占用位，
        // 维持「同一索引要么在栈里、要么被标记占用」这一不变式
        const int index = m_freeList[--m_freeTop];
        m_isOccupied[static_cast<size_t>(index)] = 1;
        return index;
    }

    void BufferPool::release(const int index)
    {
        // 越界索引直接忽略：release 只接受 acquire 交出的索引
        if (index < 0 || static_cast<size_t>(index) >= m_bufferCount)
        {
            return;
        }

        // 未占用（重复释放、或释放从未取出的索引）时不做任何事：若照旧压栈，
        // 同一索引会在空闲栈里出现两次，之后 acquire() 会把同一块缓冲交给两个调用方，
        // 两个使用者互相覆写数据且不报任何错——静默损坏比忽略一次误用危险得多
        std::uint8_t &occupiedFlag = m_isOccupied[static_cast<size_t>(index)];
        if (occupiedFlag == 0)
        {
            return;
        }

        occupiedFlag               = 0;
        m_freeList[m_freeTop++]    = index;
    }

    void *BufferPool::data(const int index)
    {
        // 越界或无效下标返回 nullptr，而不是夹取到 0 号块：调用方常拿着 acquire()
        // 失败留下的 -1 来查地址，静默换成 0 号块就等于让调用方读写别人的缓冲；
        // 负索引也在进入指针运算前拦下，避免无符号回绕后指到池外
        if (index < 0 || static_cast<size_t>(index) >= m_bufferCount)
            return nullptr;
        return m_memory.data() + static_cast<size_t>(index) * m_bufferSize;
    }

    size_t BufferPool::bufferSize() const noexcept
    {
        return m_bufferSize;
    }

    size_t BufferPool::bufferCount() const noexcept
    {
        return m_bufferCount;
    }

}
