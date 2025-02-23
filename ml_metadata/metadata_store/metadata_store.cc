/* Copyright 2019 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "ml_metadata/metadata_store/metadata_store.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include <glog/logging.h>
#include "google/protobuf/descriptor.h"
#include "google/protobuf/repeated_field.h"
#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "ml_metadata/metadata_store/metadata_access_object.h"
#include "ml_metadata/metadata_store/metadata_access_object_factory.h"
#include "ml_metadata/metadata_store/simple_types_util.h"
#include "ml_metadata/proto/metadata_store.pb.h"
#include "ml_metadata/proto/metadata_store_service.pb.h"
#include "ml_metadata/simple_types/proto/simple_types.pb.h"
#include "ml_metadata/simple_types/simple_types_constants.h"
#include "ml_metadata/util/return_utils.h"

namespace ml_metadata {
namespace {
using std::unique_ptr;

// Checks if the `stored_type` and `other_type` have the same names.
// In addition, it checks whether the types are inconsistent:
// a) `stored_type` and `other_type` have conflicting property value type
// b) `can_omit_fields` is false, while `stored_type`/`other_type` is not empty.
// c) `can_add_fields` is false, while `other_type`/`store_type` is not empty.
// Returns OK if the types are consistent and an output_type that contains the
// union of the properties in stored_type and other_type.
// Returns FAILED_PRECONDITION, if the types are inconsistent.
template <typename T>
absl::Status CheckFieldsConsistent(const T& stored_type, const T& other_type,
                                   bool can_add_fields, bool can_omit_fields,
                                   T& output_type) {
  if (stored_type.name() != other_type.name()) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Conflicting type name found in stored and given types: "
        "stored type: ",
        stored_type.DebugString(), "; given type: ", other_type.DebugString()));
  }
  // Make sure every property in stored_type matches with the one in other_type
  // unless can_omit_fields is set to true.
  int omitted_fields_count = 0;
  for (const auto& pair : stored_type.properties()) {
    const std::string& key = pair.first;
    const PropertyType value = pair.second;
    const auto other_iter = other_type.properties().find(key);
    if (other_iter == other_type.properties().end()) {
      omitted_fields_count++;
    } else if (other_iter->second != value) {
      return absl::FailedPreconditionError(absl::StrCat(
          "Conflicting property value type found in stored and given types: "
          "stored_type: ",
          stored_type.DebugString(),
          "; other_type: ", other_type.DebugString()));
    }
    if (omitted_fields_count > 0 && !can_omit_fields) {
      return absl::FailedPreconditionError(absl::StrCat(
          "can_omit_fields is false while stored type has more properties: "
          "stored type: ",
          stored_type.DebugString(),
          "; given type: ", other_type.DebugString()));
    }
  }
  if (stored_type.properties_size() - omitted_fields_count ==
      other_type.properties_size()) {
    output_type = stored_type;
    return absl::OkStatus();
  }
  if (!can_add_fields) {
    return absl::FailedPreconditionError(absl::StrCat(
        "can_add_fields is false while the given type has more properties: "
        "stored_type: ",
        stored_type.DebugString(), "; other_type: ", other_type.DebugString()));
  }
  // add new properties to output_types if can_add_fields is true.
  output_type = stored_type;
  for (const auto& pair : other_type.properties()) {
    const std::string& property_name = pair.first;
    const PropertyType value = pair.second;
    if (stored_type.properties().find(property_name) ==
        stored_type.properties().end()) {
      (*output_type.mutable_properties())[property_name] = value;
    }
  }
  return absl::OkStatus();
}

// Creates a type inheritance link between 'type'.base_type from request and
// type with 'type_id'.
//
// a) If 'type'.base_type = NULL in request 'type', no-op;
// b) If 'type'.base_type = UNSET in request 'type', error out as type deletion
//    is not supported yet;
// c) If more than 1 parent types are found for 'type_id', return error;
// d) If 'type'.base_type is set,
//    1) If no parent type is found for 'type_id', create a new parent
//       inheritance link;
//    2) If 1 parent type is found for 'type_id' and it is not equal to
//       'type'.base_type, error out as type update is not supported yet;
//    3) If 1 parent type is found for 'type_id' and it is equal to
//       'type'.base_type, no-op.
// TODO(b/195375645): support parent type update and deletion
template <typename T>
absl::Status UpsertTypeInheritanceLink(
    const T& type, int64 type_id,
    MetadataAccessObject* metadata_access_object) {
  if (!type.has_base_type()) return absl::OkStatus();

  SystemTypeExtension extension;
  MLMD_RETURN_IF_ERROR(GetSystemTypeExtension(type, extension));
  if (IsUnsetBaseType(extension)) {
    return absl::UnimplementedError("base_type deletion is not supported yet");
  }
  absl::flat_hash_map<int64, T> output_parent_types;
  MLMD_RETURN_IF_ERROR(metadata_access_object->FindParentTypesByTypeId(
      {type_id}, output_parent_types));

  const bool no_parent_type = !output_parent_types.contains(type_id);
  if (no_parent_type) {
    T type_with_id = type;
    type_with_id.set_id(type_id);
    T base_type;
    MLMD_RETURN_IF_ERROR(metadata_access_object->FindTypeByNameAndVersion(
        extension.type_name(), /*version=*/absl::nullopt, &base_type));
    return metadata_access_object->CreateParentTypeInheritanceLink(type_with_id,
                                                                   base_type);
  }

  if (output_parent_types[type_id].name() != extension.type_name()) {
    return absl::UnimplementedError("base_type update is not supported yet");
  }
  return absl::OkStatus();
}

