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

// whether to use the pthread lock
//#define USE_LOCKS

typedef uint32_t u32_t;
typedef int16_t s16_t;

// copied from squeezelite
#define VIS_BUF_SIZE 16384
#define VIS_LOCK_NS  1000000 // ns to wait for vis wrlock

static struct vis_t {
	pthread_rwlock_t rwlock;
	u32_t buf_size;
	u32_t buf_index;
	bool running;
	u32_t rate;
	time_t updated;
	s16_t buffer[VIS_BUF_SIZE];
} *vis_mmap = NULL;

// led banner definitions
#define WIDTH 80
#define HEIGHT 8

// number of audio samples used for one video frame
#define AUDIO_FRAME   (16*WIDTH*2)

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

// outputs a frame to stdout
static void output(uint8_t frame[HEIGHT][WIDTH][3], int size)
{
    write(1, frame, size);
}

#define MIN(x,y) ((x)<(y)?(x):(y))
#define MAX(x,y) ((x)>(y)?(x):(y))

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
static void draw_pixel(uint8_t frame[HEIGHT][WIDTH][3], int sample, int scale, int x, int y, int r, int g, int b)
{
    int h;
    
    if (scale != 0) {
        sample = (sample * 512) / scale;
    } else {
        sample /= 512;
    }
    h = (HEIGHT + sample - 1) / 2 + y;
    h = MAX(h, 0);
    h = MIN(h, HEIGHT - 1);
    int rr = frame[h][x][0] + r;
    int gg = frame[h][x][1] + g;
    int bb = frame[h][x][2] + b;
    
    frame[h][x][0] = MIN(rr, 255);
    frame[h][x][1] = MIN(gg, 255);
    frame[h][x][2] = MIN(bb, 255);
}

// finds the piece of audio in buf that best matches the audio in prv
static int find_match(s16_t *prv, s16_t *buf)
{
    int i, j;
    long int sum;
    long int sum_max = 0;
    int shift = 0;
    int m1, m2;
    // iterate over all shifts
    for (i = 0; i < AUDIO_FRAME; i += 2) {
        // integrate for cross-correlation
        sum = 0;
        for (j = 0; j < AUDIO_FRAME; j += 32) {
            m1 = prv[j] + prv[j + 1];
            m2 = buf[j + i] + buf[j + i + 1];
            sum += (m1 * m2);
        }
        // keep track of max correlation
        if (sum > sum_max) {
            sum_max = sum;
            shift = i;
        }
    }
    return shift;
}

// draws a waveform
static int draw_wave(uint8_t frame[HEIGHT][WIDTH][3], s16_t *buf, int scale)
{
    static s16_t prv[AUDIO_FRAME];

    // find best shift that matches the previous waveform to the current one
    int shift;
    shift = find_match(prv, buf);
    
    // copy matched buffer
    int j;
    for (j = 0; j < AUDIO_FRAME; j += 2) {
        prv[j] = buf[j + shift];
        prv[j + 1] = buf[j + shift + 1];
    }

    // draw
    int l, r, m;
    int i;
    for (i = 0; i < AUDIO_FRAME; i += 2) {
        l = prv[i];
        r = prv[i + 1];
        m = r + l;
        
        draw_pixel(frame, m, scale, i / 32, 0, 15, 30, 15);
    }

    // calculate RMS of left and right signal
    long int sum = 0;
    for (j = 0; j < AUDIO_FRAME; j++) {
        m = prv[j];
        sum += (m * m);
    }
    int rms = sqrt(sum / AUDIO_FRAME);
    return rms;
}

static uint8_t banner[HEIGHT][WIDTH][3];
static s16_t buffer[VIS_BUF_SIZE];

// argv[1] = name of /dev/shm file created by squeezelite
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    
    time_t now;
    time_t then = time(NULL);
    int fps = 0;
    
    int rms_avg = 1;

    // mmap file
    const char *filename = "/dev/shm/squeezelite-00:21:00:02:cc:45";
    if (argc > 1) {
        filename = argv[1];
    }
    do_mmap(filename);
    
    u32_t buf_index = 0;
    
    while (1) {
#ifdef USE_LOCKS
        // lock
        pthread_rwlock_rdlock(&vis_mmap->rwlock);
#endif

        // check for data available
        int avail = fix_offset(vis_mmap->buf_index - buf_index);
        bool have_new_data = (avail >= AUDIO_FRAME);
        if (have_new_data) {
            // unwrap buffer
            int i;
            for (i = 0; i < VIS_BUF_SIZE; i++) {
                buffer[i] = vis_mmap->buffer[fix_offset(buf_index + i - AUDIO_FRAME)];
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
            int rms = 256 * draw_wave(banner, buffer, rms_avg);

            // smooth rms over time
            rms_avg += (rms - rms_avg + 16) / 32;

            output(banner, sizeof(banner));
            fps++;
        }
        
        // stats
        now = time(NULL);
        if (now != then) {
            fprintf(stderr, "fps=%d, rms=%6d\n", fps, rms_avg);
            then = now;
            fps = 0;
        }
        
        // wait some time
        usleep(1000);
    }

    return 0;
}

