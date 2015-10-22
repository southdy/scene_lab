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

#ifndef SCENE_LAB_SCENE_LAB_H_
#define SCENE_LAB_SCENE_LAB_H_

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "component_library/camera_interface.h"
#include "component_library/entity_factory.h"
#include "component_library/meta.h"
#include "component_library/physics.h"
#include "component_library/rendermesh.h"
#include "component_library/transform.h"
#include "editor_components_generated.h"
#include "entity/component_interface.h"
#include "entity/entity_manager.h"
#include "flatui/flatui.h"
#include "fplbase/asset_manager.h"
#include "fplbase/input.h"
#include "fplbase/renderer.h"
#include "fplbase/utilities.h"
#include "mathfu/vector_3.h"
#include "scene_lab/edit_options.h"
#include "scene_lab/editor_controller.h"
#include "scene_lab/editor_gui.h"
#include "scene_lab_config_generated.h"

namespace fpl {
namespace scene_lab {

typedef std::function<void(const entity::EntityRef& entity)> EntityCallback;
typedef std::function<void()> EditorCallback;

class SceneLab {
 public:
  /// Initialize Scene Lab once, when starting your game.
  ///
  /// Call this function as soon as you have an entity manager and font
  /// manager. Make sure you give Scene Lab a camera via SetCamera() as well.
  void Initialize(const SceneLabConfig* config,
                  entity::EntityManager* entity_manager,
                  FontManager* font_manager);

  /// Give Scene Lab a camera that it can use.
  ///
  /// Scene Lab will take over ownership of `camera`.
  void SetCamera(std::unique_ptr<CameraInterface> camera) {
    camera_ = std::move(camera);
  }

  /// While Scene Lab is active, you must call this once a frame, every frame.
  void AdvanceFrame(WorldTime delta_time);

  /// Render Scene Lab and its GUI; only call this when Scene Lab is active.
  ///
  /// While Scene Lab is running, you are still responsible for rendering your
  /// own game world. Call GetCamera() to get the camera you should use for
  /// rendering.
  ///
  /// Warning: if you are actively using FlatUI elsewhere in your code while
  /// Scene Lab is running, you will need to modify this function to not render
  /// the GUI here, and call EditorGui::StartRender(), EditorGui::DrawGui(), and
  /// EditorGui::FinishRender() yourself.
  void Render(Renderer* renderer);

  /// Activate Scene Lab. Once you call this, you should start calling
  /// AdvanceFrame and Render each frame, and stop calling
  /// EntityManager::UpdateComponents() yourself.
  void Activate();

  /// Immediately deactivate Scene Lab. The preferred way to exit the editor is
  /// to use RequestExit, however, as that will give the user a chance to save
  /// their changes to the world.
  void Deactivate();

  /// When you activate the editor, you can pass in the camera position so the
  /// user can seamlessly be positioned at the same place they were during the
  /// game.
  void SetInitialCamera(const CameraInterface& initial_camera);

  /// Get the Scene Lab camera, so you can render the scene properly.
  const CameraInterface* GetCamera() const { return camera_.get(); }

  /// Highlight the specified entity, so that you can change its properties.
  void SelectEntity(const entity::EntityRef& entity_ref);

  /// Save the current positions and properties of all entities.
  ///
  /// If `to_disk` is true, save to .bin and .json files and update the entity
  /// factory's file cache. Otherwise, just update the file cache but don't
  /// physically save the files to disk.
  ///
  /// If you are saving to disk, entities will be saved to the files they were
  /// initially loaded from.
  void SaveScene(bool to_disk);

  /// Save the current positions and properties to disk.
  ///
  /// See SaveScene(bool to_disk) for more details.
  void SaveScene() { SaveScene(true); }

  /// Save all the entities that were from a specific file to that file on disk.
  ///
  /// Called by SaveScene() when saving to disk, but you could always call
  /// this directly.
  void SaveEntitiesInFile(const std::string& filename);

  /// Request that Scene Lab exit.
  ///
  /// If you haven't saved your changes, it will prompt you to do so, keep them
  /// in memory, or abandon them. Once Scene Lab decides it's okay to exit,
  /// IsReadyToExit() will return true.
  ///
  /// After you've exited, you can always get back into Scene Lab by calling
  /// Activate() again.
  void RequestExit();

  /// Abort a previously-requested exit, which hides the confirmation dialog.
  void AbortExit();

  /// Returns true if we are ready to exit Scene Lab (everything is saved or
  /// discarded, etc), or false if not. Once it returns true, you can safely
  /// deactivate the editor.
  bool IsReadyToExit();

  /// Add a component to the list of components Scene Lab updates each frame.
  ///
  /// While Scene Lab is activated, you should no longer be calling
  /// EntityManager::UpdateComponents(); you should let Scene Lab update only
  /// the components it cares about. If you have any components you are sure you
  /// also want updated while editing the scene, add them to the list by calling
  /// this function.
  void AddComponentToUpdate(entity::ComponentId component_id) {
    components_to_update_.push_back(component_id);
  }

  /// Externally mark that some entities have been modified.
  ///
  /// Generally used by Scene Lab's GUI, but if you change an entity's
  /// properties some other way, call this function to ensure the user will be
  /// prompted to save on exiting the editor.
  void set_entities_modified(bool b) { entities_modified_ = b; }

  /// Have entities been modified? If so, prompt the user to save before exit.
  bool entities_modified() const { return entities_modified_; }

  /// Specify a callback to call when the editor is opened.
  void AddOnEnterEditorCallback(EditorCallback callback);

  /// Specify a callback to call when the editor is exited.
  void AddOnExitEditorCallback(EditorCallback callback);

