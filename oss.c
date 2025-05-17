#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

// Define constants if not using common.h
#define SHM_KEY 'S'
#define MSG_KEY 'M'
#define MAX_PROCESSES 20
#define NANO_PER_SEC 1000000000

// Define shared memory structure for the system clock
typedef struct {
    unsigned int seconds;
    unsigned int nanoseconds;
} SystemClock;

// Define PCB structure for process table
struct PCB {
    int occupied;           // either true (1) or false (0)
    pid_t pid;              // process id of this child
    int startSeconds;       // time when it was forked
    int startNano;          // time when it was forked
    int messagesSent;       // total times oss sent a message to this process
};

// Define message structure for message queue
typedef struct {
    long mtype;             // Message type
    int status;             // 1 for running, 0 for terminating
} Message;

// Global variables for resources that need cleanup
int shmid = -1;             // Shared memory ID
int msgqid = -1;            // Message queue ID
SystemClock *systemClock;   // Pointer to shared memory clock
FILE *logfile = NULL;       // Log file pointer
struct PCB *processTable;   // Process table
int totalProcesses = 0;     // Total processes launched
int totalMessages = 0;      // Total messages sent
int simultaneousMax = 0;    // Maximum simultaneous processes
pid_t childPIDs[MAX_PROCESSES];  // Array to keep track of child PIDs

// Function prototypes
void cleanup();
void sigintHandler(int sig);
void timeoutHandler(int sig);
void incrementClock(int activeChildren);
int launchChild(int timelimit, int *processCount);
int findNextChildIndex(int currentChild);
int countActiveChildren();
void displayProcessTable();

/**
 * Main function
 */
