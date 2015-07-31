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

#ifndef FPL_FLATBUFFER_EDITOR_H_
#define FPL_FLATBUFFER_EDITOR_H_

#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/util.h"
#include "flatbuffer_editor_config_generated.h"
#include "flatui/flatui.h"
#include "mathfu/glsl_mappings.h"

namespace fpl {
namespace editor {

// An on-screen representation of a Flatbuffer, which can be edited by the user.
// Instantiate FlatbufferEditor with a Flatbuffer you'd like it to edit. It will
// create a copy of that Flatbuffer to manipulate, and you can get the modified
// Flatbuffer data back out whenever you want.
class FlatbufferEditor {
 public:
  // When you create a FlatbufferEditor, we will use reflection to
  // copy the flatbuffer into our own internal buffer. If you want to
  // change the Flatbuffer externally, call SetNewFlatbuffer() and
  // pass in the new contents. If you don't have the data you want to edit
  // yet, you can pass in nullptr, which means HasFlatbufferData() will be false
  // and can use SetFlatbufferData later on.
  // (And if you don't have a FlatbufferEditorConfig, just pass in nullptr and
  // we will use default UI settings.)
  FlatbufferEditor(const FlatbufferEditorConfig* config,
                   const reflection::Schema& schema,
                   const reflection::Object& table_def,
                   const void* flatbuffer_data);

  // Override the current Flatbuffer data with this new one. Uses reflection to
  // copy into our own internal buffers. Will discard whatever is already in our
  // copy of the Flatbuffer and in the edit fields.
  void SetFlatbufferData(const void* flatbuffer_data) {
    ClearEditFields();
    ClearFlatbufferModifiedFlag();
    if (flatbuffer_data != nullptr) {
      CopyTable(flatbuffer_data, &flatbuffer_);
    } else {
      flatbuffer_.clear();
    }
  }

  // If you passed in nullptr when setting the Flatbuffer data, this will be
  // false. Otherwise it will be true, and that means there is a Flatbuffer
  // that we are in the process of drawing / editing.
  bool HasFlatbufferData() const { return flatbuffer_.size() > 0; }

  // Update the internal state. Call each frame, OUTSIDE a gui::Run
  // context, before or after drawing.
  void Update();

  // Draw the Flatbuffer edit fields. Call this INSIDE a gui::Run context.
  void Draw();

  // Multiple ways of getting the Flatbuffer output. This will copy the data
  // out of our internal buffer using reflection (which also has the side effect
  // of normalizing the Flatbuffer if we have made changes to it).

  // Copy the modified Flatbuffer into a vector. Returns true if successful or
  // false if the Flatbuffer editor has no Flatbuffer to copy.
  bool GetFlatbufferCopy(std::vector<uint8_t>* flatbuffer_output);
  // Copy the modified Flatbuffer into a string (requires an extra
  // copy). Returns true if successful or false if there is no Flatbuffer.
  bool GetFlatbufferCopy(std::string* flatbuffer_output);
  // Copy the modified Flatbuffer into a generic buffer and get a pointer.
  // Requires an extra copy.
  std::unique_ptr<uint8_t> GetFlatbufferCopy();

  // Has the Flatbuffer data been modified? If so, you probably want to reload
  // whatever is using it.
  bool flatbuffer_modified() const { return flatbuffer_modified_; }
  // Once you have reloaded the Flatbuffer into whatever you are using it for,
  // call this to reset the "modified" flag and the list of modified fields.
  void ClearFlatbufferModifiedFlag();

  const void* flatbuffer() { return flatbuffer_.data(); }

  // Read-only mode: If true, draw the Flatbuffer using FlatUI's Label fields
  // instead of Edit fields, just showing the values and not allowing them to be
  // edited. Defaults to false.
  bool config_read_only() const { return config_read_only_; }
  void set_config_read_only(bool b) { config_read_only_ = b; }

  // Auto-commit mode: Whenever the user edits the Flatbuffer fields,
  // automatically update the Flatbuffer contents after the user finishes
  // editing (presses Enter or clicks on another field). If false, show an
  // "Apply" button next to all edited fields which will update the Flatbuffer
  // when clicked.
  bool config_auto_commit() const { return config_auto_commit_; }
  void set_config_auto_commit(bool b) { config_auto_commit_ = b; }

  // Size of all the UI elements passed to FlatUI.
  int ui_size() const { return ui_size_; }
  void set_ui_size(int s) { ui_size_ = s; }

  // Spacing of all the UI elements passed to FlatUI.
  int ui_spacing() const { return ui_spacing_; }
  void set_ui_spacing(int s) { ui_spacing_ = s; }

  int blank_field_width() const { return blank_field_width_; }
  void set_blank_field_width(int w) { blank_field_width_ = w; }

  // Show the type of each table / struct?
  bool show_types() const { return show_types_; }
  void set_show_types(bool b) { show_types_ = b; }

  // Expand all subtables?
  bool expand_all() const { return expand_all_; }
  void set_expand_all(bool b) { expand_all_ = b; }

