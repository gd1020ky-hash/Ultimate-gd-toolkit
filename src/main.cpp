#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bot.hpp"

using namespace geode::prelude;

// ============================================================
// Ultimate GD Toolkit - main.cpp
// Target: Geometry Dash Android 2.2081 / Geode 5.10.1
// ============================================================

// ---------- Saved configuration ----------

struct Cfg {
    std::string macro = "off";
    std::string sound;
    std::string key;
    std::string model = "claude-sonnet-5-5";

    float speed = 1.f;

    bool sync = true;
    bool frames = true;
    bool click = false;
    bool noclip = false;
    bool showFps = false;

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
    cfg.model = m->getSavedValue<std::string>(
        "model", "claude-sonnet-5-5"
    );

    cfg.speed = static_cast<float>(
        m->getSavedValue<double>("speed", 1.0)
    );

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
    m->setSavedValue("speed", static_cast<double>(cfg.speed));
    m->setSavedValue("sync", cfg.sync);
    m->setSavedValue("frames", cfg.frames);
    m->setSavedValue("click", cfg.click);
    m->setSavedValue("noclip", cfg.noclip);
    m->setSavedValue("showFps", cfg.showFps);
    m->setSavedValue("deco", cfg.deco);
}

// ---------- Macro data ----------

static std::vector<Click> g_macro;

int g_frame = 0;

static size_t g_idx = 0;
static bool g_inject = false;
static CCLabelBMFont* g_label = nullptr;

static auto macroPath() {
    return Mod::get()->getSaveDir() / "macro.bin";
}

// These settings are indicators for the bot implementation.
// They do not themselves guarantee that a run is unsubmitted.
bool botSafeMode() {
    return cfg.macro != "off" || cfg.speed != 1.f || cfg.noclip;
}

bool botNoclip() {
    return cfg.noclip;
}

// Called by the Pathfinder implementation when it finds a route.
void botDone(std::vector<Click> clicks) {
    g_macro = std::move(clicks);

    std::ofstream f(macroPath(), std::ios::binary);

    for (auto const& c : g_macro) {
        f.write(
            reinterpret_cast<char const*>(&c),
            sizeof(c)
        );
    }

    cfg.macro = "play";
    saveCfg();

    Notification::create(
        fmt::format("Path found: {} inputs", g_macro.size()),
        NotificationIcon::Success
    )->show();

    queueInMainThread([] {
        g_frame = 0;
        g_idx = 0;

        if (auto pl = PlayLayer::get()) {
            pl->resetLevel();
        }
    });
}

// ---------- Speed control and music pitch ----------

class $modify(CCScheduler) {
    void update(float dt) {
        loadCfg();

        // Changes the time step used by scheduled updates.
        CCScheduler::update(dt * cfg.speed);

        auto fm = FMODAudioEngine::sharedEngine();

        if (fm && fm->m_backgroundMusicChannel) {
            fm->m_backgroundMusicChannel->setPitch(
                cfg.sync ? cfg.speed : 1.f
            );
        }
    }
};

// ---------- Macro playback and frame counter ----------

