#include "graphics/host_gpu/renderer/drainStats.h"

#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <fmt/format.h>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace Libs::Graphics::DrainStats {

namespace {

constexpr size_t KindCount   = static_cast<size_t>(Kind::Count);
constexpr size_t ReasonCount = static_cast<size_t>(Reason::Count);
constexpr size_t OpCount     = Pm4OpCount + 1;
constexpr size_t CellCount   = KindCount * ReasonCount * OpCount;

struct Cell {
	std::atomic<uint64_t> count {0};
	std::atomic<uint64_t> value {0};
};

struct Snapshot {
	std::vector<uint64_t> count = std::vector<uint64_t>(CellCount);
	std::vector<uint64_t> value = std::vector<uint64_t>(CellCount);
	uint64_t              frames   = 0;
	uint64_t              presents = 0;
};

Cell                  g_cells[CellCount];
std::atomic<uint64_t> g_frames {0};
std::atomic<uint64_t> g_presents {0};

std::mutex                  g_reporter_mutex;
std::condition_variable_any g_reporter_wake;
std::jthread                g_reporter;
// Only an explicit Stop() prints the session total; static destruction may follow Log shutdown.
std::atomic_bool            g_final_report {false};

constexpr size_t Index(Kind kind, Reason reason, uint32_t op) {
	return (static_cast<size_t>(kind) * ReasonCount + static_cast<size_t>(reason)) * OpCount +
	       std::min<size_t>(op, NoPm4Op);
}

const char* KindName(Kind kind) {
	switch (kind) {
		case Kind::FullDrain: return "full-drain";
		case Kind::TickWait: return "tick-wait";
		case Kind::PriorityWait: return "priority-wait";
		case Kind::BlockedPoll: return "blocked-poll";
		case Kind::Readback: return "readback";
		case Kind::ReadbackClean: return "readback-clean";
		case Kind::DccMetaWrite: return "dcc-meta-write";
		case Kind::DccCheck: return "dcc-check";
		case Kind::DccGpuCheck: return "dcc-gpu-check";
		case Kind::Count: break;
	}
	return "?";
}

const char* ReasonName(Reason reason) {
	switch (reason) {
		case Reason::Unattributed: return "unattributed";
		case Reason::GuestReadFault: return "guest-read-fault";
		case Reason::GuestWriteFault: return "guest-write-fault";
		case Reason::GpuThreadReadFault: return "gpu-thread-read-fault";
		case Reason::GpuThreadWriteFault: return "gpu-thread-write-fault";
		case Reason::KernelInvalidate: return "kernel-invalidate";
		case Reason::DccClear: return "dcc-clear";
		case Reason::Predicate: return "predicate";
		case Reason::GdsReadback: return "gds-readback";
		case Reason::Unmap: return "unmap";
		case Reason::TexturePendingDownload: return "texture-pending-download";
		case Reason::FreeImage: return "free-image";
		case Reason::TextureGc: return "texture-gc";
		case Reason::BufferGc: return "buffer-gc";
		case Reason::UploadRingWrap: return "upload-ring-wrap";
		case Reason::StreamRingWrap: return "stream-ring-wrap";
		case Reason::DownloadRingWrap: return "download-ring-wrap";
		case Reason::FaultBuffer: return "fault-buffer";
		case Reason::PresentFrame: return "present-frame";
		case Reason::Count: break;
	}
	return "?";
}

std::string OpName(uint32_t op) {
	if (op >= NoPm4Op) {
		return "-";
	}
	if (op >= 256) {
		switch (op - 256) {
			case 0x05: return "R_DRAW_RESET";
			case 0x06: return "R_WAIT_FLIP_DONE";
			case 0x09: return "R_DISPATCH_RESET";
			case 0x14: return "R_ACQUIRE_MEM";
			case 0x15: return "R_WRITE_DATA";
			case 0x17: return "R_FLIP";
			case 0x18: return "R_RELEASE_MEM";
			case 0x19: return "R_DMA_DATA";
			case 0x1A: return "R_CONTEXT_STATE";
			default: return fmt::format("NOP_R{:#04x}", op - 256);
		}
	}
	switch (op) {
		case 0x15: return "DISPATCH_DIRECT";
		case 0x16: return "DISPATCH_INDIRECT";
		case 0x20: return "SET_PREDICATION";
		case 0x22: return "COND_EXEC";
		case 0x24: return "DRAW_INDIRECT";
		case 0x25: return "DRAW_INDEX_INDIRECT";
		case 0x27: return "DRAW_INDEX_2";
		case 0x2C: return "DRAW_INDIRECT_MULTI";
		case 0x2D: return "DRAW_INDEX_AUTO";
		case 0x35: return "DRAW_INDEX_OFFSET_2";
		case 0x37: return "WRITE_DATA";
		case 0x38: return "DRAW_INDEX_INDIRECT_MULTI";
		case 0x3C: return "WAIT_REG_MEM";
		case 0x40: return "COPY_DATA";
		case 0x41: return "CP_DMA";
		case 0x46: return "EVENT_WRITE";
		case 0x47: return "EVENT_WRITE_EOP";
		case 0x48: return "EVENT_WRITE_EOS";
		case 0x49: return "RELEASE_MEM";
		case 0x50: return "DMA_DATA";
		case 0x58: return "ACQUIRE_MEM";
		case 0x8D: return "DISPATCH_DRAW";
		default: return fmt::format("op{:#04x}", op);
	}
}

Snapshot Take() {
	Snapshot snapshot;
	for (size_t i = 0; i < CellCount; i++) {
		snapshot.count[i] = g_cells[i].count.load(std::memory_order_relaxed);
		snapshot.value[i] = g_cells[i].value.load(std::memory_order_relaxed);
	}
	snapshot.frames   = g_frames.load(std::memory_order_relaxed);
	snapshot.presents = g_presents.load(std::memory_order_relaxed);
	return snapshot;
}

struct Row {
	Kind     kind;
	Reason   reason;
	uint32_t op;
	uint64_t count;
	uint64_t value;
};

void Report(const Snapshot& before, const Snapshot& after, double seconds) {
	const auto frames = after.frames - before.frames;
	const auto per    = [frames](double value) { return frames == 0 ? 0.0 : value / frames; };

	std::array<uint64_t, KindCount> kind_count {};
	std::array<uint64_t, KindCount> kind_value {};
	std::vector<Row>                rows;
	for (size_t kind = 0; kind < KindCount; kind++) {
		for (size_t reason = 0; reason < ReasonCount; reason++) {
			for (uint32_t op = 0; op < OpCount; op++) {
				const auto index = Index(static_cast<Kind>(kind), static_cast<Reason>(reason), op);
				const auto count = after.count[index] - before.count[index];
				if (count == 0) {
					continue;
				}
				const auto value = after.value[index] - before.value[index];
				kind_count[kind] += count;
				kind_value[kind] += value;
				rows.push_back({static_cast<Kind>(kind), static_cast<Reason>(reason), op, count,
				                value});
			}
		}
	}

	std::string text = fmt::format(
	    "drain-stats: {:.1f}s frames={} ({:.1f}/s) presents={}", seconds, frames,
	    frames / seconds, after.presents - before.presents);
	for (const auto kind: {Kind::FullDrain, Kind::TickWait, Kind::PriorityWait, Kind::BlockedPoll}) {
		const auto k  = static_cast<size_t>(kind);
		const auto ms = static_cast<double>(kind_value[k]) / 1e6;
		text += fmt::format(" | {} n={} {:.1f}ms ({:.2f}ms/frame)", KindName(kind), kind_count[k],
		                    ms, per(ms));
	}
	const auto rb = static_cast<size_t>(Kind::Readback);
	text += fmt::format(" | readback n={} {:.1f}MiB clean={}\n", kind_count[rb],
	                    static_cast<double>(kind_value[rb]) / (1024.0 * 1024.0),
	                    kind_count[static_cast<size_t>(Kind::ReadbackClean)]);

	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
		const bool a_time = a.kind < Kind::Readback;
		const bool b_time = b.kind < Kind::Readback;
		if (a_time != b_time) {
			return a_time;
		}
		return a_time ? a.value > b.value : a.count > b.count;
	});
	size_t printed = 0;
	for (const auto& row: rows) {
		if (printed++ == 24) {
			break;
		}
		if (row.kind < Kind::Readback) {
			const auto ms = static_cast<double>(row.value) / 1e6;
			text += fmt::format("  {:<14} {:<26} {:<26} n={:<6} {:8.2f}ms avg={:.3f}ms\n",
			                    KindName(row.kind), ReasonName(row.reason), OpName(row.op),
			                    row.count, ms, ms / static_cast<double>(row.count));
		} else if (row.kind == Kind::DccCheck || row.kind == Kind::DccGpuCheck) {
			text += fmt::format("  {:<14} {:<26} {:<26} n={:<6} {}={}\n", KindName(row.kind),
			                    ReasonName(row.reason), OpName(row.op), row.count,
			                    row.kind == Kind::DccCheck ? "cleared-slices" : "slices",
			                    row.value);
		} else {
			text += fmt::format("  {:<14} {:<26} {:<26} n={:<6} {:8.2f}MiB\n", KindName(row.kind),
			                    ReasonName(row.reason), OpName(row.op), row.count,
			                    static_cast<double>(row.value) / (1024.0 * 1024.0));
		}
	}
	Log::WriteToConsoleAndLog(text);
}

