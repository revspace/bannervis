CFLAGS = -W -Wall -O3 -fomit-frame-pointer
#CFLAGS += -march=armv6 -mfpu=vfp -ffast-math
LDFLAGS = -lpthread -lrt -lm -lfftw3

all: vumeter waveform waveformf spectrogram spectrum envelope

clean:
	rm -f vumeter waveform waveformf spectrogram spectrum envelope

