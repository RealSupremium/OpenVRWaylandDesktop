#include "PortalCapture.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>

#include "Util.hpp"
#include "pipewire/stream.h"

PortalCapture::PortalCapture() {}

PortalCapture::~PortalCapture() { stop(); }

void PortalCapture::stopPipeWire() {
    streaming = false;
    if (pwMainLoop) {
        pw_main_loop_quit(pwMainLoop);
    }
    if (pwThread.joinable()) {
        pwThread.join();
    }
    if (pwStream) {
        spa_hook_remove(&streamListener);
        pw_stream_destroy(pwStream);
        pwStream = nullptr;
    }
    if (pwCore) {
        pw_core_disconnect(pwCore);
        pwCore = nullptr;
    }
    if (pwContext) {
        pw_context_destroy(pwContext);
        pwContext = nullptr;
    }
    if (pwMainLoop) {
        pw_main_loop_destroy(pwMainLoop);
        pwMainLoop = nullptr;
    }
}

bool PortalCapture::reconnectPipeWire(const std::vector<DrmFormatModifier>& drmModifiers) {
    if (pwNodeId == 0) {
        return false;
    }
    LOG << "Reconnecting PipeWire stream (DMA-BUF " << (enableDmaBuf ? "enabled" : "disabled") << ") for node " << pwNodeId << "..." << std::endl;
    stopPipeWire();
    return connectPipeWire(drmModifiers);
}

void PortalCapture::stop() {
    stopPipeWire();
    closePortalSession();
    if (dbusConn) {
        g_object_unref(dbusConn);
        dbusConn = nullptr;
    }
}

