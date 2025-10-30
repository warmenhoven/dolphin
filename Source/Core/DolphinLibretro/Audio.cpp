#include <libretro.h>
#include "Audio.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "AudioCommon/AudioCommon.h"
#include "VideoCommon/Present.h"
#include "DolphinLibretro/Common/Globals.h"

namespace Libretro
{
namespace Audio
{
retro_audio_sample_batch_t batch_cb = nullptr;

static std::atomic<bool> g_buf_support{false};
static std::atomic<unsigned> g_buf_occupancy{0};
static std::atomic<bool> g_buf_underrun{false};
static bool call_back_audio{false};
static bool g_audio_state_cb{false};

unsigned int GetSampleRate()
{
  // when called from retro_run
  SoundStream* sound_stream = Core::System::GetInstance().GetSoundStream();
  double sampleRate = Libretro::Options::GetCached<int>(Libretro::Options::audio::MIXER_RATE);
  if (sound_stream && sound_stream->GetMixer() &&
      sound_stream->GetMixer()->GetSampleRate() != 0)
    return sound_stream->GetMixer()->GetSampleRate();
  // used in Stream constructor
  if (Core::System::GetInstance().IsWii())
    return sampleRate;
  else if (sampleRate == 32000u)
    return 32029;

  return 48043;
}

void Reset()
{
  g_audio_state_cb = false;
}

void Init()
{
  Reset();

  call_back_audio = Libretro::Options::GetCached<bool>(Libretro::Options::audio::CALL_BACK_AUDIO);

  if (!call_back_audio)
    return;

  retro_audio_callback racb = {};
  racb.callback = &retroarch_audio_cb;
  racb.set_state = &retroarch_audio_state_cb;

  if (!Libretro::environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &racb))
  {
    call_back_audio = false;
    DEBUG_LOG_FMT(VIDEO, "Async audio callback not supported; fallback to sync");
    return;
  }

  if(!FrameTiming::IsEnabled())
  {
    call_back_audio = false;
    DEBUG_LOG_FMT(VIDEO, "Async audio callback not enabled as FrameTiming not available");
    return;
  }

  // buffer status callback
  retro_audio_buffer_status_callback bs{};
  bs.callback = &retroarch_audio_buffer_status_cb;

  if (Libretro::environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &bs))
  {
    g_buf_support.store(true, std::memory_order_relaxed);
    DEBUG_LOG_FMT(VIDEO, "Registered async audio buffer status callback");
  }
  else
  {
    g_buf_support.store(false, std::memory_order_relaxed);
    DEBUG_LOG_FMT(VIDEO, "Audio buffer status callback not supported");
  }
}

void Start()
{
  auto& system = Core::System::GetInstance();
  system.SetSoundStreamRunning(false);
  std::unique_ptr<Libretro::Audio::Stream> audio_stream = std::make_unique<Libretro::Audio::Stream>();
  AudioCommon::SetSoundStreamRunning(system, false);
  audio_stream->Init();
  system.SetSoundStream(std::move(audio_stream));
  AudioCommon::SetSoundStreamRunning(system, true);
}

inline unsigned GetSamplesForFrame(unsigned sample_rate)
{
  double frame_time_sec = FrameTiming::target_frame_duration_usec.load(std::memory_order_relaxed) * 1e-6;
  return std::clamp(static_cast<unsigned>(frame_time_sec * sample_rate),
                    MIN_SAMPLES, MAX_SAMPLES);
}

bool Stream::Init()
{
  m_sample_rate = GetSampleRate();

  GetMixer()->SetSampleRate(m_sample_rate);

  return true;
}

bool Stream::IsValid()
{
  if(batch_cb)
    return true;

  return false;
}

void Stream::MixAndPush(unsigned int num_samples)
{
  static unsigned pending = 0;
  pending += num_samples;

  if (pending < MIN_SAMPLES)
    return; // not enough yet

  unsigned avail = pending;
  pending = 0; // consume all

  // First push the minimum threshold block
  m_mixer->Mix(m_buffer, MIN_SAMPLES);
  batch_cb(m_buffer, MIN_SAMPLES);
  avail -= MIN_SAMPLES;

  // Then push any remaining in MAX_SAMPLES chunks
  while (avail > MAX_SAMPLES)
  {
    m_mixer->Mix(m_buffer, MAX_SAMPLES);
    batch_cb(m_buffer, MAX_SAMPLES);
    avail -= MAX_SAMPLES;
  }
  if (avail)
  {
    m_mixer->Mix(m_buffer, avail);
    batch_cb(m_buffer, avail);
  }
}

void Stream::Update(unsigned int num_samples)
{
  if (call_back_audio) {
    return;
  }

  if (!m_mixer || !batch_cb)
    return;

  MixAndPush(num_samples);
}

