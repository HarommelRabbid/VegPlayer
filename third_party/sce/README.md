`audiocodec.h` and `libSceAudiocodec_stub.a` come from DolceSDK (2021-01-16 build), a VitaSDK fork whose headers
derive from vita-headers. The stub uses the same `.vitalink.fstubs` format as VitaSDK, with the NIDs embedded
(library SceAudiocodec), so it links with the VitaSDK toolchain unchanged. VitaSDK has no SceAudiocodec stub.