void PortalCapture::closePortalSession() {
    if (dbusConn && !sessionHandle.empty()) {
        LOG << "Closing ScreenCast portal session: " << sessionHandle << std::endl;
        GError* error = nullptr;
        g_dbus_connection_call_sync(dbusConn, "org.freedesktop.portal.Desktop", sessionHandle.c_str(), "org.freedesktop.portal.Session", "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                                    &error);
        if (error) {
            LOG_ERROR << "Error closing portal session: " << error->message << std::endl;
            g_error_free(error);
        }
        sessionHandle.clear();
    }
}

static std::string randomToken() {
    static std::mutex tokenMutex;
    std::lock_guard<std::mutex> lock(tokenMutex);
    static std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    return "token_" + std::to_string(dist(gen));
}

static GVariant* callPortalMethodAndWaitResponse(GDBusConnection* conn, const char* method, GVariant* parameters, GVariantBuilder* optionsBuilder, std::string& outRequestPath) {
    GError* error = nullptr;
    std::string token = randomToken();

    GVariantBuilder localOptsBuilder;
    GVariantBuilder* optsBuilder = optionsBuilder;
    if (!optsBuilder) {
        optsBuilder = &localOptsBuilder;
        g_variant_builder_init(optsBuilder, G_VARIANT_TYPE("a{sv}"));
    }
    g_variant_builder_add(optsBuilder, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    g_variant_builder_add(optsBuilder, "{sv}", "session_handle_token", g_variant_new_string(token.c_str()));

    const char* uniqueName = g_dbus_connection_get_unique_name(conn);
    std::string senderName = uniqueName ? (uniqueName + 1) : "unknown";
    for (char& c : senderName) {
        if (c == '.') c = '_';
    }

    outRequestPath = "/org/freedesktop/portal/desktop/request/" + senderName + "/" + token;

    GVariantBuilder callBuilder;
    g_variant_builder_init(&callBuilder, G_VARIANT_TYPE_TUPLE);

    GVariantIter iter;
    g_variant_iter_init(&iter, parameters);
    GVariant* child = nullptr;
    while ((child = g_variant_iter_next_value(&iter))) {
        g_variant_builder_add_value(&callBuilder, child);
        g_variant_unref(child);
    }
    g_variant_builder_add_value(&callBuilder, g_variant_builder_end(optsBuilder));
    g_variant_unref(parameters);

    std::mutex resMutex;
    std::condition_variable resCv;
    bool done = false;
    GVariant* resultResponse = nullptr;

    struct LoopData {
        std::mutex* mtx;
        std::condition_variable* cv;
        bool* done;
        GVariant** res;
    } loopData{&resMutex, &resCv, &done, &resultResponse};

    auto signalCb = [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters, gpointer user_data) {
        auto* data = static_cast<LoopData*>(user_data);
        uint32_t code = 0;
        GVariant* results = nullptr;
        g_variant_get(parameters, "(u@a{sv})", &code, &results);
        {
            std::lock_guard<std::mutex> lock(*data->mtx);
            if (code == 0) {
                *data->res = results;
            } else {
                if (results) g_variant_unref(results);
                *data->res = nullptr;
            }
            *data->done = true;
        }
        data->cv->notify_all();
    };

    guint subId = g_dbus_connection_signal_subscribe(conn, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response", outRequestPath.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                     signalCb, &loopData, nullptr);

    GVariant* callRes = g_dbus_connection_call_sync(conn, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.ScreenCast", method,
                                                    g_variant_builder_end(&callBuilder), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

    if (error) {
        LOG_ERROR << "DBus call failed for " << method << ": " << error->message << std::endl;
        g_error_free(error);
        g_dbus_connection_signal_unsubscribe(conn, subId);
        return nullptr;
    }

    g_variant_unref(callRes);

    {
        std::unique_lock<std::mutex> lock(resMutex);
        resCv.wait_for(lock, std::chrono::seconds(180), [&done]() { return done; });
    }

    g_dbus_connection_signal_unsubscribe(conn, subId);
    return resultResponse;
}

bool PortalCapture::createPortalSession() {
    GError* error = nullptr;
    dbusConn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!dbusConn) {
        LOG_ERROR << "Failed to connect to Session DBus: " << (error ? error->message : "unknown") << std::endl;
        if (error) g_error_free(error);
        return false;
    }

    LOG << "Creating ScreenCast Portal Session..." << std::endl;

    std::string reqPath;

    GVariant* createRes = callPortalMethodAndWaitResponse(dbusConn, "CreateSession", g_variant_new_tuple(nullptr, 0), nullptr, reqPath);
    if (!createRes) {
        LOG_ERROR << "CreateSession response failed." << std::endl;
        return false;
    }

    const char* sessHandle = nullptr;
    if (!g_variant_lookup(createRes, "session_handle", "s", &sessHandle)) {
        LOG_ERROR << "Failed to get session_handle from response." << std::endl;
        g_variant_unref(createRes);
        return false;
    }
    sessionHandle = sessHandle;
    g_variant_unref(createRes);

    LOG << "Session created: " << sessionHandle << std::endl;

    GVariantBuilder selectOpts;
    g_variant_builder_init(&selectOpts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&selectOpts, "{sv}", "types", g_variant_new_uint32(1));  // Monitor only
    g_variant_builder_add(&selectOpts, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&selectOpts, "{sv}", "cursor_mode", g_variant_new_uint32(2));   // Embedded cursor
    g_variant_builder_add(&selectOpts, "{sv}", "persist_mode", g_variant_new_uint32(2));  // PERSIST_EXPLICITLY_REVOKED

    if (!restoreToken.empty()) {
        LOG << "Using existing restore_token: " << restoreToken << std::endl;
        g_variant_builder_add(&selectOpts, "{sv}", "restore_token", g_variant_new_string(restoreToken.c_str()));
    }

    GVariant* selectRes = callPortalMethodAndWaitResponse(dbusConn, "SelectSources", g_variant_new("(o)", sessionHandle.c_str()), &selectOpts, reqPath);
    if (!selectRes) {
        LOG_ERROR << "SelectSources failed." << std::endl;
        closePortalSession();
        return false;
    }
    g_variant_unref(selectRes);

    LOG << "Prompting user for Screen Selection..." << std::endl;
    GVariant* startRes = callPortalMethodAndWaitResponse(dbusConn, "Start", g_variant_new("(os)", sessionHandle.c_str(), ""), nullptr, reqPath);
    if (!startRes) {
        LOG_ERROR << "User canceled screen selection or Start failed." << std::endl;
        closePortalSession();
        return false;
    }

    bool ok = parseStreamMetadata(startRes);
    g_variant_unref(startRes);
    if (!ok) {
        closePortalSession();
        return false;
    }

    LOG << "Portal session ready." << std::endl;
    return true;
}

bool PortalCapture::parseStreamMetadata(GVariant* startRes) {
    const char* retToken = nullptr;
    if (g_variant_lookup(startRes, "restore_token", "s", &retToken)) {
        restoreToken = retToken;
        LOG << "Obtained restore_token: " << restoreToken << std::endl;
    }

    GVariant* streams = g_variant_lookup_value(startRes, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (!streams || g_variant_n_children(streams) == 0) {
        LOG_ERROR << "No streams returned in Start response." << std::endl;
        if (streams) g_variant_unref(streams);
        return false;
    }

    GVariant* firstStream = g_variant_get_child_value(streams, 0);
    GVariant* optsDict = nullptr;
    g_variant_get(firstStream, "(u@a{sv})", &pwNodeId, &optsDict);

    if (optsDict) {
        GVariant* serialVal = g_variant_lookup_value(optsDict, "pipewire-serial", G_VARIANT_TYPE("t"));
        if (serialVal) {
            pwSerial = g_variant_get_uint64(serialVal);
            g_variant_unref(serialVal);
        }

        GVariant* posVal = g_variant_lookup_value(optsDict, "position", G_VARIANT_TYPE("(ii)"));
        if (posVal) {
            g_variant_get(posVal, "(ii)", &posX, &posY);
            g_variant_unref(posVal);
        }

        GVariant* sizeVal = g_variant_lookup_value(optsDict, "size", G_VARIANT_TYPE("(ii)"));
        if (sizeVal) {
            int32_t w = 0, h = 0;
            g_variant_get(sizeVal, "(ii)", &w, &h);
            if (w > 0 && h > 0) {
                streamWidth = static_cast<uint32_t>(w);
                streamHeight = static_cast<uint32_t>(h);
            }
            g_variant_unref(sizeVal);
        }
        g_variant_unref(optsDict);
    }

    g_variant_unref(firstStream);
    g_variant_unref(streams);

    LOG << "Screen selection successful! PipeWire Node ID: " << pwNodeId << ", Position: (" << posX << ", " << posY << "), Size: " << streamWidth << "x" << streamHeight << std::endl;
    return true;
}

void PortalCapture::onProcess(void* data) {
    auto* self = static_cast<PortalCapture*>(data);
    self->handlePipeWireFrame();
}

void PortalCapture::onStateChanged(void* data, enum pw_stream_state /*old_state*/, enum pw_stream_state state, const char* error) {
    auto* self = static_cast<PortalCapture*>(data);
    if (!self) return;

    LOG << "PipeWire Stream state changed: " << pw_stream_state_as_string(state) << (error ? std::string(" - error: ") + error : "") << std::endl;

    if (state == PW_STREAM_STATE_STREAMING) {
        self->streaming = true;
    } else {
        self->streaming = false;
    }
}

void PortalCapture::updateStreamBuffersAndMeta() {
    if (!pwStream) return;

    uint8_t bufBuffer[512];
    spa_pod_builder bBuffers = SPA_POD_BUILDER_INIT(bufBuffer, sizeof(bufBuffer));

    uint32_t dataTypes = (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
    if (enableDmaBuf) {
        dataTypes |= (1 << SPA_DATA_DmaBuf);
    }

    const spa_pod* pBuffers =
        static_cast<const spa_pod*>(spa_pod_builder_add_object(&bBuffers, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(dataTypes), 0));

    uint8_t metaBuffer[256];
    spa_pod_builder bMeta = SPA_POD_BUILDER_INIT(metaBuffer, sizeof(metaBuffer));
    const spa_pod* pMetaHeader = static_cast<const spa_pod*>(spa_pod_builder_add_object(&bMeta, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                                                                        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)), 0));

    uint8_t xformBuffer[256];
    spa_pod_builder bXform = SPA_POD_BUILDER_INIT(xformBuffer, sizeof(xformBuffer));
    const spa_pod* pMetaXform = static_cast<const spa_pod*>(spa_pod_builder_add_object(&bXform, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
                                                                                       SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_videotransform)), 0));

    const spa_pod* updateParams[3] = {pBuffers, pMetaHeader, pMetaXform};
    pw_stream_update_params(pwStream, updateParams, 3);
}

void PortalCapture::onParamChanged(void* data, uint32_t id, const struct spa_pod* param) {
    auto* self = static_cast<PortalCapture*>(data);
    if (param == nullptr || id != SPA_PARAM_Format) return;

    struct spa_video_info info{};
    if (spa_format_video_raw_parse(param, &info.info.raw) >= 0) {
        if (info.info.raw.size.width > 0 && info.info.raw.size.height > 0) {
            self->currentWidth = info.info.raw.size.width;
            self->currentHeight = info.info.raw.size.height;
            self->videoFormat = info.info.raw.format;
            if (info.info.raw.flags & SPA_VIDEO_FLAG_MODIFIER) {
                self->currentModifier = info.info.raw.modifier;
            } else {
                self->currentModifier = 0x00ffffffffffffffULL;
            }

            LOG << "Negotiated PipeWire stream format: " << self->currentWidth << "x" << self->currentHeight << " format_enum=" << self->videoFormat << " modifier=0x" << std::hex
                << self->currentModifier << std::dec << std::endl;

            self->updateStreamBuffersAndMeta();
        }
    }
}

void PortalCapture::buildDmaBufFormatPods(size_t& bufIdx, const std::vector<DrmFormatModifier>& drmModifiers, std::vector<const spa_pod*>& params) {
    if (!enableDmaBuf || drmModifiers.empty()) return;

    spa_rectangle minRes = SPA_RECTANGLE(1, 1);
    spa_rectangle defRes = SPA_RECTANGLE(256, 256);
    spa_rectangle maxRes = SPA_RECTANGLE(8192, 8192);

    spa_fraction minFps = SPA_FRACTION(0, 1);
    spa_fraction defFps = SPA_FRACTION(0, 1);
    spa_fraction maxFps = SPA_FRACTION(1000, 1);

    std::unordered_map<spa_video_format, std::vector<uint64_t>> fmtMap;
    for (const auto& mod : drmModifiers) {
        fmtMap[mod.format].push_back(mod.modifier);
    }

    for (auto& entry : fmtMap) {
        if (entry.second.empty()) continue;
        bool hasLinear = false;
        bool hasInvalid = false;
        for (uint64_t m : entry.second) {
            if (m == 0) hasLinear = true;
            if (m == 0x00ffffffffffffffULL) hasInvalid = true;
        }
        if (!hasLinear) entry.second.push_back(0);
        if (!hasInvalid) entry.second.push_back(0x00ffffffffffffffULL);
    }

    for (const auto& entry : fmtMap) {
        if (entry.second.empty() || bufIdx >= 30) continue;
        spa_video_format fmt = entry.first;
        const auto& mods = entry.second;

        spa_pod_builder builder = SPA_POD_BUILDER_INIT(podStorage[bufIdx].data(), static_cast<uint32_t>(podStorage[bufIdx].size()));
        spa_pod_frame f;
        spa_pod_builder_push_object(&builder, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

        spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
        spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_video);

        spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
        spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);

        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_format, 0);
        spa_pod_builder_id(&builder, fmt);

        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_size, 0);
        spa_pod_frame sf;
        spa_pod_builder_push_choice(&builder, &sf, SPA_CHOICE_Range, 0);
        spa_pod_builder_rectangle(&builder, defRes.width, defRes.height);
        spa_pod_builder_rectangle(&builder, minRes.width, minRes.height);
        spa_pod_builder_rectangle(&builder, maxRes.width, maxRes.height);
        spa_pod_builder_pop(&builder, &sf);

        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_framerate, 0);
        spa_pod_frame ff;
        spa_pod_builder_push_choice(&builder, &ff, SPA_CHOICE_Range, 0);
        spa_pod_builder_fraction(&builder, defFps.num, defFps.denom);
        spa_pod_builder_fraction(&builder, minFps.num, minFps.denom);
        spa_pod_builder_fraction(&builder, maxFps.num, maxFps.denom);
        spa_pod_builder_pop(&builder, &ff);

        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_frame cf;
        spa_pod_builder_push_choice(&builder, &cf, SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(&builder, static_cast<int64_t>(mods[0]));
        for (uint64_t m : mods) {
            spa_pod_builder_long(&builder, static_cast<int64_t>(m));
        }
        spa_pod_builder_pop(&builder, &cf);

        params.push_back(static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &f)));
        bufIdx++;
    }
}

