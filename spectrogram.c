#include <string.h>     // memset
#include <stdio.h>      // perror, fprintf
#include <stdlib.h>     // exit
#include <unistd.h>     // write, usleep
#include <sys/mman.h>   // MAP_FAILED
#include <fcntl.h>      // open
#include <math.h>       // log, sqrt, etc.

#include "fftw3.h"

#include "squeeze_vis.h"

// led banner definitions
#define WIDTH 80
#define HEIGHT 8
#define BARS_SIZE   16
#define NR_COLORS   240

#define FFT_N       2048
#define AUDIO_FRAME (FFT_N)

#define CLAMP(x,min,max) ((x)<(min)?(min):(x)>(max)?(max):(x))

// mmap the file
static bool do_mmap(const char *filename)
{
    int vis_fd;

    vis_fd = open(filename, O_RDONLY, 0);
    if (vis_fd <= 0) {
        perror("open failed");
        return false;
    }

    vis_mmap = (struct vis_t *)mmap(0, sizeof(struct vis_t), PROT_READ, MAP_SHARED, vis_fd, 0);
	if (vis_mmap == MAP_FAILED) {
        perror("mmap failed");
        return false;
	}

	return true;
}

// fixes an offset x in the visualisation buffer to the range 0..VIS_BUF_SIZE-1
static int fix_offset(int offset)
{
    offset = (offset + VIS_BUF_SIZE) % VIS_BUF_SIZE;
    if (offset < 0) {
        offset += VIS_BUF_SIZE;
    }
    return offset;
}

// outputs a frame to stdout
static void output(uint8_t frame[HEIGHT][WIDTH][3], int size)
{
    write(1, frame, size);
}

// creates a palette ranging from black, blue, green, yellow, red, white
static void create_palet(uint8_t palet[][3])
{
    uint8_t r, g, b;
    r = 0;
    g = 0;
    b = 0;
    int t;
    for (t = 0; t < NR_COLORS; t++) {
        if (t < 60) {
            b += 2;
        } else if (t < 120) {
            b -= 2;
            g += 2;
        } else if (t < 180) {
            r += 4;
        } else if (t < 240) {
            g -= 2;
        }
        palet[t][0] = r;
        palet[t][1] = g;
        palet[t][2] = b;
    }
}

// draws spectrogram + spectrum bars, returns current rms value
static double draw_spect(uint8_t frame[HEIGHT][WIDTH][3], uint8_t palet[][3], fftw_plan plan, fftw_complex out[], double scale)
{
    // forward fft
    fftw_execute(plan);

    // scroll spectrogram left
    int x;
    int y;
    for (y = 0; y < HEIGHT; y++) {
        for (x = 1; x < WIDTH - BARS_SIZE; x++) {
            frame[y][x - 1][0] = frame[y][x][0];
            frame[y][x - 1][1] = frame[y][x][1];
            frame[y][x - 1][2] = frame[y][x][2];
        }
    }

    // draw new spectrogram column
    int i;
    int size = FFT_N / 1024;
    int index = size;
    double totalsum = 0.0;
    for (y = 0; y < HEIGHT; y++) {
        // sum all energy in one octave
        double sum = 0.0;
        for (i = 0; i < size; i++) {
            // re^2
            sum += out[index][0] * out[index][0];
            // im^2
            sum += out[index][1] * out[index][1];
            index++;
        }
        size *= 2;
        totalsum += sum;

        // compute palette index
        int h = 50.0 * sqrt(sqrt(sum) / scale);
        h = CLAMP(h, 0, NR_COLORS - 1);

        // spectrogram pixels
        int xx = WIDTH - BARS_SIZE - 1;
        int yy = 7 - y;
        frame[yy][xx][0] = palet[h][0];
        frame[yy][xx][1] = palet[h][1];
        frame[yy][xx][2] = palet[h][2];

        // spectrum bars
        int x;
        for (x = 0; x < BARS_SIZE; x++) {
            int xx = x + WIDTH - BARS_SIZE;
            int cc = x * NR_COLORS / BARS_SIZE;
            if (cc <= h) {
                frame[yy][xx][0] = palet[cc][0];
                frame[yy][xx][1] = palet[cc][1];
                frame[yy][xx][2] = palet[cc][2];
            } else {
                frame[yy][xx][0] = 0;
                frame[yy][xx][1] = 0;
                frame[yy][xx][2] = 0;
            }
        }
    }

    // return total energy in spectrogram
    return sqrt(totalsum / index);
}

static uint8_t banner[HEIGHT][WIDTH][3];

// argv[1] = name of /dev/shm file created by squeezelite
// argv[2] = number of seconds to run (if not present: forever)
int main(int argc, char *argv[])
{
    // mmap file
    const char *filename = "/dev/shm/squeezelite-00:21:00:02:cc:45";
    if (argc > 1) {
        filename = argv[1];
    }
    if (!do_mmap(filename)) {
        exit(-1);
    }

    // max runtime
    int seconds = 0;
    int runtime = 0;
    if (argc > 2) {
        runtime = atoi(argv[2]);
    }

    time_t now;
    time_t then = time(NULL);
    int fps = 0;

    // palette
    uint8_t palette[NR_COLORS][3];
    create_palet(palette);

    // fft initialisation
    double *in;
    fftw_complex *out;
    fftw_plan plan;
    in = (double*) fftw_malloc(sizeof(double) * FFT_N);
    out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * (FFT_N / 2 + 1));
    plan = fftw_plan_dft_r2c_1d(FFT_N, in, out, 0);
    int rms_avg = 1;

    u32_t buf_index = 0;

    while (1) {
        // check for data available
        int avail = fix_offset(vis_mmap->buf_index - buf_index);
        bool have_new_data = (avail >= AUDIO_FRAME);
        if (have_new_data) {
            // unwrap buffer, convert stereo integer to mono double, apply simple triangular window
            int i;
            for (i = 0; i < (2 * AUDIO_FRAME); i += 2) {
                int index = fix_offset(buf_index - AUDIO_FRAME + i);
                double w = (i < AUDIO_FRAME) ? i : (2*AUDIO_FRAME - i);
                in[i / 2] = w * (vis_mmap->buffer[index + 0] + vis_mmap->buffer[index + 1]);
            }
            // update our read index
            buf_index = fix_offset(buf_index + AUDIO_FRAME);
        }

        // update led banner
        if (have_new_data) {
            double rms = draw_spect(banner, palette, plan, out, rms_avg);
            rms_avg += (rms - rms_avg) / 64;
            output(banner, sizeof(banner));
            fps++;
        }

        // stats
        now = time(NULL);
        if (now != then) {
            fprintf(stderr, "fps=%d, rms=%6d\n", fps, rms_avg);
            then = now;
            fps = 0;
            seconds++;
        }

        // check max runtime
        if ((runtime > 0) && (seconds > runtime)) {
            break;
        }

        // wait some time
        usleep(1000);
    }

    return 0;
}

