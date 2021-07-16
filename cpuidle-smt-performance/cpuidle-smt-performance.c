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
#include <errno.h>
#include <dirent.h>


#define gettid()  syscall(SYS_gettid)

#define DEBUG
#undef DEBUG

#ifdef DEBUG
#define debug_printf(fmt...)    printf(fmt)
#else
#define debug_printf(fmt...)
#endif


const char *cpuidle_path = "/sys/devices/system/cpu/cpu%d/cpuidle";
const char *cpuidle_state_path = "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/";
const char *cpuidle_state_name_path = "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/name";
const char *cpuidle_state_usage_path = "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/usage";
const char *cpuidle_state_time_path = "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/time";


unsigned char stop = 0;

#define READ 0
#define WRITE 1

#define MAX_IRRITATORS  7
int cpu_workload = 0;
int cpu_irritator[MAX_IRRITATORS] = {-1};

struct idle_state {
	int cpu;
	int state;
	char name[50];
	unsigned long long usage_before;
	unsigned long long time_before;
	unsigned long long usage_after;
	unsigned long long time_after;
};

unsigned int get_nr_idle_states(void)
{
	char parent[100];
	int parent_fd;
	DIR *parentdir;
	struct dirent *entry;
	int ret = 0;

	sprintf(parent, cpuidle_path, 0);

	parentdir = opendir(parent);
	if (!parentdir)
		return 0;

	while (entry = readdir(parentdir)) {
		if (!strncmp(entry->d_name, ".", 1))
			continue;
		if (!strncmp(entry->d_name, "..", 2))
			continue;
	        ret++;
	}

	return ret;
}

void get_cpu_idle_state_name(int cpu, int state, char *name)
{
	char path[100];
	FILE *fp;

	sprintf(path, cpuidle_state_name_path, cpu, state);

	fp = fopen((const char *)path, "r");
	if (!fp)
		name[0] = '\0';
	else
		fscanf(fp, "%s", name);
}

void get_cpu_idle_state_usage(int cpu, int state, unsigned long long *usage)
{
	char path[100];
	FILE *fp;

	sprintf(path, cpuidle_state_usage_path, cpu, state);

	fp = fopen((const char *)path, "r");
	if (!fp)
		*usage = 0;
	else
		fscanf(fp, "%llu", usage);
}

void get_cpu_idle_state_time(int cpu, int state, unsigned long long *time)
{
	char path[100];
	FILE *fp;

	sprintf(path, cpuidle_state_time_path, cpu, state);

	fp = fopen((const char *)path, "r");
	if (!fp)
		*time = 0;
	else
		fscanf(fp, "%llu", time);
}

void snapshot_idle_state_name(struct idle_state *s) 
{ 
	return get_cpu_idle_state_name(s->cpu, s->state, s->name);
}


#define SNAPSHOT(_x, _y)						      \
static void snapshot_idle_state_##_x##_##_y(struct idle_state *s)              \
{								               \
	get_cpu_idle_state_##_x(s->cpu, s->state, &s->_x##_##_y);                 \
}

SNAPSHOT(usage, before);
SNAPSHOT(usage, after);
SNAPSHOT(time, before);
SNAPSHOT(time, after);

void snapshot_data_before(struct idle_state *s)
{
	snapshot_idle_state_usage_before(s);
	snapshot_idle_state_time_before(s);
}

void snapshot_data_after(struct idle_state *s)
{
	snapshot_idle_state_usage_after(s);
	snapshot_idle_state_time_after(s);
}

unsigned long long get_usage_diff(struct idle_state *s)
{
	return s->usage_after - s->usage_before;
}

unsigned long long get_time_diff(struct idle_state *s)
{
	return s->time_after - s->time_before;
}


static int pipe_fd_workload[2];
static int pipe_fd_irritator[MAX_IRRITATORS][2];
static int nr_irritators = 0;

#define MAX_IDLE_STATES     20
int nr_idle_states = 0;

struct idle_state irritator_idle_states[MAX_IRRITATORS][MAX_IDLE_STATES];
struct idle_state workload_idle_states[MAX_IDLE_STATES];

static void init_idle_states(int cpu, struct idle_state *states)
{
	int i;

	for (i = 0; i < nr_idle_states; i++) {
		states[i].cpu = cpu;
		states[i].state = i;
		snapshot_idle_state_name(&states[i]);
	}
}

