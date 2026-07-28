#include "layers.h"
#include "mutex.h"
#include "json_alloc.h"
#include "patterns/layer_shapes.h"
#include "patterns/pattern_engine.h"
#include "patterns/preset_patterns.h"
#include <LittleFS.h>
#include <esp_log.h>
#include <math.h>

namespace layers {

static const char* TAG  = "layers";
static const char* PATH = "/layers.json";

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Applies validated/clamped fields from `obj` onto `l` (partial update --
// only keys present in obj are touched). Caller holds LOCK_STATE().
static void applyLayerFields(Layer& l, JsonObjectConst obj) {
    if (!obj["enabled"].isNull())     l.enabled     = obj["enabled"] | false;
    if (!obj["shape"].isNull())
        l.shape = (uint8_t)clampi((int)(obj["shape"] | 0), 0,
                                   (int)layer_shapes::ShapeType::COUNT - 1);
    if (!obj["shapeParam"].isNull())  l.shapeParam  = (uint8_t)clampi((int)(obj["shapeParam"] | 5), 1, 20);
    if (!obj["scaleX"].isNull())      l.scaleX      = clampf(obj["scaleX"] | 0.5f, 0.05f, 1.0f);
    if (!obj["scaleY"].isNull())      l.scaleY      = clampf(obj["scaleY"] | 0.5f, 0.05f, 1.0f);
    if (!obj["hsv"].isNull())         l.hsv         = obj["hsv"] | false;
    if (!obj["hue"].isNull())         l.hue         = fmodf(clampf(obj["hue"] | 0.0f, 0.0f, 360.0f), 360.0f);
    if (!obj["sat"].isNull())         l.sat         = clampf(obj["sat"] | 1.0f, 0.0f, 1.0f);
    if (!obj["light"].isNull())       l.light       = clampf(obj["light"] | 0.5f, 0.0f, 1.0f);
    if (!obj["fadeEnds"].isNull())    l.fadeEnds    = (uint8_t)clampi((int)(obj["fadeEnds"] | 0), 0, 100);
    if (!obj["scaleLinked"].isNull()) l.scaleLinked = obj["scaleLinked"] | true;
    if (!obj["xScaleX"].isNull())     l.xScaleX     = clampf(obj["xScaleX"] | 1.0f, 0.1f, 4.0f);
    if (!obj["xScaleY"].isNull())     l.xScaleY     = clampf(obj["xScaleY"] | 1.0f, 0.1f, 4.0f);
    if (!obj["xShiftX"].isNull())     l.xShiftX     = clampf(obj["xShiftX"] | 0.0f, -32767.0f, 32767.0f);
    if (!obj["xShiftY"].isNull())     l.xShiftY     = clampf(obj["xShiftY"] | 0.0f, -32767.0f, 32767.0f);
    if (!obj["xRotation"].isNull())   l.xRotation   = fmodf(obj["xRotation"] | 0.0f, 360.0f);
}

void init() {
    load();  // no-op (active stays false) if /layers.json is missing/invalid
    ESP_LOGI(TAG, "init: %u layer(s) loaded, active=false", gLayerStack.count);
}

bool setStack(JsonArrayConst arr, bool active, uint8_t selected) {
    Layer tmp[LAYER_MAX];
    uint8_t n = 0;
    for (JsonObjectConst e : arr) {
        if (n >= LAYER_MAX) break;
        applyLayerFields(tmp[n], e);
        n++;
    }
    {
        LOCK_STATE();
        for (uint8_t i = 0; i < n; i++) gLayerStack.layers[i] = tmp[i];
        gLayerStack.count    = n;
        gLayerStack.active   = active;
        gLayerStack.selected = (n == 0) ? 0 : (uint8_t)clampi(selected, 0, n - 1);
    }
    save();
    return true;
}

static volatile bool     s_dirty       = false;
static volatile uint32_t s_dirty_since = 0;

// PATCH /api/layers?idx=N (see web_ui.cpp) runs on AsyncTCP's single
// async_tcp task, which also dispatches every other connection's events
// system-wide, including tcp_accept for brand new ones (see AsyncTCP.cpp's
// own comment on _get_async_event(): a callback that "runs too long...
// will fill the queue with events until it deadlocks"). A live slider drag
// fires this dozens of times a second; save() is a synchronous LittleFS
// flash write, and stacking that many of them inline serialized ~10s of
// flash I/O through that one task, during which nothing else on the WebUI
// (nor any brand-new TCP connection anywhere -- EtherDream/Helios/the
// WebUI itself) could be serviced, observed as "tcp_accept(): pcb is NULL"
// floods and 10s-late slider feedback. Applying the field is still
// immediate (cheap, in-RAM, under LOCK_STATE) -- only the flash write is
// deferred to maybeFlush(), polled from safety::task() well off both the
// network stack and the render loop.
bool setLayer(uint8_t idx, JsonObjectConst obj) {
    bool ok;
    {
        LOCK_STATE();
        ok = idx < gLayerStack.count;
        if (ok) applyLayerFields(gLayerStack.layers[idx], obj);
    }
    if (ok) { s_dirty = true; s_dirty_since = millis(); }
    return ok;
}

void maybeFlush() {
    if (!s_dirty || millis() - s_dirty_since < 400) return;
    s_dirty = false;
    save();
}

// Layer mode sits alongside Paint/Curve in task() priority (see
// pattern_engine.cpp). Activating it clears every other manually-toggled
// mode so only one ever renders -- same convention setPaintActive()/
// setCurve() already use for each other (see their comments).
void setActive(bool active) {
    { LOCK_STATE(); gLayerStack.active = active; }
    if (active) {
        patterns::setPreset(presets::Preset::None);
        patterns::stopTestPattern();
        gState.calib_active  = false;
        gTextConfig.active   = false;
        gPaint.active        = false;
        gCurves.active_curve = -1;
    }
    save();
}

void clear() {
    {
        LOCK_STATE();
        gLayerStack.count    = 0;
        gLayerStack.active   = false;
        gLayerStack.selected = 0;
    }
    save();
}

void fillStateJson(JsonObject& out) {
    LOCK_STATE();
    out["active"]   = gLayerStack.active;
    out["selected"] = gLayerStack.selected;
    out["count"]    = gLayerStack.count;
    JsonArray arr = out["layers"].to<JsonArray>();
    for (uint8_t i = 0; i < gLayerStack.count; i++) {
        const Layer& l = gLayerStack.layers[i];
        JsonObject o = arr.add<JsonObject>();
        o["enabled"]     = l.enabled;
        o["shape"]       = l.shape;
        o["shapeParam"]  = l.shapeParam;
        o["scaleX"]      = l.scaleX;
        o["scaleY"]      = l.scaleY;
        o["hsv"]         = l.hsv;
        o["hue"]         = l.hue;
        o["sat"]         = l.sat;
        o["light"]       = l.light;
        o["fadeEnds"]    = l.fadeEnds;
        o["scaleLinked"] = l.scaleLinked;
        o["xScaleX"]     = l.xScaleX;
        o["xScaleY"]     = l.xScaleY;
        o["xShiftX"]     = l.xShiftX;
        o["xShiftY"]     = l.xShiftY;
        o["xRotation"]   = l.xRotation;
    }
}

bool save() {
    JsonDocument doc(&jsonAllocator());
    JsonObject root = doc.to<JsonObject>();
    fillStateJson(root);  // takes/releases LOCK_STATE() itself -- fine, not nested

    File f = LittleFS.open(PATH, "w");
    if (!f) { ESP_LOGE(TAG, "save(): open for write failed"); return false; }
    size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) { ESP_LOGE(TAG, "save(): write failed"); return false; }
    return true;
}

bool load() {
    if (!LittleFS.exists(PATH)) return false;
    File f = LittleFS.open(PATH, "r");
    if (!f) return false;

    JsonDocument doc(&jsonAllocator());
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { ESP_LOGE(TAG, "load(): JSON error: %s", err.c_str()); return false; }

    setStack(doc["layers"].as<JsonArrayConst>(), false /* fail-safe-off */, doc["selected"] | 0);
    return true;
}

}  // namespace layers