class $modify(GJBaseGameLayer) {
    void processCommands(float dt, bool halfTick, bool lastTick) {
        if (PlayLayer::get()) {
            if (bot::busy()) {
                bot::tick(this);
            } else if (cfg.macro == "play") {
                while (
                    g_idx < g_macro.size() &&
                    g_macro[g_idx].f <= g_frame
                ) {
                    auto const& c = g_macro[g_idx++];

                    g_inject = true;
                    this->handleButton(c.down, c.btn, c.p1);
                    g_inject = false;
                }
            }

            ++g_frame;

            if (g_label) {
                g_label->setVisible(cfg.frames);

                if (cfg.frames) {
                    g_label->setString(
                        fmt::format("Frame: {}", g_frame).c_str()
                    );
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
    }

    void handleButton(bool down, int btn, bool p1) {
        if (bot::busy()) {
            if (bot::injecting()) {
                GJBaseGameLayer::handleButton(down, btn, p1);
            }
            return;
        }

        GJBaseGameLayer::handleButton(down, btn, p1);

        if (!PlayLayer::get()) return;

        if (!g_inject && cfg.macro == "record") {
            g_macro.push_back({g_frame, down, btn, p1});
        }

        if (down && cfg.click && !cfg.sound.empty()) {
            FMODAudioEngine::sharedEngine()->playEffect(
                cfg.sound
            );
        }
    }
};

// ---------- Gameplay lifecycle ----------

class $modify(PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        g_frame = 0;
        g_idx = 0;

        if (cfg.macro == "play") {
            g_macro.clear();

            std::ifstream f(macroPath(), std::ios::binary);
            Click c;

            while (f.read(
                reinterpret_cast<char*>(&c),
                sizeof(c)
            )) {
                g_macro.push_back(c);
            }
        }

        g_label = CCLabelBMFont::create(
            "Frame: 0",
            "bigFont.fnt"
        );

        if (g_label) {
            g_label->setScale(0.4f);
            g_label->setAnchorPoint({0.f, 1.f});

            g_label->setPosition(
                5.f,
                CCDirector::get()->getWinSize().height - 5.f
            );

            g_label->setZOrder(100);
            this->addChild(g_label);
        }

        bot::begin(this);
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        g_frame = 0;
        g_idx = 0;

        if (cfg.macro == "record") {
            g_macro.clear();
        }
    }

    void onQuit() {
        bot::cancel();

        if (cfg.macro == "record") {
            std::ofstream f(macroPath(), std::ios::binary);

            for (auto const& c : g_macro) {
                f.write(
                    reinterpret_cast<char const*>(&c),
                    sizeof(c)
                );
            }
        }

        g_label = nullptr;
        PlayLayer::onQuit();
    }
};

// ============================================================
// Unified Toolkit menu
// ============================================================

class ToolkitMenu : public Popup {
    LevelEditorLayer* m_ed = nullptr;

    std::vector<CCNode*> m_items;
    CCMenuItemSpriteExtra* m_tabs[5] = {};

    bool m_drag = false;

    ButtonSprite* m_macroBtn = nullptr;
    CCLabelBMFont* m_speedLbl = nullptr;
    CCLabelBMFont* m_status = nullptr;

    std::string m_theme = "Classic";

    async::TaskHolder<web::WebResponse> m_task;

public:
    static ToolkitMenu* create(LevelEditorLayer* ed) {
        auto r = new ToolkitMenu();

        if (r->init(ed)) {
            r->autorelease();
            return r;
        }

        delete r;
        return nullptr;
    }

protected:
    bool init(LevelEditorLayer* ed) {
        if (!Popup::init(420.f, 300.f)) {
            return false;
        }

        loadCfg();
        m_ed = ed;

        this->setTitle("Ultimate GD Toolkit");

        char const* names[5] = {
            "Game", "Sound", "Deco", "AI", "Hacks"
        };

        for (int i = 0; i < 5; ++i) {
            auto sprite = ButtonSprite::create(
                names[i],
                70,
                true,
                "bigFont.fnt",
                "GJ_button_04.png",
                26.f,
                0.6f
            );

            m_tabs[i] = CCMenuItemSpriteExtra::create(
                sprite,
                this,
                menu_selector(ToolkitMenu::onTab)
            );

            m_tabs[i]->setTag(i);

            m_buttonMenu->addChildAtPosition(
                m_tabs[i],
                Anchor::Top,
                {(i - 2.f) * 80.f, -50.f}
            );
        }

        this->page(0);
        return true;
    }

    void onClose(CCObject* sender) override {
        saveCfg();
        Popup::onClose(sender);
    }

    // Drag the popup using the title-bar region.
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        auto p = m_mainLayer->convertToNodeSpace(
            touch->getLocation()
        );

        m_drag =
            p.x > 0.f &&
            p.x < m_size.width &&
            p.y > m_size.height - 36.f &&
            p.y < m_size.height;

        return Popup::ccTouchBegan(touch, event);
    }

    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        if (!m_drag) {
            Popup::ccTouchMoved(touch, event);
            return;
        }

        auto ws = CCDirector::get()->getWinSize();

        auto pos =
            m_mainLayer->getPosition() +
            (touch->getLocation() - touch->getPreviousLocation());

        m_mainLayer->setPosition({
            std::clamp(pos.x, 0.f, ws.width),
            std::clamp(pos.y, 0.f, ws.height)
        });
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        m_drag = false;
        Popup::ccTouchEnded(touch, event);
    }

