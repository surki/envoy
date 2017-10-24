#include "common/upstream/upstream_impl.h"

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/network/dns.h"
#include "envoy/ssl/context.h"
#include "envoy/upstream/health_checker.h"

#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/config/protocol_json.h"
#include "common/config/tls_context_json.h"
#include "common/http/utility.h"
#include "common/network/address_impl.h"
#include "common/network/utility.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"
#include "common/ssl/connection_impl.h"
#include "common/ssl/context_config_impl.h"
#include "common/upstream/eds.h"
#include "common/upstream/health_checker_impl.h"
#include "common/upstream/logical_dns_cluster.h"
#include "common/upstream/original_dst_cluster.h"

#include "fmt/format.h"

namespace Envoy {
namespace Upstream {
namespace {

const Network::Address::InstanceConstSharedPtr
getSourceAddress(const envoy::api::v2::Cluster& cluster,
                 const Network::Address::InstanceConstSharedPtr source_address) {
  // The source address from cluster config takes precedence.
  if (cluster.upstream_bind_config().has_source_address()) {
    return Network::Utility::fromProtoSocketAddress(
        cluster.upstream_bind_config().source_address());
  }
  // If there's no source address in the cluster config, use any default from the bootstrap proto.
  return source_address;
}
} // namespace

Host::CreateConnectionData HostImpl::createConnection(Event::Dispatcher& dispatcher) const {
  return {createConnection(dispatcher, *cluster_, address_), shared_from_this()};
}

Network::ClientConnectionPtr
HostImpl::createConnection(Event::Dispatcher& dispatcher, const ClusterInfo& cluster,
                           Network::Address::InstanceConstSharedPtr address) {
  Network::ClientConnectionPtr connection =
      cluster.sslContext() ? dispatcher.createSslClientConnection(*cluster.sslContext(), address,
                                                                  cluster.sourceAddress())
                           : dispatcher.createClientConnection(address, cluster.sourceAddress());
  connection->setBufferLimits(cluster.perConnectionBufferLimitBytes());
  return connection;
}

void HostImpl::weight(uint32_t new_weight) { weight_ = std::max(1U, std::min(100U, new_weight)); }

ClusterStats ClusterInfoImpl::generateStats(Stats::Scope& scope) {
  return {ALL_CLUSTER_STATS(POOL_COUNTER(scope), POOL_GAUGE(scope), POOL_TIMER(scope))};
}

ClusterInfoImpl::ClusterInfoImpl(const envoy::api::v2::Cluster& config,
                                 const Network::Address::InstanceConstSharedPtr source_address,
                                 Runtime::Loader& runtime, Stats::Store& stats,
                                 Ssl::ContextManager& ssl_context_manager, bool added_via_api)
    : runtime_(runtime), name_(config.name()),
      max_requests_per_connection_(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, max_requests_per_connection, 0)),
      connect_timeout_(
          std::chrono::milliseconds(PROTOBUF_GET_MS_REQUIRED(config, connect_timeout))),
      per_connection_buffer_limit_bytes_(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, per_connection_buffer_limit_bytes, 1024 * 1024)),
      stats_scope_(stats.createScope(fmt::format("cluster.{}.", name_))),
      stats_(generateStats(*stats_scope_)), features_(parseFeatures(config)),
      http2_settings_(Http::Utility::parseHttp2Settings(config.http2_protocol_options())),
      resource_managers_(config, runtime, name_),
      maintenance_mode_runtime_key_(fmt::format("upstream.maintenance_mode.{}", name_)),
      source_address_(getSourceAddress(config, source_address)), added_via_api_(added_via_api) {
  ssl_ctx_ = nullptr;
  if (config.has_tls_context()) {
    Ssl::ClientContextConfigImpl context_config(config.tls_context());
    ssl_ctx_ = ssl_context_manager.createSslClientContext(*stats_scope_, context_config);
  }

  switch (config.lb_policy()) {
  case envoy::api::v2::Cluster::ROUND_ROBIN:
    lb_type_ = LoadBalancerType::RoundRobin;
    break;
  case envoy::api::v2::Cluster::LEAST_REQUEST:
    lb_type_ = LoadBalancerType::LeastRequest;
    break;
  case envoy::api::v2::Cluster::RANDOM:
    lb_type_ = LoadBalancerType::Random;
    break;
  case envoy::api::v2::Cluster::RING_HASH:
    lb_type_ = LoadBalancerType::RingHash;
    break;
  case envoy::api::v2::Cluster::ORIGINAL_DST_LB:
    if (config.type() != envoy::api::v2::Cluster::ORIGINAL_DST) {
      throw EnvoyException(fmt::format(
          "cluster: LB type 'original_dst_lb' may only be used with cluser type 'original_dst'"));
    }
    lb_type_ = LoadBalancerType::OriginalDst;
    break;
  case envoy::api::v2::Cluster::STANDBY:
    lb_type_ = LoadBalancerType::StandBy;
    break;
  default:
    NOT_REACHED;
  }
}

