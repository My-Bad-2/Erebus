#include "utils/logger.hpp"

extern "C" void abort() { kernel::utils::logger::fatal("Abort Called!\n"); }
