#pragma once

#include <iostream>

bool goDebugging = false;

template <typename... ARGS>
void dlog(ARGS&&... args) {
    if (goDebugging) {
        ((std::cout << args), ...);
        std::cout << "\n";
    }
}

template <typename... ARGS>
void ilog(ARGS&&... args) {
    ((std::cout << args), ...);
    std::cout << "\n";
}

template <typename... ARGS>
void elog(ARGS&&... args) {
    ((std::cout << args), ...);
    std::cout << "\n";
}