// If there is no type having the same name and version, then inserts a new
// type. If a type with the same name and version already exists
// (let's call it `old_type`), it checks the consistency of `type` and
// `old_type` as described in CheckFieldsConsistent according to
// can_add_fields and can_omit_fields.
// It returns ALREADY_EXISTS if:
//  a) any property in `type` has different value from the one in `old_type`
//  b) can_add_fields = false, `type` has more properties than `old_type`
//  c) can_omit_fields = false, `type` has less properties than `old_type`
// If `type` is a valid update, then new fields in `type` are added.
// Returns INVALID_ARGUMENT error, if name field in `type` is not given.
// Returns INVALID_ARGUMENT error, if any property type in `type` is unknown.
// Returns detailed INTERNAL error, if query execution fails.
template <typename T>
absl::Status UpsertType(const T& type, bool can_add_fields,
                        bool can_omit_fields,
                        MetadataAccessObject* metadata_access_object,
                        int64* type_id) {
  T stored_type;
  const absl::Status status = metadata_access_object->FindTypeByNameAndVersion(
      type.name(), type.version(), &stored_type);
  if (!status.ok() && !absl::IsNotFound(status)) {
    return status;
  }
  // if not found, then it creates a type. `can_add_fields` is ignored.
  if (absl::IsNotFound(status)) {
    MLMD_RETURN_IF_ERROR(metadata_access_object->CreateType(type, type_id));
    return UpsertTypeInheritanceLink(type, *type_id, metadata_access_object);
  }
  // otherwise it updates the type.
  *type_id = stored_type.id();
  // all properties in stored_type must match the given type.
  // if `can_add_fields` is set, then new properties can be added
  // if `can_omit_fields` is set, then existing properties can be missing.
  T output_type;
  const absl::Status check_status = CheckFieldsConsistent(
      stored_type, type, can_add_fields, can_omit_fields, output_type);
  if (!check_status.ok()) {
    return absl::AlreadyExistsError(
        absl::StrCat("Type already exists with different properties: ",
                     std::string(check_status.message())));
  }
  MLMD_RETURN_IF_ERROR(metadata_access_object->UpdateType(output_type));
  return UpsertTypeInheritanceLink(type, *type_id, metadata_access_object);
}

// Inserts or updates all the types in the argument list. 'can_add_fields' and
// 'can_omit_fields' are both enabled. Type ids are inserted into the
// PutTypesResponse 'response'.
absl::Status UpsertTypes(
    const google::protobuf::RepeatedPtrField<ArtifactType>& artifact_types,
    const google::protobuf::RepeatedPtrField<ExecutionType>& execution_types,
    const google::protobuf::RepeatedPtrField<ContextType>& context_types,
    const bool can_add_fields, const bool can_omit_fields,
    MetadataAccessObject* metadata_access_object, PutTypesResponse* response) {
  for (const ArtifactType& artifact_type : artifact_types) {
    int64 artifact_type_id;
    MLMD_RETURN_IF_ERROR(UpsertType(artifact_type, can_add_fields,
                                    can_omit_fields, metadata_access_object,
                                    &artifact_type_id));
    response->add_artifact_type_ids(artifact_type_id);
  }
  for (const ExecutionType& execution_type : execution_types) {
    int64 execution_type_id;
    MLMD_RETURN_IF_ERROR(UpsertType(execution_type, can_add_fields,
                                    can_omit_fields, metadata_access_object,
                                    &execution_type_id));
    response->add_execution_type_ids(execution_type_id);
  }
  for (const ContextType& context_type : context_types) {
    int64 context_type_id;
    MLMD_RETURN_IF_ERROR(UpsertType(context_type, can_add_fields,
                                    can_omit_fields, metadata_access_object,
                                    &context_type_id));
    response->add_context_type_ids(context_type_id);
  }
  return absl::OkStatus();
}

// Loads SimpleTypes proto from string and updates or inserts it into database.
absl::Status UpsertSimpleTypes(MetadataAccessObject* metadata_access_object) {
  SimpleTypes simple_types;
  PutTypesResponse response;
  MLMD_RETURN_IF_ERROR(LoadSimpleTypes(simple_types));
  return UpsertTypes(
      simple_types.artifact_types(), simple_types.execution_types(),
      simple_types.context_types(), /*can_add_fields=*/true,
      /*can_omit_fields=*/true, metadata_access_object, &response);
}

// Updates or inserts an artifact. If the artifact.id is given, it updates the
// stored artifact, otherwise, it creates a new artifact.
absl::Status UpsertArtifact(const Artifact& artifact,
                            MetadataAccessObject* metadata_access_object,
                            int64* artifact_id) {
  CHECK(artifact_id) << "artifact_id should not be null";
  if (artifact.has_id()) {
    MLMD_RETURN_IF_ERROR(metadata_access_object->UpdateArtifact(artifact));
    *artifact_id = artifact.id();
  } else {
    MLMD_RETURN_IF_ERROR(
        metadata_access_object->CreateArtifact(artifact, artifact_id));
  }
  return absl::OkStatus();
}

// Updates or inserts an execution. If the execution.id is given, it updates the
// stored execution, otherwise, it creates a new execution.
absl::Status UpsertExecution(const Execution& execution,
                             MetadataAccessObject* metadata_access_object,
                             int64* execution_id) {
  CHECK(execution_id) << "execution_id should not be null";
  if (execution.has_id()) {
    MLMD_RETURN_IF_ERROR(metadata_access_object->UpdateExecution(execution));
    *execution_id = execution.id();
  } else {
    MLMD_RETURN_IF_ERROR(
        metadata_access_object->CreateExecution(execution, execution_id));
  }
  return absl::OkStatus();
}

// Updates or inserts a context. If the context.id is given, it updates the
// stored context, otherwise, it creates a new context.
absl::Status UpsertContext(const Context& context,
                           MetadataAccessObject* metadata_access_object,
                           int64* context_id) {
  CHECK(context_id) << "context_id should not be null";
  if (context.has_id()) {
    MLMD_RETURN_IF_ERROR(metadata_access_object->UpdateContext(context));
    *context_id = context.id();
  } else {
    MLMD_RETURN_IF_ERROR(
        metadata_access_object->CreateContext(context, context_id));
  }
  return absl::OkStatus();
}

// Inserts an association. If the association already exists it returns OK.
absl::Status InsertAssociationIfNotExist(
    int64 context_id, int64 execution_id,
    MetadataAccessObject* metadata_access_object) {
  Association association;
  association.set_execution_id(execution_id);
  association.set_context_id(context_id);
  int64 dummy_assocation_id;
  absl::Status status = metadata_access_object->CreateAssociation(
      association, &dummy_assocation_id);
  if (!status.ok() && !absl::IsAlreadyExists(status)) {
    return status;
  }
  return absl::OkStatus();
}

