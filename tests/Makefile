# Host-side tests (Linux/macOS, plain cc). They exercise the portable parts: the .veg reader, the MP4 demuxer,
# the audio engine and the compositor's placement maths.  VITAHEADERS must point at vita-headers/include
# (for the struct definitions only):   make VITAHEADERS=$VITASDK/arm-vita-eabi/include
VITAHEADERS ?= $(VITASDK)/arm-vita-eabi/include
CFLAGS = -O2 -g -Wall -Wno-unused-parameter -Wno-unused-function -Wno-format-truncation -I../src

all: vegdump mp4test mixtest mixbench reverbtest pipetest stretchq

vegdump: vegdump.c ../src/veg.c
	$(CC) $(CFLAGS) $^ -lm -o $@
mp4test: mp4test.c ../src/mp4.c
	$(CC) $(CFLAGS) $^ -o $@
mixtest: mixtest.c ../src/mixer.c
	$(CC) $(CFLAGS) -idirafter $(VITAHEADERS) $^ -lm -o $@
mixbench: mixbench.c ../src/mixer.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -idirafter $(VITAHEADERS) $^ -lm -o $@
reverbtest: reverbtest.c ../src/mixer.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -idirafter $(VITAHEADERS) $^ -lm -o $@
pipetest: pipetest.c ../src/veg.c ../src/mixer.c ../src/scale.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -idirafter $(VITAHEADERS) $^ -lm -o $@

clean:
	rm -f vegdump mp4test mixtest mixbench reverbtest pipetest fxpreview stretchq mix.raw

stretchq: stretchq.c ../src/mixer.c
	$(CC) $(CFLAGS) -idirafter $(VITAHEADERS) $^ -lm -o $@
