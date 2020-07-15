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
#include <sys/cdefs.h>
#include <limits.h>
#include <sys/shm.h>
#include <linux/futex.h>
#include <sys/ioctl.h>
#include "perf_event.h"


#undef DEBUG
#define USE_L2_L3
#ifdef DEBUG
#define debug_printf(fmt...)    printf(fmt)
#else
#define debug_printf(fmt...)
#endif

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

#if defined(__PPC__) && defined(USE_L2_L3)
/*
 * We will use the generic PERF_COUNT_HW_CACHE_REFERENCES for L1
 * references on POWER and PERF_COUNT_HW_CACHE_MISSES for L1 D cache
 * misses
 */

#define PM_LD_REF_L1		0x100fc
#define PM_LD_MISS_L1_FIN	0x2c04e

#define PM_DATA_FROM_L2		0x1c042
#define PM_DATA_FROM_L2MISS	0x200fe

#define PM_DATA_FROM_L3		0x4c042
#define PM_DATA_FROM_L3MISS	0x300fe
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

static int l2_cache_hits_fd;
static int l2_cache_miss_fd;

static int l3_cache_hits_fd;
static int l3_cache_miss_fd;

static int setup_counter(const char *name,
			 unsigned char disabled,
			 unsigned int type,
			 unsigned long long config,
			 int group_fd)
{
	struct perf_event_attr attr;
	int fd;

	memset(&attr, 0, sizeof(attr));
#ifdef USERSPACE_ONLY
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	attr.exclude_idle = 1;
#endif

	attr.disabled = disabled;
	attr.type = type;
	attr.config = config;

	fd = sys_perf_event_open(&attr, 0, -1, group_fd, 0);
	if (fd < 0) {
		char err_str[100];

		sprintf(err_str, "%s: sys_perf_event_open\n", name);
		perror((const char *)err_str);
		exit(1);
	}

	return fd;
}

static void setup_counters(void)
{

	cache_refs_fd = setup_counter("cache_refs", 1, PERF_TYPE_HARDWARE,
				      PERF_COUNT_HW_CACHE_REFERENCES, -1);

	cache_miss_fd = setup_counter("cache_miss", 0, PERF_TYPE_HARDWARE,
				      PERF_COUNT_HW_CACHE_MISSES, cache_refs_fd);
#if defined(__PPC__) && defined(USE_L2_L3)

	l2_cache_hits_fd = setup_counter("l2_hits", 1, PERF_TYPE_RAW,
					PM_DATA_FROM_L2, -1);
	l2_cache_miss_fd = setup_counter("l2_miss", 0, PERF_TYPE_RAW,
					PM_DATA_FROM_L2MISS, l2_cache_hits_fd);
	l3_cache_hits_fd = setup_counter("l3_hits", 1, PERF_TYPE_RAW,
					PM_DATA_FROM_L3, -1);

	l3_cache_miss_fd = setup_counter("l3_miss", 0, PERF_TYPE_RAW,
					PM_DATA_FROM_L3MISS, l3_cache_hits_fd);
	printf("Using PM_DATA_FROM_L2 for L2 Hits = 0x%x\n",
	       PM_DATA_FROM_L2);
	printf("Using PM_DATA_FROM_L2MISS for L2-misses = 0x%x\n",
	       PM_DATA_FROM_L2MISS);
	printf("Using PM_DATA_FROM_L3 for L3 Hits = 0x%x\n",
	       PM_DATA_FROM_L3);
	printf("Using PM_DATA_FROM_L3MISS for L3-misses (0x%x)\n",
	       PM_DATA_FROM_L3MISS);
#endif

}

static void start_counters(void)
{
	/* Only need to start and stop the group leader */
	ioctl(cache_refs_fd, PERF_EVENT_IOC_ENABLE);
#if defined(__PPC__) && defined(USE_L2_L3)
	ioctl(l2_cache_hits_fd, PERF_EVENT_IOC_ENABLE);
	ioctl(l3_cache_hits_fd, PERF_EVENT_IOC_ENABLE);
#endif
}

static void stop_counters(void)
{
	ioctl(cache_refs_fd, PERF_EVENT_IOC_DISABLE);
#if defined(__PPC__) && defined(USE_L2_L3)
	ioctl(l2_cache_hits_fd, PERF_EVENT_IOC_DISABLE);
	ioctl(l3_cache_hits_fd, PERF_EVENT_IOC_DISABLE);
#endif
}

