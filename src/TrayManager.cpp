#include "TrayManager.hpp"

#include <unistd.h>

#include <iostream>

#include "Util.hpp"

const char* SNI_INTERFACE_XML =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='WindowId' type='i' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='IconThemePath' type='s' access='read'/>"
    "    <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "    <method name='ContextMenu'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='Activate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='SecondaryActivate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='Scroll'>"
    "      <arg name='delta' type='i' direction='in'/>"
    "      <arg name='orientation' type='s' direction='in'/>"
    "    </method>"
    "    <signal name='NewTitle'/>"
    "    <signal name='NewIcon'/>"
    "    <signal name='NewStatus'>"
    "      <arg name='status' type='s'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

const char* DBUSMENU_INTERFACE_XML =
    "<node>"
    "  <interface name='com.canonical.dbusmenu'>"
    "    <property name='Version' type='u' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <method name='GetLayout'>"
    "      <arg name='parentId' type='i' direction='in'/>"
    "      <arg name='recursionDepth' type='i' direction='in'/>"
    "      <arg name='propertyNames' type='as' direction='in'/>"
    "      <arg name='revision' type='u' direction='out'/>"
    "      <arg name='layout' type='(ia{sv}av)' direction='out'/>"
    "    </method>"
    "    <method name='GetGroupProperties'>"
    "      <arg name='ids' type='ai' direction='in'/>"
    "      <arg name='propertyNames' type='as' direction='in'/>"
    "      <arg name='properties' type='a(ia{sv})' direction='out'/>"
    "    </method>"
    "    <method name='GetProperty'>"
    "      <arg name='id' type='i' direction='in'/>"
    "      <arg name='name' type='s' direction='in'/>"
    "      <arg name='value' type='v' direction='out'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg name='id' type='i' direction='in'/>"
    "      <arg name='eventId' type='s' direction='in'/>"
    "      <arg name='data' type='v' direction='in'/>"
    "      <arg name='timestamp' type='u' direction='in'/>"
    "    </method>"
    "    <method name='EventGroup'>"
    "      <arg name='events' type='a(isvu)' direction='in'/>"
    "      <arg name='idErrors' type='ai' direction='out'/>"
    "    </method>"
    "    <method name='AboutToShow'>"
    "      <arg name='id' type='i' direction='in'/>"
    "      <arg name='needUpdate' type='b' direction='out'/>"
    "    </method>"
    "    <signal name='ItemsPropertiesUpdated'>"
    "      <arg name='updatedProps' type='a(ia{sv})'/>"
    "      <arg name='removedProps' type='a(ias)'/>"
    "    </signal>"
    "    <signal name='LayoutUpdated'>"
    "      <arg name='revision' type='u'/>"
    "      <arg name='parent' type='i'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

TrayManager::TrayManager(AddMonitorCallback onAdd, RemoveMonitorCallback onRemove) : addCallback(std::move(onAdd)), removeCallback(std::move(onRemove)) {}

TrayManager::~TrayManager() { stop(); }

bool TrayManager::init() {
    GError* error = nullptr;
    dbusConn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!dbusConn) {
        LOG_ERROR << "Failed to connect to D-Bus session bus: " << (error ? error->message : "unknown") << std::endl;
        if (error) g_error_free(error);
        return false;
    }

    GDBusNodeInfo* sniNodeInfo = g_dbus_node_info_new_for_xml(SNI_INTERFACE_XML, &error);
    if (!sniNodeInfo) {
        LOG_ERROR << "Failed to parse SNI XML: " << (error ? error->message : "unknown") << std::endl;
        if (error) g_error_free(error);
        return false;
    }

    static const GDBusInterfaceVTable sniVTable = {handleSniMethodCall, handleSniGetProperty, nullptr, {}};

    sniRegId = g_dbus_connection_register_object(dbusConn, "/StatusNotifierItem", sniNodeInfo->interfaces[0], &sniVTable, this, nullptr, &error);
    g_dbus_node_info_unref(sniNodeInfo);

    if (sniRegId == 0) {
        LOG_ERROR << "Failed to register SNI object: " << (error ? error->message : "unknown") << std::endl;
        if (error) g_error_free(error);
        return false;
    }

    GDBusNodeInfo* menuNodeInfo = g_dbus_node_info_new_for_xml(DBUSMENU_INTERFACE_XML, &error);
    if (!menuNodeInfo) {
        LOG_ERROR << "Failed to parse DBusMenu XML: " << (error ? error->message : "unknown") << std::endl;
        if (error) g_error_free(error);
        return false;
    }

    static const GDBusInterfaceVTable menuVTable = {handleMenuMethodCall, nullptr, nullptr, {}};

    menuRegId = g_dbus_connection_register_object(dbusConn, "/MenuBar", menuNodeInfo->interfaces[0], &menuVTable, this, nullptr, &error);
    g_dbus_node_info_unref(menuNodeInfo);

    if (menuRegId == 0) {
        LOG_ERROR << "Failed to register DBusMenu object: " << (error ? error->message : "unknown") << std::endl;
        if (error) g_error_free(error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(menuMutex);
        rebuildMenuTree();
    }

    registerWithWatcher();

    LOG << "TrayManager initialized with native GDBus StatusNotifierItem." << std::endl;
    return true;
}

