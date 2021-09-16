/*
 * futex-scalability.c :
 *
 * A test program where a given number of threads contend for access
 * to a critical section that is guarded by a mutex. The mutex is
 * implemented using a futex. The user can provide how many times
 * should the mutex implementation retry the atomic Compare-And-Swap
 * operation before making the futex call. The user can also specify
 * how much time should the threads spend inside the critical section.
 *
 * The program measures the total number of critical section entries
 * as well as the the number of critical section entries per
 * second. The program also prints the number of times each thread got
 * to enter the critical section, which is a measure of the fairness.
 *
 * Use mpstat or vmstat to measure the overall user/system/idle
 * time. What we are interested in finding out is if for given
 * critical section duration, what should be the optimal number of CAS
 * retries should the threads perform before making the futex call in
 * order to increase the total number of critical sections.  Eg : On a
 * SMT2 dual core x86 system, running 4 threads for 60 seconds with a
 * critical section period = 1000ns, the user/system/idle time as well
 * as the throughput for different retry counts is as follows:
 *
 *  Retry-count     Num CS/s    %Usr     %Sys    %Idle
 *     1            285K        33.61    22.64   43.67
 *     10           317K        35.47    24.55   39.92
 *     100          365K        54.34    29.17   16.44
 *     1000         388K        84.69    13.91    1.38
 *     10000        376K        86.68    12.98    0.34
 *     100000       362K        87.59    12.38    0.02
 *
 * On the same system if we set the critical section period to 2500ns,
 * then the results are as follows
 *  Retry-count     Num CS/s    %Usr     %Sys    %Idle
 *     1            203K        26.29    16.98   56.66
 *     10           199K        27.69    17.38   54.78
 *     100          246K        39.18    27.23   33.54
 *     1000         243K        85.81    12.76    1.40
 *     10000        231K        85.65    13.63    0.70
 *     100000       237K        87.53    12.45    0.01
 *
 *  Usage: gcc -O2 -o futex-scalability futex-scalability.c -lpthread
 *
 *        ./futex-scalability -t <number of seconds to run> -n <nr threads> -c <critical section period in ns> -r <fwait CAS retry count>
 *
 * Example:
 * $./futex-scalability -t 60 -n 4 -c 1000 -r 1
 * [22866.446915872] 0 thread is active
 * [22866.447130527] 3 thread is active
 * [22866.447131189] 1 thread is active
 * [22866.447297327] 2 thread is active
 * Timeout complete. Stopping all threads
 * [22926.447244887] 2 thread exiting...
 * [22926.447254308] 1 thread exiting...
 * [22926.447260664] 0 thread exiting...
 * [22926.447249483] 3 thread exiting...
 * The number of entries in the critical section = 18031653 (0.300528 M entries/s)
 * Thread 0 = 4468570 entries
 * Thread 1 = 4468145 entries
 * Thread 2 = 4533874 entries
 * Thread 3 = 4561064 entries
 * 
 * (C) 2021 Gautham R. Shenoy <ego@linux.vnet.ibm.com>, IBM Corp 
 * License: GPL v2
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

/* Uncomment this if you need verbose prints */
//#define DEBUG
#ifdef DEBUG
#define debug_printf printf
#else
#define debug_printf(x,...)
#endif


#define barrier() __asm__ __volatile__("": : :"memory")


#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

static inline void cpu_relax()
{
	int i;
	for (i = 0; i < 10; i++)
		barrier();
}

/***********************************************************************
 * Timer Helpers
 ***********************************************************************/
#define USECS_PER_MSEC   (1000ULL)
#define NSECS_PER_USEC   (1000ULL)
#define NSECS_PER_MSEC   (NSECS_PER_USEC * USECS_PER_MSEC)
#define MSECS_PER_SEC    (1000ULL)

#define USECS_PER_SEC    (USECS_PER_MSEC * MSECS_PER_SEC)

int clockid = CLOCK_MONOTONIC_RAW;
/*
 * Returns (after - before) in nanoseconds.
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
		ret_ns = (diff_ns + after.tv_nsec) - before.tv_nsec;
		return ret_ns;
	}

	return 0;
}


/***********************************************************************
 * Futex primitives
 ***********************************************************************/
#define BLOCKED        0
#define AVAILABLE      1

static int futex(int *uaddr, int futex_op, int val,
                 const struct timespec *timeout, int *uaddr2, int val3)
{
	return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr, val3);
}


unsigned long fwait_retry_count = 1;
static void fwait(int *futexp)
{
	int s;
	unsigned long local_count = fwait_retry_count;
	
	while (1) {

		/* Try for a few number of times in the userspace before making the kernel call */
		while (local_count > 0) {
			if (__sync_bool_compare_and_swap(futexp, AVAILABLE, BLOCKED))
				return; /* We are the first to find it available. So we have control */
			local_count--;
		}

		/* We wait for for someone to change the value of futexp from BLOCKED and wake us up */
		s = futex(futexp, FUTEX_WAIT_PRIVATE, BLOCKED, NULL, NULL, 0);
		if (s == -1 && errno != EAGAIN) {
			printf("Error futex wait\n");
			exit(1);
		}

		/* Replenish the local count */
		local_count = fwait_retry_count;
	}
}

static void fpost(int *futexp, int nr_threads_to_wake)
{
	if (__sync_bool_compare_and_swap(futexp, BLOCKED, AVAILABLE)) {
		/* We are the first to flip the value from BLOCKED to AVAILABLE. Wakeup the waiters */
		int s = futex(futexp, FUTEX_WAKE_PRIVATE, nr_threads_to_wake,
			      NULL, NULL, 0);
		if (s == -1) {
			printf("Error futex wake\n");
			exit(1);

		}
	}
}

