# Memory Allocator

## Statement

Build a minimalistic memory allocator that can be used to manually manage virtual memory.
The goal is to have a reliable library that accounts for explicit allocation, reallocation, and initialization of memory.

## Support Code

The support code consists of three directories:

- `src/` will contain your solution
- `tests/` contains the test suite and a Python script to verify your work
- `utils/` contains `osmem.h` that describes your library interface, `block_meta.h` which contains details of `struct block_meta`, and an implementation for `printf()` function that does **NOT** use the heap

The test suite consists of `.c` files that will be dynamically linked to your library, `libosmem.so`.
You can find the sources in the `tests/snippets/` directory.
The results of the previous will also be stored in `tests/snippets/` and the reference files are in the `tests/ref/` directory.

The automated checking is performed using `run_tests.py`.
It runs each test and compares the syscalls made by the `os_*` functions with the reference file, providing a diff if the test failed.



