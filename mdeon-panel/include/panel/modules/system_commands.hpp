#pragma once

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace SystemCommands {

struct Result {
    int exit_code{127};
    std::string output;
};

inline Result run(const std::vector<std::string>& arguments)
{
    Result result;
    if (arguments.empty()) {
        return result;
    }

    int output_pipe[2];
    if (pipe(output_pipe) != 0) {
        return result;
    }

    const pid_t child = fork();
    if (child == 0) {
        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(output_pipe[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(output_pipe[1]);
    if (child < 0) {
        close(output_pipe[0]);
        return result;
    }

    char buffer[512];
    ssize_t bytes_read = 0;
    while ((bytes_read = read(output_pipe[0], buffer, sizeof(buffer))) != 0) {
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        result.output.append(buffer, static_cast<std::size_t>(bytes_read));
    }
    close(output_pipe[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

inline std::string find_executable(const std::string& executable)
{
    if (executable.empty()) {
        return {};
    }
    if (executable.find('/') != std::string::npos) {
        return access(executable.c_str(), X_OK) == 0 ? executable : std::string{};
    }

    const char* path = getenv("PATH");
    if (path == nullptr) {
        return {};
    }
    const std::string search_path(path);
    std::size_t start = 0;
    while (start <= search_path.size()) {
        const std::size_t separator = search_path.find(':', start);
        const std::string directory = search_path.substr(
            start, separator == std::string::npos ? std::string::npos : separator - start);
        const std::string candidate = (directory.empty() ? "." : directory) + "/" + executable;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return {};
}

inline bool launch_first(const std::vector<std::vector<std::string>>& candidates)
{
    for (const auto& arguments : candidates) {
        if (arguments.empty()) {
            continue;
        }
        const std::string executable = find_executable(arguments.front());
        if (executable.empty()) {
            continue;
        }

        const pid_t child = fork();
        if (child == 0) {
            const pid_t detached = fork();
            if (detached < 0) {
                _exit(127);
            }
            if (detached > 0) {
                _exit(0);
            }
            setsid();
            const int null_fd = open("/dev/null", O_RDWR);
            if (null_fd < 0) {
                _exit(127);
            }
            for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
                if (dup2(null_fd, fd) < 0) {
                    _exit(127);
                }
            }
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
            std::vector<char*> argv;
            argv.reserve(arguments.size() + 1);
            argv.push_back(const_cast<char*>(executable.c_str()));
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                argv.push_back(const_cast<char*>(arguments[index].c_str()));
            }
            argv.push_back(nullptr);
            execv(executable.c_str(), argv.data());
            _exit(127);
        }
        if (child < 0) {
            continue;
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    return false;
}

inline bool launch_fallback_chain(const std::vector<std::vector<std::string>>& candidates,
                                  int timeout_seconds)
{
    if (candidates.empty()) {
        return false;
    }

    const pid_t child = fork();
    if (child == 0) {
        const pid_t worker = fork();
        if (worker < 0) {
            _exit(127);
        }
        if (worker > 0) {
            _exit(0);
        }
        setsid();

        for (const auto& arguments : candidates) {
            if (arguments.empty()) {
                continue;
            }

            const pid_t command = fork();
            if (command == 0) {
                const int null_fd = open("/dev/null", O_RDWR);
                if (null_fd >= 0) {
                    dup2(null_fd, STDIN_FILENO);
                    dup2(null_fd, STDOUT_FILENO);
                    dup2(null_fd, STDERR_FILENO);
                    if (null_fd > STDERR_FILENO) {
                        close(null_fd);
                    }
                }

                std::vector<char*> argv;
                argv.reserve(arguments.size() + 1);
                for (const auto& argument : arguments) {
                    argv.push_back(const_cast<char*>(argument.c_str()));
                }
                argv.push_back(nullptr);
                execvp(argv[0], argv.data());
                _exit(127);
            }
            if (command < 0) {
                continue;
            }

            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(timeout_seconds);
            int status = 0;
            bool finished = false;
            bool command_succeeded = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const pid_t result = waitpid(command, &status, WNOHANG);
                if (result == command) {
                    finished = true;
                    command_succeeded = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                    break;
                }
                if (result < 0 && errno != EINTR) {
                    finished = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!finished || command_succeeded) {
                _exit(0);
            }
        }
        _exit(1);
    }
    if (child < 0) {
        return false;
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

inline bool launch_shell(const std::string& command)
{
    if (command.empty()) {
        return false;
    }
    const pid_t child = fork();
    if (child == 0) {
        const pid_t detached = fork();
        if (detached < 0) {
            _exit(127);
        }
        if (detached > 0) {
            _exit(0);
        }
        setsid();
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) {
        return false;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}