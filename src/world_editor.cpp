// Copyright 2015 Google Inc. All rights reserved.
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

#include "world_editor/world_editor.h"

#include <math.h>
#include <set>
#include <string>
#include "component_library/common_services.h"
#include "editor_events_generated.h"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "fplbase/flatbuffer_utils.h"
#include "fplbase/utilities.h"
#include "library_components_generated.h"
#include "mathfu/utilities.h"

namespace fpl {
namespace editor {

using component_library::CommonServicesComponent;
using component_library::MetaComponent;
using component_library::MetaData;
using component_library::PhysicsComponent;
using component_library::RenderMeshComponent;
using component_library::RenderMeshData;
using component_library::TransformComponent;
using component_library::TransformData;
using component_library::EntityFactory;
using mathfu::vec3;

static const float kRaycastDistance = 100.0f;
static const float kMinValidDistance = 0.00001f;

void WorldEditor::Initialize(const WorldEditorConfig* config,
                             entity::EntityManager* entity_manager,
                             FontManager* font_manager) {
  config_ = config;
  entity_manager_ = entity_manager;
  font_manager_ = font_manager;

  auto services = entity_manager_->GetComponent<CommonServicesComponent>();
  renderer_ = services->renderer();
  input_system_ = services->input_system();
  event_manager_ = services->event_manager();
  entity_factory_ = services->entity_factory();

  entity_cycler_.reset(
      new entity::EntityManager::EntityStorageContainer::Iterator{
          entity_manager->end()});
  LoadSchemaFiles();
  horizontal_forward_ = mathfu::kAxisY3f;
  horizontal_right_ = mathfu::kAxisX3f;
  controller_.reset(new EditorController(config_, input_system_));
}

// Project `v` onto `unit`. That is, return the vector colinear with `unit`
// such that `v` - returned_vector is perpendicular to `unit`.
static inline vec3 ProjectOntoUnitVector(const vec3& v, const vec3& unit) {
  return vec3::DotProduct(v, unit) * unit;
}

void WorldEditor::AdvanceFrame(WorldTime delta_time) {
  // Update the editor's forward and right vectors in the horizontal plane.
  // Remove the up component from the camera's facing vector.
  vec3 forward = camera_->facing() -
                 ProjectOntoUnitVector(camera_->facing(), camera_->up());
  vec3 right = vec3::CrossProduct(camera_->facing(), camera_->up());

  // If we're in gimbal lock, use previous frame's forward calculations.
  if (forward.Normalize() > kMinValidDistance &&
      right.Normalize() > kMinValidDistance) {
    horizontal_forward_ = forward;
    horizontal_right_ = right;
  }

  controller_->Update();
  if (input_mode_ == kMoving) {
    // Allow the camera to look around and move.
    camera_->set_facing(controller_->GetFacing());

    vec3 movement = GetMovement();
    camera_->set_position(camera_->position() + movement * (float)delta_time);

    if (controller_->ButtonWentDown(config_->toggle_mode_button())) {
      input_mode_ = kEditing;
      LogInfo("Toggle to editing mode");
      controller_->UnlockMouse();
    }
  } else if (input_mode_ == kEditing) {
    if (controller_->ButtonWentDown(config_->toggle_mode_button())) {
      controller_->SetFacing(camera_->facing());
      controller_->LockMouse();
      LogInfo("Toggle to moving mode");
      input_mode_ = kMoving;
    }
  } else if (input_mode_ == kDragging) {
    if (controller_->ButtonWentUp(config_->interact_button())) {
      LogInfo("Stop dragging");
      input_mode_ = kEditing;
    }

    if (controller_->ButtonWentDown(config_->toggle_mode_button())) {
      controller_->SetFacing(camera_->facing());
      controller_->LockMouse();
      LogInfo("Toggle to moving mode");
      input_mode_ = kMoving;
    }
  }

  bool entity_changed = false;
  do {
    if (controller_->KeyWentDown(FPLK_RIGHTBRACKET)) {
      // select next entity to edit
      if (*entity_cycler_ != entity_manager_->end()) {
        (*entity_cycler_)++;
      }
      if (*entity_cycler_ == entity_manager_->end()) {
        *entity_cycler_ = entity_manager_->begin();
      }
      entity_changed = true;
    }
    if (controller_->KeyWentDown(FPLK_LEFTBRACKET)) {
      if (*entity_cycler_ == entity_manager_->begin()) {
        *entity_cycler_ = entity_manager_->end();
      }
      (*entity_cycler_)--;
      entity_changed = true;
    }
    if (entity_changed) {
      entity::EntityRef entity_ref = entity_cycler_->ToReference();
      auto data =
          entity_manager_->GetComponentData<EditOptionsData>(entity_ref);
      if (data != nullptr) {
        // Are we allowed to cycle through this entity?
        if (data->selection_option == SelectionOption_Unspecified ||
            data->selection_option == SelectionOption_Any ||
            data->selection_option == SelectionOption_CycleOnly) {
          SelectEntity(entity_ref);
        }
      }
    }
  } while (entity_changed && entity_cycler_->ToReference() != selected_entity_);

  entity_changed = false;
  if (controller_->ButtonWentDown(config_->interact_button())) {
    // Use position of the mouse pointer for the ray cast.
    vec3 start, end;
    if (controller_->mouse_locked()) {
      start = camera_->position();
      end = start + camera_->facing() * kRaycastDistance;
    } else {
      controller_->GetMouseWorldRay(*camera_, renderer_->window_size(), &start,
                                    &end);
      vec3 dir = (end - start).Normalized();
      end = start + (dir * camera_->viewport_far_plane());
    }
    entity::EntityRef result =
        entity_manager_->GetComponent<PhysicsComponent>()->RaycastSingle(
            start, end, &drag_point_);
    if (result.IsValid()) {
      *entity_cycler_ = result.ToIterator();
      entity_changed = true;
    }
  }
  bool start_dragging = false;
  if (entity_changed) {
    entity::EntityRef entity_ref = entity_cycler_->ToReference();
    if (input_mode_ == kEditing && entity_ref == selected_entity_) {
      start_dragging = true;
    } else {
      auto data =
          entity_manager_->GetComponentData<EditOptionsData>(entity_ref);
      if (data != nullptr) {
        // Are we allowed to click on this entity?
        if (data->selection_option == SelectionOption_Unspecified ||
            data->selection_option == SelectionOption_Any ||
            data->selection_option == SelectionOption_PointerOnly) {
          SelectEntity(entity_cycler_->ToReference());
        }
      }
    }
  }

  auto transform_component =
      entity_manager_->GetComponent<TransformComponent>();
  if (selected_entity_.IsValid()) {
    // We have an entity selected, let's allow it to be modified
    auto raw_data = transform_component->ExportRawData(selected_entity_);
    if (raw_data != nullptr) {
      // TODO(jsimantov): in the future, don't just assume the type of this
      TransformDef* transform =
          flatbuffers::GetMutableRoot<TransformDef>(raw_data.get());
      if (ModifyTransformBasedOnInput(transform)) {
        transform_component->AddFromRawData(selected_entity_, transform);
        auto physics = entity_manager_->GetComponent<PhysicsComponent>();
        physics->UpdatePhysicsFromTransform(selected_entity_);
        if (physics->GetComponentData(selected_entity_)->enabled) {
          // Workaround for an issue with the physics library where modifying
          // a raycast physics volume causes raycasts to stop working on it.
          physics->DisablePhysics(selected_entity_);
          physics->EnablePhysics(selected_entity_);
        }
        NotifyEntityUpdated(selected_entity_);
      }
    }

    if (controller_->KeyWentDown(FPLK_INSERT) ||
        controller_->KeyWentDown(FPLK_v)) {
      entity::EntityRef new_entity = DuplicateEntity(selected_entity_);
      *entity_cycler_ = new_entity.ToIterator();
      SelectEntity(entity_cycler_->ToReference());
      NotifyEntityUpdated(new_entity);
    }
    if (controller_->KeyWentDown(FPLK_DELETE) ||
        controller_->KeyWentDown(FPLK_x)) {
      entity::EntityRef entity = selected_entity_;
      NotifyEntityDeleted(entity);
      *entity_cycler_ = entity_manager_->end();
      selected_entity_ = entity::EntityRef();
      DestroyEntity(entity);
    }
  }

  transform_component->PostLoadFixup();

  // Any components we specifically still want to update should be updated here.
  for (size_t i = 0; i < components_to_update_.size(); i++) {
    entity_manager_->GetComponent(components_to_update_[i])
        ->UpdateAllEntities(0);
  }

  if (start_dragging && input_mode_ == kEditing && selected_entity_) {
    auto transform_component =
        entity_manager_->GetComponent<TransformComponent>();
    auto raw_data = transform_component->ExportRawData(selected_entity_);
    TransformDef* transform =
        flatbuffers::GetMutableRoot<TransformDef>(raw_data.get());
    vec3 position = LoadVec3(transform->position());
    vec3 intersect;

    vec3 start, end;
    controller_->GetMouseWorldRay(*camera_, renderer_->window_size(), &start,
                                  &end);
    vec3 mouse_ray_origin = start;
    vec3 mouse_ray_dir = (end - start).Normalized();
    if (IntersectRayToPlane(mouse_ray_origin, mouse_ray_dir, drag_point_,
                            vec3(0, 0, 1), &intersect)) {
      drag_offset_ = position - intersect;
      input_mode_ = kDragging;
    }
  }
  // Save the current mouse pointer direction in the world, for dragging
  // the object around.
  if (input_mode_ == kDragging) {
    vec3 start, end;
    if (controller_->GetMouseWorldRay(*camera_, renderer_->window_size(),
                                      &start, &end)) {
      previous_mouse_ray_origin_ = start;
      previous_mouse_ray_dir_ = (end - start).Normalized();
    }
  }

  entity_manager_->DeleteMarkedEntities();
}

void WorldEditor::HighlightEntity(const entity::EntityRef& entity, float tint) {
  if (!entity.IsValid()) return;
  auto render_data = entity_manager_->GetComponentData<RenderMeshData>(entity);
  if (render_data != nullptr) {
    render_data->tint = vec4(tint, tint, tint, 1);
  }
  // Highlight the node's children as well.
  auto transform_data =
      entity_manager_->GetComponentData<TransformData>(entity);

  if (transform_data != nullptr) {
    for (auto iter = transform_data->children.begin();
         iter != transform_data->children.end(); ++iter) {
      // Highlight the child, but slightly less brightly.
      HighlightEntity(iter->owner, 1 + ((tint - 1) * .8));
    }
  }
}

void WorldEditor::SelectEntity(const entity::EntityRef& entity_ref) {
  if (entity_ref.IsValid()) {
    auto data = entity_manager_->GetComponentData<MetaData>(entity_ref);
    if (data != nullptr) {
      LogInfo("Highlighting entity '%s' with prototype '%s'",
              data->entity_id.c_str(), data->prototype.c_str());
    }
    HighlightEntity(entity_ref, 2);
  }
  if (selected_entity_.IsValid() && selected_entity_ != entity_ref) {
    // un-highlight the old one
    HighlightEntity(selected_entity_, 1);
  }
  selected_entity_ = entity_ref;
}

void WorldEditor::Render(Renderer* /*renderer*/) {
  // Render any editor-specific things
  // if (controller_->mouse_locked()) {
  // renderer->SetCulling(Renderer::kNoCulling);
  // renderer->DepthTest(false);
  // draw a reticle
  //}
}

void WorldEditor::SetInitialCamera(const CameraInterface& initial_camera) {
  camera_->set_position(initial_camera.position());
  camera_->set_facing(initial_camera.facing());
  camera_->set_up(initial_camera.up());
}

void WorldEditor::NotifyEntityCreated(const entity::EntityRef& entity) const {
  if (event_manager_ != nullptr) {
    event_manager_->BroadcastEvent(
        EditorEventPayload(EditorEventAction_EntityCreated, entity));
  }
}

void WorldEditor::NotifyEntityUpdated(const entity::EntityRef& entity) const {
  if (event_manager_ != nullptr) {
    event_manager_->BroadcastEvent(
        EditorEventPayload(EditorEventAction_EntityUpdated, entity));
  }
}

void WorldEditor::NotifyEntityDeleted(const entity::EntityRef& entity) const {
  if (event_manager_ != nullptr) {
    event_manager_->BroadcastEvent(
        EditorEventPayload(EditorEventAction_EntityDeleted, entity));
  }
}

void WorldEditor::Activate() {
  // Set up the initial camera position.
  controller_->SetFacing(camera_->facing());
  controller_->LockMouse();

  input_mode_ = kMoving;
  *entity_cycler_ = entity_manager_->end();

  // Raise the EditorStart event.
  if (event_manager_ != nullptr) {
    event_manager_->BroadcastEvent(EditorEventPayload(EditorEventAction_Enter));
  }
}

void WorldEditor::Deactivate() {
  // Raise the EditorExit event.
  if (event_manager_ != nullptr) {
    event_manager_->BroadcastEvent(EditorEventPayload(EditorEventAction_Exit));
  }

  // de-select all entities
  SelectEntity(entity::EntityRef());

  *entity_cycler_ = entity_manager_->end();
}

void WorldEditor::SaveWorld() {
  auto editor_component = entity_manager_->GetComponent<MetaComponent>();
  // get a list of all filenames in the world
  std::set<std::string> filenames;
  for (auto entity = editor_component->begin();
       entity != editor_component->end(); ++entity) {
    filenames.insert(entity->data.source_file);
  }
  for (auto iter = filenames.begin(); iter != filenames.end(); ++iter) {
    SaveEntitiesInFile(*iter);
  }
}

entity::EntityRef WorldEditor::DuplicateEntity(entity::EntityRef& entity) {
  std::vector<uint8_t> entity_serialized;
  if (!entity_factory_->SerializeEntity(entity, entity_manager_,
                                        &entity_serialized)) {
    LogError("DuplicateEntity: Couldn't serialize entity");
  }
  std::vector<std::vector<uint8_t>> entity_defs = {entity_serialized};
  std::vector<uint8_t> entity_list;
  if (!entity_factory_->SerializeEntityList(entity_defs, &entity_list)) {
    LogError("DuplicateEntity: Couldn't create entity list");
  }
  std::vector<entity::EntityRef> entities_created;
  if (entity_factory_->LoadEntityListFromMemory(
          entity_list.data(), entity_manager_, &entities_created) > 0) {
    // We created some new duplicate entities! (most likely exactly one.)  We
    // need to remove their entity IDs since otherwise they will have duplicate
    // entity IDs to the one we created.  We also need to make sure the new
    // entity IDs are marked with the same source file as the old.

    for (size_t i = 0; i < entities_created.size(); i++) {
      entity::EntityRef& new_entity = entities_created[i];
      MetaData* old_editor_data =
          entity_manager_->GetComponentData<MetaData>(entity);
      MetaData* editor_data =
          entity_manager_->GetComponentData<MetaData>(new_entity);
      if (editor_data != nullptr) {
        editor_data->entity_id = "";
        if (old_editor_data != nullptr)
          editor_data->source_file = old_editor_data->source_file;
      }
    }
    entity_manager_->GetComponent<TransformComponent>()->PostLoadFixup();
    for (size_t i = 0; i < entities_created.size(); i++) {
      NotifyEntityCreated(entities_created[i]);
    }
    return entities_created[0];
  }
  return entity::EntityRef();
}

void WorldEditor::DestroyEntity(entity::EntityRef& entity) {
  entity_manager_->DeleteEntity(entity);
}

void WorldEditor::LoadSchemaFiles() {
  const char* schema_file_text = config_->schema_file_text()->c_str();
  const char* schema_file_binary = config_->schema_file_binary()->c_str();

  if (!LoadFile(schema_file_binary, &schema_data_)) {
    LogInfo("Failed to open binary schema file: %s", schema_file_binary);
    return;
  }
  auto schema = reflection::GetSchema(schema_data_.c_str());
  if (schema != nullptr) {
    LogInfo("WorldEditor: Binary schema %s loaded", schema_file_binary);
  }
  if (LoadFile(schema_file_text, &schema_text_)) {
    LogInfo("WorldEditor: Text schema %s loaded", schema_file_text);
  }
}

void WorldEditor::SaveEntitiesInFile(const std::string& filename) {
  if (filename == "") {
    LogInfo("Skipping saving entities to blank filename.");
    return;
  }
  LogInfo("Saving entities in file: '%s'", filename.c_str());
  // We know the FlatBuffer format we're using: components
  flatbuffers::FlatBufferBuilder builder;
  std::vector<std::vector<uint8_t>> entities_serialized;

  auto editor_component = entity_manager_->GetComponent<MetaComponent>();
  // loop through all entities
  for (auto entityiter = entity_manager_->begin();
       entityiter != entity_manager_->end(); ++entityiter) {
    entity::EntityRef entity = entityiter.ToReference();
    const MetaData* editor_data = editor_component->GetComponentData(entity);
    if (editor_data != nullptr && editor_data->source_file == filename) {
      entities_serialized.push_back({});
      entity_factory_->SerializeEntity(entity, entity_manager_,
                                       &entities_serialized.back());
    }
  }
  std::vector<uint8_t> entity_list;
  if (!entity_factory_->SerializeEntityList(entities_serialized,
                                            &entity_list)) {
    LogError("Couldn't serialize entity list");
    return;
  }
  if (SaveFile((filename + ".bin").c_str(), entity_list.data(),
               entity_list.size())) {
    LogInfo("Save (binary) successful.");
  } else {
    LogInfo("Save (binary) failed.");
  }
  // Now save to JSON file.
  // First load and parse the flatbuffer schema, then generate text.
  if (schema_text_ != "") {
    // Make a list of include paths that parser.Parse can parse.
    // char** with nullptr termination.
    std::unique_ptr<const char*> include_paths;
    size_t num_paths = config_->schema_include_paths()->size();
    include_paths.reset(new const char*[num_paths + 1]);
    for (size_t i = 0; i < num_paths; i++) {
      include_paths.get()[i] = config_->schema_include_paths()->Get(i)->c_str();
    }
    include_paths.get()[num_paths] = nullptr;
    flatbuffers::Parser parser;
    if (parser.Parse(schema_text_.c_str(), include_paths.get(),
                     config_->schema_file_text()->c_str())) {
      std::string json;
      GenerateText(parser, entity_list.data(), flatbuffers::GeneratorOptions(),
                   &json);
      if (SaveFile((filename + ".json").c_str(), json)) {
        LogInfo("Save (JSON) successful");
      } else {
        LogInfo("Save (JSON) failed.");
      }
    } else {
      LogInfo("Couldn't parse schema file: %s", parser.error_.c_str());
    }
  } else {
    LogInfo("No text schema loaded, can't save JSON file.");
  }
}

bool WorldEditor::PreciseMovement() const {
  // When the shift key is held, use more precise movement.
  // TODO: would be better if we used precise movement by default, and
  //       transitioned to fast movement after the key has been held for
  //       a while.
  return controller_->KeyIsDown(FPLK_LSHIFT) ||
         controller_->KeyIsDown(FPLK_RSHIFT);
}

vec3 WorldEditor::GlobalFromHorizontal(float forward, float right,
                                       float up) const {
  return horizontal_forward_ * forward + horizontal_right_ * right +
         camera_->up() * up;
}

bool WorldEditor::IntersectRayToPlane(const vec3& ray_origin,
                                      const vec3& ray_direction,
                                      const vec3& point_on_plane,
                                      const vec3& plane_normal,
                                      vec3* intersection_point) {
  vec3 ray_origin_to_plane = ray_origin - point_on_plane;
  float distance_from_ray_origin_to_plane =
      vec3::DotProduct(ray_origin_to_plane, plane_normal);
  // vec3 nearest_point_on_plane_to_ray_origin =
  //     plane_normal * -distance_from_ray_origin_to_plane;
  // ratio of diagonal / distance_from_ray_origin_to_plane
  float length_ratio = vec3::DotProduct(ray_direction, -plane_normal);
  // float diagonal = length_ratio * distance_from_ray_origin_to_plane;
  if (distance_from_ray_origin_to_plane < 0.001)
    *intersection_point = ray_origin_to_plane;
  else if (length_ratio < 0.001)
    return false;
  else
    *intersection_point =
        ray_origin +
        ray_direction * distance_from_ray_origin_to_plane * 1.0f / length_ratio;
  return true;
}

vec3 WorldEditor::GetMovement() const {
  // Get a movement vector to move the user forward, up, or right.
  // Movement is always relative to the camera facing, but parallel to
  // ground.
  float forward_speed = 0;
  float up_speed = 0;
  float right_speed = 0;
  const float move_speed =
      PreciseMovement()
          ? config_->camera_movement_speed() * config_->precise_movement_scale()
          : config_->camera_movement_speed();

  // TODO(jsimantov): make the specific keys configurable?
  if (controller_->KeyIsDown(FPLK_w)) {
    forward_speed += move_speed;
  }
  if (controller_->KeyIsDown(FPLK_s)) {
    forward_speed -= move_speed;
  }
  if (controller_->KeyIsDown(FPLK_d)) {
    right_speed += move_speed;
  }
  if (controller_->KeyIsDown(FPLK_a)) {
    right_speed -= move_speed;
  }
  if (controller_->KeyIsDown(FPLK_r)) {
    up_speed += move_speed;
  }
  if (controller_->KeyIsDown(FPLK_f)) {
    up_speed -= move_speed;
  }

  // Translate the keypresses into movement parallel to the ground plane.
  const vec3 movement =
      GlobalFromHorizontal(forward_speed, right_speed, up_speed);
  return movement;
}

bool WorldEditor::ModifyTransformBasedOnInput(TransformDef* transform) {
  if (input_mode_ == kDragging) {
    vec3 start, end;
    controller_->GetMouseWorldRay(*camera_, renderer_->window_size(), &start,
                                  &end);
    vec3 mouse_ray_origin = start;
    vec3 mouse_ray_dir = (end - start).Normalized();
    vec3 intersect;

    if (IntersectRayToPlane(mouse_ray_origin, mouse_ray_dir, drag_point_,
                            vec3(0, 0, 1), &intersect)) {
      vec3 new_pos = intersect + drag_offset_;
      *transform->mutable_position() =
          Vec3(new_pos.x(), new_pos.y(), new_pos.z());
      return true;
    }
  } else {
    // IJKL = move x/y axis
    float fwd_speed = 0, right_speed = 0, up_speed = 0;
    float roll_speed = 0, pitch_speed = 0, yaw_speed = 0;
    float scale_speed = 1;

    // When the shift key is held, use more precise movement.
    // TODO: would be better if we used precise movement by default, and
    //       transitioned to fast movement after the key has been held for
    //       a while.
    const float movement_scale =
        PreciseMovement() ? config_->precise_movement_scale() : 1.0f;
    const float move_speed = movement_scale * config_->object_movement_speed();
    const float angular_speed =
        movement_scale * config_->object_angular_speed();

    if (controller_->KeyIsDown(FPLK_i)) {
      fwd_speed += move_speed;
    }
    if (controller_->KeyIsDown(FPLK_k)) {
      fwd_speed -= move_speed;
    }
    if (controller_->KeyIsDown(FPLK_j)) {
      right_speed -= move_speed;
    }
    if (controller_->KeyIsDown(FPLK_l)) {
      right_speed += move_speed;
    }
    // P; = move z axis
    if (controller_->KeyIsDown(FPLK_p)) {
      up_speed += move_speed;
    }
    if (controller_->KeyIsDown(FPLK_SEMICOLON)) {
      up_speed -= move_speed;
    }
    // UO = roll
    if (controller_->KeyIsDown(FPLK_u)) {
      roll_speed += angular_speed;
    }
    if (controller_->KeyIsDown(FPLK_o)) {
      roll_speed -= angular_speed;
    }
    // YH = pitch
    if (controller_->KeyIsDown(FPLK_y)) {
      pitch_speed += angular_speed;
    }
    if (controller_->KeyIsDown(FPLK_h)) {
      pitch_speed -= angular_speed;
    }
    // NM = yaw
    if (controller_->KeyIsDown(FPLK_n)) {
      yaw_speed += angular_speed;
    }
    if (controller_->KeyIsDown(FPLK_m)) {
      yaw_speed -= angular_speed;
    }
    // +- = scale
    if (controller_->KeyIsDown(FPLK_EQUALS)) {
      scale_speed = config_->object_scale_speed();
    } else if (controller_->KeyIsDown(FPLK_MINUS)) {
      scale_speed = 1.0f / config_->object_scale_speed();
    }
    const vec3 position =
        LoadVec3(transform->mutable_position()) +
        GlobalFromHorizontal(fwd_speed, right_speed, up_speed);

    Vec3 orientation = *transform->mutable_orientation();
    orientation =
        Vec3{orientation.x() + pitch_speed, orientation.y() + roll_speed,
             orientation.z() + yaw_speed};
    Vec3 scale = *transform->scale();
    if (controller_->KeyIsDown(FPLK_0)) {
      scale = Vec3{1.0f, 1.0f, 1.0f};
      scale_speed = 0;  // to trigger returning true
    } else {
      scale = Vec3{scale.x() * scale_speed, scale.y() * scale_speed,
                   scale.z() * scale_speed};
    }
    if (fwd_speed != 0 || right_speed != 0 || up_speed != 0 || yaw_speed != 0 ||
        roll_speed != 0 || pitch_speed != 0 || scale_speed != 1) {
      *(transform->mutable_position()) =
          Vec3(position.x(), position.y(), position.z());
      *(transform->mutable_orientation()) = orientation;
      *(transform->mutable_scale()) = scale;
      return true;
    }
  }
  return false;
}

}  // namespace editor
}  // namespace fpl