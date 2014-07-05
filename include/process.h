/**
 * @file process.h
 * @author Chase Geigle
 *
 * A simple, header-only process/pipe library for C++ on UNIX platforms.
 *
 * Released under the MIT license (see LICENSE).
 */

#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <istream>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

#ifndef PROCXX_HAS_PIPE2
#define PROCXX_HAS_PIPE2 1
#endif

/**
 * Represents a UNIX pipe between processes.
 */
class pipe_t
{
  public:
    static constexpr int READ_END = 0;
    static constexpr int WRITE_END = 1;

    /**
     * Wrapper type that ensures sanity when dealing with operations on
     * the different ends of the pipe.
     */
    class pipe_end
    {
      public:
        /**
         * Constructs a new object to represent an end of a pipe. Ensures
         * the end passed makes sense (e.g., is either the READ_END or the
         * WRITE_END of the pipe).
         */
        pipe_end(int end)
        {
            if (end != READ_END && end != WRITE_END)
                throw exception{"invalid pipe end"};
            end_ = end;
        }

        /**
         * pipe_ends are implicitly convertible to ints.
         */
        operator int() const
        {
            return end_;
        }

      private:
        int end_;
    };

    /**
     * Gets a pipe_end representing the read end of a pipe.
     */
    static pipe_end read_end()
    {
        static pipe_end read{READ_END};
        return read;
    }

    /**
     * Gets a pipe_end representing the write end of a pipe.
     */
    static pipe_end write_end()
    {
        static pipe_end write{WRITE_END};
        return write;
    }

    /**
     * Constructs a new pipe.
     */
    pipe_t()
    {
#if PROCXX_HAS_PIPE2
        ::pipe2(&pipe_[0], O_CLOEXEC);
#else
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock{mutex};
        ::pipe(&pipe_[0]);

        auto flags = ::fcntl(pipe_[0], F_GETFD, 0);
        ::fcntl(pipe_[0], F_SETFD, flags | FD_CLOEXEC);

        flags = ::fcntl(pipe_[1], F_GETFD, 0);
        ::fcntl(pipe_[1], F_SETFD, flags | FD_CLOEXEC);
#endif
    }

    /**
     * Pipes may be move constructed.
     */
    pipe_t(pipe_t&& other)
    {
        pipe_ = std::move(other.pipe_);
        other.pipe_[READ_END] = -1;
        other.pipe_[WRITE_END] = -1;
    }

    /**
     * Pipes are unique---they cannot be copied.
     */
    pipe_t(const pipe_t&) = delete;

    /**
     * Writes length bytes from buf to the pipe.
     *
     * @param buf the buffer to get bytes from
     * @param length the number of bytes to write
     */
    void write(const char* buf, uint64_t length)
    {
        auto bytes = ::write(pipe_[WRITE_END], buf, length);
        if (bytes == -1)
        {
            // interrupt, just attempt to write again
            if (errno == EINTR)
                return write(buf, length);
            // otherwise, unrecoverable error
            perror("pipe_t::write()");
            throw exception{"failed to write"};
        }
        if (bytes < static_cast<ssize_t>(length))
            write(buf + bytes, length - bytes);
    }

    /**
     * Reads up to length bytes from the pipe, placing them in buf.
     *
     * @param buf the buffer to write to
     * @param length the maximum number of bytes to read
     * @return the actual number of bytes read
     */
    ssize_t read(char* buf, uint64_t length)
    {
        auto bytes = ::read(pipe_[READ_END], buf, length);
        if (bytes == -1)
        {
            perror("pipe_t::read()");
            throw exception{"failed to read"};
        }
        return bytes;
    }

    /**
     * Closes both ends of the pipe.
     */
    void close()
    {
        close(read_end());
        close(write_end());
    }

    /**
     * Closes a specific end of the pipe.
     */
    void close(pipe_end end)
    {
        if (pipe_[end] != -1)
        {
            ::close(pipe_[end]);
            pipe_[end] = -1;
        }
    }

    /**
     * Determines if an end of the pipe is still open.
     */
    bool open(pipe_end end)
    {
        return pipe_[end] != -1;
    }

