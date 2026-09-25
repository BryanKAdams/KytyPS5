// Exercise sampler controls and rendering without a host audio device.
#include "libs/ngs2.cpp"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <numbers>

using namespace Libs::Audio::Ngs2;

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "Ngs2SamplerTests: %s\n", message);
		std::abort();
	}
}

struct Fixture {
	Ngs2Internal      system;
	Ngs2RackInternal  rack {};
	Ngs2VoiceInternal voice;

	Fixture(uint32_t rate, uint32_t channels = 1, Ngs2RackType type = Ngs2RackType::Sampler) {
		system.option.sample_rate = 48000;
		rack.ngs                  = &system;
		rack.type                 = type;
		voice.rack                = &rack;
		struct Setup {
			Ngs2VoiceParamHeader header {40, 0, 0x10000000};
			Ngs2WaveformFormat   format;
			uint32_t             flags = 0, reserved = 0;
		} setup;
		setup.header.id = type == Ngs2RackType::Sampler ? 0x10000000 : 0x40010000;
		setup.format    = {0x12, channels, rate};
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
		      "sampler setup failed");
		voice.state = Ngs2VoicePlayState::Playing;
	}

	void Queue(const int16_t* data, uint32_t frames, uint32_t flags, uint32_t repeats = 0,
	           uint32_t skip = 0) {
		Ngs2WaveformBlock block {
		    0,  size_t(frames + skip) * voice.channels * sizeof(int16_t), repeats, skip, frames, 0,
		    123};
		struct Blocks {
			Ngs2VoiceParamHeader     header {32, 0, 0x10000001};
			const void*              data;
			uint32_t                 flags, count;
			const Ngs2WaveformBlock* blocks;
		} param {{32, 0, 0x10000001}, data, flags, 1, &block};
		param.header.id = rack.type == Ngs2RackType::Sampler ? 0x10000001 : 0x40010001;
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &param.header) == OK,
		      "sampler queue failed");
	}

	void Render(uint32_t frames) {
		voice.rendered = false;
		Ngs2RenderVoice(voice, {}, frames);
	}
};

void TestStreamingResample() {
	Fixture              f(44100, 2);
	std::vector<int16_t> pcm(441 * 2);
	for (int i = 0; i < 441; ++i) {
		pcm[i * 2]     = static_cast<int16_t>(i * 32);
		pcm[i * 2 + 1] = static_cast<int16_t>(-i * 32);
	}
	f.Queue(pcm.data(), 200, 1);
	f.Queue(pcm.data() + 400, 241, 0);
	for (uint32_t grain = 0; grain < 3; ++grain) {
		f.Render(160);
		for (uint32_t i = 0; i < 160; ++i) {
			const float expected = std::min((grain * 160 + i) * 44100.0 / 48000.0, 440.0) / 1024.0;
			Check(std::abs(f.voice.samples[i] - expected) < 0.00001f,
			      "resampling discontinuity across grain or block boundary");
			Check(std::abs(f.voice.samples[160 + i] + expected) < 0.00001f,
			      "stereo channels were mixed");
		}
	}
	Check(f.voice.blocks.empty(), "final streaming block did not finish");
	Check(f.voice.state == Ngs2VoicePlayState::Empty, "finished voice remained active");
	Ngs2SamplerVoiceState state {};
	Ngs2VoiceGetState(reinterpret_cast<uintptr_t>(&f.voice), &state.voice_state, sizeof(state));
	Check(state.num_decoded_samples == 441 && state.decoded_data_size == pcm.size() * 2,
	      "streaming progress does not match consumed source data");
}

void TestPitchLoopAndSkip() {
	Fixture       f(24000);
	const int16_t pcm[] = {-30000, 8192, 16384};
	f.Queue(pcm, 2, 0, 1, 1);
	struct Pitch {
		Ngs2VoiceParamHeader header {16, 0, 0x10000005};
		float                ratio    = 2.0f;
		uint32_t             reserved = 0;
	} pitch;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &pitch.header);
	f.Render(5);
	const float expected[] = {0.25f, 0.5f, 0.25f, 0.5f, 0.0f};
	for (size_t i = 0; i < 5; ++i) {
		Check(std::abs(f.voice.samples[i] - expected[i]) < 0.00001f,
		      "pitch, repeat count, or skipped samples incorrect");
	}
}

