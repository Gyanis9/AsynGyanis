#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "Core/Epoll.h"
#include "Platform/Platform.h"
#include "Platform/SocketCompat.h"
#include <thread>
#include <atomic>

using namespace Core;

// 辅助工具：创建一个可触发的文件描述符用于 epoll 测试。
// Linux: eventfd（双向，单个文件描述符）
// Windows: socket pair（wepoll 可以监听 socket）
struct TestEventFd {
    int fileDescriptor;       // 要添加到 epoll 的文件描述符（Windows 上是读端）
    int writeFileDescriptor;  // 要写入以触发的文件描述符（Linux 上与 fileDescriptor 相同）

    TestEventFd() {
#ifdef _WIN32
        if (!Platform::createSocketPair(fileDescriptor, writeFileDescriptor)) {
            fileDescriptor = -1;
            writeFileDescriptor = -1;
            return;
        }
        Platform::setNonBlocking(fileDescriptor);
#else
        fileDescriptor = static_cast<int>(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        writeFileDescriptor = fileDescriptor;
#endif
    }

    ~TestEventFd() {
        if (fileDescriptor >= 0) Platform::closeFileDescriptor(fileDescriptor);
#ifdef _WIN32
        if (writeFileDescriptor >= 0 && writeFileDescriptor != fileDescriptor) Platform::closeFileDescriptor(writeFileDescriptor);
#endif
    }

    TestEventFd(const TestEventFd &)            = delete;
    TestEventFd &operator=(const TestEventFd &) = delete;

    bool trigger() {
        uint64_t value = 1;
#ifdef _WIN32
        return ::send(writeFileDescriptor, reinterpret_cast<const char *>(&value), sizeof(value), 0) == sizeof(value);
#else
        return ::write(writeFileDescriptor, &value, sizeof(value)) == sizeof(value);
#endif
    }
};

TEST_CASE("Epoll: construction and basic properties", "[Epoll]") {
    Epoll epoll;
    REQUIRE(epoll.fileDescriptor() != kInvalidEpollHandle);
}

TEST_CASE("Epoll: move constructor", "[Epoll]") {
    Epoll ep1;
    epoll_handle_t fileDescriptor1 = ep1.fileDescriptor();
    Epoll ep2(std::move(ep1));
    REQUIRE(ep2.fileDescriptor() == fileDescriptor1);
}

TEST_CASE("Epoll: move assignment", "[Epoll]") {
    Epoll ep1;
    Epoll ep2;
    epoll_handle_t fileDescriptor1 = ep1.fileDescriptor();
    ep2 = std::move(ep1);
    REQUIRE(ep2.fileDescriptor() == fileDescriptor1);
}

TEST_CASE("Epoll: addFileDescriptor and wait for event", "[Epoll]") {
    Epoll epoll;
    TestEventFd efd;
    REQUIRE(efd.fileDescriptor >= 0);

    int sentinel = 0;
    REQUIRE(epoll.addFileDescriptor(efd.fileDescriptor, EPOLLIN, &sentinel));

    // 写入数据触发可读事件
    REQUIRE(efd.trigger());

    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    void *ptr = events[0].data.ptr;
    REQUIRE(ptr == &sentinel);
}

TEST_CASE("Epoll: delFileDescriptor removes file descriptor", "[Epoll]") {
    Epoll epoll;
    TestEventFd efd;
    REQUIRE(efd.fileDescriptor >= 0);

    REQUIRE(epoll.addFileDescriptor(efd.fileDescriptor, EPOLLIN, nullptr));
    REQUIRE(epoll.delFileDescriptor(efd.fileDescriptor));

    // 移除文件描述符后，写入不应触发 epoll
    efd.trigger();

    auto events = epoll.wait(10);
    REQUIRE(events.empty());
}

TEST_CASE("Epoll: wait timeout returns empty", "[Epoll]") {
    Epoll epoll;
    auto events = epoll.wait(10);
    REQUIRE(events.empty());
}

TEST_CASE("Epoll: modFileDescriptor changes event mask", "[Epoll]") {
    Epoll epoll;
    TestEventFd efd;
    REQUIRE(efd.fileDescriptor >= 0);

    int sentinel = 42;
    REQUIRE(epoll.addFileDescriptor(efd.fileDescriptor, EPOLLIN, &sentinel));
    REQUIRE(epoll.modFileDescriptor(efd.fileDescriptor, EPOLLOUT, &sentinel));

    // EPOLLOUT 应该立即触发（socket 始终可写）
    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    REQUIRE(events[0].events & EPOLLOUT);
}

TEST_CASE("Epoll: multiple fds", "[Epoll]") {
    Epoll epoll;
    TestEventFd efd1;
    TestEventFd efd2;
    REQUIRE(efd1.fileDescriptor >= 0);
    REQUIRE(efd2.fileDescriptor >= 0);

    int s1 = 1, s2 = 2;
    REQUIRE(epoll.addFileDescriptor(efd1.fileDescriptor, EPOLLIN, &s1));
    REQUIRE(epoll.addFileDescriptor(efd2.fileDescriptor, EPOLLIN, &s2));

    // 仅触发 efd2
    efd2.trigger();

    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    // 至少 efd2 应该就绪
    bool found = false;
    for (auto &ev : events) {
        if (ev.data.ptr == &s2) found = true;
    }
    REQUIRE(found);
}
