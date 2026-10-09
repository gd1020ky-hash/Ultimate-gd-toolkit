#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <fstream>
#include <set>
#include "bot.hpp"
using namespace geode::prelude;

// ---------- Saved config (edited only through the menu) ----------
struct Cfg {
    std::string macro = "off", sound, key, model = "claude-sonnet-5-5";
    float speed = 1.f;
    bool sync = true, frames = true, click = false, noclip = false, showFps = false;
    int deco = 211;
} cfg;

static void loadCfg() {
    static bool done = false;
    if (done) return;
    done = true;
    auto m = Mod::get();
    cfg.macro = m->getSavedValue<std::string>("macro", "off");
    cfg.sound = m->getSavedValue<std::string>("sound", "");
    cfg.key = m->getSavedValue<std::string>("key", "");
    cfg.model = m->getSavedValue<std::string>("model", "claude-sonnet-5-5");
    cfg.speed = (float)m->getSavedValue<double>("speed", 1.0);
    cfg.sync = m->getSavedValue<bool>("sync", true);
    cfg.frames = m->getSavedValue<bool>("frames", true);
    cfg.click = m->getSavedValue<bool>("click", false);
    cfg.noclip = m->getSavedValue<bool>("noclip", false);
    cfg.showFps = m->getSavedValue<bool>("showFps", false);
    cfg.deco = m->getSavedValue<int>("deco", 211);
}
static void saveCfg() {
    auto m = Mod::get();
    m->setSavedValue("macro", cfg.macro);
    m->setSavedValue("sound", cfg.sound);
    m->setSavedValue("key", cfg.key);
    m->setSavedValue("model", cfg.model);
    m->setSavedValue("speed", (double)cfg.speed);
    m->setSavedValue("sync", cfg.sync);
    m->setSavedValue("frames", cfg.frames);
    m->setSavedValue("click", cfg.click);
    m->setSavedValue("noclip", cfg.noclip);
    m->setSavedValue("showFps", cfg.showFps);
    m->setSavedValue("deco", cfg.deco);
}

static std::vector<Click> g_macro;
int g_frame = 0;
static size_t g_idx = 0;
static bool g_inject = false;
static CCLabelBMFont* g_label = nullptr;
static auto macroPath() { return Mod::get()->getSaveDir() / "macro.bin"; }

// Macro, bot, noclip and speed runs are never saved or submitted as completions.
bool botSafeMode() { return cfg.macro != "off" || cfg.speed != 1.f || cfg.noclip; }
bool botNoclip() { return cfg.noclip; }

// Called by the pathfinder when it finds a way through: save it, switch to play, restart.
void botDone(std::vector<Click> clicks) {
    g_macro = std::move(clicks);
    std::ofstream f(macroPath(), std::ios::binary);
    for (auto& c : g_macro) f.write((const char*)&c, sizeof c);
    cfg.macro = "play";
    saveCfg();
    Notification::create(fmt::format("Path found: {} inputs", g_macro.size()), NotificationIcon::Success)->show();
    queueInMainThread([] {
        g_frame = 0; g_idx = 0;
        if (auto pl = PlayLayer::get()) pl->resetLevel();
    });
}

// ---------- Speedhack + music sync ----------
class $modify(CCScheduler) {
    void update(float dt) {
        loadCfg();
        CCScheduler::update(dt * cfg.speed);
        if (auto fm = FMODAudioEngine::sharedEngine())
            if (fm->m_backgroundMusicChannel)
                fm->m_backgroundMusicChannel->setPitch(cfg.sync ? cfg.speed : 1.f);
    }
};