void TrayManager::processEvents() {
    while (g_main_context_iteration(nullptr, FALSE)) {
        // Drain pending D-Bus events
    }
}

void TrayManager::stop() {
    if (dbusConn) {
        if (sniRegId > 0) {
            g_dbus_connection_unregister_object(dbusConn, sniRegId);
            sniRegId = 0;
        }
        if (menuRegId > 0) {
            g_dbus_connection_unregister_object(dbusConn, menuRegId);
            menuRegId = 0;
        }
        g_object_unref(dbusConn);
        dbusConn = nullptr;
    }
}

void TrayManager::registerWithWatcher() {
    if (!dbusConn) return;

    g_dbus_connection_call(
        dbusConn, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem", g_variant_new("(s)", "/StatusNotifierItem"), nullptr,
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject*, GAsyncResult* res, gpointer) {
            GError* err = nullptr;
            GVariant* ret = g_dbus_connection_call_finish(nullptr, res, &err);
            if (ret) {
                g_variant_unref(ret);
            }
            if (err) {
                LOG << "Note: StatusNotifierWatcher registration response: " << err->message << std::endl;
                g_error_free(err);
            }
        },
        nullptr);
}

void TrayManager::updateMonitors(const std::vector<SharedMonitorItem>& activeMonitors) {
    std::lock_guard<std::mutex> lock(menuMutex);
    if (currentMonitors == activeMonitors) {
        return;
    }
    currentMonitors = activeMonitors;
    rebuildMenuTree();
    emitLayoutUpdated();
}

void TrayManager::setManifestInstalled(bool installed) {
    std::lock_guard<std::mutex> lock(menuMutex);
    if (manifestInstalled == installed) {
        return;
    }
    manifestInstalled = installed;
    rebuildMenuTree();
    emitLayoutUpdated();
}

void TrayManager::emitLayoutUpdated() {
    if (!dbusConn || menuRegId == 0) return;
    menuRevision++;
    GError* error = nullptr;
    g_dbus_connection_emit_signal(dbusConn, nullptr, "/MenuBar", "com.canonical.dbusmenu", "LayoutUpdated", g_variant_new("(ui)", menuRevision, 0), &error);
    if (error) {
        LOG_ERROR << "Failed to emit LayoutUpdated signal: " << error->message << std::endl;
        g_error_free(error);
    }
}

