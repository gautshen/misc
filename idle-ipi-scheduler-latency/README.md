# idle-ipi-scheduler-latency

## What
This is a benchmark to measure the impact of platform idle-states latency and the IPI latency on the scheduler wakeups.

## Why
When a task wakes up, the scheduler finds a target CPU for it to run. If the target CPU was idle, the scheduler sends an IPI to wake it up. Thus, the task has to wait until the target CPU gets the IPI, and wakes up from a platform idle state before it can run on the CPU.
If the target CPU were idle, but in a polling state, then due to a scheduler optimization, it is  not sent an IPI, but instead the scheduler updates a variable in the memory which causes the target CPU to exit the idle loop and start running the task. 

We want to measure how much of additional negative impact will the IPI latency and the platform idle-state wakeup latency have on the task-wakeup on an idle CPU compared to when the idle CPU was in the Polling loop.

## How

We use the `context_switch2` benchmark written by Anton Blanchard (http://ozlabs.org/~anton/junkcode/context_switch2.c). On a system with `N` cores, 
we run the `context_switch2` benchmark `N` times. Across all the `N` times, one of the CPUs
on which the `context_switch2` process/thread runs, is fixed. The other CPU is varied in order to cover
all the cores in the system. For each of the aformentioned CPU pairs, we run the benchmark once with
only the polling idle state enabled (`snooze` state on PowerPC), and another time with all the idle states enabled. For each run we capture the idle state statistics as well as the Interrupt Statistics. The data collected across these two runs are compared to determine the regression in the average number of context-switches when the idle-states were enabled vs when only the polling state was enabled. 

Finally, the data is postprocessed to output a percentage regression distribution for the `N` instances.

## Usage
```
$./idle_ipi_scheduler_latency.sh [-h] [-a CPUA] [-t Timeout] [-l Logdirectory]
$./postprocess_data -l Logdirectory -o SummaryDirectory 
```

where

`CPUA` is the fixed CPU in the pair of CPUs on which context_switch2 is affined.re the CPUs to which the two  processes/threads (Default value CPU`0`)

`Timeout` is the number of seconds that the context_switch2 program should run (Default value `10` seconds)

`Logdirectory` is Directory where the output logs need to be saved (Default value `logs`)

`SummaryDirectory` is the directory where the post-processed summary should be saved.

**Example**
```
$./idle_ipi_scheduler_latency.sh -a 4 -t 15 -l /tmp/logs; ./postprocess_data.sh -l /tmp/logs -o summary
==========================================
Tests are done. The logs are in /tmp/logs
==========================================
<= 1 pct difference : 35 instances
(1 - 2] pct difference : 4 instances
(2 - 3] pct difference : 0 instances
(3 - 5] pct difference : 1 instances
(5 - 10] pct difference : 0 instances
(10 - 20] pct difference : 0 instances
(20 - 50] pct difference : 0 instances
(50 - 100] pct difference : 0 instances
=====================================================
More details can be found in the summary directory
=====================================================

```

In this example we are keeping `CPU4` to be the fixed CPU, and running each instance of the `context_switch2` benchmark for 15 seconds. The logs are in the directory `logs`. The post-processed output would be in the directory `summary`.

The system has 40 SMT-4 cores. Thus we would see 40 instances in total. 

Of these 
in 35 instances we observed a regression of less than 1%
in  4 instances, we observed a regression between 1% and 2%
in  1 instance, the regression was between 3% and 5%

In the `summaries` folder, we see folder corresponding to each of the rows of the aforementioned distribution.

```
$ls summaries/
1  10  100  2  20  3  5  50  distribution.txt
```

Suppose we want to know the cases where the regression was between `X%` and `Y%`, the data would be in the subdirectory `Y`. 

In the aforementioned example, there is one sample between 3% and 5%. The corresponding data will be in `summaries/5`

```
$ls summary/5
CPUS-4-24-summary
$less summaries/5/CPUS-4-24-summary

==========================
Context Switch Information
==========================
With Only snooze Enabled     : 286441.60
With All Idle States Enabled : 273925.60
Percentage regression        : 4.36 %
==========================================
IRQ Information : With Only snooze Enabled
==========================================
CPU 4: WDG [Watchdog soft-NMI interrupts] = 3573 times
CPU 4: DBL [Doorbell interrupts] = 1381 times
CPU 4: LOC [Local timer interrupts for timer event device] = 3792 times
CPU 4: LOC [Local timer interrupts for others] = 1806 times
CPU 4: Total IRQs = 10552
-------
CPU 24: WDG [Watchdog soft-NMI interrupts] = 3482 times
CPU 24: DBL [Doorbell interrupts] = 860 times
CPU 24: LOC [Local timer interrupts for timer event device] = 3791 times
CPU 24: LOC [Local timer interrupts for others] = 1801 times
CPU 24: Total IRQs = 9934
=========================================
IRQ Information : With All States Enabled
=========================================
CPU 4: WDG [Watchdog soft-NMI interrupts] = 3495 times
CPU 4: DBL [Doorbell interrupts] = 1017 times
CPU 4: LOC [Local timer interrupts for timer event device] = 3793 times
CPU 4: LOC [Local timer interrupts for others] = 1813 times
CPU 4: Total IRQs = 10118
-------
CPU 24: WDG [Watchdog soft-NMI interrupts] = 3660 times
CPU 24: DBL [Doorbell interrupts] = 888 times
CPU 24: LOC [Local timer interrupts for timer event device] = 3792 times
CPU 24: LOC [Local timer interrupts for others] = 1800 times
CPU 24: Total IRQs = 10140
=================================================
Idle State information : With Only snooze Enabled
=================================================
CPU4: snooze (enabled ), Usage = 2145630 times, Time = 106002 us
CPU4: stop0_lite (disabled), Usage = 0 times, Time = 0 us
CPU4: stop0 (disabled), Usage = 0 times, Time = 0 us
CPU4: stop1 (disabled), Usage = 0 times, Time = 0 us
CPU4: stop2 (disabled), Usage = 0 times, Time = 0 us
CPU4: stop4 (disabled), Usage = 0 times, Time = 0 us
CPU4: stop5 (disabled), Usage = 0 times, Time = 0 us
-------
CPU24: snooze (enabled ), Usage = 2147345 times, Time = 2214543 us
CPU24: stop0_lite (disabled), Usage = 0 times, Time = 0 us
CPU24: stop0 (disabled), Usage = 0 times, Time = 0 us
CPU24: stop1 (disabled), Usage = 0 times, Time = 0 us
CPU24: stop2 (disabled), Usage = 0 times, Time = 0 us
CPU24: stop4 (disabled), Usage = 0 times, Time = 0 us
CPU24: stop5 (disabled), Usage = 0 times, Time = 0 us
================================================
Idle State information : With All States Enabled
================================================
CPU4: snooze (enabled ), Usage = 2053854 times, Time = 2041727 us
CPU4: stop0_lite (enabled ), Usage = 3 times, Time = 3514 us
CPU4: stop0 (enabled ), Usage = 3 times, Time = 17 us
CPU4: stop1 (enabled ), Usage = 1 times, Time = 0 us
CPU4: stop2 (enabled ), Usage = 23 times, Time = 40305 us
CPU4: stop4 (enabled ), Usage = 0 times, Time = 0 us
CPU4: stop5 (enabled ), Usage = 0 times, Time = 0 us
-------
CPU24: snooze (enabled ), Usage = 2055354 times, Time = 1988408 us
CPU24: stop0_lite (enabled ), Usage = 8 times, Time = 9418 us
CPU24: stop0 (enabled ), Usage = 2 times, Time = 5 us
CPU24: stop1 (enabled ), Usage = 0 times, Time = 0 us
CPU24: stop2 (enabled ), Usage = 24 times, Time = 27081 us
CPU24: stop4 (enabled ), Usage = 0 times, Time = 0 us
CPU24: stop5 (enabled ), Usage = 0 times, Time = 0 us

```
