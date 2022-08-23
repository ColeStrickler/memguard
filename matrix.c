// https://stackoverflow.com/questions/15829223/loop-tiling-blocking-for-large-dense-matrix-multiplication
//
// https://stackoverflow.com/questions/59009628/tiled-matrix-multiplication-using-avx
//
// https://stackoverflow.com/questions/8126311/how-much-of-what-every-programmer-should-know-about-memory-is-still-valid
//
// https://github.com/heechul/fastspconv-cvpr2020/blob/master/spmm-16x2-neonfma.c
//
#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>

/* change dimension size as needed */
struct timeval tv; 
int dimension = 1024;
double start, end; /* time */
int i, j, k; /* progress */
FILE *g_fd = NULL;

double timestamp()
{
        double t;
        gettimeofday(&tv, NULL);
        t = tv.tv_sec + (tv.tv_usec/1000000.0);
        return t;
}

void quit(int param)
{
	end = timestamp();

	if (g_fd) {
		fprintf(g_fd, "matrix: %d (%d,%d,%d) took %f ns\n", 
			dimension*i+j,
			i, j, k,
			end - start);
		fclose(g_fd);
	}
	printf("matrix: %d (%d,%d,%d) took %f sec\n", 
		dimension*i+j,
		i, j, k,
		end - start);
	exit(1);
}

// a naive matrix multiplication implementation. 
void matmult(double *A, double *B, double *C, int dimension)
{
	for(int i = 0; i < dimension; i++) {
		for(j = 0; j < dimension; j++) {
			for(k = 0; k < dimension; k++) {
				C[dimension*i+j] += A[dimension*i+k] * B[dimension*k+j];
			}
		}
	}	
}
int main(int argc, char *argv[])
{

	double *A, *B, *C;
	unsigned finish = 0;

	int opt;
	int cpuid = 0;
	int prio = 0;        
	int num_processors;
	struct sched_param param;

	/*
	 * get command line options 
	 */
	while ((opt = getopt(argc, argv, "m:a:n:t:c:i:p:o:f:l:xh")) != -1) {
		switch (opt) {
#if 0 
		cpu_set_t cmask;
		case 'c': /* set CPU affinity */
			cpuid = strtol(optarg, NULL, 0);
			num_processors = sysconf(_SC_NPROCESSORS_CONF);
			CPU_ZERO(&cmask);
			CPU_SET(cpuid % num_processors, &cmask);
			if (sched_setaffinity(0, num_processors, &cmask) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned to cpu %d\n", cpuid);
			break;
		case 'o': /* SCHED_BATCH */
			if (!strcmp(optarg, "batch")) {
				param.sched_priority = 0; 
				if(sched_setscheduler(0, SCHED_BATCH, &param) == -1) {
					perror("sched_setscheduler failed");
					exit(1);
				}
			} else if (!strcmp(optarg, "fifo")) {
				param.sched_priority = 1; 
				if(sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
					perror("sched_setscheduler failed");
					exit(1);
				}
			}
			break;
		case 'p': /* set priority */
			prio = strtol(optarg, NULL, 0);
			if (setpriority(PRIO_PROCESS, 0, prio) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned priority %d\n", prio);
			break;
#endif
		case 'n':
			dimension = strtol(optarg, NULL, 0);
			break;

		case 'f': /* set file descriptor */
			g_fd = fopen(optarg, "a+");
			if (g_fd == NULL) 
				perror("error");
			break;
		}
	}

	printf("dimension: %d\n", dimension);

	A = (double*)malloc(dimension*dimension*sizeof(double));
	B = (double*)malloc(dimension*dimension*sizeof(double));
	C = (double*)malloc(dimension*dimension*sizeof(double));

	srand(292);

	for(i = 0; i < dimension; i++)
			for(j = 0; j < dimension; j++)
			{   
					A[dimension*i+j] = (rand()/(RAND_MAX + 1.0));
					B[dimension*i+j] = (rand()/(RAND_MAX + 1.0));
					C[dimension*i+j] = 0.0;
			}   

	start = timestamp();

	matmult(A, B, C, dimension);

	end = timestamp();
	printf("\nsecs:%f\n", end-start);

	free(A);
	free(B);
	free(C);

	return 0;
}
