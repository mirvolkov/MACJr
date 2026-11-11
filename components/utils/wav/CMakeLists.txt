# WAV file playback on the speaker. Component name: "wav".
idf_component_register(
  SRCS "wav.c"
  INCLUDE_DIRS "."
  REQUIRES esp_common speaker freertos
)
