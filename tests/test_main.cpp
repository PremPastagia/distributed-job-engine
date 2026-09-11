// Custom entry point: the engine logs to stderr at INFO by default, which would bury the
// test output. Tests that care about log behaviour set the level themselves.
#include <gtest/gtest.h>

#include "jobengine/util.hpp"

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  je::Log::set_level(je::LogLevel::Error);
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--verbose-engine-logs") je::Log::set_level(je::LogLevel::Info);
  }
  return RUN_ALL_TESTS();
}