int main(int argc, char *argv[]) {
    // Default parameter values
    int opt;
    int processLimit = 5;        // Default number of processes to launch
    simultaneousMax = 3;         // Default simultaneous processes
    int timelimit = 5;           // Default time limit for children
    int launchInterval = 1000;   // Default interval between launches (ms)
    char logfileName[256] = "oss.log"; // Default log file name

    // Parse command line arguments
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                printf("Usage: %s [-h] [-n proc] [-s simul] [-t timelimitForChildren] ", argv[0]);
                printf("[-i intervalInMsToLaunchChildren] [-f logfile]\n");
                printf("Options:\n");
                printf("  -h                   : Display this help message\n");
                printf("  -n proc              : Number of total processes to launch (default: %d)\n", processLimit);
                printf("  -s simul             : Maximum simultaneous processes (default: %d)\n", simultaneousMax);
                printf("  -t timelimitForChildren: Upper bound for child runtime in seconds (default: %d)\n", timelimit);
                printf("  -i intervalInMsToLaunchChildren: Minimum interval between child launches (default: %d)\n", launchInterval);
                printf("  -f logfile           : Path to log file (default: %s)\n", logfileName);
                exit(EXIT_SUCCESS);
            case 'n':
                processLimit = atoi(optarg);
                if (processLimit <= 0 || processLimit > 100) {
                    fprintf(stderr, "Invalid number of processes. Using default: 5\n");
                    processLimit = 5;
                }
                break;
            case 's':
                simultaneousMax = atoi(optarg);
                if (simultaneousMax <= 0 || simultaneousMax > 20) {
                    fprintf(stderr, "Invalid number of simultaneous processes. Using default: 3\n");
                    simultaneousMax = 3;
                }
                break;
            case 't':
                timelimit = atoi(optarg);
                if (timelimit <= 0) {
                    fprintf(stderr, "Invalid time limit. Using default: 5\n");
                    timelimit = 5;
                }
                break;
            case 'i':
                launchInterval = atoi(optarg);
                if (launchInterval < 0) {
                    fprintf(stderr, "Invalid launch interval. Using default: 1000\n");
                    launchInterval = 1000;
                }
                break;
            case 'f':
                strncpy(logfileName, optarg, sizeof(logfileName) - 1);
                logfileName[sizeof(logfileName) - 1] = '\0'; // Ensure null-termination
                break;
            default:
                fprintf(stderr, "Invalid option. Use -h for help.\n");
                exit(EXIT_FAILURE);
        }
    }

    // Open log file
    logfile = fopen(logfileName, "w");
    if (logfile == NULL) {
        perror("Error opening log file");
        exit(EXIT_FAILURE);
    }

    // Set up signal handlers for proper cleanup
    signal(SIGINT, sigintHandler);
    signal(SIGALRM, timeoutHandler);

    // Set 60-second timeout
    alarm(60);

    // Create shared memory for system clock
    key_t key = ftok(".", SHM_KEY);
    if (key == -1) {
        perror("ftok");
        cleanup();
        exit(EXIT_FAILURE);
    }

    shmid = shmget(key, sizeof(SystemClock), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        cleanup();
        exit(EXIT_FAILURE);
    }
    // Attach to shared memory
    systemClock = (SystemClock *)shmat(shmid, NULL, 0);
    if (systemClock == (void *)-1) {
        perror("shmat");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Initialize system clock
    systemClock->seconds = 0;
    systemClock->nanoseconds = 0;

    // Create message queue
    key_t msgKey = ftok(".", MSG_KEY);
    if (msgKey == -1) {
        perror("ftok");
        cleanup();
        exit(EXIT_FAILURE);
    }

    msgqid = msgget(msgKey, IPC_CREAT | 0666);
    if (msgqid == -1) {
        perror("msgget");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Allocate and initialize process table
    processTable = (struct PCB *)malloc(MAX_PROCESSES * sizeof(struct PCB));
    if (processTable == NULL) {
        perror("malloc");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Initialize process table
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processTable[i].occupied = 0;
        processTable[i].pid = 0;
        processTable[i].startSeconds = 0;
        processTable[i].startNano = 0;
        processTable[i].messagesSent = 0;
        childPIDs[i] = 0;
    }

    // Seed random number generator
    srand(time(NULL));

    // Main execution loop
    int processCount = 0;
    int nextChild = -1;
    unsigned int lastLaunchTime = 0;
    unsigned int lastDisplayTime = 0;

    fprintf(stdout, "OSS PID:%d starting with parameters: n=%d, s=%d, t=%d, i=%d\n",
            getpid(), processLimit, simultaneousMax, timelimit, launchInterval);
    fprintf(logfile, "OSS PID:%d starting with parameters: n=%d, s=%d, t=%d, i=%d\n",
            getpid(), processLimit, simultaneousMax, timelimit, launchInterval);

    // Main loop: Continue until all processes have been launched and completed
    while (totalProcesses < processLimit || countActiveChildren() > 0) {
        // Count number of active children
        int activeChildren = countActiveChildren();

        // Increment the clock
        incrementClock(activeChildren > 0 ? activeChildren : 1);

        // Check if it's time to launch a new process
        unsigned int currentTimeMs = (systemClock->seconds * 1000) + (systemClock->nanoseconds / 1000000);
        if (totalProcesses < processLimit && activeChildren < simultaneousMax && 
            (currentTimeMs - lastLaunchTime) >= (unsigned int)launchInterval) {

            int newChildIndex = launchChild(timelimit, &processCount);
            if (newChildIndex >= 0) {
                lastLaunchTime = currentTimeMs;
                displayProcessTable();
            }
        }

        // Send message to next child if any are active
        if (activeChildren > 0) {
            nextChild = findNextChildIndex(nextChild);
            if (nextChild >= 0) {
                // Send message to this child
                Message msg;
                msg.mtype = processTable[nextChild].pid;  // Use child PID as message type
                msg.status = 1;  // 1 = continue

                fprintf(stdout, "OSS: Sending message to worker %d PID %d at time %d:%d\n",
                        nextChild, processTable[nextChild].pid, systemClock->seconds, systemClock->nanoseconds);
                fprintf(logfile, "OSS: Sending message to worker %d PID %d at time %d:%d\n",
                        nextChild, processTable[nextChild].pid, systemClock->seconds, systemClock->nanoseconds);

                if (msgsnd(msgqid, &msg, sizeof(msg.status), 0) == -1) {
                    perror("msgsnd");
                    // Child may have terminated, check
                    int status;
                    pid_t result = waitpid(processTable[nextChild].pid, &status, WNOHANG);
                    if (result > 0) {
                        fprintf(stdout, "OSS: Worker %d PID %d has terminated unexpectedly\n",
                                nextChild, processTable[nextChild].pid);
                        fprintf(logfile, "OSS: Worker %d PID %d has terminated unexpectedly\n",
                                nextChild, processTable[nextChild].pid);
                        processTable[nextChild].occupied = 0;
                        childPIDs[nextChild] = 0;
                        continue;
                    }
                }

                processTable[nextChild].messagesSent++;
                totalMessages++;

                // Receive message from child
                Message response;
                if (msgrcv(msgqid, &response, sizeof(response.status), getpid(), 0) == -1) {
                    perror("msgrcv");
                    continue;
                }

                fprintf(stdout, "OSS: Receiving message from worker %d PID %d at time %d:%d\n",
                        nextChild, processTable[nextChild].pid, systemClock->seconds, systemClock->nanoseconds);
                fprintf(logfile, "OSS: Receiving message from worker %d PID %d at time %d:%d\n",
                        nextChild, processTable[nextChild].pid, systemClock->seconds, systemClock->nanoseconds);

                // Check if child is terminating
                if (response.status == 0) {
                    fprintf(stdout, "OSS: Worker %d PID %d is planning to terminate\n",
                            nextChild, processTable[nextChild].pid);
                    fprintf(logfile, "OSS: Worker %d PID %d is planning to terminate\n",
                            nextChild, processTable[nextChild].pid);

                    // Wait for child to actually terminate
                    waitpid(processTable[nextChild].pid, NULL, 0);

                    // Update process table
                    processTable[nextChild].occupied = 0;
                    childPIDs[nextChild] = 0;
                }
            }
        }

        // Check if it's time to display the process table (every 0.5 seconds)
        unsigned int halfSecondInterval = 500; // 500ms = 0.5s
        if ((currentTimeMs - lastDisplayTime) >= halfSecondInterval) {
            displayProcessTable();
            lastDisplayTime = currentTimeMs;
        }
    }

    // Final statistics
    fprintf(stdout, "\n--- Final Statistics ---\n");
    fprintf(stdout, "Total processes launched: %d\n", totalProcesses);
    fprintf(stdout, "Total messages sent: %d\n", totalMessages);

    fprintf(logfile, "\n--- Final Statistics ---\n");
    fprintf(logfile, "Total processes launched: %d\n", totalProcesses);
    fprintf(logfile, "Total messages sent: %d\n", totalMessages);

    // Cleanup and exit
    cleanup();
    return EXIT_SUCCESS;
}

/**
 * Launch a new child worker process
 * @param timelimit Upper bound for child runtime
 * @param processCount Pointer to current process count
 * @return Index of the new child in the process table, or -1 on failure
 */
int launchChild(int timelimit, int *processCount) {
    // Find free slot in process table
    int freeIndex = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processTable[i].occupied == 0) {
            freeIndex = i;
            break;
        }
    }

    if (freeIndex == -1) {
        fprintf(stderr, "Error: No free slots in process table\n");
        return -1;
    }

    // Determine random lifetime for child (1 to timelimit seconds)
    int childSeconds = (rand() % timelimit) + 1;
    int childNano = rand() % NANO_PER_SEC; // Random nanoseconds

    // Record process start time
    processTable[freeIndex].startSeconds = systemClock->seconds;
    processTable[freeIndex].startNano = systemClock->nanoseconds;

    // Fork new process
    pid_t childPid = fork();

    if (childPid == -1) {
        perror("fork");
        return -1;
    } else if (childPid == 0) {
        // Child process

        // Set up signal handler for parent termination
        signal(SIGTERM, SIG_DFL);  // Default handler for SIGTERM

        char secStr[20], nanoStr[20];
        sprintf(secStr, "%d", childSeconds);
        sprintf(nanoStr, "%d", childNano);

        // Execute worker with arguments
        execl("./worker", "worker", secStr, nanoStr, NULL);

        // If execl fails
        perror("execl");
        exit(EXIT_FAILURE);
    } else {
        // Parent process - update process table
        processTable[freeIndex].occupied = 1;
        processTable[freeIndex].pid = childPid;
        processTable[freeIndex].messagesSent = 0;
        childPIDs[freeIndex] = childPid;

        (*processCount)++;
        totalProcesses++;

        fprintf(stdout, "OSS: Launching worker process PID %d (will run for %d sec, %d nano)\n",
                childPid, childSeconds, childNano);
        fprintf(logfile, "OSS: Launching worker process PID %d (will run for %d sec, %d nano)\n",
                childPid, childSeconds, childNano);

        return freeIndex;
    }
}

