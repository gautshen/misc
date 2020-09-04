#!/bin/bash

###############################################################################################################################
#
# postprocess_data.sh
#
# Author : Gautham R. Shenoy <ego@linux.vnet.ibm.com>
#
# Script to post-process the results of run_all_context_switch.sh
#
# Usage : ./postprocess_data.sh -l Logdirectory -o SummaryDir
# where
#
#    Logdirectory = The directory containing the logs of run_all_context_switch.sh
#
#    SummaryDir   = The directory where the post-processed summary needs to be saved"
###############################################################################################################################


LOGDIR=""
OUTPUTDIR=""


function print_usage {
    echo "$0:"
    echo "Script to post-process the results of run_all_context_switch.sh"
    echo ""
    echo "Usage : $0 -l Logdirectory -o SummaryDir"
    echo "where"
    printf "\t Logdirectory = The directory containing the logs of run_all_context_switch.sh\n"
    printf "\t SummaryDir   = The directory where the post-processed summary needs to be saved"
}

while getopts "hl:o:" opt;
do
    case ${opt} in
	 h )
	    print_usage
	    exit 0
	    ;;
	 l )
	     LOGDIR=$OPTARG
	     ;;
	 o )
	     OUTPUTDIR=$OPTARG
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


if [ "$LOGDIR" = "" ]
then
    echo "$0 : Please provide the Log Directory"
    echo ""
    print_usage
    exit 1
fi

if [ "$OUTPUTDIR" = "" ]
then
    echo "$0 : Please provide the Output directory"
    echo ""
    print_usage
    exit 1
fi


pct_regression=("1" "2" "3" "5" "10" "20" "50" "100")

declare -A context_switch_dict
function compute_average {
    LOGFILE=$1
    key=$2

    TOTAL=`cat $LOGFILE | awk '{ s+= $1} END {print s}'`
    COUNT=`cat $LOGFILE | wc -l`
    AVG=`echo "scale=2; $TOTAL/$COUNT" | bc`
    context_switch_dict[$key]=$AVG
}

function process_one_logdir {

    CPUA=$1
    CPUB=$2
    DIR=$3
    
    compute_average $DIR/only_snooze_enabled/context_switch.log "only_snooze_enabled"
    compute_average $DIR/all_states_enabled/context_switch.log "all_states_enabled"

    snooze_avg=${context_switch_dict["only_snooze_enabled"]}
    all_states_avg=${context_switch_dict["all_states_enabled"]}

    denom=$snooze_avg

    #if [ $snooze_avg -ge $all_states_avg ]
    diff=`scale=2; echo "$snooze_avg - $all_states_avg" | bc`

    if (( $(echo "$denom > 0" | bc -l) ))
    then
	pct=`echo "scale=2; ($diff*100)/$denom" | bc`
    else
	pct="0"
    fi
    

    for fit in ${pct_regression[*]}
    do
	if (( $(echo "$pct <= $fit" | bc -l) ))
	then
	    break
	fi
    done

    PROCESSEDLOG=$OUTPUTDIR/$fit/CPUS-$CPUA-$CPUB-summary
    echo > $PROCESSEDLOG
    echo "==========================" >> $PROCESSEDLOG
    echo "Context Switch Information" >> $PROCESSEDLOG
    echo "==========================" >> $PROCESSEDLOG
    echo "With Only snooze Enabled     : $snooze_avg" >> $PROCESSEDLOG
    echo "With All Idle States Enabled : $all_states_avg" >> $PROCESSEDLOG
    echo "Percentage regression        : $pct %" >> $PROCESSEDLOG
    echo "==========================================" >> $PROCESSEDLOG
    echo "IRQ Information : With Only snooze Enabled" >> $PROCESSEDLOG
    echo "==========================================" >> $PROCESSEDLOG
    cat $DIR/only_snooze_enabled/proc_interrupts_summary | grep "CPU $CPUA:" >> $PROCESSEDLOG
    echo "-------" >> $PROCESSEDLOG
    cat $DIR/only_snooze_enabled/proc_interrupts_summary | grep "CPU $CPUB:" >> $PROCESSEDLOG

    echo "=========================================" >> $PROCESSEDLOG
    echo "IRQ Information : With All States Enabled" >> $PROCESSEDLOG
    echo "=========================================" >> $PROCESSEDLOG
    cat $DIR/all_states_enabled/proc_interrupts_summary | grep "CPU $CPUA:" >> $PROCESSEDLOG
    echo "-------" >> $PROCESSEDLOG
    cat $DIR/all_states_enabled/proc_interrupts_summary | grep "CPU $CPUB:" >> $PROCESSEDLOG
    

    echo "=================================================" >> $PROCESSEDLOG
    echo "Idle State information : With Only snooze Enabled" >> $PROCESSEDLOG
    echo "=================================================" >> $PROCESSEDLOG
    cat $DIR/only_snooze_enabled/CPU$CPUA\_idle_state_summary >> $PROCESSEDLOG
    echo "-------" >> $PROCESSEDLOG
    cat $DIR/only_snooze_enabled/CPU$CPUB\_idle_state_summary >> $PROCESSEDLOG

    echo "================================================" >> $PROCESSEDLOG
    echo "Idle State information : With All States Enabled" >> $PROCESSEDLOG
    echo "================================================" >> $PROCESSEDLOG
    cat $DIR/all_states_enabled/CPU$CPUA\_idle_state_summary >> $PROCESSEDLOG
    echo "-------" >> $PROCESSEDLOG
    cat $DIR/all_states_enabled/CPU$CPUB\_idle_state_summary >> $PROCESSEDLOG
}


mkdir $OUTPUTDIR

for fit in ${pct_regression[*]}
do
    rm -rf $OUTPUTDIR/$fit
    mkdir $OUTPUTDIR/$fit
done

for TOPDIR in `ls -1 $LOGDIR | grep "CPUS-[0-9]\+-[0-9]\+"`
do
    SPLIT=`echo $TOPDIR | sed -e 's/-/ /g'`
    CPUA=`echo $SPLIT | awk '{print $2}'`
    CPUB=`echo $SPLIT | awk '{print $3}'`

    process_one_logdir $CPUA $CPUB $LOGDIR/$TOPDIR
done

SUM=0
len=${#pct_regression[*]}
first=0
last=`expr $len - 1`

for i in `seq $first $last`
do
    fit=${pct_regression[$i]}
    COUNT=`ls -1 $OUTPUTDIR/$fit/ | grep "CPUS-[0-9]\+-[0-9]\+" | wc -l`
    SUM=`expr $SUM + $COUNT`
    if [ $i -eq $first ]
    then
	echo "<= $fit %age regression : $COUNT instances" | tee -a $OUTPUTDIR/distribution.txt
    else
	j=`expr $i - 1`
	prevfit=${pct_regression[$j]}
	echo "($prevfit - $fit] %age regression : $COUNT instances" | tee -a $OUTPUTDIR/distribution.txt
    fi
done

echo "====================================================="
echo "More details can be found in the $OUTPUTDIR directory"
echo "====================================================="