void Stream::ProcessCallBack()
{
  if (!m_mixer || !batch_cb || !Libretro::g_emuthread_launched)
    return;

  // True: Audio driver in frontend is active
  // False: Audio driver in frontend is paused or inactive
  if (!Libretro::Audio::g_audio_state_cb)
    return;

  auto& system = Core::System::GetInstance();
  if (!system.IsSoundStreamRunning())
    return;

  if (Libretro::Audio::g_buf_support.load(std::memory_order_relaxed))
  {
    unsigned occ = Libretro::Audio::g_buf_occupancy.load(std::memory_order_relaxed);

    if (occ >= MAX_SAMPLES)
      return;

    // Use frame time to decide how much to push
    unsigned to_mix = GetSamplesForFrame(m_sample_rate);
    m_mixer->Mix(m_buffer, to_mix);
    batch_cb(m_buffer, to_mix);

    return;
  }

  unsigned to_mix = GetSamplesForFrame(m_sample_rate);

  // Clamp to sane range
  to_mix = std::clamp(to_mix, MIN_SAMPLES, MAX_SAMPLES);
  m_mixer->Mix(m_buffer, to_mix);
  batch_cb(m_buffer, to_mix);
}
} // namespace Audio

namespace FrameTiming
{
  std::atomic<retro_usec_t> target_frame_duration_usec{16667};
  std::atomic<retro_usec_t> measured_frame_duration_usec{16667};
  bool use_frame_time_cb = false;

  static retro_frame_time_callback ftcb = {};
  static auto last_frame_time = std::chrono::steady_clock::now();

  void Reset()
  {
    use_frame_time_cb = false;
  }

  void Init()
  {
    Reset();

    use_frame_time_cb = Libretro::Options::GetCached<bool>(Libretro::Options::audio::CALL_BACK_AUDIO);

    if (!use_frame_time_cb)
      return;

    // Get target refresh rate from frontend
    float refresh_rate = 60.0f;
    if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE, &refresh_rate))
    {
      use_frame_time_cb = false;
      DEBUG_LOG_FMT(VIDEO, "frame timing: unable to get target refresh rate");
      return;
    }

    if (refresh_rate < 1.0f)
      refresh_rate = 60.0f;

    // Register frame time callback to track actual frame times
    ftcb.callback = [](retro_usec_t usec) {
      measured_frame_duration_usec.store(usec, std::memory_order_relaxed);
    };
    ftcb.reference = static_cast<retro_usec_t>(1000000.0f / refresh_rate);

    target_frame_duration_usec.store(ftcb.reference, std::memory_order_relaxed);

    if (!Libretro::environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &ftcb))
    {
      use_frame_time_cb = false;
      DEBUG_LOG_FMT(VIDEO, "frame timing: unable to set frame time callback");
      return;
    }

    last_frame_time = std::chrono::steady_clock::now();
    DEBUG_LOG_FMT(VIDEO, "frame timing enabled: target={} usec ({} Hz)",
                  ftcb.reference, refresh_rate);
  }

  bool IsEnabled()
  {
    return use_frame_time_cb;
  }

  bool IsFastForwarding()
  {
    return VideoCommon::g_is_fast_forwarding;
  }

  void CheckForFastForwarding()
  {
    if (!Libretro::environ_cb)
      return;

    bool is_fast_forwarding = false;

    // Query the fast-forward state from RetroArch
    if (Libretro::environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &is_fast_forwarding))
    {
      VideoCommon::g_is_fast_forwarding = is_fast_forwarding;
      return;
    }

    // Environment call not supported
    VideoCommon::g_is_fast_forwarding = false;
  }

  void ThrottleFrame()
  {
    if (!use_frame_time_cb)
      return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
      now - last_frame_time).count();

    retro_usec_t target_us = target_frame_duration_usec.load(std::memory_order_relaxed);

    if (elapsed_us < target_us)
    {
      auto sleep_us = target_us - elapsed_us;
      if (sleep_us > 1000)
      {
        Common::SleepCurrentThread((sleep_us - 500) / 1000);
      }

      while (std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - last_frame_time).count() < target_us)
      {
      }
    }

    last_frame_time = std::chrono::steady_clock::now();
  }
} // namespace FrameTiming
} // namespace Libretro

// Call backs
void retroarch_audio_state_cb(bool enable)
{
  Libretro::Audio::g_audio_state_cb = enable;
}

// Notifies libretro that audio data should be written
void retroarch_audio_cb()
{
  if (auto* s = Core::System::GetInstance().GetSoundStream())
    s->ProcessCallBack();
}

void retroarch_audio_buffer_status_cb(bool active,
                                      unsigned occupancy,
                                      bool underrun)
{
  Libretro::Audio::g_buf_occupancy.store(occupancy, std::memory_order_relaxed);
  Libretro::Audio::g_buf_underrun.store(underrun, std::memory_order_relaxed);
}

// retroarch hooks
extern "C" {

  void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
  {
    Libretro::Audio::batch_cb = cb;
  }

  void retro_set_audio_sample(retro_audio_sample_t cb)
  {
  }
} // extern "C"
