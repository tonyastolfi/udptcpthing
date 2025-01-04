#pragma once

#include <iostream>

inline void show_help()
{
    std::cerr  //
        << std::setw(16) << "HELP" << std::setw(12) << "(optional)" << " Show this message" << std::endl
        << std::setw(16) << "MODE" << std::setw(12) << "(required)" << " 'udp2tcp' or 'tcp2udp'" << std::endl
        << std::setw(16) << "UDP_PORT" << std::setw(12) << "(optional)" << " default=19132" << std::endl
        << std::setw(16) << "TCP_PORT" << std::setw(12) << "(optional)" << " default=7777" << std::endl
        << std::setw(16) << "MAX_CLIENTS" << std::setw(12) << "(optional)"
        << " Set client port mapping limit (default=16)" << std::endl;
}
