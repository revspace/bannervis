bannervis
=========

Audio visualisations for use with squeezelite on an 80x8 pixel RGB led banner

See also: https://github.com/bertrik/ledbanner

Contains the following audio visualisations:
* a VU-meter, shows the RMS value and peak RMS of the left and right audio channels
* a waveform display, shows the instantaneous waveform of the mono audio channel (left + right)
* a spectrogram, showing spectral energy, each horizontal line representing one octave

To run this:
* get the squeezelite source code and compile it with option OPT_VIS (e.g. add OPTS+=$(OPT_VIS) to the Makefile)
* start squeezelite with option -v, this causes it to create a file /dev/shm/squeezelite-XX:XX:XX:XX:XX:XX
  containing a structure with raw audio data (16-bit little-endian stereo)
* run one of these visualisation applications with the shm file name as argument and pipe the output to the ledbanner.

To build this:
* make