const HostListsConstSharedPtr ClusterImplBase::empty_host_lists_{
    new std::vector<std::vector<HostSharedPtr>>()};

ClusterSharedPtr ClusterImplBase::create(const envoy::api::v2::Cluster& cluster, ClusterManager& cm,
                                         Stats::Store& stats, ThreadLocal::Instance& tls,
                                         Network::DnsResolverSharedPtr dns_resolver,
                                         Ssl::ContextManager& ssl_context_manager,
                                         Runtime::Loader& runtime, Runtime::RandomGenerator& random,
                                         Event::Dispatcher& dispatcher,
                                         const LocalInfo::LocalInfo& local_info,
                                         Outlier::EventLoggerSharedPtr outlier_event_logger,
                                         bool added_via_api) {
  std::unique_ptr<ClusterImplBase> new_cluster;

  // We make this a shared pointer to deal with the distinct ownership
  // scenarios that can exist: in one case, we pass in the "default"
  // DNS resolver that is owned by the Server::Instance. In the case
  // where 'dns_resolvers' is specified, we have per-cluster DNS
  // resolvers that are created here but ownership resides with
  // StrictDnsClusterImpl/LogicalDnsCluster.
  auto selected_dns_resolver = dns_resolver;
  if (!cluster.dns_resolvers().empty()) {
    const auto& resolver_addrs = cluster.dns_resolvers();
    std::vector<Network::Address::InstanceConstSharedPtr> resolvers;
    resolvers.reserve(resolver_addrs.size());
    for (const auto& resolver_addr : resolver_addrs) {
      resolvers.push_back(Network::Utility::fromProtoAddress(resolver_addr));
    }
    selected_dns_resolver = dispatcher.createDnsResolver(resolvers);
  }

  switch (cluster.type()) {
  case envoy::api::v2::Cluster::STATIC:
    new_cluster.reset(
        new StaticClusterImpl(cluster, runtime, stats, ssl_context_manager, cm, added_via_api));
    break;
  case envoy::api::v2::Cluster::STRICT_DNS:
    new_cluster.reset(new StrictDnsClusterImpl(cluster, runtime, stats, ssl_context_manager,
                                               selected_dns_resolver, cm, dispatcher,
                                               added_via_api));
    break;
  case envoy::api::v2::Cluster::LOGICAL_DNS:
    new_cluster.reset(new LogicalDnsCluster(cluster, runtime, stats, ssl_context_manager,
                                            selected_dns_resolver, tls, cm, dispatcher,
                                            added_via_api));
    break;
  case envoy::api::v2::Cluster::ORIGINAL_DST:
    if (cluster.lb_policy() != envoy::api::v2::Cluster::ORIGINAL_DST_LB) {
      throw EnvoyException(fmt::format(
          "cluster: cluster type 'original_dst' may only be used with LB type 'original_dst_lb'"));
    }
    new_cluster.reset(new OriginalDstCluster(cluster, runtime, stats, ssl_context_manager, cm,
                                             dispatcher, added_via_api));
    break;
  case envoy::api::v2::Cluster::EDS:
    if (!cluster.has_eds_cluster_config()) {
      throw EnvoyException("cannot create an sds cluster without an sds config");
    }

    // We map SDS to EDS, since EDS provides backwards compatibility with SDS.
    new_cluster.reset(new EdsClusterImpl(cluster, runtime, stats, ssl_context_manager, local_info,
                                         cm, dispatcher, random, added_via_api));
    break;
  default:
    NOT_REACHED;
  }

  if (!cluster.health_checks().empty()) {
    // TODO(htuch): Need to support multiple health checks in v2.
    ASSERT(cluster.health_checks().size() == 1);
    new_cluster->setHealthChecker(HealthCheckerFactory::create(
        cluster.health_checks()[0], *new_cluster, runtime, random, dispatcher));
  }

  new_cluster->setOutlierDetector(Outlier::DetectorImplFactory::createForCluster(
      *new_cluster, cluster, dispatcher, runtime, outlier_event_logger));
  return std::move(new_cluster);
}