    void ccTouchCancelled(
        CCTouch* touch,
        CCEvent* event
    ) override {
        m_drag = false;
        Popup::ccTouchCancelled(touch, event);
    }

    // ---------- UI helpers ----------

    template <class N>
    N* put(N* node, float x, float y, bool menu = false) {
        CCNode* parent = menu
            ? static_cast<CCNode*>(m_buttonMenu)
            : static_cast<CCNode*>(m_mainLayer);

        parent->addChildAtPosition(
            node,
            Anchor::BottomLeft,
            {x, y}
        );

        m_items.push_back(node);
        return node;
    }

    void label(char const* text, float y) {
        auto l = CCLabelBMFont::create(text, "bigFont.fnt");
        l->setScale(0.45f);

        put(
            l,
            26.f + l->getScaledContentWidth() / 2.f,
            y
        );
    }

    void note(char const* text, float y) {
        auto l = CCLabelBMFont::create(text, "bigFont.fnt");
        l->setScale(0.4f);
        l->setOpacity(170);

        put(l, m_size.width / 2.f, y);
    }

    ButtonSprite* button(
        char const* text,
        int width,
        float x,
        float y,
        SEL_MenuHandler handler,
        int tag = 0
    ) {
        auto sprite = ButtonSprite::create(
            text,
            width,
            true,
            "bigFont.fnt",
            "GJ_button_01.png",
            28.f,
            0.6f
        );

        auto item = CCMenuItemSpriteExtra::create(
            sprite,
            this,
            handler
        );

        item->setTag(tag);
        put(item, x, y, true);

        return sprite;
    }

    void toggler(float y, int tag, bool enabled) {
        auto item = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(ToolkitMenu::onToggle),
            0.65f
        );

        item->setTag(tag);
        item->toggle(enabled);

