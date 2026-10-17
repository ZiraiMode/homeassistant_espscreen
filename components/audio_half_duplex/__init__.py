"""Serialize physical capture/playback without changing logical microphone clients."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import audio, microphone, speaker
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_ID,
    CONF_MICROPHONE,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
)

CODEOWNERS = []
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["microphone", "speaker"]

ns = cg.esphome_ns.namespace("audio_half_duplex")
AudioHalfDuplex = ns.class_("AudioHalfDuplex", cg.Component)
MicrophoneProxy = ns.class_("MicrophoneProxy", microphone.Microphone)
SpeakerProxy = ns.class_("SpeakerProxy", speaker.Speaker)


def _stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=config[CONF_BITS_PER_SAMPLE],
        max_bits_per_sample=config[CONF_BITS_PER_SAMPLE],
        min_channels=config[CONF_NUM_CHANNELS],
        max_channels=config[CONF_NUM_CHANNELS],
        min_sample_rate=config[CONF_SAMPLE_RATE],
        max_sample_rate=config[CONF_SAMPLE_RATE],
    )(config)
    return config


# Separate nested declarations let native audio final validators resolve each proxy's
# actual stream (capture need not have the playback sample rate).
STREAM_SCHEMA = {
    cv.Required(CONF_BITS_PER_SAMPLE): cv.int_range(8, 32),
    cv.Required(CONF_NUM_CHANNELS): cv.int_range(1, 2),
    cv.Required(CONF_SAMPLE_RATE): cv.int_range(8000, 48000),
}
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AudioHalfDuplex),
            cv.Required(CONF_MICROPHONE): cv.All(
                microphone.MICROPHONE_SCHEMA.extend(STREAM_SCHEMA).extend(
                    {
                        cv.GenerateID(): cv.declare_id(MicrophoneProxy),
                        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
                    }
                ),
                _stream_limits,
            ),
            cv.Required(CONF_SPEAKER): cv.All(
                audio.AUDIO_COMPONENT_SCHEMA.extend(STREAM_SCHEMA).extend(
                    {
                        cv.GenerateID(): cv.declare_id(SpeakerProxy),
                        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
                    }
                ),
                _stream_limits,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


def _final_validate(config):
    for kind in (CONF_MICROPHONE, CONF_SPEAKER):
        stream = config[kind]
        audio.final_validate_audio_schema(
            "audio_half_duplex",
            audio_device=kind,
            bits_per_sample=stream[CONF_BITS_PER_SAMPLE],
            channels=stream[CONF_NUM_CHANNELS],
            sample_rate=stream[CONF_SAMPLE_RATE],
        )(stream)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    mic = cg.Pvariable(config[CONF_MICROPHONE][CONF_ID], var.get_microphone())
    await microphone.register_microphone(mic, config[CONF_MICROPHONE])
    out = cg.Pvariable(config[CONF_SPEAKER][CONF_ID], var.get_speaker())
    await speaker.register_speaker(out, config[CONF_SPEAKER])
    cg.add(var.set_microphone(await cg.get_variable(config[CONF_MICROPHONE][CONF_MICROPHONE])))
    cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER][CONF_SPEAKER])))
    # Available before setup priorities run: microphone sources may inspect their
    # stream in another component's setup(), before this coordinator's setup().
    for kind, setter in (
        (CONF_MICROPHONE, var.set_microphone_stream),
        (CONF_SPEAKER, var.set_speaker_stream),
    ):
        stream = config[kind]
        cg.add(setter(stream[CONF_BITS_PER_SAMPLE], stream[CONF_NUM_CHANNELS], stream[CONF_SAMPLE_RATE]))
