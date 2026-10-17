// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (Utilities/Generics/BlockingQueue.cs).

#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace tiscsi::util {

// Multi-producer queue whose consumers block until an item arrives or the
// queue is stopped. Items enqueued after Stop() are dropped.
template <typename T>
class BlockingQueue {
public:
    void Enqueue(T item)
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_items.push_back(std::move(item));
        }
        m_available.notify_one();
    }

    void Enqueue(std::vector<T> items)
    {
        if (items.empty()) {
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                return;
            }
            for (T& item : items) {
                m_items.push_back(std::move(item));
            }
        }
        m_available.notify_all();
    }

    // Returns std::nullopt once the queue has been stopped.
    std::optional<T> Dequeue()
    {
        std::unique_lock lock(m_mutex);
        m_available.wait(lock, [this] { return m_stopping || !m_items.empty(); });
        if (m_stopping) {
            return std::nullopt;
        }
        T item = std::move(m_items.front());
        m_items.pop_front();
        return item;
    }

    // Wakes all consumers and discards pending items.
    void Stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_items.clear();
        }
        m_available.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_available;
    std::deque<T> m_items;
    bool m_stopping = false;
};

} // namespace tiscsi::util
