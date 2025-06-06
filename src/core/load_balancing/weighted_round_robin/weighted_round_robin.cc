//
// Copyright 2022 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/event_engine/event_engine.h>
#include <grpc/impl/connectivity_state.h>
#include <grpc/support/port_platform.h>
#include <inttypes.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/meta/type_traits.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "src/core/config/core_configuration.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/experiments/experiments.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/resolved_address.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/load_balancing/backend_metric_data.h"
#include "src/core/load_balancing/endpoint_list.h"
#include "src/core/load_balancing/lb_policy.h"
#include "src/core/load_balancing/lb_policy_factory.h"
#include "src/core/load_balancing/oob_backend_metric.h"
#include "src/core/load_balancing/subchannel_interface.h"
#include "src/core/load_balancing/weighted_round_robin/static_stride_scheduler.h"
#include "src/core/load_balancing/weighted_target/weighted_target.h"
#include "src/core/resolver/endpoint_addresses.h"
#include "src/core/telemetry/metrics.h"
#include "src/core/telemetry/stats.h"
#include "src/core/telemetry/stats_data.h"
#include "src/core/util/debug_location.h"
#include "src/core/util/json/json.h"
#include "src/core/util/json/json_args.h"
#include "src/core/util/json/json_object_loader.h"
#include "src/core/util/orphanable.h"
#include "src/core/util/ref_counted.h"
#include "src/core/util/ref_counted_ptr.h"
#include "src/core/util/shared_bit_gen.h"
#include "src/core/util/sync.h"
#include "src/core/util/time.h"
#include "src/core/util/validation_errors.h"
#include "src/core/util/work_serializer.h"

namespace grpc_core {

namespace {

constexpr absl::string_view kWeightedRoundRobin = "weighted_round_robin";

constexpr absl::string_view kMetricLabelLocality = "grpc.lb.locality";

const auto kMetricRrFallback =
    GlobalInstrumentsRegistry::RegisterUInt64Counter(
        "grpc.lb.wrr.rr_fallback",
        "EXPERIMENTAL.  Number of scheduler updates in which there were not "
        "enough endpoints with valid weight, which caused the WRR policy to "
        "fall back to RR behavior.",
        "{update}", false)
        .Labels(kMetricLabelTarget)
        .OptionalLabels(kMetricLabelLocality)
        .Build();

const auto kMetricEndpointWeightNotYetUsable =
    GlobalInstrumentsRegistry::RegisterUInt64Counter(
        "grpc.lb.wrr.endpoint_weight_not_yet_usable",
        "EXPERIMENTAL.  Number of endpoints from each scheduler update that "
        "don't yet have usable weight information (i.e., either the load "
        "report has not yet been received, or it is within the blackout "
        "period).",
        "{endpoint}", false)
        .Labels(kMetricLabelTarget)
        .OptionalLabels(kMetricLabelLocality)
        .Build();

const auto kMetricEndpointWeightStale =
    GlobalInstrumentsRegistry::RegisterUInt64Counter(
        "grpc.lb.wrr.endpoint_weight_stale",
        "EXPERIMENTAL.  Number of endpoints from each scheduler update whose "
        "latest weight is older than the expiration period.",
        "{endpoint}", false)
        .Labels(kMetricLabelTarget)
        .OptionalLabels(kMetricLabelLocality)
        .Build();

const auto kMetricEndpointWeights =
    GlobalInstrumentsRegistry::RegisterDoubleHistogram(
        "grpc.lb.wrr.endpoint_weights",
        "EXPERIMENTAL.  The histogram buckets will be endpoint weight ranges.  "
        "Each bucket will be a counter that is incremented once for every "
        "endpoint whose weight is within that range. Note that endpoints "
        "without usable weights will have weight 0.",
        "{weight}", false)
        .Labels(kMetricLabelTarget)
        .OptionalLabels(kMetricLabelLocality)
        .Build();

// Config for WRR policy.
class WeightedRoundRobinConfig final : public LoadBalancingPolicy::Config {
 public:
  WeightedRoundRobinConfig() = default;

  WeightedRoundRobinConfig(const WeightedRoundRobinConfig&) = delete;
  WeightedRoundRobinConfig& operator=(const WeightedRoundRobinConfig&) = delete;

  WeightedRoundRobinConfig(WeightedRoundRobinConfig&&) = delete;
  WeightedRoundRobinConfig& operator=(WeightedRoundRobinConfig&&) = delete;

  absl::string_view name() const override { return kWeightedRoundRobin; }

  bool enable_oob_load_report() const { return enable_oob_load_report_; }
  Duration oob_reporting_period() const { return oob_reporting_period_; }
  Duration blackout_period() const { return blackout_period_; }
  Duration weight_update_period() const { return weight_update_period_; }
  Duration weight_expiration_period() const {
    return weight_expiration_period_;
  }
  float error_utilization_penalty() const { return error_utilization_penalty_; }

