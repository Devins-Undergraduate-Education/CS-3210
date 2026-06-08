#include "fs.h"
#include "sched.h"
#include "stat.h"
#include "types.h"
#include "user.h"

#define MSG_SIZE 20

int main(int argc, char *argv[])
{
	int pipe1[2], pipe2[2];
	int pid;
	char msg[MSG_SIZE];

	if (argc != 3) {
		printf(2, "Usage: %s <num_pings> <sleep_time> \n", argv[0]);
		exit();
	}

	int num_pings = atoi(argv[1]);
	int sleep_time = atoi(argv[2]);

	// Create two pipes
	if (pipe(pipe1) < 0 || pipe(pipe2) < 0) {
		printf(2, "pipe error\n");
		exit();
	}

	// Create a child process
	if ((pid = fork()) < 0) {
		printf(2, "fork error\n");
		exit();
	}

	if (pid == 0) {
		// Child process (pong)
		close(pipe1[1]); // Close write end of pipe1
		close(pipe2[0]); // Close read end of pipe2

		for (int i = 0; i < num_pings; i++) {
			sleep(sleep_time);

			// Read from pipe1
			if (read(pipe1[0], msg, MSG_SIZE) <= 0) {
				printf(2, "read error");
				exit();
			}

			// printf (1, "Child received: %s\n", msg);

			// Write to pipe2
			memset(msg, 0, MSG_SIZE);
			itoa(i, msg);
			int msg_len = strlen(msg);
			strcpy(msg + msg_len, "-ping");

			if (write(pipe2[1], msg, strlen(msg) + 1) < 0) {
				printf(2, "write error");
				exit();
			}
		}

		close(pipe1[0]);
		close(pipe2[1]);
	} else {
		// Parent process (ping)
		close(pipe1[0]); // Close read end of pipe1
		close(pipe2[1]); // Close write end of pipe2

		for (int i = 0; i < num_pings; i++) {
			memset(msg, 0, MSG_SIZE);
			itoa(i, msg);
			int msg_len = strlen(msg);
			strcpy(msg + msg_len, "-ping");

			// Write to pipe1
			if (write(pipe1[1], msg, strlen(msg) + 1) < 0) {
				printf(2, "write error");
				exit();
			}

			// Read from pipe2
			if (read(pipe2[0], msg, MSG_SIZE) <= 0) {
				printf(2, "read error");
				exit();
			}

			// printf (1, "Parent received: %s\n", msg);
		}
		close(pipe1[1]);
		close(pipe2[0]);
		wait();
	}

	exit();
}
