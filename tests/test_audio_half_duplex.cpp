#include "../components/audio_half_duplex/audio_half_duplex.h"

#include <cassert>
#include <condition_variable>
#include <thread>

using namespace esphome;

#include "amplifier_interval.h"

struct Bus {
  bool rx{false};
  bool tx{false};
  void check() const { assert(!(rx && tx)); }
};

// These fakes reproduce the native asynchronous contracts, not the transport:
// mic.start takes a reference while STOPPED; stop there is deliberately ignored.
class NativeMicrophone : public microphone::Microphone {
 public:
  explicit NativeMicrophone(Bus &bus) : bus(bus) {}
  Bus &bus;
  unsigned references{0}, starts{0}, stops{0};
  void start() override {
    ++starts;
    ++references;
    assert(references == 1);  // Multiple logical readers must never reach native.
  }
  void stop() override {
    if (is_stopped())
      return;
    assert(references == 1);
    --references;
    ++stops;
  }
  void loop() {
    if (state_ == microphone::STATE_STOPPING) {
      bus.rx = false;
      state_ = microphone::STATE_STOPPED;
    }
    if (references && is_stopped()) {
      assert(!bus.tx);
      bus.rx = true;
      state_ = microphone::STATE_STARTING;
    } else if (state_ == microphone::STATE_STARTING) {
      state_ = microphone::STATE_RUNNING;
    }
    if (!references && is_running())
      state_ = microphone::STATE_STOPPING;
    bus.check();
  }
  void emit(const std::vector<uint8_t> &samples) { data_callbacks_.call(samples); }
};

class NativeSpeaker : public speaker::Speaker {
 public:
  explicit NativeSpeaker(Bus &bus) : bus(bus) {}
  Bus &bus;
  unsigned starts{0}, stops{0}, finishes{0};
  size_t buffered{0}, dma{0};
  std::function<void(char)> close_command;
  std::function<void()> clock_stop;
  bool start_pending{false}, stop_pending{false}, finish_pending{false}, timeout{false}, paused{false};
  std::mutex gate;
  std::condition_variable condition;
  bool block_next{false}, entered{false}, released{false};

  void start() override {
    if (is_running() || state_ == speaker::STATE_STARTING)
      return;
    ++starts;
    start_pending = true;  // START is not reflected by is_stopped().
  }
  void stop() override {
    if (close_command)
      close_command('S');
    ++stops;
    stop_pending = true;
  }
  void finish() override {
    if (close_command)
      close_command('F');
    ++finishes;
    finish_pending = true;
  }
  bool has_buffered_data() const override { return buffered != 0; }
  void set_pause_state(bool value) override { paused = value; }
  bool get_pause_state() const override { return paused; }
  size_t play(const uint8_t *data, size_t size) override { return play(data, size, 0); }
  size_t play(const uint8_t *, size_t size, TickType_t) override {
    {
      std::unique_lock<std::mutex> lock(gate);
      if (block_next) {
        block_next = false;
        entered = true;
        condition.notify_all();
        condition.wait(lock, [this] { return released; });
      }
    }
    if (!is_running()) {
      start();  // Native play may queue START after the coordinator observed STOPPED.
      return 0;
    }
    buffered += size;
    return size;
  }
  void loop() {
    if (state_ == speaker::STATE_STOPPING) {
      if (bus.tx && clock_stop)
        clock_stop();
      bus.tx = false;
      buffered = dma = 0;
      stop_pending = finish_pending = timeout = false;
      state_ = speaker::STATE_STOPPED;
    } else if (is_stopped()) {
      if (stop_pending || finish_pending) {
        start_pending = stop_pending = finish_pending = false;
      } else if (start_pending) {
        assert(!bus.rx);
        bus.tx = true;
        start_pending = false;
        state_ = speaker::STATE_STARTING;
      }
    } else if (state_ == speaker::STATE_STARTING) {
      state_ = speaker::STATE_RUNNING;
    } else if (stop_pending || ((finish_pending || timeout) && !buffered && !dma)) {
      state_ = speaker::STATE_STOPPING;
    }
    bus.check();
  }
  void drain(uint32_t frames = 4, int64_t timestamp = 123456) {
    buffered = dma = 0;
    audio_output_callback_.call(frames, timestamp);
  }
  void wait_for_writer() {
    std::unique_lock<std::mutex> lock(gate);
    condition.wait(lock, [this] { return entered; });
  }
  void release_writer() {
    std::lock_guard<std::mutex> lock(gate);
    released = true;
    condition.notify_all();
  }
};