static void init_workload_idle_states()
{
	init_idle_states(cpu_workload, workload_idle_states);
}

static void init_irritator_idle_states(int id)
{
	init_idle_states(cpu_irritator[id], irritator_idle_states[id]);
}

static void snapshot_before(struct idle_state *states)
{
	int i;

	for (i = 0; i < nr_idle_states; i++)
		snapshot_data_before(&states[i]);
}

static void snapshot_after(struct idle_state *states)
{
	int i;

	for (i = 0; i < nr_idle_states; i++)
		snapshot_data_after(&states[i]);
}

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



unsigned long long irritator_wakeup_period_ns = 100000ULL;

unsigned long long workload_total_fib_count = 0;
struct wakeup_time {
	struct timespec begin;
	struct timespec end;
};

struct wakeup_time irritator_wakeup_time[MAX_IRRITATORS];
struct wakeup_time t0_wakeup_time;

unsigned long long t0_wakeup_time_total_ns;
unsigned long long irritator_wakeup_time_total_ns[MAX_IRRITATORS];

unsigned long long workload_runtime_total_ns;
unsigned long long t1_runtime_total_ns[MAX_IRRITATORS];

unsigned long long t0_wakeup_count;
unsigned long long irritator_wakeup_count[MAX_IRRITATORS];

int clockid = CLOCK_REALTIME;
//int clockid = CLOCK_MONOTONIC_RAW;
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


#define UNAVAILABLE    0
#define AVAILABLE      1

int irritator_futex = UNAVAILABLE;
static int futex(int *uaddr, int futex_op, int val,
                 const struct timespec *timeout, int *uaddr2, int val3)
{
	return syscall(SYS_futex, uaddr, futex_op, val,
                       timeout, uaddr, val3);
}

static void fwait(int *futexp)
{
	int s;
	while (1) {
		if (__sync_bool_compare_and_swap(futexp, AVAILABLE, UNAVAILABLE))
			break; /* We have been wokenup */

		s = futex(futexp, FUTEX_WAIT_PRIVATE, UNAVAILABLE, NULL, NULL, 0);
		if (s == -1 && errno != EAGAIN) {
			printf("Error futex wait\n");
			exit(1);
		}
	}
}

static void fpost(int *futexp)
{
	if (__sync_bool_compare_and_swap(futexp, UNAVAILABLE, AVAILABLE)) {
		int s = futex(futexp, FUTEX_WAKE_PRIVATE, AVAILABLE, NULL, NULL, 0);
		if (s == -1) {
			printf("Error futex wake\n");
			exit(1);

		}
	}
}

static void wake_all_irritators(void)
{
	int i;
	for (i = 0; i < nr_irritators; i++) {
		clock_gettime(clockid, &irritator_wakeup_time[i].begin);
		debug_printf("Workload writing to Irritator %d pipe\n", i);
		assert(write(pipe_fd_irritator[i][WRITE], &pipec, 1) == 1);
	}
}

static void irritator_wait(int id)
{
	unsigned long long diff;

	debug_printf("Irritator %d waiting\n", id);
	assert(read(pipe_fd_irritator[id][READ], &pipec, 1) == 1);
	clock_gettime(clockid, &(irritator_wakeup_time[id].end));
	diff = compute_timediff(irritator_wakeup_time[id].begin,
				 irritator_wakeup_time[id].end);
	irritator_wakeup_time_total_ns[id] += diff;
	debug_printf("Irritator %d wokeup. latency = %lld ns\n", id,
		     diff);
	irritator_wakeup_count[id]++;
}

static void print_irritator_thread_details(void)
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
	printf("Irritator[PID %d] affined to CPUs: %s\n", my_pid, cpu_list_str);
	CPU_FREE(cpuset);
}

static void print_workload_thread_details(void)
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
	printf("Workload[PID %d] affined to CPUs: %s\n", my_pid, cpu_list_str);
	CPU_FREE(cpuset);
}

#define FIB_ITER_COUNT 128
int fib_vals[FIB_ITER_COUNT];

static void prep_fib_val_array(void)
{
	fib_vals[FIB_ITER_COUNT - 2] = -1;
	fib_vals[FIB_ITER_COUNT - 1] = 1;
}

