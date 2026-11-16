// IBus adapter for the shared, dependency-free Telex/VNI composition engine.
#include "engine.hpp"

#include <new>
#include <string>

#include "yui/ime.hpp"
#include "yui/text.hpp"

struct _YuiIBusEngine {
  IBusEngine parent;
  yui::ime::Composition* composition;
  yui::ime::DirectComposition* direct_composition;
  IBusProperty* mode_property;
  gboolean method_selected;
  gboolean enabled;
  gboolean direct_mode;
  gboolean properties_registered;
  gboolean surrounding_valid;
  guint expected_cursor;
  guint expected_anchor;
};

struct _YuiIBusEngineClass {
  IBusEngineClass parent;
};

typedef struct _YuiIBusEngine YuiIBusEngine;
typedef struct _YuiIBusEngineClass YuiIBusEngineClass;

G_DEFINE_TYPE(YuiIBusEngine, yui_ibus_engine, IBUS_TYPE_ENGINE)

namespace {

constexpr const char* kModeProperty = "input-mode";

YuiIBusEngine* self_of(IBusEngine* engine) {
  return reinterpret_cast<YuiIBusEngine*>(engine);
}

void select_method(YuiIBusEngine* self) {
  if (self->method_selected || !self->composition || !self->direct_composition) return;
  const gchar* name = ibus_engine_get_name(IBUS_ENGINE(self));
  const yui::ime::Method method =
      g_strcmp0(name, "yui-vni") == 0 ? yui::ime::Method::Vni : yui::ime::Method::Telex;
  self->composition->set_method(method);
  self->direct_composition->set_method(method);
  self->method_selected = TRUE;
}

void hide_preedit(IBusEngine* engine) { ibus_engine_hide_preedit_text(engine); }

bool composition_empty(const YuiIBusEngine* self) {
  if (self->direct_mode)
    return !self->direct_composition || self->direct_composition->empty();
  return !self->composition || self->composition->empty();
}

void clear_composition(YuiIBusEngine* self) {
  if (self->composition) self->composition->reset();
  if (self->direct_composition) self->direct_composition->reset();
  self->surrounding_valid = FALSE;
  hide_preedit(IBUS_ENGINE(self));
}

void update_preedit(YuiIBusEngine* self) {
  if (self->direct_mode) {
    hide_preedit(IBUS_ENGINE(self));
    return;
  }
  if (!self->composition || self->composition->empty()) {
    hide_preedit(IBUS_ENGINE(self));
    return;
  }

  const std::string value = self->composition->preedit();
  IBusText* text = ibus_text_new_from_string(value.c_str());
  if (!text) {
    clear_composition(self);
    return;
  }
  // IBus consumes floating IBusText instances. Sink explicitly so we can keep
  // ordinary RAII-style ownership and release our reference after the call.
  g_object_ref_sink(text);
  const guint cursor = static_cast<guint>(g_utf8_strlen(value.c_str(), -1));
  ibus_engine_update_preedit_text(IBUS_ENGINE(self), text, cursor, TRUE);
  g_object_unref(text);
}

void emit_commit(IBusEngine* engine, const std::string& value) {
  if (value.empty()) {
    hide_preedit(engine);
    return;
  }

  IBusText* text = ibus_text_new_from_string(value.c_str());
  if (!text) return;
  // Keep one owned reference: the IBus API consumes floating objects.
  g_object_ref_sink(text);
  ibus_engine_hide_preedit_text(engine);
  ibus_engine_commit_text(engine, text);
  g_object_unref(text);
}

void note_replay_edit(YuiIBusEngine* self, const yui::ime::ReplayEdit& edit) {
  if (!self->surrounding_valid) return;
  if (edit.backspaces > self->expected_cursor) {
    // The surrounding-text window is too short to predict a safe cursor index.
    // Continue editing the current word, but wait for a fresh client update.
    self->surrounding_valid = FALSE;
    return;
  }
  self->expected_cursor -= static_cast<guint>(edit.backspaces);
  self->expected_cursor += static_cast<guint>(yui::text::length(edit.text));
  self->expected_anchor = self->expected_cursor;
}

void emit_replay_edit(YuiIBusEngine* self, const yui::ime::ReplayEdit& edit) {
  IBusEngine* engine = IBUS_ENGINE(self);
  note_replay_edit(self, edit);
  // Keep the exact requested order: delete the old rendered composition, then
  // commit the full replacement. Each call is one BackSpace key-press event.
  for (size_t i = 0; i < edit.backspaces; ++i)
    ibus_engine_forward_key_event(engine, IBUS_KEY_BackSpace, 0, 0);
  if (!edit.text.empty())
    emit_commit(engine, edit.text);
  else
    hide_preedit(engine);
}

void commit_composition(YuiIBusEngine* self) {
  if (composition_empty(self)) {
    hide_preedit(IBUS_ENGINE(self));
    return;
  }
  if (self->direct_mode) {
    emit_replay_edit(self, self->direct_composition->commit());
  } else {
    emit_commit(IBUS_ENGINE(self), self->composition->commit());
  }
}

void restore_composition(YuiIBusEngine* self) {
  if (composition_empty(self)) {
    hide_preedit(IBUS_ENGINE(self));
    return;
  }
  if (self->direct_mode) {
    emit_replay_edit(self, self->direct_composition->restore());
  } else {
    emit_commit(IBUS_ENGINE(self), self->composition->restore());
  }
}

gboolean has_shortcut_modifier(guint modifiers) {
  const guint shortcut_mask = IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK |
                              IBUS_HYPER_MASK | IBUS_META_MASK;
  return (modifiers & shortcut_mask) != 0;
}

void note_forwarded_boundary(YuiIBusEngine* self, guint keyval, guint modifiers) {
  if (!self->surrounding_valid) return;
  if (has_shortcut_modifier(modifiers)) {
    self->surrounding_valid = FALSE;
    return;
  }

  const gunichar codepoint = ibus_keyval_to_unicode(keyval);
  if (codepoint != 0 && g_unichar_isprint(codepoint)) {
    ++self->expected_cursor;
    self->expected_anchor = self->expected_cursor;
  } else {
    // Cursor movement, deletion, and non-text actions cannot be predicted from
    // the keyval alone; wait for the client's next surrounding-text update.
    self->surrounding_valid = FALSE;
  }
}

gboolean forward_after_commit(YuiIBusEngine* self, guint keyval, guint keycode, guint modifiers) {
  if (composition_empty(self)) return FALSE;
  // Forward only after the active composition is finalized. This makes spaces,
  // punctuation, navigation, and shortcut keys hard replay boundaries.
  commit_composition(self);
  note_forwarded_boundary(self, keyval, modifiers);
  ibus_engine_forward_key_event(IBUS_ENGINE(self), keyval, keycode, modifiers);
  return TRUE;
}

void update_mode_property(YuiIBusEngine* self) {
  if (!self->mode_property) return;
  const char* label_string = self->direct_mode ? "Input mode: Direct" : "Input mode: Preedit";
  const char* tooltip_string = self->direct_mode
                                   ? "Direct input (Backspace replay); click to use Preedit"
                                   : "Preedit input; click to use Direct input";
  IBusText* label = ibus_text_new_from_string(label_string);
  IBusText* tooltip = ibus_text_new_from_string(tooltip_string);
  if (label) {
    g_object_ref_sink(label);
    ibus_property_set_label(self->mode_property, label);
    g_object_unref(label);
  }
  if (tooltip) {
    g_object_ref_sink(tooltip);
    ibus_property_set_tooltip(self->mode_property, tooltip);
    g_object_unref(tooltip);
  }
  ibus_property_set_state(self->mode_property,
                          self->direct_mode ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);
  if (self->properties_registered)
    ibus_engine_update_property(IBUS_ENGINE(self), self->mode_property);
}

void set_mode(YuiIBusEngine* self, gboolean direct) {
  if (self->direct_mode == direct) {
    update_mode_property(self);
    return;
  }
  // Finish the previous mode before changing bookkeeping. In Direct mode this
  // is normally a no-op replacement, but it also resets the raw keystroke word.
  commit_composition(self);
  self->direct_mode = direct;
  self->surrounding_valid = FALSE;
  if (direct) hide_preedit(IBUS_ENGINE(self));
  update_mode_property(self);
}

gboolean process_key_event(IBusEngine* base, guint keyval, guint keycode, guint modifiers) {
  YuiIBusEngine* self = self_of(base);
  if (!self->composition || !self->direct_composition || !self->enabled ||
      (modifiers & IBUS_RELEASE_MASK))
    return FALSE;
  select_method(self);

  const gboolean shortcut = has_shortcut_modifier(modifiers);
  if (!shortcut && keyval == IBUS_KEY_Escape && !composition_empty(self)) {
    restore_composition(self);
    return TRUE;
  }
  if (!shortcut && keyval == IBUS_KEY_BackSpace && !composition_empty(self)) {
    if (self->direct_mode) {
      yui::ime::ReplayEdit edit;
      if (self->direct_composition->backspace(&edit)) emit_replay_edit(self, edit);
    } else {
      self->composition->backspace();
      update_preedit(self);
    }
    return TRUE;
  }

  if (!shortcut) {
    if (self->direct_mode) {
      yui::ime::ReplayEdit edit;
      if (self->direct_composition->feed(keyval, &edit)) {
        emit_replay_edit(self, edit);
        return TRUE;
      }
    } else if (self->composition->feed(keyval)) {
      update_preedit(self);
      return TRUE;
    }
  }

  return forward_after_commit(self, keyval, keycode, modifiers);
}

void reset_engine(IBusEngine* base) {
  YuiIBusEngine* self = self_of(base);
  clear_composition(self);
  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->reset) parent->reset(base);
}