void TestStarvationAndPause() {
	Fixture       f(48000);
	const int16_t pcm[] = {16384, 8192};
	f.Queue(pcm, 2, 1);
	f.Render(4);
	Check(f.voice.state == Ngs2VoicePlayState::Playing, "stream starvation stopped voice");
	Check(f.voice.samples[2] == 0.0f, "starved output was not silent");
	f.Queue(pcm, 2, 0);
	f.voice.SetEvent(16);
	f.Render(2);
	Check(!f.voice.has_samples && f.voice.blocks.front().cursor == 0, "paused voice consumed data");
	f.voice.SetEvent(32);
	f.Render(2);
	Check(f.voice.samples[0] == 0.5f && f.voice.samples[1] == 0.25f,
	      "stream failed to resume after starvation/pause");
}

void TestMonoRateAndRouting() {
	Fixture              f(22050);
	std::vector<int16_t> pcm(441, 16384);
	f.Queue(pcm.data(), 441, 0);
	Ngs2RackInternal mastering_rack {};
	mastering_rack.type = Ngs2RackType::Mastering;
	Ngs2VoiceInternal mastering;
	mastering.rack     = &mastering_rack;
	mastering.channels = 2;
	mastering.state    = Ngs2VoicePlayState::Playing;
	f.voice.matrices   = {{0.5f, 0.25f}};
	f.voice.ports.push_back({&mastering, 0, 1.0f, 0});
	for (int i = 0; i < 3; ++i) {
		mastering.rendered = false;
		f.voice.rendered   = false;
		Ngs2RenderVoice(mastering, {&f.voice, &mastering}, 320);
		Check(mastering.has_samples, "standard sampler did not reach mastering voice");
		for (uint32_t j = 0; j < 320; ++j) {
			Check(mastering.samples[j] == 0.25f && mastering.samples[320 + j] == 0.125f,
			      "mono PCM routing matrix was not applied");
		}
	}
	Check(f.voice.decoded_samples == 441 && f.voice.blocks.empty(),
	      "22.05 kHz effect duration was incorrect");
}

void TestCustomPcmStillPlays() {
	Fixture       f(48000, 1, Ngs2RackType::CustomSampler);
	const int16_t pcm[] = {8192, -16384};
	f.Queue(pcm, 2, 0);
	f.Render(4);
	Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == -0.5f &&
	          f.voice.samples[2] == 0.0f && f.voice.blocks.empty(),
	      "custom PCM sampler regressed");
}

void TestSamplerReuseResetsRouting() {
	for (auto type: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		Fixture          f(48000, 1, type);
		Ngs2RackInternal mastering_rack {};
		mastering_rack.type = Ngs2RackType::Mastering;
		Ngs2VoiceInternal mastering;
		mastering.rack     = &mastering_rack;
		mastering.channels = 2;
		mastering.state    = Ngs2VoicePlayState::Playing;
		f.voice.ports.push_back({&mastering, 1, 0.25f, 0});
		f.voice.matrices = {{0.5f, 0.25f}};
		f.voice.SetupSampler({0x12, 2, 48000});
		Check(f.voice.ports.size() == 1 && f.voice.ports[0].dest == nullptr &&
		          f.voice.ports[0].input == 0 && f.voice.ports[0].volume == 1.0f &&
		          f.voice.ports[0].matrix == -1 && f.voice.matrices.size() == 1 &&
		          f.voice.matrices[0].empty(),
		      "sampler setup retained routing from the previous channel layout");
		const int16_t pcm[] = {8192, 16384, -8192, -16384};
		f.Queue(pcm, 2, 0);
		f.Render(1);
		Check(!f.voice.has_samples && f.voice.blocks.front().cursor == 0,
		      "sampler setup started the new waveform without a play event");
		f.voice.ports[0].dest = &mastering;
		f.voice.SetEvent(1);
		f.voice.rendered = false;
		Ngs2RenderVoice(mastering, {&f.voice, &mastering}, 2);
		Check(mastering.samples == std::vector<float> {0.25f, -0.25f, 0.5f, -0.5f},
		      "reused stereo sampler did not restore default channel routing");
	}
}

