#include <stdint.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <pthread.h>

#include <stdio.h>
#include <sys/mman.h>

#include <unistd.h> // write

#include <stdlib.h> // exit
#include <string.h> // memcpy

#include <math.h>   // sqrt

#include "squeeze_vis.h"

// whether to use the pthread lock
//#define USE_LOCKS

// led banner definitions
#define WIDTH 80
#define HEIGHT 8

// mmap the file
static void do_mmap(const char *filename)
{
    int vis_fd;
    
#if USE_LOCKS
    vis_fd = open(filename, O_RDWR, 0666);
#else
    vis_fd = open(filename, O_RDONLY, 0);
#endif
    if (vis_fd <= 0) {
        perror("open failed!\n");
        exit(-1);
    }

#if USE_LOCKS
    vis_mmap = (struct vis_t *)mmap(0, sizeof(struct vis_t), PROT_READ | PROT_WRITE, MAP_SHARED, vis_fd, 0);
#else
    vis_mmap = (struct vis_t *)mmap(0, sizeof(struct vis_t), PROT_READ, MAP_SHARED, vis_fd, 0);
#endif
	if (vis_mmap == MAP_FAILED) {
	    perror("mmap failed!\n");
        exit(-1);
	}
}

// calculates rms values for left and right channel (0..32768)
static void calc_rms(s16_t *buf, int samples, int *rms_l, int *rms_r)
{
    s16_t l,r;
    int i;
    int suml = 0;
    int sumr = 0;
    for (i = 0; i < samples; i += 2) {
        l = buf[i];
        r = buf[i+1];
        suml += (l * l) >> 16;
        sumr += (r * r) >> 16;
    }
    if (samples > 0) {
        suml /= samples;
        sumr /= samples;
    } else {
        suml = 0;
        sumr = 0;
    }
    
    *rms_l = sqrt(suml << 8);
    *rms_r = sqrt(sumr << 8);
}

// outputs a frame to stdout
static void output(uint8_t frame[HEIGHT][WIDTH][3], int size)
{
    write(1, frame, size);
}

#define MIN(x,y) ((x)<(y)?(x):(y))
#define MAX(x,y) ((x)>(y)?(x):(y))

// draws a vu meter pixel
static void vu_pixel(uint8_t frame[HEIGHT][WIDTH][3], int x, int c)
{
    x = MAX(x, 2);
    x = MIN(x, (WIDTH - 3));

    int r,g,b;
    if (c < 25) {
        r = 0;
        g = 8 * c;
        b = 0;
    } else if (c < 50) {
        r = (c - 25) * 10;
        g = 200;
        b = 0;
    } else if (c < 75) {
        r = 250;
        g = 200 - (c - 50) * 8;
        b = 0;
    } else {
        r = 255;
        g = 0;
        b = 0;
    }

    int y;
    for (y = 2; y < 6; y++) {
        frame[y][x][0] = r;
        frame[y][x][1] = g;
        frame[y][x][2] = b;
    }
}

// maps rms value to bitmap size
static int map(int rms)
{
    int v = rms / 8;
    if (v >= (WIDTH-1)) {
        v = WIDTH-1;
    }
    return v;
}

struct peak_t {
    int level;
    int time;
};

// calculates the peak value
static void calc_peak(struct peak_t *p, int level)
{
    if (level > p->level) {
        p->level = level;
        p->time = 50;
    } else {
        if (p->time > 0) {
            p->time--;
        } else {
            if (p->level > 0) {
                p->level--;
            }
        }
    }
}

// draw a dual VU
static void draw_vu(uint8_t frame[HEIGHT][WIDTH][3], int l, int r)
{
    int i;
    int x, y;
    static struct peak_t peak_l;
    static struct peak_t peak_r;

    // blue line around VU
    memset(frame, 0, WIDTH*HEIGHT*3);
    for (x = 0; x < WIDTH; x++) {
        frame[0][x][2] = 0xFF;
        frame[HEIGHT - 1][x][2] = 0xFF;
    }
    for (y = 0; y < HEIGHT; y++) {
        frame[y][0][2] = 0xFF;
        frame[y][WIDTH - 1][2] = 0xFF;
    }

    // left VU bar
    int il = map(l);
    for (i = 0; i < il; i++) {
        x = (WIDTH - i - 1) / 2;
        vu_pixel(frame, x, i);
    }
    // right VU bar
    int ir = map(r);
    for (i = 0; i < ir; i++) {
        x = (WIDTH + i + 1) / 2;
        vu_pixel(frame, x, i);
    }
    
    // left peak indicator
    calc_peak(&peak_l, il);
    vu_pixel(frame, (WIDTH - peak_l.level - 1) / 2, 1000);

    // right peak indicator
    calc_peak(&peak_r, ir);
    vu_pixel(frame, (WIDTH + peak_r.level + 1) / 2, 1000);
}

// argv[1] = name of /dev/shm file created by squeezelite
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    
    uint8_t banner[HEIGHT][WIDTH][3];

    // mmap file
    const char *filename = "/dev/shm/squeezelite-00:21:00:02:cc:45";
    if (argc > 1) {
        filename = argv[1];
    }
    do_mmap(filename);
    
    u32_t buf_index = 0;
    
    int rms_l, rms_r;
    int l = 0;
    int r = 0;
    while (1) {
#ifdef USE_LOCKS
        // lock
        pthread_rwlock_rdlock(&vis_mmap->rwlock);
#endif

        // process
        bool have_new_data = (vis_mmap->buf_index != buf_index);
        if (have_new_data) {
            buf_index = vis_mmap->buf_index;
            // calculate rms over buffer
            calc_rms(vis_mmap->buffer, vis_mmap->buf_size / 2, &rms_l, &rms_r);
            // average rms value
            l += (rms_l - l) / 2;
            r += (rms_r - r) / 2;
        }
        
#ifdef USE_LOCKS
        // unlock
        pthread_rwlock_unlock(&vis_mmap->rwlock);
#endif

        // update led banner
        if (have_new_data) {
            draw_vu(banner, l, r);
            output(banner, sizeof(banner));
        }
        
        // wait some time
        usleep(10000);
    }

    return 0;
}