void PortalCapture::buildFallbackFormatPod(size_t& bufIdx, std::vector<const spa_pod*>& params) {
    spa_rectangle minRes = SPA_RECTANGLE(1, 1);
    spa_rectangle defRes = SPA_RECTANGLE(256, 256);
    spa_rectangle maxRes = SPA_RECTANGLE(8192, 8192);

    spa_fraction minFps = SPA_FRACTION(0, 1);
    spa_fraction defFps = SPA_FRACTION(0, 1);
    spa_fraction maxFps = SPA_FRACTION(1000, 1);

    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podStorage[bufIdx].data(), static_cast<uint32_t>(podStorage[bufIdx].size()));
    spa_pod_frame f;
    spa_pod_builder_push_object(&builder, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

    spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
    spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_video);

    spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
    spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);

    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_format, 0);
    spa_pod_frame vf;
    spa_pod_builder_push_choice(&builder, &vf, SPA_CHOICE_Enum, 0);
    spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_RGBA);
    spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_RGBA);
    spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_BGRA);
    spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_RGBx);
    spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_BGRx);
    spa_pod_builder_pop(&builder, &vf);

    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_size, 0);
    spa_pod_frame sf;
    spa_pod_builder_push_choice(&builder, &sf, SPA_CHOICE_Range, 0);
    spa_pod_builder_rectangle(&builder, defRes.width, defRes.height);
    spa_pod_builder_rectangle(&builder, minRes.width, minRes.height);
    spa_pod_builder_rectangle(&builder, maxRes.width, maxRes.height);
    spa_pod_builder_pop(&builder, &sf);

    spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_framerate, 0);
    spa_pod_frame ff;
    spa_pod_builder_push_choice(&builder, &ff, SPA_CHOICE_Range, 0);
    spa_pod_builder_fraction(&builder, defFps.num, defFps.denom);
    spa_pod_builder_fraction(&builder, minFps.num, minFps.denom);
    spa_pod_builder_fraction(&builder, maxFps.num, maxFps.denom);
    spa_pod_builder_pop(&builder, &ff);

    params.push_back(static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &f)));
    bufIdx++;
}