ClusterImplBase::ClusterImplBase(const envoy::api::v2::Cluster& cluster,
                                 const Network::Address::InstanceConstSharedPtr source_address,
                                 Runtime::Loader& runtime, Stats::Store& stats,
                                 Ssl::ContextManager& ssl_context_manager, bool added_via_api)
    : runtime_(runtime), info_(new ClusterInfoImpl(cluster, source_address, runtime, stats,
                                                   ssl_context_manager, added_via_api)) {}

HostVectorConstSharedPtr
ClusterImplBase::createHealthyHostList(const std::vector<HostSharedPtr>& hosts) {
  HostVectorSharedPtr healthy_list(new std::vector<HostSharedPtr>());
  for (const auto& host : hosts) {
    if (host->healthy()) {
      healthy_list->emplace_back(host);
    }
  }

  return healthy_list;
}

HostListsConstSharedPtr
ClusterImplBase::createHealthyHostLists(const std::vector<std::vector<HostSharedPtr>>& hosts) {
  HostListsSharedPtr healthy_list(new std::vector<std::vector<HostSharedPtr>>());

  for (const auto& hosts_zone : hosts) {
    std::vector<HostSharedPtr> current_zone_hosts;
    for (const auto& host : hosts_zone) {
      if (host->healthy()) {
        current_zone_hosts.emplace_back(host);
      }
    }
    healthy_list->push_back(std::move(current_zone_hosts));
  }

  return healthy_list;
}

bool ClusterInfoImpl::maintenanceMode() const {
  return runtime_.snapshot().featureEnabled(maintenance_mode_runtime_key_, 0);
}

uint64_t ClusterInfoImpl::parseFeatures(const envoy::api::v2::Cluster& config) {
  uint64_t features = 0;
  if (config.has_http2_protocol_options()) {
    features |= Features::HTTP2;
  }
  return features;
}

ResourceManager& ClusterInfoImpl::resourceManager(ResourcePriority priority) const {
  ASSERT(enumToInt(priority) < resource_managers_.managers_.size());
  return *resource_managers_.managers_[enumToInt(priority)];
}

void ClusterImplBase::runUpdateCallbacks(const std::vector<HostSharedPtr>& hosts_added,
                                         const std::vector<HostSharedPtr>& hosts_removed) {
  if (!hosts_added.empty() || !hosts_removed.empty()) {
    info_->stats().membership_change_.inc();
  }

  info_->stats().membership_healthy_.set(healthyHosts().size());
  info_->stats().membership_total_.set(hosts().size());
  HostSetImpl::runUpdateCallbacks(hosts_added, hosts_removed);
}

void ClusterImplBase::setHealthChecker(const HealthCheckerSharedPtr& health_checker) {
  ASSERT(!health_checker_);
  health_checker_ = health_checker;
  health_checker_->start();
  health_checker_->addHostCheckCompleteCb([this](HostSharedPtr, bool changed_state) -> void {
    // If we get a health check completion that resulted in a state change, signal to
    // update the host sets on all threads.
    if (changed_state) {
      reloadHealthyHosts();
    }
  });
}