// Root START is deferred just like the native resampler's command event. Its
// public buffers may be empty while STARTING/RUNNING/STOPPING is still active.
class PlaybackRoot : public speaker::Speaker {
 public:
  bool start_pending{false};
  void start() override { start_pending = true; }
  void process_start() {
    assert(start_pending);
    start_pending = false;
    state_ = speaker::STATE_STARTING;
  }
  void running() { state_ = speaker::STATE_RUNNING; }
  void stop() override { state_ = speaker::STATE_STOPPING; }
  void complete() { state_ = speaker::STATE_STOPPED; }
  size_t play(const uint8_t *, size_t) override { return 0; }
  bool has_buffered_data() const override { return false; }
};

enum class LoopOrder { NATIVE_FIRST, COORDINATOR_FIRST, ALTERNATING };
static LoopOrder loop_order = LoopOrder::NATIVE_FIRST;

struct Fixture {
  Bus bus;
  NativeMicrophone raw_mic{bus};
  NativeSpeaker raw_speaker{bus};
  PlaybackRoot announcement;
  PlaybackRoot music;
  const speaker::Speaker *const playback_sources[2]{&announcement, &music};
  audio_half_duplex::AudioHalfDuplex transport;
  audio_half_duplex::MicrophoneProxy *mic{transport.get_microphone()};
  audio_half_duplex::SpeakerProxy *speaker{transport.get_speaker()};
  TestAmplifier amplifier;
  unsigned stopping_callbacks{0};
  std::vector<char> playback_events;
  const uint8_t data[8]{1, 2, 3, 4, 5, 6, 7, 8};
  bool coordinator_first{loop_order == LoopOrder::COORDINATOR_FIRST};
  Fixture() {
    transport.set_microphone(&raw_mic);
    transport.set_speaker(&raw_speaker);
    transport.set_playback_sources(playback_sources, 2);
    transport.set_microphone_stream(16, 1, 16000);
    transport.set_speaker_stream(16, 1, 48000);
    assert(mic->get_audio_stream_info().get_sample_rate() == 16000);
    assert(speaker->get_audio_stream_info().get_sample_rate() == 48000);
    transport.setup();
    transport.add_on_playback_stopping_callback([this] {
      assert(!speaker->is_running() && !speaker->is_stopped());
      amplifier.turn_off();
      ++stopping_callbacks;
      playback_events.push_back('M');
    });
    raw_speaker.close_command = [this](char command) {
      assert(stopping_callbacks != 0 && !amplifier.state);
      playback_events.push_back(command);
    };
    raw_speaker.clock_stop = [this] { playback_events.push_back('C'); };
  }
  void cycle() {
    if (coordinator_first)
      transport.loop();
    raw_mic.loop();
    raw_speaker.loop();
    if (!coordinator_first)
      transport.loop();
    if (loop_order == LoopOrder::ALTERNATING)
      coordinator_first = !coordinator_first;
    bus.check();
  }
  template<class F> void until(F ready) {
    for (unsigned i = 0; i < 80 && !ready(); ++i)
      cycle();
    assert(ready());
  }
  void cycles(unsigned count) { while (count--) cycle(); }
  void listen() { mic->start(); until([this] { return mic->is_running(); }); }
  void transmit() {
    assert(speaker->play(data, sizeof(data), 3) == 0);
    until([this] { return speaker->is_running(); });
    assert(!bus.rx && bus.tx);
    assert(raw_speaker.get_audio_stream_info().get_sample_rate() == 48000);
    assert(speaker->play(data, sizeof(data), 0) == sizeof(data));
  }
  void finish() {
    speaker->finish();
    raw_speaker.drain();
    until([this] { return speaker->is_stopped(); });
  }
};

static void two_readers_survive_playback() {
  Fixture f;
  f.listen();
  f.mic->start();
  assert(f.raw_mic.starts == 1);
  f.transmit();
  assert(f.raw_mic.references == 0 && f.raw_mic.stops == 1);
  f.speaker->finish();
  f.raw_speaker.buffered = 0;
  f.raw_speaker.dma = 4;
  f.cycles(5);
  assert(f.bus.tx && !f.bus.rx);  // Ring empty does not mean DMA playback finished.
  f.raw_speaker.drain();
  f.until([&] { return f.mic->is_running(); });
  assert(f.raw_mic.references == 1 && f.raw_mic.starts == 2);
  f.mic->stop();
  f.cycles(5);
  assert(f.mic->is_running());  // The second original listener was not lost.
  f.mic->stop();
  f.until([&] { return !f.bus.rx; });
  assert(f.raw_mic.references == 0);
}

