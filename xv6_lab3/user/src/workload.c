#include "fs.h"
#include "sched.h"
#include "stat.h"
#include "types.h"
#include "user.h"

#define NTASKS 64
#define NBENCHMARKS 4
#define NARGS 4

char *init_tasks[NBENCHMARKS][NARGS] = {
	{ "mmult", "0" },
	{ "compress", "compress" },
	{ "ping", "0", "0" },
	{ "networkd", "0", "0" },
};

char *scheduler_tasks[NTASKS][NARGS] = {
	{ "networkd", "20", "20" }, { "networkd", "10", "40" },
	{ "mmult", "368" },	    { "ping", "16", "26" },
	{ "mmult", "232" },	    { "mmult", "296" },
	{ "mmult", "80" },	    { "mmult", "304" },
	{ "compress", "compress" }, { "mmult", "216" },
	{ "ping", "20", "28" },	    { "ping", "8", "22" },
	{ "mmult", "24" },	    { "compress", "sh" },
	{ "mmult", "312" },	    { "mmult", "344" },
	{ "mmult", "56" },	    { "mmult", "136" },
	{ "mmult", "200" },	    { "compress", "workload" },
	{ "mmult", "240" },	    { "mmult", "336" },
	{ "mmult", "360" },	    { "mmult", "224" },
	{ "mmult", "104" },	    { "mmult", "72" },
	{ "mmult", "16" },	    { "mmult", "96" },
	{ "mmult", "48" },	    { "mmult", "112" },
	{ "mmult", "32" },	    { "mmult", "248" },
	{ "mmult", "88" },	    { "mmult", "8" },
	{ "compress", "mmult" },    { "mmult", "160" },
	{ "mmult", "184" },	    { "mmult", "208" },
	{ "mmult", "144" },	    { "mmult", "168" },
	{ "mmult", "120" },	    { "mmult", "152" },
	{ "mmult", "128" },	    { "ping", "4", "20" },
	{ "ping", "12", "24" },	    { "mmult", "256" },
	{ "mmult", "192" },	    { "compress", "compress" },
	{ "mmult", "280" },	    { "mmult", "320" },
	{ "mmult", "40" },	    { "mmult", "176" },
	{ "ping", "28", "32" },	    { "mmult", "264" },
	{ "ping", "24", "30" },	    { "mmult", "64" },
	{ "mmult", "272" },	    { "mmult", "328" },
	{ "ping", "32", "34" },	    { "compress", "workload" },
	{ "mmult", "352" },	    { "compress", "ping" },
	{ "mmult", "288" },	    { "compress", "sh" }
};

void nwait(uint n)
{
	struct schedinfo info = (struct schedinfo){ 0 };
	(void)info;
	for (; n != 0; n--) {
		// TODO replace wait with waitinfo once it is implemented
		int pid = waitinfo(&info);
		//int pid = wait();
		printf(1,
		       "[%d] creation %d, exit %d, response %d, execution %d, wait %d, io %d\n",
		       pid, info.creation_time, info.exit_time,
		       info.response_time, info.execution_time, info.wait_time,
		       info.io_time);
	}
}

void spawn_tasks(char **args, int sched)
{
	int pid = fork();
	if (pid < 0) {
		printf(1, "fork failed\n");
		exit();
	}
	if (pid == 0) {
		setscheduler(getpid(), sched, 0);
		exec(args[0], args);
		exit();
	}
}

void benchmark(int scheduler)
{
	// make the workload spawner high-priority

	setscheduler(getpid(), scheduler, 42);

	for (int t = 0; t < NTASKS; t++) {
		// sequentially schedule tasks
		spawn_tasks((char **)scheduler_tasks[t], scheduler);
		sleep(1);
	}

	nwait(NTASKS);
}

void init()
{
	// initialze all the tasks so that their
	// files are loaded from disk and do not stall exec
	for (int n = 0; n < NBENCHMARKS; n++) {
		int pid = fork();
		if (pid < 0) {
			printf(1, "fork failed\n");
			exit();
		}
		if (pid == 0) {
			exec(init_tasks[n][0], init_tasks[n]);
			exit();
		}
		wait();
	}
}

int main(int argc, char *argv[])
{
	uint start_time, end_time;

	init();

	/********** FIFO SCHEDULER **********/
	printf(1, "*******************************\n");
	printf(1, "FIFO\n");

	start_time = uptime();
	benchmark(SCHED_FIFO);
	end_time = uptime();

	printf(1, "FIFO total time ticks: %d\n", end_time - start_time);

	/********** ROUND ROBIN SCHEDULER **********/
	printf(1, "*******************************\n");
	printf(1, "ROUND ROBIN\n");

	start_time = uptime();
	benchmark(SCHED_RR);
	end_time = uptime();

	printf(1, "RR total time ticks: %d\n", end_time - start_time);

	/********** OTHER SCHEDULER **********/
	printf(1, "*******************************\n");
	printf(1, "OTHER\n");

	start_time = uptime();
	benchmark(SCHED_OTHER);
	end_time = uptime();

	printf(1, "OTHER total time ticks: %d\n", end_time - start_time);
	exit();
}