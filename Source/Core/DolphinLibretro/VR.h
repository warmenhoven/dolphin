#pragma once

namespace Libretro
{
namespace VR
{
enum class Mode
{
  Off,
  // Both eyes side by side, shown by the frontend on a virtual screen.
  Stereo,
  // As Stereo, with the game camera turned by the headset and drawn at its field of view.
  Immersive,
};

Mode GetMode();
bool IsEnabled();

// Sets Dolphin's stereoscopy (and, when immersive, aspect ratio) settings for the current mode.
void ApplyConfig();

// Called every retro_run() before the frame is emulated: tells the frontend where each eye is in
// the frame and, in immersive mode, points the game camera along the headset's pose.
void Update(float eye_aspect_ratio);

void Shutdown();
}  // namespace VR
}  // namespace Libretro