static void workload_fib_iterations(void)
{
	int i;
	struct timespec begin, end;
	unsigned long long time_diff_ns;
	int a = 0, b = 1 , c;

	clock_gettime(clockid, &begin);
	while (1) {
		/* Do some iterations before checking time */
		for (i = 0; i < FIB_ITER_COUNT; i++) {
			a = fib_vals[ (i - 2) % FIB_ITER_COUNT];
			b = fib_vals[ (i - 1) % FIB_ITER_COUNT];
			c = a + b;
			fib_vals[i] = c;
		}

		workload_total_fib_count += FIB_ITER_COUNT;
		clock_gettime(clockid, &end);
		time_diff_ns = compute_timediff(begin, end);
		if (time_diff_ns > irritator_wakeup_period_ns)
			break;
	}

	workload_runtime_total_ns += compute_timediff(begin, end);
}

unsigned long timeout = 5;
static void *workload_fn(void *arg)
{
	print_workload_thread_details();
	prep_fib_val_array();

	signal(SIGALRM, sigalrm_handler);
	alarm(timeout);

	snapshot_before(workload_idle_states);
	while (!stop) {
		wake_all_irritators();
		workload_fib_iterations();
	}
	snapshot_after(workload_idle_states);

	wake_all_irritators();

	return NULL;
}

static void irritator_fib_iterations(void)
{
	int i;
	struct timespec begin, end;
	unsigned long long time_diff_ns = 0;
	int a = 0, b = 1 , c;

	for (i = 0; i < 1; i++) {
		c = a + b;
		a = b;
		b = c;
	}
}

static void *irritator_fn(void *arg)
{
	int c_id = *((int *)arg);

	print_irritator_thread_details();

	snapshot_before(irritator_idle_states[c_id]);
	while (!stop) {
		irritator_wait(c_id);
		if (stop)
			break;

		irritator_fib_iterations();
		//wake_workload();
	}
	snapshot_after(irritator_idle_states[c_id]);
	
	/* Wakeup the workload, just in case! */
	//wake_workload();
	return NULL;
}


void print_usage(int argc, char *argv[])
{
	printf("Usage: %s [OPTIONS]\n", argv[0]);
	printf("Following options are available\n");
	printf("-w, --wcpu\t\t\t The CPU to which the workload should be affined\n");
	printf("-i, --icpu\t\t\t The CPU to which the irritator should be affined\n");
	printf("-t, --timeout\t\t\t Number of seconds to run the benchmark\n");
	printf("-r, --runtime\t\t\t Amount of time in microseconds irritators should  be woken up\n");
}

