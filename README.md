procxx
======

A simple process management library for C++ on UNIX platforms.

## Usage
Here is a simple (toy) example of setting up a pipeline.

```cpp
// construct a child process that runs `cat`
process cat{"cat"};

// construct a child process, reading from the stdout of the previously
// created process, running `wc -c`
process wc{cat, "wc", "-c"};

// write "hello world" to the standard input of the cat child process
cat.write("hello world", 11);

// close the write end (stdin) of the cat child
cat.close(pipe_t::write_end());

// read from the `wc -l` process's stdout
char buf[101];
auto bytes = cat.read(buf, 100);
buf[bytes] = '\0';
std::cout << buf;
```