static void microphone_pending_start_cancellation() {
  Fixture f;
  f.mic->start();
  f.transport.loop();  // Native start takes a ref, still reports STOPPED.
  assert(f.raw_mic.is_stopped() && f.raw_mic.references == 1);
  f.mic->stop();
  f.speaker->start();
  f.until([&] { return f.speaker->is_running(); });
  assert(f.raw_mic.starts == 1 && f.raw_mic.stops == 1);
  f.finish();
  f.cycles(5);
  assert(!f.bus.rx && !f.bus.tx && f.raw_mic.references == 0);

  Fixture canceled;
  canceled.mic->start();
  canceled.transport.loop();
  canceled.mic->stop();
  canceled.cycles(10);
  assert(canceled.raw_mic.references == 0 && !canceled.bus.rx);
}

static void readers_change_during_playback() {
  Fixture f;
  f.listen();
  f.mic->start();
  f.transmit();
  f.mic->stop();  // One original client leaves.
  f.mic->start();  // A new client joins while TX owns the bus.
  f.finish();
  f.until([&] { return f.mic->is_running(); });
  f.mic->stop();
  f.cycles(4);
  assert(f.mic->is_running());
  f.mic->stop();
  f.until([&] { return !f.bus.rx; });

  Fixture all_stopped;
  all_stopped.listen();
  all_stopped.transmit();
  all_stopped.mic->stop();
  all_stopped.finish();
  all_stopped.cycles(5);
  assert(!all_stopped.bus.rx && all_stopped.raw_mic.references == 0);

  Fixture joining;
  joining.transmit();  // Initially no reader.
  joining.mic->start();
  joining.mic->start();
  joining.finish();
  joining.until([&] { return joining.mic->is_running(); });
  assert(joining.raw_mic.starts == 1);
  joining.mic->stop();
  joining.cycles(3);
  assert(joining.mic->is_running());
}

static void canceled_playback_and_repeated_timeout_cycles() {
  Fixture f;
  f.listen();
  f.speaker->start();
  f.speaker->stop();  // Cancel before any physical microphone release.
  f.cycles(4);
  assert(f.mic->is_running() && f.raw_speaker.starts == 0);
  f.speaker->start();
  f.cycle();  // Waiting for RX teardown.
  f.speaker->finish();
  f.cycles(8);
  assert(f.mic->is_running() && f.raw_speaker.starts == 0);

  // Cancel after native START submission, before its loop has processed it.
  f.speaker->start();
  f.until([&] { return f.raw_mic.is_stopped() && f.raw_mic.references == 0; });
  if (!f.raw_speaker.start_pending)
    f.transport.loop();  // Submit START after native's pass in either loop order.
  assert(f.raw_speaker.is_stopped() && f.raw_speaker.start_pending);
  f.speaker->stop();
  f.until([&] { return f.mic->is_running(); });
  assert(!f.bus.tx && !f.raw_speaker.start_pending);

  for (unsigned i = 0; i < 12; ++i) {
    f.transmit();
    f.raw_speaker.drain();
    f.raw_speaker.timeout = true;
    f.until([&] { return f.mic->is_running(); });
    assert(f.raw_mic.references == 1 && !f.bus.tx && !f.raw_speaker.start_pending);
  }
  f.transmit();
  f.speaker->finish();
  f.cycles(2);
  assert(f.bus.tx);  // Still buffered, finish must not discard it.
  const auto callbacks_before_upgrade = f.stopping_callbacks;
  f.speaker->stop();  // Immediate stop upgrades an already-submitted finish.
  f.until([&] { return f.mic->is_running(); });
  assert(f.stopping_callbacks == callbacks_before_upgrade);
}