static void reset_counters(void)
{
	ioctl(cache_refs_fd, PERF_EVENT_IOC_RESET);
	ioctl(cache_miss_fd, PERF_EVENT_IOC_RESET);
#if defined(__PPC__) && defined(USE_L2_L3)
	ioctl(l2_cache_hits_fd, PERF_EVENT_IOC_RESET);
	ioctl(l2_cache_miss_fd, PERF_EVENT_IOC_RESET);
	ioctl(l3_cache_hits_fd, PERF_EVENT_IOC_RESET);
	ioctl(l3_cache_miss_fd, PERF_EVENT_IOC_RESET);

#endif
}


unsigned long iterations;
unsigned long iterations_prev;

unsigned long long consumer_time_ns;
unsigned long long consumer_time_ns_prev;

unsigned long long cache_refs_total;
unsigned long long cache_refs_total_prev;

unsigned long long cache_miss_total;
unsigned long long cache_miss_total_prev;

unsigned long long l2_cache_hits_total;
unsigned long long l2_cache_hits_total_prev;

unsigned long long l2_cache_miss_total;
unsigned long long l2_cache_miss_total_prev;

unsigned long long l3_cache_hits_total;
unsigned long long l3_cache_hits_total_prev;

unsigned long long l3_cache_miss_total;
unsigned long long l3_cache_miss_total_prev;

static void read_and_add_counter(unsigned int fd, unsigned long long *acc)
{
	size_t res;
	unsigned long long counter;

	res = read(fd,&counter, sizeof(unsigned long long));
	assert(res == sizeof(unsigned long long));

	*acc += counter;
}

static void read_counters(void)
{

	read_and_add_counter(cache_refs_fd, &cache_refs_total);
	read_and_add_counter(cache_miss_fd, &cache_miss_total);

#if defined(__PPC__) && defined(USE_L2_L3)
	read_and_add_counter(l2_cache_hits_fd, &l2_cache_hits_total);
	read_and_add_counter(l2_cache_miss_fd, &l2_cache_miss_total);

	read_and_add_counter(l3_cache_hits_fd, &l3_cache_hits_total);
	read_and_add_counter(l3_cache_miss_fd, &l3_cache_miss_total);
#endif
}

static unsigned int timeout = 5;

enum access_type{
	reference,
	hit,
	miss,
};

static void print_cache_details(const char *name,
				unsigned long long *hit_ref,
				unsigned long long *miss,
				unsigned long long *hit_ref_prev,
				unsigned long long *miss_prev,
				unsigned long iter_diff,
				enum access_type type)
{
	unsigned long cur_hit_ref = *hit_ref;
	unsigned long cur_miss = *miss;
	unsigned long hit_ref_diff = cur_hit_ref - *hit_ref_prev;
	unsigned long miss_diff = cur_miss - *miss_prev;
	float cache_miss_pct = 0;
	unsigned long long avg_hit_ref_diff = 0, avg_miss_diff = 0;
	unsigned long long pct_denominator;
	char *ref_hit_str = "unknown";

	/*
	 * Compute the average number of hits/references and misses
	 * per consumer iteration in this last second.
	 */
	if (iter_diff) {
		avg_hit_ref_diff = hit_ref_diff/iter_diff;
		avg_miss_diff = miss_diff/iter_diff;
	}

	/*
	 * Compute the denominator for determining the cache miss
	 * rate, based on whether the hit_ref data is either a
	 * reference or a hit.
	 */
	if (type == reference) {
		pct_denominator = hit_ref_diff;
		ref_hit_str = "refs";
	} else if (type == hit) {
		pct_denominator = hit_ref_diff + miss_diff;
		ref_hit_str = "hits";
	}

	/* Compute the percentage cache-miss */
	if (pct_denominator)
		cache_miss_pct = ((float)miss_diff * 100)/pct_denominator;

	printf("%s: avg cache-%s: %6lld, avg cache-misses: %6lld, cache-miss rate: %3.2f percentage\n", name, ref_hit_str, avg_hit_ref_diff,
		avg_miss_diff, cache_miss_pct);

