`audiocodec.h` and `libSceAudiocodec_stub.a` come from DolceSDK (2021-01-16 build), a VitaSDK fork whose headers
derive from vita-headers. The stub uses the same `.vitalink.fstubs` format as VitaSDK, with the NIDs embedded
(library SceAudiocodec), so it links with the VitaSDK toolchain unchanged. VitaSDK has no SceAudiocodec stub.

`mp4rec.h` is the SceMp4Rec (MP4 recorder) header from vita-headers (vitasdk/vita-headers, 2026), copied here because
older VitaSDK installs don't have it yet; the stub `SceLibMp4Recorder_stub` ships with VitaSDK.

`libSceAacenc_stub_weak.a` was generated with `vita-libs-gen-2` from the NIDs of the AAC encoder's
`SceAacencInternal` library (module SceAacenc, loaded along with the MP4 recorder); the NIDs come from the module's
export table. The function names are ours (the library has no published header); see `src/encapi.h`.
