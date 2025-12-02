#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string>
#include <cstdlib>
#include <errno.h>
#include "resources.h"
#include <random>
#include <algorithm>
#include <cstring> 

using namespace std;

struct MessageBuffer {
    long mtype;
    pid_t pid;
    int request_or_release; // 1 for request, 0 for release
    int resource_request[MAX_RESOURCES]; // array of resource requests
    int resource_release[MAX_RESOURCES]; // array of resource releases
    int mass_release; // 1 if mass release 0 if not
    int process_running; // 1 if running, 0 if not
};

random_device rd;
mt19937 gen(rd());

int get_resource_request(int* held_resources) {
    uniform_int_distribution<> dis(0, MAX_RESOURCES - 1);
    int resource_index = dis(gen);
    if (held_resources[resource_index] >= MAX_INSTANCES) {
        // already holding max instances of this resource, try again
        return get_resource_request(held_resources);
    }
    return resource_index;
}

int get_resource_release(int* held_resources) {
    uniform_int_distribution<> dis(0, MAX_RESOURCES - 1);
    int resource_index = dis(gen);
    if (held_resources[resource_index] == 0) {
        // not holding any instances of this resource, try again
        return get_resource_release(held_resources);
    }
    return resource_index;
}


int main(int argc, char* argv[]) {
    key_t sh_key = ftok("oss.cpp", 0);

    // create/get shared memory
    int shmid = shmget(sh_key, sizeof(int)*2, 0666);
    if (shmid == -1) {
        cerr << "shmget";
        exit(1);
    }

    // attach shared memory to shm_ptr
    int* clock = (int*) shmat(shmid, nullptr, 0);
    if (clock == (int*) -1) {
        cerr << "shmat";
        exit(1);
    }

    int *sec = &(clock[0]);
    int *nano = &(clock[1]);
    
    // get target time from command line args
    int target_seconds = stoi(argv[1]);
    int target_nano = stoi(argv[2]);

    // setup message queue
    key_t msg_key = ftok("oss.cpp", 1);
    int msgid = msgget(msg_key, 0666);
    if (msgid == -1) {
        cerr << "msgget";
        exit(1);
    }

    // how many of each resource this process has
    int held_resources[MAX_RESOURCES] = {0};
    int latest_requested_resource_index = -1;

    // calculate termination time
    int end_seconds = *sec + target_seconds;
    int end_nano = *nano + target_nano;
    if (end_nano >= 1000000000) {
        end_seconds += end_nano / 1000000000;
        end_nano = end_nano % 1000000000;
    }
    
    // get random time interval for when to request/release resources
    uniform_int_distribution<> dis(0, 100000000); // between 0 and 100 milliseconds
    long long request_release_interval = dis(gen);
    long long next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval;

        // Print starting message
    cout << "Worker starting, " << "PID:" << getpid() << " PPID:" << getppid() << endl
         << "Called With:" << endl
         << "Interval: " << target_seconds << " seconds, " << target_nano << " nanoseconds" << endl
         << "Request/Release Interval: " << request_release_interval << " nanoseconds" << endl;

    // setup distribution for request/release action
    uniform_int_distribution<> action_dis(1, 100);

    // worker just staring message
    cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
         << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TermTimeS: " << end_seconds << " TermTimeNano: " << end_nano << endl
         << "--Just Starting" << endl;

    // message-driven loop: block until OSS tells us to check the clock
    MessageBuffer msg;
    pid_t oss_pid = getppid();

    while (true) {
        // check if its time to terminate 
        bool should_terminate = ((*sec > end_seconds) || (*sec == end_seconds && *nano >= end_nano));

        if (should_terminate) {
            // print terminating message
            // TODO: add more deailated info
            cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
                 << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TermTimeS: " << end_seconds << " TermTimeNano: " << end_nano << endl
                 << "--Terminating" << endl;
            // send message to OSS indicating termination
            memset(&msg, 0, sizeof(msg));
            msg.mtype = getppid();
            msg.pid = getpid();
            msg.process_running = 0; // indicate process is terminating
            size_t msg_size = sizeof(MessageBuffer) - sizeof(long);
            if (msgsnd(msgid, &msg, msg_size, 0) == -1) {
                perror("worker msgsnd failed");
                exit(1);
            }
            break; // exit loop and terminate
        }

        // check if its time to request/release resources
        long long current_total = (long long)(*sec) * 1000000000LL + (long long)(*nano);
        if (current_total >= next_request_release_total) {
            if (all_of(held_resources, held_resources + MAX_RESOURCES, [](int i){ return i >= MAX_INSTANCES; })) {
                // holding max of all resources skip request
                continue;
            }
            // decide whether to request or release a resource 60% request, 40% release
            int action_roll = action_dis(gen);
            if (action_roll <= 60) {
                // request resource
                int resource_index = get_resource_request(held_resources);
                // determine how much to request
                int max_amount = MAX_INSTANCES - held_resources[resource_index];
                uniform_int_distribution<> amount_dis(1, max_amount);
                int amount = amount_dis(gen);

                // out of order request
                if (resource_index <= latest_requested_resource_index) {
                    cout << "Worker PID:" << getpid() << " making out-of-order request for resource " << resource_index << endl;
                    int release_request[MAX_RESOURCES] = {0};
                    for (int i = resource_index; i < MAX_RESOURCES; i++) {
                        if (held_resources[i] > 0) {
                            release_request[i] = held_resources[i];
                            held_resources[i] = 0;
                            cout << "Worker PID:" << getpid() << " releasing " << release_request[i] << " instances of resource " << i << " to make out-of-order request" << endl;
                        }
                    }
                    // release higher-indexed resources
                    memset(&msg, 0, sizeof(msg));
                    msg.mtype = getppid();
                    msg.pid = getpid();
                    msg.process_running = 1; // indicate process is running
                    msg.request_or_release = 0; // indicate release
                    msg.mass_release = 1; // indicate mass release

                    for (int i = 0; i < MAX_RESOURCES; ++i) {
                        msg.resource_release[i] = release_request[i];
                    }
                    size_t msg_size = sizeof(MessageBuffer) - sizeof(long);
                    if (msgsnd(msgid, &msg, msg_size, 0) == -1) {
                        perror("worker msgsnd failed");
                        exit(1);
                    }
                    // wait for message from OSS acknowledging release
                    size_t rcv_size = sizeof(MessageBuffer) - sizeof(long);
                    if (msgrcv(msgid, &msg, rcv_size, getpid(), 0) == -1) {
                        perror("worker msgrcv failed");
                        exit(1);
                    }
                    // now request back the released resources plus the new request
                    memset(&msg, 0, sizeof(msg));
                    msg.mtype = getppid();
                    msg.pid = getpid();
                    msg.process_running = 1; // indicate process is running
                    msg.request_or_release = 1; // indicate request
                    for (int i = 0; i < MAX_RESOURCES; ++i) {
                        if (release_request[i] > 0) {
                            msg.resource_request[i] = release_request[i];
                        }
                    }
                    msg.resource_request[resource_index] += amount;

                    cout << "Worker PID:" << getpid() << " requesting back released resources plus " << amount << " instances of resource " << resource_index << " at SysClockS: " << *sec << " SysclockNano: " << *nano << endl;
                    if (msgsnd(msgid, &msg, msg_size, 0) == -1) {
                        perror("worker msgsnd failed");
                        exit(1);
                    }
                    // wait for message from OSS acknowledging request
                    if (msgrcv(msgid, &msg, rcv_size, getpid(), 0) == -1) {
                        perror("worker msgrcv failed");
                        exit(1);
                    }
                    // update resources
                    for (int i = 0; i < MAX_RESOURCES; ++i) {
                        held_resources[i] += release_request[i];
                    }
                    held_resources[resource_index] += amount;
                    // update latest requested resource index
                    latest_requested_resource_index = -1;
                    for (int i = MAX_RESOURCES - 1; i >= 0; --i) {
                        if (held_resources[i] > 0) {
                            latest_requested_resource_index = i;
                            break;
                        }
                    }
                    next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval; // schedule next request/release time
                    continue;
                }

                cout << "Worker PID:" << getpid() << " requesting " << amount << " instances of resource " << resource_index << " at SysClockS: " << *sec << " SysclockNano: " << *nano << endl;
                // send message to OSS requesting resource
                memset(&msg, 0, sizeof(msg));
                msg.mtype = getppid();
                msg.pid = getpid();
                msg.process_running = 1; // indicate process is running
                msg.request_or_release = 1; // indicate request
                msg.resource_request[resource_index] = amount;
                size_t msg_size = sizeof(MessageBuffer) - sizeof(long);
                if (msgsnd(msgid, &msg, msg_size, 0) == -1) {
                    perror("worker msgsnd failed");
                    exit(1);
                }
                // wait for message from OSS acknowledging request
                size_t rcv_size = sizeof(MessageBuffer) - sizeof(long);
                if (msgrcv(msgid, &msg, rcv_size, getpid(), 0) == -1) {
                    perror("worker msgrcv failed");
                    exit(1);
                }

                // update held resources
                latest_requested_resource_index = resource_index;
                held_resources[resource_index] += amount;
                next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval; // schedule next request/release time
            } else {
                if (latest_requested_resource_index == -1 || all_of(held_resources, held_resources + MAX_RESOURCES, [](int i){ return i == 0; })) {
                    // no resources held, skip release
                    next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval; // schedule next request/release time
                    continue;
                }
                // release resource
                // choose resource to release
                int resource_index = get_resource_release(held_resources);
                // determine how much to release
                int held_amount = held_resources[resource_index];
                uniform_int_distribution<> amount_dis(1, held_amount);
                int amount = amount_dis(gen);
                cout << "Worker PID:" << getpid() << " releasing " << amount << " instances of resource " << resource_index << " at SysClockS: " << *sec << " SysclockNano: " << *nano << endl;
                // send message to OSS releasing resource
                memset(&msg, 0, sizeof(msg));
                msg.mtype = getppid();
                msg.pid = getpid();
                msg.process_running = 1; // indicate process is running
                msg.request_or_release = 0; // indicate release
                msg.resource_release[resource_index] = amount;
                size_t msg_size = sizeof(MessageBuffer) - sizeof(long);
                if (msgsnd(msgid, &msg, msg_size, 0) == -1) cerr << "msgsnd" << endl;
                // wait for message from OSS acknowledging release
                size_t rcv_size = sizeof(MessageBuffer) - sizeof(long);
                if (msgrcv(msgid, &msg, rcv_size, getpid(), 0) == -1) {
                    cerr << "msgrcv" << endl;
                }

                // update held resources
                held_resources[resource_index] -= amount;
                if (resource_index == latest_requested_resource_index && held_resources[resource_index] == 0) {

                    // released all instances of latest requested resource, need to update latest_requested_resource_index
                    latest_requested_resource_index = -1;
                    for (int i = MAX_RESOURCES - 1; i >= 0; --i) {
                        if (held_resources[i] > 0) {
                            latest_requested_resource_index = i;
                            break;
                        }
                    }
                }
                next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval; // schedule next request/release time
            }
        }
    }
    shmdt(clock);
    return 0;
}