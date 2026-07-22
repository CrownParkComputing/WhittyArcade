#include "platform_file_dialog.h"

#include <array>
#include <sstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

std::vector<std::string> platform_file_selection(bool directory,
                                                 bool multiple,
                                                 const char* title) {
    int output_pipe[2]{};
    if (pipe(output_pipe) != 0) return {};
    const pid_t child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return {};
    }
    if (child == 0) {
        dup2(output_pipe[1], STDOUT_FILENO);
        close(output_pipe[0]);
        close(output_pipe[1]);
        const std::string title_argument = std::string("--title=") + title;
        if (directory) {
            execlp("zenity", "zenity", "--file-selection", "--directory",
                   "--modal", title_argument.c_str(),
                   static_cast<char*>(nullptr));
        } else if (multiple) {
            execlp("zenity", "zenity", "--file-selection", "--multiple",
                   "--separator=\n", "--modal",
                   "--file-filter=MAME ROM archives | *.zip *.ZIP",
                   title_argument.c_str(), static_cast<char*>(nullptr));
        } else {
            execlp("zenity", "zenity", "--file-selection", "--modal",
                   title_argument.c_str(), static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    close(output_pipe[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(output_pipe[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};

    std::vector<std::string> paths;
    std::stringstream lines(output);
    std::string path;
    while (std::getline(lines, path)) {
        if (!path.empty() && path.back() == '\r') path.pop_back();
        if (!path.empty()) paths.push_back(std::move(path));
    }
    return paths;
}
