CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -lrt -lpthread

# Executable names
OSS_EXEC = oss
WORKER_EXEC = worker

# Source files
OSS_SRC = oss.c
WORKER_SRC = worker.c

# Object files
OSS_OBJ = oss.o
WORKER_OBJ = worker.o

all: $(OSS_EXEC) $(WORKER_EXEC)

$(OSS_EXEC): $(OSS_OBJ)
	$(CC) $(CFLAGS) -o $(OSS_EXEC) $(OSS_OBJ) $(LDFLAGS)

$(WORKER_EXEC): $(WORKER_OBJ)
	$(CC) $(CFLAGS) -o $(WORKER_EXEC) $(WORKER_OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OSS_EXEC) $(WORKER_EXEC) $(OSS_OBJ) $(WORKER_OBJ)

run:
	./oss -n 5 -s 3 -t 7 -i 100 -f logfile.txt

