#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>

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

// Define PCB structure for process table
struct PCB {
    int occupied;           // either true (1) or false (0)
    pid_t pid;              // process id of this child
    int startSeconds;       // time when it was forked
    int startNano;          // time when it was forked
    int messagesSent;       // total times oss sent a message to this process
};

// Constants
#define MAX_PROCESSES 20     // Maximum processes in process table
#define NANO_PER_SEC 1000000000  // Nanoseconds per second

// Keys for IPC
#define SHM_KEY 'S'  // Shared memory key
#define MSG_KEY 'M'  // Message queue key

#endif /* COMMON_H */
