// Copyright 2025 The gRPC Authors.
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

#include "src/core/telemetry/default_tcp_tracer.h"

#include "third_party/grpc/src/core/telemetry/default_tcp_tracer.h"
#include "third_party/grpc/src/core/util/sync.h"

namespace grpc_core {

void DefaultTcpTracer::RecordConnectionMetrics(
    grpc_core::TcpConnectionMetrics metrics) {
  MutexLock lock(&mu_);
  connection_metrics_ = metrics;
}

}  // namespace grpc_core
