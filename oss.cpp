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

using namespace std;

const int MAX_FRAMES = 64;
const int PAGE_SIZE = 1024;
const int TOTAL_PAGES = 16;

struct PCB {
    bool occupied;
    pid_t pid;
    int start_sec;
    int start_nano;
    int pcb_index;
    vector<int> page_table;

    PCB() : page_table(TOTAL_PAGES, -1) {} // constructor to initialize page_table
};

struct MessageBuffer {
    long mtype;
    pid_t pid;
    int process_running; // 1 if running, 0 if not
    int memory_location;
    bool write; // true if write request, false if read request
};

struct Frame {
    bool occupied;
    pid_t pid;
    int page_number;
    bool dirty;
};

struct IOQueueEntry {
    pid_t pid;
    int memory_location;
    bool write;
    long long unblock_time;
    int victim_frame_index;
};

// Globals
key_t sh_key = ftok("oss.cpp", 0);
int MAX_PROCESSES = 18;
int shmid = shmget(sh_key, sizeof(int)*2, IPC_CREAT | 0666);
int *shm_clock;
int *sec;
vector <PCB> table(MAX_PROCESSES);
vector <Frame> frame_table(MAX_FRAMES);
deque <int> frame_queue; // for FIFO frame replacement
deque <IOQueueEntry> IO_queue; // processes waiting for dirty bit writes
const int increment_amount = 1000000; // 100000 nanoseconds per loop iteration

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
            table[i].page_table.assign(TOTAL_PAGES, -1); // reset page table
            return i;
        }
    }
    return -1;
}

void print_process_table(const std::vector<PCB> &table) {
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

void print_page_tables(const std::vector<PCB> &table) {
    std::ostringstream ss;
    using std::endl;
    // Header
    ss << std::left << std::setw(6)  << "Index"
       << std::setw(6)  << "Occ"
       << std::setw(10) << "PID";
    for (int i = 0; i < 16; ++i) {
        ss << std::setw(6) << (std::string("P") + std::to_string(i));
    }
    ss << endl;

    // Divider
    ss << std::string(6 + 6 + 10 + 16 * 6, '-') << endl;

    // Rows
    for (size_t i = 0; i < table.size(); ++i) {
        const PCB &p = table[i];
        ss << std::left << std::setw(6) << i
           << std::setw(6) << (p.occupied ? 1 : 0);

        if (p.occupied) {
            ss << std::setw(10) << p.pid;
            // Print 16 page entries (use '-' for -1 / not present)
            for (int j = 0; j < 16; ++j) {
                int val = (j < (int)p.page_table.size()) ? p.page_table[j] : -1;
                if (val == -1) ss << std::setw(6) << "-";
                else ss << std::setw(6) << val;
            }
        } else {
            ss << std::setw(10) << "-";
            for (int j = 0; j < 16; ++j) ss << std::setw(6) << "-";
        }
        ss << endl;
    }

    ss << endl;
    oss_log_msg(ss.str());
}

void print_frame_table(const std::vector<Frame> &frames) {
    std::ostringstream ss;
    using std::endl;
    ss << std::left
       << std::setw(6)  << "Index"
       << std::setw(6)  << "Occ"
       << std::setw(12) << "PID"
       << std::setw(8)  << "Page"
       << std::setw(8)  << "Dirty" << endl;
    ss << std::string(6 + 6 + 12 + 8 + 8, '-') << endl;

    for (size_t i = 0; i < frames.size(); ++i) {
        const Frame &f = frames[i];
        ss << std::left << std::setw(6) << i
           << std::setw(6) << (f.occupied ? 1 : 0);
        if (f.occupied) {
            ss << std::setw(12) << f.pid
               << std::setw(8)  << f.page_number
               << std::setw(8)  << (f.dirty ? "Y" : "N");
        } else {
            ss << std::setw(12) << "-" 
               << std::setw(8)  << "-" 
               << std::setw(8)  << "-";
        }
        ss << endl;
    }

    ss << endl;
    oss_log_msg(ss.str());
}

int get_page_number(int address) {
    return address / PAGE_SIZE;
}

void load_frame(const MessageBuffer &msg, int frame_index, int page_number) {
    int pcb_index = find_pcb_by_pid(msg.pid);
    if (pcb_index == -1) return; // invalid PID

    frame_table[frame_index].occupied = true;
    frame_table[frame_index].pid = msg.pid;
    frame_table[frame_index].page_number = page_number;
    frame_table[frame_index].dirty = msg.write;

    table[pcb_index].page_table[page_number] = frame_index;
    frame_queue.push_back(frame_index); // add frame to FIFO queue
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

// Return index of a free (not occupied) frame in frame_table, or -1 if none found.
int find_free_frame(const std::vector<Frame> &frames) {
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i].occupied) return static_cast<int>(i);
    }
    return -1;
}

