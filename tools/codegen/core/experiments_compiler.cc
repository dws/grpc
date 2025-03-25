#include "tools/codegen/core/experiments_compiler.h"

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "third_party/yamlcpp/wrapped/yaml_cpp_wrapped.h"

namespace grpc_core {

ExperimentDefinition::ExperimentDefinition(
    const std::string& name, const std::string& description,
    const std::string& owner, const std::string& expiry, bool uses_polling,
    bool allow_in_fuzzing_config, const std::vector<std::string>& test_tags,
    const std::vector<std::string>& requirements)
    : error_(false),
      name_(name),
      description_(description),
      owner_(owner),
      expiry_(expiry),
      uses_polling_(uses_polling),
      allow_in_fuzzing_config_(allow_in_fuzzing_config),
      test_tags_(test_tags),
      requires_(requirements) {
  if (name.empty()) {
    LOG(ERROR) << "ERROR: experiment with no name";
    error_ = true;
  }
  if (description.empty()) {
    LOG(ERROR) << "ERROR: no description for experiment " << name_;
    error_ = true;
  }
  if (owner.empty()) {
    LOG(ERROR) << "ERROR: no owner for experiment " << name_;
    error_ = true;
  }
  if (expiry.empty()) {
    LOG(ERROR) << "ERROR: no expiry for experiment " << name_;
    error_ = true;
  }
  if (name_ == "monitoring_experiment" && expiry_ != "never-ever") {
    LOG(ERROR) << "ERROR: monitoring_experiment should never expire";
    error_ = true;
  }
  if (error_) {
    LOG(ERROR) << "Failed to create experiment definition";
  }
}

bool ExperimentDefinition::IsValid(bool check_expiry) const {
  if (error_) {
    return false;
  }
  if (name_ == "monitoring_experiment" && expiry_ == "never-ever") {
    return true;
  }

  absl::Time expiry_time;
  std::string error;
  if (!absl::ParseTime("%Y-%m-%d", expiry_, &expiry_time, &error)) {
    LOG(ERROR) << "ERROR: Invalid date format in expiry: " << expiry_
               << " for experiment " << name_;
    return false;
  }
  absl::CivilMonth expiry_month =
      absl::ToCivilMonth(expiry_time, absl::UTCTimeZone());
  absl::CivilDay expiry_day =
      absl::ToCivilDay(expiry_time, absl::UTCTimeZone());
  if (expiry_month.month() == 11 || expiry_month.month() == 12 ||
      (expiry_month.month() == 1 && expiry_day.day() < 15)) {
    LOG(ERROR) << "For experiment " << name_
               << ": Experiment expiration is not allowed between Nov 1 and "
                  "Jan 15 (experiment lists "
               << expiry_ << ").";
    return false;
  }

  if (!check_expiry) {
    return true;
  }

  if (expiry_time < absl::Now()) {
    LOG(WARNING) << "WARNING: experiment " << name_ << " expired on "
                 << expiry_;
  }
  absl::Time two_quarters_from_now = absl::Now() + absl::Hours(180 * 24);
  if (expiry_time > two_quarters_from_now) {
    LOG(WARNING) << "WARNING: experiment " << name_
                 << " expires far in the future on " << expiry_;
    LOG(WARNING) << "expiry should be no more than two quarters from now";
  }

  return !error_;
}

bool ExperimentDefinition::AddRolloutSpecification(
    const std::set<std::string>& allowed_defaults,
    const std::set<std::string>& allowed_platforms,
    RolloutSpecification& rollout_attributes) {
  if (error_) {
    return false;
  }
  if (rollout_attributes.name != name_) {
    LOG(ERROR)
        << "ERROR: Rollout specification does not apply to this experiment: "
        << name_;
    return false;
  }

  if (!rollout_attributes.requirements.empty()) {
    for (const auto& requirement : rollout_attributes.requirements) {
      requires_.push_back(requirement);
    }
  }

  if (rollout_attributes.default_value.empty() &&
      rollout_attributes.platform_value.empty()) {
    LOG(ERROR) << "ERROR: no default for experiment "
               << rollout_attributes.name;
    error_ = true;
    return false;
  }

  for (const auto& platform : allowed_platforms) {
    if (!rollout_attributes.default_value.empty()) {
      if (allowed_defaults.find(rollout_attributes.default_value) ==
          allowed_defaults.end()) {
        LOG(ERROR) << "ERROR: default for experiment "
                   << rollout_attributes.name << " on platform " << platform
                   << "is of incorrect format";
        error_ = true;
        return false;
      }
      defaults_[platform] = rollout_attributes.default_value;
      additional_constraints_[platform] = {};
    } else {
      if (rollout_attributes.platform_value.find(platform) ==
          rollout_attributes.platform_value.end()) {
        LOG(ERROR) << "ERROR: no default for experiment "
                   << rollout_attributes.name << " on platform " << platform;
        error_ = true;
        return false;
      } else {
        // debug is assumed for all rollouts with additional constraints
        defaults_[platform] = "debug";
        additional_constraints_[platform] =
            rollout_attributes.platform_value[platform];
      }
    }
  }
  return true;
}

absl::Status ExperimentsCompiler::AddExperimentDefinition(
    std::string experiments_yaml_content) {
  // Process the yaml content and add the experiment definitions to the map.
  auto result = yaml_cpp_wrapped::YamlLoadAll(experiments_yaml_content);
  if (!result.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to parse yaml: ", result.status().ToString()));
  }
  for (const auto& value : result.value()) {
    if (value.IsMap()) {
      auto key = value.begin()->first.as<std::string>();
      ExperimentDefinition experiment_definition(
          key, value["description"].as<std::string>(),
          value["owner"].as<std::string>(), value["expiry"].as<std::string>(),
          value["uses_polling"].as<bool>(),
          value["allow_in_fuzzing_config"].as<bool>(),
          value["test_tags"].as<std::vector<std::string>>());
      LOG(INFO) << "Experiment definition: " << experiment_definition.name()
                << " " << experiment_definition.description() << " "
                << experiment_definition.owner() << " "
                << experiment_definition.expiry() << " "
                << experiment_definition.uses_polling() << " "
                << experiment_definition.allow_in_fuzzing_config();
      experiment_definitions_.emplace(key, experiment_definition);
    }
  }
  return absl::OkStatus();
}

absl::Status ExperimentsCompiler::AddRolloutSpecification(
    std::string experiments_rollout_yaml_content) {
  // Process the yaml content and add the rollout specifications to the map.
  auto result = yaml_cpp_wrapped::YamlLoadAll(experiments_rollout_yaml_content);
  if (!result.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to parse yaml: ", result.status().ToString()));
  }
  for (const auto& value : result.value()) {
    if (value.IsMap()) {
      auto key = value.begin()->first.as<std::string>();
      RolloutSpecification rollout_specification;
      if (value["default_value"].IsDefined()) {
        rollout_specification = RolloutSpecification(
            key, value["default_value"].as<std::string>(),
            std::map<std::string, std::string>(), std::vector<std::string>());
      } else {
        if (!value["platform_value"].IsDefined()) {
          return absl::InvalidArgumentError(absl::StrCat(
              "No default value or platform value for rollout: ", key));
        }
        rollout_specification = RolloutSpecification(
            key, std::string(),
            value["platform_value"].as<std::map<std::string, std::string>>(),
            value["requirements"].as<std::vector<std::string>>());
      }
      experiment_definitions_[key].AddRolloutSpecification(
          allowed_defaults_, allowed_platforms_, rollout_specification);
    }
  }
  return absl::OkStatus();
}

absl::Status ExperimentsCompiler::WriteFile(const std::string& output_file,
                                            const std::string& contents) {
  std::ofstream outfile(output_file);  // Open the file for writing

  if (outfile.is_open()) {
    // Write data to the file
    outfile << contents;
    outfile.close();

    // Check if the file was closed successfully
    if (!outfile.good()) {
      LOG(ERROR) << "Error: Failed to close file: " << output_file;
      return absl::InternalError("Failed to close file: " + output_file);
    }
  } else {
    LOG(ERROR) << "Error: Failed to open file: " << output_file;
    return absl::InternalError("Failed to open file: " + output_file);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> ExperimentsCompiler::_GenerateExperimentsHdr(
    const std::string& mode) {
  std::string output;
  if (mode == "grpc_google3") {
    GrpcGoogle3ExperimentsOutputGenerator generator;
    generator.GenerateHeader(output);
  } else if (mode == "grpc_oss_production") {
    GrpcOssExperimentsOutputGenerator generator("production");
    generator.GenerateHeader(output);
  } else if (mode == "grpc_oss_test") {
    GrpcOssExperimentsOutputGenerator generator("test");
    generator.GenerateHeader(output);
  } else {
    LOG(ERROR) << "Unsupported mode: " << mode;
    return absl::InvalidArgumentError(absl::StrCat("Unsupported mode: ", mode));
  }
  return output;
}

absl::Status ExperimentsCompiler::GenerateExperimentsHdr(
    const std::string& output_file, const std::string& mode) {
  auto contents = _GenerateExperimentsHdr(mode);
  if (!contents.ok()) {
    return contents.status();
  }
  absl::Status status = WriteFile(output_file, contents.value());
  if (!status.ok()) {
    // Handle the error
    LOG(ERROR) << "Failed to write to file: " << output_file
               << " with error: " << status.message();
    return status;
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> ExperimentsCompiler::_GenerateExperimentsSrc(
    const std::string& header_file_path, const std::string& mode) {
  std::string output;
  if (mode == "grpc_google3") {
    GrpcGoogle3ExperimentsOutputGenerator generator;
    generator.GenerateSource(output);
  } else if (mode == "grpc_oss_production") {
    GrpcOssExperimentsOutputGenerator generator("production");
    generator.GenerateSource(output);
  } else if (mode == "grpc_oss_test") {
    GrpcOssExperimentsOutputGenerator generator("test");
    generator.GenerateSource(output);
  } else {
    LOG(ERROR) << "Unsupported mode: " << mode;
    return absl::InvalidArgumentError(absl::StrCat("Unsupported mode: ", mode));
  }
  return output;
}

absl::Status ExperimentsCompiler::GenerateExperimentsSrc(
    const std::string& output_file, const std::string& header_file_path,
    const std::string& mode) {
  auto contents = _GenerateExperimentsSrc(header_file_path, mode);
  if (!contents.ok()) {
    return contents.status();
  }
  absl::Status status = WriteFile(output_file, contents.value());
  if (!status.ok()) {
    // Handle the error
    LOG(ERROR) << "Failed to write to file: " << output_file
               << " with error: " << status.message();
    return status;
  }
  return absl::OkStatus();
}

void ExperimentsOutputGenerator::PutCopyright(std::string& output) {
  absl::StrAppend(&output, GetCopyright());
}

void ExperimentsOutputGenerator::PutBanner(const std::string& prefix,
                                           std::vector<std::string>& lines,
                                           std::string& output) {
  for (const auto& line : lines) {
    absl::StrAppend(&output, prefix, line, "\n");
  }
}

void GrpcGoogle3ExperimentsOutputGenerator::GenerateHeader(
    std::string& output) {
  std::vector<std::string> lines;
  lines.push_back(
      " Auto generated by tools/codegen/core/gen_experiments_grpc_google3.cc");
  lines.push_back(GetGrpcCodegenPlaceholderText());
  PutCopyright(output);
  PutBanner("//", lines, output);
  // Generate the experiment interfaces.
}

void GrpcGoogle3ExperimentsOutputGenerator::GenerateSource(
    std::string& output) {}

void GrpcOssExperimentsOutputGenerator::GenerateHeader(std::string& output) {
  std::vector<std::string> lines;
  lines.push_back(
      " Auto generated by tools/codegen/core/gen_experiments_grpc_oss.cc");
  lines.push_back(GetGrpcCodegenPlaceholderText());
  PutCopyright(output);
  PutBanner("//", lines, output);
  // Generate the experiment interfaces.
}

void GrpcOssExperimentsOutputGenerator::GenerateSource(std::string& output) {}
}  // namespace grpc_core
