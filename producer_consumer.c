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
#include <limits.h>
#include <sys/shm.h>
#include <linux/futex.h>

#undef DEBUG

#ifdef DEBUG
#define dprintf(fmt,...)    printf(fmt,...)
#else
#define dprintf(fmt,...)
#endif

#define INDEX_ARRAY_SIZE (256) //1k
#define DATA_ARRAY_SIZE  (1024*1024*512) //256M
unsigned long seed = 6407741;


unsigned long iterations;
unsigned long iterations_prev;
static unsigned int timeout = 10;

static void sigalrm_handler(int junk)
{
	unsigned long i = iterations;

	printf("%ld\n", i - iterations_prev);
	iterations_prev = i;

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

	/* printf("Producer : idx_array_size = %ld,  data_array_size = %ld\n", */
	/* 	idx_arr_size, data_arr_size); */
	/* printf("Producer : idx_array = 0x%llx,  data_array = 0x%llx\n", */
	/* 	index_array, data_array); */
	
	signal(SIGALRM, sigalrm_handler);
	alarm(1);

	while (1) {

		dprintf("Producer while begin\n");
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

			dprintf("Producer : [%d] = %ld,  [%ld] = 0x%llx\n",
				i, idx, idx, data);
			index_array[i] = idx;
			data_array[idx] = data;
		}

		dprintf("Producer writing to pipe\n");
		assert(write(pipe_fd2[WRITE], &c, 1) == 1);
//		touch();

		dprintf("Producer waiting\n");
		assert(read(pipe_fd1[READ], &c, 1) == 1);
//		touch();
		dprintf("Producer read from pipe\n");
		iterations += 2;
	}

	return NULL;
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

	/* printf("Consumer : idx_array_size = %ld,  data_array_size = %ld\n", */
	/* 	idx_arr_size, data_arr_size); */
	/* printf("Consumer : idx_array = 0x%llx,  data_array = 0x%llx\n", */
	/* 	index_array, data_array); */


	while (1) {
		unsigned long idx = 0;
		unsigned int sum = 0;
		

		dprintf("Consumer While begin\n");
		dprintf("Consumer waiting\n");
		assert(read(pipe_fd2[READ], &c, 1) == 1);
//		touch();
		dprintf("Consumer read from pipe\n");
		dprintf("Consume idx_arr_size = %ld\n", idx_arr_size);

		for (i = 0; i < idx_arr_size; i++) {
			unsigned long idx;
			unsigned long data;

			idx = index_array[i];
			data = data_array[idx];

			dprintf("Consumer : [%d] = %ld,  [%ld] = 0x%llx\n",
				i, idx, idx, data);
			sum = (sum + data) % INT_MAX;
			
		}


		idx = random() % data_arr_size;
		dprintf("Consumer writing [%ld] = 0x%llx\n", idx, sum);
		data_array[idx] = sum;

		/* data_array[idx] = sum; */

		dprintf("Consumer writing to pipe\n");
		assert(write(pipe_fd1[WRITE], &c, 1) == 1);
//		touch();

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
	int cpu_producer = 0, cpu_consumer = 8;
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

	CPU_ZERO(&cpuset);
	CPU_SET(cpu_producer, &cpuset);
	pthread_attr_init(&producer_attr);
	if (pthread_attr_setaffinity_np(&producer_attr, sizeof(cpu_set_t),
					&cpuset)) {
		perror("Error setting affinity for producer");
		exit(1);
	}
	
	if (pthread_create(&producer_tid,
			   &producer_attr, producer, &producer_args)) {
		printf("Error creating the producer\n");
		exit(1);
	}

	printf("Affined producer to CPU %d\n", cpu_producer);
	pthread_attr_init(&consumer_attr);
	CPU_ZERO(&cpuset);
	CPU_SET(cpu_consumer, &cpuset);
	if (pthread_attr_setaffinity_np(&consumer_attr, sizeof(cpu_set_t),
					&cpuset)) {
		perror("Error setting affinity for consumer");
		exit(1);
	}
	printf("Affined consumer to CPU %d\n", cpu_consumer);	
	if (pthread_create(&consumer_tid,
			   &consumer_attr, consumer, &consumer_args)) {
		printf("Error creating the consumer\n");
		exit(1);
	}

	while (1)
		sleep(3600);

	return 0;
}
