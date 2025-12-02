#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string>
#include <cstdlib>
#include <sys/wait.h>
#include <vector>
#include <array>
#include <deque>
#include <iomanip>
#include <signal.h>
#include <random>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <chrono>
#include "resources.h"

using namespace std;

struct PCB {
    bool occupied;
    pid_t pid;
    int start_sec;
    int start_nano;
    int pcb_index;
};

struct MessageBuffer {
    long mtype;
    pid_t pid;
    int request_or_release; // 1 for request, 0 for release
    int resource_request[MAX_RESOURCES]; // array of resource requests
    int resource_release[MAX_RESOURCES]; // array of resource releases
    int mass_release; // 1 if mass release 0 if not
    int process_running; // 1 if running, 0 if not
};

// Globals
key_t sh_key = ftok("oss.cpp", 0);
int shmid = shmget(sh_key, sizeof(int)*2, IPC_CREAT | 0666);
int *shm_clock;
int *sec;
vector <PCB> table(MAX_PROCESSES);
resource_descriptor resource_table;
deque<MessageBuffer> process_queue;
const int increment_amount = 10000;

// setup message queue
key_t msg_key = ftok("oss.cpp", 1);
int msgid = msgget(msg_key, IPC_CREAT | 0666);

// global log stream and helper so other functions can log to the same place as main
ofstream log_fs;
static const size_t MAX_LOG_LINES = 10000;
static size_t log_lines_written = 0;
static inline void oss_log_msg(const string &s) {
    // always print to stdout
    cout << s;
    if (!log_fs.is_open()) return;
    size_t newlines = count(s.begin(), s.end(), '\n'); // count how many new lines this message contains
    if (log_lines_written >= MAX_LOG_LINES) return; // if limit is reached, skip
    if (log_lines_written + newlines > MAX_LOG_LINES) return; // skip message if it would exceed limit
    // else write the whole message and update counter
    log_fs << s;
    log_fs.flush();
    log_lines_written += newlines;
}

void increment_clock(int* sec, int* nano, long long inc_ns) {
    const long long NSEC_PER_SEC = 1000000000LL;
    if (inc_ns <= 0) inc_ns = 1; // guard against non-positive increments
    long long total = (long long)(*nano) + inc_ns;
    *sec += (int)(total / NSEC_PER_SEC);
    *nano = (int)(total % NSEC_PER_SEC);
}

// convert float time interval to seconds and nanoseconds and return nannoseconds
int seconds_conversion(float interval) {
    int seconds = (int)interval;
    float fractional = interval - (float)seconds;
    int nanoseconds = (int)(fractional * 1e9);
    return nanoseconds;
}

// check if any child has terminated, return pid if so, else -1
pid_t child_Terminated() {
    int status;
    pid_t result = waitpid(-1, &status, WNOHANG);
    if (result > 0) {
        return result;
    }
    return -1;
}

pid_t launch_worker(float time_limit) {
    pid_t worker_pid = fork();
    if (worker_pid < 0) {
        cerr << "fork failed" << endl;
        exit(1);
    }

    if (worker_pid == 0) {
        string arg_sec = to_string((int)time_limit);
        string arg_nsec = to_string(seconds_conversion(time_limit));
        char* args[] = {
            (char*)"./worker",
            const_cast<char*>(arg_sec.c_str()),
            const_cast<char*>(arg_nsec.c_str()),
            NULL
        };
        execv(args[0], args);
        cerr << "Exec failed" << endl;
        exit(1);
    }
    return worker_pid;
}

// find an empty PCB slot, return index or -1 if none found
int find_empty_pcb(const vector<PCB> &table) {
    for (size_t i = 0; i < table.size(); ++i) {
        if (!table[i].occupied) {
            return i;
        }
    }
    return -1;
}

int find_pcb_by_pid(pid_t pid) {
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].occupied && table[i].pid == pid) {
            return (int)i;
        }
    }
    return -1;
}

int remove_pcb(vector<PCB> &table, pid_t pid) {
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].occupied && table[i].pid == pid) {
            table[i].occupied = false;
            table[i].pid = -1;
            table[i].start_sec = 0;
            table[i].start_nano = 0;
            table[i].pcb_index = -1;
            return i;
        }
    }
    return -1;
}

