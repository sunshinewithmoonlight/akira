#include "stream/audio_manager.hpp"
#include <borealis.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace {

constexpr unsigned int kPrefillMs = 30;
constexpr unsigned int kLowWatermarkMs = 20;
constexpr unsigned int kTargetWatermarkMs = 30;
constexpr unsigned int kHighWatermarkMs = 50;
constexpr unsigned int kCapacityMs = 60;
constexpr unsigned int kControlBlockMs = 10;

std::uint64_t steadySeconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

AudioManager::AudioManager()
{
}

AudioManager::~AudioManager()
{
    cleanup();
}

bool AudioManager::init(unsigned int channels, unsigned int rate)
{
    cleanup();

    if (channels == 0 || rate == 0 ||
        !m_ring.configure(
            rate,
            channels,
            kPrefillMs,
            kLowWatermarkMs,
            kTargetWatermarkMs,
            kHighWatermarkMs,
            kCapacityMs,
            kControlBlockMs))
    {
        m_open_errors.fetch_add(1, std::memory_order_relaxed);
        brls::Logger::error(
            "Invalid audio ring configuration: rate={}, channels={}",
            rate,
            channels);
        return false;
    }

    SDL_AudioSpec want;
    SDL_AudioSpec have;
    SDL_memset(&want, 0, sizeof(want));
    SDL_memset(&have, 0, sizeof(have));

    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = channels;
    want.samples = static_cast<Uint16>(
        std::max<std::size_t>(
            1,
            std::min<std::size_t>(
                static_cast<std::size_t>(rate) * kControlBlockMs / 1000,
                std::numeric_limits<Uint16>::max())));
    want.callback = &AudioManager::sdlAudioCallback;
    want.userdata = this;

    m_shutdown.store(false, std::memory_order_release);
    m_callback_active.store(false, std::memory_order_release);
    m_playback_started.store(false, std::memory_order_release);
    m_last_summary_second.store(
        steadySeconds(),
        std::memory_order_relaxed);
    m_ring.notePrefillStart();

    m_device_id = SDL_OpenAudioDevice(
        nullptr,
        0,
        &want,
        &have,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
            SDL_AUDIO_ALLOW_CHANNELS_CHANGE |
            SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (m_device_id == 0)
    {
        m_open_errors.fetch_add(1, std::memory_order_relaxed);
        brls::Logger::error(
            "SDL_OpenAudioDevice failed: {}",
            SDL_GetError());
        return false;
    }

    m_requested = want;
    m_obtained = have;
    if (have.freq != want.freq || have.format != want.format ||
        have.channels != want.channels)
    {
        m_open_errors.fetch_add(1, std::memory_order_relaxed);
        brls::Logger::error(
            "SDL_OpenAudioDevice changed PCM contract: requested "
            "freq={} channels={} format={} samples={}; obtained "
            "freq={} channels={} format={} samples={}",
            want.freq,
            want.channels,
            static_cast<int>(want.format),
            want.samples,
            have.freq,
            have.channels,
            static_cast<int>(have.format),
            have.samples);
        SDL_CloseAudioDevice(m_device_id);
        m_device_id = 0;
        return false;
    }

    brls::Logger::info(
        "Audio SDL open: requested_freq={} requested_channels={} "
        "requested_format={} requested_samples={} obtained_freq={} "
        "obtained_channels={} obtained_format={} obtained_samples={}",
        want.freq,
        want.channels,
        static_cast<int>(want.format),
        want.samples,
        have.freq,
        have.channels,
        static_cast<int>(have.format),
        have.samples);

    SDL_PauseAudioDevice(m_device_id, 0);
    return true;
}

void AudioManager::play(int16_t* buf, size_t samples_count)
{
    if (!isInitialized() || !buf || samples_count == 0)
    {
        return;
    }

    const auto channels = m_ring.channels();
    if (channels == 0 ||
        samples_count >
            std::numeric_limits<std::size_t>::max() / channels)
    {
        return;
    }

    const std::size_t sample_count = samples_count * channels;
    for (size_t x = 0; x < sample_count; x++)
    {
        int sample = buf[x] * 1.80;
        if (sample > INT16_MAX)
        {
            buf[x] = INT16_MAX;
        }
        else if (sample < INT16_MIN)
        {
            buf[x] = INT16_MIN;
        }
        else
        {
            buf[x] = (int16_t)sample;
        }
    }

    m_ring.producerWrite(buf, samples_count);
    const auto queued = m_ring.currentFrames();
    if (!m_playback_started.load(std::memory_order_acquire) &&
        queued >= prefillFrames())
    {
        m_playback_started.store(true, std::memory_order_release);
        m_ring.notePrefillComplete();
        brls::Logger::info(
            "Audio prefill complete at {} frames, starting SDL playback",
            queued);
    }

    logSummary(false);
}

void AudioManager::cleanup()
{
    if (m_device_id > 0)
    {
        m_shutdown.store(true, std::memory_order_release);
        SDL_PauseAudioDevice(m_device_id, 1);
        logSummary(true);
        SDL_CloseAudioDevice(m_device_id);
        m_device_id = 0;
        resetRing();
    }
}

std::size_t AudioManager::prefillFrames() const
{
    return static_cast<std::size_t>(m_ring.rate()) *
           kPrefillMs / 1000;
}

void AudioManager::resetRing()
{
    m_playback_started.store(false, std::memory_order_release);
    m_ring.clear();
}

void AudioManager::sdlAudioCallback(
    void* userdata,
    Uint8* stream,
    int len)
{
    auto* self = static_cast<AudioManager*>(userdata);
    if (self)
    {
        self->audioCallback(stream, len);
    }
    else if (stream && len > 0)
    {
        std::memset(stream, 0, static_cast<std::size_t>(len));
    }
}

void AudioManager::audioCallback(Uint8* stream, int len)
{
    if (!stream || len <= 0 || !m_ring.configured() ||
        m_shutdown.load(std::memory_order_acquire))
    {
        if (stream && len > 0)
        {
            std::memset(stream, 0, static_cast<std::size_t>(len));
        }
        return;
    }

    m_callback_active.store(true, std::memory_order_release);
    const auto bytes_per_frame =
        m_ring.channels() * sizeof(int16_t);
    const std::size_t frames =
        bytes_per_frame == 0
            ? 0
            : static_cast<std::size_t>(len) / bytes_per_frame;
    const auto started =
        m_playback_started.load(std::memory_order_acquire);
    const auto before = m_ring.currentFrames();
    const std::size_t missing =
        started && before < frames ? frames - before : 0;

    m_ring.consumerRead(
        reinterpret_cast<int16_t*>(stream),
        frames,
        started,
        missing);
    m_callback_active.store(false, std::memory_order_release);
}

void AudioManager::logSummary(bool force)
{
    const auto now = steadySeconds();
    auto last = m_last_summary_second.load(std::memory_order_relaxed);
    if (!force &&
        !m_last_summary_second.compare_exchange_strong(
            last,
            now,
            std::memory_order_relaxed))
    {
        return;
    }

    const auto stats = m_ring.stats();
    brls::Logger::info(
        "audio-summary requested_freq={} obtained_freq={} "
        "requested_channels={} obtained_channels={} requested_format={} "
        "obtained_format={} requested_samples={} obtained_samples={} "
        "decoded_pcm_blocks={} ring_current_samples={} ring_max_samples={} "
        "prefill_starts={} prefill_completions={} underrun_callbacks={} "
        "underrun_missing_samples={} dropped_oldest_blocks={} "
        "dropped_oldest_samples={} callback_count={} open_errors={}",
        m_requested.freq,
        m_obtained.freq,
        m_requested.channels,
        m_obtained.channels,
        static_cast<int>(m_requested.format),
        static_cast<int>(m_obtained.format),
        m_requested.samples,
        m_obtained.samples,
        stats.decoded_blocks,
        stats.current_frames,
        stats.max_frames,
        stats.prefill_starts,
        stats.prefill_completions,
        stats.underrun_callbacks,
        stats.underrun_missing_frames,
        stats.dropped_oldest_blocks,
        stats.dropped_oldest_frames,
        stats.callback_count,
        m_open_errors.load(std::memory_order_relaxed));
}
