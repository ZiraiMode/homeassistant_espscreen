#pragma once

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome::audio_half_duplex {

class AudioHalfDuplex;

class MicrophoneProxy : public microphone::Microphone {
 public:
  explicit MicrophoneProxy(AudioHalfDuplex *parent) : parent_(parent) {}
  void start() override;
  void stop() override;

 protected:
  friend class AudioHalfDuplex;
  AudioHalfDuplex *parent_;
};

class SpeakerProxy : public speaker::Speaker {
 public:
  explicit SpeakerProxy(AudioHalfDuplex *parent) : parent_(parent) {}
  void start() override;
  void stop() override;
  void finish() override;
  size_t play(const uint8_t *data, size_t length) override;
#ifdef USE_ESP32
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
#endif
  bool has_buffered_data() const override;
  void set_pause_state(bool paused) override;
  bool get_pause_state() const override;
  void set_volume(float volume) override;
  float get_volume() override;
  void set_mute_state(bool muted) override;
  bool get_mute_state() override;

 protected:
  friend class AudioHalfDuplex;
  AudioHalfDuplex *parent_;
};

class AudioHalfDuplex : public Component {
 public:
  AudioHalfDuplex() : microphone_(this), speaker_(this) {}
  void set_microphone(microphone::Microphone *mic) { this->raw_microphone_ = mic; }
  void set_speaker(speaker::Speaker *speaker) { this->raw_speaker_ = speaker; }
  void set_playback_sources(const speaker::Speaker *const *sources, size_t count) {
    this->playback_sources_ = sources;
    this->playback_source_count_ = count;
  }
  template<typename F> void add_on_playback_stopping_callback(F &&callback) {
    this->playback_stopping_callbacks_.add(std::forward<F>(callback));
  }
  void set_microphone_stream(uint8_t bits, uint8_t channels, uint32_t rate) {
    this->microphone_.audio_stream_info_ = audio::AudioStreamInfo(bits, channels, rate);
  }
  void set_speaker_stream(uint8_t bits, uint8_t channels, uint32_t rate) {
    this->speaker_.set_audio_stream_info(audio::AudioStreamInfo(bits, channels, rate));
  }
  MicrophoneProxy *get_microphone() { return &this->microphone_; }
  SpeakerProxy *get_speaker() { return &this->speaker_; }
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 1.0f; }

 protected:
  friend class MicrophoneProxy;
  friend class SpeakerProxy;
  enum class Phase : uint8_t { RECEIVE, WAIT_MICROPHONE, TRANSMIT, STOP_SPEAKER };
  enum class Close : uint8_t { NONE, FINISH, STOP };
  void request_playback_();  // mutex held
  void close_playback_(Close close);  // mutex held
  void update_states_();  // mutex held
  bool loop_();  // mutex held; true requests the unlocked stopping callback
  bool playback_sources_stopped_() const;  // mutex held

  MicrophoneProxy microphone_;
  SpeakerProxy speaker_;
  microphone::Microphone *raw_microphone_{nullptr};
  speaker::Speaker *raw_speaker_{nullptr};
  const speaker::Speaker *const *playback_sources_{nullptr};
  size_t playback_source_count_{0};
  CallbackManager<void()> playback_stopping_callbacks_;

  // Ownership invariant: only loop() calls native start/stop/finish. Logical mic
  // clients own this count, never native semaphore references. The coordinator
  // holds at most one native mic reference, including a START not processed yet.
  // A short mutex protects commands and writer admission. Never hold it across a
  // blocking native play() or a callback. Revocation waits for admitted writers,
  // then sends STOP/FINISH and skips two subsequent coordinator passes before
  // trusting STOPPED. This spans a full native main-loop pass even if ESPHome
  // reorders components at runtime. No worker can queue START behind that fence.
  mutable Mutex mutex_;
  uint32_t listeners_{0};
  uint32_t writers_{0};
  bool microphone_reference_{false};
  bool playback_requested_{false};
  bool speaker_seen_started_{false};
  bool playback_source_seen_active_{false};
  bool playback_sources_were_stopped_{true};
  bool stopping_callback_started_{false};
  bool stopping_callback_done_{false};
  bool speaker_stop_sent_{false};
  uint8_t speaker_stop_wait_{0};
  bool ready_{false};
  Phase phase_{Phase::RECEIVE};
  Close close_{Close::NONE};
};

}  // namespace esphome::audio_half_duplex
