CFLAGS = -W -Wall -O3
LDFLAGS = -lpthread -lrt -lm -lfftw3

all: vumeter waveform spectrogram

clean:
	rm -f vumeter waveform spectrogram