  static const JsonLoaderInterface* JsonLoader(const JsonArgs&) {
    static const auto* loader =
        JsonObjectLoader<WeightedRoundRobinConfig>()
            .OptionalField("enableOobLoadReport",
                           &WeightedRoundRobinConfig::enable_oob_load_report_)
            .OptionalField("oobReportingPeriod",
                           &WeightedRoundRobinConfig::oob_reporting_period_)
            .OptionalField("blackoutPeriod",
                           &WeightedRoundRobinConfig::blackout_period_)
            .OptionalField("weightUpdatePeriod",
                           &WeightedRoundRobinConfig::weight_update_period_)
            .OptionalField("weightExpirationPeriod",
                           &WeightedRoundRobinConfig::weight_expiration_period_)
            .OptionalField(
                "errorUtilizationPenalty",
                &WeightedRoundRobinConfig::error_utilization_penalty_)
            .Finish();
    return loader;
  }

  void JsonPostLoad(const Json&, const JsonArgs&, ValidationErrors* errors) {
    // Impose lower bound of 100ms on weightUpdatePeriod.
    weight_update_period_ =
        std::max(weight_update_period_, Duration::Milliseconds(100));
    if (error_utilization_penalty_ < 0) {
      ValidationErrors::ScopedField field(errors, ".errorUtilizationPenalty");
      errors->AddError("must be non-negative");
    }
  }

 private:
  bool enable_oob_load_report_ = false;
  Duration oob_reporting_period_ = Duration::Seconds(10);
  Duration blackout_period_ = Duration::Seconds(10);
  Duration weight_update_period_ = Duration::Seconds(1);
  Duration weight_expiration_period_ = Duration::Minutes(3);
  float error_utilization_penalty_ = 1.0;
};

// WRR LB policy
class WeightedRoundRobin final : public LoadBalancingPolicy {
 public:
  explicit WeightedRoundRobin(Args args);

  absl::string_view name() const override { return kWeightedRoundRobin; }

  absl::Status UpdateLocked(UpdateArgs args) override;
  void ResetBackoffLocked() override;

 private:
  // Represents the weight for a given address.
  class EndpointWeight final : public RefCounted<EndpointWeight> {
   public:
    EndpointWeight(RefCountedPtr<WeightedRoundRobin> wrr,
                   EndpointAddressSet key)
        : wrr_(std::move(wrr)), key_(std::move(key)) {}
    ~EndpointWeight() override;

    void MaybeUpdateWeight(double qps, double eps, double utilization,
                           float error_utilization_penalty);

    float GetWeight(Timestamp now, Duration weight_expiration_period,
                    Duration blackout_period, uint64_t* num_not_yet_usable,
                    uint64_t* num_stale);

    void ResetNonEmptySince();

   private:
    RefCountedPtr<WeightedRoundRobin> wrr_;
    const EndpointAddressSet key_;

    Mutex mu_;
    float weight_ ABSL_GUARDED_BY(&mu_) = 0;
    Timestamp non_empty_since_ ABSL_GUARDED_BY(&mu_) = Timestamp::InfFuture();
    Timestamp last_update_time_ ABSL_GUARDED_BY(&mu_) = Timestamp::InfFuture();
  };

  class WrrEndpointList final : public EndpointList {
   public:
    class WrrEndpoint final : public Endpoint {
     public:
      WrrEndpoint(RefCountedPtr<EndpointList> endpoint_list,
                  const EndpointAddresses& addresses, const ChannelArgs& args,
                  std::shared_ptr<WorkSerializer> work_serializer,
                  std::vector<std::string>* errors)
          : Endpoint(std::move(endpoint_list)),
            weight_(policy<WeightedRoundRobin>()->GetOrCreateWeight(
                addresses.addresses())) {
        absl::Status status = Init(addresses, args, std::move(work_serializer));
        if (!status.ok()) {
          errors->emplace_back(absl::StrCat("endpoint ", addresses.ToString(),
                                            ": ", status.ToString()));
        }
      }

      RefCountedPtr<EndpointWeight> weight() const { return weight_; }

     private:
      class OobWatcher final : public OobBackendMetricWatcher {
       public:
        OobWatcher(RefCountedPtr<EndpointWeight> weight,
                   float error_utilization_penalty)
            : weight_(std::move(weight)),
              error_utilization_penalty_(error_utilization_penalty) {}

        void OnBackendMetricReport(
            const BackendMetricData& backend_metric_data) override;

       private:
        RefCountedPtr<EndpointWeight> weight_;
        const float error_utilization_penalty_;
      };

      RefCountedPtr<SubchannelInterface> CreateSubchannel(
          const grpc_resolved_address& address,
          const ChannelArgs& per_address_args,
          const ChannelArgs& args) override;

      // Called when the child policy reports a connectivity state update.
      void OnStateUpdate(std::optional<grpc_connectivity_state> old_state,
                         grpc_connectivity_state new_state,
                         const absl::Status& status) override;

      RefCountedPtr<EndpointWeight> weight_;
    };