	/*
	 * Record the current snapshots of both hits/references and
	 * misses.
	 */
	*hit_ref_prev = cur_hit_ref;
	*miss_prev = cur_miss;
}

static void print_caches(unsigned long iter_diff)
{
	print_cache_details("L1", &cache_refs_total, &cache_miss_total,
			&cache_refs_total_prev, &cache_miss_total_prev,
			iter_diff, reference);
#if defined(__PPC__) && defined(USE_L2_L3)
	print_cache_details("L2", &l2_cache_hits_total, &l2_cache_miss_total,
			&l2_cache_hits_total_prev, &l2_cache_miss_total_prev,
			iter_diff, hit);
	print_cache_details("L3", &l3_cache_hits_total, &l3_cache_miss_total,
			&l3_cache_hits_total_prev, &l3_cache_miss_total_prev,
			iter_diff, hit);
#endif
}

unsigned char stop = 0;

static void sigalrm_handler(int junk)
{
	unsigned long i = iterations;
	unsigned long long j = consumer_time_ns;
	unsigned long iter_diff = i - iterations_prev;
	unsigned long long time_ns_diff = j - consumer_time_ns_prev;
	unsigned long long avg_time_ns = 0;

	if (iter_diff)
		avg_time_ns = (time_ns_diff) / iter_diff;

	printf("%8ld iterations, avg time:%6lld ns\n", iter_diff, avg_time_ns);
	print_caches(iter_diff);

	iterations_prev = i;
	consumer_time_ns_prev = j;

	if (--timeout == 0) {
		kill(0, SIGUSR1);
		return;
	}

	alarm(1);
}

static void sigusr1_handler(int junk)
{
	stop = 1;
}

#undef L1_CONTAINED

#if defined(__PPC__)
#define L1_CACHE_SHIFT		7
#define L1_CACHE_SIZE		(32*1024)  //32K
#define L2_CACHE_SIZE		(512*1024) //512K
#else
#define L1_CACHE_SHIFT	        6
#define L1_CACHE_SIZE		(32*1024)
#define L2_CACHE_SIZE		(256*1024)
#endif

#define	L1_CACHE_BYTES		(1 << L1_CACHE_SHIFT)
#define	SMP_CACHE_BYTES		L1_CACHE_BYTES
#define ____cacheline_aligned __attribute__((__aligned__(SMP_CACHE_BYTES)))

#ifdef L1_CONTAINED
#define CACHE_SIZE		L1_CACHE_SIZE
#else
#define CACHE_SIZE		L2_CACHE_SIZE
#endif

#define INDEX_ARRAY_SIZE (CACHE_SIZE >> L1_CACHE_SHIFT) //
#define DATA_ARRAY_SIZE  (INDEX_ARRAY_SIZE*1024)


#define READ 0
#define WRITE 1

static int pipe_fd1[2];
static int pipe_fd2[2];

char c;

typedef unsigned long long u64;


struct big_data {
	u64 content;
} ____cacheline_aligned;

struct data_args {
	unsigned long idx_arr_size;
	unsigned long data_arr_size;
	unsigned long *index_array;
	struct big_data *data_array;
};

static void cpuset_to_list(cpu_set_t *cpuset, char *str)
{
	int start = -1;
	int end = -2;
	int cur, i;

	for (i = 0; i < CPU_SETSIZE; i++) {
		cur = i;
		if (CPU_ISSET(i, cpuset)) {
			if (end != cur - 1) /* New streak */
				start = cur;
			end = cur;
		} else if (end == cur - 1) {
			int len;
			/* Streak has ended. Print the last streak */
			if (start == end)
				len = sprintf(str, "%d,", start);
			else
				len = sprintf(str, "%d-%d,", start, end);

			str = str + len;
		}

	}

	if (end == CPU_SETSIZE - 1) {
		int len;
		/* Streak has ended. Print the last streak */
		if (start == end)
			len = sprintf(str, "%d,", start);
		else
			len = sprintf(str, "%d-%d,", start, end);

		str = str + len;
	}
}

