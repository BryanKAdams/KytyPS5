#include "kernel/syncOnAddress.h"

#include "common/threads.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::LibKernel::SyncOnAddress {

namespace {

constexpr uint32_t SIGNAL_POLL_MICROS = 10000;

using Clock = std::chrono::steady_clock;

template <typename T>
[[nodiscard]] bool IsValidWaitAddress(const volatile T* address) {
	return address != nullptr && (reinterpret_cast<uintptr_t>(address) & (alignof(T) - 1u)) == 0;
}

[[nodiscard]] bool IsValidWakeAddress(const volatile void* address) {
	return address != nullptr &&
	       (reinterpret_cast<uintptr_t>(address) & (alignof(uint32_t) - 1u)) == 0;
}

template <typename T>
[[nodiscard]] T ReadWord(const volatile T* address) {
	return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

struct WaitDeadline {
	bool              finite = false;
	Clock::time_point end {};
};

[[nodiscard]] WaitDeadline MakeDeadline(std::chrono::nanoseconds timeout) {
	const auto now = Clock::now();
	const auto remaining = Clock::time_point::max() - now;
	return {true, timeout >= remaining ? Clock::time_point::max() : now + timeout};
}

[[nodiscard]] WaitDeadline MakeDeadline(const uint32_t* timeout_micros) {
	if (timeout_micros == nullptr) {
		return {};
	}
	return MakeDeadline(std::chrono::microseconds(*timeout_micros));
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
[[nodiscard]] uint32_t GetWaitSliceMicros(const WaitDeadline& deadline, bool first_wait) {
	if (!deadline.finite) {
		return SIGNAL_POLL_MICROS;
	}

	const auto now = Clock::now();
	if (now >= deadline.end) {
		return first_wait ? 0u : UINT32_MAX;
	}

	const auto remaining =
	    std::chrono::ceil<std::chrono::microseconds>(deadline.end - now).count();
	return static_cast<uint32_t>(std::min<int64_t>(remaining, SIGNAL_POLL_MICROS));
}
#endif

void PollSignals(signal_poll_func_t signal_poll) {
	if (signal_poll != nullptr) {
		signal_poll();
	}
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)

template <typename T>
int WaitLinux(volatile T* address, T expected, const WaitDeadline& deadline,
              signal_poll_func_t signal_poll) {
	bool       first_wait = true;

	for (;;) {
		if (ReadWord(address) != expected) {
			return OK;
		}
		const auto slice_micros = GetWaitSliceMicros(deadline, first_wait);
		if (slice_micros == UINT32_MAX) {
			return ReadWord(address) == expected ? KERNEL_ERROR_ETIMEDOUT : OK;
		}
		const timespec timeout = {
		    .tv_sec  = static_cast<time_t>(slice_micros / 1000000u),
		    .tv_nsec = static_cast<long>(slice_micros % 1000000u) * 1000L,
		};

		long result     = 0;
		int  wait_error = 0;
		result          = syscall(SYS_futex, const_cast<T*>(address), FUTEX_WAIT_PRIVATE,
		                          static_cast<uint32_t>(expected), &timeout, nullptr, 0);
		if (result != 0) {
			wait_error = errno;
		}
		if (result == 0 || wait_error == EAGAIN) {
			return OK;
		}
		if (wait_error != ETIMEDOUT && wait_error != EINTR) {
			return KERNEL_ERROR_EINVAL;
		}

		PollSignals(signal_poll);
		if (deadline.finite && Clock::now() >= deadline.end) {
			return ReadWord(address) == expected ? KERNEL_ERROR_ETIMEDOUT : OK;
		}
		first_wait = false;
	}
}

int WakeLinux(volatile void* address, int32_t count) {
	// The legacy FUTEX_WAKE path may wake one waiter when count is zero.
	if (count == 0) {
		return OK;
	}

	const auto result = syscall(SYS_futex, const_cast<void*>(address), FUTEX_WAKE_PRIVATE, count,
	                            nullptr, nullptr, 0);
	return result < 0 ? KERNEL_ERROR_EINVAL : OK;
}

#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// Park on a private wake word rather than the guest address. A guest signal callback runs
// between native waits; a Wake during that callback must still release this registered wait.
// Intrusive stack nodes avoid per-wait allocations, and unrelated addresses use separate locks.
struct WindowsWaiter {
	const volatile void* address  = nullptr;
	WindowsWaiter*       previous = nullptr;
	WindowsWaiter*       next     = nullptr;
	uint32_t             woken    = 0;
};

struct alignas(64) WindowsWaitBucket {
	SRWLOCK        lock  = SRWLOCK_INIT;
	WindowsWaiter* first = nullptr;
	WindowsWaiter* last  = nullptr;
};

WindowsWaitBucket& WindowsBucketFor(const volatile void* address) {
	static std::array<WindowsWaitBucket, 256> buckets;
	auto                                      key = reinterpret_cast<uintptr_t>(address) >> 2u;
	key ^= key >> 16u;
	key ^= key >> 8u;
	return buckets[key & (buckets.size() - 1u)];
}

class WindowsWaitRegistration {
public:
	explicit WindowsWaitRegistration(WindowsWaiter& waiter)
	    : m_waiter(waiter), m_bucket(WindowsBucketFor(waiter.address)) {
		AcquireSRWLockExclusive(&m_bucket.lock);
		waiter.previous = m_bucket.last;
		if (m_bucket.last != nullptr) {
			m_bucket.last->next = &waiter;
		} else {
			m_bucket.first = &waiter;
		}
		m_bucket.last = &waiter;
		ReleaseSRWLockExclusive(&m_bucket.lock);
	}

	~WindowsWaitRegistration() { Unregister(); }

	void Unregister() {
		if (!m_registered) return;
		// Wake holds this lock until its last access to the node, including the native wake.
		// Consequently the stack word cannot disappear while another thread still signals it.
		AcquireSRWLockExclusive(&m_bucket.lock);
		if (m_waiter.previous != nullptr) {
			m_waiter.previous->next = m_waiter.next;
		} else {
			m_bucket.first = m_waiter.next;
		}
		if (m_waiter.next != nullptr) {
			m_waiter.next->previous = m_waiter.previous;
		} else {
			m_bucket.last = m_waiter.previous;
		}
		ReleaseSRWLockExclusive(&m_bucket.lock);
		m_registered = false;
	}

	WindowsWaitRegistration(const WindowsWaitRegistration&)            = delete;
	WindowsWaitRegistration& operator=(const WindowsWaitRegistration&) = delete;

private:
	WindowsWaiter&     m_waiter;
	WindowsWaitBucket& m_bucket;
	bool               m_registered = true;
};

template <typename T>
int WaitWindows(volatile T* address, T expected, const WaitDeadline& deadline,
                signal_poll_func_t signal_poll) {
	if (ReadWord(address) != expected) return OK;

	WindowsWaiter           waiter {.address = address};
	WindowsWaitRegistration registration(waiter);
	int                     result = OK;
	for (;;) {
		// Recheck after registration: a store/wake racing the initial comparison must not
		// leave us sleeping. The private word also remembers wakes before WaitOnAddress parks.
		if (ReadWord(&waiter.woken) != 0 || ReadWord(address) != expected) break;

		DWORD milliseconds = INFINITE;
		if (deadline.finite) {
			const auto now = Clock::now();
			if (now >= deadline.end) {
				result = KERNEL_ERROR_ETIMEDOUT;
				break;
			}
			const auto remaining =
			    std::chrono::ceil<std::chrono::milliseconds>(deadline.end - now).count();
			milliseconds = static_cast<DWORD>(std::min<int64_t>(remaining, INFINITE - 1u));
		}
		if (signal_poll != nullptr) {
			milliseconds = std::min<DWORD>(milliseconds, SIGNAL_POLL_MICROS / 1000u);
		}

		uint32_t comparison = 0;
		if (!WaitOnAddress(&waiter.woken, &comparison, sizeof(comparison), milliseconds) &&
		    GetLastError() != ERROR_TIMEOUT) {
			result = KERNEL_ERROR_EINVAL;
			break;
		}
		// Native spurious wakes do not consume a guest wake. Callbacks run without a bucket
		// lock, and any explicit Wake during the callback remains recorded in waiter.woken.
		PollSignals(signal_poll);
	}
	// Serialize timeout completion with counted wakes. A wake that selected this node before
	// removal must produce success, even when it raced the deadline/error check above.
	registration.Unregister();
	return ReadWord(&waiter.woken) != 0 || ReadWord(address) != expected ? OK : result;
}

int WakeWindows(volatile void* address, int32_t count) {
	if (count == 0) return OK;
	auto& bucket = WindowsBucketFor(address);
	AcquireSRWLockExclusive(&bucket.lock);
	for (auto* waiter = bucket.first; waiter != nullptr && count != 0; waiter = waiter->next) {
		if (waiter->address != address || ReadWord(&waiter->woken) != 0) continue;
		__atomic_store_n(&waiter->woken, 1u, __ATOMIC_RELEASE);
		WakeByAddressSingle(&waiter->woken);
		--count;
	}
	ReleaseSRWLockExclusive(&bucket.lock);
	return OK;
}

#else

struct PortableWaiter {
	Common::CondVar condition;
	bool            wake_requested = false;
};

struct PortableAddressEntry {
	Common::Mutex              mutex;
	std::list<PortableWaiter*> waiters;
};

struct PortableAddressRegistry {
	std::mutex                                                           mutex;
	std::unordered_map<uintptr_t, std::shared_ptr<PortableAddressEntry>> entries;
};

PortableAddressRegistry& GetPortableRegistry() {
	static PortableAddressRegistry registry;
	return registry;
}

std::shared_ptr<PortableAddressEntry> RegisterPortableWaiter(volatile void*  address,
                                                             PortableWaiter* waiter) {
	auto&           registry = GetPortableRegistry();
	std::lock_guard registry_lock(registry.mutex);
	auto&           entry = registry.entries[reinterpret_cast<uintptr_t>(address)];
	if (!entry) {
		entry = std::make_shared<PortableAddressEntry>();
	}
	entry->mutex.Lock();
	entry->waiters.push_back(waiter);
	return entry;
}

void UnregisterPortableWaiter(volatile void*                               address,
                              const std::shared_ptr<PortableAddressEntry>& entry,
                              PortableWaiter*                              waiter) {
	entry->waiters.remove(waiter);
	const bool empty = entry->waiters.empty();
	entry->mutex.Unlock();
	if (!empty) {
		return;
	}

	auto&           registry = GetPortableRegistry();
	std::lock_guard registry_lock(registry.mutex);
	entry->mutex.Lock();
	const auto it = registry.entries.find(reinterpret_cast<uintptr_t>(address));
	if (it != registry.entries.end() && it->second == entry && entry->waiters.empty()) {
		registry.entries.erase(it);
	}
	entry->mutex.Unlock();
}

template <typename T>
int WaitPortable(volatile T* address, T expected, const WaitDeadline& deadline,
                 signal_poll_func_t signal_poll) {
	PortableWaiter waiter;
	auto           entry      = RegisterPortableWaiter(address, &waiter);
	bool           first_wait = true;
	int            result     = OK;

	while (ReadWord(address) == expected && !waiter.wake_requested) {
		const auto slice_micros = GetWaitSliceMicros(deadline, first_wait);
		if (slice_micros == UINT32_MAX) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}
		if (slice_micros == 0) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}

		(void)waiter.condition.WaitFor(&entry->mutex, slice_micros);
		entry->mutex.Unlock();
		PollSignals(signal_poll);
		entry->mutex.Lock();

		if (deadline.finite && Clock::now() >= deadline.end && ReadWord(address) == expected &&
		    !waiter.wake_requested) {
			result = KERNEL_ERROR_ETIMEDOUT;
			break;
		}
		first_wait = false;
	}

	UnregisterPortableWaiter(address, entry, &waiter);
	return result;
}

int WakePortable(volatile void* address, int32_t count) {
	if (count == 0) {
		return OK;
	}
	auto&            registry = GetPortableRegistry();
	std::unique_lock registry_lock(registry.mutex);
	const auto       it = registry.entries.find(reinterpret_cast<uintptr_t>(address));
	if (it == registry.entries.end()) {
		return OK;
	}
	auto entry = it->second;
	entry->mutex.Lock();
	registry_lock.unlock();

	int32_t remaining = count;
	for (auto* waiter: entry->waiters) {
		if (!waiter->wake_requested) {
			waiter->wake_requested = true;
			waiter->condition.Signal();
			if (remaining != INT_MAX && --remaining == 0) {
				break;
			}
		}
	}
	entry->mutex.Unlock();
	return OK;
}

#endif

template <typename T>
int WaitImpl(volatile T* address, T expected, const WaitDeadline& deadline,
             signal_poll_func_t signal_poll) {
	if (!IsValidWaitAddress(address)) {
		return KERNEL_ERROR_EINVAL;
	}

	int result = OK;
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	result = WaitLinux(address, expected, deadline, signal_poll);
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	result = WaitWindows(address, expected, deadline, signal_poll);
#else
	result = WaitPortable(address, expected, deadline, signal_poll);
#endif
	PollSignals(signal_poll);
	return result;
}

} // namespace

int Wait32(volatile uint32_t* address, uint32_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout_micros), signal_poll);
}

int Wait64(volatile uint64_t* address, uint64_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout_micros), signal_poll);
}

int Wait64(volatile uint64_t* address, uint64_t expected, std::chrono::nanoseconds timeout,
           signal_poll_func_t signal_poll) {
	return WaitImpl(address, expected, MakeDeadline(timeout), signal_poll);
}

int Wake(volatile void* address, int32_t count) {
	if (!IsValidWakeAddress(address) || count < 0) {
		return KERNEL_ERROR_EINVAL;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	return WakeLinux(address, count);
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return WakeWindows(address, count);
#else
	return WakePortable(address, count);
#endif
}

} // namespace Libs::LibKernel::SyncOnAddress