static void empty_output_and_callback_controls() {
  Fixture f;
  assert(f.speaker->play(f.data, 0, 2) == 0);
  f.cycles(3);
  assert(f.raw_speaker.starts == 0 && f.speaker->is_stopped());
  const auto before = delayed_ticks;
  assert(f.speaker->play(f.data, sizeof(f.data), 7) == 0);
  assert(delayed_ticks == before + 7);
  f.until([&] { return f.speaker->is_running(); });
  f.speaker->set_volume(0.25f);
  f.speaker->set_mute_state(true);
  f.speaker->set_pause_state(true);
  assert(f.raw_speaker.get_volume() == 0.25f && f.speaker->get_volume() == 0.25f);
  assert(f.raw_speaker.get_mute_state() && f.speaker->get_mute_state());
  assert(f.raw_speaker.get_pause_state() && f.speaker->get_pause_state());
  f.speaker->set_mute_state(false);
  f.speaker->set_pause_state(false);
  uint32_t frames = 0;
  int64_t timestamp = 0;
  f.speaker->add_audio_output_callback([&](uint32_t count, int64_t time) { frames = count; timestamp = time; });
  f.raw_speaker.drain(37, 987654321);
  assert(frames == 37 && timestamp == 987654321);
  f.finish();
  assert(!f.bus.rx && f.raw_mic.starts == 0);

  std::vector<uint8_t> samples{1, 2, 3, 4};
  const std::vector<uint8_t> *observed = nullptr;
  bool silent = false;
  f.mic->add_data_callback([&](const std::vector<uint8_t> &data) {
    observed = &data;
    silent = data == std::vector<uint8_t>(4, 0);
  });
  f.raw_mic.emit(samples);
  assert(observed == &samples && !silent);  // The transport does not copy frames.
  f.mic->set_mute_state(true);
  f.raw_mic.emit(samples);
  assert(silent);
}

static void in_flight_writer_fences_late_native_start() {
  Fixture f;
  f.listen();
  f.transmit();
  f.raw_speaker.drain();
  f.raw_speaker.block_next = true;
  size_t written = 99;
  std::thread writer([&] { written = f.speaker->play(f.data, sizeof(f.data), 0); });
  f.raw_speaker.wait_for_writer();  // Admitted, but has not entered native write yet.
  const auto stops = f.raw_speaker.stops;
  f.speaker->stop();
  f.raw_speaker.timeout = true;
  f.cycles(2);  // Native TX times out while writer is still in flight.
  assert(f.raw_speaker.is_stopped());
  assert(f.raw_speaker.stops == stops && !f.bus.rx);

  // Simulate a worker between native's loop and the coordinator.
  f.raw_mic.loop();
  f.raw_speaker.loop();
  f.raw_speaker.release_writer();
  writer.join();
  assert(written == 0 && f.raw_speaker.start_pending);
  f.transport.loop();  // Must cancel the hidden START, not trust native STOPPED.
  assert(f.raw_speaker.stop_pending && !f.bus.rx);
  f.transport.loop();  // First fenced pass may precede native after a reorder.
  assert(!f.bus.rx && f.raw_mic.references == 0);
  f.raw_speaker.loop();
  assert(!f.raw_speaker.start_pending && !f.raw_speaker.stop_pending);
  f.transport.loop();  // Second conservative pass still cannot grant RX.
  assert(!f.bus.rx && f.raw_mic.references == 0);
  f.until([&] { return f.mic->is_running(); });
  assert(!f.raw_speaker.start_pending && !f.bus.tx && f.raw_mic.references == 1);
}

static void completed_root_finishes_before_idle_timeout() {
  Fixture f;
  f.listen();
  f.announcement.start();  // A queued START still reports STOPPED.
  f.transmit();
  f.cycles(6);
  assert(f.announcement.is_stopped() && f.announcement.start_pending);
  assert(f.raw_speaker.finishes == 0 && f.stopping_callbacks == 0);

  f.announcement.process_start();
  f.cycle();
  assert(f.raw_speaker.finishes == 0);  // STARTING is not completion.
  f.announcement.running();
  f.cycle();
  amplifier_interval(*f.speaker, f.amplifier, 1000);
  assert(f.amplifier.state);

  f.raw_speaker.buffered = 0;
  f.raw_speaker.dma = 4;
  f.announcement.stop();
  f.cycles(3);
  assert(f.raw_speaker.finishes == 0);  // STOPPING is not completion either.
  f.announcement.complete();
  f.transport.loop();
  assert(f.raw_speaker.finishes == 1 && !f.raw_speaker.timeout);
  assert(f.stopping_callbacks == 1 && !f.amplifier.state);
  assert((f.playback_events == std::vector<char>{'M', 'F'}));
  // The old 300 ms hold must not re-enable the amplifier during native drain.
  amplifier_interval(*f.speaker, f.amplifier, 1001);
  assert(!f.amplifier.state);
  f.cycles(8);
  assert(f.bus.tx && !f.bus.rx && f.raw_speaker.dma == 4);
  assert(f.raw_speaker.finishes == 1 && f.stopping_callbacks == 1);
  f.raw_speaker.drain();
  f.until([&] { return f.mic->is_running(); });
  assert((f.playback_events == std::vector<char>{'M', 'F', 'C'}));
  assert(f.raw_speaker.stops == 0 && f.raw_mic.references == 1);
}