void TrayManager::rebuildMenuTree() {
    rootMenu = MenuItem{};
    rootMenu.id = 0;

    MenuItem titleItem;
    titleItem.id = nextMenuItemId++;
    titleItem.label = "OpenVR Wayland Desktop";
    titleItem.enabled = false;
    rootMenu.children.push_back(titleItem);

    MenuItem sep1;
    sep1.id = nextMenuItemId++;
    sep1.isSeparator = true;
    rootMenu.children.push_back(sep1);

    if (currentMonitors.empty()) {
        MenuItem noMonItem;
        noMonItem.id = nextMenuItemId++;
        noMonItem.label = "(No active monitors)";
        noMonItem.enabled = false;
        rootMenu.children.push_back(noMonItem);
    } else {
        for (const auto& mon : currentMonitors) {
            std::string statusStr;
            if (mon.isStreaming) {
                statusStr = mon.isOverlayVisible ? " (Streaming)" : " (Standby)";
            } else {
                statusStr = " (Connecting)";
            }

            MenuItem monMenu;
            monMenu.id = nextMenuItemId++;
            monMenu.label = mon.name + statusStr;
            monMenu.iconName = "video-display";
            monMenu.monitorId = mon.id;

            MenuItem dmaBufItem;
            dmaBufItem.id = nextMenuItemId++;
            dmaBufItem.label = "Enable DMA-BUF";
            dmaBufItem.action = "toggle_monitor_dmabuf";
            dmaBufItem.monitorId = mon.id;
            dmaBufItem.toggleType = "checkmark";
            dmaBufItem.toggleState = mon.enableDmaBuf ? 1 : 0;
            monMenu.children.push_back(dmaBufItem);

            MenuItem removeMonItem;
            removeMonItem.id = nextMenuItemId++;
            removeMonItem.label = "Delete " + mon.name;
            removeMonItem.iconName = "list-remove";
            removeMonItem.action = "remove_monitor";
            removeMonItem.monitorId = mon.id;
            monMenu.children.push_back(removeMonItem);

            rootMenu.children.push_back(monMenu);
        }
    }

    MenuItem sep2;
    sep2.id = nextMenuItemId++;
    sep2.isSeparator = true;
    rootMenu.children.push_back(sep2);

    MenuItem addItem;
    addItem.id = nextMenuItemId++;
    addItem.label = "Add Monitor";
    addItem.iconName = "list-add";
    addItem.action = "add_monitor";
    rootMenu.children.push_back(addItem);

    MenuItem manifestItem;
    manifestItem.id = nextMenuItemId++;
    manifestItem.label = "Install as OpenVR App";
    manifestItem.action = "toggle_manifest";
    manifestItem.toggleType = "checkmark";
    manifestItem.toggleState = manifestInstalled ? 1 : 0;
    rootMenu.children.push_back(manifestItem);

    MenuItem sep3;
    sep3.id = nextMenuItemId++;
    sep3.isSeparator = true;
    rootMenu.children.push_back(sep3);

    MenuItem quitItem;
    quitItem.id = nextMenuItemId++;
    quitItem.label = "Quit";
    quitItem.iconName = "application-exit";
    quitItem.action = "quit";
    rootMenu.children.push_back(quitItem);
}

void TrayManager::collectAllProperties(GVariantBuilder* builder, const MenuItem& item) {
    GVariant* props = buildPropertiesVariant(item);
    g_variant_builder_add(builder, "(i@a{sv})", item.id, props);
    for (const auto& child : item.children) {
        collectAllProperties(builder, child);
    }
}

GVariant* TrayManager::buildPropertiesVariant(const MenuItem& item) {
    GVariantBuilder dictBuilder;
    g_variant_builder_init(&dictBuilder, G_VARIANT_TYPE("a{sv}"));

    if (item.isSeparator) {
        g_variant_builder_add(&dictBuilder, "{sv}", "type", g_variant_new_string("separator"));
    } else {
        if (!item.label.empty()) {
            g_variant_builder_add(&dictBuilder, "{sv}", "label", g_variant_new_string(item.label.c_str()));
        }
        if (!item.iconName.empty()) {
            g_variant_builder_add(&dictBuilder, "{sv}", "icon-name", g_variant_new_string(item.iconName.c_str()));
        }
        g_variant_builder_add(&dictBuilder, "{sv}", "enabled", g_variant_new_boolean(item.enabled));
        g_variant_builder_add(&dictBuilder, "{sv}", "visible", g_variant_new_boolean(item.visible));

        if (!item.toggleType.empty()) {
            g_variant_builder_add(&dictBuilder, "{sv}", "toggle-type", g_variant_new_string(item.toggleType.c_str()));
            g_variant_builder_add(&dictBuilder, "{sv}", "toggle-state", g_variant_new_int32(item.toggleState));
        }

        if (!item.children.empty()) {
            g_variant_builder_add(&dictBuilder, "{sv}", "children-display", g_variant_new_string("submenu"));
        }
    }

    return g_variant_builder_end(&dictBuilder);
}