    /**
     * Redirects the given file descriptor to the given end of the pipe.
     *
     * @param end the end of the pipe to connect to the file descriptor
     * @param fd the file descriptor to connect
     */
    void dup(pipe_end end, int fd)
    {
        if (::dup2(pipe_[end], fd) == -1)
        {
            perror("pipe_t::dup()");
            throw exception{"failed to dup"};
        }
    }

    /**
     * Redirects the given end of the given pipe to the current pipe.
     *
     * @param end the end of the pipe to redirect
     * @param other the pipe to redirect to the current pipe
     */
    void dup(pipe_end end, pipe_t& other)
    {
        dup(end, other.pipe_[end]);
    }

    /**
     * The destructor for pipes relinquishes any file descriptors that
     * have not yet been closed.
     */
    ~pipe_t()
    {
        close();
    }

    /**
     * An exception type for any unrecoverable errors that occur during
     * pipe operations.
     */
    class exception : public std::runtime_error
    {
      public:
        using std::runtime_error::runtime_error;
    };

  private:
    std::array<int, 2> pipe_;
};

/**
 * Streambuf for reading/writing to pipes.
 *
 * @see http://www.mr-edd.co.uk/blog/beginners_guide_streambuf
 */
class pipe_streambuf : public std::streambuf
{
  public:
    /**
     * Constructs a new streambuf, with the given buffer size and put_back
     * buffer space.
     */
    pipe_streambuf(size_t buffer_size = 512, size_t put_back_size = 8)
        : put_back_size_{put_back_size},
          in_buffer_(buffer_size + put_back_size),
          out_buffer_(buffer_size + 1)
    {
        auto end = &in_buffer_.back() + 1;
        setg(end, end, end);

        auto begin = &out_buffer_.front();
        setp(begin, begin + out_buffer_.size() - 1);
    }

    /**
     * Destroys the streambuf, which will flush any remaining content on
     * the output buffer.
     */
    ~pipe_streambuf()
    {
        flush();
    }

    int_type underflow() override
    {
        // if the buffer is not exhausted, return the next element
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());

        auto base = &in_buffer_.front();
        auto start = base;

        // if we are not the first fill of the buffer
        if (eback() == base)
        {
            // move the put_back area to the front
            std::memmove(base, egptr() - put_back_size_, put_back_size_);
            start += put_back_size_;
        }

        // start now points to the head of the usable area of the buffer
        auto bytes =
            stdout_pipe_.read(start, in_buffer_.size() - (start - base));
        if (bytes == 0)
            return traits_type::eof();

        setg(base, start, start + bytes);

        return traits_type::to_int_type(*gptr());
    }

    int_type overflow(int_type ch) override
    {
        if (ch != traits_type::eof())
        {
            *pptr() = ch; // safe because of -1 in setp() in ctor
            pbump(1);
            flush();
            return ch;
        }

        return traits_type::eof();
    }

    int sync() override
    {
        flush();
        return 0;
    }

    /**
     * An exception for pipe_streambuf interactions.
     */
    class exception : public std::runtime_error
    {
      public:
        using std::runtime_error::runtime_error;
    };

    /**
     * Gets the stdin pipe.
     */
    pipe_t& stdin_pipe()
    {
        return stdin_pipe_;
    }

    /**
     * Gets the stdout pipe.
     */
    pipe_t& stdout_pipe()
    {
        return stdout_pipe_;
    }

    /**
     * Closes one of the pipes. This will flush any remaining bytes in the
     * output buffer.
     */
    void close(pipe_t::pipe_end end)
    {
        if (end == pipe_t::read_end())
            stdout_pipe().close(pipe_t::read_end());
        else
        {
            flush();
            stdin_pipe().close(pipe_t::write_end());
        }
    }

  private:
    void flush()
    {
        if (stdin_pipe_.open(pipe_t::write_end()))
        {
            stdin_pipe_.write(pbase(), pptr() - pbase());
            pbump(-(pptr() - pbase()));
        }
    }

    pipe_t stdin_pipe_;
    pipe_t stdout_pipe_;
    size_t put_back_size_;
    std::vector<char> in_buffer_;
    std::vector<char> out_buffer_;
};

