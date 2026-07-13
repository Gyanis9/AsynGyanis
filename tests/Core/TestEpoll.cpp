#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "Core/Epoll.h"
#include "Platform/Platform.h"
#include "Platform/SocketCompat.h"
#include <thread>
#include <atomic>

using namespace Core;

// Helper: create a triggerable fd for epoll testing.
// Linux: eventfd (bidirectional, single fd)
// Windows: socket pair (wepoll can monitor sockets)
struct TestEventFd {
    int fd;       // fd to add to epoll (read end on Windows)
    int writeFd;  // fd to write to trigger (same as fd on Linux)

    TestEventFd() {
#ifdef _WIN32
        if (!Platform::createSocketPair(fd, writeFd)) {
            fd = -1;
            writeFd = -1;
            return;
        }
        Platform::setNonBlocking(fd);
#else
        fd = static_cast<int>(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        writeFd = fd;
#endif
    }

    ~TestEventFd() {
        if (fd >= 0) Platform::closeFd(fd);
#ifdef _WIN32
        if (writeFd >= 0 && writeFd != fd) Platform::closeFd(writeFd);
#endif
    }

    TestEventFd(const TestEventFd &)            = delete;
    TestEventFd &operator=(const TestEventFd &) = delete;

    bool trigger() {
        uint64_t val = 1;
#ifdef _WIN32
        return ::send(writeFd, reinterpret_cast<const char *>(&val), sizeof(val), 0) == sizeof(val);
#else
        return ::write(writeFd, &val, sizeof(val)) == sizeof(val);
#endif
    }
};

TEST_CASE("Epoll: construction and basic properties", "[Epoll]") {
    Epoll epoll;
    REQUIRE(epoll.fd() != kInvalidEpollHandle);
}

TEST_CASE("Epoll: move constructor", "[Epoll]") {
    Epoll ep1;
    epoll_handle_t fd1 = ep1.fd();
    Epoll ep2(std::move(ep1));
    REQUIRE(ep2.fd() == fd1);
}

TEST_CASE("Epoll: move assignment", "[Epoll]") {
    Epoll ep1;
    Epoll ep2;
    epoll_handle_t fd1 = ep1.fd();
    ep2 = std::move(ep1);
    REQUIRE(ep2.fd() == fd1);
}

TEST_CASE("Epoll: addFd and wait for event", "[Epoll]") {
    Epoll epoll;
    TestEventFd efd;
    REQUIRE(efd.fd >= 0);

    int sentinel = 0;
    REQUIRE(epoll.addFd(efd.fd, EPOLLIN, &sentinel));

    // Write to trigger readability
    REQUIRE(efd.trigger());

    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    void *ptr = events[0].data.ptr;
    REQUIRE(ptr == &sentinel);
}

TEST_CASE("Epoll: delFd removes fd", "[Epoll]") {
    Epoll epoll;
    TestEventFd efd;
    REQUIRE(efd.fd >= 0);

    REQUIRE(epoll.addFd(efd.fd, EPOLLIN, nullptr));
    REQUIRE(epoll.delFd(efd.fd));

    // After delFd, writing to efd should NOT trigger epoll
    efd.trigger();

    auto events = epoll.wait(10);
    REQUIRE(events.empty());
}

TEST_CASE("Epoll: wait timeout returns empty", "[Epoll]") {
    Epoll epoll;
    auto events = epoll.wait(10);
    REQUIRE(events.empty());
}

TEST_CASE("Epoll: modFd changes event mask", "[Epoll]") {
    Epoll epoll;
    TestEventFd efd;
    REQUIRE(efd.fd >= 0);

    int sentinel = 42;
    REQUIRE(epoll.addFd(efd.fd, EPOLLIN, &sentinel));
    REQUIRE(epoll.modFd(efd.fd, EPOLLOUT, &sentinel));

    // EPOLLOUT should immediately fire (always writable)
    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    REQUIRE(events[0].events & EPOLLOUT);
}

TEST_CASE("Epoll: multiple fds", "[Epoll]") {
    Epoll epoll;
    TestEventFd efd1;
    TestEventFd efd2;
    REQUIRE(efd1.fd >= 0);
    REQUIRE(efd2.fd >= 0);

    int s1 = 1, s2 = 2;
    REQUIRE(epoll.addFd(efd1.fd, EPOLLIN, &s1));
    REQUIRE(epoll.addFd(efd2.fd, EPOLLIN, &s2));

    // Trigger only efd2
    efd2.trigger();

    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    // At least efd2 should be ready
    bool found = false;
    for (auto &ev : events) {
        if (ev.data.ptr == &s2) found = true;
    }
    REQUIRE(found);
}