void TestPlayStateBeforeRender() {
	Fixture       f(48000);
	const int16_t pcm[] = {8192, -16384};
	f.voice.state       = Ngs2VoicePlayState::Empty;
	f.Queue(pcm, 2, 0);
	Ngs2VoiceEventParam event {{sizeof(Ngs2VoiceEventParam), 0, 6}, 1};
	const auto handle = reinterpret_cast<uintptr_t>(&f.voice);
	Ngs2VoiceControl(handle, &event.header);
	uint32_t flags = 0;
	Ngs2VoiceGetStateFlags(handle, &flags);
	Check(flags == 1, "play must reserve the voice before rendering starts playback");
	Ngs2SamplerVoiceState state {};
	Ngs2VoiceGetState(handle, &state.voice_state, sizeof(state));
	Check(state.voice_state.state_flags == flags && state.num_decoded_samples == 0,
	      "state queries disagreed or play consumed samples before render");
	f.Render(4);
	Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == -0.5f,
	      "one-shot effect was dropped before its first render");
}

void TestStatePublication() {
	Ngs2Internal system;
	system.option = Ngs2DefaultSystemOption();
	Ngs2RackOptionUnion option {};
	Ngs2FillDefaultRackOption(0x1000, &option);
	option.common.max_voices = 1;
	alignas(Ngs2RackInternal) alignas(Ngs2VoiceInternal)
	    std::byte         storage[sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal)];
	Ngs2ContextBufferInfo buffer {storage, sizeof(storage)};
	uintptr_t             rack_handle   = 0;
	const auto            system_handle = reinterpret_cast<uintptr_t>(&system);
	Check(Ngs2RackCreate(system_handle, 0x1000, &option.common, &buffer, &rack_handle) == OK,
	      "state publication rack creation failed");
	auto& voice =
	    *reinterpret_cast<Ngs2VoiceInternal*>(reinterpret_cast<Ngs2RackInternal*>(rack_handle) + 1);
	voice.SetupSampler({0x12, 1, 48000});
	const auto handle      = reinterpret_cast<uintptr_t>(&voice);
	auto       check_flags = [&](uint32_t expected) {
		uint32_t              flags = 0;
		Ngs2SamplerVoiceState state {};
		Ngs2VoiceGetStateFlags(handle, &flags);
		Ngs2VoiceGetState(handle, &state.voice_state, sizeof(state));
		Check(flags == expected && state.voice_state.state_flags == expected,
		      "voice state was published at the wrong render boundary");
	};
	struct Transition {
		uint32_t event, before_render, after_render;
	};
	const Transition     transitions[] = {{1, 1, 3}, {16, 3, 5}, {32, 5, 3}, {2, 3, 0},
	                                      {1, 1, 3}, {16, 3, 5}, {2, 5, 0},  {1, 1, 3},
	                                      {4, 3, 0}, {1, 1, 3},  {8, 3, 0}};
	Ngs2RenderBufferInfo output {};
	for (const auto& transition: transitions) {
		Ngs2VoiceEventParam event {{sizeof(Ngs2VoiceEventParam), 0, 6}, transition.event};
		Ngs2VoiceControl(handle, &event.header);
		check_flags(transition.before_render);
		Ngs2SystemRender(system_handle, &output, 1);
		check_flags(transition.after_render);
	}
	Check(Ngs2RackDestroy(rack_handle, nullptr) == OK, "state publication rack cleanup failed");
}

void TestStopThenPlayBeforeRender() {
	Fixture       f(48000);
	const int16_t pcm[] = {16384, -8192};
	f.Queue(pcm, 2, 0);
	f.voice.SetEvent(2);
	f.voice.SetEvent(1);
	Check(Ngs2GetStateFlags(&f.voice) == 1, "stopped voice could not restart");
	f.voice.SetEvent(8);
	f.voice.SetEvent(1);
	Check(Ngs2GetStateFlags(&f.voice) == 1, "kill/play commands lost their ordering");
	f.Render(2);
	Check(f.voice.samples[0] == 0.5f && f.voice.samples[1] == -0.25f,
	      "restarted voice produced no audio");
}