void parse_args(int argc, char *argv[])
{
	int c;

	int iteration_length_provided = 0;
	int cache_size_provided = 0;

	while(1) {
		static struct option long_options[] = {
			{"wcpu", required_argument, 0, 'w'},
			{"icpu", required_argument, 0, 'i'},
			{"runtime", required_argument, 0, 'r'},
			{"timeout", required_argument, 0, 't'},
			{0, 0, 0, 0},
		};

		int option_index = 0;
		int cpu;

		c = getopt_long(argc, argv, "hw:i:r:t:", long_options, &option_index);

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

		case 'w':
			cpu_workload = (int) strtoul(optarg, NULL, 10);
			debug_printf("Got option to pin the workload to CPU %d\n",
				cpu_workload);
			break;

		case 'i':
			cpu =  (int) strtoul(optarg, NULL, 10);
			if (nr_irritators < MAX_IRRITATORS) {
				cpu_irritator[nr_irritators] = cpu;
				debug_printf("Got option to pin the irritator to CPU %d\n", cpu);//cpu_irritator[nr_irritators]);
				nr_irritators++;
			}
			break;

		case 'r':
			irritator_wakeup_period_ns = strtoul(optarg, NULL, 10) * 1000;
			debug_printf("Setting fib_iteration duration to %lld ns\n",
				irritator_wakeup_period_ns);
			
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
			int cpu, int *irritator_id)
{
	pthread_t tid;
	cpu_set_t *cpuset;
	size_t size;
	static int max_cpus = 2048;
	struct data_args *args;
	void *thread_args = (void *)irritator_id;
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
		if (*irritator_id == -1)
			debug_printf("Workload will be affined to CPU %s\n",
				cpulist);
		else
			debug_printf("Irritator[%d] will be affined to CPUs %s\n",
				*irritator_id, cpulist);

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
	pthread_t workload_tid, irritator_tid[MAX_IRRITATORS];
	pthread_attr_t workload_attr, irritator_attr[MAX_IRRITATORS];
	int workload_id = -1;
	int irritator_id[MAX_IRRITATORS];
	int i, j;
	double t0_avg_wakeup_time_ns = 0;
	double t1_avg_wakeup_time_ns = 0;
	unsigned long long total_ops = 0;
	unsigned long long total_runtime_ns = 0;
	double ops_per_second = 0;
	unsigned long long total_wakeup_time_ns = 0;
	unsigned long long total_wakeup_count = 0;


	parse_args(argc, argv);
	if (pipe(pipe_fd_workload)) {

		printf("Error creating Workload pipes\n");
		exit(1);
	}

	for (i = 0; i < nr_irritators; i++) {
		if (pipe(pipe_fd_irritator[i])) {
			printf("Error creating Irritator(%d) pipes\n", i);
			exit(1);
		}
	}

	setpgid(getpid(), getpid());

	nr_idle_states = get_nr_idle_states();
	init_workload_idle_states();

	workload_tid = create_thread("workload", &workload_attr,
				     workload_fn, cpu_workload, &workload_id);

	for (i = 0; i < nr_irritators;i++) {
		irritator_id[i] = i;
		init_irritator_idle_states(i);
		irritator_tid[i] = create_thread("irritator", &irritator_attr[i],
						 irritator_fn, cpu_irritator[i],
						 &irritator_id[i]);
	}
	
	pthread_join(workload_tid, NULL);
	for (i = 0; i < nr_irritators; i++) {
		pthread_join(irritator_tid[i], NULL);
	}


	printf("===============================================\n");
	printf("                  Summary \n");
	printf("===============================================\n");

	printf("Irritator wakeup period = %lld us\n",
		irritator_wakeup_period_ns/1000);

	total_ops = workload_total_fib_count;
	total_runtime_ns = workload_runtime_total_ns;

	ops_per_second = ((double)total_ops * 1000000000ULL)/total_runtime_ns;

	debug_printf("Total operations        = %f Mops\n", (double)total_ops/1000000);
	debug_printf("Total run time          = %f seconds \n", (double)total_runtime_ns/1000000000ULL);
	printf("Throughput              = %4.3f Mops/seconds\n",
		ops_per_second/1000000);
	printf("CPU %d:\n", cpu_workload);
	for (j = 0; j < nr_idle_states; j++) {
		int wcpu = cpu_workload;
		char *name = workload_idle_states[j].name;
		unsigned long long usage_diff
			= get_usage_diff(&workload_idle_states[j]);
		unsigned long long time_diff
			= get_time_diff(&workload_idle_states[j]);
		printf("\tState %10s : Usage = %6llu, Time = %9llu us (%3.2f %)\n",
			name, usage_diff, time_diff, ((float)time_diff*100)/(timeout * 1000000));
	}

	for (i = 0; i < nr_irritators; i++) {
		total_wakeup_time_ns = irritator_wakeup_time_total_ns[i];
		total_wakeup_count = irritator_wakeup_count[i];
		printf("Irritator %d average wakeup latency  = %4.3f us\n",
			i, total_wakeup_count ? ((double)(total_wakeup_time_ns)/((total_wakeup_count)*1000)) : 0);
		printf("CPU %d:\n", cpu_irritator[i]);
		for (j = 0; j < nr_idle_states; j++) {
			int wcpu = cpu_workload;
			char *name = irritator_idle_states[i][j].name;
			unsigned long long usage_diff
				= get_usage_diff(&irritator_idle_states[i][j]);
			unsigned long long time_diff
				= get_time_diff(&irritator_idle_states[i][j]);
			printf("\tState %10s : Usage = %6llu, Time = %9llu us(%3.2f %)\n",
				name, usage_diff, time_diff, ((float)time_diff*100)/(timeout * 1000000));
		}
		
	}
		
	printf("===============================================\n");
	pthread_attr_destroy(&workload_attr);
	for (i = 0; i < nr_irritators; i++)
		pthread_attr_destroy(&irritator_attr[i]);

	return 0;
}