static void *producer(void *arg)
{
	int i;
	struct data_args *p = (struct data_args *) arg;
	unsigned long idx_arr_size = p->idx_arr_size;
	unsigned long data_arr_size = p->data_arr_size;
	unsigned long *index_array = p->index_array;
	struct big_data *data_array = p->data_array;
	pthread_t thread = pthread_self();
        cpu_set_t cpuset;
	char cpu_list_str[1024];

	CPU_ZERO(&cpuset);
	pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

	cpuset_to_list(&cpuset, cpu_list_str);
	printf("Producer affined to CPUs: %s\n", cpu_list_str);


	debug_printf("Producer : idx_array_size = %ld,  data_array_size = %ld\n",
		idx_arr_size, data_arr_size);
	debug_printf("Producer : idx_array = 0x%llx,  data_array = 0x%llx\n",
		index_array, data_array);
	
	signal(SIGALRM, sigalrm_handler);
	alarm(1);

	while (!stop) {

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
			data = random() % UINT_MAX;

			debug_printf("Producer : [%d] = %ld,  [%ld] = 0x%llx\n",
				i, idx, idx, data);
			index_array[i] = idx;
			data_array[idx].content = data;
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
	struct big_data *data_array = con->data_array;
	pthread_t thread = pthread_self();
        cpu_set_t cpuset;
	char cpu_list_str[1024];

	CPU_ZERO(&cpuset);
	pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

	cpuset_to_list(&cpuset, cpu_list_str);
	printf("Consumer affined to CPUs: %s\n", cpu_list_str);

	debug_printf("Consumer : idx_array_size = %ld,  data_array_size = %ld\n",
		idx_arr_size, data_arr_size);
	debug_printf("Consumer : idx_array = 0x%llx,  data_array = 0x%llx\n",
		index_array, data_array);

	setup_counters();
	while (!stop) {
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
			data = data_array[idx].content;

			debug_printf("Consumer : [%d] = %ld,  [%ld] = 0x%llx\n",
				i, idx, idx, data);
			sum = (sum + data) % INT_MAX;
		}
		stop_counters();
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);

		time_diff_ns = compute_timediff(begin, end);
		if (time_diff_ns > ns_per_msec) {
			debug_printf("========= WARNING !!!! ===================\n");
			debug_printf("Begin = %10ld.%09ld ns\n", begin.tv_sec, end.tv_nsec);
			debug_printf("End   = %10ld.%09ld ns\n", end.tv_sec, end.tv_nsec);
		        debug_printf("Diff  = %10lld ns\n", time_diff_ns);
			debug_printf("========= END WARNING !!!! ===============\n");
			goto update_done;
		}
		iterations++;
		consumer_time_ns += time_diff_ns;
		read_counters();
update_done:
		reset_counters();
		idx = 0;
		debug_printf("Consumer writing [%ld] = 0x%llx\n", idx, sum);
		data_array[idx].content = sum;

		debug_printf("Consumer writing to pipe\n");
		assert(write(pipe_fd1[WRITE], &c, 1) == 1);
	}

	return NULL;
}


int idx_arr_size = INDEX_ARRAY_SIZE;
int data_arr_size =  DATA_ARRAY_SIZE;
int cpu_producer = -1, cpu_consumer = -1;
unsigned long seed = 6407741;
unsigned long cache_size = CACHE_SIZE;
unsigned long *idx_arr;
struct big_data *data_arr;

void print_usage(int argc, char *argv[])
{
	printf("Usage: %s [OPTIONS]\n", argv[0]);
	printf("Following options are available\n");
	printf("-p, --pcpu\t\t\t The CPU to which the producer should be affined\n");
	printf("-c, --ccpu\t\t\t The CPU to which the producer should be affined\n");
	printf("-r, --random-seed\t\t The seed used for random number generation\n");
	printf("-l, --iteration-length\t\t The number of loads per consumer-iteration\n");
	printf("-s, --cache-size\t\t Size of the cache in bytes.\n");
	printf("-x, --timeout\t\t\t Number of seconds to run the benchmark\n");	
	printf("Note : Atmost one of --iteration-length or --cache-size can be provided\n");
}