void focus_in(IBusEngine* base) {
  YuiIBusEngine* self = self_of(base);
  select_method(self);
  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->focus_in) parent->focus_in(base);
}

void focus_out(IBusEngine* base) {
  YuiIBusEngine* self = self_of(base);
  commit_composition(self);
  self->surrounding_valid = FALSE;
  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->focus_out) parent->focus_out(base);
}

void enable_engine(IBusEngine* base) {
  YuiIBusEngine* self = self_of(base);
  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->enable) parent->enable(base);

  if (!self->properties_registered && self->mode_property) {
    IBusPropList* properties = ibus_prop_list_new();
    if (properties) {
      ibus_prop_list_append(properties, self->mode_property);
      ibus_engine_register_properties(base, properties);
      self->properties_registered = TRUE;
    }
  }
  // Request surrounding text so Direct mode can detect a cursor/selection move
  // (including a mouse click) before it replays Backspace events.
  ibus_engine_get_surrounding_text(base, nullptr, nullptr, nullptr);
}

// IBusEngineClass declares this virtual with guint arguments (not the
// IBusInputPurpose/IBusInputHints enum typedefs); keep the exact ABI signature.
void set_content_type(IBusEngine* base, guint purpose, guint hints) {
  YuiIBusEngine* self = self_of(base);
  (void)hints;

  // Never transform or retain text in password, PIN, numeric, URL, or email
  // fields. Clear (do not commit) any pending composition before switching modes.
  switch (static_cast<IBusInputPurpose>(purpose)) {
    case IBUS_INPUT_PURPOSE_DIGITS:
    case IBUS_INPUT_PURPOSE_NUMBER:
    case IBUS_INPUT_PURPOSE_PHONE:
    case IBUS_INPUT_PURPOSE_URL:
    case IBUS_INPUT_PURPOSE_EMAIL:
    case IBUS_INPUT_PURPOSE_PASSWORD:
    case IBUS_INPUT_PURPOSE_PIN:
      self->enabled = FALSE;
      clear_composition(self);
      self->surrounding_valid = FALSE;
      break;
    default:
      self->enabled = TRUE;
      break;
  }

  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->set_content_type) parent->set_content_type(base, purpose, hints);
}

