#include "udptcpthing.h"
#include <vector>
#include <string>

int main() {
    udptcpthing();

    std::vector<std::string> vec;
    vec.push_back("test_package");

    udptcpthing_print_vector(vec);
}