  // Is the keyboard in use? A field is being edited? You probably want
  // to check this to make sure you don't use keypresses yourself.
  bool keyboard_in_use() const { return keyboard_in_use_; }

  // Set a unique root ID for all edit fields, required by FlatUI. If you don't
  // set this, it will use a unique value based on our pointer address.
  void set_root_id(const std::string& id) { root_id_ = id; }
  const std::string& root_id() const { return root_id_; }

 private:
  // kCheckEdits: Traverse and check if fields have changed, but don't commit
  // any changes.
  // kDraw*: Draw the Flatbuffer. ReadOnly means use Labels instead of Edit
  // fields. Manual means use Edit fields, but require the user to explicitly
  // save them back out to the Flatbuffer. Auto means automatically commit the
  // values into the Flatbuffer as you edit them.
  // kCommitEdits: Traverse, and if fields have changed, commit them to the
  // Flatbuffer.
  enum VisitMode {
    kCheckEdits,      // Only check if any fields have been modified.
    kDrawEditAuto,    // Draw using edit fields that auto-update the FB.
    kDrawEditManual,  // Draw using edit fields that manually update the FB.
    kDrawReadOnly,    // Draw using label fields, not editable
    kCommitEdits      // Write out edits to the Flatbuffer.
  };

  enum Button { kNone, kCommit, kRevert };

  // Copy the table using reflection and the existing schema and table def.
  void CopyTable(const void* src, std::vector<uint8_t>* dest);

  void ClearEditFields() {
    edit_fields_.clear();
    edit_fields_modified_ = false;
  }

  // This function takes the edit_fields_ that the user has been working on, and
  // writes them all out to the Flatbuffer.  This is an expensive operation as
  // it may require completely invalidating the existing Flatbuffer and copying
  // in a new one, so we only do this when the user chooses to commit their
  // edits.
  void CommitEditsToFlatbuffer();

  // Functions for traversing the Flatbuffer and drawing or editing its fields.

  // Visit a single field with the given name and value. The "id" should
  // uniquely identify it in the tree of data structures.
  //
  // If mode is kDrawEdit, the Flatbuffer field will be drawn using a FlatUI
  // Edit()
  // control. kDrawReadOnly is similar, but it will use a Label() control and
  // won't be editable.
  //
  // If mode is kEdit, it won't actually draw, but it apply any edits made from
  // the previously visited fields, hitting all of the previously calculated IDs
  // to apply the edits.
  //
  // Only returns true if mode = kEdit and if we applied any edits.
  bool VisitField(VisitMode mode, const std::string& name,
                  const std::string& value, const std::string& type,
                  const std::string& comment, const std::string& id);

  // Visit a subtable with the given name. The "id" should uniquely identify it
  // in the tree of data structures. If mode is kEdit, don't actually draw,
  // but do still propagate through the tree of data structures so we can apply
  // edits to the previously determined IDs. Returns true if kEdit and if we
  // applied any edits.
  bool VisitSubtable(VisitMode mode, const std::string& field,
                     const std::string& type, const std::string& comment,
                     const std::string& id, const reflection::Schema& schema,
                     const reflection::Object& subobjdef,
                     flatbuffers::Table& subtable);

  // Helper functions for traversing flatbuffers via reflection.
  // They may only return true if mode = kEdit and a field was edited in such a
  // way as to force the flatbuffer to be resized, which means you'll need to
  // start over and propagate edits again to catch the subsequent edits.
  bool VisitFlatbufferField(VisitMode mode, const reflection::Schema& schema,
                            const reflection::Field& fielddef,
                            const reflection::Object& objectdef,
                            flatbuffers::Table& table, const std::string& id);
  bool VisitFlatbufferScalar(VisitMode mode, const reflection::Schema& schema,
                             const reflection::Field& fielddef,
                             flatbuffers::Table& table, const std::string& id);
  bool VisitFlatbufferTable(VisitMode mode, const reflection::Schema& schema,
                            const reflection::Object& objectdef,
                            flatbuffers::Table& table, const std::string& id);
  bool VisitFlatbufferVector(VisitMode mode, const reflection::Schema& schema,
                             const reflection::Field& fielddef,
                             flatbuffers::Table& table, const std::string& id);
  bool VisitFlatbufferUnion(VisitMode mode, const reflection::Schema& schema,
                            const reflection::Field& fielddef,
                            const reflection::Object& objectdef,
                            flatbuffers::Table& table, const std::string& id);
  bool VisitFlatbufferStruct(VisitMode mode, const reflection::Schema& schema,
                             const reflection::Field& fielddef,
                             const reflection::Object& objectdef,
                             flatbuffers::Struct& fbstruct,
                             const std::string& id);
  bool VisitFlatbufferString(VisitMode mode, const reflection::Schema& schema,
                             const reflection::Field& fielddef,
                             flatbuffers::Table& table, const std::string& id);

