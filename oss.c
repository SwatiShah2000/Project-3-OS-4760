/* oss.c - Operating System Simulator */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <mqueue.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>

#define MAX_PROCESSES 20
#define CLOCK_INCREMENT_BASE 250000000 // 250 ms in nanoseconds
#define MAX_RUNTIME 60 // Real time limit in seconds
#define MQ_NAME "/oss_mq"

// Simulated clock structure
typedef struct {
    int seconds;
    int nanoseconds;
} SysClock;

// Process Control Block
struct PCB {
    int occupied;
    pid_t pid;
    int startSeconds;
    int startNano;
    int messagesSent;
};

// Global Variables
SysClock *simulatedClock;
struct PCB processTable[MAX_PROCESSES];
int shmID;
mqd_t mq;
int totalProcesses = 0, maxConcurrent = 5, maxRuntime = 5, launchInterval = 100, logFileSet = 0;
FILE *logFile;

void incrementClock(int activeProcesses) {
    int increment = (activeProcesses > 0) ? CLOCK_INCREMENT_BASE / activeProcesses : CLOCK_INCREMENT_BASE;
    simulatedClock->nanoseconds += increment;
    if (simulatedClock->nanoseconds >= 1000000000) {
        simulatedClock->nanoseconds -= 1000000000;
        simulatedClock->seconds++;
    }
    printf("OSS: Updated SysClockS: %d SysClockNano: %d\n", simulatedClock->seconds, simulatedClock->nanoseconds);
}

void printProcessTable() {
    printf("OSS: Process Table\n");
    printf("PID\tStart Sec\tStart Nano\tMessages Sent\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processTable[i].occupied) {
            printf("%d\t%d\t%d\t%d\n", processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano, processTable[i].messagesSent);
        }
    }
}

void cleanup(int sig) {
    printf("OSS: Sending termination messages to active workers...\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processTable[i].occupied) {
            int message = 0; // Termination message
            if (mq_send(mq, (char*)&message, sizeof(int), 0) == -1) {
                perror("OSS: Message queue send failed");
            }
            waitpid(processTable[i].pid, NULL, 0);
            printf("OSS: Worker %d terminated.\n", processTable[i].pid);
        }
    }
}




void parseArguments(int argc, char *argv[]) {
    int option;
    while ((option = getopt(argc, argv, "n:s:t:i:f:")) != -1) {
        switch (option) {
            case 'n': totalProcesses = atoi(optarg); break;
            case 's': maxConcurrent = atoi(optarg); break;
            case 't': maxRuntime = atoi(optarg); break;
            case 'i': launchInterval = atoi(optarg); break;
            case 'f': logFile = fopen(optarg, "w"); logFileSet = 1; break;
            default:
                fprintf(stderr, "Usage: %s [-n num] [-s simul] [-t maxTime] [-i interval] [-f logFile]\n", argv[0]);
                exit(1);
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, cleanup);
    signal(SIGALRM, cleanup);
    alarm(MAX_RUNTIME);
    parseArguments(argc, argv);

    key_t shmKey = ftok(".", 65);
    if (shmKey == -1) {
        perror("ftok failed");
        exit(1);
    }
    printf("OSS: Created shared memory with key: %d\n", shmKey);

    shmID = shmget(shmKey, sizeof(SysClock), IPC_CREAT | 0666);
    simulatedClock = (SysClock *)shmat(shmID, NULL, 0);
    simulatedClock->seconds = 0;
    simulatedClock->nanoseconds = 0;

    struct mq_attr attr = {.mq_flags = 0, .mq_maxmsg = 10, .mq_msgsize = sizeof(int), .mq_curmsgs = 0};
    mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0666, &attr);

    int numChildren = 0, totalLaunched = 0;
    int lastSentIndex = -1;
    while (totalLaunched < totalProcesses || numChildren > 0) {
        incrementClock(numChildren);
        static int printCounter = 0;
        if (printCounter++ % 5 == 0) {
            printProcessTable();
        }

        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (processTable[i].occupied) {
                    int message = 1;
                    if (mq_send(mq, (char*)&message, sizeof(int), 0) == -1) {
                        perror("OSS: Message queue send failed");
                    }
                    processTable[i].messagesSent++;
                }
            }
        }


        if (numChildren < maxConcurrent && totalLaunched < totalProcesses) {
            pid_t pid = fork();
            if (pid == 0) {
                char secStr[10], nanoStr[10], shmKeyStr[10];
                snprintf(secStr, 10, "%d", rand() % maxRuntime + 1);
                snprintf(nanoStr, 10, "%d", rand() % 1000000000);
                snprintf(shmKeyStr, 10, "%d", shmKey);
                execl("./worker", "worker", secStr, nanoStr, shmKeyStr, NULL);
                exit(0);
            } else {
                for (int i = 0; i < MAX_PROCESSES; i++) {
                    if (processTable[i].occupied == 0) {
                        processTable[i].occupied = 1;
                        processTable[i].pid = pid;
                        processTable[i].startSeconds = simulatedClock->seconds;
                        processTable[i].startNano = simulatedClock->nanoseconds;
                        processTable[i].messagesSent = 0;
                        numChildren++;
                        totalLaunched++;
                        break;
                    }
                }
            }
        }

        lastSentIndex = (lastSentIndex + 1) % MAX_PROCESSES;
        if (processTable[lastSentIndex].occupied) {
            int message = 1;
            mq_send(mq, (char*)&message, sizeof(int), 0);
            processTable[lastSentIndex].messagesSent++;
        }
    }
    cleanup(0);
    return 0;
}

