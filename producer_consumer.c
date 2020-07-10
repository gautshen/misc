#define _GNU_SOURCE
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/time.h>
#include <limits.h>
#include <sys/shm.h>
#include <linux/futex.h>
#include <sys/ioctl.h>
#include "perf_event.h"


#undef DEBUG

#ifdef DEBUG
#define debug_printf(fmt...)    printf(fmt)
#else
#define debug_printf(fmt...)
#endif

#define INDEX_ARRAY_SIZE (1024) //1k
#define DATA_ARRAY_SIZE  (1024*1024*512) //512M

/*********************** Perf related stuff ************************
 *
 * From : https://ozlabs.org/~anton/junkcode/perf_events_example1.c
 *
 ********************************************************************/

#define USERSPACE_ONLY

#ifndef __NR_perf_event_open
#if defined(__PPC__)
#define __NR_perf_event_open	319
#elif defined(__i386__)
#define __NR_perf_event_open	336
#elif defined(__x86_64__)
#define __NR_perf_event_open	298
#else
#error __NR_perf_event_open must be defined
#endif
#endif

static inline int sys_perf_event_open(struct perf_event_attr *attr, pid_t pid,
				      int cpu, int group_fd,
				      unsigned long flags)
{
	attr->size = sizeof(*attr);
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int cache_refs_fd;
static int cache_miss_fd;

static void setup_counters(void)
{
	struct perf_event_attr attr;

	memset(&attr, 0, sizeof(attr));
#ifdef USERSPACE_ONLY
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	attr.exclude_idle = 1;
#endif

	attr.disabled = 1;
	attr.type = PERF_TYPE_HARDWARE;
	attr.config = PERF_COUNT_HW_CACHE_REFERENCES;
	cache_refs_fd = sys_perf_event_open(&attr, 0, -1, -1, 0);
	if (cache_refs_fd < 0) {
		perror("sys_perf_event_open");
		exit(1);
	}

	/*
	 * We use cache_refs_fd as the group leader in order to ensure
	 * both counters run at the same time and our CPI statistics are
	 * valid.
	 */
	attr.disabled = 0; /* The group leader will start/stop us */
	attr.type = PERF_TYPE_HARDWARE;
	attr.config = PERF_COUNT_HW_CACHE_MISSES;
	cache_miss_fd = sys_perf_event_open(&attr, 0, -1, cache_refs_fd, 0);
	if (cache_miss_fd < 0) {
		perror("sys_perf_event_open");
		exit(1);
	}
}

static void start_counters(void)
{
	/* Only need to start and stop the group leader */
	ioctl(cache_refs_fd, PERF_EVENT_IOC_ENABLE);
}

static void stop_counters(void)
{
	ioctl(cache_refs_fd, PERF_EVENT_IOC_DISABLE);
}

static void reset_counters(void)
{
	ioctl(cache_refs_fd, PERF_EVENT_IOC_RESET);
	ioctl(cache_miss_fd, PERF_EVENT_IOC_RESET);
}


unsigned long iterations;
unsigned long iterations_prev;

unsigned long long consumer_time_ns;
unsigned long long consumer_time_ns_prev;

unsigned long long cache_refs_total;
unsigned long long cache_refs_total_prev;

unsigned long long cache_miss_total;
unsigned long long cache_miss_total_prev;

static void read_counters(void)
{
	size_t res;
	unsigned long long cache_refs;
	unsigned long long cache_misses;

	res = read(cache_refs_fd, &cache_refs, sizeof(unsigned long long));
	assert(res == sizeof(unsigned long long));

	res = read(cache_miss_fd, &cache_misses, sizeof(unsigned long long));
	assert(res == sizeof(unsigned long long));

	cache_refs_total += cache_refs;
	cache_miss_total += cache_misses;
}

static unsigned int timeout = 100;

static void sigalrm_handler(int junk)
{
	unsigned long i = iterations;
	unsigned long long j = consumer_time_ns;
	unsigned long iter_diff = i - iterations_prev;
	unsigned long long time_ns_diff = j - consumer_time_ns_prev;
	unsigned long long avg_time_ns = (time_ns_diff) / iter_diff;
	unsigned long long k = cache_refs_total;
	unsigned long long l = cache_miss_total;
	unsigned long long cache_ref_diff = k - cache_refs_total_prev;
	unsigned long long avg_cache_ref_diff = cache_ref_diff/iter_diff;
	unsigned long long cache_miss_diff = l - cache_miss_total_prev;
	unsigned long long avg_cache_miss_diff = cache_miss_diff/iter_diff;
	float cache_miss_pct = ((float) cache_miss_diff * 100)/cache_ref_diff;

	printf("%8ld iterations, avg time:%6lld ns, avg cache-ref:%6lld, avg cache-miss:%6lld (cache-miss rate %3.2f \%)\n",
		iter_diff, avg_time_ns, avg_cache_ref_diff, avg_cache_miss_diff,
	        cache_miss_pct);

	iterations_prev = i;
	consumer_time_ns_prev = j;
	cache_refs_total_prev = k;
	cache_miss_total_prev = l;

	if (--timeout == 0)
		kill(0, SIGUSR1);

	alarm(1);
}

static void sigusr1_handler(int junk)
{
	exit(0);
}


#define READ 0
#define WRITE 1

static int pipe_fd1[2];
static int pipe_fd2[2];

char c;


struct data_args {
	unsigned long idx_arr_size;
	unsigned long data_arr_size;
	unsigned long *index_array;
	unsigned long *data_array;
};

static void *producer(void *arg)
{
	int i;
	struct data_args *p = (struct data_args *) arg;
	unsigned long idx_arr_size = p->idx_arr_size;
	unsigned long data_arr_size = p->data_arr_size;
	unsigned long *index_array = p->index_array;
	unsigned long *data_array = p->data_array;
	pthread_t thread = pthread_self();
        cpu_set_t cpuset;

	CPU_ZERO(&cpuset);
	pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

	for ( i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &cpuset)) 
			printf("Producer affined to  CPU %d\n", i);
	}

	debug_printf("Producer : idx_array_size = %ld,  data_array_size = %ld\n",
		idx_arr_size, data_arr_size);
	debug_printf("Producer : idx_array = 0x%llx,  data_array = 0x%llx\n",
		index_array, data_array);
	
	signal(SIGALRM, sigalrm_handler);
	alarm(1);

	while (1) {

		debug_printf("Producer while begin\n");
		/*
		 * We will write to p->idx_array_size random locations within
		 * the p->data_array. We record where we have written in
		 * p->index_array
		 */
		for (i = 0; i < idx_arr_size; i++) {
			unsigned long idx;
			unsigned long data;

			idx = random() % data_arr_size;
			idx += 128;
			data = random() % UINT_MAX;

			debug_printf("Producer : [%d] = %ld,  [%ld] = 0x%llx\n",
				i, idx, idx, data);
			index_array[i] = idx;
			data_array[idx] = data;
		}

		debug_printf("Producer writing to pipe\n");
		assert(write(pipe_fd2[WRITE], &c, 1) == 1);

		debug_printf("Producer waiting\n");
		assert(read(pipe_fd1[READ], &c, 1) == 1);
		debug_printf("Producer read from pipe\n");
	}

	return NULL;
}

