#include "memory/memory.hpp"
#include "boot/boot.hpp"
#include "memory/address.hpp"

namespace kernel::memory {
void initialize() {
  DirectMap::initialize(boot::hhdm_request.response->offset);
}
} // namespace kernel::memory