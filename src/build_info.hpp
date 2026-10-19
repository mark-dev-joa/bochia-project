#pragma once
// The build time and git hash of this program: build_stamp.hpp is generated into the build folder by cmake/build_info.cmake.
#include "build_stamp.hpp"
#include <string>

namespace boccia {
inline std::string versionString() { return std::string(BOCCIA_BUILD_TIME) + " · " + BOCCIA_GIT_HASH; }
}
