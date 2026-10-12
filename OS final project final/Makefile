CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread -I./src
LDFLAGS = -lpthread

CRAWLER_SRCS = src/crawler/main.c \
               src/crawler/queue.c \
               src/crawler/visited.c \
               src/crawler/fetch.c \
               src/crawler/parse.c \
               src/crawler/threadpool.c \
               src/common/log.c

INDEXER_SRCS = src/indexer/main.c \
               src/indexer/indexer.c \
               src/common/log.c

QUERY_SRCS   = src/query/main.c \
               src/common/log.c

CRAWLER_OBJS = $(patsubst src/%.c, build/%.o, $(CRAWLER_SRCS))
INDEXER_OBJS = $(patsubst src/%.c, build/%.o, $(INDEXER_SRCS))
QUERY_OBJS   = $(patsubst src/%.c, build/%.o, $(QUERY_SRCS))

.PHONY: all clean run test dirs

all: dirs crawler indexer query

dirs:
	mkdir -p build/crawler build/indexer build/query build/common \
	         data/pages data/index

crawler: $(CRAWLER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

indexer: $(INDEXER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

query: $(QUERY_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Generic pattern rule: src/foo/bar.c -> build/foo/bar.o
build/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build/ crawler indexer query data/pages/* data/index/* /tmp/crawl.sock

# Demo run — scan the sample folder (depth 3, 4 threads), then query
run: all
	mkdir -p data/pages data/index
	rm -f /tmp/crawl.sock data/pages/* data/index/*
	./indexer --ipc /tmp/crawl.sock --out data/index & IDX=$$!; \
	sleep 1; \
	./crawler --root testdata --max-depth 3 --max-pages 100 -t 4 \
	          --out data --ipc /tmp/crawl.sock; \
	wait $$IDX; \
	./query --index data/index operating systems threads

# Automated check — builds, scans testdata, and verifies the query results
test: all
	bash test.sh