static unsigned long long compute_timediff(struct timespec before, struct timespec after)
{
	unsigned long long ret_ns;
	unsigned long long ns_per_sec = 1000UL*1000*1000;

	if (after.tv_sec == before.tv_sec) {
		ret_ns = after.tv_nsec - before.tv_nsec;

		return ret_ns;
	}

	if (after.tv_sec > before.tv_sec) {
		unsigned long long diff_ns = 0;

		diff_ns = (after.tv_sec - before.tv_sec) * ns_per_sec;
		ret_ns = diff_ns + after.tv_nsec - before.tv_nsec;
		return ret_ns;
	}

	return 0;
}

static void *consumer(void *arg)
{
	int i;
	struct data_args *con = (struct data_args *) arg;
	unsigned long idx_arr_size = con->idx_arr_size;
	unsigned long data_arr_size = con->data_arr_size;
	unsigned long *index_array = con->index_array;
	unsigned long *data_array = con->data_array;
	pthread_t thread = pthread_self();
        cpu_set_t cpuset;

	CPU_ZERO(&cpuset);
	pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset);


	for (i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &cpuset)) 
			printf("Consumer affined to  CPU %d\n", i);
	}

	debug_printf("Consumer : idx_array_size = %ld,  data_array_size = %ld\n",
		idx_arr_size, data_arr_size);
	debug_printf("Consumer : idx_array = 0x%llx,  data_array = 0x%llx\n",
		index_array, data_array);

	setup_counters();
	while (1) {
		unsigned long idx = 0;
		volatile unsigned int sum = 0;
		struct timespec begin, end;
		unsigned long long time_diff_ns;
		const unsigned long long ns_per_msec = 1000*1000;

		debug_printf("Consumer While begin\n");
		debug_printf("Consumer waiting\n");
		assert(read(pipe_fd2[READ], &c, 1) == 1);
		debug_printf("Consumer read from pipe\n");
		debug_printf("Consume idx_arr_size = %ld\n", idx_arr_size);

		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &begin);
		start_counters();
		for (i = 0; i < idx_arr_size; i++) {
			unsigned long idx;
			unsigned long data;

			idx = index_array[i];
			data = data_array[idx];

			debug_printf("Consumer : [%d] = %ld,  [%ld] = 0x%llx\n",
				i, idx, idx, data);
			sum = (sum + data) % INT_MAX;
		}
		stop_counters();
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);

		time_diff_ns = compute_timediff(begin, end);
		if (time_diff_ns > ns_per_msec) {
			printf("========= WARNING !!!! ===================\n");
			printf("Begin = %10ld.%09ld ns\n", begin.tv_sec, end.tv_nsec);
			printf("End   = %10ld.%09ld ns\n", end.tv_sec, end.tv_nsec);
		        printf("Diff  = %10lld ns\n", time_diff_ns);
			printf("========= END WARNING !!!! ===============\n");
			goto update_done;
		}
		iterations++;
		consumer_time_ns += time_diff_ns;
		read_counters();
		reset_counters();
