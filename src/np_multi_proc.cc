#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#define IS_PIPE(x) ((x) > 2)
using namespace std;

int my_uid = -1;
string my_address, my_name;
int shm_pid, shm_address[30], shm_name[30], shm_msg[30];
int sem_pid, sem_address, sem_name, sem_msg;

void sem_wait(int key, short unsigned int sem = 0) {
    // lock
    static struct sembuf act = {sem, -1, SEM_UNDO};
    semop(key, &act, sizeof(act));
}

void sem_signal(int key, short unsigned int sem = 0) {
    // unlock
    static struct sembuf act = {sem, 1, SEM_UNDO};
    semop(key, &act, sizeof(act));
}

void broadcast(string msg) {
    sem_wait(sem_pid);
    int *np_user = (int *)shmat(shm_pid, nullptr, 0);
    for (size_t i = 0; i < 30; ++i) {
        if (np_user[i] != -1) {
            sem_wait(sem_msg, i);
            char *np_msg = (char *)shmat(shm_msg[i], nullptr, 0);
            strcpy(np_msg, msg.c_str());
            shmdt(np_msg);
            kill(np_user[i], SIGUSR1);
        }
    }
    shmdt(np_user);
    sem_signal(sem_pid);
}

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
        if (!cmd.empty() && cmd[cmd.length() - 1] == '\n')
            cmd.erase(cmd.length() - 1);
        if (!cmd.empty() && cmd[cmd.length() - 1] == '\r')
            cmd.erase(cmd.length() - 1);
        const string full_cmd = cmd;
        stringstream ss(cmd);
        ss >> cmd;
        if (cmd.empty()) continue;
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
        } else if (cmd == "name") {
            // synopsis: name [new username]
            ss >> arg;
            bool found = false;
            const char *name = arg.c_str();
            char *np_name;
            sem_wait(sem_name);
            for (int shm_ni : shm_name) {
                np_name = (char *)shmat(shm_ni, nullptr, 0);
                if (strcmp(np_name, name) == 0) {
                    found = true;
                    shmdt(np_name);
                    break;
                }
                shmdt(np_name);
            }
            if (found) {
                cout << "*** User '" << arg << "' already exists. ***" << endl;
            } else {
                np_name = (char *)shmat(shm_name[my_uid], nullptr, 0);
                my_name = name;
                strcpy(np_name, name);
                shmdt(np_name);
                string msg = "*** User from " + my_address + " is named '" +
                             arg + "'. ***\n";
                broadcast(msg);
            }
            sem_signal(sem_name);
        } else if (cmd == "who") {
            // synopsis: who
            sem_wait(sem_pid);
            sem_wait(sem_address);
            sem_wait(sem_name);
            int *np_pid = (int *)shmat(shm_pid, nullptr, 0);
            char *np_address, *np_name;
            cout << "<ID>\t<nickname>\t<IP/port>\t<indicate me>" << endl;
            for (int i = 0; i < 30; ++i) {
                if (np_pid[i] != -1) {
                    np_address = (char *)shmat(shm_address[i], nullptr, 0);
                    np_name = (char *)shmat(shm_name[i], nullptr, 0);
                    cout << i + 1 << '\t' << string(np_name) << '\t'
                         << string(np_address);
                    if (i == my_uid) cout << "\t<-me";
                    cout << endl;
                    shmdt(np_address);
                    shmdt(np_name);
                }
            }
            shmdt(np_pid);
            sem_signal(sem_pid);
            sem_signal(sem_address);
            sem_signal(sem_name);
        } else if (cmd == "tell") {
            // synopsis: tell [user id] [message]
            int tuid, tpid;
            ss >> tuid;
            --tuid;
            ws(ss);
            getline(ss, arg);
            sem_wait(sem_pid);
            int *np_pid = (int *)shmat(shm_pid, nullptr, 0);
            tpid = np_pid[tuid];
            shmdt(np_pid);
            sem_signal(sem_pid);
            if (tpid == -1) {
                cout << "*** Error: user #" << (tuid + 1)
                     << " does not exist yet. ***" << endl;
            } else {
                string msg = "*** " + my_name + " told you ***: " + arg + "\n";
                sem_wait(sem_msg, tuid);
                char *np_msg = (char *)shmat(shm_msg[tuid], nullptr, 0);
                strcpy(np_msg, msg.c_str());
                shmdt(np_msg);
                kill(tpid, SIGUSR1);
            }
        } else if (cmd == "yell") {
            // synopsis: yell [message]
            ws(ss);
            getline(ss, arg);
            string msg = "*** " + my_name + " yelled ***: " + arg + "\n";
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

void cleanIPC(int sig) {
    // shared memory
    shmctl(shm_pid, IPC_RMID, nullptr);
    for (size_t i = 0; i < 30; ++i) {
        shmctl(shm_address[i], IPC_RMID, nullptr);
        shmctl(shm_name[i], IPC_RMID, nullptr);
        shmctl(shm_msg[i], IPC_RMID, nullptr);
    }
    // semaphore
    semctl(sem_pid, 0, IPC_RMID, nullptr);
    semctl(sem_address, 0, IPC_RMID, nullptr);
    semctl(sem_name, 0, IPC_RMID, nullptr);
    semctl(sem_msg, 0, IPC_RMID, nullptr);
    exit(0);
}

void show_msg(int sig) {
    char *np_msg = (char *)shmat(shm_msg[my_uid], nullptr, 0);
    cout << string(np_msg);
    shmdt(np_msg);
    sem_signal(sem_msg, my_uid);
}

int initialize_uid() {
    int ret;
    sem_wait(sem_pid);
    int *np_pid = (int *)shmat(shm_pid, nullptr, 0);
    for (size_t i = 0; i < 30; ++i) {
        if (np_pid[i] == -1) {
            ret = i;
            break;
        }
    }
    shmdt(np_pid);
    sem_signal(sem_pid);
    return ret;
}

int main(int argc, char **argv) {
    // shared memory
    const int ipcflag = IPC_CREAT | 0600;
    shm_pid = shmget(IPC_PRIVATE, 30 * sizeof(int), ipcflag);
    for (size_t i = 0; i < 30; ++i) {
        shm_address[i] = shmget(IPC_PRIVATE, 24, ipcflag);
        shm_name[i] = shmget(IPC_PRIVATE, 24, ipcflag);
        shm_msg[i] = shmget(IPC_PRIVATE, 1025, ipcflag);
    }
    // initialized shared memory
    int *np_pid = (int *)shmat(shm_pid, nullptr, 0);
    for (size_t i = 0; i < 30; ++i) np_pid[i] = -1;
    shmdt(np_pid);
    // semaphore
    sem_pid = semget(IPC_PRIVATE, 1, ipcflag);
    sem_address = semget(IPC_PRIVATE, 1, ipcflag);
    sem_name = semget(IPC_PRIVATE, 1, ipcflag);
    sem_msg = semget(IPC_PRIVATE, 30 * 1, ipcflag);
    // cleanup ipc
    struct sigaction sa_sigterm;
    sa_sigterm.sa_handler = &cleanIPC;
    sigemptyset(&sa_sigterm.sa_mask);
    sigaction(SIGINT, &sa_sigterm, nullptr);
    sigaction(SIGTERM, &sa_sigterm, nullptr);
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
    struct sigaction sa_sigchld;
    sa_sigchld.sa_handler = &reaper;
    sigemptyset(&sa_sigchld.sa_mask);
    sa_sigchld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_sigchld, nullptr);
    // accept client
    int csock, pid, uid;
    char cip[INET_ADDRSTRLEN];
    socklen_t clen = sizeof(caddr);
    string address;
    while (true) {
        csock = accept(ssock, (struct sockaddr *)&caddr, &clen);
        inet_ntop(AF_INET, &caddr.sin_addr, cip, INET_ADDRSTRLEN);
        address = string(cip) + "/" + to_string(htons(caddr.sin_port));
        uid = initialize_uid();
        // shm_name
        sem_wait(sem_name);
        char *np_name = (char *)shmat(shm_name[uid], nullptr, 0);
        strcpy(np_name, "(no name)");
        shmdt(np_name);
        sem_signal(sem_name);
        // shm_address
        sem_wait(sem_address);
        char *np_address = (char *)shmat(shm_address[uid], nullptr, 0);
        strcpy(np_address, address.c_str());
        shmdt(np_address);
        sem_signal(sem_address);
        // shm_pid
        sem_wait(sem_pid);
        // fork client
        if ((pid = fork()) == 0) {
            close(ssock);
            break;
        }
        close(csock);
        int *np_pid = (int *)shmat(shm_pid, nullptr, 0);
        np_pid[uid] = pid;
        shmdt(np_pid);
        sem_signal(sem_pid);
    }
    // client npshell
    dup2(csock, 0);
    dup2(csock, 1);
    dup2(csock, 2);
    // message handler
    my_uid = uid;
    my_address = address;
    my_name = "(no name)";
    struct sigaction sa_sigusr1;
    sa_sigusr1.sa_handler = &show_msg;
    sigemptyset(&sa_sigusr1.sa_mask);
    sigaction(SIGUSR1, &sa_sigusr1, nullptr);
    cout << "****************************************" << endl
         << "** Welcome to the information server. **" << endl
         << "****************************************" << endl;
    // broadcast login
    string msg = "*** User '(no name)' entered from " + my_address + ". ***\n";
    broadcast(msg);
    // npshell
    npshell();
    // broadcast logout
    sem_wait(sem_name);
    char *np_user = (char *)shmat(shm_name[uid], nullptr, 0);
    msg = "*** User '" + string(np_user) + "' left. ***\n";
    strcpy(np_user, "(no name)");
    shmdt(np_user);
    sem_signal(sem_name);
    broadcast(msg);
    // cleanup shell
    sem_wait(sem_pid);
    np_pid = (int *)shmat(shm_pid, nullptr, 0);
    np_pid[uid] = -1;
    shmdt(np_pid);
    sem_signal(sem_pid);
}
