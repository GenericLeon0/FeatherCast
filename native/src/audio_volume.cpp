#include "audio_volume.hpp"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace feathercast::audio {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<IAudioEndpointVolume> DefaultOutputEndpoint() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(enumerator.GetAddressOf())))) {
    return nullptr;
  }

  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                 device.GetAddressOf()))) {
    return nullptr;
  }

  ComPtr<IAudioEndpointVolume> volume;
  if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                              nullptr, reinterpret_cast<void**>(
                                           volume.GetAddressOf())))) {
    return nullptr;
  }
  return volume;
}

}  // namespace

std::optional<int> ReadDefaultOutputVolumePercent() {
  const auto volume = DefaultOutputEndpoint();
  if (!volume) return std::nullopt;

  float scalar = 0.0f;
  if (FAILED(volume->GetMasterVolumeLevelScalar(&scalar))) return std::nullopt;
  return ClampPercent(static_cast<int>(std::lround(scalar * 100.0f)));
}

bool SetDefaultOutputVolumePercent(int percent) {
  const auto volume = DefaultOutputEndpoint();
  if (!volume) return false;
  const float scalar = static_cast<float>(ClampPercent(percent)) / 100.0f;
  return SUCCEEDED(volume->SetMasterVolumeLevelScalar(scalar, nullptr));
}

bool StepDefaultOutputVolume(bool increase) {
  const auto volume = DefaultOutputEndpoint();
  if (!volume) return false;
  return SUCCEEDED(increase ? volume->VolumeStepUp(nullptr)
                            : volume->VolumeStepDown(nullptr));
}

bool ToggleDefaultOutputMute() {
  const auto volume = DefaultOutputEndpoint();
  if (!volume) return false;

  BOOL muted = FALSE;
  if (FAILED(volume->GetMute(&muted))) return false;
  return SUCCEEDED(volume->SetMute(!muted, nullptr));
}

}  // namespace feathercast::audio
