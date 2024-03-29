/*****************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2007-2009  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  ---
 *
 *  The license mentioned above concerns the source code only. Using the
 *  EtherCAT technology and brand is only permitted in compliance with the
 *  industrial property and similar rights of Beckhoff Automation GmbH.
 *
 ****************************************************************************/

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <malloc.h>

/****************************************************************************/
#include "share.h"
#include "ecrt.h"

/****************************************************************************/

// Application parameters
#define FREQUENCY 1000
#define CLOCK_TO_USE CLOCK_REALTIME
#define MEASURE_TIMING

/****************************************************************************/

#define NSEC_PER_SEC (1000000000L)
#define PERIOD_NS (NSEC_PER_SEC / FREQUENCY)

#define DIFF_NS(A, B) (((B).tv_sec - (A).tv_sec) * NSEC_PER_SEC + \
	(B).tv_nsec - (A).tv_nsec)

#define TIMESPEC2NS(T) ((uint64_t) (T).tv_sec * NSEC_PER_SEC + (T).tv_nsec)

/****************************************************************************/

// EtherCAT
static ec_master_t *master = NULL;
static ec_master_state_t master_state = {};

static ec_domain_t *domain1 = NULL;
static ec_domain_state_t domain1_state = {};
static ec_slave_config_t *sc_ana_in = NULL;
static ec_slave_config_state_t sc_ana_in_state = {};


/****************************************************************************/

// process data
static uint8_t *domain1_pd = NULL;

#define BusCouplerPos    0, 0
#define DigOutSlavePos   0, 1
#define CounterSlavePos  0, 2

#define Beckhoff_Slave_0 0x0000066f, 0x515050a1 
//#define Beckhoff_Slave_0 0x0000066f,  0x525100a1
#define Beckhoff_EK1100 0x00000002, 0x044c2c52
#define Beckhoff_EL2008 0x00000002, 0x07d83052
#define IDS_Counter     0x000012ad, 0x05de3052

// offsets for PDO entries
static int off_dig_out;
static int off_counter_in;
static int off_counter_out;

static unsigned int counter = 0;
static unsigned int blink = 0;
static unsigned int sync_ref_counter = 0;
const struct timespec cycletime = {0, PERIOD_NS};



ShareData *data;
static unsigned int off_slave_0_Controlword;
static unsigned int off_slave_0_operation_mode;
static unsigned int off_slave_0_target_position;
static unsigned int off_slave_0_Statusword;
static unsigned int off_slave_0_Modes;
static unsigned int off_slave_0_position_actual_value;

const static ec_pdo_entry_reg_t domain1_regs[] = {
    	{0,0,  Beckhoff_Slave_0, 0x6040, 0, &off_slave_0_Controlword},
	{0,0,  Beckhoff_Slave_0, 0x6060, 0, &off_slave_0_operation_mode},
	{0,0,  Beckhoff_Slave_0, 0x607a, 0, &off_slave_0_target_position},
	{0,0,  Beckhoff_Slave_0, 0x6041, 0, &off_slave_0_Statusword},
	{0,0,  Beckhoff_Slave_0, 0x6061, 0, &off_slave_0_Modes},
	{0,0,  Beckhoff_Slave_0, 0x6064, 0, &off_slave_0_position_actual_value},
   // {AnaInSlavePos,  Beckhoff_EL3102, 0x3101, 2, &off_ana_in_value},
   // {AnaOutSlavePos, Beckhoff_EL4102, 0x3001, 1, &off_ana_out},
   // {DigOutSlavePos, Beckhoff_EL2032, 0x3001, 1, &off_dig_out},
    {}
};
static ec_pdo_entry_info_t slave_0_pdo_entries[] = {
    {0x6040, 0x00, 16}, /* Controlword */
    {0x6060, 0x00, 8}, /* Modes of operation */
    {0x607a, 0x00, 32}, /* Target position */
    {0x60b8, 0x00, 16}, /* Touch probe function */
    {0x603f, 0x00, 16}, /* Error code */
    {0x6041, 0x00, 16}, /* Statusword */
    {0x6061, 0x00, 8}, /* Modes of operation display */
    {0x6064, 0x00, 32}, /* Position actual value */
    {0x60b9, 0x00, 16}, /* Touch probe status */
    {0x60ba, 0x00, 32}, /* Touch probe pos1 pos value */
    {0x60f4, 0x00, 32}, /* Following error actual value */
    {0x60fd, 0x00, 32}, /* Digital inputs */
};

