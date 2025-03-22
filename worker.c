/* worker.c */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/shm.h>
#include <mqueue.h>
#include <time.h>
#include <fcntl.h>

#define NANOSECONDS_IN_SECOND 1000000000
#define MQ_NAME "/oss_mq"

// Structure for the simulated clock
typedef struct {
    int seconds;
    int nanoseconds;
} SysClock;

long long toNanoseconds(int sec, int nano) {
    return (long long)sec * NANOSECONDS_IN_SECOND + nano;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <sec> <nano> <shm_key>\n", argv[0]);
        exit(1);
    }

    int waitSeconds = atoi(argv[1]);
    int waitNanoseconds = atoi(argv[2]);
    key_t shm_key = atoi(argv[3]);

    printf("WORKER PID: %d - Attempting to attach to shared memory ID: %d\n", getpid(), shm_key);
    int retries = 5;
    int shm_id;
    while ((shm_id = shmget(shm_key, sizeof(SysClock), IPC_CREAT | 0666)) == -1 && retries > 0) {
        perror("Worker failed to attach to shared memory, retrying...");
        sleep(1);  // Give OSS time to create the memory
        retries--;
    }
    if (shm_id == -1) {
        perror("Worker failed to attach to shared memory after retries");
        exit(1);
    }

    SysClock* clock_ptr = (SysClock*)shmat(shm_id, NULL, 0);
    if (clock_ptr == (void*)-1) {
        perror("shmat failed");
        exit(1);
    }

    mqd_t mq = mq_open(MQ_NAME, O_RDWR);
    if (mq == (mqd_t)-1) {
        perror("Failed to open message queue");
        shmdt(clock_ptr);
        exit(1);
    }

    long long targetTime = toNanoseconds(clock_ptr->seconds, clock_ptr->nanoseconds) + toNanoseconds(waitSeconds, waitNanoseconds);
    printf("WORKER PID: %d PPID: %d StartTimeS: %d StartTimeNano: %d TermTimeS: %d TermTimeNano: %d\n", getpid(), getppid(), clock_ptr->seconds, clock_ptr->nanoseconds, waitSeconds, waitNanoseconds);

    int iterations = 0;
    while (1) {
        int message;
        if (mq_receive(mq, (char*)&message, sizeof(int), NULL) == -1) {
            perror("Worker: Message queue receive failed");
            exit(1);
        }


        if (message == 0) {
            printf("WORKER PID: %d Terminating as per OSS request\n", getpid());
            break;
        }

        long long currentTime = toNanoseconds(clock_ptr->seconds, clock_ptr->nanoseconds);

        if (currentTime >= targetTime) {
            printf("WORKER PID: %d SysClockS: %d SysClockNano: %d - Reached termination time\n", getpid(), clock_ptr->seconds, clock_ptr->nanoseconds);
            int message = 0;
            mq_receive(mq, (char*)&message, sizeof(int), NULL);
            if (message == 0) {
                printf("WORKER PID: %d Terminating as per OSS request\n", getpid());
                break;
            }
        }

        iterations++;
        printf("WORKER PID: %d Iteration: %d SysClockS: %d SysClockNano: %d\n", getpid(), iterations, clock_ptr->seconds, clock_ptr->nanoseconds);
        // Send message only if still running
        message = 1;
        mq_send(mq, (char*)&message, sizeof(int), 0);
    }

    mq_close(mq);
    shmdt(clock_ptr);
    return 0;
}

