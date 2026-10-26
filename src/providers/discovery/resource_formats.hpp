#pragma once
#include "providers/classifier/signal_candidates.hpp"

namespace na::discovery {
// Inventory classification only; this does not establish a sample interpretation.
inline classifier::Format ResourceFormat(uint32_t f) {
    if(f==40 || f==45 || f==55 || f==20 || f==19 || f==44) return classifier::Format::Depth;
    if(f==39 || f==41 || f==53 || f==54 || f==56) return classifier::Format::ScalarFloat;
    if(f==2 || f==10 || f==26) return classifier::Format::ColorFloat;
    if(f==28 || f==29 || f==87 || f==91 || f==24) return classifier::Format::ColorUnorm;
    if(f==16 || f==34) return classifier::Format::TwoChannelFloat;
    return classifier::Format::Unknown;
}
} // namespace na::discovery