void ClusterImplBase::setOutlierDetector(const Outlier::DetectorSharedPtr& outlier_detector) {
  if (!outlier_detector) {
    return;
  }

  outlier_detector_ = outlier_detector;
  outlier_detector_->addChangedStateCb([this](HostSharedPtr) -> void { reloadHealthyHosts(); });
}

void ClusterImplBase::reloadHealthyHosts() {
  HostVectorConstSharedPtr hosts_copy(new std::vector<HostSharedPtr>(hosts()));
  HostListsConstSharedPtr hosts_per_zone_copy(
      new std::vector<std::vector<HostSharedPtr>>(hostsPerZone()));
  updateHosts(hosts_copy, createHealthyHostList(hosts()), hosts_per_zone_copy,
              createHealthyHostLists(hostsPerZone()), {}, {});
}

ClusterInfoImpl::ResourceManagers::ResourceManagers(const envoy::api::v2::Cluster& config,
                                                    Runtime::Loader& runtime,
                                                    const std::string& cluster_name) {
  managers_[enumToInt(ResourcePriority::Default)] =
      load(config, runtime, cluster_name, envoy::api::v2::RoutingPriority::DEFAULT);
  managers_[enumToInt(ResourcePriority::High)] =
      load(config, runtime, cluster_name, envoy::api::v2::RoutingPriority::HIGH);
}

ResourceManagerImplPtr
ClusterInfoImpl::ResourceManagers::load(const envoy::api::v2::Cluster& config,
                                        Runtime::Loader& runtime, const std::string& cluster_name,
                                        const envoy::api::v2::RoutingPriority& priority) {
  uint64_t max_connections = 1024;
  uint64_t max_pending_requests = 1024;
  uint64_t max_requests = 1024;
  uint64_t max_retries = 3;
  const std::string runtime_prefix = fmt::format("circuit_breakers.{}.{}.", cluster_name, priority);

  const auto& thresholds = config.circuit_breakers().thresholds();
  const auto it =
      std::find_if(thresholds.cbegin(), thresholds.cend(),
                   [priority](const envoy::api::v2::CircuitBreakers::Thresholds& threshold) {
                     return threshold.priority() == priority;
                   });
  if (it != thresholds.cend()) {
    max_connections = PROTOBUF_GET_WRAPPED_OR_DEFAULT(*it, max_connections, max_connections);
    max_pending_requests =
        PROTOBUF_GET_WRAPPED_OR_DEFAULT(*it, max_pending_requests, max_pending_requests);
    max_requests = PROTOBUF_GET_WRAPPED_OR_DEFAULT(*it, max_requests, max_requests);
    max_retries = PROTOBUF_GET_WRAPPED_OR_DEFAULT(*it, max_retries, max_retries);
  }
  return ResourceManagerImplPtr{new ResourceManagerImpl(
      runtime, runtime_prefix, max_connections, max_pending_requests, max_requests, max_retries)};
}

StaticClusterImpl::StaticClusterImpl(const envoy::api::v2::Cluster& cluster,
                                     Runtime::Loader& runtime, Stats::Store& stats,
                                     Ssl::ContextManager& ssl_context_manager, ClusterManager& cm,
                                     bool added_via_api)
    : ClusterImplBase(cluster, cm.sourceAddress(), runtime, stats, ssl_context_manager,
                      added_via_api) {
  HostVectorSharedPtr new_hosts(new std::vector<HostSharedPtr>());
  for (const auto& host : cluster.hosts()) {
    new_hosts->emplace_back(
        HostSharedPtr{new HostImpl(info_, "", Network::Utility::fromProtoAddress(host),
                                   envoy::api::v2::Metadata::default_instance(), 1, "")});
  }

  updateHosts(new_hosts, createHealthyHostList(*new_hosts), empty_host_lists_, empty_host_lists_,
              {}, {});
}

