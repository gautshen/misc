# cpuidle SMT performance

This creates a workload thread pinned to the CPU specified by `-w` or
`--wcpu`. This workload thread keeps generating fibonacci numbers until
the timeout specified by `-t` option is done (Default timeout is `5s`). It keeps a count of the number of fibonacci numbers computed and the time it spent computing them. The  throughput is the ratio of the number of fibonacci computations done to the time spent working on them.
    
The user can also specify irritator threads which are pinned to the CPU provided by `--icpu` or `-i`. If the irritators are specified, then, every period (provided by `-r` option, default period being `100us`), the workload thread wakes up the irritators. The irritators don't do much, but just wakeup on the CPU where they are pinned, do some minimal computation and go back to sleep. If the irritators are specified, the test keeps track of the
wakeup-latency for the irritators, computed as the time between the workload wokeup an irritator, to the time that the irritator got to run on the CPU (we assume that the irritator CPU is idle)
    
The purpose of the testcase is to understand the wakeup latency and the workload throughput as a function of the irritator wakeup period which is a proxy for the target residency of a particular cpuidle state. 
    
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
threads of that same core (Eg: CPUs 10,12,14 on POWER9 system).
    
Provide the irritator wakeup period corresponding to the cpuidle
residency of the desired state, so that when a thread is idling, the
CPU goes to that idle state.
    
The greater the duration, the deeper the idle state, and the higher
the chances of boosting the single threaded performance. But there is
a penalty in terms of wakeup latency deeper the state.
    

**Sample output : With wakeup period = 10us**
``` 
  $ ./cpuidle-smt-performance -w 8 -i 10 -i 12 -i 14 -r 10 -t 15
    Workload[PID 58692] affined to CPUs: 8,
    Irritator[PID 58693] affined to CPUs: 10,
    Irritator[PID 58694] affined to CPUs: 12,
    Irritator[PID 58695] affined to CPUs: 14,
    ===============================================
                      Summary
    ===============================================
    Irritator wakeup period = 10 us
    Throughput              = 51.766 Mops/seconds
    Irritator 0 average wakeup latency  = 8.676 us
    Irritator 1 average wakeup latency  = 8.639 us
    Irritator 2 average wakeup latency  = 9.102 us
    ===============================================
```    
   
  **Sample output : with wakeup period = 100us**
```    
   $ ./cpuidle-smt-performance -w 8 -i 10 -i 12 -i 14 -r 100 -t 15
    Workload[PID 59402] affined to CPUs: 8,
    Irritator[PID 59403] affined to CPUs: 10,
    Irritator[PID 59404] affined to CPUs: 12,
    Irritator[PID 59405] affined to CPUs: 14,
    ===============================================
                      Summary
    ===============================================
    Irritator wakeup period = 100 us
    Throughput              = 59.226 Mops/seconds
    Irritator 0 average wakeup latency  = 11.316 us
    Irritator 1 average wakeup latency  = 12.742 us
    Irritator 2 average wakeup latency  = 13.463 us
```