void set_surrounding_text(IBusEngine* base, IBusText* text, guint cursor_index, guint anchor_pos) {
  YuiIBusEngine* self = self_of(base);
  (void)text;

  if (self->direct_mode && self->direct_composition && !self->direct_composition->empty()) {
    const gboolean unexpected_cursor =
        self->surrounding_valid &&
        (cursor_index != self->expected_cursor || anchor_pos != self->expected_anchor);
    const gboolean selection_active = cursor_index != anchor_pos;
    if (unexpected_cursor || selection_active) {
      // The already committed word stays in the application, but no future
      // replay is allowed to reach back across the user's new cursor/selection.
      self->direct_composition->reset();
    }
  }
  self->expected_cursor = cursor_index;
  self->expected_anchor = anchor_pos;
  self->surrounding_valid = TRUE;

  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->set_surrounding_text)
    parent->set_surrounding_text(base, text, cursor_index, anchor_pos);
}

void property_activate(IBusEngine* base, const gchar* prop_name, guint prop_state) {
  YuiIBusEngine* self = self_of(base);
  (void)prop_state;
  if (g_strcmp0(prop_name, kModeProperty) == 0) set_mode(self, !self->direct_mode);

  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->property_activate) parent->property_activate(base, prop_name, prop_state);
}

IBusProperty* new_mode_property(gboolean direct) {
  const char* label_string = direct ? "Input mode: Direct" : "Input mode: Preedit";
  const char* tooltip_string = direct
                                   ? "Direct input (Backspace replay); click to use Preedit"
                                   : "Preedit input; click to use Direct input";
  IBusText* label = ibus_text_new_from_string(label_string);
  IBusText* tooltip = ibus_text_new_from_string(tooltip_string);
  if (label) g_object_ref_sink(label);
  if (tooltip) g_object_ref_sink(tooltip);
  IBusProperty* property = ibus_property_new(
      kModeProperty, PROP_TYPE_TOGGLE, label, nullptr, tooltip, TRUE, TRUE,
      direct ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED, nullptr);
  if (label) g_object_unref(label);
  if (tooltip) g_object_unref(tooltip);
  if (property) g_object_ref_sink(property);
  return property;
}