/**
 * A handle that represents a child process.
 */
class process
{
  public:
    /**
     * Constructs a new child process, executing the given application and
     * passing the given arguments to it.
     */
    template <class... Args>
    process(std::string&& application, Args&&... args)
        : args_{std::move(application), std::forward<Args>(args)...},
          out_stream_{&pipe_buf_},
          in_stream_{&pipe_buf_}
    {
        // nothing
    }

    /**
     * Sets the process to read from the standard output of another
     * process.
     */
    void read_from(process& other)
    {
        read_from_ = &other;
    }

    /**
     * Executes the process.
     */
    void exec()
    {
        if (pid_ != -1)
            throw exception{"process already started"};

        auto pid = fork();
        if (pid == -1)
        {
            perror("fork()");
            throw exception{"Failed to fork child process"};
        }
        else if (pid == 0)
        {
            pipe_buf_.stdin_pipe().close(pipe_t::write_end());
            pipe_buf_.stdout_pipe().close(pipe_t::read_end());
            pipe_buf_.stdout_pipe().dup(pipe_t::write_end(), STDOUT_FILENO);

            if (read_from_)
            {
                read_from_->recursive_close_stdin();
                pipe_buf_.stdin_pipe().close(pipe_t::read_end());
                read_from_->pipe_buf_.stdout_pipe().dup(pipe_t::read_end(),
                                                        STDIN_FILENO);
            }
            else
            {
                pipe_buf_.stdin_pipe().dup(pipe_t::read_end(), STDIN_FILENO);
            }

            std::vector<char*> args;
            args.reserve(args_.size() + 1);
            for (auto& arg : args_)
                args.push_back(const_cast<char*>(arg.c_str()));
            args.push_back(nullptr);

            limits_.set_limits();
            execvp(args[0], args.data());
            throw exception{"Failed to execute child process"};
        }
        else
        {
            pipe_buf_.stdout_pipe().close(pipe_t::write_end());
            pipe_buf_.stdin_pipe().close(pipe_t::read_end());
            if (read_from_)
            {
                pipe_buf_.stdin_pipe().close(pipe_t::write_end());
                read_from_->pipe_buf_.stdout_pipe().close(pipe_t::read_end());
            }
            pid_ = pid;
        }
    }

    /**
     * Process handles may be moved.
     */
    process(process&&) = default;

    /**
     * Process handles are unique: they may not be copied.
     */
    process(const process&) = delete;

    /**
     * The destructor for a process will wait for the child if client code
     * has not already explicitly waited for it.
     */
    ~process()
    {
        wait();
    }

    /**
     * Simple wrapper for process limit settings. Currently supports
     * setting processing time and memory usage limits.
     */
    class limits_t
    {
      public:
        /**
         * Sets the maximum amount of cpu time, in seconds.
         */
        void cpu_time(rlim_t max)
        {
            lim_cpu_ = true;
            cpu_.rlim_cur = cpu_.rlim_max = max;
        }

        /**
         * Sets the maximum allowed memory usage, in bytes.
         */
        void memory(rlim_t max)
        {
            lim_as_ = true;
            as_.rlim_cur = as_.rlim_max = max;
        }

        /**
         * Applies the set limits to the current process.
         */
        void set_limits()
        {
            if (lim_cpu_ && setrlimit(RLIMIT_CPU, &cpu_) != 0)
            {
                perror("limits_t::set_limits()");
                throw exception{"Failed to set cpu time limit"};
            }

            if (lim_as_ && setrlimit(RLIMIT_AS, &as_) != 0)
            {
                perror("limits_t::set_limits()");
                throw exception{"Failed to set memory limit"};
            }
        }

      private:
        bool lim_cpu_ = false;
        rlimit cpu_;
        bool lim_as_ = false;
        rlimit as_;
    };

    /**
     * Sets the limits for this process.
     */
    void limit(const limits_t& limits)
    {
        limits_ = limits;
    }

    /**
     * Waits for the child to exit.
     */
    void wait()
    {
        if (!waited_)
        {
            pipe_buf_.close(pipe_t::write_end());
            waitpid(pid_, &status_, 0);
            waited_ = true;
        }
    }

