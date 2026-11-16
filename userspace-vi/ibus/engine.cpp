// IBus adapter for the shared, dependency-free Telex/VNI composition engine.
#include "engine.hpp"

#include <new>
#include <string>

#include "yui/ime.hpp"

struct _YuiIBusEngine {
  IBusEngine parent;
  yui::ime::Composition* composition;
  gboolean method_selected;
  gboolean enabled;
};

struct _YuiIBusEngineClass {
  IBusEngineClass parent;
};

typedef struct _YuiIBusEngine YuiIBusEngine;
typedef struct _YuiIBusEngineClass YuiIBusEngineClass;

G_DEFINE_TYPE(YuiIBusEngine, yui_ibus_engine, IBUS_TYPE_ENGINE)

namespace {

YuiIBusEngine* self_of(IBusEngine* engine) {
  return reinterpret_cast<YuiIBusEngine*>(engine);
}

void select_method(YuiIBusEngine* self) {
  if (self->method_selected || !self->composition) return;
  const gchar* name = ibus_engine_get_name(IBUS_ENGINE(self));
  const yui::ime::Method method =
      g_strcmp0(name, "yui-vni") == 0 ? yui::ime::Method::Vni : yui::ime::Method::Telex;
  self->composition->set_method(method);
  self->method_selected = TRUE;
}

void hide_preedit(IBusEngine* engine) { ibus_engine_hide_preedit_text(engine); }

void clear_composition(YuiIBusEngine* self) {
  if (self->composition) self->composition->reset();
  hide_preedit(IBUS_ENGINE(self));
}

void update_preedit(YuiIBusEngine* self) {
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

void commit_composition(YuiIBusEngine* self) {
  if (!self->composition || self->composition->empty()) {
    hide_preedit(IBUS_ENGINE(self));
    return;
  }
  emit_commit(IBUS_ENGINE(self), self->composition->commit());
}

void restore_composition(YuiIBusEngine* self) {
  if (!self->composition || self->composition->empty()) {
    hide_preedit(IBUS_ENGINE(self));
    return;
  }
  emit_commit(IBUS_ENGINE(self), self->composition->restore());
}

gboolean has_shortcut_modifier(guint modifiers) {
  const guint shortcut_mask = IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK |
                              IBUS_HYPER_MASK | IBUS_META_MASK;
  return (modifiers & shortcut_mask) != 0;
}

gboolean forward_after_commit(YuiIBusEngine* self, guint keyval, guint keycode, guint modifiers) {
  if (!self->composition || self->composition->empty()) return FALSE;
  // Forward on the same IBus connection after commit so the boundary key (space,
  // punctuation, navigation, shortcut, …) cannot race the asynchronous commit.
  commit_composition(self);
  ibus_engine_forward_key_event(IBUS_ENGINE(self), keyval, keycode, modifiers);
  return TRUE;
}

gboolean process_key_event(IBusEngine* base, guint keyval, guint keycode, guint modifiers) {
  YuiIBusEngine* self = self_of(base);
  if (!self->composition || !self->enabled || (modifiers & IBUS_RELEASE_MASK)) return FALSE;
  select_method(self);

  const gboolean shortcut = has_shortcut_modifier(modifiers);
  if (!shortcut && keyval == IBUS_KEY_Escape && !self->composition->empty()) {
    restore_composition(self);
    return TRUE;
  }
  if (!shortcut && keyval == IBUS_KEY_BackSpace && !self->composition->empty()) {
    self->composition->backspace();
    update_preedit(self);
    return TRUE;
  }

  if (!shortcut && self->composition->feed(keyval)) {
    update_preedit(self);
    return TRUE;
  }

  return forward_after_commit(self, keyval, keycode, modifiers);
}

void reset_engine(IBusEngine* base) {
  YuiIBusEngine* self = self_of(base);
  clear_composition(self);
  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->reset) parent->reset(base);
}

void focus_out(IBusEngine* base) {
  YuiIBusEngine* self = self_of(base);
  commit_composition(self);
  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->focus_out) parent->focus_out(base);
}

// IBusEngineClass declares this virtual with guint arguments (not the
// IBusInputPurpose/IBusInputHints enum typedefs); keep the exact ABI signature
// so C++ builds don't need an unsafe function-pointer cast.
void set_content_type(IBusEngine* base, guint purpose, guint hints) {
  YuiIBusEngine* self = self_of(base);
  (void)hints;

  // Never transform or retain text in password, PIN, numeric, URL, or email
  // fields. Clear (do not commit) any pending preedit before switching modes.
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
      break;
    default:
      self->enabled = TRUE;
      break;
  }

  IBusEngineClass* parent = IBUS_ENGINE_CLASS(yui_ibus_engine_parent_class);
  if (parent->set_content_type) parent->set_content_type(base, purpose, hints);
}

void finalize(GObject* object) {
  YuiIBusEngine* self = reinterpret_cast<YuiIBusEngine*>(object);
  delete self->composition;
  self->composition = nullptr;

  GObjectClass* parent = G_OBJECT_CLASS(yui_ibus_engine_parent_class);
  if (parent->finalize) parent->finalize(object);
}

}  // namespace

static void yui_ibus_engine_class_init(YuiIBusEngineClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = finalize;

  IBusEngineClass* engine_class = IBUS_ENGINE_CLASS(klass);
  engine_class->process_key_event = process_key_event;
  engine_class->focus_out = focus_out;
  engine_class->reset = reset_engine;
  engine_class->set_content_type = set_content_type;
}

static void yui_ibus_engine_init(YuiIBusEngine* self) {
  self->composition = new (std::nothrow) yui::ime::Composition();
  self->method_selected = FALSE;
  self->enabled = TRUE;
}
