// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (Utilities/Threading/CountdownLatch.cs).

#pragma once

#include <condition_variable>
#include <mutex>

namespace tiscsi::util {

// Counter that can be waited on until it drops to zero (e.g. in-flight commands).
class CountdownLatch {
public:
    void Add(int count = 1)
    {
        std::lock_guard lock(m_mutex);
        m_count += count;
    }

    void Decrement()
    {
        bool zero;
        {
            std::lock_guard lock(m_mutex);
            zero = --m_count <= 0;
        }
        if (zero) {
            m_zero.notify_all();
        }
    }

    void WaitUntilZero()
    {
        std::unique_lock lock(m_mutex);
        m_zero.wait(lock, [this] { return m_count <= 0; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_zero;
    int m_count = 0;
};

} // namespace tiscsi::util
