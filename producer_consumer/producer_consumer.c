/*
 * Cache-affinity scheduler wakeup benchmark
 *
 * Build with:
 *
 * gcc -o producer_consumer producer_consumer.c -lpthread
 *
 * Copyright (C) 2020 Gautham R. Shenoy <ego@linux.vnet.ibm.com>, IBM
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */


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
#undef USE_L2_L3

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

#if defined(__PPC__)
#define USE_L2_L3
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

static int verbose = 0;
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
	if (!verbose)
		return;

	/*
	 * For each cache type below, we make the refs/hits fd the
	 * group leader of the corresponding miss fd. Thus, at
	 * runtime, sufficient to enable the group leader. The others
	 * will automatically get enabled.
	 *
	 * During initialization, we keep the group leader disabled.
	 */
	cache_refs_fd = setup_counter("cache_refs", 1, PERF_TYPE_HARDWARE,
				      PERF_COUNT_HW_CACHE_REFERENCES, -1);

	cache_miss_fd = setup_counter("cache_miss", 0, PERF_TYPE_HARDWARE,
				      PERF_COUNT_HW_CACHE_MISSES, cache_refs_fd);
#if defined(USE_L2_L3)

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
	if (!verbose)
		return;
	/* Only need to start the group leader */
	ioctl(cache_refs_fd, PERF_EVENT_IOC_ENABLE);
#if defined(USE_L2_L3)
	ioctl(l2_cache_hits_fd, PERF_EVENT_IOC_ENABLE);
	ioctl(l3_cache_hits_fd, PERF_EVENT_IOC_ENABLE);
#endif
}

static void stop_counters(void)
{
	if (!verbose)
		return;
	/* Only need to stop the group leader */
	ioctl(cache_refs_fd, PERF_EVENT_IOC_DISABLE);
#if defined(USE_L2_L3)
	ioctl(l2_cache_hits_fd, PERF_EVENT_IOC_DISABLE);
	ioctl(l3_cache_hits_fd, PERF_EVENT_IOC_DISABLE);
#endif
}

static void reset_counters(void)
{

	if (!verbose)
		return;

	/* Reset all counters */
	ioctl(cache_refs_fd, PERF_EVENT_IOC_RESET);
	ioctl(cache_miss_fd, PERF_EVENT_IOC_RESET);
#if defined(USE_L2_L3)
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

	if (!verbose)
		return;

	read_and_add_counter(cache_refs_fd, &cache_refs_total);
	read_and_add_counter(cache_miss_fd, &cache_miss_total);

#if defined(USE_L2_L3)
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
	if (!verbose)
		return;

	print_cache_details("L1", &cache_refs_total, &cache_miss_total,
			&cache_refs_total_prev, &cache_miss_total_prev,
			iter_diff, reference);
#if defined(USE_L2_L3)
	print_cache_details("L2", &l2_cache_hits_total, &l2_cache_miss_total,
			&l2_cache_hits_total_prev, &l2_cache_miss_total_prev,
			iter_diff, hit);
	print_cache_details("L3", &l3_cache_hits_total, &l3_cache_miss_total,
			&l3_cache_hits_total_prev, &l3_cache_miss_total_prev,
			iter_diff, hit);
#endif
}

unsigned char stop = 0;

/* We print the statistics of this last second here */
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
		stop = 1;
		return;
	}

	alarm(1);
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

/*
 * Helper function to pretty-print cpuset into list for easy viewing.
 *
 * WARNING: Hasn't been extensively tested.
 */
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

/*
 * Producer function : Performs idx_arr_size number of stores to
 * random locations in the data_array.  These locations are recorded
 * in index_array for the consumer to later access.
 */
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

	/* Wakeup the consumer, just in case! */
	assert(write(pipe_fd2[WRITE], &c, 1) == 1);
	return NULL;
}