void print_process_table(const std::vector<PCB> &table, bool verbose) {
    ostringstream ss;
    using std::endl;
    ss << std::left
         << std::setw(6)  << "Index"
         << std::setw(10) << "Occ"
         << std::setw(12) << "PID"
         << std::setw(12) << "StartSec"
         << std::setw(12) << "StartNano" << endl;
    ss << std::string(52, '-') << endl;

    for (size_t i = 0; i < table.size(); ++i) {
        const PCB &p = table[i];
        ss << std::left << std::setw(6) << i
           << std::setw(10) << (p.occupied ? 1 : 0);
        if (p.occupied) {
            ss << std::setw(12) << p.pid
               << std::setw(12) << p.start_sec
               << std::setw(12) << p.start_nano;
        } else {
            ss << std::setw(12) << "-" << std::setw(12) << "-" << std::setw(12) << "-";
        }
        ss << endl;
    }
    ss << endl;
    oss_log_msg(ss.str());

}

void print_allocation_matrix(const std::array<std::array<int, MAX_RESOURCES>, MAX_PROCESSES> &allocation_matrix, bool verbose) {
    using std::endl;
    std::ostringstream ss;

    const int proc_col = 8;
    const int res_col = 8;

    // Header
    ss << std::left << std::setw(proc_col) << "Index";
    for (int r = 0; r < MAX_RESOURCES; ++r) {
        ss << std::right << std::setw(res_col) << ("R" + std::to_string(r));
    }
    ss << endl;

    // Separator
    ss << std::string(proc_col + res_col * MAX_RESOURCES, '-') << endl;

    // Rows
    for (int p = 0; p < MAX_PROCESSES; ++p) {
        ss << std::left << std::setw(proc_col) << p;
        for (int r = 0; r < MAX_RESOURCES; ++r) {
            ss << std::right << std::setw(res_col) << allocation_matrix[p][r];
        }
        ss << endl;
    }
    ss << endl;
    oss_log_msg(ss.str());
}

void signal_handler(int sig) {
    if (sig == SIGALRM || sig == SIGINT) {
        cout << "Received SIGALRM or SIGINT, terminating all child processes..." << endl;
        // Terminate all child processes and clean up shared memory
        shmdt(shm_clock);
        shmctl(shmid, IPC_RMID, nullptr);
        msgctl(msgid, IPC_RMID, nullptr);
        kill(0, SIGTERM); 
        exit(0);
    }
}

void exit_handler() {
    shmdt(shm_clock);
    shmctl(shmid, IPC_RMID, nullptr);
    msgctl(msgid, IPC_RMID, nullptr);
    exit(1);
}

// helper to detect empty/blank optarg
static inline bool optarg_blank(const char* s) {
    return (s == nullptr) || (s[0] == '\0');
}

