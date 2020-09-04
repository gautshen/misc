#!/bin/bash

###############################################################################################################################
#
# idle_ipi_schduler_latency.sh:
#
# Author : Gautham R. Shenoy <ego@linux.vnet.ibm.com>
#
# Script to run N context_switch2 between N different pair of CPUs while capturing
# the interrupt statistics and the idle statistics on those CPUs (N = number of cores in the system)
# One of the CPU is fixed in every pair. The other CPU is a thread from a core in the system.
#
# Usage : ./run_all_context_switch.sh [-h] [-a CPUA] [-t Timeout] [-l Logdirectory] [-z LastCPU]
# where
#
#    CPUA                    = The fixed CPU in the pair of CPUs on which context_switch2
#                              is affined.re the CPUs to which the two
#                              processes/threads (Default value CPU0)
#
#    Timeout                 = Number of seconds that the context_switch2 program
#                              should run (Default value 10 seconds)
#
#    Logdirectory            = Directory where the output logs need to be saved
#                              (Default value "logs")
#
#    LastCPU                 = The upper limit on the CPUs to be considered in the pair.
#                              (Default value : The last CPU in the system)
#
###############################################################################################################################


RUNONE=./run_one_pair_context_switch.sh

CPUA=0
TIMEOUT=10
LOGDIR=logs

NRCPUS=`ls -1 /sys/devices/system/cpu/ | grep "cpu[0-9]\+" | wc -l`
MAXCPU=`expr $NRCPUS - 1`

function print_usage {
    echo "$0:"
    echo "Script to run N context_switch2 between N different pair of CPUs while capturing"
    echo "the interrupt statistics and the idle statistics on those CPUs (N = number of cores in the system)"
    echo "One of the CPU is fixed in every pair. The other CPU is a thread from a core in the system."
    echo ""
    echo "Usage : $0 [-a CPUA] [-t Timeout] [-l Logdirectory] [-z LastCPU]"
    echo "where"
    printf "\tCPUA          = The fixed CPU in the pair of CPUs to which one processes/threads \n"
    printf "\t                of the context_switch2 program be affined.\n"
    printf "\t                The other CPU will be varied by the test\n"
    printf "\tTimeout       = Number of seconds that the context_switch2 program should run\n"
    printf "\t Logdirectory = The directory where all the logs should be saved\n"
    printf "\t LastCPU      = The CPU-Id beyond which we don't run this test\n"
}

while getopts "ha:t:l:z:" opt;
do
    case ${opt} in
	 h )
	    print_usage
	    exit 0
	    ;;
	 a )
	     CPUA=$OPTARG
	     ;;
	 t )
	     TIMEOUT=$OPTARG
	     ;;
	 l )
	     LOGDIR=$OPTARG
	     ;;
	 z )
	     MAXCPU=$OPTARG
	     ;;

	 \? )
	     echo "Unknown Option $OPTARG"
	     print_usage
	     exit 0
	     ;;
	 : )
	    echo "Invalid option. $OPTARG requires an argument"
	    print_usage
	    exit 0
	     ;;
	 esac
done
shift $((OPTIND - 1))


TPC=`lscpu | grep "Thread(s) per core:" | cut -d':' -f2 | awk '{print $1}'`
CPUB=0

make clean; make

if [ ! -d "$LOGDIR" ]
then
    mkdir $LOGDIR
fi

while [ $CPUB -le $MAXCPU ]
do
    for IDLEENABLED in 0 1
    do
	echo "CPU$CPUA-CPU$CPUB : Idle-Enabled=$IDLEENABLED"
	DIR=$LOGDIR/CPUS-$CPUA-$CPUB

	if [ ! -d "$DIR" ]
	then
	    mkdir $DIR
	fi

	if [ "$IDLEENABLED" -eq "0" ]
	then
	    TESTNAME="only_snooze_enabled"
	else
	    TESTNAME="all_states_enabled"
	fi

	rm -rf $DIR/$TESTNAME
	mkdir $DIR/$TESTNAME
	
	$RUNONE -e $IDLEENABLED -a $CPUA -b $CPUB -t $TIMEOUT -l $DIR/$TESTNAME
    done

    CPUB=`expr $CPUB + $TPC`
done

echo "=========================================="
echo "Tests are done. The logs are in $LOGDIR"
echo "=========================================="