    WrrEndpointList(RefCountedPtr<WeightedRoundRobin> wrr,
                    EndpointAddressesIterator* endpoints,
                    const ChannelArgs& args, std::string resolution_note,
                    std::vector<std::string>* errors)
        : EndpointList(std::move(wrr), std::move(resolution_note),
                       GRPC_TRACE_FLAG_ENABLED(weighted_round_robin_lb)
                           ? "WrrEndpointList"
                           : nullptr) {
      Init(endpoints, args,
           [&](RefCountedPtr<EndpointList> endpoint_list,
               const EndpointAddresses& addresses, const ChannelArgs& args) {
             return MakeOrphanable<WrrEndpoint>(
                 std::move(endpoint_list), addresses, args,
                 policy<WeightedRoundRobin>()->work_serializer(), errors);
           });
    }

   private:
    LoadBalancingPolicy::ChannelControlHelper* channel_control_helper()
        const override {
      return policy<WeightedRoundRobin>()->channel_control_helper();
    }

    // Updates the counters of children in each state when a
    // child transitions from old_state to new_state.
    void UpdateStateCountersLocked(
        std::optional<grpc_connectivity_state> old_state,
        grpc_connectivity_state new_state);

    // Ensures that the right child list is used and then updates
    // the WRR policy's connectivity state based on the child list's
    // state counters.
    void MaybeUpdateAggregatedConnectivityStateLocked(
        absl::Status status_for_tf);

    std::string CountersString() const {
      return absl::StrCat("num_children=", size(), " num_ready=", num_ready_,
                          " num_connecting=", num_connecting_,
                          " num_transient_failure=", num_transient_failure_);
    }

    size_t num_ready_ = 0;
    size_t num_connecting_ = 0;
    size_t num_transient_failure_ = 0;

    absl::Status last_failure_;
  };

  // A picker that performs WRR picks with weights based on
  // endpoint-reported utilization and QPS.
  class Picker final : public SubchannelPicker {
   public:
    Picker(RefCountedPtr<WeightedRoundRobin> wrr,
           WrrEndpointList* endpoint_list);

    ~Picker() override;

    PickResult Pick(PickArgs args) override;

   private:
    // A call tracker that collects per-call endpoint utilization reports.
    class SubchannelCallTracker final : public SubchannelCallTrackerInterface {
     public:
      SubchannelCallTracker(
          RefCountedPtr<EndpointWeight> weight, float error_utilization_penalty,
          std::unique_ptr<SubchannelCallTrackerInterface> child_tracker)
          : weight_(std::move(weight)),
            error_utilization_penalty_(error_utilization_penalty),
            child_tracker_(std::move(child_tracker)) {}

      void Start() override;

      void Finish(FinishArgs args) override;

     private:
      RefCountedPtr<EndpointWeight> weight_;
      const float error_utilization_penalty_;
      std::unique_ptr<SubchannelCallTrackerInterface> child_tracker_;
    };

    // Info stored about each endpoint.
    struct EndpointInfo {
      EndpointInfo(RefCountedPtr<SubchannelPicker> picker,
                   RefCountedPtr<EndpointWeight> weight)
          : picker(std::move(picker)), weight(std::move(weight)) {}

      RefCountedPtr<SubchannelPicker> picker;
      RefCountedPtr<EndpointWeight> weight;
    };

    void Orphaned() override;

    // Returns the index into endpoints_ to be picked.
    size_t PickIndex();

    // Builds a new scheduler and swaps it into place, then starts a
    // timer for the next update.
    void BuildSchedulerAndStartTimerLocked()
        ABSL_EXCLUSIVE_LOCKS_REQUIRED(&timer_mu_);

    RefCountedPtr<WeightedRoundRobin> wrr_;
    RefCountedPtr<WeightedRoundRobinConfig> config_;
    std::vector<EndpointInfo> endpoints_;

    Mutex scheduler_mu_;
    std::shared_ptr<StaticStrideScheduler> scheduler_
        ABSL_GUARDED_BY(&scheduler_mu_);

    Mutex timer_mu_ ABSL_ACQUIRED_BEFORE(&scheduler_mu_);
    std::optional<grpc_event_engine::experimental::EventEngine::TaskHandle>
        timer_handle_ ABSL_GUARDED_BY(&timer_mu_);

    // Used when falling back to RR.
    std::atomic<size_t> last_picked_index_;
  };

  ~WeightedRoundRobin() override;

  void ShutdownLocked() override;

  RefCountedPtr<EndpointWeight> GetOrCreateWeight(
      const std::vector<grpc_resolved_address>& addresses);

  RefCountedPtr<WeightedRoundRobinConfig> config_;

  // List of endpoints.
  OrphanablePtr<WrrEndpointList> endpoint_list_;
  // Latest pending endpoint list.
  // When we get an updated address list, we create a new endpoint list
  // for it here, and we wait to swap it into endpoint_list_ until the new
  // list becomes READY.
  OrphanablePtr<WrrEndpointList> latest_pending_endpoint_list_;

  Mutex endpoint_weight_map_mu_;
  std::map<EndpointAddressSet, EndpointWeight*> endpoint_weight_map_
      ABSL_GUARDED_BY(&endpoint_weight_map_mu_);

