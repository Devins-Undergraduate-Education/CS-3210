#include "fcntl.h"
#include "fs.h"
#include "sched.h"
#include "stat.h"
#include "types.h"
#include "user.h"

int main(int argc, char *argv[])
{
	if (argc != 3) {
		printf(2, "Usage: %s <nworkers> <sleep> \n", argv[0]);
		return 1;
	}

	int nworkers = atoi(argv[1]);
	int sleeptime = atoi(argv[2]);

  // spawn N workers that will sleep for an
  // incremental time and then wake up to do work
  // simulating a daemon process

	for (int n = 0; n < nworkers; n++) {
		int pid = fork();
		if (pid < 0) {
			printf(1, "fork failed\n");
			exit();
		}
		if (pid == 0) {
			sleep(n * sleeptime);

			// do some useful work
			double sum = 1;
			for (double i = 0; i < 1000000; i++) {
				sum += i * i;
			}

			exit();
		}
	}

	for (int n = 0; n < nworkers; n++) {
		wait();
	}

	exit();
}