void SetLowPass(Fixture& f, float frequency = 1000.0f, uint64_t mask = 0) {
	struct Param {
		Ngs2VoiceParamHeader   header {56, 0, 0x1000000a};
		Ngs2SamplerFilterParam filter {0, 1, 0, 1, 1000.0f, 0.70710678f, 1.0f, {}};
	} param;
	param.filter.frequency            = frequency;
	param.filter.channel_mask         = mask;
	f.rack.option.sampler.max_filters = 8;
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &param.header) == OK,
	      "filter ABI command failed");
}

void TestLowPassResponse() {
	// A second-order Butterworth LPF has unity DC gain and -3 dB at cutoff.
	for (double frequency: {100.0, 1000.0, 10000.0}) {
		Fixture f(48000);
		SetLowPass(f);
		std::vector<int16_t> pcm(48000);
		for (size_t i = 0; i < pcm.size(); ++i) {
			pcm[i] =
			    static_cast<int16_t>(8192 * std::sin((2.0 * std::numbers::pi) * frequency * i / 48000));
		}
		f.Queue(pcm.data(), static_cast<uint32_t>(pcm.size()), 0);
		f.Render(static_cast<uint32_t>(pcm.size()));
		double input = 0, output = 0;
		for (size_t i = 24000; i < pcm.size(); ++i) {
			input += std::pow(pcm[i] / 32768.0, 2);
			output += std::pow(f.voice.samples[i], 2);
		}
		const double ratio = std::sqrt(output / input);
		if (frequency == 100) {
			Check(std::abs(ratio - 1.0) < 0.002, "low-pass lost bass");
		}
		if (frequency == 1000) {
			Check(std::abs(ratio - std::sqrt(0.5)) < 0.002, "cutoff gain incorrect");
		}
		if (frequency == 10000) {
			Check(ratio < 0.01, "low-pass did not reject high frequencies");
		}
	}
}

void TestFilterHistoryAndChannels() {
	Fixture whole(48000, 2), split(48000, 2);
	SetLowPass(whole, 1000, 1);
	SetLowPass(split, 1000, 1);
	std::vector<int16_t> pcm(256 * 2, 0);
	pcm[0] = pcm[1] = 16384;
	whole.Queue(pcm.data(), 256, 0);
	split.Queue(pcm.data(), 32, 1);
	split.Queue(pcm.data() + 64, 224, 0);
	whole.Render(256);
	for (uint32_t g = 0; g < 8; ++g) {
		// Games resend unchanged parameters; this must not reset the delay line.
		SetLowPass(split, 1000, 1);
		split.Render(32);
		for (uint32_t c = 0; c < 2; ++c) {
			for (uint32_t i = 0; i < 32; ++i) {
				Check(std::abs(split.voice.samples[c * 32 + i] -
				               whole.voice.samples[c * 256 + g * 32 + i]) < 1e-7,
				      "filter history changed at a grain/block/control boundary");
			}
		}
	}
	Check(whole.voice.samples[0] == 0.5f && whole.voice.samples[1] == 0.0f &&
	          whole.voice.samples[256] > 0.0f && whole.voice.samples[256] < 0.5f,
	      "channel mask must bypass selected channels and filter the others");
	Fixture stereo(48000, 2);
	SetLowPass(stereo);
	pcm[1] = 0;
	stereo.Queue(pcm.data(), 256, 0);
	stereo.Render(256);
	for (size_t i = 256; i < 512; ++i) {
		Check(stereo.voice.samples[i] == 0.0f, "filter leaked history between channels");
	}
	stereo.voice.SetupSampler({0x12, 2, 48000});
	Check(stereo.voice.filters.empty(), "reused voice retained old filters");
}

