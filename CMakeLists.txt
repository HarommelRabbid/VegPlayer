cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VegPlayer C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VegPlayer")
set(VITA_TITLEID  "VEGP00001")
set(VITA_VERSION  "01.00")
# ask for the extended memory budget (more room for frame caches and decoded audio)
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -ffast-math -mtune=cortex-a9 -mfpu=neon -std=gnu11")

add_executable(${PROJECT_NAME}
  src/main.c
  src/util.c
  src/veg.c
  src/mp4.c
  src/gpu.c
  src/scale.c
  src/sharptext.c
  src/vdec.c
  src/adec.c
  src/image.c
  src/prepare.c
  src/mixer.c
  src/scene.c
  src/export.c
  src/pfs.c
  src/vexport.c
  src/vdec_sw.c
  src/m4vprobe.c
  third_party/shine/bitstream.c
  third_party/shine/huffman.c
  third_party/shine/l3bitstream.c
  third_party/shine/l3loop.c
  third_party/shine/l3mdct.c
  third_party/shine/l3subband.c
  third_party/shine/layer3.c
  third_party/shine/reservoir.c
  third_party/shine/tables.c
)

# shine (MP3 encoder): ARM mode so its fixed-point multiply macros use the ARM instructions
file(GLOB SHINE_SRC third_party/shine/*.c)
set_source_files_properties(${SHINE_SRC} PROPERTIES COMPILE_FLAGS "-marm -O3 -w")

# third_party/*.h and stb_vorbis.c are compiled through image.c / adec.c
set_source_files_properties(src/vdec_sw.c PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/include)
set_source_files_properties(src/adec.c src/image.c PROPERTIES COMPILE_FLAGS "-Wno-unused-function -Wno-unused-value -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-implicit-fallthrough")

target_link_libraries(${PROJECT_NAME}
  ${CMAKE_SOURCE_DIR}/third_party/sce/libSceAudiocodec_stub.a   # ATRAC3/ATRAC3plus (stub from DolceSDK)
  ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/lib/libavcodec.a       # MPEG-4 Part 2 video only (see third_party/ffmpeg/README.md)
  ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/lib/libavutil.a
  vita2d
  vitashark
  SceShaccCgExt
  taihen_stub
  SceShaccCg_stub
  freetype
  png
  jpeg
  z
  m
  c
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAudio_stub
  SceAudiodec_stub
  SceVideodec_stub
  SceAtrac_stub
  SceLibMp4Recorder_stub
  ScePower_stub
  SceIofilemgr_stub
  SceProcessmgr_stub
  SceSysmem_stub
  SceKernelThreadMgr_stub
  SceKernelModulemgr_stub
  SceLibKernel_stub
)

# room after the code segment for the SCE data vita-elf-create appends (else: "segment 1 overlaps")
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--defsym=__sce_headroom=0x10000")

# UNSAFE: the shader compiler module (ur0:data/libshacccg.suprx) is loaded at run time on first start
vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
