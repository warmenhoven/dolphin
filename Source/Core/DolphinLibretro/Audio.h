#include <libretro.h>
#include <atomic>
#include <condition_variable>
#include "Core/State.h"
#include "Core/System.h"
#include "AudioCommon/SoundStream.h"
#include "Common/Options.h"

namespace Libretro
{
namespace Audio
{
extern retro_audio_sample_batch_t batch_cb;
static constexpr unsigned int MIN_SAMPLES = 96;
static constexpr unsigned int MAX_SAMPLES = 512;

void Reset();
void Init();
void Start();
unsigned int GetSampleRate();

class Stream final : public SoundStream
{
public:
  bool Init() override;

  static bool IsValid();

  bool SetRunning(bool running) override { return true; }

  void MixAndPush(unsigned int num_samples);
  void Update(unsigned int num_samples) override;

  void ProcessCallBack() override;

private:
  s16 m_buffer[MAX_SAMPLES * 2];
  std::atomic<bool> m_callback_received{false};
  unsigned m_sample_rate{48000};
};

}  // namespace Audio

namespace FrameTiming
{
  extern std::atomic<retro_usec_t> target_frame_duration_usec;
  extern std::atomic<retro_usec_t> measured_frame_duration_usec;
  extern bool use_frame_time_cb;

  void Init();
  void Reset();
  bool IsEnabled();
  void CheckForFastForwarding();
  bool IsFastForwarding();
  void ThrottleFrame();
} // namespace FrameTiming

}  // namespace Libretro

void retroarch_audio_cb();
void retroarch_audio_state_cb(bool enable);
void retroarch_audio_buffer_status_cb(bool active, unsigned occupancy, bool underrun);
