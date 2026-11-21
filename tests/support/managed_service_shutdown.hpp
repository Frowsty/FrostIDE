// SPDX-License-Identifier: Apache-2.0
// Drains a real managed service before its reactor and scheduler on failed assertions.
#pragma once

#include "frost/io/reactor.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/sched/thread_pool.hpp"

namespace frost::test {

class ManagedServiceShutdown {
public:
    ManagedServiceShutdown(lang::ManagedService& service, io::Reactor& reactor, sched::ThreadPool& pool)
        : m_service(service), m_reactor(reactor), m_pool(pool) {}
    ~ManagedServiceShutdown() { stop(); }
    ManagedServiceShutdown(const ManagedServiceShutdown&) = delete;
    ManagedServiceShutdown& operator=(const ManagedServiceShutdown&) = delete;

    void stop() {
        if (m_stopped)
            return;
        m_pool.spawn(sched::Lane::UserVisible, m_service.stop()).join();
        m_reactor.shutdown();
        m_pool.shutdown();
        m_stopped = true;
    }

private:
    lang::ManagedService& m_service;
    io::Reactor& m_reactor;
    sched::ThreadPool& m_pool;
    bool m_stopped = false;
};

} // namespace frost::test