void parse_args(int argc, char *argv[])
{
	int c;

	int iteration_length_provided = 0;
	int cache_size_provided = 0;

	while(1) {
		static struct option long_options[] = {
			{"pcpu", required_argument, 0, 'p'},
			{"ccpu", required_argument, 0, 'c'},
			{"random-seed", required_argument, 0, 'r'},
			{"iteration-length", required_argument, 0, 'l'},
			{"cache-size", required_argument, 0, 's'},
			{"timeout", required_argument, 0, 't'},
			{"help", no_argument, 0, 'h'},
			{0, 0, 0, 0},
		};

		int option_index = 0;

		c = getopt_long(argc, argv, "hp:c:r:l:s:t:", long_options, &option_index);

		/* Options are done */
		if (c == -1)
			break;
		switch (c) {
		case 'h':
			print_usage(argc, argv);
			exit(0);

		case 'p':
			cpu_producer = (int) strtoul(optarg, NULL, 10);
			break;

		case 'c':
			cpu_consumer = (int) strtoul(optarg, NULL, 10);
			break;

		case 'r':
			seed = strtoul(optarg, NULL, 10);
			break;

		case 'l':
			if (cache_size_provided) {
				printf("Only one of --iteration-length and --cache-size should be provided\n");
				exit(1);
			}

			iteration_length_provided = 1;
			idx_arr_size = strtoul(optarg, NULL, 10);
			break;

		case 's':
			if (iteration_length_provided) {
				printf("Only one of --iteration-length and --cache-size should be provided\n");
				exit(1);
			}
			cache_size_provided = 1;
			cache_size = strtoul(optarg, NULL, 10);
			idx_arr_size = cache_size >> L1_CACHE_SHIFT;
			break;

		case 't':
			timeout = strtoul(optarg, NULL, 10);
			break;

		default:
			printf("Invalid Options\n");
			print_usage(argc, argv);
			exit(1);
		}
	}
}

pthread_t create_thread(const char *name, pthread_attr_t *attr, void *(*fn)(void *),
			struct data_args *args, int cpu)
			/* unsigned long idx_arr_size, unsigned long data_arr_size, */
			/* unsigned long *idx_arr, struct big_data *data_arr) */
{
	pthread_t tid;
	cpu_set_t cpuset;

        args->idx_arr_size = idx_arr_size;
	args->index_array = idx_arr;

	args->data_arr_size = data_arr_size;
	args->data_array = data_arr;

	pthread_attr_init(attr);
	if (cpu != -1) {
		CPU_ZERO(&cpuset);
		CPU_SET(cpu, &cpuset);
		if (pthread_attr_setaffinity_np(attr,
						sizeof(cpu_set_t),
						&cpuset)) {
			perror("Error setting affinity for producer");
			exit(1);
		}
	}

	if (pthread_create(&tid, attr, fn, args)) {
		printf("Error creating the %s\n", name);
		exit(1);
	} else {
		printf("%s created with tid %ld\n", name, tid);
	}

	return tid;
}

int main(int argc, char *argv[])
{
	signed char c;
	pthread_t producer_tid, consumer_tid;	
	pthread_attr_t producer_attr, consumer_attr;
	struct data_args producer_args, consumer_args;

	parse_args(argc, argv);

	if (idx_arr_size * 1024 < DATA_ARRAY_SIZE)
		data_arr_size = idx_arr_size * 1024;

	printf("seed = %ld\n", seed);
	printf("Size of cacheline = %lu bytes\n", sizeof(struct big_data));
	printf("Number of indices in an iteration = %d\n", idx_arr_size);
	printf("Data array size = %d indices x %ld bytes = %ld bytes\n",
	       data_arr_size, sizeof(struct big_data),
	       data_arr_size * sizeof(struct big_data));

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

	printf("idx_arr = 0x%p\n", (void *)idx_arr);

	data_arr = malloc(data_arr_size * sizeof(struct big_data));
	if (!data_arr) {
		printf("Not enough memory for allocating an data array\n");
		exit(1);
	}
	printf("data_arr = 0x%p\n", (void *)data_arr);

	setpgid(getpid(), getpid());
	signal(SIGUSR1, sigusr1_handler);

	producer_tid = create_thread("producer", &producer_attr,
				     producer, &producer_args, cpu_producer);

	consumer_tid = create_thread("consumer", &consumer_attr,
				     consumer, &consumer_args, cpu_consumer);

	pthread_join(producer_tid, NULL);
	pthread_join(consumer_tid, NULL);

	pthread_attr_destroy(&producer_attr);
	pthread_attr_destroy(&consumer_attr);
	free(idx_arr);
	free(data_arr);

	return 0;
}
