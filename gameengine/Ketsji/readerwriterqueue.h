#ifndef MOODYCAMEL_READERWRITERQUEUE_H
#define MOODYCAMEL_READERWRITERQUEUE_H

// ©2013-2020 Cameron Desrochers.
// Distributed under the simplified BSD license (see the license file that
// should have come with this header).

// Modified by ThunderBlue Studios (2026):
// - Removed unused features.
// - Reduced dependencies.
// - Adapted for the BlenderPlayer/UPBGE runtime.
// Based on the original ReaderWriterQueue implementation by Cameron Desrochers.
// Simplified for the requirements of this project.
//
// The unmodified, full-featured implementation is also included in
// readerwriterqueue/readerwriterqueue.h for advanced use cases.

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace moodycamel {

template<typename T>
class ReaderWriterQueue
{
public:
	explicit ReaderWriterQueue(std::size_t capacity = 1024)
		: m_capacity(NormalizeCapacity(capacity)),
		  m_mask(m_capacity - 1),
		  m_buffer(new T[m_capacity])
	{
	}

	ReaderWriterQueue(const ReaderWriterQueue&) = delete;
	ReaderWriterQueue& operator=(const ReaderWriterQueue&) = delete;

	bool enqueue(const T& item)
	{
		static_assert(std::is_copy_assignable<T>::value || std::is_copy_constructible<T>::value, "T must be copyable");
		const std::size_t tail = m_tail.load(std::memory_order_relaxed);
		const std::size_t next = (tail + 1) & m_mask;
		if (next == m_head.load(std::memory_order_acquire)) {
			return false;
		}
		m_buffer[tail] = item;
		m_tail.store(next, std::memory_order_release);
		return true;
	}

	bool enqueue(T&& item)
	{
		static_assert(std::is_move_assignable<T>::value || std::is_move_constructible<T>::value, "T must be movable");
		const std::size_t tail = m_tail.load(std::memory_order_relaxed);
		const std::size_t next = (tail + 1) & m_mask;
		if (next == m_head.load(std::memory_order_acquire)) {
			return false;
		}
		m_buffer[tail] = std::move(item);
		m_tail.store(next, std::memory_order_release);
		return true;
	}

	bool try_dequeue(T& item)
	{
		const std::size_t head = m_head.load(std::memory_order_relaxed);
		if (head == m_tail.load(std::memory_order_acquire)) {
			return false;
		}
		item = std::move(m_buffer[head]);
		m_head.store((head + 1) & m_mask, std::memory_order_release);
		return true;
	}

private:
	static std::size_t NormalizeCapacity(std::size_t capacity)
	{
		if (capacity < 2) {
			capacity = 2;
		}
		std::size_t pow2 = 1;
		while (pow2 < capacity) {
			pow2 <<= 1;
		}
		return pow2;
	}

	const std::size_t m_capacity;
	const std::size_t m_mask;
	std::unique_ptr<T[]> m_buffer;

	std::atomic<std::size_t> m_head {0};
	std::atomic<std::size_t> m_tail {0};
};

}  // namespace moodycamel

#endif  // MOODYCAMEL_READERWRITERQUEUE_H
