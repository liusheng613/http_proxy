#include "event_poller.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace tunnel {

struct EventPoller::Impl {
    int epfd = -1;
};

EventPoller::~EventPoller() { close(); }

bool EventPoller::create() {
    if (impl_) return true;
    impl_ = new Impl;
    impl_->epfd = epoll_create1(EPOLL_CLOEXEC);
    return impl_->epfd >= 0;
}

static uint32_t to_epoll_events(uint32_t ev) {
    uint32_t r = 0;
    if (ev & EventPoller::READABLE)  r |= EPOLLIN;
    if (ev & EventPoller::WRITABLE)  r |= EPOLLOUT;
    if (ev & EventPoller::ERROR_EVT) r |= EPOLLERR | EPOLLHUP;
    if (ev & EventPoller::ET_MODE)   r |= EPOLLET;
    return r;
}

static uint32_t from_epoll_events(uint32_t ev) {
    uint32_t r = 0;
    if (ev & EPOLLIN)                 r |= EventPoller::READABLE;
    if (ev & EPOLLOUT)                r |= EventPoller::WRITABLE;
    if (ev & (EPOLLERR | EPOLLHUP))   r |= EventPoller::ERROR_EVT;
    return r;
}

bool EventPoller::add(int fd, uint32_t events) {
    if (!impl_ || impl_->epfd < 0) return false;
    epoll_event ev{};
    ev.events = to_epoll_events(events);
    ev.data.fd = fd;
    return epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EventPoller::mod(int fd, uint32_t events) {
    if (!impl_ || impl_->epfd < 0) return false;
    epoll_event ev{};
    ev.events = to_epoll_events(events);
    ev.data.fd = fd;
    return epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool EventPoller::del(int fd) {
    if (!impl_ || impl_->epfd < 0) return false;
    return epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int EventPoller::wait(Event* events, int max_events, int timeout_ms) {
    if (!impl_ || impl_->epfd < 0) return -1;
    epoll_event evs[max_events];
    int n = epoll_wait(impl_->epfd, evs, max_events, timeout_ms);
    if (n < 0) return -1;
    for (int i = 0; i < n; ++i) {
        events[i].fd = evs[i].data.fd;
        events[i].events = from_epoll_events(evs[i].events);
    }
    return n;
}

void EventPoller::close() {
    if (impl_) {
        if (impl_->epfd >= 0) ::close(impl_->epfd);
        delete impl_;
        impl_ = nullptr;
    }
}

}  // namespace tunnel
