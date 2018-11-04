#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
//#define DBG(x, ...) fprintf(stderr, x, __VA_ARGS__);
#define IS_PIPE(x) ((x) > 2)
using namespace std;

void convert(const vector<string> &from, vector<char *> &to) {
    // convert vector of c++ string to vector of c string
    auto it_end = from.end();
    for (auto it = from.begin(); it != it_end; ++it)
        to.push_back(const_cast<char *>(it->c_str()));
    to.push_back(NULL);
}

void mywait(deque<int> &pid) {
    int p;
    bool has_wait = false;
    // clean up finished process
    while ((p = waitpid(-1, NULL, WNOHANG)) > 0) {
        pid.erase(std::remove(pid.begin(), pid.end(), p), pid.end());
        has_wait = true;
    }
    if (has_wait) return;
    // wait for the front of deque
    if (!pid.empty()) {
        waitpid(pid.front(), NULL, 0);
        pid.pop_front();
    }
}

void exec(const vector<vector<string>> &args, deque<int> &pidout, int fdin,
          int fdout, int mode) {
    // fdin -> (exec args)  -> fdout
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

// default file permission mask 0666
const mode_t file_perm =
    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
// stdandard file descriptors
int stdfd[3];
// user info
int np_user[30];
string np_name[30], np_address[30];
// user npshell
map<string, string> np_env[30];
int np_line[30], np_fd_table[30][2000][2];
deque<int> np_pid_table[30][2000];
int initialize(int csock) {
    int uid = -1;
    for (size_t i = 0; i < 30; ++i)
        if (np_user[i] == -1) {
            np_user[i] = csock;
            uid = i;
            break;
        }
    // info
    np_name[uid] = "(no name)";
    // shell
    np_env[uid]["PATH"] = "bin:/";
    np_line[uid] = 1;
    int(&fd_table)[2000][2] = np_fd_table[uid];
    for (size_t i = 0; i < 2000; ++i) fd_table[i][0] = 0, fd_table[i][1] = 1;
    return uid;
}

void terminate(int uid) {
    np_user[uid] = -1;
    np_name[uid] = "(no name)";
    np_env[uid].clear();
    for (size_t i = 0; i < 2000; ++i) np_pid_table[uid][i].clear();
}

void broadcast(string msg) {
    const char *cmsg = msg.c_str();
    size_t clen = msg.size() + 1;
    for (size_t i = 0; i < 30; ++i) {
        if (np_user[i] != -1) {
            write(np_user[i], cmsg, clen);
        }
    }
}

int npshell(int uid) {
    const int sock = np_user[uid];
    for (size_t i = 0; i < 3; ++i) dup2(sock, i);
    // numbered pipe
    int &line = np_line[uid];
    int(&fd_table)[2000][2] = np_fd_table[uid];
    deque<int>(&pid_table)[2000] = np_pid_table[uid];
    string cmd, arg;
    // EOF
    if (!getline(cin, cmd)) {
        cout << endl;
        return -1;
    }
    stringstream ss(cmd);
    ss >> cmd;
    if (cmd.size() == 0) return 0;
    if (cmd == "\r") return 0;
    line = (line + 1) % 2000;
    // set environment variables
    for (const pair<string, string> var : np_env[uid])
        setenv(var.first.c_str(), var.second.c_str(), 1);
    if (cmd == "setenv") {
        // synopsis: setenv [environment variable] [value to assign]
        ss >> cmd >> arg;
        np_env[uid][cmd] = arg;
        setenv(cmd.c_str(), arg.c_str(), 1);
    } else if (cmd == "printenv") {
        // synopsis: printenv [environment variable]
        ss >> arg;
        char *env = getenv(arg.c_str());
        if (env) cout << env << endl;
    } else if (cmd == "exit") {
        // synopsis: exit
        // unset env
        for (const pair<string, string> var : np_env[uid])
            unsetenv(var.first.c_str());
        return -1;
    } else if (cmd == "name") {
        ss >> arg;
        bool found = false;
        for (size_t i = 0; i < 30; ++i)
            if (np_name[i] == arg) {
                found = true;
                break;
            }
        if (found) {
            cout << "*** User '" << arg << "' already exists. ***" << endl;
        } else {
            np_name[uid] = arg;
            string msg = "*** User from " + np_address[uid] + " is named '" +
                         arg + "'. ***\n";
            broadcast(msg);
        }
    } else if (cmd == "who") {
        cout << "<ID>\t<nickname>\t<IP/port>\t <indicate me>" << endl;
        for (int i = 0; i < 30; ++i) {
            if (np_user[i] != -1) {
                cout << i + 1 << '\t' << np_name[i] << '\t' << np_address[i];
                if (i == uid) cout << "\t<-me";
                cout << endl;
            }
        }
    } else if (cmd == "tell") {
        int tuid;
        ss >> tuid;
        --tuid;
        ws(ss);
        getline(ss, arg);
        if (np_user[tuid] == -1) {
            cout << "*** Error: user #" << tuid << " does not exist yet. ***"
                 << endl;
        } else {
            string msg = "*** " + np_name[uid] + " told you ***: " + arg + "\n";
            write(np_user[tuid], msg.c_str(), msg.size() + 1);
        }
    } else if (cmd == "yell") {
        ws(ss);
        getline(ss, arg);
        string msg = "*** " + np_name[uid] + " yelled ***: " + arg + "\n";
        broadcast(msg);
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
                                pid_table[line].begin(), pid_table[line].end());
        pid_table[line].clear();
        // prepare fd
        if (mode == 0) {
            // 0: open file
            fd_table[nline][1] =
                open(cmd.c_str(), O_WRONLY | O_CREAT | O_TRUNC, file_perm);
        } else if (mode == 20 || mode == 21) {
            // 20, 21: open numbered pipe
            if (!IS_PIPE(fd_table[nline][0]))
                while (pipe(fd_table[nline]) == -1) mywait(pid_table[nline]);
        }
        // execute commands
        if (IS_PIPE(fd_table[line][1])) close(fd_table[line][1]);
        exec(args, pid_table[nline], fd_table[line][0], fd_table[nline][1],
             mode);
        if (IS_PIPE(fd_table[line][0])) close(fd_table[line][0]);
        // wait for current line
        if (mode < 20) {
            for (int p : pid_table[nline]) waitpid(p, NULL, 0);
        }
        // cleanup current line
        fd_table[line][0] = 0;
        fd_table[line][1] = 1;
    }
    // unset
    for (const pair<string, string> var : np_env[uid])
        unsetenv(var.first.c_str());
    return 0;
}

