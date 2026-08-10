#pragma once

#include <gio/gio.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct SharedMonitorItem {
    std::string id;
    std::string name;
    uint32_t nodeId = 0;
    bool isStreaming = false;
    bool isOverlayVisible = false;
    bool enableDmaBuf = false;

    bool operator==(const SharedMonitorItem& other) const {
        return id == other.id && name == other.name && nodeId == other.nodeId && isStreaming == other.isStreaming && isOverlayVisible == other.isOverlayVisible && enableDmaBuf == other.enableDmaBuf;
    }

    bool operator!=(const SharedMonitorItem& other) const { return !(*this == other); }
};

class TrayManager {
   public:
    using AddMonitorCallback = std::function<void()>;
    using RemoveMonitorCallback = std::function<void(const std::string& id)>;
    using MonitorDmaBufToggleCallback = std::function<void(const std::string& id, bool enabled)>;
    using ManifestToggleCallback = std::function<void(bool install)>;
    using QuitCallback = std::function<void()>;

    TrayManager(AddMonitorCallback onAdd, RemoveMonitorCallback onRemove);
    ~TrayManager();

    void setMonitorDmaBufToggleCallback(MonitorDmaBufToggleCallback cb) { monitorDmaBufToggleCallback = cb; }
    void setManifestToggleCallback(ManifestToggleCallback cb) { manifestToggleCallback = cb; }
    void setQuitCallback(QuitCallback cb) { quitCallback = cb; }

    void setManifestInstalled(bool installed);

    bool init();
    void processEvents();
    void stop();
    void updateMonitors(const std::vector<SharedMonitorItem>& activeMonitors);

   private:
    struct MenuItem {
        int id = 0;
        std::string label;
        std::string iconName;
        bool isSeparator = false;
        bool enabled = true;
        bool visible = true;
        std::string toggleType;
        int toggleState = 0;
        std::string action;
        std::string monitorId;
        std::vector<MenuItem> children;
    };

    AddMonitorCallback addCallback;
    RemoveMonitorCallback removeCallback;
    MonitorDmaBufToggleCallback monitorDmaBufToggleCallback;
    ManifestToggleCallback manifestToggleCallback;
    QuitCallback quitCallback;

    bool manifestInstalled = false;
    uint32_t menuRevision = 1;

    GDBusConnection* dbusConn = nullptr;
    guint sniRegId = 0;
    guint menuRegId = 0;

    std::mutex menuMutex;
    std::vector<SharedMonitorItem> currentMonitors;
    MenuItem rootMenu;
    int nextMenuItemId = 1;

    void registerWithWatcher();
    void rebuildMenuTree();
    void emitLayoutUpdated();
    void collectAllProperties(GVariantBuilder* builder, const MenuItem& item);

    static void handleSniMethodCall(GDBusConnection* connection, const gchar* sender, const gchar* object_path, const gchar* interface_name, const gchar* method_name, GVariant* parameters,
                                    GDBusMethodInvocation* invocation, gpointer user_data);
    static GVariant* handleSniGetProperty(GDBusConnection* connection, const gchar* sender, const gchar* object_path, const gchar* interface_name, const gchar* property_name, GError** error,
                                          gpointer user_data);

    static void handleMenuMethodCall(GDBusConnection* connection, const gchar* sender, const gchar* object_path, const gchar* interface_name, const gchar* method_name, GVariant* parameters,
                                     GDBusMethodInvocation* invocation, gpointer user_data);

    GVariant* buildPropertiesVariant(const MenuItem& item);
    GVariant* buildLayoutVariant(const MenuItem& item, int recursionDepth);
    const MenuItem* findMenuItemById(const MenuItem& current, int id);
    void handleItemClick(const MenuItem& item);
};