// Inserts an attribution. If the attribution already exists it returns OK.
absl::Status InsertAttributionIfNotExist(
    int64 context_id, int64 artifact_id,
    MetadataAccessObject* metadata_access_object) {
  Attribution attribution;
  attribution.set_artifact_id(artifact_id);
  attribution.set_context_id(context_id);
  int64 dummy_attribution_id;
  absl::Status status = metadata_access_object->CreateAttribution(
      attribution, &dummy_attribution_id);
  if (!status.ok() && !absl::IsAlreadyExists(status)) {
    return status;
  }
  return absl::OkStatus();
}

// Updates or inserts a pair of {Artifact, Event}. If artifact is not given,
// the event.artifact_id must exist, and it inserts the event, and returns the
// artifact_id. Otherwise if artifact is given, event.artifact_id is optional,
// if set, then artifact.id and event.artifact_id must align.
absl::Status UpsertArtifactAndEvent(
    const PutExecutionRequest::ArtifactAndEvent& artifact_and_event,
    MetadataAccessObject* metadata_access_object, int64* artifact_id) {
  CHECK(artifact_id) << "The output artifact_id pointer should not be null";
  if (!artifact_and_event.has_artifact() && !artifact_and_event.has_event()) {
    return absl::OkStatus();
  }
  // validate event and artifact's id aligns.
  // if artifact is not given, the event.artifact_id must exist
  absl::optional<int64> maybe_event_artifact_id =
      artifact_and_event.has_event() &&
              artifact_and_event.event().has_artifact_id()
          ? absl::make_optional<int64>(artifact_and_event.event().artifact_id())
          : absl::nullopt;
  if (!artifact_and_event.has_artifact() && !maybe_event_artifact_id) {
    return absl::InvalidArgumentError(absl::StrCat(
        "If no artifact is present, given event must have an artifact_id: ",
        artifact_and_event.DebugString()));
  }
  // if artifact and event.artifact_id is given, then artifact.id and
  // event.artifact_id must align.
  absl::optional<int64> maybe_artifact_id =
      artifact_and_event.has_artifact() &&
              artifact_and_event.artifact().has_id()
          ? absl::make_optional<int64>(artifact_and_event.artifact().id())
          : absl::nullopt;
  if (artifact_and_event.has_artifact() && maybe_event_artifact_id &&
      maybe_artifact_id != maybe_event_artifact_id) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Given event.artifact_id is not aligned with the artifact: ",
        artifact_and_event.DebugString()));
  }
  // upsert artifact if present.
  if (artifact_and_event.has_artifact()) {
    MLMD_RETURN_IF_ERROR(UpsertArtifact(artifact_and_event.artifact(),
                                        metadata_access_object, artifact_id));
  }
  // insert event if any.
  if (!artifact_and_event.has_event()) {
    return absl::OkStatus();
  }
  Event event = artifact_and_event.event();
  if (artifact_and_event.has_artifact()) {
    event.set_artifact_id(*artifact_id);
  } else {
    *artifact_id = event.artifact_id();
  }
  int64 dummy_event_id = -1;
  return metadata_access_object->CreateEvent(event, &dummy_event_id);
}

// A util to handle type_version in type read/write API requests.
template <typename T>
absl::optional<std::string> GetRequestTypeVersion(const T& type_request) {
  return type_request.has_type_version() && !type_request.type_version().empty()
             ? absl::make_optional(type_request.type_version())
             : absl::nullopt;
}

// Sets base_type field in `type` with its parent type queried from ParentType
// table.
// Returns FAILED_PRECONDITION if there are more than 1 system type.
// TODO(b/153373285): consider moving it to FindTypesFromRecordSet at MAO layer
template <typename T, typename ST>
absl::Status SetBaseType(absl::Span<T* const> types,
                         MetadataAccessObject* metadata_access_object) {
  if (types.empty()) return absl::OkStatus();
  absl::flat_hash_map<int64, T> output_parent_types;
  std::vector<int64> type_ids;
  absl::c_transform(types, std::back_inserter(type_ids),
                    [](const T* type) { return type->id(); });
  MLMD_RETURN_IF_ERROR(metadata_access_object->FindParentTypesByTypeId(
      type_ids, output_parent_types));

  for (T* type : types) {
    if (output_parent_types.find(type->id()) == output_parent_types.end())
      continue;
    auto parent_type = output_parent_types[type->id()];
    SystemTypeExtension extension;
    extension.set_type_name(parent_type.name());
    ST type_enum;
    MLMD_RETURN_IF_ERROR(GetSystemTypeEnum(extension, type_enum));
    type->set_base_type(type_enum);
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status MetadataStore::InitMetadataStore() {
  MLMD_RETURN_IF_ERROR(transaction_executor_->Execute([this]() -> absl::Status {
    return metadata_access_object_->InitMetadataSource();
  }));
  return transaction_executor_->Execute([this]() -> absl::Status {
    return UpsertSimpleTypes(metadata_access_object_.get());
  });
}

// TODO(b/187357155): duplicated results when inserting simple types
// concurrently
absl::Status MetadataStore::InitMetadataStoreIfNotExists(
    const bool enable_upgrade_migration) {
  MLMD_RETURN_IF_ERROR(transaction_executor_->Execute(
      [this, &enable_upgrade_migration]() -> absl::Status {
        return metadata_access_object_->InitMetadataSourceIfNotExists(
            enable_upgrade_migration);
      }));
  return transaction_executor_->Execute([this]() -> absl::Status {
    return UpsertSimpleTypes(metadata_access_object_.get());
  });
}



absl::Status MetadataStore::PutTypes(const PutTypesRequest& request,
                                     PutTypesResponse* response) {
  if (!request.all_fields_match()) {
    return absl::UnimplementedError("Must match all fields.");
  }
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        return UpsertTypes(request.artifact_types(), request.execution_types(),
                           request.context_types(), request.can_add_fields(),
                           request.can_omit_fields(),
                           metadata_access_object_.get(), response);
      },
      request.transaction_options());
}

