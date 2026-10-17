"""Compile and execute the production half-duplex transport against deferred native contracts."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Host-only interfaces; the arbitration algorithm is the production .cpp, not a
# Python/source-text reconstruction. Deferred I2S behavior lives in the C++ fakes.
INTERFACES = r"""
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>
using TickType_t = uint32_t;
inline uint32_t delayed_ticks = 0;
inline void vTaskDelay(TickType_t ticks) { delayed_ticks += ticks; }
namespace esphome {
using Mutex = std::mutex;
using LockGuard = std::lock_guard<Mutex>;
template<class> class CallbackManager;
template<class... Args> class CallbackManager<void(Args...)> {
  std::vector<std::function<void(Args...)>> callbacks_;
 public:
  template<class F> void add(F &&f) { callbacks_.emplace_back(std::forward<F>(f)); }
  void call(Args... args) { for (auto &f : callbacks_) f(args...); }
};
namespace setup_priority { constexpr float PROCESSOR = 400; }
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual float get_setup_priority() const { return 600; }
};
namespace audio {
class AudioStreamInfo {
  uint8_t bits_, channels_;
  uint32_t rate_;
 public:
  AudioStreamInfo(uint8_t bits=16, uint8_t channels=1, uint32_t rate=16000)
    : bits_(bits), channels_(channels), rate_(rate) {}
  uint32_t get_sample_rate() const { return rate_; }
  bool operator==(const AudioStreamInfo &o) const {
    return bits_ == o.bits_ && channels_ == o.channels_ && rate_ == o.rate_;
  }
  bool operator!=(const AudioStreamInfo &o) const { return !(*this == o); }
};
}
namespace microphone {
enum State : uint8_t { STATE_STOPPED, STATE_STARTING, STATE_RUNNING, STATE_STOPPING };
class Microphone {
 public:
  virtual ~Microphone() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  template<class F> void add_data_callback(F &&f) {
    data_callbacks_.add([this, f](const std::vector<uint8_t> &data) {
      if (mute_state_) f(std::vector<uint8_t>(data.size(), 0)); else f(data);
    });
  }
  bool is_running() const { return state_ == STATE_RUNNING; }
  bool is_stopped() const { return state_ == STATE_STOPPED; }
  void set_mute_state(bool muted) { mute_state_ = muted; }
  bool get_mute_state() { return mute_state_; }
  audio::AudioStreamInfo get_audio_stream_info() { return audio_stream_info_; }
 protected:
  State state_{STATE_STOPPED};
  bool mute_state_{false};
  audio::AudioStreamInfo audio_stream_info_;
  CallbackManager<void(const std::vector<uint8_t> &)> data_callbacks_;
};
}
namespace speaker {
enum State : uint8_t { STATE_STOPPED, STATE_STARTING, STATE_RUNNING, STATE_STOPPING };
class Speaker {
 public:
  virtual ~Speaker() = default;
  virtual size_t play(const uint8_t *data, size_t size, TickType_t) { return play(data, size); }
  virtual size_t play(const uint8_t *, size_t) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void finish() { stop(); }
  virtual bool has_buffered_data() const = 0;
  bool is_running() const { return state_ == STATE_RUNNING; }
  bool is_stopped() const { return state_ == STATE_STOPPED; }
  virtual void set_volume(float value) { volume_ = value; }
  virtual float get_volume() { return volume_; }
  virtual void set_mute_state(bool value) { mute_state_ = value; }
  virtual bool get_mute_state() { return mute_state_; }
  virtual void set_pause_state(bool) {}
  virtual bool get_pause_state() const { return false; }
  void set_audio_stream_info(const audio::AudioStreamInfo &info) { audio_stream_info_ = info; }
  audio::AudioStreamInfo &get_audio_stream_info() { return audio_stream_info_; }
  template<class F> void add_audio_output_callback(F &&f) { audio_output_callback_.add(std::forward<F>(f)); }
 protected:
  State state_{STATE_STOPPED};
  audio::AudioStreamInfo audio_stream_info_;
  float volume_{1};
  bool mute_state_{false};
  CallbackManager<void(uint32_t, int64_t)> audio_output_callback_;
};
}
}
"""


@unittest.skipUnless(shutil.which("g++"), "needs the host C++ compiler")
class AudioHalfDuplexHost(unittest.TestCase):
    def test_production_transport(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            common = root / "interfaces.h"
            common.write_text(INTERFACES)
            for name in (
                "esphome/components/microphone/microphone.h",
                "esphome/components/speaker/speaker.h",
                "esphome/core/component.h",
                "esphome/core/helpers.h",
                "freertos/task.h",
            ):
                header = root / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "interfaces.h"\n')
            executable = root / "audio-half-duplex"
            compiled = subprocess.run(
                ["g++", "-std=c++17", "-pthread", "-DUSE_ESP32", "-Wall", "-Wextra", "-Werror",
                 "-I", str(root), str(ROOT / "tests/test_audio_half_duplex.cpp"),
                 str(ROOT / "components/audio_half_duplex/audio_half_duplex.cpp"), "-o", str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