int main(int argc, char* argv[]) {
    //parse command line args
    int proc = -1;
    int simul = -1;
    float time_limit = -1;
    float launch_interval = -1;
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
        oss_log_msg(ss.str());
    }

    int launched_processes = 0;
    int running_processes = 0;
    int total_requests = 0;
    int total_page_faults = 0;
    int total_writes = 0;
    int total_reads = 0;

    long long launch_interval_nano = (long long)(launch_interval * 1e9); // convert launch interval to nanoseconds
    long long next_launch_total = 0; 

    MessageBuffer rcvMessage;
    MessageBuffer ackMessage;

    while (launched_processes < proc || running_processes > 0) {
        // increment clock
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
            print_process_table(table);
        }

        // process IO queue for blocked processes
        while (!IO_queue.empty()) {
            IOQueueEntry &entry = IO_queue.front();
            long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
            if (current_total >= entry.unblock_time) {
                // unblock process and load page into victim frame
                int pcb_index = find_pcb_by_pid(entry.pid);
                if (pcb_index == -1) {
                    cerr << "OSS: IO queue entry for unknown PID " << entry.pid << endl;
                    exit_handler();
                }
                // update page table for victim process
                pid_t victim_pid = frame_table[entry.victim_frame_index].pid;
                int victim_page = frame_table[entry.victim_frame_index].page_number;
                int victim_pcb_index = find_pcb_by_pid(victim_pid);
                if (victim_pcb_index != -1) {
                    table[victim_pcb_index].page_table[victim_page] = -1; // mark page as not present
                }
                
                int page_number = get_page_number(entry.memory_location);
                frame_table[entry.victim_frame_index].occupied = false;
                frame_table[entry.victim_frame_index].pid = -1;
                frame_table[entry.victim_frame_index].page_number = -1;
                frame_table[entry.victim_frame_index].dirty = false;
                load_frame(MessageBuffer{0, entry.pid, 1, entry.memory_location, entry.write}, entry.victim_frame_index, page_number);
                {
                    ostringstream ss;
                    ss << "OSS: Unblocked Worker " << entry.pid << ". Loaded page "
                       << page_number << " into frame " << entry.victim_frame_index << endl;
                    oss_log_msg(ss.str());
                }
                // send message to worker acknowledging page loaded into frame
                memset(&ackMessage, 0, sizeof(ackMessage));
                ackMessage.mtype = entry.pid;
                ackMessage.process_running = 1;
                size_t ack_size = sizeof(MessageBuffer) - sizeof(long);
                if (msgsnd(msgid, &ackMessage, ack_size, 0) == -1) {
                    perror("oss msgsnd ack failed");
                    exit_handler();
                }
                increment_clock(sec, nano, 100); // increment clock by 100 ns for page load
                IO_queue.pop_front(); // remove from IO queue
            } else {
                break; // front of queue not ready yet
            }
        }

        // check if all running process are blocked
        if (running_processes > 0 && running_processes == (int)IO_queue.size()) {
            IOQueueEntry &entry = IO_queue.front();
            long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
            // fast forward clock to when the next process unblocks
            if (current_total < entry.unblock_time) {
                long long diff = entry.unblock_time - current_total;
                increment_clock(sec, nano, diff);
                {
                    ostringstream ss;
                    ss << "OSS: All processes blocked. Fast forwarding clock by " << diff << " ns." << endl;
                    oss_log_msg(ss.str());
                }
            }
            continue; // skip message processing this loop
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
                // remove the processes pages from frame table
                for (int page = 0; page < TOTAL_PAGES; ++page) {
                    int frame_index = table[pcb_index].page_table[page];
                    if (frame_index != -1) {
                        frame_table[frame_index].occupied = false;
                        frame_table[frame_index].pid = -1;
                        frame_table[frame_index].page_number = -1;
                        frame_table[frame_index].dirty = false;
                        // also remove from frame queue
                        auto it = find(frame_queue.begin(), frame_queue.end(), frame_index);
                        if (it != frame_queue.end()) {
                            frame_queue.erase(it);
                        }
                    }
                }
                if (pcb_index != -1) {
                    // clean PCB entry
                    remove_pcb(table, rcvMessage.pid);
                }
                running_processes--;
                continue;
            }
            // process memory request from worker
            {
                ostringstream ss;
                ss << "OSS: Received memory request from Worker " << rcvMessage.pid
                   << " for location " << rcvMessage.memory_location
                   << (rcvMessage.write ? " (write)" : " (read)") << endl;
                oss_log_msg(ss.str());
            }
            total_requests++;
            if (rcvMessage.write) total_writes++;
            else total_reads++;

            int page_number = get_page_number(rcvMessage.memory_location); // page number requested
            int pcb_index = find_pcb_by_pid(rcvMessage.pid);
            if (pcb_index == -1) {
                cerr << "OSS: Received message from unknown PID " << rcvMessage.pid << endl;
                exit_handler();
            }
            // check if page is in page table
            if (table[pcb_index].page_table[page_number] == -1) { // page not in page table
                int free_frame_index = find_free_frame(frame_table);
                if (free_frame_index == -1) {
                    // no free frame found block for page replacement
                    total_page_faults++;
                    int frame_to_replace = frame_queue.front(); // get frame to replace using FIFO
                    frame_queue.pop_front();
                    IOQueueEntry entry;
                    entry.pid = rcvMessage.pid;
                    entry.memory_location = rcvMessage.memory_location;
                    entry.write = rcvMessage.write;
                    entry.victim_frame_index = frame_to_replace;
                    if(frame_table[frame_to_replace].dirty) {
                        // dirty frame block for extra time
                        entry.unblock_time = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano) + 20000000LL; // unblock after 20 ms
                        IO_queue.push_back(entry);
                        {
                            ostringstream ss;
                            ss << "OSS: No free frame for Worker " << rcvMessage.pid << ". Frame "
                               << frame_to_replace << " is dirty. Blocking for write." << endl;
                            oss_log_msg(ss.str());
                        }
                    } else {
                        // clean frame, block for 14 ms
                        entry.unblock_time = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano) + 14000000LL; // unblock after 14 ms
                        IO_queue.push_back(entry);
                        {
                            ostringstream ss;
                            ss << "OSS: No free frame for Worker " << rcvMessage.pid << ". Frame "
                               << frame_to_replace << " is clean. Blocking for page load." << endl;
                            oss_log_msg(ss.str());
                        }
                    }
                } else {
                    // free frame found, load page into frame
                    load_frame(rcvMessage, free_frame_index, page_number);
                    {
                        ostringstream ss;
                        ss << "OSS: Worker " << rcvMessage.pid << " Page " << page_number
                           << ". Loaded into free frame " << free_frame_index << endl;
                        oss_log_msg(ss.str());
                    }
                    // send message to worker acknowledging page loaded into frame
                    memset(&ackMessage, 0, sizeof(ackMessage));
                    ackMessage.mtype = rcvMessage.pid;
                    ackMessage.process_running = 1;
                    size_t ack_size = sizeof(MessageBuffer) - sizeof(long);
                    if (msgsnd(msgid, &ackMessage, ack_size, 0) == -1) {
                        perror("oss msgsnd ack failed");
                        exit_handler();
                    }
                    increment_clock(sec, nano, 100); // increment clock by 100 ns for page load
                }
            } else {
                // page hit
                {
                    ostringstream ss;
                    ss << "OSS: Page hit for Worker " << rcvMessage.pid
                       << " on page " << page_number << endl;
                    oss_log_msg(ss.str());
                }
                int frame_index = table[pcb_index].page_table[page_number];
                // mark frame as dirty if write request
                if (rcvMessage.write) {
                    frame_table[frame_index].dirty = true;
                }
                increment_clock(sec, nano, 100); // increment clock by 100 ns for page hit
                
                // send message to worker acknowledging release on page hit
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

        // call tables every half-second of simulated time
        {
            long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
            if (current_total >= next_print_total) {
                print_process_table(table);
                print_page_tables(table);
                print_frame_table(frame_table);
                next_print_total += PRINT_INTERVAL_NANO;
            }
        }
    }

    // final stats
    {
        ostringstream ss;
        ss << "OSS: All worker processes have terminated." << endl
           << "Total memory requests: " << total_requests << endl
           << "Total page faults: " << total_page_faults << endl
           << "Total read requests: " << total_reads << endl
           << "Total write requests: " << total_writes << endl
           << "Percentage of page faults: ";
        if (total_requests > 0) {
            double fault_percentage = (double)total_page_faults / (double)total_requests * 100.0;
            ss << fixed << setprecision(2) << fault_percentage << "%" << endl;
        } else {
            ss << "N/A (no requests)" << endl;
        }
        oss_log_msg(ss.str());
    }

    // cleanup
     shmdt(shm_clock);
     shmctl(shmid, IPC_RMID, nullptr);
     msgctl(msgid, IPC_RMID, nullptr);
     return 0;
 }