int main() {
    // server socket
    int ssock = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(ssock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in saddr, caddr;
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(5566);
    bind(ssock, (struct sockaddr *)&saddr, sizeof(saddr));
    listen(ssock, 30);
    // client socket
    int csock;
    socklen_t clen = sizeof(caddr);
    char cip[INET_ADDRSTRLEN];
    int nfds = getdtablesize();
    fd_set rfds, afds;
    FD_ZERO(&afds);
    FD_SET(ssock, &afds);
    // initialize
    for (size_t i = 0; i < 3; ++i) stdfd[i] = dup(i);
    for (size_t i = 0; i < 30; ++i) np_user[i] = -1;
    // welcome message
    const char welcome[] =
        "***************************************\n"
        "** Welcome to the information server **\n"
        "***************************************\n";
    const size_t wlen = sizeof(welcome);
    while (true) {
        memcpy(&rfds, &afds, sizeof(rfds));
        select(nfds, &rfds, NULL, NULL, NULL);
        if (FD_ISSET(ssock, &rfds)) {
            csock = accept(ssock, (struct sockaddr *)&caddr, &clen);
            FD_SET(csock, &afds);
            // initialize
            int uid = initialize(csock);
            inet_ntop(AF_INET, &caddr.sin_addr, cip, INET_ADDRSTRLEN);
            np_address[uid] =
                string(cip) + "/" + to_string(htons(caddr.sin_port));
            // welcome message
            write(csock, welcome, wlen);
            // broadcast login
            string msg = "*** User '(no name)' entered from " +
                         np_address[uid] + ". ***\n";
            broadcast(msg);
            // prompt
            write(csock, "% ", 3);
        }
        for (int sock = 0; sock < nfds; ++sock) {
            if (sock != ssock && FD_ISSET(sock, &rfds)) {
                // map sock -> uid
                int uid = -1;
                for (size_t i = 0; i < 30; ++i) {
                    if (np_user[i] == sock) {
                        uid = i;
                        break;
                    }
                }
                // npshell
                int ret = npshell(uid);
                if (ret == -1) {
                    shutdown(sock, SHUT_RDWR);
                    close(sock);
                    FD_CLR(sock, &afds);
                    // broadcast logout
                    string msg = "*** User '" + np_name[uid] + "' left. ***\n";
                    broadcast(msg);
                    // cleanup shell
                    terminate(uid);
                } else {
                    // prompt
                    write(sock, "% ", 3);
                }
            }
        }
    }
    for (size_t i = 0; i < 3; ++i) close(stdfd[i]);
}