  const absl::string_view locality_name_;

  bool shutdown_ = false;

  // Accessed by picker.
  std::atomic<uint32_t> scheduler_state_{
      absl::Uniform<uint32_t>(SharedBitGen())};
};

//
// WeightedRoundRobin::EndpointWeight
//

WeightedRoundRobin::EndpointWeight::~EndpointWeight() {
  MutexLock lock(&wrr_->endpoint_weight_map_mu_);
  auto it = wrr_->endpoint_weight_map_.find(key_);
  if (it != wrr_->endpoint_weight_map_.end() && it->second == this) {
    wrr_->endpoint_weight_map_.erase(it);
  }
}

void WeightedRoundRobin::EndpointWeight::MaybeUpdateWeight(
    double qps, double eps, double utilization,
    float error_utilization_penalty) {
  // Compute weight.
  float weight = 0;
  if (qps > 0 && utilization > 0) {
    double penalty = 0.0;
    if (eps > 0 && error_utilization_penalty > 0) {
      penalty = eps / qps * error_utilization_penalty;
    }
    weight = qps / (utilization + penalty);
  }
  if (weight == 0) {
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << wrr_.get() << "] subchannel " << key_.ToString()
        << ": qps=" << qps << ", eps=" << eps << ", utilization=" << utilization
        << ": error_util_penalty=" << error_utilization_penalty
        << ", weight=" << weight << " (not updating)";
    return;
  }
  Timestamp now = Timestamp::Now();
  // Grab the lock and update the data.
  MutexLock lock(&mu_);
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr_.get() << "] subchannel " << key_.ToString()
      << ": qps=" << qps << ", eps=" << eps << ", utilization=" << utilization
      << " error_util_penalty=" << error_utilization_penalty
      << " : setting weight=" << weight << " weight_=" << weight_
      << " now=" << now.ToString()
      << " last_update_time_=" << last_update_time_.ToString()
      << " non_empty_since_=" << non_empty_since_.ToString();
  if (non_empty_since_ == Timestamp::InfFuture()) non_empty_since_ = now;
  weight_ = weight;
  last_update_time_ = now;
}

float WeightedRoundRobin::EndpointWeight::GetWeight(
    Timestamp now, Duration weight_expiration_period, Duration blackout_period,
    uint64_t* num_not_yet_usable, uint64_t* num_stale) {
  MutexLock lock(&mu_);
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr_.get() << "] subchannel " << key_.ToString()
      << ": getting weight: now=" << now.ToString()
      << " weight_expiration_period=" << weight_expiration_period.ToString()
      << " blackout_period=" << blackout_period.ToString()
      << " last_update_time_=" << last_update_time_.ToString()
      << " non_empty_since_=" << non_empty_since_.ToString()
      << " weight_=" << weight_;
  // If the most recent update was longer ago than the expiration
  // period, reset non_empty_since_ so that we apply the blackout period
  // again if we start getting data again in the future, and return 0.
  if (now - last_update_time_ >= weight_expiration_period) {
    ++*num_stale;
    non_empty_since_ = Timestamp::InfFuture();
    return 0;
  }
  // If we don't have at least blackout_period worth of data, return 0.
  if (blackout_period > Duration::Zero() &&
      now - non_empty_since_ < blackout_period) {
    ++*num_not_yet_usable;
    return 0;
  }
  // Otherwise, return the weight.
  return weight_;
}

void WeightedRoundRobin::EndpointWeight::ResetNonEmptySince() {
  MutexLock lock(&mu_);
  non_empty_since_ = Timestamp::InfFuture();
}

//
// WeightedRoundRobin::Picker::SubchannelCallTracker
//

void WeightedRoundRobin::Picker::SubchannelCallTracker::Start() {
  if (child_tracker_ != nullptr) child_tracker_->Start();
}

void WeightedRoundRobin::Picker::SubchannelCallTracker::Finish(
    FinishArgs args) {
  if (child_tracker_ != nullptr) child_tracker_->Finish(args);
  auto* backend_metric_data =
      args.backend_metric_accessor->GetBackendMetricData();
  double qps = 0;
  double eps = 0;
  double utilization = 0;
  if (backend_metric_data != nullptr) {
    qps = backend_metric_data->qps;
    eps = backend_metric_data->eps;
    utilization = backend_metric_data->application_utilization;
    if (utilization <= 0) {
      utilization = backend_metric_data->cpu_utilization;
    }
  }
  weight_->MaybeUpdateWeight(qps, eps, utilization, error_utilization_penalty_);
}

//
// WeightedRoundRobin::Picker
//

WeightedRoundRobin::Picker::Picker(RefCountedPtr<WeightedRoundRobin> wrr,
                                   WrrEndpointList* endpoint_list)
    : wrr_(std::move(wrr)),
      config_(wrr_->config_),
      last_picked_index_(absl::Uniform<size_t>(SharedBitGen())) {
  for (auto& endpoint : endpoint_list->endpoints()) {
    auto* ep = static_cast<WrrEndpointList::WrrEndpoint*>(endpoint.get());
    if (ep->connectivity_state() == GRPC_CHANNEL_READY) {
      endpoints_.emplace_back(ep->picker(), ep->weight());
    }
  }
  global_stats().IncrementWrrSubchannelListSize(endpoint_list->size());
  global_stats().IncrementWrrSubchannelReadySize(endpoints_.size());
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr_.get() << " picker " << this
      << "] created picker from endpoint_list=" << endpoint_list << " with "
      << endpoints_.size() << " subchannels";
  // Note: BuildSchedulerAndStartTimerLocked() passes out pointers to `this`,
  // so we need to ensure that we really hold timer_mu_.
  MutexLock lock(&timer_mu_);
  BuildSchedulerAndStartTimerLocked();
}

WeightedRoundRobin::Picker::~Picker() {
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr_.get() << " picker " << this << "] destroying picker";
}

void WeightedRoundRobin::Picker::Orphaned() {
  MutexLock lock(&timer_mu_);
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr_.get() << " picker " << this << "] cancelling timer";
  wrr_->channel_control_helper()->GetEventEngine()->Cancel(*timer_handle_);
  timer_handle_.reset();
  wrr_.reset();
}

WeightedRoundRobin::PickResult WeightedRoundRobin::Picker::Pick(PickArgs args) {
  size_t index = PickIndex();
  CHECK(index < endpoints_.size());
  auto& endpoint_info = endpoints_[index];
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr_.get() << " picker " << this << "] returning index "
      << index << ", picker=" << endpoint_info.picker.get();
  auto result = endpoint_info.picker->Pick(args);
  // Collect per-call utilization data if needed.
  if (!config_->enable_oob_load_report()) {
    auto* complete = std::get_if<PickResult::Complete>(&result.result);
    if (complete != nullptr) {
      complete->subchannel_call_tracker =
          std::make_unique<SubchannelCallTracker>(
              endpoint_info.weight, config_->error_utilization_penalty(),
              std::move(complete->subchannel_call_tracker));
    }
  }
  return result;
}

size_t WeightedRoundRobin::Picker::PickIndex() {
  // Grab a ref to the scheduler.
  std::shared_ptr<StaticStrideScheduler> scheduler;
  {
    MutexLock lock(&scheduler_mu_);
    scheduler = scheduler_;
  }
  // If we have a scheduler, use it to do a WRR pick.
  if (scheduler != nullptr) return scheduler->Pick();
  // We don't have a scheduler (i.e., either all of the weights are 0 or
  // there is only one subchannel), so fall back to RR.
  return last_picked_index_.fetch_add(1) % endpoints_.size();
}

void WeightedRoundRobin::Picker::BuildSchedulerAndStartTimerLocked() {
  auto& stats_plugins = wrr_->channel_control_helper()->GetStatsPluginGroup();
  // Build scheduler, reporting metrics on endpoint weights.
  const Timestamp now = Timestamp::Now();
  std::vector<float> weights;
  weights.reserve(endpoints_.size());
  uint64_t num_not_yet_usable = 0;
  uint64_t num_stale = 0;
  for (const auto& endpoint : endpoints_) {
    float weight = endpoint.weight->GetWeight(
        now, config_->weight_expiration_period(), config_->blackout_period(),
        &num_not_yet_usable, &num_stale);
    weights.push_back(weight);
    stats_plugins.RecordHistogram(kMetricEndpointWeights, weight,
                                  {wrr_->channel_control_helper()->GetTarget()},
                                  {wrr_->locality_name_});
  }
  stats_plugins.AddCounter(
      kMetricEndpointWeightNotYetUsable, num_not_yet_usable,
      {wrr_->channel_control_helper()->GetTarget()}, {wrr_->locality_name_});
  stats_plugins.AddCounter(kMetricEndpointWeightStale, num_stale,
                           {wrr_->channel_control_helper()->GetTarget()},
                           {wrr_->locality_name_});
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr_.get() << " picker " << this
      << "] new weights: " << absl::StrJoin(weights, " ");
  auto scheduler_or = StaticStrideScheduler::Make(
      weights, [this]() { return wrr_->scheduler_state_.fetch_add(1); });
  std::shared_ptr<StaticStrideScheduler> scheduler;
  if (scheduler_or.has_value()) {
    scheduler =
        std::make_shared<StaticStrideScheduler>(std::move(*scheduler_or));
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << wrr_.get() << " picker " << this
        << "] new scheduler: " << scheduler.get();
  } else {
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << wrr_.get() << " picker " << this
        << "] no scheduler, falling back to RR";
    stats_plugins.AddCounter(kMetricRrFallback, 1,
                             {wrr_->channel_control_helper()->GetTarget()},
                             {wrr_->locality_name_});
  }
  {
    MutexLock lock(&scheduler_mu_);
    scheduler_ = std::move(scheduler);
  }
  // Start timer.
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr_.get() << " picker " << this
      << "] scheduling timer for "
      << config_->weight_update_period().ToString();
  // It's insufficient to hold the implicit constructor lock here, a real lock
  // over timer_mu_ is needed: we update timer_handle_ after the timer is
  // scheduled, but it may run on another thread before that occurs, causing a
  // race.
  timer_handle_ = wrr_->channel_control_helper()->GetEventEngine()->RunAfter(
      config_->weight_update_period(),
      [self = WeakRefAsSubclass<Picker>(),
       work_serializer = wrr_->work_serializer()]() mutable {
        ExecCtx exec_ctx;
        {
          MutexLock lock(&self->timer_mu_);
          if (self->timer_handle_.has_value()) {
            GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
                << "[WRR " << self->wrr_.get() << " picker " << self.get()
                << "] timer fired";
            self->BuildSchedulerAndStartTimerLocked();
          }
        }
        self.reset();
      });
}

//
// WeightedRoundRobin
//

WeightedRoundRobin::WeightedRoundRobin(Args args)
    : LoadBalancingPolicy(std::move(args)),
      locality_name_(channel_args()
                         .GetString(GRPC_ARG_LB_WEIGHTED_TARGET_CHILD)
                         .value_or("")) {
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << this << "] Created -- locality_name=\""
      << std::string(locality_name_) << "\"";
}

WeightedRoundRobin::~WeightedRoundRobin() {
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << this << "] Destroying Round Robin policy";
  CHECK(endpoint_list_ == nullptr);
  CHECK(latest_pending_endpoint_list_ == nullptr);
}

void WeightedRoundRobin::ShutdownLocked() {
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << this << "] Shutting down";
  shutdown_ = true;
  endpoint_list_.reset();
  latest_pending_endpoint_list_.reset();
}

void WeightedRoundRobin::ResetBackoffLocked() {
  endpoint_list_->ResetBackoffLocked();
  if (latest_pending_endpoint_list_ != nullptr) {
    latest_pending_endpoint_list_->ResetBackoffLocked();
  }
}

absl::Status WeightedRoundRobin::UpdateLocked(UpdateArgs args) {
  global_stats().IncrementWrrUpdates();
  config_ = args.config.TakeAsSubclass<WeightedRoundRobinConfig>();
  std::shared_ptr<EndpointAddressesIterator> addresses;
  if (args.addresses.ok()) {
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << this << "] received update";
    // Weed out duplicate endpoints.  Also sort the endpoints so that if
    // the set of endpoints doesn't change, their indexes in the endpoint
    // list don't change, since this avoids unnecessary churn in the
    // picker.  Note that this does not ensure that if a given endpoint
    // remains present that it will have the same index; if, for example,
    // an endpoint at the end of the list is replaced with one that sorts
    // much earlier in the list, then all of the endpoints in between those
    // two positions will have changed indexes.
    struct EndpointAddressesLessThan {
      bool operator()(const EndpointAddresses& endpoint1,
                      const EndpointAddresses& endpoint2) const {
        // Compare unordered addresses only, not channel args.
        EndpointAddressSet e1(endpoint1.addresses());
        EndpointAddressSet e2(endpoint2.addresses());
        return e1 < e2;
      }
    };
    std::set<EndpointAddresses, EndpointAddressesLessThan> ordered_addresses;
    (*args.addresses)->ForEach([&](const EndpointAddresses& endpoint) {
      ordered_addresses.insert(endpoint);
    });
    addresses =
        std::make_shared<EndpointAddressesListIterator>(EndpointAddressesList(
            ordered_addresses.begin(), ordered_addresses.end()));
  } else {
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << this << "] received update with address error: "
        << args.addresses.status().ToString();
    // If we already have an endpoint list, then keep using the existing
    // list, but still report back that the update was not accepted.
    if (endpoint_list_ != nullptr) return args.addresses.status();
  }
  // Create new endpoint list, replacing the previous pending list, if any.
  if (GRPC_TRACE_FLAG_ENABLED(weighted_round_robin_lb) &&
      latest_pending_endpoint_list_ != nullptr) {
    LOG(INFO) << "[WRR " << this
              << "] replacing previous pending endpoint list "
              << latest_pending_endpoint_list_.get();
  }
  std::vector<std::string> errors;
  latest_pending_endpoint_list_ = MakeOrphanable<WrrEndpointList>(
      RefAsSubclass<WeightedRoundRobin>(), addresses.get(), args.args,
      std::move(args.resolution_note), &errors);
  // If the new list is empty, immediately promote it to
  // endpoint_list_ and report TRANSIENT_FAILURE.
  if (latest_pending_endpoint_list_->size() == 0) {
    if (GRPC_TRACE_FLAG_ENABLED(weighted_round_robin_lb) &&
        endpoint_list_ != nullptr) {
      LOG(INFO) << "[WRR " << this << "] replacing previous endpoint list "
                << endpoint_list_.get();
    }
    endpoint_list_ = std::move(latest_pending_endpoint_list_);
    absl::Status status = args.addresses.ok()
                              ? absl::UnavailableError("empty address list")
                              : args.addresses.status();
    endpoint_list_->ReportTransientFailure(status);
    return status;
  }
  // Otherwise, if this is the initial update, immediately promote it to
  // endpoint_list_.
  if (endpoint_list_.get() == nullptr) {
    endpoint_list_ = std::move(latest_pending_endpoint_list_);
  }
  if (!errors.empty()) {
    return absl::UnavailableError(absl::StrCat(
        "errors from children: [", absl::StrJoin(errors, "; "), "]"));
  }
  return absl::OkStatus();
}

RefCountedPtr<WeightedRoundRobin::EndpointWeight>
WeightedRoundRobin::GetOrCreateWeight(
    const std::vector<grpc_resolved_address>& addresses) {
  EndpointAddressSet key(addresses);
  MutexLock lock(&endpoint_weight_map_mu_);
  auto it = endpoint_weight_map_.find(key);
  if (it != endpoint_weight_map_.end()) {
    auto weight = it->second->RefIfNonZero();
    if (weight != nullptr) return weight;
  }
  auto weight = MakeRefCounted<EndpointWeight>(
      RefAsSubclass<WeightedRoundRobin>(DEBUG_LOCATION, "EndpointWeight"), key);
  endpoint_weight_map_.emplace(key, weight.get());
  return weight;
}

//
// WeightedRoundRobin::WrrEndpointList::WrrEndpoint::OobWatcher
//

void WeightedRoundRobin::WrrEndpointList::WrrEndpoint::OobWatcher::
    OnBackendMetricReport(const BackendMetricData& backend_metric_data) {
  double utilization = backend_metric_data.application_utilization;
  if (utilization <= 0) {
    utilization = backend_metric_data.cpu_utilization;
  }
  weight_->MaybeUpdateWeight(backend_metric_data.qps, backend_metric_data.eps,
                             utilization, error_utilization_penalty_);
}

//
// WeightedRoundRobin::WrrEndpointList::WrrEndpoint
//

RefCountedPtr<SubchannelInterface>
WeightedRoundRobin::WrrEndpointList::WrrEndpoint::CreateSubchannel(
    const grpc_resolved_address& address, const ChannelArgs& per_address_args,
    const ChannelArgs& args) {
  auto* wrr = policy<WeightedRoundRobin>();
  auto subchannel = wrr->channel_control_helper()->CreateSubchannel(
      address, per_address_args, args);
  // Start OOB watch if configured.
  if (wrr->config_->enable_oob_load_report()) {
    subchannel->AddDataWatcher(MakeOobBackendMetricWatcher(
        wrr->config_->oob_reporting_period(),
        std::make_unique<OobWatcher>(
            weight_, wrr->config_->error_utilization_penalty())));
  }
  return subchannel;
}

void WeightedRoundRobin::WrrEndpointList::WrrEndpoint::OnStateUpdate(
    std::optional<grpc_connectivity_state> old_state,
    grpc_connectivity_state new_state, const absl::Status& status) {
  auto* wrr_endpoint_list = endpoint_list<WrrEndpointList>();
  auto* wrr = policy<WeightedRoundRobin>();
  GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
      << "[WRR " << wrr << "] connectivity changed for child " << this
      << ", endpoint_list " << wrr_endpoint_list << " (index " << Index()
      << " of " << wrr_endpoint_list->size() << "): prev_state="
      << (old_state.has_value() ? ConnectivityStateName(*old_state) : "N/A")
      << " new_state=" << ConnectivityStateName(new_state) << " (" << status
      << ")";
  if (new_state == GRPC_CHANNEL_IDLE) {
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << wrr << "] child " << this
        << " reported IDLE; requesting connection";
    ExitIdleLocked();
  } else if (new_state == GRPC_CHANNEL_READY) {
    // If we transition back to READY state, restart the blackout period.
    // Skip this if this is the initial notification for this
    // endpoint (which happens whenever we get updated addresses and
    // create a new endpoint list).  Also skip it if the previous state
    // was READY (which should never happen in practice, but we've seen
    // at least one bug that caused this in the outlier_detection
    // policy, so let's be defensive here).
    //
    // Note that we cannot guarantee that we will never receive
    // lingering callbacks for backend metric reports from the previous
    // connection after the new connection has been established, but they
    // should be masked by new backend metric reports from the new
    // connection by the time the blackout period ends.
    if (old_state.has_value() && old_state != GRPC_CHANNEL_READY) {
      weight_->ResetNonEmptySince();
    }
  }
  // If state changed, update state counters.
  if (!old_state.has_value() || *old_state != new_state) {
    wrr_endpoint_list->UpdateStateCountersLocked(old_state, new_state);
  }
  // Update the policy state.
  wrr_endpoint_list->MaybeUpdateAggregatedConnectivityStateLocked(status);
}

//
// WeightedRoundRobin::WrrEndpointList
//

void WeightedRoundRobin::WrrEndpointList::UpdateStateCountersLocked(
    std::optional<grpc_connectivity_state> old_state,
    grpc_connectivity_state new_state) {
  // We treat IDLE the same as CONNECTING, since it will immediately
  // transition into that state anyway.
  if (old_state.has_value()) {
    CHECK(*old_state != GRPC_CHANNEL_SHUTDOWN);
    if (*old_state == GRPC_CHANNEL_READY) {
      CHECK_GT(num_ready_, 0u);
      --num_ready_;
    } else if (*old_state == GRPC_CHANNEL_CONNECTING ||
               *old_state == GRPC_CHANNEL_IDLE) {
      CHECK_GT(num_connecting_, 0u);
      --num_connecting_;
    } else if (*old_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
      CHECK_GT(num_transient_failure_, 0u);
      --num_transient_failure_;
    }
  }
  CHECK(new_state != GRPC_CHANNEL_SHUTDOWN);
  if (new_state == GRPC_CHANNEL_READY) {
    ++num_ready_;
  } else if (new_state == GRPC_CHANNEL_CONNECTING ||
             new_state == GRPC_CHANNEL_IDLE) {
    ++num_connecting_;
  } else if (new_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
    ++num_transient_failure_;
  }
}

void WeightedRoundRobin::WrrEndpointList::
    MaybeUpdateAggregatedConnectivityStateLocked(absl::Status status_for_tf) {
  auto* wrr = policy<WeightedRoundRobin>();
  // If this is latest_pending_endpoint_list_, then swap it into
  // endpoint_list_ in the following cases:
  // - endpoint_list_ has no READY children.
  // - This list has at least one READY child and we have seen the
  //   initial connectivity state notification for all children.
  // - All of the children in this list are in TRANSIENT_FAILURE.
  //   (This may cause the channel to go from READY to TRANSIENT_FAILURE,
  //   but we're doing what the control plane told us to do.)
  if (wrr->latest_pending_endpoint_list_.get() == this &&
      (wrr->endpoint_list_->num_ready_ == 0 ||
       (num_ready_ > 0 && AllEndpointsSeenInitialState()) ||
       num_transient_failure_ == size())) {
    if (GRPC_TRACE_FLAG_ENABLED(weighted_round_robin_lb)) {
      LOG(INFO) << "[WRR " << wrr << "] swapping out endpoint list "
                << wrr->endpoint_list_.get() << " ("
                << wrr->endpoint_list_->CountersString() << ") in favor of "
                << this << " (" << CountersString() << ")";
    }
    wrr->endpoint_list_ = std::move(wrr->latest_pending_endpoint_list_);
  }
  // Only set connectivity state if this is the current endpoint list.
  if (wrr->endpoint_list_.get() != this) return;
  // First matching rule wins:
  // 1) ANY child is READY => policy is READY.
  // 2) ANY child is CONNECTING => policy is CONNECTING.
  // 3) ALL children are TRANSIENT_FAILURE => policy is TRANSIENT_FAILURE.
  if (num_ready_ > 0) {
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << wrr << "] reporting READY with endpoint list " << this;
    wrr->channel_control_helper()->UpdateState(
        GRPC_CHANNEL_READY, absl::OkStatus(),
        MakeRefCounted<Picker>(wrr->RefAsSubclass<WeightedRoundRobin>(), this));
  } else if (num_connecting_ > 0) {
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << wrr << "] reporting CONNECTING with endpoint list "
        << this;
    wrr->channel_control_helper()->UpdateState(
        GRPC_CHANNEL_CONNECTING, absl::OkStatus(),
        MakeRefCounted<QueuePicker>(nullptr));
  } else if (num_transient_failure_ == size()) {
    GRPC_TRACE_LOG(weighted_round_robin_lb, INFO)
        << "[WRR " << wrr << "] reporting TRANSIENT_FAILURE with endpoint list "
        << this << ": " << status_for_tf;
    if (!status_for_tf.ok()) {
      last_failure_ = absl::UnavailableError(
          absl::StrCat("connections to all backends failing; last error: ",
                       status_for_tf.ToString()));
    }
    ReportTransientFailure(last_failure_);
  }
}

//
// factory
//

class WeightedRoundRobinFactory final : public LoadBalancingPolicyFactory {
 public:
  OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
      LoadBalancingPolicy::Args args) const override {
    return MakeOrphanable<WeightedRoundRobin>(std::move(args));
  }

  absl::string_view name() const override { return kWeightedRoundRobin; }

  absl::StatusOr<RefCountedPtr<LoadBalancingPolicy::Config>>
  ParseLoadBalancingConfig(const Json& json) const override {
    return LoadFromJson<RefCountedPtr<WeightedRoundRobinConfig>>(
        json, JsonArgs(),
        "errors validating weighted_round_robin LB policy config");
  }
};

}  // namespace

void RegisterWeightedRoundRobinLbPolicy(CoreConfiguration::Builder* builder) {
  builder->lb_policy_registry()->RegisterLoadBalancingPolicyFactory(
      std::make_unique<WeightedRoundRobinFactory>());
}

}  // namespace grpc_core
