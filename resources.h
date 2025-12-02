#ifndef RESOURCES_H
#define RESOURCES_H

#include <array>

#define MAX_RESOURCES 10
#define MAX_INSTANCES 5
#define MAX_PROCESSES 18

struct resource_descriptor {
    std::array<int, MAX_RESOURCES> available_resources = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    std::array<std::array<int, MAX_RESOURCES>, MAX_PROCESSES> allocation_matrix = {};
};

#endif