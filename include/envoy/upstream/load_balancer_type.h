#pragma once

namespace Envoy {
namespace Upstream {

/**
 * Type of load balancing to perform.
 */
enum class LoadBalancerType { RoundRobin, LeastRequest, Random, RingHash, OriginalDst, StandBy };

} // namespace Upstream
} // namespace Envoy