        put(item, m_size.width - 60.f, y, true);
    }

    template <class F>
    TextInput* input(
        float x,
        float y,
        float width,
        char const* placeholder,
        std::string const& value,
        F callback,
        char const* filter = nullptr,
        bool password = false
    ) {
        auto field = TextInput::create(
            width,
            placeholder,
            "chatFont.fnt"
        );

        field->setString(value, false);
        field->setCallback(std::move(callback));

        if (filter) {
            field->setFilter(filter);
        }

        if (password) {
            field->setPasswordMode(true);
        }

        return put(field, x, y);
    }

    void showSpeed() {
        if (m_speedLbl) {
            m_speedLbl->setString(
                fmt::format("{:.2f}x", cfg.speed).c_str()
            );
        }
    }

    // ---------- Pages ----------

    void page(int i) {
        for (auto node : m_items) {
            node->removeFromParent();
        }

        m_items.clear();

        m_macroBtn = nullptr;
        m_speedLbl = nullptr;
        m_status = nullptr;

        for (int k = 0; k < 5; ++k) {
            auto sprite = static_cast<ButtonSprite*>(
                m_tabs[k]->getNormalImage()
            );

            sprite->updateBGImage(
                k == i
                    ? "GJ_button_01.png"
                    : "GJ_button_04.png"
            );
        }

        float W = m_size.width;
        float H = m_size.height;

        float r1 = H - 95.f;
        float r2 = H - 135.f;
        float r3 = H - 175.f;
        float r4 = H - 215.f;
        float r5 = H - 255.f;

        // Game and macro page
        if (i == 0) {
            label("Macro: off > record > play > off", r1);

            m_macroBtn = button(
                cfg.macro.c_str(),
                110,
                W - 95.f,
                r1,
                menu_selector(ToolkitMenu::onMacro)
            );

            label("Speed", r2);

            button(
                "<<", 36, 184.f, r2,
                menu_selector(ToolkitMenu::onSpeed), -50
            );

            button(
                "<", 36, 222.f, r2,
                menu_selector(ToolkitMenu::onSpeed), -5
            );

            m_speedLbl = CCLabelBMFont::create("", "bigFont.fnt");
            m_speedLbl->setScale(0.55f);
            put(m_speedLbl, 287.f, r2);
            showSpeed();

            button(
                ">", 36, 352.f, r2,
                menu_selector(ToolkitMenu::onSpeed), 5
            );

            button(
                ">>", 36, 390.f, r2,
                menu_selector(ToolkitMenu::onSpeed), 50
            );

            label("Sync music speed", r3);
            toggler(r3, 1, cfg.sync);

            label("Frame counter", r4);
            toggler(r4, 2, cfg.frames);

            label("Pathfinder next level", r5);
            toggler(r5, 4, bot::armed);
        }

        // Sound page
        else if (i == 1) {
            label("Click sound", r1);
            toggler(r1, 3, cfg.click);

            label("Sound file path", r2);

            input(
                W / 2.f,
                r3,
                360.f,
                "/storage/emulated/0/Download/click.ogg",
                cfg.sound,
                [](std::string const& value) {
                    cfg.sound = value;
                }
            );
        }

        // Decoration page
        else if (i == 2) {
            label("Decoration object ID", r1);

            input(
                W - 100.f,
                r1,
                110.f,
                "211",
                std::to_string(cfg.deco),
                [](std::string const& value) {
                    cfg.deco = std::atoi(value.c_str());
                },
                "0123456789"
            );

            if (m_ed) {
                button(
                    "Auto Deco",
                    150,
                    W / 2.f,
                    r2,
                    menu_selector(ToolkitMenu::onDeco)
                );
            } else {
                note(
                    "Open from the editor pause menu",
                    r2
                );
            }
        }

        // Hacks page
        else if (i == 4) {
            label("Noclip", r1);
            toggler(r1, 5, cfg.noclip);

            label("Show FPS", r2);
            toggler(r2, 6, cfg.showFps);

            note(
                "These settings do not guarantee a legitimate run",
                r4
            );
        }

        // AI and settings page
        else {
            label("API key", r1);

            input(
                260.f,
                r1,
                250.f,
                "API key",
                cfg.key,
                [](std::string const& value) {
                    cfg.key = value;
                },
                nullptr,
                true
            );

            label("Model", r2);

            input(
                260.f,
                r2,
                250.f,
                "Model name",
                cfg.model,
                [](std::string const& value) {
                    cfg.model = value;
                }
            );

            label("Theme", r3);

            input(
                260.f,
                r3,
                250.f,
                "Classic / Neon / Purple",
                m_theme,
                [this](std::string const& value) {
                    m_theme = value;
                }
            );

            if (m_ed) {
                button(
                    "Build",
                    110,
                    100.f,
                    r4,
                    menu_selector(ToolkitMenu::onBuild)
                );

                m_status = CCLabelBMFont::create(
                    "Ready",
                    "bigFont.fnt"
                );

                m_status->setScale(0.4f);
                put(m_status, 285.f, r4);
            } else {
                note(
                    "Open from the editor pause menu",
                    r4
                );
            }
        }
    }

    // ---------- Menu callbacks ----------

    void onTab(CCObject* sender) {
        page(static_cast<CCNode*>(sender)->getTag());
    }

    void onToggle(CCObject* sender) {
        auto toggle = static_cast<CCMenuItemToggler*>(sender);

        // Geode's toggle callback runs before the state flips.
        bool enabled = !toggle->isToggled();

        switch (toggle->getTag()) {
            case 1:
                cfg.sync = enabled;
                break;

            case 2:
                cfg.frames = enabled;
                break;

            case 3:
                cfg.click = enabled;
                break;

            case 4:
                bot::armed = enabled;
                break;

            case 5:
                cfg.noclip = enabled;
                break;

            case 6:
                cfg.showFps = enabled;
                CCDirector::get()->setDisplayStats(enabled);
                break;
        }

        saveCfg();
    }

    void onMacro(CCObject*) {
        if (cfg.macro == "off") {
            cfg.macro = "record";
            g_macro.clear();
            g_frame = 0;
        } else if (cfg.macro == "record") {
            cfg.macro = "play";

            std::ofstream f(macroPath(), std::ios::binary);

            for (auto const& c : g_macro) {
                f.write(
                    reinterpret_cast<char const*>(&c),
                    sizeof(c)
                );
            }

            g_idx = 0;
        } else {
            cfg.macro = "off";
            g_idx = 0;
        }

        if (m_macroBtn) {