// ---------- Macro + frame counter ----------
class $modify(GJBaseGameLayer) {
    void processCommands(float dt, bool halfTick, bool lastTick) {
        if (PlayLayer::get()) {
            if (bot::busy()) bot::tick(this);
            else if (cfg.macro == "play")
                while (g_idx < g_macro.size() && g_macro[g_idx].f <= g_frame) {
                    auto& c = g_macro[g_idx++];
                    g_inject = true;
                    this->handleButton(c.down, c.btn, c.p1);
                    g_inject = false;
                }
            g_frame++;
            if (g_label) {
                g_label->setVisible(cfg.frames);
                if (cfg.frames) g_label->setString(fmt::format("Frame: {}", g_frame).c_str());
            }
        }
        GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
    }
    void handleButton(bool down, int btn, bool p1) {
        if (bot::busy()) {                       // during a search only the bot may press
            if (bot::injecting()) GJBaseGameLayer::handleButton(down, btn, p1);
            return;
        }
        GJBaseGameLayer::handleButton(down, btn, p1);
        if (!PlayLayer::get()) return;
        if (!g_inject && cfg.macro == "record") g_macro.push_back({g_frame, down, btn, p1});
        if (down && cfg.click && !cfg.sound.empty())
            FMODAudioEngine::sharedEngine()->playEffect(cfg.sound);
    }
};

class $modify(PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        g_frame = 0; g_idx = 0;
        if (cfg.macro == "play") {
            g_macro.clear();
            std::ifstream f(macroPath(), std::ios::binary);
            Click c;
            while (f.read((char*)&c, sizeof c)) g_macro.push_back(c);
        }
        g_label = CCLabelBMFont::create("Frame: 0", "bigFont.fnt");
        g_label->setScale(0.4f);
        g_label->setAnchorPoint({0, 1});
        g_label->setPosition(5, CCDirector::get()->getWinSize().height - 5);
        g_label->setZOrder(100);
        this->addChild(g_label);
        bot::begin(this);
    }
    void resetLevel() {
        PlayLayer::resetLevel();
        g_frame = 0; g_idx = 0;
        if (cfg.macro == "record") g_macro.clear();
    }
    void onQuit() {
        bot::cancel();
        if (cfg.macro == "record") {
            std::ofstream f(macroPath(), std::ios::binary);
            for (auto& c : g_macro) f.write((const char*)&c, sizeof c);
        }
        g_label = nullptr;
        PlayLayer::onQuit();
    }
};

// ---------- The one mod menu ----------
class ToolkitMenu : public Popup {
    LevelEditorLayer* m_ed = nullptr;
    std::vector<CCNode*> m_items;
    CCMenuItemSpriteExtra* m_tabs[5] = {};
    bool m_drag = false;
    ButtonSprite* m_macroBtn = nullptr;
    CCLabelBMFont* m_speedLbl = nullptr;
    CCLabelBMFont* m_status = nullptr;
    std::string m_theme;
    async::TaskHolder<web::WebResponse> m_task;
public:
    static ToolkitMenu* create(LevelEditorLayer* ed) {
        auto r = new ToolkitMenu();
        if (r->init(ed)) { r->autorelease(); return r; }
        delete r; return nullptr;
    }
protected:
    bool init(LevelEditorLayer* ed) {
        if (!Popup::init(420.f, 300.f)) return false;
        loadCfg();
        m_ed = ed;
        this->setTitle("GD Toolkit  (drag here)");
        char const* names[5] = {"Game", "Sound", "Deco", "AI", "Hacks"};
        for (int i = 0; i < 5; i++) {
            auto s = ButtonSprite::create(names[i], 70, true, "bigFont.fnt", "GJ_button_04.png", 26.f, .6f);
            m_tabs[i] = CCMenuItemSpriteExtra::create(s, this, menu_selector(ToolkitMenu::onTab));
            m_tabs[i]->setTag(i);
            m_buttonMenu->addChildAtPosition(m_tabs[i], Anchor::Top, {(i - 2.f) * 80.f, -50.f});
        }
        this->page(0);
        return true;
    }
    void onClose(CCObject* o) override { saveCfg(); Popup::onClose(o); }

    // ----- drag the whole window by its title bar -----
    bool ccTouchBegan(CCTouch* t, CCEvent* e) override {
        auto p = m_mainLayer->convertToNodeSpace(t->getLocation());
        m_drag = p.x > 0 && p.x < m_size.width && p.y > m_size.height - 36 && p.y < m_size.height;
        return Popup::ccTouchBegan(t, e);
    }
    void ccTouchMoved(CCTouch* t, CCEvent* e) override {
        if (!m_drag) return Popup::ccTouchMoved(t, e);
        auto ws = CCDirector::get()->getWinSize();
        auto pos = m_mainLayer->getPosition() + (t->getLocation() - t->getPreviousLocation());
        m_mainLayer->setPosition({std::clamp(pos.x, 0.f, ws.width), std::clamp(pos.y, 0.f, ws.height)});
    }
    void ccTouchEnded(CCTouch* t, CCEvent* e) override { m_drag = false; Popup::ccTouchEnded(t, e); }
    void ccTouchCancelled(CCTouch* t, CCEvent* e) override { m_drag = false; Popup::ccTouchCancelled(t, e); }