/**
 * Find the next child to send a message to
 * @param currentChild Index of the current child
 * @return Index of the next child, or -1 if none available
 */
int findNextChildIndex(int currentChild) {
    int startIdx = (currentChild + 1) % MAX_PROCESSES;
    int idx = startIdx;

    do {
        if (processTable[idx].occupied) {
            return idx;
        }
        idx = (idx + 1) % MAX_PROCESSES;
    } while (idx != startIdx);

    return -1;  // No active children
}

/**
 * Count number of active child processes
 * @return Number of active children
 */
int countActiveChildren() {
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processTable[i].occupied) {
            count++;
        }
    }
    return count;
}

/**
 * Increment the system clock
 * @param activeChildren Number of active children
 */
void incrementClock(int activeChildren) {
    // Increment by 250ms divided by number of children
    unsigned int incrementNano = (250 * 1000000) / activeChildren;

    systemClock->nanoseconds += incrementNano;
    if (systemClock->nanoseconds >= NANO_PER_SEC) {
        systemClock->seconds += systemClock->nanoseconds / NANO_PER_SEC;
        systemClock->nanoseconds %= NANO_PER_SEC;
    }
}

/**
 * Display the current process table
 */
void displayProcessTable() {
    fprintf(stdout, "OSS PID:%d SysClockS: %d SysclockNano: %d\n",
            getpid(), systemClock->seconds, systemClock->nanoseconds);
    fprintf(stdout, "Process Table:\n");
    fprintf(stdout, "Entry\tOccupied\tPID\tStartS\tStartN\tMessagesSent\n");

    fprintf(logfile, "OSS PID:%d SysClockS: %d SysclockNano: %d\n",
            getpid(), systemClock->seconds, systemClock->nanoseconds);
    fprintf(logfile, "Process Table:\n");
    fprintf(logfile, "Entry\tOccupied\tPID\tStartS\tStartN\tMessagesSent\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        fprintf(stdout, "%d\t%d\t\t%d\t%d\t%d\t%d\n", i, processTable[i].occupied,
                processTable[i].pid, processTable[i].startSeconds,
                processTable[i].startNano, processTable[i].messagesSent);
        fprintf(logfile, "%d\t%d\t\t%d\t%d\t%d\t%d\n", i, processTable[i].occupied,
                processTable[i].pid, processTable[i].startSeconds,
                processTable[i].startNano, processTable[i].messagesSent);
    }
    fprintf(stdout, "\n");
    fprintf(logfile, "\n");
}

