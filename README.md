# Memory Allocator

A small C memory allocator built on `sbrk`, with aligned allocations, free-block splitting and coalescing, and a C11 atomic lock.

The test program in `main.c` checks alignment, allocation, block merging, and heap expansion. This is an experimental learning project, not a production allocator.

## Build and run

```sh
cc -std=gnu11 main.c -o allocator
./allocator
```
