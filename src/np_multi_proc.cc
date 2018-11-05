#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#define IS_PIPE(x) ((x) > 2)
using namespace std;

void convert(const vector<string> &from, vector<char *> &to) {
    // convert vector of c++ string to vector of c string
    auto it_end = from.end();
    for (auto it = from.begin(); it != it_end; ++it)
        to.push_back(const_cast<char *>(it->c_str()));
    to.push_back(nullptr);
}

void mywait(deque<int> &pid) {
    int p;
    bool has_wait = false;
    // clean up finished process
    while ((p = waitpid(-1, nullptr, WNOHANG)) > 0) {
        pid.erase(std::remove(pid.begin(), pid.end(), p), pid.end());
        has_wait = true;
    }
    if (has_wait) return;
    // wait for the front of deque
    if (!pid.empty()) {
        waitpid(pid.front(), nullptr, 0);
        pid.pop_front();
    }
}

void exec(const vector<vector<string>> &args, deque<int> &pidout, int fdin,
          int fdout, int mode) {
    // fdin -> (exec args) -> fdout
    const size_t len = args.size();
    size_t i, cur;
    int pid[2], fd[2][2];
    for (i = 0; i < len; ++i) {
        cur = i & 1;
        if (i != len - 1) {
            while (pipe(fd[cur]) == -1) mywait(pidout);
        }
        while ((pid[cur] = fork()) == -1) mywait(pidout);
        if (pid[cur] == 0) {
            // fd[0] -> stdin
            if (i != 0) {
                close(fd[1 - cur][1]);
                dup2(fd[1 - cur][0], 0);
            } else {
                dup2(fdin, 0);
            }
            // fd[1] -> stdout
            if (i != len - 1) {
                close(fd[cur][0]);
                dup2(fd[cur][1], 1);
            } else {
                dup2(fdout, 1);
                if (mode == 21) dup2(fdout, 2);
            }
            vector<char *> arg;
            convert(args[i], arg);
            execvp(arg[0], &arg[0]);
            cerr << "Unknown command: [" << arg[0] << "]." << endl;
            exit(0);
        }
        pidout.push_back(pid[cur]);
        if (i != 0) {
            close(fd[1 - cur][0]);
            close(fd[1 - cur][1]);
        }
    }
}

void npshell() {
    // default environment variables
    clearenv();
    setenv("PATH", "bin:.", 1);
    // default file permission mask 0666
    mode_t file_perm =
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    // numbered pipe
    int line, fd_table[2000][2];
    for (int(&fd)[2] : fd_table) fd[0] = 0, fd[1] = 1;
    deque<int> pid_table[2000];
    // npshell
    string cmd, arg;
    while (true) {
        // prompt string
        cout << "% ";
        // EOF
        if (!getline(cin, cmd)) {
            cout << endl;
            break;
        }
        stringstream ss(cmd);
        ss >> cmd;
        if (cmd.size() == 0) continue;
        line = (line + 1) % 2000;
        if (cmd == "setenv") {
            // synopsis: setenv [environment variable] [value to assign]
            ss >> cmd >> arg;
            setenv(cmd.c_str(), arg.c_str(), 1);
        } else if (cmd == "printenv") {
            // synopsis: printenv [environment variable]
            ss >> arg;
            char *env = getenv(arg.c_str());
            if (env) cout << env << endl;
        } else if (cmd == "exit") {
            // synopsis: exit
            break;
        } else {
            /* mode
             0: stdout to overwrite file
             10: single line stdout pipe (default)
             20: stdout numbered pipe
             21: stdout stderr numbered pipe
            */
            int np = -1, mode = 10;
            // parse all commands and arguments
            vector<vector<string>> args;
            bool has_next = true;
            while (has_next) {
                has_next = false;
                vector<string> argv = {cmd};
                while (ss >> arg) {
                    if (arg == ">") {
                        mode = 0;
                        ss >> cmd;
                        break;
                    } else if (arg == "|") {
                        has_next = true;
                        ss >> cmd;
                        break;
                    } else if (arg[0] == '|' || arg[0] == '!') {
                        np = stoi(arg.substr(1, arg.size() - 1));
                        assert(np > 0);
                        mode = 20 + (arg[0] == '!' ? 1 : 0);
                        break;
                    }
                    argv.push_back(arg);
                }
                args.emplace_back(argv);
            }
            // enqueue previous pid
            int nline = (line + np) % 2000;
            pid_table[nline].insert(pid_table[nline].begin(),
                                    pid_table[line].begin(),
                                    pid_table[line].end());
            pid_table[line].clear();
            // prepare fd
            if (mode == 0) {
                // 0: open file
                fd_table[nline][1] =
                    open(cmd.c_str(), O_WRONLY | O_CREAT | O_TRUNC, file_perm);
            } else if (mode == 20 || mode == 21) {
                // 20, 21: open numbered pipe
                if (!IS_PIPE(fd_table[nline][0]))
                    while (pipe(fd_table[nline]) == -1)
                        mywait(pid_table[nline]);
            }
            // execute commands
            if (IS_PIPE(fd_table[line][1])) close(fd_table[line][1]);
            exec(args, pid_table[nline], fd_table[line][0], fd_table[nline][1],
                 mode);
            if (IS_PIPE(fd_table[line][0])) close(fd_table[line][0]);
            // wait for current line
            if (mode < 20) {
                for (int p : pid_table[nline]) waitpid(p, nullptr, 0);
            }
            // cleanup current line
            fd_table[line][0] = 0;
            fd_table[line][1] = 1;
        }
    }
}

void reaper(int sig) {
    while (waitpid(-1, nullptr, WNOHANG) > 0)
        ;
}

int main(int argc, char **argv) {
    // server socket
    int ssock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr, caddr;
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (argc > 1) {
        uint16_t port;
        stringstream ss(argv[1]);
        ss >> port;
        saddr.sin_port = htons(port);
    } else {
        saddr.sin_port = htons(5566);
    }
    bind(ssock, (struct sockaddr *)&saddr, sizeof(saddr));
    listen(ssock, 5);
    // reap client child
    struct sigaction sa;
    sa.sa_handler = &reaper;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);
    // accept client
    int csock;
    socklen_t clen = sizeof(caddr);
    while (true) {
        csock = accept(ssock, (struct sockaddr *)&caddr, &clen);
        if (fork() == 0) {
            close(ssock);
            break;
        }
        close(csock);
    }
    // client npshell
    dup2(csock, 0);
    dup2(csock, 1);
    dup2(csock, 2);
    npshell();
}
