# Futex Scalability

A test program where a given number of threads contend for access to a
critical section that is guarded by a mutex. The mutex is implemented
using a futex. The user can provide how many times should the mutex
implementation retry the atomic Compare-And-Swap operation before
making the futex call. The user can also specify how much time should
the threads spend inside the critical section.

The program measures the total number of critical section entries as
well as the the number of critical section entries per second. The
program also prints the number of times each thread got to enter the
critical section, which is a measure of the fairness.

Use `mpstat` or `vmstat` to measure the overall user/system/idle
time. What we are interested in finding out is if for given critical
section duration, what should be the optimal number of CAS retries
should the threads perform before making the futex call in order to
increase the total number of critical sections.  Eg : On a SMT2 dual
core x86 system, running 4 threads for 60 seconds with a critical
section period = 1000ns, the user/system/idle time as well as the
throughput for different retry counts is as follows:

```
 Retry-count     Num CS/s    %Usr     %Sys    %Idle
 ====================================================
    1            285K        33.61    22.64   43.67
    10           317K        35.47    24.55   39.92
    100          365K        54.34    29.17   16.44
    1000         388K        84.69    13.91    1.38
    10000        376K        86.68    12.98    0.34
    100000       362K        87.59    12.38    0.02
```

On the same system if we set the critical section period to 2500ns,
then the results are as follows
```
 Retry-count     Num CS/s    %Usr     %Sys    %Idle
  ====================================================
    1            203K        26.29    16.98   56.66
    10           199K        27.69    17.38   54.78
    100          246K        39.18    27.23   33.54
    1000         243K        85.81    12.76    1.40
    10000        231K        85.65    13.63    0.70
    100000       237K        87.53    12.45    0.01
```

## How to use this benchmark:
```
gcc -O2 -o futex-scalability futex-scalability.c -lpthread
./futex-scalability -t <number of seconds to run> -n <nr threads> -c
<critical section period in ns> -r <fwait CAS retry count>
```
**Sample output**
```
$./futex-scalability -t 60 -n 4 -c 1000 -r 1
[22866.446915872] 0 thread is active
[22866.447130527] 3 thread is active
[22866.447131189] 1 thread is active
[22866.447297327] 2 thread is active
Timeout complete. Stopping all threads
[22926.447244887] 2 thread exiting...
[22926.447254308] 1 thread exiting...
[22926.447260664] 0 thread exiting...
[22926.447249483] 3 thread exiting...
The number of entries in the critical section = 18031653 (0.300528 M entries/s)
Thread 0 = 4468570 entries
Thread 1 = 4468145 entries
Thread 2 = 4533874 entries
Thread 3 = 4561064 entries
```
