CC = g++
CFLAGS = -Wall -g

OSS_SRC = oss.cpp
WORKER_SRC = worker.cpp

OSS_BIN = oss
WORKER_BIN = worker

all: $(OSS_BIN) $(WORKER_BIN)

$(OSS_BIN): $(OSS_SRC)
	$(CC) $(CFLAGS) -o $(OSS_BIN) $(OSS_SRC)

$(WORKER_BIN): $(WORKER_SRC)
	$(CC) $(CFLAGS) -o $(WORKER_BIN) $(WORKER_SRC)

clean:
	rm -f $(OSS_BIN) $(WORKER_BIN) *.o

.PHONY: all clean