#include "hermes/tools/interrupt.hpp"

namespace hermes::tools {

InterruptFlag& InterruptFlag::global() {
    static InterruptFlag inst;
    return inst;
}

}  // namespace hermes::tools
