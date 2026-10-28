`audiocodec.h` and `libSceAudiocodec_stub.a` come from DolceSDK (2021-01-16 build), a VitaSDK fork whose headers
derive from vita-headers. The stub uses the same `.vitalink.fstubs` format as VitaSDK, with the NIDs embedded
(library SceAudiocodec), so it links with the VitaSDK toolchain unchanged. VitaSDK has no SceAudiocodec stub.

`mp4rec.h` is the SceMp4Rec (MP4 recorder) header from vita-headers (vitasdk/vita-headers, 2026), copied here because
older VitaSDK installs don't have it yet; the stub `SceLibMp4Recorder_stub` ships with VitaSDK.