update_done:
		idx = 0;
		debug_printf("Consumer writing [%ld] = 0x%llx\n", idx, sum);
		data_array[idx] = sum;

		debug_printf("Consumer writing to pipe\n");
		assert(write(pipe_fd1[WRITE], &c, 1) == 1);
	}

	return NULL;
}


int main(int argc, char *argv[])
{
	signed char c;
	pthread_t producer_tid, consumer_tid;	
	pthread_attr_t producer_attr, consumer_attr;
	struct data_args producer_args, consumer_args;
	unsigned long *idx_arr, *data_arr;
	int idx_arr_size = INDEX_ARRAY_SIZE;
	int data_arr_size =  DATA_ARRAY_SIZE;
	int cpu_producer = -1, cpu_consumer = -1;
	unsigned long seed = 6407741;
	cpu_set_t cpuset;
	

	if (argc == 2 || 4) {
		seed = (unsigned int) strtoul(argv[1], NULL, 10);
	}

	if (argc == 4) {
		cpu_producer = (unsigned int) strtoul(argv[2], NULL, 10);
		cpu_consumer = (unsigned int) strtoul(argv[3], NULL, 10);
	}
	
	srandom(seed);
	if (pipe(pipe_fd1) || pipe(pipe_fd2)) {
		printf("Error creating pipes\n");
		exit(1);
	}

	
	idx_arr = malloc(idx_arr_size * sizeof(unsigned long));

	if (!idx_arr) {
		printf("Not enough memory for allocating an index array\n");
		exit(1);
	}

	producer_args.idx_arr_size = consumer_args.idx_arr_size = idx_arr_size;
	producer_args.index_array = idx_arr;
	consumer_args.index_array = idx_arr;

	data_arr = malloc(data_arr_size * sizeof(unsigned long));
	if (!data_arr) {
		printf("Not enough memory for allocating an data array\n");
		exit(1);
	}

	producer_args.data_arr_size = consumer_args.data_arr_size = data_arr_size;
	producer_args.data_array = data_arr;
	consumer_args.data_array = data_arr;

	setpgid(getpid(), getpid());
	signal(SIGUSR1, sigusr1_handler);

	pthread_attr_init(&producer_attr);
	if (cpu_producer != -1) {
		CPU_ZERO(&cpuset);
		CPU_SET(cpu_producer, &cpuset);
		if (pthread_attr_setaffinity_np(&producer_attr,
						sizeof(cpu_set_t),
						&cpuset)) {
			perror("Error setting affinity for producer");
			exit(1);
		}
		printf("Affined producer to CPU %d\n", cpu_producer);
	}
	
	if (pthread_create(&producer_tid,
			   &producer_attr, producer, &producer_args)) {
		printf("Error creating the producer\n");
		exit(1);
	}

	pthread_attr_init(&consumer_attr);
	if (cpu_consumer != -1) {
		CPU_ZERO(&cpuset);
		CPU_SET(cpu_consumer, &cpuset);
		if (pthread_attr_setaffinity_np(&consumer_attr,
						sizeof(cpu_set_t),
						&cpuset)) {
			printf("Error setting affinity for consumer");
			exit(1);
		}
		printf("Affined consumer to CPU %d\n", cpu_producer);
	}

	if (pthread_create(&consumer_tid,
			   &consumer_attr, consumer, &consumer_args)) {
		printf("Error creating the consumer\n");
		exit(1);
	}

	while (1)
		sleep(3600);

	return 0;
}
