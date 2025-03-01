//
// Copyright 2021 gRPC authors.
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

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/fault_injection/fault_injection_filter.h"

#include <atomic>

#include "absl/strings/numbers.h"

#include <grpc/status.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/ext/filters/fault_injection/service_config_parser.h"
#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/channel/status_util.h"
#include "src/core/lib/promise/sleep.h"
#include "src/core/lib/promise/try_seq.h"
#include "src/core/lib/service_config/service_config_call_data.h"
#include "src/core/lib/transport/status_conversion.h"
#include "src/core/lib/transport/transport.h"

namespace grpc_core {

TraceFlag grpc_fault_injection_filter_trace(false, "fault_injection_filter");

namespace {

std::atomic<uint32_t> g_active_faults{0};
static_assert(
    std::is_trivially_destructible<std::atomic<uint32_t>>::value,
    "the active fault counter needs to have a trivially destructible type");

template <typename T>
auto AsInt(absl::string_view s) -> absl::optional<T> {
  T x;
  if (absl::SimpleAtoi(s, &x)) return x;
  return absl::nullopt;
}

inline bool UnderFraction(const uint32_t numerator,
                          const uint32_t denominator) {
  if (numerator <= 0) return false;
  if (numerator >= denominator) return true;
  // Generate a random number in [0, denominator).
  const uint32_t random_number = rand() % denominator;
  return random_number < numerator;
}

}  // namespace

class FaultInjectionFilter::InjectionDecision {
 public:
  InjectionDecision(uint32_t max_faults, Duration delay_time,
                    absl::optional<absl::Status> abort_request)
      : max_faults_(max_faults),
        delay_time_(delay_time),
        abort_request_(abort_request) {}

  std::string ToString() const;
  Timestamp DelayUntil() const;
  absl::Status MaybeAbort() const;

 private:
  bool HaveActiveFaultsQuota(bool increment) const;

  uint32_t max_faults_;
  Duration delay_time_;
  absl::optional<absl::Status> abort_request_;
};

absl::StatusOr<FaultInjectionFilter> FaultInjectionFilter::Create(
    ChannelArgs, ChannelFilter::Args filter_args) {
  return FaultInjectionFilter(filter_args);
}

FaultInjectionFilter::FaultInjectionFilter(ChannelFilter::Args filter_args)
    : index_(grpc_channel_stack_filter_instance_number(
          filter_args.channel_stack(),
          filter_args.uninitialized_channel_element())),
      service_config_parser_index_(
          FaultInjectionServiceConfigParser::ParserIndex()) {}

// Construct a promise for one call.
ArenaPromise<ServerMetadataHandle> FaultInjectionFilter::MakeCallPromise(
    CallArgs call_args, NextPromiseFactory next_promise_factory) {
  auto decision = MakeInjectionDecision(call_args.client_initial_metadata);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_fault_injection_filter_trace)) {
    gpr_log(GPR_INFO, "chand=%p: Fault injection triggered %s", this,
            decision.ToString().c_str());
  }
  return TrySeq(
      Sleep(decision.DelayUntil()),
      [decision]() { return decision.MaybeAbort(); },
      next_promise_factory(std::move(call_args)));
}