/*
 * Helper function to compute the difference between two timespec
 * structures.  The return value is in nanoseconds.
 *
 * WARNING : Have occassionally seen incorrect values when
 * after.tv_sec > before.tv_sec.
 */
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

/*
 * Consumer function : Performs idx_arr_size number of loads from the
 * locations in data_array. These were the ones that producer had
 * written to and are obtained from index_array.
 */
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
		if (stop)
			break;
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

		/* Iteration shouldn't take more than a milli-second */
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

	/* Wakeup the producer, just in case! */
	assert(write(pipe_fd1[WRITE], &c, 1) == 1);
	return NULL;
}


int idx_arr_size = INDEX_ARRAY_SIZE;
int data_arr_size =  DATA_ARRAY_SIZE;
int *cpu_producer, *cpu_consumer;
int producer_count = 0, consumer_count = 0;
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
	printf("-t, --timeout\t\t\t Number of seconds to run the benchmark\n");
	printf("    --verbose\t\t\t Also print the cache-access statistics\n");
	printf("Note : Atmost one of --iteration-length or --cache-size can be provided\n");
}

void parse_args(int argc, char *argv[])
{
	int c;
	char *arg;
	int temp, ind;

	int iteration_length_provided = 0;
	int cache_size_provided = 0;

	while(1) {
		static struct option long_options[] = {
			{"verbose", no_argument, &verbose, 1},
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
		case 0:
			/* If this option set a flag, do nothing else now. */
			if (long_options[option_index].flag != 0)
				break;
			printf("option %s", long_options[option_index].name);
			if (optarg)
				printf(" with arg %s", optarg);
			printf("\n");
			break;
		case 'h':
			print_usage(argc, argv);
			exit(0);

		case 'p':
			ind = 0;
			arg = optarg;
			cpu_producer = malloc(sizeof(int) * 1000);

			if (strchr(arg, ',') == NULL) {
				if (strchr(arg, '-') != NULL) {
					char *token = strtok(arg, "-");
					cpu_producer[ind++] = strtoul(token, NULL, 10);
					token = strtok(NULL, "");
					temp = strtoul(token, NULL, 10);
					for (int i = cpu_producer[ind-1]; i < temp; i++)
						cpu_producer[ind++] = i + 1;
				} else {
					cpu_producer[ind++] = strtoul(arg, NULL, 10);
				}
			} else {
				char *sep;
				char *token = strtok_r(arg, ",", &sep);
				while (token != NULL) {
					if (strchr(token, '-') != NULL) {
						char *sep2;
						char *token2 = strtok_r(token, "-", &sep2);
						cpu_producer[ind++] = strtoul(token2, NULL, 10);
						token2 = strtok_r(NULL, "", &sep2);
						temp = strtoul(token2, NULL, 10);
						for (int i = cpu_producer[ind-1]; i < temp; i++)
							cpu_producer[ind++] = cpu_producer[ind-1] + 1;
					} else {
						cpu_producer[ind++] = strtoul(token, NULL, 10);
					}
				token = strtok_r(NULL, ",", &sep);
				}
			}
			producer_count = ind;
			break;

		case 'c':
			ind = 0;
			arg = optarg;
			cpu_consumer = malloc(sizeof(int) * 1000);

			if (strchr(arg, ',') == NULL) {
				if (strchr(arg, '-') != NULL) {
					char *token = strtok(arg, "-");
					cpu_consumer[ind++] = (int) atoi(token);
					token = strtok(NULL, "");
					temp = strtoul(token, NULL, 10);
					for (int i = cpu_consumer[ind-1]; i < temp; i++)
						cpu_consumer[ind++] = cpu_consumer[ind-1] + 1;
				} else {
					cpu_consumer[ind++] = (int) atoi(arg);
				}
			} else {
				char *sep;
				char *token = strtok_r(arg, ",", &sep);
				while (token != NULL) {
					if (strchr(token, '-') != NULL) {
						char *sep2;
						char *token2 = strtok_r(token, "-", &sep2);
						cpu_consumer[ind++] = (int) atoi(token2);
						token2 = strtok_r(NULL, "", &sep2);
						temp = strtoul(token2, NULL, 10);
						for (int i = cpu_consumer[ind-1]; i < temp; i++)
							cpu_consumer[ind++] = cpu_consumer[ind-1] + 1;
					} else {
						cpu_consumer[ind++] = (int) atoi(token);
					}
				token = strtok_r(NULL, ",", &sep);
				}
			}
			consumer_count = ind;
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
	} else if (verbose) {
		printf("%s created with tid %ld\n", name, tid);
	}

	return tid;
}

int main(int argc, char *argv[])
{
	int i;
	signed char c;
	pthread_t *producer_tid, *consumer_tid;
	pthread_t producer_tid_base, consumer_tid_base;
	pthread_attr_t *producer_attr, *consumer_attr;
	pthread_attr_t producer_attr_base, consumer_attr_base;
	struct data_args *producer_args, *consumer_args;
	struct data_args producer_args_base, consumer_args_base;

	parse_args(argc, argv);

	producer_tid = malloc(sizeof(pthread_t) * producer_count);
	consumer_tid = malloc(sizeof(pthread_t) * consumer_count);
	producer_attr = malloc(sizeof(pthread_attr_t) * producer_count);
	consumer_attr = malloc(sizeof(pthread_attr_t) * consumer_count);
	producer_args = malloc(sizeof(struct data_args) * producer_count);
	consumer_args = malloc(sizeof(struct data_args) * consumer_count);

	if (idx_arr_size * 1024 < DATA_ARRAY_SIZE)
		data_arr_size = idx_arr_size * 1024;

	if (verbose) {
		printf("seed = %ld\n", seed);
		printf("Size of cacheline = %lu bytes\n", sizeof(struct big_data));
		printf("Number of indices in an iteration = %d\n", idx_arr_size);
		printf("Data array size = %d indices x %ld bytes = %ld bytes\n",
			data_arr_size, sizeof(struct big_data),
			data_arr_size * sizeof(struct big_data));
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

	if (verbose)
		printf("idx_arr = 0x%p\n", (void *)idx_arr);

	data_arr = malloc(data_arr_size * sizeof(struct big_data));
	if (!data_arr) {
		printf("Not enough memory for allocating an data array\n");
		exit(1);
	}

	if (verbose)
		printf("data_arr = 0x%p\n", (void *)data_arr);

	setpgid(getpid(), getpid());

	if (producer_count == 0)
		producer_tid_base = create_thread("producer", &producer_attr_base,
						producer, &producer_args_base, -1);

	for (i = 0; i < producer_count; i++)
		producer_tid[i] = create_thread("producer", &producer_attr[i],
					producer, &producer_args[i], cpu_producer[i]);

	if (consumer_count == 0)
		consumer_tid_base = create_thread("consumer", &consumer_attr_base,
					consumer, &consumer_args_base, -1);

	for (i = 0; i < consumer_count; i++)
		consumer_tid[i] = create_thread("consumer", &consumer_attr[i],
					consumer, &consumer_args[i], cpu_consumer[i]);

	for (i = 0; i < producer_count; i++)
		pthread_join(producer_tid[i], NULL);
	if (producer_count == 0)
		pthread_join(producer_tid_base, NULL);
	for (i = 0; i < consumer_count; i++)
		pthread_join(consumer_tid[i], NULL);
	if (consumer_count == 0)
		pthread_join(consumer_tid_base, NULL);

	for (i = 0; i < producer_count; i++)
		pthread_attr_destroy(&producer_attr[i]);
	if (producer_count == 0)
		pthread_attr_destroy(&producer_attr_base);
	for (i = 0; i < consumer_count; i++)
		pthread_attr_destroy(&consumer_attr[i]);
	if (consumer_count == 0)
		pthread_attr_destroy(&consumer_attr_base);
	free(idx_arr);
	free(data_arr);

	return 0;
}
