procxx
======

A simple process management library for C++ on UNIX platforms.

## Usage
Here is a simple (toy) example of setting up a very basic pipeline.

```cpp
// construct a child process that runs `cat`
procxx::process cat{"cat"};

// construct a child process that runs `wc -c`
procxx::process wc{"wc", "-c"};

// set up the pipeline and execute the child processes
(cat | wc).exec();

// write "hello world" to the standard input of the cat child process
cat << "hello world";

// close the write end (stdin) of the cat child
cat.close(procxx::pipe_t::write_end());

// read from the `wc -c` process's stdout, line by line
std::string line;
while (std::getline(wc.output(), line))
    std::cout << line << std::endl;
```

procxx also provides functionality for setting resource limits on the
child processes, as demonstrated below. The functionality is implemented
via the POSIX `rlimit` functions.

```cpp
procxx::process cat{"cat"};
procxx::process wc{"wc", "-c"};

// OPTION 1: same limits for all processes in the pipeline
procxx::process::limits_t limits;
limits.cpu_time(3);         // 3 second execution time limit
limits.memory(1024*1024*1); // 1 MB memory usage limit
(cat | wc).limit(limits).exec();

// OPTION 2: individual limits for each process
procxx::process::limits_t limits;
limits.cpu_time(3);         // 3 second execution time limit
limits.memory(1024*1024*1); // 1 MB memory usage limit
wc.limit(limits);

procxx::process::limits_t limits;
limits.cpu_time(1);  // 1 second execution time limit
limits.memory(1024); // 1 KB memory usage limit
cat.limit(limits);

(cat | wc).exec();
```