/**
 * Signal handler for SIGINT (Ctrl+C)
 */
void sigintHandler(int sig) {
    fprintf(stderr, "\nCaught SIGINT. Cleaning up and terminating...\n");
    fprintf(logfile, "\nCaught SIGINT. Cleaning up and terminating...\n");
    fprintf(logfile, "\n--- Final Statistics at Termination ---\n");
    fprintf(logfile, "Total processes launched: %d\n", totalProcesses);
    fprintf(logfile, "Total messages sent: %d\n", totalMessages);
    cleanup();
    exit(EXIT_SUCCESS);
}

/**
 * Signal handler for SIGALRM (timeout)
 */
void timeoutHandler(int sig) {
    fprintf(stderr, "\nTimeout reached (60 seconds). Cleaning up and terminating...\n");
    fprintf(logfile, "\nTimeout reached (60 seconds). Cleaning up and terminating...\n");
    fprintf(logfile, "\n--- Final Statistics at Timeout ---\n");
    fprintf(logfile, "Total processes launched: %d\n", totalProcesses);
    fprintf(logfile, "Total messages sent: %d\n", totalMessages);
    cleanup();
    exit(EXIT_SUCCESS);
}

/**
 * Cleanup function to release resources
 */
void cleanup() {
    // Log cleanup start
    if (logfile != NULL) {
        fprintf(logfile, "Starting cleanup process...\n");
    }

    // Kill any remaining child processes
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (childPIDs[i] > 0) {
            if (kill(childPIDs[i], SIGTERM) == 0) {
                if (logfile != NULL) {
                    fprintf(logfile, "Sent SIGTERM to child PID %d\n", childPIDs[i]);
                }
            }
        }
    }

    // Wait for all children to terminate with a timeout
    int status;
    pid_t wpid;
    time_t startTime = time(NULL);

    while ((wpid = waitpid(-1, &status, WNOHANG)) > 0 || time(NULL) - startTime < 2) {
        if (wpid > 0 && logfile != NULL) {
            fprintf(logfile, "Child PID %d terminated with status %d\n", wpid, status);
        }
        // Small delay to avoid busy waiting
        usleep(10000);
    }

    // Force kill any lingering processes
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (childPIDs[i] > 0) {
            if (kill(childPIDs[i], SIGKILL) == 0 && logfile != NULL) {
                fprintf(logfile, "Sent SIGKILL to lingering child PID %d\n", childPIDs[i]);
            }
        }
    }

    // Detach and remove shared memory
    if (systemClock != (void *)-1) {
        if (shmdt(systemClock) == 0 && logfile != NULL) {
            fprintf(logfile, "Detached from shared memory\n");
        }
    }

    if (shmid != -1) {
        if (shmctl(shmid, IPC_RMID, NULL) == 0 && logfile != NULL) {
            fprintf(logfile, "Removed shared memory segment\n");
        }
    }

    // Remove message queue
    if (msgqid != -1) {
        if (msgctl(msgqid, IPC_RMID, NULL) == 0 && logfile != NULL) {
            fprintf(logfile, "Removed message queue\n");
        }
    }

    // Free process table
    if (processTable != NULL) {
        free(processTable);
        if (logfile != NULL) {
            fprintf(logfile, "Freed process table memory\n");
        }
    }

    // Close log file
    if (logfile != NULL) {
        fprintf(logfile, "Cleanup complete\n");
        fclose(logfile);
    }
}
