/*
 * CPU Idle SMT folding benchmark with wakeup latency
 *
 * Build with:
 *
 * gcc -o cpuidle-smt-performance cpuidle-smt-performance.c -lpthread
 *
 * Copyright (C) 2021 Gautham R. Shenoy <ego@linux.vnet.ibm.com>, IBM
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


#define gettid()  syscall(SYS_gettid)

#undef DEBUG
#ifdef DEBUG
#define debug_printf(fmt...)    printf(fmt)
#else
#define debug_printf(fmt...)
#endif

unsigned char stop = 0;

#define READ 0
#define WRITE 1

static int pipe_fd_thread0[2];
static int pipe_fd_thread1[2];

char pipec;

typedef unsigned long long u64;


static void sigalrm_handler(int junk)
{
	stop = 1;
}

/*
 * Helper function to pretty-print cpuset into list for easy viewing.
 *
 * WARNING: Hasn't been extensively tested.
 */
static void cpuset_to_list(cpu_set_t *cpuset, int size, char *str)
{
	int start = -1;
	int end = -2;
	int cur, i;
	int max_cpus = 2048;

	for (i = 0; i < max_cpus; i++) {
		cur = i;
		if (CPU_ISSET_S(i, size, cpuset)) {
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

	if (end == size - 1) {
		int len;
		/* Streak has ended. Print the last streak */
		if (start == end)
			len = sprintf(str, "%d,", start);
		else
			len = sprintf(str, "%d-%d,", start, end);

		str = str + len;
	}
}



unsigned long long max_fib_iteration_ns = 100000ULL;

unsigned long long t1_total_fib_count = 0;
unsigned long long t0_total_fib_count = 0;
struct wakeup_time {
	struct timespec begin;
	struct timespec end;
};

struct wakeup_time t1_wakeup_time;
struct wakeup_time t0_wakeup_time;

unsigned long long t0_wakeup_time_total_ns;
unsigned long long t1_wakeup_time_total_ns;

unsigned long long t0_runtime_total_ns;
unsigned long long t1_runtime_total_ns;

unsigned long long t0_wakeup_count;
unsigned long long t1_wakeup_count;

/*
 * Helper function to compute the difference between two timespec
 * structures.  The return value is in nanoseconds.
 *
 * WARNING : Have occassionally seen incorrect values when
 * after.tv_sec > before.tv_sec.
 */
static unsigned long long compute_timediff(struct timespec before,
					   struct timespec after)
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


static void wake_all_thread1s(void)
{
	clockid_t clockid  = CLOCK_MONOTONIC_RAW; //CLOCK_THREAD_CPUTIME_ID;
	clock_gettime(clockid, &t1_wakeup_time.begin);
	debug_printf("Thread0 writing to pipe\n");
	assert(write(pipe_fd_thread1[WRITE], &pipec, 1) == 1);
}

static void thread0_wait(void)
{
	clockid_t clockid  = CLOCK_MONOTONIC_RAW; //CLOCK_THREAD_CPUTIME_ID;	

	debug_printf("Thread0 waiting\n");
	assert(read(pipe_fd_thread0[READ], &pipec, 1) == 1);
	debug_printf("Thread0 read from pipe\n");
	clock_gettime(clockid, &t0_wakeup_time.end);
	t0_wakeup_time_total_ns += compute_timediff(t0_wakeup_time.begin,
							  t0_wakeup_time.end);
	t0_wakeup_count++;
}

static void thread1_wait(void)
{
	clockid_t clockid  = CLOCK_MONOTONIC_RAW; //CLOCK_THREAD_CPUTIME_ID;

	debug_printf("Thread1 waiting\n");
	assert(read(pipe_fd_thread1[READ], &pipec, 1) == 1);
	clock_gettime(clockid, &(t1_wakeup_time.end));
	t1_wakeup_time_total_ns += compute_timediff(t1_wakeup_time.begin,
							  t1_wakeup_time.end);
	t1_wakeup_count++;
}

static void wake_thread0(void)
{
	clockid_t clockid  = CLOCK_MONOTONIC_RAW; //CLOCK_THREAD_CPUTIME_ID;

	clock_gettime(clockid, &(t0_wakeup_time.begin));
	assert(write(pipe_fd_thread0[WRITE], &pipec, 1) == 1);
}

static void print_thread1_thread_details(void)
{
	pthread_t thread = pthread_self();
        cpu_set_t *cpuset;
	size_t size;
	static int max_cpus = 2048;
	pid_t my_pid = gettid();
	char cpu_list_str[2048];

	cpuset = CPU_ALLOC(max_cpus);
	if (cpuset == NULL) {
		printf("Unable to allocate cpuset\n");
		exit(1);
	}

	size = CPU_ALLOC_SIZE(max_cpus);
	CPU_ZERO_S(size, cpuset);

	pthread_getaffinity_np(thread, size, cpuset);

	cpuset_to_list(cpuset, size, cpu_list_str);
	printf("Thread1[PID %d] affined to CPUs: %s\n", my_pid, cpu_list_str);
	CPU_FREE(cpuset);
}

static void print_thread0_thread_details(void)
{
	pthread_t thread = pthread_self();
        cpu_set_t *cpuset;
	size_t size;
	static int max_cpus = 2048;
	pid_t my_pid = gettid();

	char cpu_list_str[2048];

	cpuset = CPU_ALLOC(max_cpus);
	if (cpuset == NULL) {
		printf("Unable to allocate cpuset\n");
		exit(1);
	}

	size = CPU_ALLOC_SIZE(max_cpus);
	CPU_ZERO_S(size, cpuset);

	pthread_getaffinity_np(thread, size, cpuset);

	cpuset_to_list(cpuset, size, cpu_list_str);
	printf("Thread0[PID %d] affined to CPUs: %s\n", my_pid, cpu_list_str);
	CPU_FREE(cpuset);
}

static void thread0_fib_iterations(void)
{
	int i;
	struct timespec begin, end;
	unsigned long long time_diff_ns;
	clockid_t clockid  = CLOCK_MONOTONIC_RAW; //CLOCK_THREAD_CPUTIME_ID;
	int a = 0, b = 1 , c;

	clock_gettime(clockid, &begin);
	while (1) {
		/* Do some iterations before checking time */
		for (i = 0; i < 1000; i++) {
			c = a + b;
			a = b;
			b = c;
		}

		t0_total_fib_count += 1000;
		clock_gettime(clockid, &end);
		time_diff_ns = compute_timediff(begin, end);
		if (time_diff_ns > max_fib_iteration_ns)
			break;

	}

	t0_runtime_total_ns += time_diff_ns;
}

unsigned long timeout = 5;
static void *thread0_fn(void *arg)
{
	print_thread0_thread_details();

	signal(SIGALRM, sigalrm_handler);
	alarm(timeout);

	while (!stop) {
		wake_all_thread1s();
		thread0_wait();
		thread0_fib_iterations();
	}

	wake_all_thread1s();

	return NULL;
}

static void thread1_fib_iterations(void)
{
	int i;
	struct timespec begin, end;
	unsigned long long time_diff_ns = 0;
	clockid_t clockid  = CLOCK_MONOTONIC_RAW; //CLOCK_THREAD_CPUTIME_ID;
	int a = 0, b = 1 , c;

	clock_gettime(clockid, &begin);
	while (1) {
		/* Do some iterations before checking time */
		for (i = 0; i < 1000; i++) {
			c = a + b;
			a = b;
			b = c;
		}

		t1_total_fib_count += 1000;
		clock_gettime(clockid, &end);
		time_diff_ns = compute_timediff(begin, end);
		if (time_diff_ns > max_fib_iteration_ns)
			break;

	}

	t1_runtime_total_ns += time_diff_ns;
	
}

static void *thread1_fn(void *arg)
{
	int c_id = *((int *)arg);

	print_thread1_thread_details();
	clockid_t clockid  = CLOCK_MONOTONIC_RAW; //CLOCK_THREAD_CPUTIME_ID;	
	while (!stop) {
		thread1_wait();
		if (stop)
			break;

		thread1_fib_iterations();
		wake_thread0();
	}

	/* Wakeup the thread0, just in case! */
	wake_thread0();
	return NULL;
}


int cpu_thread0 = 0;
int cpu_thread1 = 1;

void print_usage(int argc, char *argv[])
{
	printf("Usage: %s [OPTIONS]\n", argv[0]);
	printf("Following options are available\n");
	printf("-p, --pcpu\t\t\t The CPU to which the thread0 should be affined\n");
	printf("-c, --ccpu\t\t\t The CPU to which the thread0 should be affined\n");
	printf("-t, --timeout\t\t\t Number of seconds to run the benchmark\n");
	printf("-r, --runtime\t\t\t Amount of time in microseconds that each iter of prod/cons runs\n");
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
			{"runtime", required_argument, 0, 'r'},
			{"timeout", required_argument, 0, 't'},
			{0, 0, 0, 0},
		};

		int option_index = 0;
		int cpu;

		c = getopt_long(argc, argv, "hp:c:r:t:", long_options, &option_index);

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
			cpu_thread0 = (int) strtoul(optarg, NULL, 10);
			break;

		case 'c':
			cpu =  (int) strtoul(optarg, NULL, 10);
			cpu_thread1 = cpu;
			debug_printf("Got option to pin the thread1 to CPU %d\n", cpu_thread1);
			break;

		case 'r':
			max_fib_iteration_ns = strtoul(optarg, NULL, 10) * 1000;
			debug_printf("Setting fib_iteration duration to %lld ns\n",
				max_fib_iteration_ns);
			
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
			int cpu, int *thread1_id)
{
	pthread_t tid;
	cpu_set_t *cpuset;
	size_t size;
	static int max_cpus = 2048;
	struct data_args *args;
	void *thread_args = (void *)thread1_id;
	char cpulist[5000];

	pthread_attr_init(attr);
	if (cpu != -1) {
		cpuset = CPU_ALLOC(max_cpus);
		if (cpuset == NULL) {
			printf("Unable to allocate cpuset\n");
			exit(1);
		}

		size = CPU_ALLOC_SIZE(max_cpus);
		CPU_ZERO_S(size, cpuset);
		CPU_SET_S(cpu, size, cpuset);
		cpuset_to_list(cpuset, size, cpulist);
		if (*thread1_id == -1)
			debug_printf("Thread0 will be affined to CPU %s\n",
				cpulist);
		else
			debug_printf("Thread1[%d] will be affined to CPUs %s\n",
				*thread1_id, cpulist);

		if (pthread_attr_setaffinity_np(attr,
						size,
						cpuset)) {
			perror("Error setting affinity");
			exit(1);
		}
	}

	if (pthread_create(&tid, attr, fn, thread_args)) {
		debug_printf("Error creating the %s\n", name);
		exit(1);
	} else {
		debug_printf("%s created with tid %ld\n", name, tid);
	}

	if (cpu != -1)
		CPU_FREE(cpuset);

	return tid;
}

int main(int argc, char *argv[])
{
	signed char c;
	pthread_t thread0_tid, thread1_tid;
	pthread_attr_t thread0_attr, thread1_attr;
	int thread0_id = -1;
	int thread1_id = 1;
	int nr_thread1s = 1;
	int i;
	double t0_avg_wakeup_time_ns;
	double t1_avg_wakeup_time_ns;
	unsigned long long total_ops;
	unsigned long long total_runtime_ns;
	double ops_per_second;
	unsigned long long total_wakeup_time_ns;
	unsigned long long total_wakeup_count;


	parse_args(argc, argv);
	if (pipe(pipe_fd_thread0)) {

		printf("Error creating Thread0 pipes\n");
		exit(1);
	}

	if (pipe(pipe_fd_thread1)) {
		printf("Error creating Thread1(%d) pipes\n", i);
		exit(1);
	}

	setpgid(getpid(), getpid());

	thread0_tid = create_thread("thread0", &thread0_attr,
				     thread0_fn, cpu_thread0, &thread0_id);

	thread1_tid = create_thread("thread1", &thread1_attr,
				     thread1_fn, cpu_thread1, &thread1_id);
	
	pthread_join(thread0_tid, NULL);
	pthread_join(thread1_tid, NULL);


	printf("===============================================\n");
	printf("                  Summary \n");
	printf("===============================================\n");

        t0_avg_wakeup_time_ns = (double)t0_wakeup_time_total_ns/t0_wakeup_count;
        t1_avg_wakeup_time_ns = (double)t1_wakeup_time_total_ns/t1_wakeup_count;
	debug_printf("Total Number of Thread0 Operations = %lld K\n",
		t0_total_fib_count/1000);

	debug_printf("Total Number of Thread1 Operations = %lld K\n",
		t1_total_fib_count/1000);

	debug_printf("Total number of thread0 wakeups = %lld\n",
		t0_wakeup_count);

	debug_printf("Total number of thread1 wakeups = %lld\n",
		t1_wakeup_count);

	debug_printf("Avg Thread0 wakeup latency = %4.3f us\n",
		t0_avg_wakeup_time_ns/1000);
	debug_printf("Avg Thread1 wakeup latency = %4.2f us\n",
		t1_avg_wakeup_time_ns/1000);

	printf("Per runtime duration   = %lld us\n",
		max_fib_iteration_ns/1000);

	total_ops = t0_total_fib_count 	+ t1_total_fib_count;
	total_runtime_ns = t0_runtime_total_ns + t1_runtime_total_ns;
	ops_per_second = ((double)total_ops * 1000000000ULL)/total_runtime_ns;

	printf("Total operations       = %f Mops\n", (double)total_ops/1000000);
	printf("Total run time         = %f seconds \n", (double)total_runtime_ns/1000000000ULL);
	printf("Throughput             = %4.3f Mops/seconds\n",
		ops_per_second/1000000);

	total_wakeup_time_ns = t0_wakeup_time_total_ns + t1_wakeup_time_total_ns;
	total_wakeup_count = t0_wakeup_count + t1_wakeup_count;
	printf("Average wakeup latency = %4.3f us\n",
		((double)(total_wakeup_time_ns)/((total_wakeup_count)*1000)));
		
	printf("===============================================\n");
	pthread_attr_destroy(&thread0_attr);
	pthread_attr_destroy(&thread1_attr);
	return 0;
}