  /// Specify a callback to call when an entity is created.
  void AddOnCreateEntityCallback(EntityCallback callback);

  /// Specify a callback to call when an entity's data is updated.
  void AddOnUpdateEntityCallback(EntityCallback callback);

  /// Specify a callback to call when an entity is deleted.
  void AddOnDeleteEntityCallback(EntityCallback callback);

  /// Call all 'EditorEnter' callbacks.
  void NotifyEnterEditor() const;

  /// Call all 'EditorExit' callbacks.
  void NotifyExitEditor() const;

  /// Call all 'EntityCreated' callbacks.
  void NotifyCreateEntity(const entity::EntityRef& entity) const;

  /// Call all 'EntityUpdated' callbacks.
  void NotifyUpdateEntity(const entity::EntityRef& entity) const;

  /// Call all 'EntityDeleted' callbacks.
  void NotifyDeleteEntity(const entity::EntityRef& entity) const;

 private:
  enum InputMode { kMoving, kEditing, kDragging };
  enum MouseMode {
    kMoveHorizontal,    // Move along the ground.
    kMoveVertical,      // Move along a plane perpendicular to the ground and
                        // perpendicular to the camera.
    kRotateHorizontal,  // Rotate horizontally--that is, about an axis
                        // perpendicular to the ground.
    kRotateVertical,    // Rotate vertically--that is, about an axis parallel to
                        // the ground that points back towards the camera.
    kScaleAll,          // Scale on all axes as you drag up and down.
    kScaleX,            // Scale on the X axis as you drag along the ground.
    kScaleY,            // Scale on the Y axis as you drag along the ground.
    kScaleZ,            // Scale on the Z axis as you drag up and down.
    kMouseModeCount
  };

  // return true if we should be moving the camera and objects slowly.
  bool PreciseMovement() const;

  // return a global vector from camera coordinates relative to the horizontal
  // plane.
  mathfu::vec3 GlobalFromHorizontal(float forward, float right, float up) const;

  // get camera movement via W-A-S-D
  mathfu::vec3 GetMovement() const;

  entity::EntityRef DuplicateEntity(entity::EntityRef& entity);
  void DestroyEntity(entity::EntityRef& entity);
  void HighlightEntity(const entity::EntityRef& entity, float tint);

  // returns true if the transform was modified
  bool ModifyTransformBasedOnInput(TransformDef* transform);

  // Find the intersection between a ray and a plane.
  // Ensure ray_direction and plane_normal are both normalized.
  // Returns true if it intersects with the plane, and sets the
  // intersection point.
  static bool IntersectRayToPlane(const vec3& ray_origin,
                                  const vec3& ray_direction,
                                  const vec3& point_on_plane,
                                  const vec3& plane_normal,
                                  vec3* intersection_point);

  // Take a point, and project it onto a plane in the direction of the plane
  // normal. Ensure plane_normal is normalized. Returns true if was able to
  // project the point, false if it wasn't (which would be a weird situation).
  static bool ProjectPointToPlane(const vec3& point_to_project,
                                  const vec3& point_on_plane,
                                  const vec3& plane_normal,
                                  vec3* point_projected);

  // Serialize the entities from the given file into the given vector.
  // Returns true if it succeeded, false if there was an error.
  bool SerializeEntitiesFromFile(const std::string& filename,
                                 std::vector<uint8_t>* output);

  void LoadSchemaFiles();

  const SceneLabConfig* config_;
  Renderer* renderer_;
  InputSystem* input_system_;
  entity::EntityManager* entity_manager_;
  component_library::EntityFactory* entity_factory_;
  FontManager* font_manager_;
  // Which entity are we currently editing?
  entity::EntityRef selected_entity_;

  InputMode input_mode_;
  MouseMode mouse_mode_;

  // Temporary solution to let us cycle through all entities.
  std::unique_ptr<entity::EntityManager::EntityStorageContainer::Iterator>
      entity_cycler_;

  // For storing the FlatBuffers schema we use for exporting.
  std::string schema_data_;
  std::string schema_text_;

  std::vector<entity::ComponentId> components_to_update_;
  std::unique_ptr<EditorController> controller_;
  std::unique_ptr<EditorGui> gui_;
  std::unique_ptr<CameraInterface> camera_;

  // Camera angles, projected onto the horizontal plane, as defined by the
  // camera's `up()` direction.
  mathfu::vec3 horizontal_forward_;
  mathfu::vec3 horizontal_right_;

  mathfu::vec3 drag_point_;  // Point on the object at which we began dragging.
  mathfu::vec3 drag_plane_normal_;  // Normal of the plane we're dragging along.
  mathfu::vec3 drag_offset_;  // Offset between drag point and object's origin.
  mathfu::vec3 drag_prev_intersect_;  // Previous intersection point
  mathfu::vec3 drag_orig_scale_;      // Object scale when we started dragging.

  float rendermesh_culling_distance_squared_;

  bool exit_requested_;
  bool exit_ready_;
  bool entities_modified_;

  // A collection of callbacks that are called when various Scene Lab events
  // occur that other game systems may want to respond to.
  std::vector<EditorCallback> on_enter_editor_callbacks_;
  std::vector<EditorCallback> on_exit_editor_callbacks_;
  std::vector<EntityCallback> on_create_entity_callbacks_;
  std::vector<EntityCallback> on_update_entity_callbacks_;
  std::vector<EntityCallback> on_delete_entity_callbacks_;
};

}  // namespace scene_lab
}  // namespace fpl

#endif  // SCENE_LAB_SCENE_LAB_H_