static void active_music_prevents_root_completion_close() {
  Fixture f;
  f.listen();
  f.announcement.running();
  f.music.start();
  f.music.process_start();
  f.transmit();
  f.raw_speaker.drain();
  f.announcement.complete();
  f.cycles(6);
  assert(f.raw_speaker.finishes == 0 && f.bus.tx);  // Music STARTING.
  f.music.running();
  f.cycles(6);
  assert(f.raw_speaker.finishes == 0 && f.bus.tx);  // Empty but still playing.
  f.music.stop();
  f.cycles(4);
  assert(f.raw_speaker.finishes == 0);  // Music STOPPING.
  f.music.complete();
  f.until([&] { return f.mic->is_running(); });
  assert(f.raw_speaker.finishes == 1 && f.stopping_callbacks == 1);
}

static void completed_root_waits_for_admitted_writer() {
  Fixture f;
  f.listen();
  f.announcement.running();
  f.transmit();
  f.raw_speaker.drain();
  f.raw_speaker.block_next = true;
  size_t written = 0;
  std::thread writer([&] { written = f.speaker->play(f.data, sizeof(f.data), 0); });
  f.raw_speaker.wait_for_writer();
  f.announcement.complete();
  f.cycles(5);
  assert(f.raw_speaker.finishes == 0 && f.stopping_callbacks == 0);
  f.raw_speaker.release_writer();
  writer.join();
  assert(written == sizeof(f.data));
  f.transport.loop();
  assert(f.raw_speaker.finishes == 1 && f.raw_speaker.buffered == sizeof(f.data));
  f.cycles(4);
  assert(f.bus.tx && !f.bus.rx);
  f.raw_speaker.drain();
  f.until([&] { return f.mic->is_running(); });
}

static void source_start_during_drain_keeps_successor_and_readers() {
  Fixture f;
  f.listen();
  f.announcement.running();
  f.transmit();
  f.announcement.complete();
  f.raw_speaker.buffered = 0;
  f.raw_speaker.dma = 4;
  f.transport.loop();
  assert(f.raw_speaker.finishes == 1);
  f.music.start();
  assert(f.music.is_stopped());  // New root's START is not processed yet.
  f.cycles(2);
  f.music.process_start();
  f.cycle();  // Observe the new root even before its worker calls the proxy.
  f.mic->stop();  // The original reader leaves.
  f.mic->start();
  f.mic->start();
  f.mic->stop();  // One new reader cancels during drain.
  assert(f.raw_speaker.starts == 1 && f.raw_speaker.dma == 4);
  f.raw_speaker.drain();
  f.until([&] { return f.speaker->is_running(); });
  assert(f.raw_speaker.starts == 2 && !f.bus.rx && f.raw_mic.starts == 1);
  f.music.running();
  assert(f.speaker->play(f.data, sizeof(f.data), 0) == sizeof(f.data));
  f.cycle();
  f.music.complete();
  f.raw_speaker.drain();
  f.until([&] { return f.mic->is_running(); });
  assert(f.raw_speaker.finishes == 2 && f.stopping_callbacks == 2);
  assert(f.raw_mic.references == 1 && f.raw_mic.starts == 2);
  f.mic->stop();
  f.until([&] { return !f.bus.rx; });
  assert(f.raw_mic.references == 0);
}

static void stopping_callback_reenters_without_losing_successor() {
  Fixture f;
  f.listen();
  f.transmit();
  f.raw_speaker.buffered = 0;
  f.raw_speaker.dma = 4;
  f.transport.add_on_playback_stopping_callback([&] {
    if (f.stopping_callbacks != 1)
      return;
    assert(f.bus.tx && f.raw_speaker.finishes == 0);
    f.mic->start();
    f.mic->stop();
    f.speaker->start();
    assert(f.speaker->play(f.data, sizeof(f.data), 0) == 0);
    f.speaker->finish();  // Cancel that successor; retain the next one.
    f.speaker->start();
    f.transport.loop();  // Even recursive dispatch cannot close before callback returns.
    assert(f.raw_speaker.finishes == 0);
  });
  f.speaker->finish();
  f.transport.loop();
  assert(f.raw_speaker.finishes == 1 && f.stopping_callbacks == 1);
  f.cycles(5);
  assert(f.raw_speaker.starts == 1 && f.bus.tx && !f.bus.rx);
  f.raw_speaker.drain();
  f.until([&] { return f.speaker->is_running(); });
  assert(f.raw_speaker.starts == 2 && f.raw_mic.starts == 1);
  assert(f.speaker->play(f.data, sizeof(f.data), 0) == sizeof(f.data));
  f.finish();
  f.until([&] { return f.mic->is_running(); });
  assert(f.raw_mic.references == 1 && f.stopping_callbacks == 2);
}