void Run(std::stop_token stop, uint32_t interval_seconds) {
	auto previous      = Take();
	auto previous_time = std::chrono::steady_clock::now();
	const auto first   = previous;
	const auto start   = previous_time;
	while (!stop.stop_requested()) {
		{
			std::unique_lock lock(g_reporter_mutex);
			g_reporter_wake.wait_for(lock, stop, std::chrono::seconds(interval_seconds),
			                         [] { return false; });
		}
		auto current      = Take();
		auto current_time = std::chrono::steady_clock::now();
		if (stop.stop_requested()) {
			if (g_final_report.load(std::memory_order_acquire)) {
				Log::WriteToConsoleAndLog("drain-stats: session total\n");
				Report(first, current,
				       std::chrono::duration<double>(current_time - start).count());
			}
			break;
		}
		Report(previous, current,
		       std::chrono::duration<double>(current_time - previous_time).count());
		previous      = std::move(current);
		previous_time = current_time;
	}
}

} // namespace

void Start(uint32_t interval_seconds) {
	if (interval_seconds == 0 || g_reporter.joinable()) {
		return;
	}
	g_enabled.store(true, std::memory_order_relaxed);
	g_reporter = std::jthread(Run, interval_seconds);
}

void Stop() {
	if (g_reporter.joinable()) {
		g_final_report.store(true, std::memory_order_release);
		g_reporter.request_stop();
		g_reporter.join();
	}
	g_enabled.store(false, std::memory_order_relaxed);
}

void Record(Kind kind, Reason reason, uint32_t pm4_op, uint64_t value) noexcept {
	auto& cell = g_cells[Index(kind, reason, pm4_op)];
	cell.count.fetch_add(1, std::memory_order_relaxed);
	cell.value.fetch_add(value, std::memory_order_relaxed);
}

void CountFrame(bool new_frame) noexcept {
	if (!Enabled()) {
		return;
	}
	g_presents.fetch_add(1, std::memory_order_relaxed);
	if (new_frame) {
		g_frames.fetch_add(1, std::memory_order_relaxed);
	}
}

} // namespace Libs::Graphics::DrainStats
