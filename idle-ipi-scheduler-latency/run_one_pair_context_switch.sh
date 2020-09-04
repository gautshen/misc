#!/bin/bash

###############################################################################################################################
#
# run_one_pair_context_switch.sh:
#
# Author : Gautham R. Shenoy <ego@linux.vnet.ibm.com>
#
# Script to run context_switch2 between a pair of CPUs and captures
# the interrupt statistics and the idle statistics on those CPUs.
#
# Usage : ./run_one_pair_context_switch.sh [-h] [-a CPUA] [-b CPUB] [-t Timeout] -l Logdirectory -e All_Idle_States_Enabled
# where
#    CPUA, CPUB              = are the CPUs to which the two processes/threads
#                              of the context_switch2 program are respectively affined.
#
#    Timeout                 = Number of seconds that the context_switch2 program should run
#
#    Logdirectory            = Directory where the output logs need to be saved
#
#    All_Idle_States_Enabled = If this value is non-zero, then on CPUA and CPUB,
#                              all the idle states will be enabled during the test.
#                              If this value is zero, then only snooze will be enabled
#                              on CPUA and CPUB during the test
#
###############################################################################################################################


CONTEXT_SWITCH=./context_switch2


CPUA=0
CPUB=1
TIMEOUT=10
LOGDIR=""
ALLENABLED=""

function print_usage {
    echo "Usage : $0 [-h] [-a CPUA] [-b CPUB] [-t Timeout] -l Logdirectory -e All_Idle_State_Enabled"
    echo "where"
    printf "\tCPUA, CPUB: are the CPUs to which the two processes/threads \n"
    printf "\t              of the context_switch2 program are respectively affined.\n"
    printf "\tTimeout = Number of seconds that the context_switch2 program should run\n"
}

#### Option Parser code ######
while getopts "ha:b:t:l:e:" opt;
do
    case ${opt} in
	 h )
	    print_usage
	    exit 0
	    ;;
	 a )
	     CPUA=$OPTARG
	     ;;
	 b )
	     CPUB=$OPTARG
	     ;;

	 t )
	     TIMEOUT=$OPTARG
	     ;;
	 l )
	     LOGDIR=$OPTARG
	     ;;
	 e )
	     ALLENABLED=$OPTARG
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


### LOGDIR is a mandatory argument ###
if [ "$LOGDIR" = "" ]
then
    echo "$0 : Log Directory needs to be provided"
    print_usage
    exit 0
fi


### So is whether we need to enable all the idle states or not
if [ "$ALLENABLED" = "" ]
then
    echo "$0 : Specify if the test should run with all idle states enabled or not"
    print_usage
    exit 0
fi

### Create the logdirectory if it doesn't exist
if [ ! -d "$LOGDIR" ]
then
    mkdir $LOGDIR
fi


# Uncomment this for debug
#echo "$0 :  CPUA=$CPUA, CPUB=$CPUB, TIMEOUT=$TIMEOUT LOGDIR=$LOGDIR ALLENABLED=$ALLENABLED"

#### Define a bunch of arrays to store the cpuidle properties for various states.
#### The indices/keys to these arrays are the state ids.
declare -A names               # Names of the idle states
 
declare -A CPUA_disabled       # If an idle state is disabled or not
declare -A CPUB_disabled

declare -A CPUA_usagebefore    # Usage of the idle state before the test
declare -A CPUB_usagebefore

declare -A CPUA_timebefore     # Amount of time spent in the state before the test
declare -A CPUB_timebefore

declare -A CPUA_usageafter     # Usage of the idle state before the test
declare -A CPUB_usageafter

declare -A CPUA_timeafter      # Amount of time spent in the state before the test
declare -A CPUB_timeafter



###
# Helper function to enable only one cpuidle state for a given CPU,
# and disable the idle other states
#
# $1 = CPU for which the sole idle state needs to be enabled
# $2 = The index of the sole idle state to be enabled.
function cpu_enable_only_one_state {
    CPU=$1
    NRSTATES=`ls -1 /sys/devices/system/cpu/cpu$CPU/cpuidle/ | grep state*| wc -l`
    FIRST=0
    ((LAST = NRSTATES - 1))
    STATE=$2


    for i in `seq $FIRST $LAST`
    do
	if [ $i -eq $STATE ]
	then
	    echo 0 > /sys/devices/system/cpu/cpu$CPU/cpuidle/state$i/disable
	else
	    echo 1 > /sys/devices/system/cpu/cpu$CPU/cpuidle/state$i/disable
	fi
    done
}

###
# Helper function to enable all the idle other states for a given CPU
#
# $1 = CPU for which all the idle state needs to be enabled
#
function cpu_enable_all_states {
    CPU=$1
    NRSTATES=`ls -1 /sys/devices/system/cpu/cpu$CPU/cpuidle/ | grep state*| wc -l`
    FIRST=0
    ((LAST = NRSTATES - 1))


    for i in `seq $FIRST $LAST`
    do
	echo 0 > /sys/devices/system/cpu/cpu$CPU/cpuidle/state$i/disable
    done

}

