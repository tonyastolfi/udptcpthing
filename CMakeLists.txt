cmake_minimum_required(VERSION 3.15)
project(udptcpthing CXX)



add_executable(udptcpthing src/udptcpthing.cpp src/main.cpp)



install(TARGETS udptcpthing DESTINATION "."
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        )