/***********************************************************************
 * Mutex implementation
 ***********************************************************************/

struct mutex {
	int futexval;
};

#define DEFINE_MUTEX(m)     struct mutex m = {.futexval = AVAILABLE}

static void mutex_lock(struct mutex *m)
{
	fwait(&m->futexval);
}

static void mutex_unlock(struct mutex *m)
{
	fpost(&m->futexval, 1);
}

DEFINE_MUTEX(thread_mutex);

/***********************************************************************
 * Critical Section using Mutexes
 ***********************************************************************/
#define MAX_THREADS 2048
int nr_threads = 4;

unsigned long long critical_section_time_ns = 0;
unsigned long long critical_section_entries = 0;
unsigned long thread_entries[MAX_THREADS];

static void critical_section(int id)
{
	struct timespec start, end;
	unsigned long long diff_ns;

	mutex_lock(&thread_mutex);
	clock_gettime(clockid, &start);
	critical_section_entries++;
	thread_entries[id]++;

	do {
		cpu_relax();
		clock_gettime(clockid, &end);
		diff_ns = compute_timediff(start, end);
	} while(diff_ns < critical_section_time_ns);

	mutex_unlock(&thread_mutex);
}


/* Global timeout : Default 10 seconds */
unsigned long timeout = 10;

/* Global variable indicating that overall timeout is done and that all threads have to exit */
int stop = 0;

/* Signal handler to be called when the global timeout is done */
static void sigalrm_handler(int junk)
{
	printf("Timeout complete. Stopping all threads\n");
	stop = 1;
}

/***********************************************************************
 * Contending Threads
 ***********************************************************************/

static void *thread_fn(void *arg)
{
	int my_idx = *((int *)arg);
	struct timespec cur;
	clock_gettime(clockid, &cur);
	printf("[%lld.%lld] %d thread is active\n",
		cur.tv_sec, cur.tv_nsec, my_idx);

	debug_printf("critical section time = %lld ns\n", critical_section_time_ns);
	if (my_idx == 0) {
		/* Set an alarm for the global timeout */
		signal(SIGALRM, sigalrm_handler);
		alarm(timeout);
	}

	while (!stop)
		critical_section(my_idx);

	clock_gettime(clockid, &cur);
	printf("[%lld.%lld] %d thread exiting...\n",
		cur.tv_sec, cur.tv_nsec, my_idx);
	
	return NULL;
}

void print_usage(int argc, char *argv[])
{
	printf("Usage: %s [OPTIONS]\n", argv[0]);
	printf("Following options are available\n");
	printf("-n, --nthreads\t\t\t Number of contending threads\n");
	printf("-c, --crittime\t\t\t Time in ns spent inside critical section\n");
	printf("-r, --retrycount\t\t The number of userspace retries before making futex syscall\n");
	printf("-t, --timeout\t\t\t Time in seconds for program to run\n");
	
	printf("-h, --help\t\t\t Print this message\n");
}

void parse_args(int argc, char *argv[])
{
	int c;

	int iteration_length_provided = 0;
	int cache_size_provided = 0;

	while(1) {
		static struct option long_options[] = {
			{"nthreads", required_argument, 0, 'n'},
			{"crittime", required_argument, 0, 'c'},
			{"retrycount", required_argument, 0, 'r'},
			{"timeout", required_argument, 0, 't'},
			{"help", no_argument, 0, 'h'},
			{0, 0, 0, 0},
		};

		int option_index = 0;
		int cpu;

		c = getopt_long(argc, argv, "hn:c:r:t:", long_options, &option_index);

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

		case 'n':
			nr_threads = (int) strtoul(optarg, NULL, 10);
			if (nr_threads > MAX_THREADS) {
				nr_threads = MAX_THREADS;
				printf("Capping number of threads to %d\n",
					nr_threads);
			}
			break;

		case 'c':
			critical_section_time_ns = strtoul(optarg, NULL, 10);
			break;

		case 'r':
			fwait_retry_count = strtoul(optarg, NULL, 10);
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




int main(int argc, char *argv[])
{
	int i;

	pthread_t thread_tid[MAX_THREADS];
	pthread_attr_t thread_attr[MAX_THREADS];
	int thread_args[MAX_THREADS];

	pthread_t watchdog_tid;
	pthread_attr_t watchdog_attr;

	parse_args(argc, argv);
	setpgid(getpid(), getpid());
	

	for (i = 0; i < nr_threads; i++) {
		thread_args[i] = i;
		
		pthread_attr_init(&thread_attr[i]);
		
		if (pthread_create(&thread_tid[i], &thread_attr[i], thread_fn,
					&thread_args[i])) {
			printf("Error creating thread %d\n", i);
			exit(1);
		}
		
	}

	for (i = 0; i < nr_threads; i++) {
		pthread_join(thread_tid[i], NULL);
	}

	
	for (i = 0; i < nr_threads; i++)
		pthread_attr_destroy(&thread_attr[i]);

	printf("The number of entries in the critical section = %lld (%6.6f M entries/s)\n",
		critical_section_entries,
		((double) critical_section_entries/timeout)/1000000);


	for (i = 0; i < nr_threads; i++) {
		printf("Thread %d = %ld entries\n", i,
			thread_entries[i]);
	}

	return 0;
}