bool PortalCapture::connectPipeWire(const std::vector<DrmFormatModifier>& drmModifiers) {
    pw_init(nullptr, nullptr);
    pwMainLoop = pw_main_loop_new(nullptr);
    if (!pwMainLoop) return false;
    pwContext = pw_context_new(pw_main_loop_get_loop(pwMainLoop), nullptr, 0);
    if (!pwContext) return false;
    pwCore = pw_context_connect(pwContext, nullptr, 0);
    if (!pwCore) return false;

    pw_properties* props = pw_properties_new(PW_KEY_NODE_NAME, "OpenVROverlayCapture", PW_KEY_NODE_DESCRIPTION, "SteamVR Wayland Desktop Capture", PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                             "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);

    static const pw_stream_events streamEvents = [] {
        pw_stream_events ev{};
        ev.version = PW_VERSION_STREAM_EVENTS;
        ev.state_changed = &PortalCapture::onStateChanged;
        ev.param_changed = &PortalCapture::onParamChanged;
        ev.process = &PortalCapture::onProcess;
        return ev;
    }();

    pwStream = pw_stream_new(pwCore, "Wayland Desktop VR Capture", props);
    pw_stream_add_listener(pwStream, &streamListener, &streamEvents, this);

    std::vector<const spa_pod*> params;
    podStorage.clear();
    podStorage.resize(40, std::vector<uint8_t>(2048));
    size_t bufIdx = 0;

    buildDmaBufFormatPods(bufIdx, drmModifiers, params);
    buildFallbackFormatPod(bufIdx, params);

    uint32_t targetId = (pwNodeId != 0) ? pwNodeId : PW_ID_ANY;
    int flags = PW_STREAM_FLAG_AUTOCONNECT;
    if (!enableDmaBuf) {
        flags |= PW_STREAM_FLAG_MAP_BUFFERS;
    }

    if (pw_stream_connect(pwStream, PW_DIRECTION_INPUT, targetId, static_cast<pw_stream_flags>(flags), params.data(), params.size()) < 0) {
        LOG_ERROR << "Failed to connect PipeWire stream." << std::endl;
        return false;
    }

    pwThread = std::thread([this]() { pw_main_loop_run(pwMainLoop); });
    return true;
}

void PortalCapture::handlePipeWireFrame() {
    if (!pwStream || !streaming) return;

    pw_buffer* buf = pw_stream_dequeue_buffer(pwStream);
    if (!buf) return;

    spa_buffer* spaBuf = buf->buffer;
    if (spaBuf && spaBuf->n_datas > 0) {
        CapturedFrame frame{};
        spa_data& data = spaBuf->datas[0];

        frame.bufferId = spaBuf;
        frame.drmModifier = currentModifier;
        frame.drmFormat = videoFormat;

        if (data.type == SPA_DATA_DmaBuf) {
            frame.isDmaBuf = true;
            frame.fd = data.fd;
            frame.memoryData = static_cast<uint8_t*>(data.data);
        } else if (data.type == SPA_DATA_MemFd || data.type == SPA_DATA_MemPtr) {
            frame.isDmaBuf = false;
            frame.fd = data.fd;
            if (data.data != nullptr) {
                uint32_t offset = (data.chunk != nullptr) ? data.chunk->offset : 0;
                frame.memoryData = static_cast<uint8_t*>(data.data) + offset;
            }
        }

        frame.width = currentWidth;
        frame.height = currentHeight;
        frame.stride = (data.chunk != nullptr && data.chunk->stride > 0) ? data.chunk->stride : (frame.width * 4);

        if (frameCallback && (frame.isDmaBuf || frame.memoryData)) {
            frameCallback(frame);
        }
    }
    pw_stream_queue_buffer(pwStream, buf);
}

bool PortalCapture::initAndRequestCapture(FrameCallback onFrame, const std::string& existingRestoreToken, const std::vector<DrmFormatModifier>& drmModifiers) {
    frameCallback = onFrame;
    restoreToken = existingRestoreToken;

    // Mutex that so it should be one portal dialog at a time.
    static std::mutex portalMutex;
    {
        std::lock_guard<std::mutex> lock(portalMutex);
        if (!createPortalSession()) {
            return false;
        }
    }

    return connectPipeWire(drmModifiers);
}