void finalize(GObject* object) {
  YuiIBusEngine* self = reinterpret_cast<YuiIBusEngine*>(object);
  delete self->composition;
  self->composition = nullptr;
  delete self->direct_composition;
  self->direct_composition = nullptr;
  if (self->mode_property) {
    g_object_unref(self->mode_property);
    self->mode_property = nullptr;
  }

  GObjectClass* parent = G_OBJECT_CLASS(yui_ibus_engine_parent_class);
  if (parent->finalize) parent->finalize(object);
}

}  // namespace

static void yui_ibus_engine_class_init(YuiIBusEngineClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = finalize;

  IBusEngineClass* engine_class = IBUS_ENGINE_CLASS(klass);
  engine_class->process_key_event = process_key_event;
  engine_class->focus_in = focus_in;
  engine_class->focus_out = focus_out;
  engine_class->reset = reset_engine;
  engine_class->enable = enable_engine;
  engine_class->set_content_type = set_content_type;
  engine_class->set_surrounding_text = set_surrounding_text;
  engine_class->property_activate = property_activate;
}

static void yui_ibus_engine_init(YuiIBusEngine* self) {
  self->composition = new (std::nothrow) yui::ime::Composition();
  self->direct_composition = new (std::nothrow) yui::ime::DirectComposition();
  self->mode_property = nullptr;
  self->method_selected = FALSE;
  self->enabled = TRUE;
  self->direct_mode = g_strcmp0(g_getenv("YUI_IME_MODE"), "direct") == 0;
  self->properties_registered = FALSE;
  self->surrounding_valid = FALSE;
  self->expected_cursor = 0;
  self->expected_anchor = 0;
  self->mode_property = new_mode_property(self->direct_mode);
}