bool BaseDynamicClusterImpl::updateDynamicHostList(const std::vector<HostSharedPtr>& new_hosts,
                                                   std::vector<HostSharedPtr>& current_hosts,
                                                   std::vector<HostSharedPtr>& hosts_added,
                                                   std::vector<HostSharedPtr>& hosts_removed,
                                                   bool depend_on_hc) {
  uint64_t max_host_weight = 1;

  // Go through and see if the list we have is different from what we just got. If it is, we
  // make a new host list and raise a change notification. This uses an N^2 search given that
  // this does not happen very often and the list sizes should be small. We also check for
  // duplicates here. It's possible for DNS to return the same address multiple times, and a bad
  // SDS implementation could do the same thing.
  std::unordered_set<std::string> host_addresses;
  std::vector<HostSharedPtr> final_hosts;
  for (const HostSharedPtr& host : new_hosts) {
    if (host_addresses.count(host->address()->asString())) {
      continue;
    }
    host_addresses.emplace(host->address()->asString());

    bool found = false;
    for (auto i = current_hosts.begin(); i != current_hosts.end();) {
      // If we find a host matched based on address, we keep it. However we do change weight inline
      // so do that here.
      if (*(*i)->address() == *host->address()) {
        if (host->weight() > max_host_weight) {
          max_host_weight = host->weight();
        }

        (*i)->weight(host->weight());
        final_hosts.push_back(*i);
        i = current_hosts.erase(i);
        found = true;
      } else {
        i++;
      }
    }

    if (!found) {
      if (host->weight() > max_host_weight) {
        max_host_weight = host->weight();
      }

      final_hosts.push_back(host);
      hosts_added.push_back(host);

      // If we are depending on a health checker, we initialize to unhealthy.
      if (depend_on_hc) {
        hosts_added.back()->healthFlagSet(Host::HealthFlag::FAILED_ACTIVE_HC);
      }
    }
  }

  // If there are removed hosts, check to see if we should only delete if unhealthy.
  if (!current_hosts.empty() && depend_on_hc) {
    for (auto i = current_hosts.begin(); i != current_hosts.end();) {
      if (!(*i)->healthFlagGet(Host::HealthFlag::FAILED_ACTIVE_HC)) {
        if ((*i)->weight() > max_host_weight) {
          max_host_weight = (*i)->weight();
        }

        final_hosts.push_back(*i);
        i = current_hosts.erase(i);
      } else {
        i++;
      }
    }
  }

  info_->stats().max_host_weight_.set(max_host_weight);

  if (!hosts_added.empty() || !current_hosts.empty()) {
    hosts_removed = std::move(current_hosts);
    current_hosts = std::move(final_hosts);
    return true;
  } else {
    // During the search we moved all of the hosts from hosts_ into final_hosts so just
    // move them back.
    current_hosts = std::move(final_hosts);
    return false;
  }
}

StrictDnsClusterImpl::StrictDnsClusterImpl(const envoy::api::v2::Cluster& cluster,
                                           Runtime::Loader& runtime, Stats::Store& stats,
                                           Ssl::ContextManager& ssl_context_manager,
                                           Network::DnsResolverSharedPtr dns_resolver,
                                           ClusterManager& cm, Event::Dispatcher& dispatcher,
                                           bool added_via_api)
    : BaseDynamicClusterImpl(cluster, cm.sourceAddress(), runtime, stats, ssl_context_manager,
                             added_via_api),
      dns_resolver_(dns_resolver),
      dns_refresh_rate_ms_(
          std::chrono::milliseconds(PROTOBUF_GET_MS_OR_DEFAULT(cluster, dns_refresh_rate, 5000))) {
  switch (cluster.dns_lookup_family()) {
  case envoy::api::v2::Cluster::V6_ONLY:
    dns_lookup_family_ = Network::DnsLookupFamily::V6Only;
    break;
  case envoy::api::v2::Cluster::V4_ONLY:
    dns_lookup_family_ = Network::DnsLookupFamily::V4Only;
    break;
  case envoy::api::v2::Cluster::AUTO:
    dns_lookup_family_ = Network::DnsLookupFamily::Auto;
    break;
  default:
    NOT_REACHED;
  }

  for (const auto& host : cluster.hosts()) {
    resolve_targets_.emplace_back(
        new ResolveTarget(*this, dispatcher,
                          fmt::format("tcp://{}:{}", host.socket_address().address(),
                                      host.socket_address().port_value())));
  }

  // We have to first construct resolve_targets_ before invoking startResolve(),
  // since startResolve() might resolve immediately and relies on
  // resolve_targets_ indirectly for performing host updates on resolution.
  for (const ResolveTargetPtr& target : resolve_targets_) {
    target->startResolve();
  }
}