    // ----- layout helpers (x, y are measured from the popup's bottom-left) -----
    template <class N>
    N* put(N* n, float x, float y, bool menu = false) {
        CCNode* parent = menu ? static_cast<CCNode*>(m_buttonMenu) : static_cast<CCNode*>(m_mainLayer);
        parent->addChildAtPosition(n, Anchor::BottomLeft, {x, y});
        m_items.push_back(n);
        return n;
    }
    void label(char const* t, float y) {
        auto l = CCLabelBMFont::create(t, "bigFont.fnt");
        l->setScale(.45f);
        put(l, 26.f + l->getScaledContentWidth() / 2, y);
    }
    void note(char const* t, float y) {
        auto l = CCLabelBMFont::create(t, "bigFont.fnt");
        l->setScale(.4f);
        l->setOpacity(170);
        put(l, m_size.width / 2, y);
    }
    ButtonSprite* button(char const* t, int w, float x, float y, SEL_MenuHandler h, int tag = 0) {
        auto s = ButtonSprite::create(t, w, true, "bigFont.fnt", "GJ_button_01.png", 28.f, .6f);
        auto b = CCMenuItemSpriteExtra::create(s, this, h);
        b->setTag(tag);
        put(b, x, y, true);
        return s;
    }
    void toggler(float y, int tag, bool on) {
        auto t = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(ToolkitMenu::onToggle), .65f);
        t->setTag(tag);
        t->toggle(on);
        put(t, m_size.width - 60, y, true);
    }
    template <class F>
    TextInput* input(float x, float y, float w, char const* ph, std::string const& v, F cb,
                     char const* filter = nullptr, bool pw = false) {
        auto in = TextInput::create(w, ph, "chatFont.fnt");
        in->setString(v, false);
        in->setCallback(std::move(cb));
        if (filter) in->setFilter(filter);
        if (pw) in->setPasswordMode(true);
        return put(in, x, y);
    }
    void showSpeed() {
        if (m_speedLbl) m_speedLbl->setString(fmt::format("{:.2f}x", cfg.speed).c_str());
    }

    // ----- pages -----
    void page(int i) {
        for (auto n : m_items) n->removeFromParent();
        m_items.clear();
        m_macroBtn = nullptr; m_speedLbl = nullptr; m_status = nullptr;
        for (int k = 0; k < 5; k++)
            static_cast<ButtonSprite*>(m_tabs[k]->getNormalImage())
                ->updateBGImage(k == i ? "GJ_button_01.png" : "GJ_button_04.png");
        float W = m_size.width, H = m_size.height;
        float r1 = H - 95, r2 = H - 135, r3 = H - 175, r4 = H - 215, r5 = H - 255;

        if (i == 0) {
            label("Macro", r1);
            m_macroBtn = button(cfg.macro.c_str(), 110, W - 95, r1, menu_selector(ToolkitMenu::onMacro));
            label("Speed", r2);
            button("<<", 36, 184, r2, menu_selector(ToolkitMenu::onSpeed), -50);
            button("<", 36, 222, r2, menu_selector(ToolkitMenu::onSpeed), -5);
            m_speedLbl = CCLabelBMFont::create("", "bigFont.fnt");
            m_speedLbl->setScale(.55f);
            put(m_speedLbl, 287.f, r2);
            showSpeed();
            button(">", 36, 352, r2, menu_selector(ToolkitMenu::onSpeed), 5);
            button(">>", 36, 390, r2, menu_selector(ToolkitMenu::onSpeed), 50);
            label("Sync music speed", r3);
            toggler(r3, 1, cfg.sync);
            label("Frame counter", r4);
            toggler(r4, 2, cfg.frames);
            label("Pathfinder on next level", r5);
            toggler(r5, 4, bot::armed);
        } else if (i == 1) {
            label("Click sound", r1);
            toggler(r1, 3, cfg.click);
            label("Sound file (full path)", r2);
            input(W / 2, r3, 360, "/storage/emulated/0/Download/click.ogg", cfg.sound,
                  [](std::string const& s) { cfg.sound = s; });
        } else if (i == 2) {
            label("Deco object ID", r1);
            input(W - 100, r1, 110, "211", std::to_string(cfg.deco),
                  [](std::string const& s) { cfg.deco = std::atoi(s.c_str()); }, "0123456789");
            if (m_ed) button("Auto Deco", 150, W / 2, r2, menu_selector(ToolkitMenu::onDeco));
            else note("Open this from the editor pause menu to use it", r2);
        } else if (i == 4) {
            label("Noclip", r1);
            toggler(r1, 5, cfg.noclip);
            label("Show FPS", r2);
            toggler(r2, 6, cfg.showFps);
            note("Noclip, macro, speed and bot runs are never saved", r4);
        } else {
            label("API key", r1);
            input(260, r1, 250, "sk-ant-...", cfg.key,
                  [](std::string const& s) { cfg.key = s; }, nullptr, true);
            label("Model", r2);
            input(260, r2, 250, "claude-sonnet-5-5", cfg.model,
                  [](std::string const& s) { cfg.model = s; });
            label("Theme", r3);
            input(260, r3, 250, "neon cyber city, lots of glow", m_theme,
                  [this](std::string const& s) { m_theme = s; });
            if (m_ed) {
                button("Build", 110, 100, r4, menu_selector(ToolkitMenu::onBuild));
                m_status = CCLabelBMFont::create("", "bigFont.fnt");
                m_status->setScale(.4f);
                put(m_status, 285.f, r4);
            } else note("Open this from the editor pause menu to use it", r4);
        }
    }

    // ----- callbacks -----
    void onTab(CCObject* s) { page(static_cast<CCNode*>(s)->getTag()); }
    void onToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        bool on = !t->isToggled();   // GD calls this before the state flips
        switch (t->getTag()) {
            case 1: cfg.sync = on; break;
            case 2: cfg.frames = on; break;
            case 3: cfg.click = on; break;
            case 4: bot::armed = on; break;
            case 5: cfg.noclip = on; break;
            case 6:
                cfg.showFps = on;
                CCDirector::get()->setDisplayStats(on);
                break;
        }
    }
    void onMacro(CCObject*) {
        cfg.macro = cfg.macro == "off" ? "record" : cfg.macro == "record" ? "play" : "off";
        if (m_macroBtn) m_macroBtn->setString(cfg.macro.c_str());
    }
    void onSpeed(CCObject* s) {
        float v = cfg.speed + static_cast<CCNode*>(s)->getTag() / 100.f;
        cfg.speed = std::round(std::clamp(v, .1f, 5.f) * 100.f) / 100.f;
        showSpeed();
    }

    // ----- auto deco -----
    void onDeco(CCObject*) {
        if (cfg.deco <= 0) return (void)Notification::create("Set a deco object ID", NotificationIcon::Error)->show();
        std::set<std::pair<int, int>> cells;
        std::vector<CCPoint> tops;
        CCObject* o;
        CCARRAY_FOREACH(m_ed->m_objects, o) {
            auto g = static_cast<GameObject*>(o);
            if (g->m_objectType != GameObjectType::Solid) continue;
            auto p = g->getPosition();
            cells.insert({(int)p.x / 30, (int)p.y / 30});
            tops.push_back(p);
        }
        int n = 0;
        for (auto& p : tops)
            if (!cells.count({(int)p.x / 30, (int)p.y / 30 + 1})) {
                m_ed->createObject(cfg.deco, {p.x, p.y + 30}, false);
                n++;
            }
        Notification::create(fmt::format("Added {} deco", n), NotificationIcon::Success)->show();
    }

    // ----- AI build -----
    void onBuild(CCObject*) {
        if (cfg.key.empty()) return (void)Notification::create("Enter your API key first", NotificationIcon::Error)->show();
        float minX = 1e9, maxX = -1e9, maxY = 0;
        CCObject* o;
        CCARRAY_FOREACH(m_ed->m_objects, o) {
            auto p = static_cast<GameObject*>(o)->getPosition();
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y);
        }
        if (minX > maxX) { minX = 0; maxX = 3000; maxY = 300; }
        auto prompt = fmt::format(
            "Geometry Dash decoration generator. Theme: {}. Return ONLY a JSON array of up to 150 "
            "objects like [{{\"id\":211,\"x\":300,\"y\":90}}]. Use non-solid decoration object IDs, "
            "x between {} and {}, y between 0 and {}, all multiples of 15.",
            m_theme, (int)minX, (int)maxX, (int)maxY + 150);
        auto body = matjson::makeObject({
            {"model", cfg.model},
            {"max_tokens", 4000},
            {"messages", std::vector<matjson::Value>{
                matjson::makeObject({{"role", "user"}, {"content", prompt}})}}});
        web::WebRequest req;
        req.header("x-api-key", cfg.key);
        req.header("anthropic-version", "2023-06-01");
        req.bodyJSON(body);
        m_task.spawn(req.post("https://api.anthropic.com/v1/messages"),
            [this](web::WebResponse res) { this->onAI(res); });
        if (m_status) m_status->setString("AI is building...");
    }
    void onAI(web::WebResponse res) {
        auto say = [this](std::string t, NotificationIcon ic) {
            if (m_status) m_status->setString(t.c_str());
            Notification::create(t, ic)->show();
        };
        if (!res.ok()) return say("Request failed", NotificationIcon::Error);
        auto j = res.json().unwrapOr(matjson::Value());
        auto t = j["content"][0]["text"].asString().unwrapOr("");
        auto a = t.find('['), z = t.rfind(']');
        if (a == std::string::npos || z == std::string::npos) return say("Bad AI reply", NotificationIcon::Error);
        auto arr = matjson::parse(t.substr(a, z - a + 1)).unwrapOr(matjson::Value());
        int n = 0;
        for (auto& o : arr.asArray().unwrapOr({})) {
            int id = o["id"].asInt().unwrapOr(0);
            if (id <= 0) continue;
            m_ed->createObject(id, {(float)o["x"].asDouble().unwrapOr(0), (float)o["y"].asDouble().unwrapOr(0)}, false);
            n++;
        }
        say(fmt::format("Placed {} objects", n), NotificationIcon::Success);
    }
};