absl::Status MetadataStore::PutArtifactType(
    const PutArtifactTypeRequest& request, PutArtifactTypeResponse* response) {
  if (!request.all_fields_match()) {
    return absl::UnimplementedError("Must match all fields.");
  }
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 type_id;
        MLMD_RETURN_IF_ERROR(
            UpsertType(request.artifact_type(), request.can_add_fields(),
                       request.can_omit_fields(), metadata_access_object_.get(),
                       &type_id));
        response->set_type_id(type_id);
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::PutExecutionType(
    const PutExecutionTypeRequest& request,
    PutExecutionTypeResponse* response) {
  if (!request.all_fields_match()) {
    return absl::UnimplementedError("Must match all fields.");
  }
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 type_id;
        MLMD_RETURN_IF_ERROR(
            UpsertType(request.execution_type(), request.can_add_fields(),
                       request.can_omit_fields(), metadata_access_object_.get(),
                       &type_id));
        response->set_type_id(type_id);
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::PutContextType(const PutContextTypeRequest& request,
                                           PutContextTypeResponse* response) {
  if (!request.all_fields_match()) {
    return absl::UnimplementedError("Must match all fields.");
  }
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 type_id;
        MLMD_RETURN_IF_ERROR(
            UpsertType(request.context_type(), request.can_add_fields(),
                       request.can_omit_fields(), metadata_access_object_.get(),
                       &type_id));
        response->set_type_id(type_id);
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifactType(
    const GetArtifactTypeRequest& request, GetArtifactTypeResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        ArtifactType type;
        MLMD_RETURN_IF_ERROR(metadata_access_object_->FindTypeByNameAndVersion(
            request.type_name(), GetRequestTypeVersion(request), &type));
        std::vector<ArtifactType*> types({&type});
        MLMD_RETURN_IF_ERROR(
            SetBaseType<ArtifactType, ArtifactType::SystemDefinedBaseType>(
                absl::MakeSpan(types), metadata_access_object_.get()));
        *response->mutable_artifact_type() = type;
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetExecutionType(
    const GetExecutionTypeRequest& request,
    GetExecutionTypeResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        ExecutionType type;
        MLMD_RETURN_IF_ERROR(metadata_access_object_->FindTypeByNameAndVersion(
            request.type_name(), GetRequestTypeVersion(request), &type));
        std::vector<ExecutionType*> types({&type});
        MLMD_RETURN_IF_ERROR(
            SetBaseType<ExecutionType, ExecutionType::SystemDefinedBaseType>(
                absl::MakeSpan(types), metadata_access_object_.get()));
        *response->mutable_execution_type() = type;
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContextType(const GetContextTypeRequest& request,
                                           GetContextTypeResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        return metadata_access_object_->FindTypeByNameAndVersion(
            request.type_name(), GetRequestTypeVersion(request),
            response->mutable_context_type());
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifactTypesByID(
    const GetArtifactTypesByIDRequest& request,
    GetArtifactTypesByIDResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        for (const int64 type_id : request.type_ids()) {
          ArtifactType artifact_type;
          // TODO(b/218884256): replace FindTypeById with FindTypesById.
          const absl::Status status =
              metadata_access_object_->FindTypeById(type_id, &artifact_type);
          if (status.ok()) {
            *response->mutable_artifact_types()->Add() = artifact_type;
          } else if (!absl::IsNotFound(status)) {
            return status;
          }
        }
        MLMD_RETURN_IF_ERROR(
            SetBaseType<ArtifactType, ArtifactType::SystemDefinedBaseType>(
                absl::MakeSpan(const_cast<ArtifactType* const*>(
                                   response->mutable_artifact_types()->data()),
                               response->artifact_types_size()),
                metadata_access_object_.get()));
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetExecutionTypesByID(
    const GetExecutionTypesByIDRequest& request,
    GetExecutionTypesByIDResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        for (const int64 type_id : request.type_ids()) {
          ExecutionType execution_type;
          const absl::Status status =
              metadata_access_object_->FindTypeById(type_id, &execution_type);
          if (status.ok()) {
            *response->mutable_execution_types()->Add() = execution_type;
          } else if (!absl::IsNotFound(status)) {
            return status;
          }
        }
        MLMD_RETURN_IF_ERROR(
            SetBaseType<ExecutionType, ExecutionType::SystemDefinedBaseType>(
                absl::MakeSpan(const_cast<ExecutionType* const*>(
                                   response->mutable_execution_types()->data()),
                               response->execution_types_size()),
                metadata_access_object_.get()));
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContextTypesByID(
    const GetContextTypesByIDRequest& request,
    GetContextTypesByIDResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        for (const int64 type_id : request.type_ids()) {
          ContextType context_type;
          const absl::Status status =
              metadata_access_object_->FindTypeById(type_id, &context_type);
          if (status.ok()) {
            *response->mutable_context_types()->Add() = context_type;
          } else if (!absl::IsNotFound(status)) {
            return status;
          }
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifactsByID(
    const GetArtifactsByIDRequest& request,
    GetArtifactsByIDResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Artifact> artifacts;
        const std::vector<int64> ids(request.artifact_ids().begin(),
                                     request.artifact_ids().end());
        const absl::Status status =
            metadata_access_object_->FindArtifactsById(ids, &artifacts);
        if (!status.ok() && !absl::IsNotFound(status)) {
          return status;
        }
        absl::c_copy(artifacts, google::protobuf::RepeatedPtrFieldBackInserter(
                                    response->mutable_artifacts()));
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetExecutionsByID(
    const GetExecutionsByIDRequest& request,
    GetExecutionsByIDResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Execution> executions;
        const std::vector<int64> ids(request.execution_ids().begin(),
                                     request.execution_ids().end());
        const absl::Status status =
            metadata_access_object_->FindExecutionsById(ids, &executions);
        if (!status.ok() && !absl::IsNotFound(status)) {
          return status;
        }
        absl::c_copy(executions, google::protobuf::RepeatedPtrFieldBackInserter(
                                     response->mutable_executions()));
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContextsByID(
    const GetContextsByIDRequest& request, GetContextsByIDResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Context> contexts;
        const std::vector<int64> ids(request.context_ids().begin(),
                                     request.context_ids().end());
        const absl::Status status =
            metadata_access_object_->FindContextsById(ids, &contexts);
        if (!status.ok() && !absl::IsNotFound(status)) {
          return status;
        }
        absl::c_copy(contexts, google::protobuf::RepeatedFieldBackInserter(
                                   response->mutable_contexts()));
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::PutArtifacts(const PutArtifactsRequest& request,
                                         PutArtifactsResponse* response) {
  return transaction_executor_->Execute([this, &request,
                                         &response]() -> absl::Status {
    response->Clear();
    for (const Artifact& artifact : request.artifacts()) {
      int64 artifact_id = -1;
      // Verify the latest_updated_time before upserting the artifact.
      if (artifact.has_id() &&
          request.options().abort_if_latest_updated_time_changed()) {
        Artifact existing_artifact;
        absl::Status status;
        {
          std::vector<Artifact> artifacts;
          status = metadata_access_object_->FindArtifactsById({artifact.id()},
                                                              &artifacts);
          if (status.ok()) {
            existing_artifact = artifacts.at(0);
          }
        }
        if (!absl::IsNotFound(status)) {
          MLMD_RETURN_IF_ERROR(status);
          if (artifact.last_update_time_since_epoch() !=
              existing_artifact.last_update_time_since_epoch()) {
            return absl::FailedPreconditionError(absl::StrCat(
                "`abort_if_latest_updated_time_changed` is set, and the stored "
                "artifact with id = ",
                artifact.id(),
                " has a different last_update_time_since_epoch: ",
                existing_artifact.last_update_time_since_epoch(),
                " from the one in the given artifact: ",
                artifact.last_update_time_since_epoch()));
          }
          // If set the option and all check succeeds, we make sure the
          // timestamp after the update increases.
          absl::SleepFor(absl::Milliseconds(1));
        }
      }
      MLMD_RETURN_IF_ERROR(UpsertArtifact(
          artifact, metadata_access_object_.get(), &artifact_id));
      response->add_artifact_ids(artifact_id);
    }
    return absl::OkStatus();
  },
  request.transaction_options());
}

absl::Status MetadataStore::PutExecutions(const PutExecutionsRequest& request,
                                          PutExecutionsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        for (const Execution& execution : request.executions()) {
          int64 execution_id = -1;
          MLMD_RETURN_IF_ERROR(UpsertExecution(
              execution, metadata_access_object_.get(), &execution_id));
          response->add_execution_ids(execution_id);
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::PutContexts(const PutContextsRequest& request,
                                        PutContextsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        for (const Context& context : request.contexts()) {
          int64 context_id = -1;
          MLMD_RETURN_IF_ERROR(UpsertContext(
              context, metadata_access_object_.get(), &context_id));
          response->add_context_ids(context_id);
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::Create(
    const MetadataSourceQueryConfig& query_config,
    const MigrationOptions& migration_options,
    unique_ptr<MetadataSource> metadata_source,
    unique_ptr<TransactionExecutor> transaction_executor,
    unique_ptr<MetadataStore>* result) {
  unique_ptr<MetadataAccessObject> metadata_access_object;
  MLMD_RETURN_IF_ERROR(CreateMetadataAccessObject(
      query_config, metadata_source.get(), &metadata_access_object));
  // if downgrade migration is specified
  if (migration_options.downgrade_to_schema_version() >= 0) {
    MLMD_RETURN_IF_ERROR(transaction_executor->Execute(
        [&migration_options, &metadata_access_object]() -> absl::Status {
          return metadata_access_object->DowngradeMetadataSource(
              migration_options.downgrade_to_schema_version());
        }));
    return absl::CancelledError(absl::StrCat(
        "Downgrade migration was performed. Connection to the downgraded "
        "database is Cancelled. Now the database is at schema version ",
        migration_options.downgrade_to_schema_version(),
        ". Please refer to the migration guide and use lower version of the "
        "library to connect to the metadata store."));
  }
  *result = absl::WrapUnique(new MetadataStore(
      std::move(metadata_source), std::move(metadata_access_object),
      std::move(transaction_executor)));
  return absl::OkStatus();
}

absl::Status MetadataStore::PutEvents(const PutEventsRequest& request,
                                      PutEventsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        for (const Event& event : request.events()) {
          int64 dummy_event_id = -1;
          MLMD_RETURN_IF_ERROR(
              metadata_access_object_->CreateEvent(event, &dummy_event_id));
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::PutExecution(const PutExecutionRequest& request,
                                         PutExecutionResponse* response) {
  return transaction_executor_->Execute([this, &request,
                                         &response]() -> absl::Status {
    response->Clear();
    if (!request.has_execution()) {
      return absl::InvalidArgumentError(
          absl::StrCat("No execution is found: ", request.DebugString()));
    }
    // 1. Upsert Execution
    const Execution& execution = request.execution();
    int64 execution_id = -1;
    MLMD_RETURN_IF_ERROR(UpsertExecution(
        execution, metadata_access_object_.get(), &execution_id));
    response->set_execution_id(execution_id);
    // 2. Upsert Artifacts and insert events
    for (PutExecutionRequest::ArtifactAndEvent artifact_and_event :
         request.artifact_event_pairs()) {
      // validate execution and event if given
      if (artifact_and_event.has_event()) {
        Event* event = artifact_and_event.mutable_event();
        if (event->has_execution_id() &&
            (!execution.has_id() || execution.id() != event->execution_id())) {
          return absl::InvalidArgumentError(absl::StrCat(
              "Request's event.execution_id does not match with the given "
              "execution: ",
              request.DebugString()));
        }
        event->set_execution_id(execution_id);
      }
      int64 artifact_id = -1;
      MLMD_RETURN_IF_ERROR(UpsertArtifactAndEvent(
          artifact_and_event, metadata_access_object_.get(), &artifact_id));
      response->add_artifact_ids(artifact_id);
    }
    // 3. Upsert contexts and insert associations and attributions.
    for (const Context& context : request.contexts()) {
      int64 context_id = -1;
      // Try to reuse existing context if the options is set.
      if (request.options().reuse_context_if_already_exist() &&
          !context.has_id()) {
        Context existing_context;
        const absl::Status status =
            metadata_access_object_->FindContextByTypeIdAndContextName(
                context.type_id(), context.name(), &existing_context);
        if (!absl::IsNotFound(status)) {
          MLMD_RETURN_IF_ERROR(status);
          context_id = existing_context.id();
        }
      }
      if (context_id == -1) {
        const absl::Status status =
            UpsertContext(context, metadata_access_object_.get(), &context_id);
        // When `reuse_context_if_already_exist`, there are concurrent timelines
        // to create the same new context. If use the option, let client side
        // to retry the failed transaction safely.
        if (request.options().reuse_context_if_already_exist() &&
            absl::IsAlreadyExists(status)) {
          return absl::AbortedError(absl::StrCat(
              "Concurrent creation of the same context at the first time. "
              "Retry the transaction to reuse the context: ",
              context.DebugString()));
        }
        MLMD_RETURN_IF_ERROR(status);
      }
      response->add_context_ids(context_id);
      MLMD_RETURN_IF_ERROR(InsertAssociationIfNotExist(
          context_id, response->execution_id(), metadata_access_object_.get()));
      for (const int64 artifact_id : response->artifact_ids()) {
        MLMD_RETURN_IF_ERROR(InsertAttributionIfNotExist(
            context_id, artifact_id, metadata_access_object_.get()));
      }
    }
    return absl::OkStatus();
  },
  request.transaction_options());
}


absl::Status MetadataStore::GetEventsByExecutionIDs(
    const GetEventsByExecutionIDsRequest& request,
    GetEventsByExecutionIDsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Event> events;
        const absl::Status status =
            metadata_access_object_->FindEventsByExecutions(
                std::vector<int64>(request.execution_ids().begin(),
                                   request.execution_ids().end()),
                &events);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        for (const Event& event : events) {
          *response->mutable_events()->Add() = event;
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetEventsByArtifactIDs(
    const GetEventsByArtifactIDsRequest& request,
    GetEventsByArtifactIDsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Event> events;
        const absl::Status status =
            metadata_access_object_->FindEventsByArtifacts(
                std::vector<int64>(request.artifact_ids().begin(),
                                   request.artifact_ids().end()),
                &events);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        for (const Event& event : events) {
          *response->mutable_events()->Add() = event;
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetExecutions(const GetExecutionsRequest& request,
                                          GetExecutionsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Execution> executions;
        absl::Status status;
        std::string next_page_token;
        if (request.has_options()) {
          status = metadata_access_object_->ListExecutions(
              request.options(), &executions, &next_page_token);
        } else {
          status = metadata_access_object_->FindExecutions(&executions);
        }

        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }

        for (const Execution& execution : executions) {
          *response->mutable_executions()->Add() = execution;
        }

        if (!next_page_token.empty()) {
          response->set_next_page_token(next_page_token);
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifacts(const GetArtifactsRequest& request,
                                         GetArtifactsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Artifact> artifacts;
        absl::Status status;
        std::string next_page_token;
        if (request.has_options()) {
          status = metadata_access_object_->ListArtifacts(
              request.options(), &artifacts, &next_page_token);
        } else {
          status = metadata_access_object_->FindArtifacts(&artifacts);
        }

        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }

        for (const Artifact& artifact : artifacts) {
          *response->mutable_artifacts()->Add() = artifact;
        }

        if (!next_page_token.empty()) {
          response->set_next_page_token(next_page_token);
        }

        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContexts(const GetContextsRequest& request,
                                        GetContextsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Context> contexts;
        absl::Status status;
        std::string next_page_token;
        if (request.has_options()) {
          status = metadata_access_object_->ListContexts(
              request.options(), &contexts, &next_page_token);
        } else {
          status = metadata_access_object_->FindContexts(&contexts);
        }

        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }

        for (const Context& context : contexts) {
          *response->mutable_contexts()->Add() = context;
        }

        if (!next_page_token.empty()) {
          response->set_next_page_token(next_page_token);
        }

        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifactTypes(
    const GetArtifactTypesRequest& request,
    GetArtifactTypesResponse* response) {
  return transaction_executor_->Execute(
      [this, &response]() -> absl::Status {
        response->Clear();
        std::vector<ArtifactType> artifact_types;
        const absl::Status status =
            metadata_access_object_->FindTypes(&artifact_types);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        absl::c_copy_if(
            artifact_types,
            google::protobuf::RepeatedFieldBackInserter(
                response->mutable_artifact_types()),
            [](const ArtifactType& type) {
              // Simple types will not be returned by Get*Types APIs
              // because they are invisible to users.
              return std::find(kSimpleTypeNames.begin(), kSimpleTypeNames.end(),
                               type.name()) == kSimpleTypeNames.end();
            });
        MLMD_RETURN_IF_ERROR(
            SetBaseType<ArtifactType, ArtifactType::SystemDefinedBaseType>(
                absl::MakeSpan(const_cast<ArtifactType* const*>(
                                   response->mutable_artifact_types()->data()),
                               response->artifact_types_size()),
                metadata_access_object_.get()));
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetExecutionTypes(
    const GetExecutionTypesRequest& request,
    GetExecutionTypesResponse* response) {
  return transaction_executor_->Execute(
      [this, &response]() -> absl::Status {
        response->Clear();
        std::vector<ExecutionType> execution_types;
        const absl::Status status =
            metadata_access_object_->FindTypes(&execution_types);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        absl::c_copy_if(
            execution_types,
            google::protobuf::RepeatedFieldBackInserter(
                response->mutable_execution_types()),
            [](const ExecutionType& type) {
              // Simple types will not be returned by Get*Types APIs
              // because they are invisible to users.
              return std::find(kSimpleTypeNames.begin(), kSimpleTypeNames.end(),
                               type.name()) == kSimpleTypeNames.end();
            });
        MLMD_RETURN_IF_ERROR(
            SetBaseType<ExecutionType, ExecutionType::SystemDefinedBaseType>(
                absl::MakeSpan(const_cast<ExecutionType* const*>(
                                   response->mutable_execution_types()->data()),
                               response->execution_types_size()),
                metadata_access_object_.get()));
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContextTypes(
    const GetContextTypesRequest& request, GetContextTypesResponse* response) {
  return transaction_executor_->Execute(
      [this, &response]() -> absl::Status {
        response->Clear();
        std::vector<ContextType> context_types;
        const absl::Status status =
            metadata_access_object_->FindTypes(&context_types);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        for (const ContextType& context_type : context_types) {
          *response->mutable_context_types()->Add() = context_type;
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifactsByURI(
    const GetArtifactsByURIRequest& request,
    GetArtifactsByURIResponse* response) {
  // Validate if there's already deprecated optional string uri = 1 field.
  const google::protobuf::UnknownFieldSet& unknown_field_set =
      request.GetReflection()->GetUnknownFields(request);
  for (int i = 0; i < unknown_field_set.field_count(); i++) {
    const google::protobuf::UnknownField& unknown_field = unknown_field_set.field(i);
    if (unknown_field.number() == 1) {
      return absl::InvalidArgumentError(absl::StrCat(
          "The request contains deprecated field `uri`. Please upgrade the "
          "client library version above 0.21.0. GetArtifactsByURIRequest: ",
          request.DebugString()));
    }
  }
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        absl::flat_hash_set<std::string> uris(request.uris().begin(),
                                              request.uris().end());
        for (const std::string& uri : uris) {
          std::vector<Artifact> artifacts;
          const absl::Status status =
              metadata_access_object_->FindArtifactsByURI(uri, &artifacts);
          if (!status.ok() && !absl::IsNotFound(status)) {
            // If any none NotFound error returned, we do early stopping as
            // the query execution has internal db errors.
            return status;
          }
          for (const Artifact& artifact : artifacts) {
            *response->mutable_artifacts()->Add() = artifact;
          }
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifactsByType(
    const GetArtifactsByTypeRequest& request,
    GetArtifactsByTypeResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 artifact_type_id;
        absl::Status status =
            metadata_access_object_->FindTypeIdByNameAndVersion(
                request.type_name(), GetRequestTypeVersion(request),
                TypeKind::ARTIFACT_TYPE, &artifact_type_id);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        std::vector<Artifact> artifacts;
        std::string next_page_token;
        status = metadata_access_object_->FindArtifactsByTypeId(
            artifact_type_id,
            (request.has_options() ? absl::make_optional(request.options())
                                   : absl::nullopt),
            &artifacts, &next_page_token);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        for (const Artifact& artifact : artifacts) {
          *response->mutable_artifacts()->Add() = artifact;
        }
        if (request.has_options()) {
          response->set_next_page_token(next_page_token);
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifactByTypeAndName(
    const GetArtifactByTypeAndNameRequest& request,
    GetArtifactByTypeAndNameResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 artifact_type_id;
        absl::Status status =
            metadata_access_object_->FindTypeIdByNameAndVersion(
                request.type_name(), GetRequestTypeVersion(request),
                TypeKind::ARTIFACT_TYPE, &artifact_type_id);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        Artifact artifact;
        status = metadata_access_object_->FindArtifactByTypeIdAndArtifactName(
            artifact_type_id, request.artifact_name(), &artifact);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        *response->mutable_artifact() = artifact;
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetExecutionsByType(
    const GetExecutionsByTypeRequest& request,
    GetExecutionsByTypeResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 execution_type_id;
        absl::Status status =
            metadata_access_object_->FindTypeIdByNameAndVersion(
                request.type_name(), GetRequestTypeVersion(request),
                TypeKind::EXECUTION_TYPE, &execution_type_id);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        std::vector<Execution> executions;
        std::string next_page_token;
        status = metadata_access_object_->FindExecutionsByTypeId(
            execution_type_id,
            (request.has_options() ? absl::make_optional(request.options())
                                   : absl::nullopt),
            &executions, &next_page_token);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        for (const Execution& execution : executions) {
          *response->mutable_executions()->Add() = execution;
        }
        if (request.has_options()) {
          response->set_next_page_token(next_page_token);
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetExecutionByTypeAndName(
    const GetExecutionByTypeAndNameRequest& request,
    GetExecutionByTypeAndNameResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 execution_type_id;
        absl::Status status =
            metadata_access_object_->FindTypeIdByNameAndVersion(
                request.type_name(), GetRequestTypeVersion(request),
                TypeKind::EXECUTION_TYPE, &execution_type_id);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        Execution execution;
        status = metadata_access_object_->FindExecutionByTypeIdAndExecutionName(
            execution_type_id, request.execution_name(), &execution);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        *response->mutable_execution() = execution;
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContextsByType(
    const GetContextsByTypeRequest& request,
    GetContextsByTypeResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 context_type_id;
        {
          absl::Status status =
              metadata_access_object_->FindTypeIdByNameAndVersion(
                  request.type_name(), GetRequestTypeVersion(request),
                  TypeKind::CONTEXT_TYPE, &context_type_id);
          if (absl::IsNotFound(status)) {
            return absl::OkStatus();
          } else if (!status.ok()) {
            return status;
          }
        }
        std::vector<Context> contexts;
        std::string next_page_token;
        {
          absl::Status status;
          status = metadata_access_object_->FindContextsByTypeId(
              context_type_id,
              (request.has_options() ? absl::make_optional(request.options())
                                     : absl::nullopt),
              &contexts, &next_page_token);
          if (absl::IsNotFound(status)) {
            return absl::OkStatus();
          } else if (!status.ok()) {
            return status;
          }
        }
        for (const Context& context : contexts) {
          *response->mutable_contexts()->Add() = context;
        }
        if (request.has_options()) {
          response->set_next_page_token(next_page_token);
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContextByTypeAndName(
    const GetContextByTypeAndNameRequest& request,
    GetContextByTypeAndNameResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        int64 context_type_id;
        absl::Status status =
            metadata_access_object_->FindTypeIdByNameAndVersion(
                request.type_name(), GetRequestTypeVersion(request),
                TypeKind::CONTEXT_TYPE, &context_type_id);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        Context context;
        status = metadata_access_object_->FindContextByTypeIdAndContextName(
            context_type_id, request.context_name(), &context);
        if (absl::IsNotFound(status)) {
          return absl::OkStatus();
        } else if (!status.ok()) {
          return status;
        }
        *response->mutable_context() = context;
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::PutAttributionsAndAssociations(
    const PutAttributionsAndAssociationsRequest& request,
    PutAttributionsAndAssociationsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        for (const Attribution& attribution : request.attributions()) {
          MLMD_RETURN_IF_ERROR(InsertAttributionIfNotExist(
              attribution.context_id(), attribution.artifact_id(),
              metadata_access_object_.get()));
        }
        for (const Association& association : request.associations()) {
          MLMD_RETURN_IF_ERROR(InsertAssociationIfNotExist(
              association.context_id(), association.execution_id(),
              metadata_access_object_.get()));
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::PutParentContexts(
    const PutParentContextsRequest& request,
    PutParentContextsResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        for (const ParentContext& parent_context : request.parent_contexts()) {
          MLMD_RETURN_IF_ERROR(
              metadata_access_object_->CreateParentContext(parent_context));
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContextsByArtifact(
    const GetContextsByArtifactRequest& request,
    GetContextsByArtifactResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Context> contexts;
        MLMD_RETURN_IF_ERROR(metadata_access_object_->FindContextsByArtifact(
            request.artifact_id(), &contexts));
        for (const Context& context : contexts) {
          *response->mutable_contexts()->Add() = context;
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetContextsByExecution(
    const GetContextsByExecutionRequest& request,
    GetContextsByExecutionResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Context> contexts;
        MLMD_RETURN_IF_ERROR(metadata_access_object_->FindContextsByExecution(
            request.execution_id(), &contexts));
        for (const Context& context : contexts) {
          *response->mutable_contexts()->Add() = context;
        }
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetArtifactsByContext(
    const GetArtifactsByContextRequest& request,
    GetArtifactsByContextResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Artifact> artifacts;
        std::string next_page_token;
        auto list_options = request.has_options()
                                ? absl::make_optional(request.options())
                                : absl::nullopt;
        MLMD_RETURN_IF_ERROR(metadata_access_object_->FindArtifactsByContext(
            request.context_id(), list_options, &artifacts, &next_page_token));

        for (const Artifact& artifact : artifacts) {
          *response->mutable_artifacts()->Add() = artifact;
        }

        if (!next_page_token.empty()) {
          response->set_next_page_token(next_page_token);
        }

        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetExecutionsByContext(
    const GetExecutionsByContextRequest& request,
    GetExecutionsByContextResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Execution> executions;
        std::string next_page_token;
        auto list_options = request.has_options()
                                ? absl::make_optional(request.options())
                                : absl::nullopt;

        MLMD_RETURN_IF_ERROR(metadata_access_object_->FindExecutionsByContext(
            request.context_id(), list_options, &executions, &next_page_token));

        for (const Execution& execution : executions) {
          *response->mutable_executions()->Add() = execution;
        }

        if (!next_page_token.empty()) {
          response->set_next_page_token(next_page_token);
        }

        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetParentContextsByContext(
    const GetParentContextsByContextRequest& request,
    GetParentContextsByContextResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Context> parent_contexts;
        const absl::Status status =
            metadata_access_object_->FindParentContextsByContextId(
                request.context_id(), &parent_contexts);
        if (!status.ok() && !absl::IsNotFound(status)) {
          return status;
        }
        absl::c_copy(parent_contexts, google::protobuf::RepeatedPtrFieldBackInserter(
                                          response->mutable_contexts()));
        return absl::OkStatus();
      },
      request.transaction_options());
}

absl::Status MetadataStore::GetChildrenContextsByContext(
    const GetChildrenContextsByContextRequest& request,
    GetChildrenContextsByContextResponse* response) {
  return transaction_executor_->Execute(
      [this, &request, &response]() -> absl::Status {
        response->Clear();
        std::vector<Context> child_contexts;
        const absl::Status status =
            metadata_access_object_->FindChildContextsByContextId(
                request.context_id(), &child_contexts);
        if (!status.ok() && !absl::IsNotFound(status)) {
          return status;
        }
        absl::c_copy(child_contexts, google::protobuf::RepeatedPtrFieldBackInserter(
                                         response->mutable_contexts()));
        return absl::OkStatus();
      },
      request.transaction_options());
}


absl::Status MetadataStore::GetLineageGraph(
    const GetLineageGraphRequest& request, GetLineageGraphResponse* response) {
  if (!request.options().has_artifacts_options()) {
    return absl::InvalidArgumentError("Missing query_nodes conditions");
  }
  static constexpr int64 kMaxDistance = 20;
  int64 max_num_hops = kMaxDistance;
  if (request.options().stop_conditions().has_max_num_hops()) {
    if (request.options().stop_conditions().max_num_hops() < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("max_num_hops cannot be negative: max_num_hops =",
                       request.options().stop_conditions().max_num_hops()));
    }
    max_num_hops = std::min<int64>(
        max_num_hops, request.options().stop_conditions().max_num_hops());
    if (request.options().stop_conditions().max_num_hops() > max_num_hops) {
      LOG(WARNING) << "stop_conditions.max_num_hops: "
                   << request.options().stop_conditions().max_num_hops()
                   << " is greater than the maximum value allowed: "
                   << kMaxDistance << "; use " << kMaxDistance
                   << " instead to limit the size of the traversal.";
    }
  } else {
    LOG(INFO) << "stop_conditions.max_num_hops is not set. Use maximum value: "
              << kMaxDistance << " to limit the size of the traversal.";
  }
  return transaction_executor_->Execute(
      [this, &request, &response, max_num_hops]() -> absl::Status {
        response->Clear();
        std::vector<Artifact> artifacts;
        std::string dummy_token;
        MLMD_RETURN_IF_ERROR(metadata_access_object_->ListArtifacts(
            request.options().artifacts_options(), &artifacts, &dummy_token));
        if (artifacts.empty()) {
          return absl::NotFoundError(
              "The query_nodes condition does not match any nodes to do "
              "traversal.");
        }
        if (request.options().max_node_size() > 0 &&
            artifacts.size() > request.options().max_node_size()) {
          artifacts.erase(artifacts.begin() + request.options().max_node_size(),
                          artifacts.end());
        }
        const LineageGraphQueryOptions::BoundaryConstraint& stop_conditions =
            request.options().stop_conditions();
        return metadata_access_object_->QueryLineageGraph(
            artifacts, max_num_hops,
            request.options().max_node_size() > 0
                ? absl::make_optional<int64>(request.options().max_node_size())
                : absl::nullopt,
            !stop_conditions.boundary_artifacts().empty()
                ? absl::make_optional<std::string>(
                      stop_conditions.boundary_artifacts())
                : absl::nullopt,
            !stop_conditions.boundary_executions().empty()
                ? absl::make_optional<std::string>(
                      stop_conditions.boundary_executions())
                : absl::nullopt,
            *response->mutable_subgraph());
      },
      request.transaction_options());
}


MetadataStore::MetadataStore(
    std::unique_ptr<MetadataSource> metadata_source,
    std::unique_ptr<MetadataAccessObject> metadata_access_object,
    std::unique_ptr<TransactionExecutor> transaction_executor)
    : metadata_source_(std::move(metadata_source)),
      metadata_access_object_(std::move(metadata_access_object)),
      transaction_executor_(std::move(transaction_executor)) {}

}  // namespace ml_metadata
