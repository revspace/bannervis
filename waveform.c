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

static void draw_pixel(uint8_t frame[HEIGHT][WIDTH][3], int sample, int x, int y, int r, int g, int b)
{
    int h;
    
    h = (HEIGHT - 1 + (sample / 512)) / 2 + y;
    h = MAX(h, 0);
    h = MIN(h, HEIGHT - 1);
    int rr = frame[h][x][0] + r;
    int gg = frame[h][x][1] + g;
    int bb = frame[h][x][2] + b;
    
    frame[h][x][0] = MIN(rr, 255);
    frame[h][x][1] = MIN(gg, 255);
    frame[h][x][2] = MIN(bb, 255);
}

// draw a waveform
static void draw_wave(uint8_t frame[HEIGHT][WIDTH][3], s16_t *buf, int samples)
{

    memset(frame, 0, WIDTH*HEIGHT*3);
    
    int l, r, m;
    int x = 0;
    int t = 0;
    int i;
    for (i = 0; i < samples; i += 2) {
        l = buf[i];
        r = buf[i+1];
        m = r + l;

        x = t++ / 32;
        if (x == WIDTH) {
            break;
        }
        
        draw_pixel(frame, m, x, 0, 10, 20, 30);
//        draw_pixel(frame, l, x, -2, 20, 0, 0);
//        draw_pixel(frame, -d, x, 0, 0, 20, 0);
//        draw_pixel(frame, r, x, +2, 0, 0, 20);
    }
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
            draw_wave(banner, vis_mmap->buffer, vis_mmap->buf_size / 2);
        }
        
#ifdef USE_LOCKS
        // unlock
        pthread_rwlock_unlock(&vis_mmap->rwlock);
#endif

        // update led banner
        if (have_new_data) {
            output(banner, sizeof(banner));
        }
        
        // wait some time
        usleep(5000);
    }

    return 0;
}

