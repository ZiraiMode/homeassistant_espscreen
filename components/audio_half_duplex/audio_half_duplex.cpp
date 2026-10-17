#include "audio_half_duplex.h"

#ifdef USE_ESP32
#include <freertos/task.h>
#endif

namespace esphome::audio_half_duplex {

void AudioHalfDuplex::setup() {
  // Forward the original sample vector and native playback timestamps. Inherited
  // Microphone callbacks retain ESPHome's ordinary per-client mute semantics.
  this->raw_microphone_->add_data_callback(
      [this](const std::vector<uint8_t> &data) { this->microphone_.data_callbacks_.call(data); });
  this->raw_speaker_->add_audio_output_callback(
      [this](uint32_t frames, int64_t timestamp) { this->speaker_.audio_output_callback_.call(frames, timestamp); });
  this->ready_ = true;
}

void AudioHalfDuplex::request_playback_() {
  if (this->phase_ != Phase::TRANSMIT || this->close_ != Close::NONE || this->raw_speaker_->is_stopped())
    this->playback_requested_ = true;
  this->update_states_();
}

void AudioHalfDuplex::close_playback_(Close close) {
  this->playback_requested_ = false;
  if (close == Close::STOP || this->close_ == Close::NONE) {
    // An immediate stop may supersede a finish already draining native DMA.
    if (close == Close::STOP && this->close_ == Close::FINISH)
      this->speaker_stop_sent_ = false;
    this->close_ = close;
  }
  this->update_states_();
}

void AudioHalfDuplex::update_states_() {
  if (this->listeners_ == 0) {
    this->microphone_.state_ = this->raw_microphone_->is_stopped() ? microphone::STATE_STOPPED
                                                               : microphone::STATE_STOPPING;
  } else {
    this->microphone_.state_ = this->phase_ == Phase::RECEIVE && this->microphone_reference_ &&
                                      this->raw_microphone_->is_running()
                                  ? microphone::STATE_RUNNING
                                  : microphone::STATE_STARTING;
  }
  if (this->phase_ == Phase::STOP_SPEAKER ||
      (this->phase_ == Phase::TRANSMIT && this->close_ != Close::NONE)) {
    this->speaker_.state_ = speaker::STATE_STOPPING;
  } else if (this->phase_ == Phase::TRANSMIT) {
    this->speaker_.state_ = this->raw_speaker_->is_running() ? speaker::STATE_RUNNING : speaker::STATE_STARTING;
  } else {
    this->speaker_.state_ = this->playback_requested_ ? speaker::STATE_STARTING : speaker::STATE_STOPPED;
  }
}

void AudioHalfDuplex::loop() {
  LockGuard lock(this->mutex_);
  if (!this->ready_)
    return;

  if (this->phase_ == Phase::TRANSMIT) {
    if (!this->raw_speaker_->is_stopped())
      this->speaker_seen_started_ = true;
    if (this->close_ != Close::NONE || (this->speaker_seen_started_ && this->raw_speaker_->is_stopped()))
      this->phase_ = Phase::STOP_SPEAKER;
  }
  if (this->phase_ == Phase::STOP_SPEAKER) {
    if (!this->speaker_stop_sent_) {
      if (this->writers_ == 0) {
        if (this->close_ == Close::FINISH)
          this->raw_speaker_->finish();
        else
          this->raw_speaker_->stop();
        this->speaker_stop_sent_ = true;
        this->speaker_stop_wait_ = 2;
      }
      this->update_states_();
      return;
    }
    // ESPHome has no loop-priority API and may reorder active components. Skip
    // two coordinator passes so native has processed STOP even across a reorder.
    // Native STOPPED alone is insufficient: it may still have a pending START.
    if (this->speaker_stop_wait_ != 0) {
      --this->speaker_stop_wait_;
      this->update_states_();
      return;
    }
    if (!this->raw_speaker_->is_stopped()) {
      this->update_states_();
      return;
    }
    this->speaker_stop_sent_ = false;
    this->speaker_seen_started_ = false;
    this->close_ = Close::NONE;
    this->phase_ = Phase::RECEIVE;
  }

  if (this->phase_ != Phase::TRANSMIT) {
    // Neither native start nor a write has been submitted for WAIT_MICROPHONE,
    // so canceling it needs no speaker fence.
    this->close_ = Close::NONE;
    this->phase_ = this->playback_requested_ ? Phase::WAIT_MICROPHONE : Phase::RECEIVE;
  }
  const bool want_microphone = this->listeners_ != 0 && this->phase_ == Phase::RECEIVE;
  if (this->microphone_reference_ && !want_microphone && !this->raw_microphone_->is_stopped()) {
    this->raw_microphone_->stop();
    this->microphone_reference_ = false;
  }
  // Native microphone.stop() ignores STATE_STOPPED: a just-acquired reference
  // must first become STARTING before it can be returned, even after cancellation.
  if (!this->microphone_reference_ && this->raw_microphone_->is_stopped()) {
    if (want_microphone) {
      this->raw_microphone_->start();
      this->microphone_reference_ = true;
    } else if (this->phase_ == Phase::WAIT_MICROPHONE) {
      this->raw_speaker_->set_audio_stream_info(this->speaker_.get_audio_stream_info());
      this->raw_speaker_->start();
      this->playback_requested_ = false;
      this->phase_ = Phase::TRANSMIT;
    }
  }
  this->update_states_();
}

void MicrophoneProxy::start() {
  LockGuard lock(this->parent_->mutex_);
  ++this->parent_->listeners_;
  this->parent_->update_states_();
}

void MicrophoneProxy::stop() {
  LockGuard lock(this->parent_->mutex_);
  if (this->parent_->listeners_ != 0)
    --this->parent_->listeners_;
  this->parent_->update_states_();
}

void SpeakerProxy::start() {
  LockGuard lock(this->parent_->mutex_);
  this->parent_->request_playback_();
}

void SpeakerProxy::stop() {
  LockGuard lock(this->parent_->mutex_);
  this->parent_->close_playback_(AudioHalfDuplex::Close::STOP);
}

void SpeakerProxy::finish() {
  LockGuard lock(this->parent_->mutex_);
  this->parent_->close_playback_(AudioHalfDuplex::Close::FINISH);
}

size_t SpeakerProxy::play(const uint8_t *data, size_t length) {
#ifdef USE_ESP32
  return this->play(data, length, 0);
}

size_t SpeakerProxy::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
#endif
  if (length == 0)
    return 0;
  bool admitted = false;
  {
    LockGuard lock(this->parent_->mutex_);
    this->parent_->request_playback_();
    if (this->parent_->ready_ && this->parent_->phase_ == AudioHalfDuplex::Phase::TRANSMIT &&
        this->parent_->close_ == AudioHalfDuplex::Close::NONE && this->parent_->raw_speaker_->is_running()) {
      ++this->parent_->writers_;
      admitted = true;
      if (this->parent_->raw_speaker_->get_audio_stream_info() != this->audio_stream_info_)
        this->parent_->raw_speaker_->set_audio_stream_info(this->audio_stream_info_);
    }
  }
  if (!admitted) {
#ifdef USE_ESP32
    // The caller retains unaccepted bytes (the mixer's existing transfer buffer).
    // Honor its worker wait budget without blocking the coordinator's loop.
    if (ticks_to_wait != 0)
      vTaskDelay(ticks_to_wait);
#endif
    return 0;
  }
#ifdef USE_ESP32
  const size_t written = this->parent_->raw_speaker_->play(data, length, ticks_to_wait);
#else
  const size_t written = this->parent_->raw_speaker_->play(data, length);
#endif
  {
    LockGuard lock(this->parent_->mutex_);
    --this->parent_->writers_;
  }
  return written;
}

bool SpeakerProxy::has_buffered_data() const { return this->parent_->raw_speaker_->has_buffered_data(); }
void SpeakerProxy::set_pause_state(bool paused) { this->parent_->raw_speaker_->set_pause_state(paused); }
bool SpeakerProxy::get_pause_state() const { return this->parent_->raw_speaker_->get_pause_state(); }
void SpeakerProxy::set_volume(float volume) { this->parent_->raw_speaker_->set_volume(volume); }
float SpeakerProxy::get_volume() { return this->parent_->raw_speaker_->get_volume(); }
void SpeakerProxy::set_mute_state(bool muted) { this->parent_->raw_speaker_->set_mute_state(muted); }
bool SpeakerProxy::get_mute_state() { return this->parent_->raw_speaker_->get_mute_state(); }

}  // namespace esphome::audio_half_duplex