GVariant* TrayManager::buildLayoutVariant(const MenuItem& item, int recursionDepth) {
    GVariant* dictVariant = buildPropertiesVariant(item);

    GVariantBuilder childrenBuilder;
    g_variant_builder_init(&childrenBuilder, G_VARIANT_TYPE("av"));

    if (recursionDepth != 0) {
        int nextDepth = (recursionDepth > 0) ? recursionDepth - 1 : -1;
        for (const auto& child : item.children) {
            GVariant* childLayout = buildLayoutVariant(child, nextDepth);
            g_variant_builder_add(&childrenBuilder, "v", childLayout);
        }
    }

    GVariant* childrenVariant = g_variant_builder_end(&childrenBuilder);

    return g_variant_new("(i@a{sv}@av)", item.id, dictVariant, childrenVariant);
}

const TrayManager::MenuItem* TrayManager::findMenuItemById(const MenuItem& current, int id) {
    if (current.id == id) {
        return &current;
    }
    for (const auto& child : current.children) {
        const MenuItem* found = findMenuItemById(child, id);
        if (found) return found;
    }
    return nullptr;
}

void TrayManager::handleItemClick(const MenuItem& item) {
    if (item.action == "add_monitor") {
        if (addCallback) addCallback();
    } else if (item.action == "remove_monitor") {
        if (removeCallback) removeCallback(item.monitorId);
    } else if (item.action == "toggle_monitor_dmabuf") {
        if (monitorDmaBufToggleCallback) {
            monitorDmaBufToggleCallback(item.monitorId, item.toggleState == 0);
        }
    } else if (item.action == "toggle_manifest") {
        if (manifestToggleCallback) {
            manifestToggleCallback(item.toggleState == 0);
        }
    } else if (item.action == "quit") {
        if (quitCallback) quitCallback();
    }
}

