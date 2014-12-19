#include <stdint.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>      // open

#include <stdio.h>
#include <sys/mman.h>   // MAP_FAILED

#include <unistd.h>     // write

#include <stdlib.h>     // exit
#include <string.h>     // memcpy

#include <math.h>       // sqrt

#include "squeeze_vis.h"

// whether to use the pthread lock
//#define USE_LOCKS

// led banner definitions
#define WIDTH 80
#define HEIGHT 8

#define BUF_SIZE    (16*WIDTH)
#define AUDIO_FRAME (2*BUF_SIZE)

// mmap the file
static bool do_mmap(const char *filename)
{
    int vis_fd;
    
#if USE_LOCKS
    vis_fd = open(filename, O_RDWR, 0666);
#else
    vis_fd = open(filename, O_RDONLY, 0);
#endif
    if (vis_fd <= 0) {
        perror("open failed");
        return false;
    }

#if USE_LOCKS
    vis_mmap = (struct vis_t *)mmap(0, sizeof(struct vis_t), PROT_READ | PROT_WRITE, MAP_SHARED, vis_fd, 0);
#else
    vis_mmap = (struct vis_t *)mmap(0, sizeof(struct vis_t), PROT_READ, MAP_SHARED, vis_fd, 0);
#endif
	if (vis_mmap == MAP_FAILED) {
        perror("mmap failed");
        return false;
	}

	return true;
}

// outputs a frame to stdout
static void output(uint8_t frame[HEIGHT][WIDTH][3], int size)
{
    write(1, frame, size);
}

#define MIN(x,y) ((x)<(y)?(x):(y))
#define MAX(x,y) ((x)>(y)?(x):(y))

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef struct {
    rgb_t   c[17];
} palet_t;

// fixes an offset x in the visualisation buffer to the range 0..VIS_BUF_SIZE-1
static int fix_offset(int offset)
{
    offset = (offset + VIS_BUF_SIZE) % VIS_BUF_SIZE;
    if (offset < 0) {
        offset += VIS_BUF_SIZE;
    }
    return offset;
}

// draws a waveform pixel, clipping the coordinate and saturating the colour as needed
static void draw_pixel(uint8_t frame[HEIGHT][WIDTH], int sample, int x)
{
    int h;
    
    h = (HEIGHT + sample - 1) / 2;
    h = MAX(h, 0);
    h = MIN(h, HEIGHT - 1);
    
    frame[h][x]++;
}

// finds the piece of audio in buf that best matches the audio in prv
static int find_match(double *prv, double *buf)
{
    int i, j;
    double sum;
    double sum_max = 0;
    int shift = 0;
    // iterate over all shifts
    for (i = 0; i < BUF_SIZE; i++) {
        // integrate for cross-correlation
        sum = 0;
        for (j = 0; j < BUF_SIZE; j += 16) {
            sum += prv[j] * buf[i + j];
        }
        // keep track of max correlation
        if (sum > sum_max) {
            sum_max = sum;
            shift = i;
        }
    }
    return shift;
}

// render one pixel from intensity to an RGB value
static void render_pixel(palet_t *palet, int i, uint8_t pixel[3])
{
    pixel[0] = palet->c[i].r;
    pixel[1] = palet->c[i].g;
    pixel[2] = palet->c[i].b;
}

// draws a waveform
static double draw_wave(uint8_t frame[HEIGHT][WIDTH][3], double *buf, palet_t *palet, double rms_avg)
{
    static double prv[BUF_SIZE];
    uint8_t intensity[HEIGHT][WIDTH];

    // find best shift that matches the previous waveform to the current one
    int shift;
    shift = find_match(prv, buf);
    
    // copy matched buffer
    int j;
    for (j = 0; j < BUF_SIZE; j++) {
        prv[j] = buf[j + shift];
    }

    // draw as intensity map
    int h;
    double m;
    int i;
    memset(intensity, 0, sizeof(intensity));
    double scale = 3.0 / rms_avg;
    for (i = 0; i < BUF_SIZE; i++) {
        m = prv[i];
        h = m * scale;
        draw_pixel(intensity, h, i / 16);
    }
    
    // render intensity to color
    int x, y;
    for (y = 0; y < HEIGHT; y++) {
        for (x = 0; x < WIDTH; x++) {
            render_pixel(palet, intensity[y][x], frame[y][x]);
        }
    }

    // calculate RMS of left and right signal
    double sum = 0.0;
    for (j = 0; j < BUF_SIZE; j++) {
        m = prv[j];
        sum += (m * m);
    }
    double rms = sqrt(sum / BUF_SIZE);
    return rms;
}

static uint8_t banner[HEIGHT][WIDTH][3];
static double buffer[2*BUF_SIZE];

// limits x to the range [min,max]
static int limit(int x, int min, int max)
{
    if (x < min) {
        return min;
    }
    if (x > max) {
        return max;
    }
    return x;
}

// creates a smooth fading palet
static void create_palet(palet_t *pal, rgb_t col, double scale)
{
    int i;
    double r, g, b;
    for (i = 0; i < 17; i++) {
        r = scale * col.r * i / 16;
        g = scale * col.g * i / 16;
        b = scale * col.b * i / 16;
        pal->c[i].r = limit(r, 0, 255);
        pal->c[i].g = limit(g, 0, 255);
        pal->c[i].b = limit(b, 0, 255);
   }
}

// argv[1] = name of /dev/shm file created by squeezelite
// argv[2] = number of seconds to run (if not present: forever)
int main(int argc, char *argv[])
{
    time_t now;
    time_t then = time(NULL);
    int fps = 0;
    
    double rms_avg = 1.0;

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
    
    // create a palet
    palet_t palet;
    srand(time(NULL));
    uint8_t r = random() & 255;
    uint8_t g = random() & 255;
    uint8_t b = random() & 255;
    create_palet(&palet, (rgb_t){r, g, b}, 1000.0 / (r + g + b + 1));
    
    u32_t buf_index = 0;
    
    while (vis_mmap->running) {
#ifdef USE_LOCKS
        // lock
        pthread_rwlock_rdlock(&vis_mmap->rwlock);
#endif

        // check for data available
        int avail = fix_offset(vis_mmap->buf_index - buf_index);
        bool have_new_data = (avail >= AUDIO_FRAME);
        if (have_new_data) {
            // unwrap buffer, convert to mono double
            int i;
            for (i = 0; i < (2 * AUDIO_FRAME); i += 2) {
                int index = fix_offset(buf_index + i - AUDIO_FRAME);
                buffer[i / 2] = (vis_mmap->buffer[index] + vis_mmap->buffer[index + 1]) / 2;
            }
            // update our read index
            buf_index = fix_offset(buf_index + AUDIO_FRAME);
        }

#ifdef USE_LOCKS
        // unlock
        pthread_rwlock_unlock(&vis_mmap->rwlock);
#endif

        // update led banner
        if (have_new_data) {
            memset(banner, 0, sizeof(banner));
            double rms = draw_wave(banner, buffer, &palet, rms_avg);

            // smooth rms over time
            rms_avg += (rms - rms_avg) / 64.0;

            output(banner, sizeof(banner));
            fps++;
        }
        
        // stats
        now = time(NULL);
        if (now != then) {
            fprintf(stderr, "fps=%d, rms=%.6f\n", fps, rms_avg);
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