void TestFilterUsesOutputRate() {
	Fixture f(22050);
	SetLowPass(f, 6833.0f);
	const auto& filter = f.voice.filters[0];
	// Rear filtering operates at the 48 kHz system rate, not the PCM source rate.
	const double w        = (2.0 * std::numbers::pi) * 6833 / 48000;
	const double expected = (1 - std::cos(w)) / (2 * (1 + std::sin(w) / (2 * double(0.70710678f))));
	Check(std::abs(filter.b0 - expected) < 1e-10, "rear filter used source sample rate");
	SetLowPass(f, 48000);
	const int16_t pcm[] = {16384, 16384};
	f.Queue(pcm, 2, 0);
	f.Render(2);
	Check(f.voice.samples[0] == 0.5f && f.voice.samples[1] == 0.5f,
	      "above-Nyquist cutoff was unstable");
}

void TestFilterControlLayout() {
	Fixture f(48000, 2);
	f.rack.option.sampler.max_filters = 8;
	// A 56-byte control command with nonzero trailing padding.
	alignas(8) uint32_t words[] = {56,         0x1000000a, 0,          1, 0, 0, 1,
	                               0x45d58800, 0x3fd55555, 0x3f800000, 0, 0, 0, 0x13f};
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice),
	                 reinterpret_cast<const Ngs2VoiceParamHeader*>(words));
	Check(f.voice.filters.size() == 1 && f.voice.filters[0].enabled,
	      "FCQ filter command was not recognized");
	const double omega    = (2.0 * std::numbers::pi) * 6833.0 / 48000.0;
	const double q        = static_cast<double>(std::bit_cast<float>(0x3fd55555u));
	const double expected = (1 - std::cos(omega)) / (2 * (1 + std::sin(omega) / (2 * q)));
	Check(std::abs(f.voice.filters[0].b0 - expected) < 1e-10,
	      "FCQ fields were read at the wrong offsets");
	words[4] = 1; // Bypass channel 0 in the low half of the 64-bit mask.
	words[5] = 1; // Bypass channel 32 in the high half.
	Fixture wide(48000, 33);
	wide.rack.option.sampler.max_filters = 8;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&wide.voice),
	                 reinterpret_cast<const Ngs2VoiceParamHeader*>(words));
	const std::vector<int16_t> pcm(33, 16384);
	wide.Queue(pcm.data(), 1, 1);
	wide.Render(1);
	Check(wide.voice.samples[0] == 0.5f && wide.voice.samples[32] == 0.5f &&
	          std::abs(wide.voice.samples[1] - expected * 0.5) < 1e-7,
	      "channel mask was not decoded as a 64-bit bypass mask");
	words[6] = 0; // OFF ignores unused FCQ values and clears the old delay line.
	words[7] = 0x7fc00000;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&wide.voice),
	                 reinterpret_cast<const Ngs2VoiceParamHeader*>(words));
	Check(!wide.voice.filters[0].enabled && wide.voice.filters[0].history.empty() &&
	          !wide.voice.filters[0].warned,
	      "filter OFF was treated as an unsupported FCQ configuration");
	wide.Queue(pcm.data(), 1, 1);
	wide.Render(1);
	Check(wide.voice.samples[1] == 0.5f, "filter OFF did not bypass audio");
	SetLowPass(wide, 0);
	wide.Queue(pcm.data(), 1, 1);
	wide.Render(1);
	Check(wide.voice.samples[1] == 0.0f, "zero-Hz low-pass leaked source audio");
}

struct LoopCallbacks {
	std::vector<std::pair<uint32_t, uint32_t>> events;
	bool                                       pause = false;
};

void KYTY_SYSV_ABI RecordLoopCallback(const Ngs2VoiceCallbackInfo* info) {
	auto& log = *reinterpret_cast<LoopCallbacks*>(info->data);
	log.events.emplace_back(info->flag, info->repeats);
	Check(info->user == 123 && info->attributes == 0, "loop callback metadata changed");
	if (log.pause && info->flag == 2) {
		reinterpret_cast<Ngs2VoiceInternal*>(info->voice)->SetEvent(16);
	}
}

