#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "Core/Epoll.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <thread>
#include <atomic>

using namespace Core;

TEST_CASE("Epoll: construction and basic properties", "[Epoll]") {
    Epoll epoll;
    REQUIRE(epoll.fd() >= 0);
}

TEST_CASE("Epoll: move constructor", "[Epoll]") {
    Epoll ep1;
    int fd1 = ep1.fd();
    Epoll ep2(std::move(ep1));
    REQUIRE(ep2.fd() == fd1);
}

TEST_CASE("Epoll: move assignment", "[Epoll]") {
    Epoll ep1;
    Epoll ep2;
    int fd1 = ep1.fd();
    ep2 = std::move(ep1);
    REQUIRE(ep2.fd() == fd1);
}

TEST_CASE("Epoll: addFd and wait for event", "[Epoll]") {
    Epoll epoll;
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd >= 0);

    int sentinel = 0;
    REQUIRE(epoll.addFd(efd, EPOLLIN, &sentinel));

    // Write to eventfd to trigger readability
    uint64_t val = 1;
    REQUIRE(write(efd, &val, sizeof(val)) == sizeof(val));

    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    void *ptr = events[0].data.ptr;
    REQUIRE(ptr == &sentinel);

    close(efd);
}

TEST_CASE("Epoll: delFd removes fd", "[Epoll]") {
    Epoll epoll;
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd >= 0);

    REQUIRE(epoll.addFd(efd, EPOLLIN, nullptr));
    REQUIRE(epoll.delFd(efd));

    // After delFd, writing to efd should NOT trigger epoll
    uint64_t val = 1;
    write(efd, &val, sizeof(val));

    auto events = epoll.wait(10);
    REQUIRE(events.empty());

    close(efd);
}

TEST_CASE("Epoll: wait timeout returns empty", "[Epoll]") {
    Epoll epoll;
    auto events = epoll.wait(10);
    REQUIRE(events.empty());
}

TEST_CASE("Epoll: modFd changes event mask", "[Epoll]") {
    Epoll epoll;
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd >= 0);

    int sentinel = 42;
    REQUIRE(epoll.addFd(efd, EPOLLIN, &sentinel));
    REQUIRE(epoll.modFd(efd, EPOLLOUT, &sentinel));

    // EPOLLOUT should immediately fire on an eventfd (always writable)
    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    REQUIRE(events[0].events & EPOLLOUT);

    close(efd);
}

TEST_CASE("Epoll: multiple fds", "[Epoll]") {
    Epoll epoll;
    int efd1 = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int efd2 = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd1 >= 0);
    REQUIRE(efd2 >= 0);

    int s1 = 1, s2 = 2;
    REQUIRE(epoll.addFd(efd1, EPOLLIN, &s1));
    REQUIRE(epoll.addFd(efd2, EPOLLIN, &s2));

    // Trigger only efd2
    uint64_t val = 1;
    write(efd2, &val, sizeof(val));

    auto events = epoll.wait(100);
    REQUIRE_FALSE(events.empty());
    // At least efd2 should be ready
    bool found = false;
    for (auto &ev : events) {
        if (ev.data.ptr == &s2) found = true;
    }
    REQUIRE(found);

    close(efd1);
    close(efd2);
}
