// IBus component process. The same service exposes separate Telex and VNI engines.
#include <ibus.h>

#include "engine.hpp"

int main(int argc, char** argv) {
  ibus_init();
  IBusBus* bus = ibus_bus_new();
  if (!bus || !ibus_bus_is_connected(bus)) {
    g_printerr("yui-ibus-engine: cannot connect to the IBus daemon\n");
    if (bus) g_object_unref(bus);
    return 1;
  }

  IBusFactory* factory = ibus_factory_new(ibus_bus_get_connection(bus));
  if (!factory) {
    g_printerr("yui-ibus-engine: cannot create the engine factory\n");
    g_object_unref(bus);
    return 1;
  }
  ibus_factory_add_engine(factory, "yui-telex", YUI_TYPE_IBUS_ENGINE);
  ibus_factory_add_engine(factory, "yui-vni", YUI_TYPE_IBUS_ENGINE);

  const guint32 owner = ibus_bus_request_name(bus, "org.freedesktop.IBus.Yui", 0);
  if (owner == 0) {
    g_printerr("yui-ibus-engine: cannot register org.freedesktop.IBus.Yui\n");
    g_object_unref(factory);
    g_object_unref(bus);
    return 1;
  }

  ibus_main();
  g_object_unref(factory);
  g_object_unref(bus);
  return 0;
}