void TestLoopCallbacksAndExit() {
	const int16_t pcm[] = {8192, 16384, 24576};
	Fixture       f(48000);
	LoopCallbacks log;
	f.voice.callback       = reinterpret_cast<uintptr_t>(&RecordLoopCallback);
	f.voice.callback_data  = reinterpret_cast<uintptr_t>(&log);
	f.voice.callback_flags = 3;
	f.Queue(pcm, 2, 0, 2);
	f.Render(6);
	Check(log.events == std::vector<std::pair<uint32_t, uint32_t>> {{2, 1}, {2, 2}, {1, 2}},
	      "loop and completion callbacks lost the completed repeat count");
	Check(f.voice.blocks.empty(), "finite loop did not complete");

	Fixture active(48000);
	log.events.clear();
	log.pause                   = true;
	active.voice.callback       = reinterpret_cast<uintptr_t>(&RecordLoopCallback);
	active.voice.callback_data  = reinterpret_cast<uintptr_t>(&log);
	active.voice.callback_flags = 2;
	active.Queue(pcm, 2, 1, UINT32_MAX);
	active.Queue(pcm + 2, 1, 0);
	active.Render(6);
	Check(log.events.size() == 1 && active.voice.decoded_samples == 2 &&
	          active.voice.blocks.front().cursor == 0 && active.voice.samples[2] == 0,
	      "loop callback pause consumed another iteration");
	Ngs2VoiceParamHeader exit_loop {sizeof(Ngs2VoiceParamHeader), 0, 0x10000004};
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&active.voice), &exit_loop);
	active.voice.SetEvent(32);
	active.Render(3);
	Check(active.voice.samples == std::vector<float> {0.25f, 0.5f, 0.75f} &&
	          active.voice.blocks.empty() && log.events.size() == 1,
	      "exiting infinite loop lost its final iteration or ignored callback flags");

	Fixture pending(48000);
	pending.Queue(pcm, 1, 1);
	pending.Queue(pcm + 1, 1, 1, UINT32_MAX);
	pending.Queue(pcm + 2, 1, 0);
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&pending.voice), &exit_loop);
	pending.Render(3);
	Check(pending.voice.samples == std::vector<float> {0.25f, 0.5f, 0.75f} &&
	          pending.voice.blocks.empty(),
	      "exiting a pending loop skipped its first iteration");
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&pending.voice), &exit_loop);
}

void TestWaveformReadAddress() {
	Fixture       f(48000, 2);
	const int16_t pcm[] = {-100, -100, 8192, 4096, 16384, 8192};
	f.Queue(pcm, 2, 1, 1, 1);
	const auto next_data = [&]() {
		Ngs2SamplerVoiceState state {};
		Ngs2VoiceGetState(reinterpret_cast<uintptr_t>(&f.voice), &state.voice_state, sizeof(state));
		return state.waveform_data;
	};
	Check(next_data() == pcm + 2, "read address ignored skipped source samples");
	f.Render(1);
	Check(next_data() == pcm + 4, "read address did not advance by a stereo frame");
	f.Render(1);
	Check(next_data() == pcm + 2, "read address did not wrap at a loop boundary");
	f.Render(2);
	Check(f.voice.blocks.empty() && next_data() == pcm + 6,
	      "starved read address did not preserve the consumed waveform end");
	f.Queue(pcm + 2, 2, 0);
	Check(next_data() == pcm + 2, "refilled read address retained the previous waveform end");
	f.Render(2);
	Check(next_data() == pcm + 6, "completed read address did not preserve waveform end");
	f.voice.SetupSampler({0x12, 2, 48000});
	Check(next_data() == nullptr, "setup retained an old waveform address");
}

struct CallbackEvent {
	Ngs2VoiceInternal* voice;
	uint32_t           event;
	uint32_t           calls = 0;
};

void KYTY_SYSV_ABI ApplyCallbackEvent(const Ngs2VoiceCallbackInfo* info) {
	auto& context = *reinterpret_cast<CallbackEvent*>(info->data);
	if (++context.calls == 1) {
		context.voice->SetEvent(context.event);
	}
}