void StrictDnsClusterImpl::updateAllHosts(const std::vector<HostSharedPtr>& hosts_added,
                                          const std::vector<HostSharedPtr>& hosts_removed) {
  // At this point we know that we are different so make a new host list and notify.
  HostVectorSharedPtr new_hosts(new std::vector<HostSharedPtr>());
  for (const ResolveTargetPtr& target : resolve_targets_) {
    for (const HostSharedPtr& host : target->hosts_) {
      new_hosts->emplace_back(host);
    }
  }

  updateHosts(new_hosts, createHealthyHostList(*new_hosts), empty_host_lists_, empty_host_lists_,
              hosts_added, hosts_removed);
}

StrictDnsClusterImpl::ResolveTarget::ResolveTarget(StrictDnsClusterImpl& parent,
                                                   Event::Dispatcher& dispatcher,
                                                   const std::string& url)
    : parent_(parent), dns_address_(Network::Utility::hostFromTcpUrl(url)),
      port_(Network::Utility::portFromTcpUrl(url)),
      resolve_timer_(dispatcher.createTimer([this]() -> void { startResolve(); })) {}

StrictDnsClusterImpl::ResolveTarget::~ResolveTarget() {
  if (active_query_) {
    active_query_->cancel();
  }
}

void StrictDnsClusterImpl::ResolveTarget::startResolve() {
  ENVOY_LOG(debug, "starting async DNS resolution for {}", dns_address_);
  parent_.info_->stats().update_attempt_.inc();

  active_query_ = parent_.dns_resolver_->resolve(
      dns_address_, parent_.dns_lookup_family_,
      [this](std::list<Network::Address::InstanceConstSharedPtr>&& address_list) -> void {
        active_query_ = nullptr;
        ENVOY_LOG(debug, "async DNS resolution complete for {}", dns_address_);
        parent_.info_->stats().update_success_.inc();

        std::vector<HostSharedPtr> new_hosts;
        for (const Network::Address::InstanceConstSharedPtr& address : address_list) {
          // TODO(mattklein123): Currently the DNS interface does not consider port. We need to make
          // a new address that has port in it. We need to both support IPv6 as well as potentially
          // move port handling into the DNS interface itself, which would work better for SRV.
          ASSERT(address != nullptr);
          new_hosts.emplace_back(new HostImpl(parent_.info_, dns_address_,
                                              Network::Utility::getAddressWithPort(*address, port_),
                                              envoy::api::v2::Metadata::default_instance(), 1, ""));
        }

        std::vector<HostSharedPtr> hosts_added;
        std::vector<HostSharedPtr> hosts_removed;
        if (parent_.updateDynamicHostList(new_hosts, hosts_, hosts_added, hosts_removed, false)) {
          ENVOY_LOG(debug, "DNS hosts have changed for {}", dns_address_);
          parent_.updateAllHosts(hosts_added, hosts_removed);
        }

        // If there is an initialize callback, fire it now. Note that if the cluster refers to
        // multiple DNS names, this will return initialized after a single DNS resolution completes.
        // This is not perfect but is easier to code and unclear if the extra complexity is needed
        // so will start with this.
        if (parent_.initialize_callback_) {
          parent_.initialize_callback_();
          parent_.initialize_callback_ = nullptr;
        }
        parent_.initialized_ = true;

        resolve_timer_->enableTimer(parent_.dns_refresh_rate_ms_);
      });
}

} // namespace Upstream
} // namespace Envoy
