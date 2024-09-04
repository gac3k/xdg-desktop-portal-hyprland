/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "../config.h"

#include <cassert>
#include <cerrno>
#include <sys/epoll.h>

#include "util-object.h"
#include "util-io.h"
#include "util-list.h"
#include "util-sources.h"

struct sink {
    object object;
    int epollfd;
    list sources;
    list sources_removed;
};

enum source_close_behavior {
    SOURCE_CLOSE_FD_ON_REMOVE = 1, /* default */
    SOURCE_CLOSE_FD_ON_DESTROY,
    SOURCE_CLOSE_FD_NEVER,
};

struct source {
    object object;
    sink* sink;
    list link; /* sink.sources or sink.sources_removed */
    source_dispatch_t dispatch;
    void* user_data;
    source_close_behavior close_behavior;
    int fd;
    bool is_active;
};

OBJECT_IMPLEMENT_REF(source);
OBJECT_IMPLEMENT_UNREF_CLEANUP(source);
OBJECT_IMPLEMENT_GETTER(source, fd, int);
OBJECT_IMPLEMENT_GETTER(source, user_data, void*);
OBJECT_IMPLEMENT_SETTER(source, user_data, void*);

/**
 * Remove the source, closing the fd. The source is tagged as removed and
 * will be removed whenever sink_dispatch() finishes (or is called next).
 */
void source_remove(source* src) {
    if (!src || !src->is_active)
        return;

    epoll_ctl(src->sink->epollfd, EPOLL_CTL_DEL, src->fd, nullptr);
    if (src->close_behavior == SOURCE_CLOSE_FD_ON_REMOVE)
        src->fd = xclose(src->fd);
    src->is_active = false;
    source_unref(src);

    // Note: sources list was the owner of the source, new owner
    // is the removed list
    list_remove(&src->link);
    list_append(&src->sink->sources_removed, &src->link);
    src->sink = nullptr;
}

// Ignore, use source_unref()
static void source_destroy(source* src) {
    // We expect source_remove() to be called before we ever get here
    assert(!src->is_active);

    if (src->close_behavior == SOURCE_CLOSE_FD_ON_DESTROY)
        src->fd = xclose(src->fd);
}

static OBJECT_IMPLEMENT_CREATE(source);

source* source_new(int sourcefd, source_dispatch_t dispatch, void* user_data) {
    source* src = source_create(nullptr);

    src->dispatch = dispatch;
    src->user_data = user_data;
    src->fd = sourcefd;
    src->close_behavior = SOURCE_CLOSE_FD_ON_REMOVE;
    src->is_active = false;
    list_init(&src->link);

    return src;
}

void source_never_close_fd(source* s) {
    s->close_behavior = SOURCE_CLOSE_FD_NEVER;
}

static void sink_destroy(sink* s) {
    source* src;
    list_for_each_safe(src, &s->sources, link) {
        source_remove(src);
    }
    list_for_each_safe(src, &s->sources_removed, link) {
        source_unref(src);
    }
    xclose(s->epollfd);
}

OBJECT_IMPLEMENT_UNREF_CLEANUP(sink);
static OBJECT_IMPLEMENT_CREATE(sink);

int sink_get_fd(sink* s) {
    assert(s);
    return s->epollfd;
}

sink* sink_new() {
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0)
        return nullptr;

    sink* s = sink_create(nullptr);

    s->epollfd = fd;
    list_init(&s->sources);
    list_init(&s->sources_removed);

    return s;
}

int sink_dispatch(sink* s) {
    epoll_event ep[32];
    int count = epoll_wait(s->epollfd, ep, sizeof(ep)/sizeof(ep[0]), 0);
    if (count < 0)
        return -errno;

    for (int i = 0; i < count; ++i) {
        source* src = static_cast<source*>(ep[i].data.ptr);
        if (src->fd == -1)
            continue;

        src->dispatch(src, src->user_data);
    }

    source* src;
    list_for_each_safe(src, &s->sources_removed, link) {
        list_remove(&src->link);
        list_init(&src->link);
        source_unref(src);
    }

    return 0;
}

int sink_add_source(sink* s, source* src) {
    struct epoll_event e = {
        .events = EPOLLIN,
        .data = {
            .ptr =  source_ref(src)
        },
    };


    int rc = xerrno(epoll_ctl(s->epollfd, EPOLL_CTL_ADD, source_get_fd(src), &e));
    if (rc < 0) {
        source_unref(src);
        return rc;
    }

    src->is_active = true;
    src->sink = s;
    source_ref(src);
    list_append(&s->sources, &src->link);

    return 0;
}

int source_enable_write(source* src, bool enable) {
    assert(src->is_active);

    epoll_event e = {
        .events = EPOLLIN | (enable ? EPOLLOUT : 0),
        .data = {
            .ptr = src
        }
    };

    int rc = xerrno(epoll_ctl(src->sink->epollfd, EPOLL_CTL_MOD, source_get_fd(src), &e));
    if (rc < 0) {
        source_unref(src);
        return rc;
    }
    return 0;
}

#if _enable_tests_
#include <fcntl.h>
#include <signal.h>