  // Utility functions for dealing with Flatbuffer data. All of them are static.

  // Get the string representation of a Flatbuffers struct at a given pointer
  // location. For example a Vec3 with x = 1.2, y = 3.4, z = 5 would show up as
  // < 1.2, 3.4, 5 >. Set field_names_only = true to output the field names
  // instead.
  static std::string StructToString(const reflection::Schema& schema,
                                    const reflection::Object& objectdef,
                                    const flatbuffers::Struct& struct_ptr,
                                    bool field_names_only);

  // Parse a string that specifies a FlatBuffers struct in the format outputted
  // above. The format is "< 1, 2, < 3.4, 5, 6.7 >, 8 >". Each number must have
  // some combination of whitespace, comma, or angle brackets around it.
  // If you call this with a null struct_ptr it will just check whether your
  // string parses correctly.
  static bool ParseStringIntoStruct(const std::string& string,
                                    const reflection::Schema& schema,
                                    const reflection::Object& objectdef,
                                    flatbuffers::Struct* struct_ptr);

  // Extract an inline struct definition from a string containing a complex
  // struct definition that may contain nested struct definitions.
  //
  // str is a string that starts with '<'. Returns the string in between that
  // '<' and the matching '>' (exclusive), or empty string if there is a
  // mismatch.
  static std::string ExtractInlineStructDef(const std::string& str);

  // If this scalar value is an enum, get its type name and the name of its
  // value. Returns a string with the corrected integer value after parsing.
  static std::string GetEnumTypeAndValue(const reflection::Schema& schema,
                                         const reflection::Field& fielddef,
                                         const std::string& value,
                                         std::string* type,
                                         std::string* value_name);

  // UI helper functions

  // For a string for a field name, showing optional type.
  std::string FormatFieldName(const std::string& name, const std::string& type);

  // Draw a text button with the given text and the given ID.
  // Size is the vertical size of the button; the text will be smaller inside
  // that size.
  gui::Event TextButton(const char* text, const char* id, int size);

  // If mode is a draw mode, draw a button to add the current field to the
  // FlatBuffer. If mode is CommitEdits, then return true if this is the
  // node we want to commit, so the calling code can actually add the
  // field.
  bool AddFieldButton(VisitMode mode, const std::string& name,
                      const std::string& type_str, const std::string& id);

  void set_keyboard_in_use(bool b) { keyboard_in_use_ = b; }

  static bool IsDraw(VisitMode mode) {
    return (mode == kDrawEditAuto || mode == kDrawEditManual ||
            mode == kDrawReadOnly);
  }

  static bool IsDrawEdit(VisitMode mode) {
    return (mode == kDrawEditAuto || mode == kDrawEditManual);
  }

  const reflection::Schema& schema_;
  const reflection::Object& table_def_;
  std::unordered_map<std::string, std::string> edit_fields_;
  // List of table names we have expanded the view for.
  std::set<std::string> expanded_subtables_;
  // List of modified fields that were committed into the Flatbuffer. This is
  // cleared whenever ClearFlatbufferModifiedFlag() is called, as this assumes
  // that your code is now using the new Flatbuffer data.
  std::set<std::string> committed_fields_;
  // The actual Flatbuffer data.
  std::vector<uint8_t> flatbuffer_;
  // The root ID for our UI controls.
  std::string root_id_;
  std::string currently_editing_field_;  // What field has focus now?
  std::string force_commit_field_;  // If set, force this field to be committed
                                    // to the Flatbuffer next Update.

  // What on-screen button was pressed?
  Button button_pressed_;
  // UI settings.
  int ui_size_;            // Set to kDefaultUISize by default.
  int ui_spacing_;         // Set to kDefaultUISpacing by default.
  int blank_field_width_;  // How wide an edit area for blank strings?
  bool keyboard_in_use_;   // Is the keyboard in use?
  bool show_types_;        // Show type names?
  bool expand_all_;        // Expand all subtables?
  // Configuration settings, defaults taken from Flatbuffer.
  bool config_read_only_;    // If true, only draw and don't allow edits.
  bool config_auto_commit_;  // Auto-commit edited fields to the Flatbuffer.
  // Information about fields being edited.
  bool edit_fields_modified_;  // Have GUI edit fields been modified?
  bool flatbuffer_modified_;   // Has the Flatbuffer data been modified?

  // Temporary: Colors to use for rendering the Flatbuffer UI.
  // TODO: Move these to a standalone config structure.
  mathfu::vec4 bg_button_color_;
  mathfu::vec4 bg_button_hover_color_;
  mathfu::vec4 bg_button_click_color_;

  mathfu::vec4 text_button_color_;
  mathfu::vec4 text_normal_color_;
  mathfu::vec4 text_disabled_color_;
  mathfu::vec4 text_editable_color_;
  mathfu::vec4 text_modified_color_;
  mathfu::vec4 text_error_color_;
};

}  // namespace editor
}  // namespace fpl

#endif  // FPL_FLATBUFFER_EDITOR_H_