// ---------- Entry buttons (main menu, pause, editor pause) ----------
static void addToolkitBtn(CCNode* parent, CCObject* target, SEL_MenuHandler h, CCPoint pos) {
    CCNode* img = CCSprite::create("btn.png"_spr);
    if (img) img->setScale(40.f / img->getContentSize().width);
    else img = ButtonSprite::create("Toolkit", 70, true, "bigFont.fnt", "GJ_button_01.png", 26.f, .6f);
    auto m = CCMenu::create();
    m->setPosition(pos);
    m->addChild(CCMenuItemSpriteExtra::create(img, target, h));
    parent->addChild(m, 100);
}

class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        loadCfg();
        CCDirector::get()->setDisplayStats(cfg.showFps);
        auto ws = CCDirector::get()->getWinSize();
        addToolkitBtn(this, this, menu_selector(MyMenuLayer::onToolkit), {ws.width - 32, ws.height / 2 - 40});
        return true;
    }
    void onToolkit(CCObject*) { ToolkitMenu::create(nullptr)->show(); }
};

class $modify(MyPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto ws = CCDirector::get()->getWinSize();
        addToolkitBtn(this, this, menu_selector(MyPause::onToolkit), {ws.width - 32, 120});
    }
    void onToolkit(CCObject*) { ToolkitMenu::create(nullptr)->show(); }
};

class $modify(MyEditorPause, EditorPauseLayer) {
    void customSetup() {
        EditorPauseLayer::customSetup();
        addToolkitBtn(this, this, menu_selector(MyEditorPause::onToolkit), {75, 90});
    }
    void onToolkit(CCObject*) { ToolkitMenu::create(m_editorLayer)->show(); }
};
