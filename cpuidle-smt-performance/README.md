# cpuidle SMT performance

This creates a workload thread pinned to the CPU specified by `-w` or
`--wcpu`. This workload thread keeps generating fibonacci numbers
until the timeout specified by `-t` option is done (Default timeout is
`5s`). It keeps a count of the number of fibonacci numbers computed
and the time it spent computing them. The throughput is the ratio of
the number of fibonacci computations done to the time spent working on
them.
    
The user can also specify irritator threads which are pinned to the
CPU provided by `--icpu` or `-i`. These irritator threads sleep by
blocking on a pipe. These threads are periodically woken up by the
waker thread. On being woken up, the irritator threads go back to
sleep by blocking on their respective pipes.

The user can specify the waker thread which is pinned to the CPU
provided by `--acpu` or `-a`. This waker thread periodicall wakes up
the all the irritators by writing to their respective pipes. The
wakeup period is specified by the option `-r` (default wakeup period
is `100us`).
    
The purpose of the testcase is to understand the wakeup latency and
the workload throughput as a function of the irritator wakeup period
which is a proxy for the target residency of a particular cpuidle
state.

The lower the period, the lower the wakeup latency will be, since the
cpuidle subsystem will pick a shallower idle state. But this means
lack of SMT folding as well, which should show up as lower workload
throughput.
    
Similarly, higher the irritator wakeup period, higher would be the
wakeup latency, since the cpuidle subsystem would have put the idling
irritator's CPU into a deeper idle state. Thus waking up the deeper
idle CPU should take a longer time. This should hopefully translate
into higher workload throughput due to increase in single-threaded
performance.
    
## How to use this benchmark:

    
On an idle system, pin the workload to the primary thread of a core
(Eg : CPU 8 on POWER9 system) . Pin the irritators to the secondary
threads of that same core (Eg: CPUs 10,12,14 on POWER9 system).  Pin
the waker thread to a different core (Eg: CPU 16 on a POWER9 system)
    
Provide the irritator wakeup period corresponding to the cpuidle
residency of the desired state, so that when a thread is idling, the
CPU goes to that idle state.
    
The greater the duration, the deeper the idle state, and the higher
the chances of boosting the single threaded performance. But there is
a penalty in terms of wakeup latency deeper the state.
    

**Sample output : With wakeup period = 10us. Timeout = 60s**
``` 
  $ ./cpuidle-smt-performance -w 8 -i 10 -i 12 -i 14 -a 16 -r 10 -t 60
  Workload[PID 74619] affined to CPUs: 8,
  Irritator 0[PID 74620] affined to CPUs: 10,
  Irritator 1[PID 74621] affined to CPUs: 12,
  Irritator 2[PID 74622] affined to CPUs: 14,
  Waker[PID 74623] affined to CPUs: 16,
  ===============================================
                    Summary 
  ===============================================
  Irritator wakeup period = 10 us
  Workload Throughput            = 46.049 Mops/seconds
  Overall average sleep duration = 16.513 us
  Overall average wakeup latency = 8.242 us
  	  Overall State     snooze : Usage =  3582087, Time =  12973757 us(21.62 %)
	  Overall State       CEDE : Usage =        4, Time =        88 us(0.00 %)
  ===============================================
```    
   
  **Sample output : with wakeup period = 120us, timeout=60s**
```
   $ ./cpuidle-smt-performance -w 8 -i 10 -i 12 -i 14 -a 16 -r 120 -t 60

   Workload[PID 74899] affined to CPUs: 8,
   Irritator 0[PID 74900] affined to CPUs: 10,
   Irritator 1[PID 74901] affined to CPUs: 12,
   Irritator 2[PID 74902] affined to CPUs: 14,
   Waker[PID 74903] affined to CPUs: 16,
   ===============================================
	               Summary 
   ===============================================
   Irritator wakeup period = 120 us
   Workload Throughput            = 59.511 Mops/seconds
   Overall average sleep duration = 126.603 us
   Overall average wakeup latency = 14.414 us
   	   Overall State     snooze : Usage =   165978, Time =   9472751 us(15.79 %)
	   Overall State       CEDE : Usage =   392170, Time =  43965318 us(73.28 %)
===============================================

```
