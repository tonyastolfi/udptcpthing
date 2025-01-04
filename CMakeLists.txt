cmake_minimum_required(VERSION 3.15)
project(udptcpthing CXX)


find_package(batteries)
find_package(glog)

add_executable(udptcpthing src/udptcpthing.cpp src/main.cpp)

target_link_libraries(udptcpthing batteries::batteries glog::glog)

install(TARGETS udptcpthing DESTINATION "."
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        )