void TestCallbackStopsConsumption() {
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		for (uint32_t event: {2u, 4u, 8u, 16u}) {
			Fixture       f(48000, 1, rack);
			const int16_t pcm[] = {8192, 16384};
			f.Queue(pcm, 1, 1);
			f.Queue(pcm + 1, 1, 1);
			CallbackEvent context {&f.voice, event};
			f.voice.callback       = reinterpret_cast<uintptr_t>(&ApplyCallbackEvent);
			f.voice.callback_data  = reinterpret_cast<uintptr_t>(&context);
			f.voice.callback_flags = 1;
			f.Render(2);
			Check(f.voice.state != Ngs2VoicePlayState::Playing && context.calls == 1,
			      "completion callback failed to interrupt playback");
			Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == 0.0f &&
			          f.voice.decoded_samples == 1 && f.voice.blocks.size() == 1 &&
			          f.voice.blocks.front().cursor == 0,
			      "callback pause/stop consumed the next queued block");
			if (event == 16) {
				f.voice.SetEvent(32);
				f.Render(1);
				Check(f.voice.samples[0] == 0.5f && f.voice.decoded_samples == 2,
				      "callback pause failed to preserve queued audio for resume");
			}
		}
	}
}

void TestCallbackPauseDuringLargeStep() {
	Fixture       f(48000);
	const int16_t pcm[] = {8192, 16384, 24576, 4096};
	f.Queue(pcm, 1, 1);
	f.Queue(pcm + 1, 1, 1);
	f.Queue(pcm + 2, 2, 1);
	f.voice.sample_step = uint64_t(120000) << 32u;
	CallbackEvent context {&f.voice, 16};
	f.voice.callback       = reinterpret_cast<uintptr_t>(&ApplyCallbackEvent);
	f.voice.callback_data  = reinterpret_cast<uintptr_t>(&context);
	f.voice.callback_flags = 1;
	f.Render(2);
	Check(context.calls == 1 && f.voice.decoded_samples == 1 &&
	          f.voice.sample_phase == (uint64_t(72000) << 32u),
	      "pause advanced across additional blocks in a large resampling step");
	f.voice.SetEvent(32);
	f.Render(1);
	Check(std::abs(f.voice.samples[0] - 0.4375f) < 1e-7f && context.calls == 3,
	      "resume lost fractional position or read past the next short block");
}

void TestUpstreamCustomSamplerControls() {
	Fixture f(24000, 1, Ngs2RackType::CustomSampler);
	const int16_t pcm[] = {8192, 16384, 24576, 4096};
	f.Queue(pcm, 4, 1);
	f.Render(1);
	Check(f.voice.samples[0] == 0.25f, "custom sampler source rate was rejected");
	f.Queue(pcm + 2, 2, 4);
	f.Render(1);
	Check(f.voice.samples[0] == 0.75f && f.voice.blocks.size() == 1,
	      "replacement blocks retained old data or fractional position");
	struct Pitch {
		Ngs2VoiceParamHeader header {16, 0, 0x40010005};
		float                ratio = 0.0f;
		uint32_t             reserved = 0;
	} pitch;
	const auto handle = reinterpret_cast<uintptr_t>(&f.voice);
	Ngs2VoiceControl(handle, &pitch.header);
	f.Render(2);
	Check(f.voice.samples[0] == 0.4375f && f.voice.samples[1] == 0.4375f,
	      "zero custom sampler pitch advanced playback");
	pitch.ratio = 4.0f;
	Ngs2VoiceControl(handle, &pitch.header);
	f.Render(1);
	Check(f.voice.blocks.empty(), "custom sampler pitch did not advance playback");
	f.rack.option.custom_sampler.custom_rack_option.state_size =
	    sizeof(Ngs2CustomSamplerVoiceState);
	Ngs2CustomSamplerVoiceState state {};
	Check(Ngs2VoiceGetState(handle, &state.voice_state, sizeof(state)) == OK &&
	          state.voice_state.state_flags == Ngs2GetStateFlags(&f.voice),
	      "custom sampler state layout regressed");
}