static ec_pdo_info_t slave_0_pdos[] = {
    {0x1600, 4, slave_0_pdo_entries + 0}, /* Receive PDO mapping 1 */
    {0x1a00, 8, slave_0_pdo_entries + 4}, /* Transmit PDO mapping 1 */
};

static ec_sync_info_t slave_0_syncs[] = {
    {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE},
    {1, EC_DIR_INPUT, 0, NULL, EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, slave_0_pdos + 0, EC_WD_ENABLE},
    {3, EC_DIR_INPUT, 1, slave_0_pdos + 1, EC_WD_DISABLE},
    {0xff}
};
/*****************************************************************************/

struct timespec timespec_add(struct timespec time1, struct timespec time2)
{
	struct timespec result;

	if ((time1.tv_nsec + time2.tv_nsec) >= NSEC_PER_SEC) {
		result.tv_sec = time1.tv_sec + time2.tv_sec + 1;
		result.tv_nsec = time1.tv_nsec + time2.tv_nsec - NSEC_PER_SEC;
	} else {
		result.tv_sec = time1.tv_sec + time2.tv_sec;
		result.tv_nsec = time1.tv_nsec + time2.tv_nsec;
	}

	return result;
}

/*****************************************************************************/

void check_domain1_state(void)
{
    ec_domain_state_t ds;

    ecrt_domain_state(domain1, &ds);

    if (ds.working_counter != domain1_state.working_counter)
        printf("Domain1: WC %u.\n", ds.working_counter);
    if (ds.wc_state != domain1_state.wc_state)
        printf("Domain1: State %u.\n", ds.wc_state);

    domain1_state = ds;
}

/*****************************************************************************/

void check_master_state(void)
{
    ec_master_state_t ms;

    ecrt_master_state(master, &ms);

	if (ms.slaves_responding != master_state.slaves_responding)
        printf("%u slave(s).\n", ms.slaves_responding);
    if (ms.al_states != master_state.al_states)
        printf("AL states: 0x%02X.\n", ms.al_states);
    if (ms.link_up != master_state.link_up)
        printf("Link is %s.\n", ms.link_up ? "up" : "down");

    master_state = ms;
}

/****************************************************************************/

