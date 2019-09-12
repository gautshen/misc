#!/usr/bin/python3

######################################################################
# purr_tb.py : A utility for validating whether the sum of the
#              values of the PURR register across the active threads
#              of cores over a time iterval matches the number of
#              timebase ticks for that interval.
#
# Authors:
#         Kamalesh Babulal <kamalesh@linux.vnet.ibm.com>
#         Gautham R. Shenoy <ego@linux.vnet.ibm.com>
#
# (C) Copyright IBM Corp, 2019
######################################################################
import os, time, subprocess, getopt, sys

online_cpus=[]
PURR={}
PURR_new={}
cores_list = {}

###################################################
# Computes the list of online CPUs
###################################################
def get_online_cpus():
    with open('/proc/cpuinfo', 'r') as f:
        for line in f.readlines():
            if line.startswith('processor'):
                cpu = int(line.split()[2])
                online_cpus.append(cpu)

########################################################################
# Returns a string of online threads of the core @tl from the cores_map
########################################################################
def stringify(tl):
    retstring='['
    for t in tl:
        if int(t) in online_cpus:
            retstring = retstring + "%3d," %(int(t))
    last_comma_id=retstring.rfind(',')
    retstring = retstring[0:last_comma_id] + "]"
    return retstring

########################################################################
# Determines the map of active cores to their constituent threads
# from the ppc64_cpu utility
#
# The map is saved in the global variable cores_list{}
########################################################################
def get_core_map():
    cores_str = str(subprocess.check_output('ppc64_cpu --info', shell=True)).split('\n')
    active_cores_list = map(lambda c:c.replace('*', ''),
                            filter(lambda c:c.find('*') >= 0, cores_str))
    idx = 0
    for core in active_cores_list:
        if (core.find('Core') == -1):
            continue
        tmp = core.split(':')[1]
        tmp = ' '.join(tmp.split())
        tmp = tmp.split('\'')[0].replace('\n', '').split(" ")
        cores_list[idx] = tmp
        idx = idx + 1

########################################################################
#  Prints the following banner:
#  =========================================
#  Core      delta tb(apprx)    delta purr
#  =========================================
########################################################################
def print_banner():
    banner = ""
    header = ""
    if len(cores_list) == 0:
        return

    banner = "Core"
    mystring = "Core%02d %s" %(0, stringify(cores_list[0]))

    paddinglen = len(mystring) - len(banner)
    padding = "".join([' ' for i  in range(0, paddinglen)])
    banner = banner + padding + "\t%s\t\t%s\t" %("delta tb(apprx)", "delta purr")
    header = "".join(['=' for i in range(0, len(banner) + 16)])

    print(header)
    print(banner)
    print(header)

########################################################################
#  For a CPU @c, read the PURR value from sysfs
########################################################################    
def read_purr(c):
    cpu = str(c)
    with open('/sys/devices/system/cpu/cpu'+cpu+'/purr') as f:
        purr = int(f.readline().rstrip('\n'), 16)
    return purr

def help():
    print('monitor.py -i <interval seconds> -s <samples count>\n')

########################################################################
# Parse commandline options and update the value of samples and
# interval
########################################################################    
def parse_cmdline():
    samples=10
    interval=1 #seconds
    try:
        options, others = getopt.getopt(
                sys.argv[1:],
                'hi:s:',
                ['interval=',
                 'samples='])
    except getopt.GetoptError as err:
        help()
        sys.exit(1)

    for opt, arg in options:
        if opt in ('-i', '--interval'):
            interval = int(arg)
        elif opt in ('-s', '--samples'):
            samples = int(arg)
        elif opt in ('-h', '--help'):
            help()
            sys.exit(0)
    return interval, samples

#####################################################################
# For each time interval, compute the sum of purr increments for each
# online thread in each active core and print the sum of purr
# increments in comparison with the tb ticks elapsed in that interval.
#
# Expectation:
# sum of purr increments in an active core == elapsed tb-ticks
#####################################################################
def validate():
    for i in range(0, samples):
        old_sec = int(time.time())
        for cpu in online_cpus:
            PURR[cpu] = read_purr(cpu)

        time.sleep(interval)

        now_sec = int(time.time())
        delta_sec = now_sec - old_sec
        delta_tb = delta_sec * 512000000
        for cpu in online_cpus:
            PURR_new[cpu] = read_purr(cpu)

        for key,value in cores_list.items():
            purr_total = 0
            for cpustr in value:
                cpu = int (cpustr)
                if cpu not in online_cpus:
                    continue
                purr_total += PURR_new[cpu] - PURR[cpu]
            print("core%02d %s\t%d\t\t%d\t" % (key, stringify(cores_list[key]), delta_tb, purr_total))
        print("")


interval,samples=parse_cmdline()
get_online_cpus()
get_core_map()
print_banner()
validate()

sys.exit(0)
