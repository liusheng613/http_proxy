#ifdef _WIN32

#include "event_poller.h"

#include <winsock2.h>
#include <windows.h>

#include <unordered_map>
#include <vector>

namespace tunnel {

struct EventPoller::Impl {
    std::unordered_map<int, uint32_t> fds;
};

EventPoller::~EventPoller() { close(); }

bool EventPoller::create() {
    if (impl_) return true;
    impl_ = new Impl;
    return true;
}

bool EventPoller::add(int fd, uint32_t events) {
    if (!impl_) return false;
    impl_->fds[fd] = events;
    return true;
}

bool EventPoller::mod(int fd, uint32_t events) {
    if (!impl_) return false;
    auto it = impl_->fds.find(fd);
    if (it == impl_->fds.end()) return false;
    it->second = events;
    return true;
}

bool EventPoller::del(int fd) {
    if (!impl_) return false;
    impl_->fds.erase(fd);
    return true;
}

int EventPoller::wait(EventPoller::Event* events, int max_events, int timeout_ms) {
    if (!impl_) return -1;
    if (impl_->fds.empty()) {
        if (timeout_ms > 0) { Sleep(timeout_ms); }
        return 0;
    }

    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
    int maxfd = -1;

    for (auto& kv : impl_->fds) {
        SOCKET fd = kv.first;
        uint32_t mask = kv.second;
        if (mask & EventPoller::READABLE)  FD_SET(fd, &rfds);
        if (mask & EventPoller::WRITABLE)  FD_SET(fd, &wfds);
        if (mask & EventPoller::ERROR_EVT) FD_SET(fd, &efds);
        if (fd > maxfd) maxfd = static_cast<int>(fd);
    }

    timeval tv{};
    timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    int n = select(maxfd + 1, &rfds, &wfds, &efds, ptv);
    if (n < 0) return -1;
    if (n == 0) return 0;

    int count = 0;
    for (auto& kv : impl_->fds) {
        if (count >= max_events) break;
        SOCKET fd = kv.first;
        uint32_t ev = 0;
        if (FD_ISSET(fd, &rfds)) ev |= EventPoller::READABLE;
        if (FD_ISSET(fd, &wfds)) ev |= EventPoller::WRITABLE;
        if (FD_ISSET(fd, &efds)) ev |= EventPoller::ERROR_EVT;
        if (ev != 0) {
            events[count].fd = static_cast<int>(fd);
            events[count].events = ev;
            ++count;
        }
    }
    return count;
}

void EventPoller::close() {
    if (impl_) { impl_->fds.clear(); delete impl_; impl_ = nullptr; }
}

}  // namespace tunnel

#endif  // _WIN32
