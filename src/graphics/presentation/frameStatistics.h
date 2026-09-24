#ifndef KYTY_GRAPHICS_PRESENTATION_FRAME_STATISTICS_H_
#define KYTY_GRAPHICS_PRESENTATION_FRAME_STATISTICS_H_

#include <cstdint>

namespace Libs::Graphics {

class FrameStatistics {
public:
	bool Record(uint64_t now, uint64_t frequency, bool new_frame) {
		++m_total_presents;
		m_total_frames += new_frame ? 1u : 0u;
		if (!m_initialized || now < m_start) {
			m_initialized  = true;
			m_start        = now;
			m_frames       = 0;
			m_presents     = 0;
			m_frame_rate   = 0.0;
			m_present_rate = 0.0;
			return true;
		}
		++m_presents;
		m_frames += new_frame ? 1u : 0u;
		const auto elapsed = now - m_start;
		if (frequency == 0 || elapsed < frequency) {
			return false;
		}
		m_frame_rate   = static_cast<double>(m_frames) * static_cast<double>(frequency) /
		                 static_cast<double>(elapsed);
		m_present_rate = static_cast<double>(m_presents) * static_cast<double>(frequency) /
		                 static_cast<double>(elapsed);
		m_start        = now;
		m_frames       = 0;
		m_presents     = 0;
		return true;
	}

	[[nodiscard]] uint64_t TotalFrames() const noexcept { return m_total_frames; }
	[[nodiscard]] uint64_t TotalPresents() const noexcept { return m_total_presents; }
	[[nodiscard]] double   FrameRate() const noexcept { return m_frame_rate; }
	[[nodiscard]] double   PresentRate() const noexcept { return m_present_rate; }

private:
	uint64_t m_start          = 0;
	uint64_t m_frames         = 0;
	uint64_t m_presents       = 0;
	uint64_t m_total_frames   = 0;
	uint64_t m_total_presents = 0;
	double   m_frame_rate     = 0.0;
	double   m_present_rate   = 0.0;
	bool     m_initialized    = false;
};

} // namespace Libs::Graphics

#endif
