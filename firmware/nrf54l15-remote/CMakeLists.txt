cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mi_remote_voice_remote)

target_sources(app PRIVATE
  src/main.c
  src/hid.c
  src/atvv_service.c
  src/audio.c
  src/battery.c
  src/battery_level.c
  src/remote_sm.c
  src/atvv_proto.c
  src/adpcm.c
)