    /**
     * Determines if the child exited properly.
     */
    bool exited() const
    {
        if (!waited_)
            throw exception{"process::wait() not yet called"};
        return WIFEXITED(status_);
    }

    /**
     * Determines if the child was killed.
     */
    bool killed() const
    {
        if (!waited_)
            throw exception{"process::wait() not yet called"};
        return WIFSIGNALED(status_);
    }

    /**
     * Determines if the child was stopped.
     */
    bool stopped() const
    {
        if (!waited_)
            throw exception{"process::wait() not yet called"};
        return WIFSTOPPED(status_);
    }

    /**
     * Gets the exit code for the child. If it was killed or stopped, the
     * signal that did so is returned instead.
     */
    int code() const
    {
        if (!waited_)
            throw exception{"process::wait() not yet called"};
        if (exited())
            return WEXITSTATUS(status_);
        if (killed())
            return WTERMSIG(status_);
        if (stopped())
            return WSTOPSIG(status_);
        return -1;
    }

    /**
     * Closes the given end of the pipe.
     */
    void close(pipe_t::pipe_end end)
    {
        pipe_buf_.close(end);
    }

    /**
     * Write operator.
     */
    template <class T>
    friend std::ostream& operator<<(process& proc, T&& input)
    {
        return proc.in_stream_ << input;
    }

    /**
     * Conversion to std::ostream.
     */
    std::ostream& input()
    {
        return in_stream_;
    }

    /**
     * Conversion to std::istream.
     */
    std::istream& output()
    {
        return out_stream_;
    }

    /**
     * Read operator.
     */
    template <class T>
    friend std::istream& operator>>(process& proc, T& output)
    {
        return proc.out_stream_ >> output;
    }

    /**
     * An exception type for any unrecoverable errors that occur during
     * process operations.
     */
    class exception : public std::runtime_error
    {
      public:
        using std::runtime_error::runtime_error;
    };

  private:
    void recursive_close_stdin()
    {
        pipe_buf_.stdin_pipe().close();
        if (read_from_)
            read_from_->recursive_close_stdin();
    }

    std::vector<std::string> args_;
    process* read_from_ = nullptr;
    limits_t limits_;
    pid_t pid_ = -1;
    pipe_streambuf pipe_buf_;
    std::istream out_stream_;
    std::ostream in_stream_;
    bool waited_ = false;
    int status_;
};

/**
 * Class that represents a pipeline of child processes. The process objects
 * that are part of the pipeline are assumed to live longer than or as long
 * as the pipeline itself---the pipeline does not take ownership of the
 * processes.
 */
class pipeline
{
  public:
    friend pipeline operator|(process& first, process& second);

    /**
     * Constructs a longer pipeline by adding an additional process.
     */
    pipeline& operator|(process& tail)
    {
        tail.read_from(*processes_.back());
        processes_.push_back(&tail);
        return *this;
    }

    /**
     * Sets limits on all processes in the pipieline.
     */
    pipeline& limit(process::limits_t limits)
    {
        for (auto& proc : processes_)
            proc->limit(limits);
        return *this;
    }

    /**
     * Executes all processes in the pipeline.
     */
    void exec() const
    {
        for_each([](process& proc)
        { proc.exec(); });
    }

    /**
     * Obtains the process at the head of the pipeline.
     */
    process& head() const
    {
        return *processes_.front();
    }

    /**
     * Obtains the process at the tail of the pipeline.
     */
    process& tail() const
    {
        return *processes_.back();
    }

    /**
     * Waits for all processes in the pipeline to finish.
     */
    void wait() const
    {
        for_each([](process& proc)
        { proc.wait(); });
    }

    /**
     * Performs an operation on each process in the pipeline.
     */
    template <class Function>
    void for_each(Function&& function) const
    {
        for (auto& proc : processes_)
            function(*proc);
    }

  private:
    pipeline(process& head) : processes_{&head}
    {
        // nothing
    }

    std::vector<process*> processes_;
};

/**
 * Begins constructing a pipeline from two processes.
 */
inline pipeline operator|(process& first, process& second)
{
    pipeline p{first};
    p | second;
    return std::move(p);
}
#endif