#include "util-munit.h"
#include "util-macros.h"

MUNIT_TEST(test_sink) {
    sink* s = sink_new();
    sink_dispatch(s);
    sink_dispatch(s);

    int fd = sink_get_fd(s);
    munit_assert_int(fd, !=, -1);

    sink_unref(s);

    return MUNIT_OK;
}

struct buffer {
    size_t size;
    size_t len;
    char* buffer;
};

static void read_buffer(source* src, void* user_data) {
    buffer* buf = static_cast<buffer*>(user_data);
    size_t sz = max(buf->size, 1024);

    buf->size = sz;
    buf->buffer = xrealloc(buf->buffer, sz);

    int nread = read(source_get_fd(src), buf->buffer, sz);
    munit_assert_int(nread, >=, 0);

    buf->len = nread;
}

MUNIT_TEST(test_source) {
    std::unique_ptr<sink> s(sink_new());

    int fd[2];
    int rc = pipe2(fd, O_CLOEXEC|O_NONBLOCK);
    munit_assert_int(rc, !=, -1);

    buffer buf = {0};
    source* src = source_new(fd[0], read_buffer, &buf);

    munit_assert_int(source_get_fd(src), ==, fd[0]);

    sink_add_source(s.get(), src);

    // Nothing to read yet, dispatch is a noop
    sink_dispatch(s.get());
    munit_assert_int(buf.len, ==, 0);

    const char token[] = "foobar";
    int wrc = write(fd[1], token, sizeof(token));
    munit_assert_int(wrc, ==, sizeof(token));

    // haven't called dispatch yet
    munit_assert_int(buf.len, ==, 0);
    sink_dispatch(s.get());
    munit_assert_int(buf.len, ==, sizeof(token));
    munit_assert_string_equal(buf.buffer, token);

    // multiple removals shouldn't matter
    source_remove(src);
    source_remove(src);
    sink_dispatch(s.get());
    source_remove(src);
    sink_dispatch(s.get());

    // source pipe is already closed
    signal(SIGPIPE, SIG_IGN);
    const char token2[] = "bazbat";
    wrc = write(fd[1], token2, sizeof(token2));
    munit_assert_int(wrc, ==, -1);
    munit_assert_int(errno, ==, EPIPE);

    sink_dispatch(s.get());
    source_unref(src);
    sink_dispatch(s.get());

    free(buf.buffer);

    return MUNIT_OK;
}

static void drain_data(source* src, void* user_data) {
    char buf[1024] = {0};
    read(source_get_fd(src), buf, sizeof(buf));
}

MUNIT_TEST(test_source_readd) {
    std::unique_ptr<sink> s(sink_new());

    int fd[2];
    int rc = pipe2(fd, O_CLOEXEC|O_NONBLOCK);
    munit_assert_int(rc, !=, -1);

    std::unique_ptr<source> src(source_new(fd[0], drain_data, nullptr));
    sink_add_source(s.get(), src.get());
    sink_dispatch(s.get());
    // remove and re-add without calling dispatch
    source_remove(src.get());
    sink_add_source(s.get(), src.get());
    source_remove(src.get());

    return MUNIT_OK;
}

static void count_calls(source* src, void* user_data) {
    unsigned int* arg = static_cast<unsigned int*>(user_data);
    *arg = *arg + 1;
}

MUNIT_TEST(test_source_write) {
    std::unique_ptr<sink> s(sink_new());

    int fd[2];
    int rc = pipe2(fd, O_CLOEXEC|O_NONBLOCK);
    munit_assert_int(rc, !=, -1);

    int read_fd = fd[0];
    int write_fd = fd[1];

    int dispatch_called = 0;
    std::unique_ptr<source> src(source_new(write_fd, count_calls, &dispatch_called));
    sink_add_source(s.get(), src.get());
    sink_dispatch(s.get());
    sink_dispatch(s.get());
    sink_dispatch(s.get());

    munit_assert_uint(dispatch_called, ==, 0);

    source_enable_write(src.get(), true);
    sink_dispatch(s.get());
    munit_assert_uint(dispatch_called, ==, 1);
    sink_dispatch(s.get());
    munit_assert_uint(dispatch_called, ==, 2);

    // Fill up the buffer
    do {
        char buf[4096] = {0};
        rc = write(write_fd, buf, sizeof(buf));
    } while (rc != -1);
    munit_assert_int(errno, ==, EAGAIN);

    // Buffer is full, expect our dispatch to NOT be called
    sink_dispatch(s.get());
    munit_assert_uint(dispatch_called, ==, 2);
    sink_dispatch(s.get());
    munit_assert_uint(dispatch_called, ==, 2);

    do {
        char buf[406];
        rc = read(read_fd, buf, sizeof(buf));
    } while (rc != -1);
    munit_assert_int(errno, ==, EAGAIN);

    sink_dispatch(s.get());
    munit_assert_uint(dispatch_called, ==, 3);

    source_enable_write(src.get(), false);

    sink_dispatch(s.get());
    munit_assert_uint(dispatch_called, ==, 3);

    return MUNIT_OK;
}
#endif