FaultInjectionFilter::InjectionDecision
FaultInjectionFilter::MakeInjectionDecision(
    const ClientMetadataHandle& initial_metadata) {
  // Fetch the fault injection policy from the service config, based on the
  // relative index for which policy should this CallData use.
  auto* service_config_call_data = static_cast<ServiceConfigCallData*>(
      GetContext<
          grpc_call_context_element>()[GRPC_CONTEXT_SERVICE_CONFIG_CALL_DATA]
          .value);
  auto* method_params = static_cast<FaultInjectionMethodParsedConfig*>(
      service_config_call_data->GetMethodParsedConfig(
          service_config_parser_index_));
  const FaultInjectionMethodParsedConfig::FaultInjectionPolicy* fi_policy =
      nullptr;
  if (method_params != nullptr) {
    fi_policy = method_params->fault_injection_policy(index_);
  }

  grpc_status_code abort_code = fi_policy->abort_code;
  uint32_t abort_percentage_numerator = fi_policy->abort_percentage_numerator;
  uint32_t delay_percentage_numerator = fi_policy->delay_percentage_numerator;
  Duration delay = fi_policy->delay;

  // Update the policy with values in initial metadata.
  if (!fi_policy->abort_code_header.empty() ||
      !fi_policy->abort_percentage_header.empty() ||
      !fi_policy->delay_header.empty() ||
      !fi_policy->delay_percentage_header.empty()) {
    std::string buffer;
    if (!fi_policy->abort_code_header.empty() && abort_code == GRPC_STATUS_OK) {
      auto value = initial_metadata->GetStringValue(
          fi_policy->abort_code_header, &buffer);
      if (value.has_value()) {
        grpc_status_code_from_int(
            AsInt<int>(*value).value_or(GRPC_STATUS_UNKNOWN), &abort_code);
      }
    }
    if (!fi_policy->abort_percentage_header.empty()) {
      auto value = initial_metadata->GetStringValue(
          fi_policy->abort_percentage_header, &buffer);
      if (value.has_value()) {
        abort_percentage_numerator = std::min(
            AsInt<uint32_t>(*value).value_or(-1), abort_percentage_numerator);
      }
    }
    if (!fi_policy->delay_header.empty() && delay == Duration::Zero()) {
      auto value =
          initial_metadata->GetStringValue(fi_policy->delay_header, &buffer);
      if (value.has_value()) {
        delay = Duration::Milliseconds(
            std::max(AsInt<int64_t>(*value).value_or(0), int64_t(0)));
      }
    }
    if (!fi_policy->delay_percentage_header.empty()) {
      auto value = initial_metadata->GetStringValue(
          fi_policy->delay_percentage_header, &buffer);
      if (value.has_value()) {
        delay_percentage_numerator = std::min(
            AsInt<uint32_t>(*value).value_or(-1), delay_percentage_numerator);
      }
    }
  }
  // Roll the dice
  const bool delay_request =
      delay != Duration::Zero() &&
      UnderFraction(delay_percentage_numerator,
                    fi_policy->delay_percentage_denominator);
  const bool abort_request =
      abort_code != GRPC_STATUS_OK &&
      UnderFraction(abort_percentage_numerator,
                    fi_policy->abort_percentage_denominator);

  return InjectionDecision(
      fi_policy->max_faults, delay_request ? delay : Duration::Zero(),
      abort_request ? absl::optional<absl::Status>(absl::Status(
                          static_cast<absl::StatusCode>(abort_code),
                          fi_policy->abort_message))
                    : absl::nullopt);
}

bool FaultInjectionFilter::InjectionDecision::HaveActiveFaultsQuota(
    bool increment) const {
  if (g_active_faults.load(std::memory_order_acquire) >= max_faults_) {
    return false;
  }
  if (increment) g_active_faults.fetch_add(1, std::memory_order_relaxed);
  return true;
}

Timestamp FaultInjectionFilter::InjectionDecision::DelayUntil() const {
  if (delay_time_ != Duration::Zero() && HaveActiveFaultsQuota(true)) {
    return ExecCtx::Get()->Now() + delay_time_;
  }
  return Timestamp::InfPast();
}

absl::Status FaultInjectionFilter::InjectionDecision::MaybeAbort() const {
  if (abort_request_.has_value() &&
      (delay_time_ != Duration::Zero() || HaveActiveFaultsQuota(false))) {
    return abort_request_.value();
  }
  return absl::OkStatus();
}

std::string FaultInjectionFilter::InjectionDecision::ToString() const {
  return absl::StrCat("delay=", delay_time_ != Duration::Zero(),
                      " abort=", abort_request_.has_value());
}

const grpc_channel_filter FaultInjectionFilter::kFilter =
    MakePromiseBasedFilter<FaultInjectionFilter, FilterEndpoint::kClient>(
        "fault_injection_filter");

void FaultInjectionFilterRegister(CoreConfiguration::Builder* builder) {
  FaultInjectionServiceConfigParser::Register(builder);
}

}  // namespace grpc_core