// StatusNotifierItem VTable Callbacks
void TrayManager::handleSniMethodCall(GDBusConnection* /*connection*/, const gchar* /*sender*/, const gchar* /*object_path*/, const gchar* /*interface_name*/, const gchar* /*method_name*/,
                                      GVariant* /*parameters*/, GDBusMethodInvocation* invocation, gpointer /*user_data*/) {
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* TrayManager::handleSniGetProperty(GDBusConnection* /*connection*/, const gchar* /*sender*/, const gchar* /*object_path*/, const gchar* /*interface_name*/, const gchar* property_name,
                                            GError** /*error*/, gpointer /*user_data*/) {
    if (g_strcmp0(property_name, "Category") == 0) {
        return g_variant_new_string("ApplicationStatus");
    } else if (g_strcmp0(property_name, "Id") == 0) {
        return g_variant_new_string("openvr_desktop_overlay");
    } else if (g_strcmp0(property_name, "Title") == 0) {
        return g_variant_new_string("OpenVR Wayland Desktop");
    } else if (g_strcmp0(property_name, "Status") == 0) {
        return g_variant_new_string("Active");
    } else if (g_strcmp0(property_name, "WindowId") == 0) {
        return g_variant_new_int32(0);
    } else if (g_strcmp0(property_name, "IconName") == 0) {
        return g_variant_new_string("openvr_desktop_overlay");
    } else if (g_strcmp0(property_name, "IconThemePath") == 0) {
        std::string configDir = Util::getConfigDirectory();
        return g_variant_new_string(configDir.c_str());
    } else if (g_strcmp0(property_name, "IconPixmap") == 0) {
        GVariantBuilder arrayBuilder;
        g_variant_builder_init(&arrayBuilder, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_builder_end(&arrayBuilder);
    } else if (g_strcmp0(property_name, "Menu") == 0) {
        return g_variant_new_object_path("/MenuBar");
    } else if (g_strcmp0(property_name, "ItemIsMenu") == 0) {
        return g_variant_new_boolean(TRUE);
    } else if (g_strcmp0(property_name, "ToolTip") == 0) {
        GVariantBuilder iconArray;
        g_variant_builder_init(&iconArray, G_VARIANT_TYPE("a(iiay)"));
        GVariant* emptyIcons = g_variant_builder_end(&iconArray);
        return g_variant_new("(s@a(iiay)ss)", "", emptyIcons, "OpenVR Wayland Desktop", "Wayland VR Desktop Streaming");
    }
    return nullptr;
}

// DBusMenu VTable Callbacks
void TrayManager::handleMenuMethodCall(GDBusConnection* /*connection*/, const gchar* /*sender*/, const gchar* /*object_path*/, const gchar* /*interface_name*/, const gchar* method_name,
                                       GVariant* parameters, GDBusMethodInvocation* invocation, gpointer user_data) {
    auto* self = static_cast<TrayManager*>(user_data);
    MenuItem itemToClick;
    bool hasItemToClick = false;

    {
        std::lock_guard<std::mutex> lock(self->menuMutex);

        if (g_strcmp0(method_name, "GetLayout") == 0) {
            gint32 parentId = 0;
            gint32 recursionDepth = -1;
            GVariantIter* propertyNamesIter = nullptr;
            g_variant_get(parameters, "(iias)", &parentId, &recursionDepth, &propertyNamesIter);
            if (propertyNamesIter) g_variant_iter_free(propertyNamesIter);

            const MenuItem* target = (parentId == 0) ? &self->rootMenu : self->findMenuItemById(self->rootMenu, parentId);
            if (!target) {
                target = &self->rootMenu;
            }

            GVariant* layoutVariant = self->buildLayoutVariant(*target, recursionDepth);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", self->menuRevision, layoutVariant));
            return;
        } else if (g_strcmp0(method_name, "GetGroupProperties") == 0) {
            GVariantIter* idsIter = nullptr;
            GVariantIter* propNamesIter = nullptr;
            g_variant_get(parameters, "(aias)", &idsIter, &propNamesIter);

            GVariantBuilder groupBuilder;
            g_variant_builder_init(&groupBuilder, G_VARIANT_TYPE("a(ia{sv})"));

            if (idsIter && g_variant_iter_n_children(idsIter) > 0) {
                gint32 itemId = 0;
                while (g_variant_iter_next(idsIter, "i", &itemId)) {
                    const MenuItem* item = (itemId == 0) ? &self->rootMenu : self->findMenuItemById(self->rootMenu, itemId);
                    if (item) {
                        GVariant* props = self->buildPropertiesVariant(*item);
                        g_variant_builder_add(&groupBuilder, "(i@a{sv})", itemId, props);
                    }
                }
            } else {
                self->collectAllProperties(&groupBuilder, self->rootMenu);
            }

            if (idsIter) g_variant_iter_free(idsIter);
            if (propNamesIter) g_variant_iter_free(propNamesIter);

            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&groupBuilder)));
            return;
        } else if (g_strcmp0(method_name, "GetProperty") == 0) {
            gint32 id = 0;
            const gchar* name = nullptr;
            g_variant_get(parameters, "(is)", &id, &name);
            const MenuItem* item = (id == 0) ? &self->rootMenu : self->findMenuItemById(self->rootMenu, id);
            if (item) {
                GVariant* props = self->buildPropertiesVariant(*item);
                GVariant* val = g_variant_lookup_value(props, name, nullptr);
                if (val) {
                    g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", val));
                    g_variant_unref(props);
                    return;
                }
                g_variant_unref(props);
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", g_variant_new_string("")));
            return;
        } else if (g_strcmp0(method_name, "Event") == 0) {
            gint32 id = 0;
            const gchar* eventId = nullptr;
            GVariant* data = nullptr;
            guint32 timestamp = 0;
            g_variant_get(parameters, "(isvu)", &id, &eventId, &data, &timestamp);
            if (data) g_variant_unref(data);

            if (g_strcmp0(eventId, "clicked") == 0) {
                const MenuItem* item = self->findMenuItemById(self->rootMenu, id);
                if (item) {
                    itemToClick = *item;
                    hasItemToClick = true;
                }
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
        } else if (g_strcmp0(method_name, "EventGroup") == 0) {
            GVariantBuilder errBuilder;
            g_variant_builder_init(&errBuilder, G_VARIANT_TYPE("ai"));
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)", g_variant_builder_end(&errBuilder)));
            return;
        } else if (g_strcmp0(method_name, "AboutToShow") == 0) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
            return;
        }
    }

    if (hasItemToClick) {
        self->handleItemClick(itemToClick);
    }
}