void TestFilterTailDuringStarvation() {
	Fixture reference(48000), stream(48000);
	SetLowPass(reference);
	SetLowPass(stream);
	std::vector<int16_t> pcm(128, 0);
	pcm[0] = pcm[64] = 16384;
	reference.Queue(pcm.data(), 128, 1);
	reference.Render(128);
	stream.Queue(pcm.data(), 1, 1);
	stream.Render(1);
	Check(stream.voice.samples[0] == reference.voice.samples[0], "initial filter sample differs");
	stream.Render(63);
	Check(stream.voice.has_samples, "starved filter tail was not available for routing");
	for (size_t i = 0; i < 63; ++i) {
		Check(std::abs(stream.voice.samples[i] - reference.voice.samples[i + 1]) < 1e-7f,
		      "filter tail changed at a starvation/grain boundary");
	}
	stream.Queue(pcm.data() + 64, 64, 1);
	stream.Render(64);
	for (size_t i = 0; i < 64; ++i) {
		Check(std::abs(stream.voice.samples[i] - reference.voice.samples[i + 64]) < 1e-7f,
		      "refilled stream reused stale filter history");
	}
	stream.Render(8192);
	stream.Render(32);
	Check(!stream.voice.has_samples && !stream.voice.filters[0].HasHistory(),
	      "fully decayed filter kept a silent stream active");
}

void TestResamplingPhaseAfterStarvation() {
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		for (uint32_t rate: {44100u, 96000u}) {
			Fixture        reference(rate, 1, rack), stream(rate, 1, rack);
			const int16_t  pcm[]  = {0, 0, 16384, 8192};
			const uint32_t frames = rate == 44100 ? 2 : 1;
			reference.Queue(pcm, 4, 0);
			stream.Queue(pcm, 1, 1);
			reference.Render(frames);
			stream.Render(frames);
			Check(stream.voice.blocks.empty() && stream.voice.state == Ngs2VoicePlayState::Playing,
			      "open stream did not enter starvation");
			stream.Render(8);
			Check(!stream.voice.has_samples, "starved stream produced audio without a filter");
			stream.Queue(pcm + 1, 3, 0);
			reference.Render(1);
			stream.Render(1);
			Check(std::abs(stream.voice.samples[0] - reference.voice.samples[0]) < 1e-7f,
			      "stream refill lost resampling phase");
			Check(stream.voice.decoded_samples == reference.voice.decoded_samples,
			      "stream refill lost pending source advancement");
			stream.voice.SetupSampler({0x12, 1, rate});
			Check(stream.voice.sample_phase == 0, "sampler setup retained old resampling phase");
		}
	}
}

void TestFiniteFilterTail() {
	Fixture whole(48000), split(48000);
	SetLowPass(whole);
	SetLowPass(split);
	const int16_t impulse = 16384;
	whole.Queue(&impulse, 1, 0);
	split.Queue(&impulse, 1, 0);
	whole.Render(256);
	for (size_t i = 0; i < whole.voice.samples.size(); ++i) {
		split.Render(1);
		Check(std::abs(split.voice.samples[0] - whole.voice.samples[i]) < 1e-7f,
		      "finite filter tail changed with render grain size");
	}
	Check(split.voice.state == Ngs2VoicePlayState::Playing,
	      "finite voice stopped before its filter tail decayed");
	split.Render(8192);
	Check(split.voice.state == Ngs2VoicePlayState::Empty,
	      "finite voice remained active after its filter tail decayed");
	split.Render(32);
	Check(!split.voice.has_samples, "completed finite filter tail still produced samples");
}

} // namespace

int main() {
	TestResamplingPhaseAfterStarvation();
	TestSamplerReuseResetsRouting();
	TestLoopCallbacksAndExit();
	TestWaveformReadAddress();
	TestFilterTailDuringStarvation();
	TestFiniteFilterTail();
	TestLowPassResponse();
	TestFilterHistoryAndChannels();
	TestFilterUsesOutputRate();
	TestFilterControlLayout();
	TestUpstreamCustomSamplerControls();
	TestCallbackStopsConsumption();
	TestCallbackPauseDuringLargeStep();
	TestStreamingResample();
	TestPitchLoopAndSkip();
	TestStarvationAndPause();
	TestMonoRateAndRouting();
	TestCustomPcmStillPlays();
	TestPlayStateBeforeRender();
	TestStatePublication();
	TestStopThenPlayBeforeRender();
	std::puts("Ngs2SamplerTests: all cases passed");
}