###
# Helper store the values of a given cpuidle property for all the idle states for a given CPU
#
# $1 = CPU for which the cpuidle property needs to be captured
# $2 = The name of the cpuidle property that needs to be captured
# $3 = The associative array in which the values of the property are stored,
#      indexed by the id of the state
#
function cpu_get_cpuidle_prop_array {
    CPU=$1
    prop=$2
    declare -n retarr="$3"

    NRSTATES=`ls -1 /sys/devices/system/cpu/cpu$CPU/cpuidle/ | grep state*| wc -l`
    FIRST=0
    ((LAST = NRSTATES - 1))


    for i in `seq $FIRST $LAST`
    do
	val=`taskset -c $CPU cat /sys/devices/system/cpu/cpu$CPU/cpuidle/state$i/$prop`
	retarr[$i]=$val
    done
}


###
# Helper function to print the cpu-idle statistics for the duration of the test-run on a given CPU
#
# $1 = CPU for which the cpuidle statistics need to be printed
# $2 = The associativity array with the names of the cpuidle states
# $3 = The associativity array whose values indicate whether the cpuidle state of a
#      correponding index was disabled or not.
#
# $4 = The associative array with the usage count of the idle states on this CPU prior to the test run
# $5 = The associative array with the residency-value (in us) of the idle states on this CPU after to the test run
# $6 = The associative array with the usage count of the idle states on this CPU prior to the test run
# $7 = The associative array with the residency-value (in us) of the idle states on this CPU after to the test run
#
function cpu_print_cpuidle_summary {
    CPU=$1
    declare -n namearr="$2"
    declare -n disabledarr="$3"
    declare -n usagebeforearr="$4"
    declare -n usageafterarr="$5"
    declare -n timebeforearr="$6"
    declare -n timeafterarr="$7"

    NRSTATES=`ls -1 /sys/devices/system/cpu/cpu$CPU/cpuidle/ | grep state*| wc -l`
    FIRST=0
    ((LAST = NRSTATES - 1))

    for i in `seq $FIRST $LAST`
    do
	name=`echo ${namearr[$i]}`
	disableval=`echo ${disabledarr[$i]}`
	status="disabled"
	if [ $disableval -eq "0" ]
	then
	    status="enabled "
	fi

	usagediff=`expr  ${usageafterarr[$i]} - ${usagebeforearr[$i]}`
	timediff=`expr  ${timeafterarr[$i]} - ${timebeforearr[$i]}`

	echo "CPU$CPU: $name ($status), Usage = $usagediff times, Time = $timediff us"
    done

}


## Enable/Disable the non-snooze states depending on what type of test it is
if [ $ALLENABLED -eq "0" ]
then
    cpu_enable_only_one_state $CPUA 0
    cpu_enable_only_one_state $CPUB 0
else
    cpu_enable_all_states $CPUA
    cpu_enable_all_states $CPUB
fi


# Capture the interrupts before
cat /proc/interrupts > $LOGDIR/proc_interrupts_before

NRSTATES=`ls -1 /sys/devices/system/cpu/cpu$CPUA/cpuidle/ | grep state*| wc -l`
FIRST=0
((LAST = NRSTATES - 1))

#Capture the cpuidle statistics on both CPUs prior to the test
cpu_get_cpuidle_prop_array $CPUA name names

cpu_get_cpuidle_prop_array $CPUA disable CPUA_disabled
cpu_get_cpuidle_prop_array $CPUB disable CPUB_disabled


cpu_get_cpuidle_prop_array $CPUA usage CPUA_usagebefore
cpu_get_cpuidle_prop_array $CPUB usage CPUB_usagebefore

cpu_get_cpuidle_prop_array $CPUA time CPUA_timebefore
cpu_get_cpuidle_prop_array $CPUB time CPUB_timebefore

#Run the context switch test
$CONTEXT_SWITCH --timeout=$TIMEOUT $CPUA $CPUB > $LOGDIR/context_switch.log

#Capture the cpuidle statistics on both CPUs after the test
cpu_get_cpuidle_prop_array $CPUA usage CPUA_usageafter
cpu_get_cpuidle_prop_array $CPUB usage CPUB_usageafter

cpu_get_cpuidle_prop_array $CPUA time CPUA_timeafter
cpu_get_cpuidle_prop_array $CPUB time CPUB_timeafter

# Capture the interrupts after the test
cat /proc/interrupts > $LOGDIR/proc_interrupts_after

#Summarize the interrupts generated during the duration of the test into a separate file
python proc_interrupts_parse.py -b $LOGDIR/proc_interrupts_before -a $LOGDIR/proc_interrupts_after > $LOGDIR/proc_interrupts_summary

#Summarize the CPU idle statistics for each of the CPUs in their respective files.
cpu_print_cpuidle_summary $CPUA names CPUA_disabled CPUA_usagebefore CPUA_usageafter CPUA_timebefore CPUA_timeafter > $LOGDIR/CPU$CPUA\_idle_state_summary

cpu_print_cpuidle_summary $CPUB names CPUB_disabled CPUB_usagebefore CPUB_usageafter CPUB_timebefore CPUB_timeafter > $LOGDIR/CPU$CPUB\_idle_state_summary

# Enable all the idle states on both CPUs
cpu_enable_all_states $CPUA
cpu_enable_all_states $CPUB




