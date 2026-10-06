#include "audiostream.h"
#include "cfg/option.h"
#include "emulator.h"

#include <algorithm>

static void registerForEvents();

struct SoundFrame { s16 l; s16 r; };

static SoundFrame Buffer[SAMPLE_COUNT];
static u32 writePtr;  // next sample index

static AudioBackend *currentBackend;
std::vector<AudioBackend *> *AudioBackend::backends;

static bool audio_recording_started;
static bool eight_khz;

AudioBackend *AudioBackend::getBackend(const std::string& slug)
{
	if (backends == nullptr)
		return nullptr;
	if (slug == "auto")
	{
		// Prefer sdl2 if available and avoid the null driver
		AudioBackend *sdlBackend = nullptr;
		AudioBackend *autoBackend = nullptr;
		for (auto backend : *backends)
		{
			if (backend->slug == "sdl2")
				sdlBackend = backend;
			if (backend->slug != "null" && autoBackend == nullptr)
				autoBackend = backend;
		}
		if (sdlBackend != nullptr)
			autoBackend = sdlBackend;
		if (autoBackend == nullptr)
			autoBackend = backends->front();
		INFO_LOG(AUDIO, "Auto-selected audio backend \"%s\" (%s).", autoBackend->slug.c_str(), autoBackend->getName().c_str());

		return autoBackend;
	}
	for (auto backend : *backends)
	{
		if (backend->slug == slug)
			return backend;
	}
	WARN_LOG(AUDIO, "WARNING: Audio backend \"%s\" not found!", slug.c_str());
	return nullptr;
}

// Audio rate control, for replay. It runs without audio sync, as the online
// battle does, so the emulator's sample rate drifts from the device's 44.1kHz
// with the playback pacing, and the queue overflows or runs dry - either one
// clicks. Resampling by up to 0.5% to hold the queue near half full absorbs
// that without an audible pitch change (RetroArch's dynamic rate control).
namespace {
constexpr double kRateMaxCorrection = 0.005;
// Smoothing per push (~12ms). The SDL2 device takes 1024 frames per callback,
// so the queue level is a sawtooth of that size.
constexpr double kQueueAlpha = 0.05;

struct RateControl
{
	bool active = false;
	SoundFrame prev {};
	double phase = 0.0;	// position of the next output frame after prev, in input frames
	double step = 1.0;	// input frames per output frame
	double queueLevel = 0.5;
};
RateControl rateControl;
}

static void updateRate()
{
	RateControl& rc = rateControl;
	u32 queued, capacity;
	if (currentBackend == nullptr || !currentBackend->getQueueLevel(queued, capacity) || capacity == 0)
		return;
	rc.queueLevel += kQueueAlpha * (std::min<double>(1.0, (double)queued / capacity) - rc.queueLevel);
	rc.step = 1.0 / (1.0 + kRateMaxCorrection * (1.0 - 2.0 * rc.queueLevel));
}

static void pushFrame(const SoundFrame& frame)
{
	Buffer[writePtr] = frame;
	if (++writePtr == SAMPLE_COUNT)
	{
		if (currentBackend != nullptr)
			currentBackend->push(Buffer, SAMPLE_COUNT, config::LimitFPS);
		writePtr = 0;
		if (rateControl.active)
			updateRate();
	}
}

void WriteSample(s16 r, s16 l)
{
	float vol = config::AudioVolume.dbPower() * settings.aica.audioFade;
	if (0.f < settings.gdxsv.audioScale && settings.gdxsv.audioScale < 1.f)
		vol *= settings.gdxsv.audioScale;
	SoundFrame frame;
	frame.r = r * vol;
	frame.l = l * vol;

	RateControl& rc = rateControl;
	if (rc.active != settings.gdxsv.audioRateControl) {
		rc = {};
		rc.active = settings.gdxsv.audioRateControl;
		rc.prev = frame;
	}
	if (!rc.active) {
		pushFrame(frame);
		return;
	}

	// Linear interpolation between the previous input frame and this one.
	while (rc.phase < 1.0)
	{
		const float t = (float)rc.phase;
		SoundFrame out;
		out.l = (s16)(rc.prev.l + (frame.l - rc.prev.l) * t);
		out.r = (s16)(rc.prev.r + (frame.r - rc.prev.r) * t);
		pushFrame(out);
		rc.phase += rc.step;
	}
	rc.phase -= 1.0;
	rc.prev = frame;
}

void InitAudio()
{
	registerForEvents();
	TermAudio();

	std::string slug = config::AudioBackend;
	currentBackend = AudioBackend::getBackend(slug);
	if (currentBackend == nullptr && slug != "auto")
	{
		slug = "auto";
		currentBackend = AudioBackend::getBackend(slug);
	}
	if (currentBackend != nullptr)
	{
		INFO_LOG(AUDIO, "Initializing audio backend \"%s\" (%s)...", currentBackend->slug.c_str(), currentBackend->getName().c_str());
		if (!currentBackend->init())
		{
			currentBackend = nullptr;
			if (slug != "auto")
			{
				WARN_LOG(AUDIO, "Audio driver %s failed to initialize. Defaulting to 'auto'", slug.c_str());
				slug = "auto";
				currentBackend = AudioBackend::getBackend(slug);
				if (!currentBackend->init())
					currentBackend = nullptr;
			}
		}
	}

	if (currentBackend == nullptr)
	{
		WARN_LOG(AUDIO, "Running without audio!");
		return;
	}

	if (audio_recording_started)
	{
		// Restart recording
		audio_recording_started = false;
		StartAudioRecording(eight_khz);
	}
}

void TermAudio()
{
	if (currentBackend == nullptr)
		return;

	// Save recording state before stopping
	bool rec_started = audio_recording_started;
	StopAudioRecording();
	audio_recording_started = rec_started;
	currentBackend->term();
	INFO_LOG(AUDIO, "Terminating audio backend \"%s\" (%s)...", currentBackend->slug.c_str(), currentBackend->getName().c_str());
	currentBackend = nullptr;
}

void FlushAudio()
{
	writePtr = 0;
}

void StartAudioRecording(bool eight_khz)
{
	::eight_khz = eight_khz;
	if (currentBackend != nullptr)
		audio_recording_started = currentBackend->initRecord(eight_khz ? 8000 : 11025);
	else
		// might be called between TermAudio/InitAudio
		audio_recording_started = true;
}

u32 RecordAudio(void *buffer, u32 samples)
{
	if (!audio_recording_started || currentBackend == nullptr)
		return 0;
	return currentBackend->record(buffer, samples);
}

void StopAudioRecording()
{
	// might be called between TermAudio/InitAudio
	if (audio_recording_started && currentBackend != nullptr)
		currentBackend->termRecord();
	audio_recording_started = false;
}

static void registerForEvents()
{
	static bool done;
	if (done)
		return;
	done = true;
	// Empty the audio buffer when loading a state or terminating the game
	const auto& callback = [](Event, void *) {
		writePtr = 0;
	};
	EventManager::listen(Event::Terminate, callback);
	EventManager::listen(Event::LoadState, callback);
}