void cyclic_task()
{
    struct timespec wakeupTime, time;
#ifdef MEASURE_TIMING
    struct timespec startTime, endTime, lastStartTime = {};
    uint32_t period_ns = 0, exec_ns = 0, latency_ns = 0,
             latency_min_ns = 0, latency_max_ns = 0,
             period_min_ns = 0, period_max_ns = 0,
             exec_min_ns = 0, exec_max_ns = 0;
#endif

    // get current time
    clock_gettime(CLOCK_TO_USE, &wakeupTime);

	while(1) {
		wakeupTime = timespec_add(wakeupTime, cycletime);
        clock_nanosleep(CLOCK_TO_USE, TIMER_ABSTIME, &wakeupTime, NULL);

#ifdef MEASURE_TIMING
        clock_gettime(CLOCK_TO_USE, &startTime);
        latency_ns = DIFF_NS(wakeupTime, startTime);
        period_ns = DIFF_NS(lastStartTime, startTime);
        exec_ns = DIFF_NS(lastStartTime, endTime);
        lastStartTime = startTime;

        if (latency_ns > latency_max_ns) {
            latency_max_ns = latency_ns;
        }
        if (latency_ns < latency_min_ns) {
            latency_min_ns = latency_ns;
        }
        if (period_ns > period_max_ns) {
            period_max_ns = period_ns;
        }
        if (period_ns < period_min_ns) {
            period_min_ns = period_ns;
        }
        if (exec_ns > exec_max_ns) {
            exec_max_ns = exec_ns;
        }
        if (exec_ns < exec_min_ns) {
            exec_min_ns = exec_ns;
        }
#endif

		// receive process data
		ecrt_master_receive(master);
		ecrt_domain_process(domain1);
	

		// check process data state (optional)
		check_domain1_state();

   		data->statusword = EC_READ_U16(domain1_pd+off_slave_0_Statusword);
   		data->mode_display = EC_READ_U8(domain1_pd+off_slave_0_Modes);
   		data->actual_position = EC_READ_S32(domain1_pd+off_slave_0_position_actual_value);


		if (counter) {
			counter--;
		} else { // do this at 1 Hz
			counter = FREQUENCY;

			// check for master state (optional)
			check_master_state();

#ifdef MEASURE_TIMING
            // output timing stats
            printf("period     %10u ... %10u\n",
                    period_min_ns, period_max_ns);
            printf("exec       %10u ... %10u\n",
                    exec_min_ns, exec_max_ns);
            printf("latency    %10u ... %10u\n",
                    latency_min_ns, latency_max_ns);
            period_max_ns = 0;
            period_min_ns = 0xffffffff;
            exec_max_ns = 0;
            exec_min_ns = 0xffffffff;
            latency_max_ns = 0;
            latency_min_ns = 0xffffffff;
#endif

			// calculate new process data
			blink = !blink;
		}

		// write process data
		//EC_WRITE_U8(domain1_pd + off_dig_out, blink ? 0x66 : 0x99);
		//EC_WRITE_U8(domain1_pd + off_counter_out, blink ? 0x00 : 0x02);

		if((1==data->clock_wise_turn)&&(0==data->stop))
		{
			data->target_position += 100;
		}
		else if((1==data->unticlock_wise_turn)&&(0==data->stop))
		{
			data->target_position -= 100;
		}
		else
		{
			//data->target_position = data->actual_position;
		}
		
		if(data->head == data->tail)
    		{
		    EC_WRITE_U16(domain1_pd + off_slave_0_Controlword,  data->controlword);
		    EC_WRITE_U8(domain1_pd + off_slave_0_operation_mode,  0x08);
		    EC_WRITE_S32(domain1_pd + off_slave_0_target_position,  data->target_position);
    		}
    
		// write application time to master
		clock_gettime(CLOCK_TO_USE, &time);
		ecrt_master_application_time(master, TIMESPEC2NS(time));

		if (sync_ref_counter) {
			sync_ref_counter--;
		} else {
			sync_ref_counter = 1; // sync every cycle
			ecrt_master_sync_reference_clock(master);
		}
		ecrt_master_sync_slave_clocks(master);

		// send process data



		ecrt_domain_queue(domain1);
		ecrt_master_send(master);

#ifdef MEASURE_TIMING
        clock_gettime(CLOCK_TO_USE, &endTime);
#endif
	}
}

/****************************************************************************/

int main(int argc, char **argv)
{
    ec_slave_config_t *sc;

	if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
		perror("mlockall failed");
		return -1;
	}
    int    shmid;

    shmid = shmget(SHARE_DATA_SHARED_MEMORY_KEY, sizeof(ShareData), IPC_CREAT | 0666);
    if (shmid == -1)
    {
        perror("shmget err");
        return -1;
    }
    data = (ShareData*)shmat(shmid, NULL, 0);
    if (data == (void *)-1 )
    {
        perror("shmget err ");
	return -1;
    }
    master = ecrt_request_master(0);
    if (!master)
        return -1;

    domain1 = ecrt_master_create_domain(master);
    if (!domain1)
        return -1;

    // Create configuration for bus coupler


    if (!(sc_ana_in = ecrt_master_slave_config(
                    master, 0,0, Beckhoff_Slave_0))) {
        fprintf(stderr, "Failed to get slave configuration.\n");
        return -1;
    }
    printf("Configuring PDOs...\n");
    if (ecrt_slave_config_pdos(sc_ana_in, EC_END, slave_0_syncs)) {
        fprintf(stderr, "Failed to configure PDOs.\n");
        return -1;
    }			
     if (ecrt_domain_reg_pdo_entry_list(domain1, domain1_regs)) {
        fprintf(stderr, "PDO entry registration failed!\n");
        return -1;
    }

    // configure SYNC signals for this slave
	ecrt_slave_config_dc(sc_ana_in, 0x0300, PERIOD_NS, 4400000, 0, 0);

    printf("Activating master...\n");
    if (ecrt_master_activate(master))
        return -1;
 
	 
    if (!(domain1_pd = ecrt_domain_data(domain1))) {
        printf("domain1= %x domain_pd =%x \n",domain1,domain1_pd);
	return -1;
    }

    printf("failed to active master \n");	
    pid_t pid = getpid();
    if (setpriority(PRIO_PROCESS, pid, -19))
        fprintf(stderr, "Warning: Failed to set priority: %s\n",
                strerror(errno));

	printf("Starting cyclic function.\n");
    cyclic_task();

    return 0;
}

/****************************************************************************/
