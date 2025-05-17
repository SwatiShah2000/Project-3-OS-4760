#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#define SHM_KEY 'S'
#define MSG_KEY 'M'
#define NANO_PER_SEC 1000000000

// Define shared memory structure for the system clock
typedef struct {
    unsigned int seconds;
    unsigned int nanoseconds;
} SystemClock;

// Define message structure for message queue
typedef struct {
    long mtype;     // Message type
    int status;     // 1 for running, 0 for terminating
} Message;

int main(int argc, char *argv[]) {
    // Check command line arguments
    if (argc != 3) {
        fprintf(stderr, "Usage: %s seconds nanoseconds\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parse command line arguments
    int terminateSeconds = atoi(argv[1]);
    int terminateNano = atoi(argv[2]);

    if (terminateSeconds < 0 || terminateNano < 0 || terminateNano >= NANO_PER_SEC) {
        fprintf(stderr, "Invalid time values. Seconds must be >= 0, nanoseconds must be >= 0 and < %d\n", NANO_PER_SEC);
        exit(EXIT_FAILURE);
    }

    // Set up signal handlers
    signal(SIGINT, SIG_IGN);  // Ignore SIGINT, let parent handle it

    // Get process IDs
    pid_t myPid = getpid();
    pid_t parentPid = getppid();

    // Attach to shared memory for system clock
    key_t key = ftok(".", SHM_KEY);
    if (key == -1) {
        perror("ftok for shared memory");
        exit(EXIT_FAILURE);
    }

    int shmid = shmget(key, sizeof(SystemClock), 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(EXIT_FAILURE);
    }

    SystemClock *systemClock = (SystemClock *)shmat(shmid, NULL, 0);
    if (systemClock == (void *)-1) {
        perror("shmat");
        exit(EXIT_FAILURE);
    }

    // Access message queue
    key_t msgKey = ftok(".", MSG_KEY);
    if (msgKey == -1) {
        perror("ftok for message queue");
        shmdt(systemClock);
        exit(EXIT_FAILURE);
    }

    int msgqid = msgget(msgKey, 0666);
    if (msgqid == -1) {
        perror("msgget");
        shmdt(systemClock);
        exit(EXIT_FAILURE);
    }

    // Calculate absolute termination time
    unsigned int terminationSeconds = systemClock->seconds + terminateSeconds;
    unsigned int terminationNano = systemClock->nanoseconds + terminateNano;

    // Adjust if nanoseconds overflow
    if (terminationNano >= NANO_PER_SEC) {
        terminationSeconds += terminationNano / NANO_PER_SEC;
        terminationNano %= NANO_PER_SEC;
    }

    // Output initial status
    printf("WORKER PID:%d PPID:%d SysClockS: %d SysclockNano: %d TermTimeS: %d TermTimeNano: %d\n",
           myPid, parentPid, systemClock->seconds, systemClock->nanoseconds, 
           terminationSeconds, terminationNano);
    printf("--Just Starting\n");

    // Main loop
    int iterations = 0;
    int shouldTerminate = 0;

    do {
        // Wait for message from oss
        Message msg;
        if (msgrcv(msgqid, &msg, sizeof(msg.status), myPid, 0) == -1) {
            if (errno == EINTR) {
                // Interrupted by signal, try again
                continue;
            }
            perror("msgrcv");
            break;
        }

        // Check if we should terminate based on clock time
        if (systemClock->seconds > terminationSeconds ||
            (systemClock->seconds == terminationSeconds &&
             systemClock->nanoseconds >= terminationNano)) {
            shouldTerminate = 1;
        }

        // Increment iterations
        iterations++;

        // Print status
        printf("WORKER PID:%d PPID:%d SysClockS: %d SysclockNano: %d TermTimeS: %d TermTimeNano: %d\n",
               myPid, parentPid, systemClock->seconds, systemClock->nanoseconds,
               terminationSeconds, terminationNano);

        if (shouldTerminate) {
            printf("--Terminating after sending message back to oss after %d iterations.\n", iterations);
        } else {
            printf("--%d iteration%s have passed since starting\n",
                   iterations, (iterations == 1) ? "" : "s");
        }

        // Send message back to oss
        Message response;
        response.mtype = parentPid;
        response.status = shouldTerminate ? 0 : 1;  // 0 = terminate, 1 = continue

        if (msgsnd(msgqid, &response, sizeof(response.status), 0) == -1) {
            perror("msgsnd");
            break;
        }

    } while (!shouldTerminate);

    // Detach from shared memory
    shmdt(systemClock);

    return EXIT_SUCCESS;
}
