# Makefile for OSS Project 3
# Author: Claude
# Date: May 16, 2025

CC = gcc
CFLAGS = -Wall -g
DEPS = common.h
EXECUTABLES = oss worker

all: $(EXECUTABLES)

oss: oss.c $(DEPS)
	$(CC) $(CFLAGS) -o oss oss.c

worker: worker.c $(DEPS)
	$(CC) $(CFLAGS) -o worker worker.c

clean:
	rm -f $(EXECUTABLES) *.o *.log