static void repeated_completed_root_cycles_are_stable() {
  Fixture f;
  f.listen();
  for (unsigned i = 0; i < 12; ++i) {
    // A physical START with all roots STOPPED must not inherit the prior epoch's
    // completion latch, even though that root START can still be queued.
    f.announcement.start();
    f.transmit();
    f.cycles(3);
    assert(f.raw_speaker.finishes == i);
    f.announcement.process_start();
    f.announcement.running();
    f.cycle();
    f.announcement.complete();
    f.raw_speaker.drain();
    f.until([&] { return f.mic->is_running(); });
    assert(f.raw_speaker.finishes == i + 1 && f.stopping_callbacks == i + 1);
    assert(f.raw_speaker.stops == 0 && f.raw_mic.references == 1 && !f.bus.tx);
  }
}

static void late_write_after_root_completion_keeps_successor() {
  Fixture f;
  f.listen();
  f.announcement.running();
  f.transmit();
  f.announcement.complete();
  f.raw_speaker.buffered = 0;
  f.raw_speaker.dma = 4;
  f.transport.loop();
  assert(f.raw_speaker.finishes == 1);
  f.announcement.start();  // Deferred new START still looks STOPPED.
  assert(f.speaker->play(f.data, sizeof(f.data), 0) == 0);
  f.raw_speaker.drain();
  f.until([&] { return f.speaker->is_running(); });
  assert(f.raw_speaker.starts == 2 && f.raw_mic.starts == 1);
  f.cycles(4);
  assert(f.raw_speaker.finishes == 1);  // Do not reuse the first epoch's activity.
  f.announcement.process_start();
  f.announcement.running();
  assert(f.speaker->play(f.data, sizeof(f.data), 0) == sizeof(f.data));
  f.cycle();
  f.announcement.complete();
  f.raw_speaker.drain();
  f.until([&] { return f.mic->is_running(); });
  assert(f.raw_speaker.finishes == 2 && f.stopping_callbacks == 2);
}

static void amplifier_waits_for_running_buffered_output() {
  Fixture f;
  f.listen();
  amplifier_interval(*f.speaker, f.amplifier, 2000);
  assert(!f.amplifier.state);
  f.speaker->start();
  for (uint32_t now = 2001; now < 2080 && !f.speaker->is_running(); ++now) {
    f.cycle();
    amplifier_interval(*f.speaker, f.amplifier, now);
    assert(!f.amplifier.state);
  }
  assert(f.speaker->is_running() && !f.raw_speaker.has_buffered_data());
  assert(f.speaker->play(f.data, sizeof(f.data), 0) == sizeof(f.data));
  amplifier_interval(*f.speaker, f.amplifier, 2080);
  assert(f.amplifier.state);  // No artificial wait after actual buffered audio.
  f.speaker->finish();
  f.transport.loop();
  assert(!f.amplifier.state && f.raw_speaker.finishes == 1);
  amplifier_interval(*f.speaker, f.amplifier, 2081);
  assert(!f.amplifier.state && f.bus.tx);  // Still draining, never re-enable.
  f.raw_speaker.drain();
  f.until([&] { return f.mic->is_running(); });
}

int main() {
  for (const auto order : {LoopOrder::NATIVE_FIRST, LoopOrder::COORDINATOR_FIRST, LoopOrder::ALTERNATING}) {
    loop_order = order;
    two_readers_survive_playback();
    microphone_pending_start_cancellation();
    readers_change_during_playback();
    canceled_playback_and_repeated_timeout_cycles();
    empty_output_and_callback_controls();
    in_flight_writer_fences_late_native_start();
    completed_root_finishes_before_idle_timeout();
    active_music_prevents_root_completion_close();
    completed_root_waits_for_admitted_writer();
    source_start_during_drain_keeps_successor_and_readers();
    stopping_callback_reenters_without_losing_successor();
    repeated_completed_root_cycles_are_stable();
    late_write_after_root_completion_keeps_successor();
    amplifier_waits_for_running_buffered_output();
  }
}