int main(int argc, char* argv[]) {
    //parse command line args
    int proc = -1;
    int simul = -1;
    float time_limit = -1;
    float launch_interval = -1;
    bool verbose_mode = false;
    string log_file = "";
    int opt;

    while((opt = getopt(argc, argv, "hn:s:t:i:f:v")) != -1) {
        switch(opt) {
            case 'h': {
                cout << "Usage: oss -n proc -s simul -t time_limit -i launch_interval\n"
                    << "Options:\n"
                    << "  -h                Show this help message and exit\n"
                    << "  -n proc           Total number of worker processes to launch (non-negative integer)\n"
                    << "  -s simul          Maximum number of simultaneous worker processes (positive integer)\n"
                    << "  -t time_limit     Time limit for each worker process in seconds (non-negative float)\n"
                    << "  -i launch_interval Interval between launching worker processes in seconds (non-negative float)\n"
                    << "  -f logfile        Log file name (optional)\n"
                    << "  -v                Turn on verbose mode\n"
                    << "Example:\n"
                    << "  ./oss -n 10 -s 3 -t 2.5 -i 0.5 -f oss.log\n";
                exit_handler();
            }
            case 'n': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -n requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    int val = stoi(optarg);
                    if (val < 0) throw invalid_argument("negative");
                    proc = val;
                } catch (...) {
                    cerr << "Error: -n must be a non-negative integer." << endl;
                    exit_handler();
                }
                 break;
            }
            case 's': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -s requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    int val = stoi(optarg);
                    if (val <= 0) throw invalid_argument("non-positive");
                    simul = val;
                } catch (...) {
                    cerr << "Error: -s must be a positive integer." << endl;
                    exit_handler();
                }
                 break;
            }
            case 't': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -t requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    float val = stof(optarg);
                    if (val < 0.0f) throw invalid_argument("negative");
                    time_limit = val;
                } catch (...) {
                    cerr << "Error: -t must be a non-negative number." << endl;
                    exit_handler();
                }
                 break;
            }
            case 'i': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -i requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    float val = stof(optarg);
                    if (val < 0.0f) throw invalid_argument("negative");
                    launch_interval = val;
                } catch (...) {
                    cerr << "Error: -i must be a non-negative number." << endl;
                    exit_handler();
                }
                 break;
            }
            case 'f': {
                // Optional: handle log file name if needed
                if (!optarg_blank(optarg)) log_file = optarg;
                else {
                    cerr << "Error: -f requires a non-blank filename." << endl;
                    exit_handler();
                }
                 break;
            }
            case 'v': {
                verbose_mode = true;
                break;
            }
            default:
                cerr << "Error: Unknown option or missing argument." << endl;
                exit_handler();
        }
    }

    // final validation of required options
    if (proc == -1 || simul == -1 || time_limit < 0.0f || launch_interval < 0.0f) {
        cerr << "Error: Missing required options. Usage: ./oss -n proc -s simul -t time_limit -i launch_interval [-f logfile]" << endl;
        exit_handler();
    }

    // attach shared memory to shm_ptr
    shm_clock = (int*) shmat(shmid, nullptr, 0);
    if (shm_clock == (int*) -1) {
        cerr << "shmat";
        exit_handler();
    }

    // pointers to seconds and nanoseconds in shared memory
    int *sec = &(shm_clock[0]);
    int *nano = &(shm_clock[1]);
    *sec = *nano = 0;

    // Initialize PCB 
    for (size_t i = 0; i < table.size(); ++i) {
        table[i].occupied = false;
        table[i].pid = -1;
        table[i].start_sec = 0;
        table[i].start_nano = 0;
        table[i].pcb_index = -1;
    }

    time_t start_time = time(nullptr); // track time for 5 second real-time limit

    // print interval using simulated clock: 0.5 seconds
    const long long NSEC_PER_SEC = 1000000000LL;
    const long long PRINT_INTERVAL_NANO = 500000000LL;
    long long next_print_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano) + PRINT_INTERVAL_NANO;

    // signal handling
    signal(SIGALRM, signal_handler);
    signal(SIGINT, signal_handler);
    alarm(60);

    // Initialize random number generator
    random_device rd;
    mt19937 gen(rd());
    uniform_real_distribution<double> dis(1, time_limit);

    // open log file if specified
    if (!log_file.empty()) {
        log_fs.open(log_file);
        if (!log_fs) {
            cerr << "Error: Could not open log file " << log_file << endl;
            exit(1);
        }
    }

    // helper to log messages originating from OSS (writes to stdout and to log file if open)
    auto oss_log = [&](const string &s) {
        cout << s;
        if (log_fs.is_open()) log_fs << s;
    };

    // oss starting message
    {
        ostringstream ss;
        ss << "OSS starting, PID:" << getpid() << " PPID:" << getppid() << endl
           << "Called With:" << endl
           << "-n: " << proc << endl
           << "-s: " << simul << endl
           << "-t: " << time_limit << endl
           << "-i: " << launch_interval << endl;
        oss_log(ss.str());
    }

    // set initial resource table state
    resource_table.allocation_matrix.fill({0});
    int total_requests = 0;
    int total_mass_release = 0;
    int total_resources_requested = 0;
    int total_immediate_requests = 0;
    int print_allo_table_interval = 0; 

    int launched_processes = 0;
    int running_processes = 0;

    long long launch_interval_nano = (long long)(launch_interval * 1e9); // convert launch interval to nanoseconds
    long long next_launch_total = 0; 

    MessageBuffer rcvMessage;
    MessageBuffer ackMessage;

    while (launched_processes < proc || running_processes > 0) {
        increment_clock(sec, nano, increment_amount);

        // Check if it's time to launch a new worker
        long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
        if (launched_processes < proc && running_processes < simul && running_processes < MAX_PROCESSES && current_total >= next_launch_total && (time(nullptr) - start_time) < 5) {
            pid_t worker_pid = launch_worker(time_limit);

            // Find empty slot in PCB array and populate it with new process info
            int pcb_index = find_empty_pcb(table);
            if (pcb_index == -1) {
                // no free PCB slot found; avoid undefined behavior and kill the worker
                cerr << "OSS: no free PCB slot available for new worker (pid=" << worker_pid << "). Killing worker." << endl;
                kill(worker_pid, SIGTERM);
            } else {
                table[pcb_index].occupied = true;
                table[pcb_index].pid = worker_pid;
                table[pcb_index].start_sec = *sec;
                table[pcb_index].start_nano = *nano;
                table[pcb_index].pcb_index = pcb_index;

                launched_processes++;
                running_processes++;
            }

            // Update the next allowed launch time
            next_launch_total = current_total + launch_interval_nano;
            print_process_table(table, verbose_mode);
        }

        // process queued requests: scan whole queue and allocate any request that can be satisfied
        if (!process_queue.empty()) {
            bool allocated_any;
            do {
                allocated_any = false;
                for (size_t qi = 0; qi < process_queue.size(); ++qi) {
                    MessageBuffer &queued_msg = process_queue[qi];
                    int pcb_index = find_pcb_by_pid(queued_msg.pid);
                    if (pcb_index == -1) {
                        // PCB no longer exists; remove this queued message
                        process_queue.erase(process_queue.begin() + qi);
                        allocated_any = true; // restart scan
                        break;
                    }

                    bool can_allocate = true;
                    for (int i = 0; i < MAX_RESOURCES; i++) {
                        if (queued_msg.resource_request[i] > resource_table.available_resources[i]) {
                            can_allocate = false;
                            break;
                        }
                    }
                    if (can_allocate) {
                        // allocate resources
                        for (int i = 0; i < MAX_RESOURCES; i++) {
                            resource_table.available_resources[i] -= queued_msg.resource_request[i];
                            resource_table.allocation_matrix[pcb_index][i] += queued_msg.resource_request[i];
                        }
                        {
                            ostringstream ss;
                            ss << "OSS: Allocated queued resources to worker " << queued_msg.pid << " ";
                            for (int i = 0; i < MAX_RESOURCES; i++) {
                                if (queued_msg.resource_request[i] > 0) ss << "R" << i << ":" << queued_msg.resource_request[i] << " ";
                            }
                            ss << "at time " << *sec << "s " << *nano << "ns" << endl;
                            oss_log(ss.str());
                        }
                        // send ack message
                        memset(&ackMessage, 0, sizeof(ackMessage));
                        ackMessage.mtype = queued_msg.pid;
                        ackMessage.process_running = 1;
                        size_t ack_size = sizeof(MessageBuffer) - sizeof(long);
                        if (msgsnd(msgid, &ackMessage, ack_size, 0) == -1) {
                            perror("oss msgsnd ack failed");
                            exit_handler();
                        }
                        // remove this entry and restart scanning
                        process_queue.erase(process_queue.begin() + qi);
                        allocated_any = true;
                        break;
                    }
                    // if cannot allocate, continue to next queued message
                }
            } while (allocated_any && !process_queue.empty());
        }

        // non blocking message receive 
        ssize_t msg_size = sizeof(MessageBuffer) - sizeof(long);
        ssize_t ret = msgrcv(msgid, &rcvMessage, msg_size, getpid(), IPC_NOWAIT);
        if (ret == -1) {
            if (errno == ENOMSG) {
                // no message available, continue
            } else {
                perror("msgrcv");
                exit_handler();
            }
        } else {
            if (rcvMessage.process_running == 0) {
                // worker indicates it is terminating
                {
                    ostringstream ss;
                    ss << "OSS: Worker " << rcvMessage.pid << " indicates it is terminating. " << endl;
                    oss_log(ss.str());
                }
                int pcb_index = find_pcb_by_pid(rcvMessage.pid);
                if (pcb_index != -1) {
                    // clean PCB entry
                    remove_pcb(table, rcvMessage.pid);
                    // release allocated resources add them back to available pool
                    for (int i = 0; i < MAX_RESOURCES; i++) {
                        resource_table.available_resources[i] += resource_table.allocation_matrix[pcb_index][i];
                    }
                    resource_table.allocation_matrix[pcb_index].fill(0); // clean allocation entry
                }
                running_processes--;
                continue;
            }
            // process resource requests/releases
            if (rcvMessage.request_or_release == 1) {
                // update total requests and total resources requested
                total_requests++;
                for (int i = 0; i < MAX_RESOURCES; i++) {
                    total_resources_requested += rcvMessage.resource_request[i];
                }

                // check if resources are available
                int pcb_index = find_pcb_by_pid(rcvMessage.pid);
                if (pcb_index != -1) {
                    bool can_allocate = true;
                    for (int i = 0; i < MAX_RESOURCES; i++) {
                        if (rcvMessage.resource_request[i] > resource_table.available_resources[i]) {
                            can_allocate = false;
                            break;
                        }
                    }
                    if (can_allocate) {
                        // allocate resources
                        for (int i = 0; i < MAX_RESOURCES; i++) {
                            resource_table.available_resources[i] -= rcvMessage.resource_request[i];
                            resource_table.allocation_matrix[pcb_index][i] += rcvMessage.resource_request[i];
                        }
                    } else {
                        {
                            if (verbose_mode) {
                                ostringstream ss;
                                ss << "OSS: Resources not available for worker " << rcvMessage.pid << ", request queued." << " At time " << *sec << "s " << *nano << "ns" << endl;
                                oss_log(ss.str());
                            } else {
                                cout << "OSS: Resources not available for worker " << rcvMessage.pid << ", request queued." << " At time " << *sec << "s " << *nano << "ns" << endl;
                            }
                        }
                        process_queue.push_back(rcvMessage);
                        continue; // skip sending ack for now
                    }
                }
                {
                    ostringstream ss;
                    ss << "OSS: Resources allocated to worker " << rcvMessage.pid << " ";
                    for (int i = 0; i < MAX_RESOURCES; i++) {
                        if (rcvMessage.resource_request[i] > 0) ss << "R" << i << ":" << rcvMessage.resource_request[i] << " ";
                    }
                    ss << "at time " << *sec << "s " << *nano << "ns" << endl;
                    ss << "OSS: available resources: ";
                    for (int i = 0; i < MAX_RESOURCES; i++) {
                        ss << "R" << i << ":" << resource_table.available_resources[i] << " ";
                    }
                    ss << endl;
                    oss_log(ss.str());
                }
                total_immediate_requests++;
                if (++print_allo_table_interval >= 20 && verbose_mode) {
                    print_allocation_matrix(resource_table.allocation_matrix, verbose_mode);
                    print_allo_table_interval = 0;
                }
                // send message to worker acknowledging request
                memset(&ackMessage, 0, sizeof(ackMessage));
                ackMessage.mtype = rcvMessage.pid;
                ackMessage.process_running = 1;
                size_t ack_size = sizeof(MessageBuffer) - sizeof(long);
                if (msgsnd(msgid, &ackMessage, ack_size, 0) == -1) {
                    perror("oss msgsnd ack failed");
                    exit_handler();
                }
            }
            if (rcvMessage.request_or_release == 0) {
                if (rcvMessage.mass_release == 1) { total_mass_release++; }
                // release resources back to the available pool
                int pcb_index = find_pcb_by_pid(rcvMessage.pid);
                if (pcb_index != -1) {
                    for (int i = 0; i < MAX_RESOURCES; i++) {
                        resource_table.available_resources[i] += rcvMessage.resource_release[i];
                        resource_table.allocation_matrix[pcb_index][i] -= rcvMessage.resource_release[i];
                    }
                }
                {
                    if (verbose_mode) {
                        ostringstream ss;
                        ss << "OSS: Resources released by worker " << rcvMessage.pid << " ";
                        for (int i = 0; i < MAX_RESOURCES; i++) {
                            if (rcvMessage.resource_release[i] > 0) ss << "R" << i << ":" << rcvMessage.resource_release[i] << " ";
                        }
                        ss << "at time " << *sec << "s " << *nano << "ns" << endl;
                        ss << "OSS: available resources: ";
                        for (int i = 0; i < MAX_RESOURCES; i++) {
                            ss << "R" << i << ":" << resource_table.available_resources[i] << " ";
                        }
                        ss << endl;
                        oss_log(ss.str());
                    } else {
                        
                    }
                }
                // send message to worker acknowledging release
                memset(&ackMessage, 0, sizeof(ackMessage));
                ackMessage.mtype = rcvMessage.pid;
                ackMessage.process_running = 1;
                size_t ack_size = sizeof(MessageBuffer) - sizeof(long);
                if (msgsnd(msgid, &ackMessage, ack_size, 0) == -1) {
                    perror("oss msgsnd ack failed");
                    exit_handler();
                }
            }
        }

        // call print_process_table every half-second of simulated time
        {
            long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
            while (current_total >= next_print_total) {
                print_process_table(table, verbose_mode);
                print_allocation_matrix(resource_table.allocation_matrix, verbose_mode);
                next_print_total += PRINT_INTERVAL_NANO;
            }
        }
    }

    // ending report
    ostringstream ss;
    ss << "ENDING REPORT" << endl;
    ss << "Total resources Requested: " << total_resources_requested << endl;
    ss << "Total requests: " << total_requests << endl;
    ss << "Times mass release was done: " << total_mass_release << endl;
    ss << "Percentage of request granted immediately vs amount of total requests: " << (total_immediate_requests * 100.0 / total_requests) << "%" << endl;
    oss_log_msg(ss.str());

    // cleanup
     shmdt(shm_clock);
     shmctl(shmid, IPC_RMID, nullptr);
     msgctl(msgid, IPC_RMID, nullptr);
     return 0;
 }