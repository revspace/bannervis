#include <stdint.h>
#include <stdbool.h>

#include <pthread.h>
#include <time.h>

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

