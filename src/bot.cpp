// Pathfinder: rewind search. Tries inputs in short chunks, rewinds on death with
// checkpoints, and keeps only the last WINDOW decisions in memory. The winning
// path is handed to main.cpp as a normal macro.
#include "bot.hpp"
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <bit>
#include <chrono>
#include <deque>
#include <unordered_set>
using namespace geode::prelude;

// ---- tuning ----
constexpr int    CH      = 3;            // update() calls per decision (3 = 80 Hz inputs)
constexpr int    WINDOW  = 80;           // decisions that can still be rewound (~1 s)
constexpr int    MAXFLIP = 6;            // max input changes inside that window
constexpr int    LIMIT   = 30000;        // chunk tries allowed between two commits
constexpr float  SIM_DT  = 1.002f / 240.f;
constexpr double BUDGET  = 10.0;         // ms of search per rendered frame

namespace bot { bool armed = false; }

namespace {
    struct BNode { CheckpointObject* cp; uint64_t key; int frame, flips; bool held; int tried; bool capHit; };
    struct BStep { int frame; bool act; };

    bool running = false, pending = false, dead = false, won = false;
    bool want = false, held = false, inject = false, inSim = false;
    int nodes = 0, stall = 0;
    float bestX = 0.f;
    std::deque<BNode> stack;
    std::vector<BStep> done;
    std::unordered_set<uint64_t> failed;
    CCLabelBMFont* label = nullptr;

    void mute(bool m) {
        if (auto fm = FMODAudioEngine::sharedEngine()) {
            if (fm->m_globalChannel) fm->m_globalChannel->setMute(m);
            if (fm->m_backgroundMusicChannel) fm->m_backgroundMusicChannel->setMute(m);
        }
    }

    void teardown() {
        for (auto& n : stack) if (n.cp) n.cp->release();
        stack.clear(); done.clear(); failed.clear();
        if (label) label->removeFromParent();
        label = nullptr;
        if (running) mute(false);
        running = pending = false;
    }

    void fail(PlayLayer* pl, std::string const& why) {
        float pct = pl->m_levelLength > 0 ? bestX / pl->m_levelLength * 100.f : 0.f;
        log::warn("pathfinder stopped: {} (best {:.1f}%, frame {})", why, pct, g_frame);
        teardown();
        Notification::create(fmt::format("Pathfinder: {} ({:.0f}%)", why, pct), NotificationIcon::Error)->show();
        queueInMainThread([] { if (auto p = PlayLayer::get()) p->resetLevel(); });
    }

    uint64_t keyOf(PlayLayer* pl, bool act) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; h ^= h >> 29; };
        mix((uint64_t)g_frame);
        mix(act);
        for (auto* p : {pl->m_player1, pl->m_player2}) {
            if (!p) continue;
            mix(std::bit_cast<uint32_t>(p->getPositionX()));
            mix(std::bit_cast<uint32_t>(p->getPositionY()));
            mix(std::bit_cast<uint64_t>(p->m_yVelocity));
            mix((uint64_t)(p->m_isOnGround | p->m_isUpsideDown << 1 | p->m_isShip << 2 | p->m_isBall << 3 |
                           p->m_isBird << 4 | p->m_isDart << 5 | p->m_isRobot << 6 | p->m_isSpider << 7 |
                           p->m_isSwing << 8));
        }
        return h;
    }

    // Snapshot the current state as a node. Returns false if the game refused.
    bool snap(PlayLayer* pl, bool heldNow, int flips) {
        auto cp = pl->createCheckpoint();
        if (!cp) return false;
        cp->retain();
        stack.push_back({cp, keyOf(pl, heldNow), g_frame, flips, heldNow, 0, false});
        return true;
    }

    // Rewind to a node. Checkpoints don't store the held button, so put it back by hand.
    void restore(PlayLayer* pl, BNode const& n) {
        pl->loadFromCheckpoint(n.cp);
        g_frame = n.frame;
        pl->m_extraDelta = 0.0;
        held = n.held;
        for (auto* p : {pl->m_player1, pl->m_player2})
            if (p) p->m_holdingButtons[1] = n.held;
        dead = won = false;
    }

    // Simulate one decision (CH physics updates) with the button forced to `act`.
    void chunk(PlayLayer* pl, bool act) {
        want = act;
        inSim = true;
        for (int i = 0; i < CH && !dead && !won; i++) {
            pl->m_extraDelta = 0.0;
            pl->GJBaseGameLayer::update(SIM_DT);
            if (pl->m_levelEndAnimationStarted) won = true;
        }
        inSim = false;
    }

    void finish(PlayLayer* pl, bool act) {
        std::vector<Click> clicks;
        bool prev = false;
        auto add = [&](int f, bool a) { if (a != prev) { clicks.push_back({f, a, 1, true}); prev = a; } };
        for (auto& s : done) add(s.frame, s.act);
        for (size_t i = 0; i + 1 < stack.size(); i++) add(stack[i].frame, stack[i + 1].held);
        add(stack.back().frame, act);
        log::info("pathfinder: level solved, {} inputs", clicks.size());
        teardown();
        botDone(std::move(clicks));
    }

    void commit() {
        done.push_back({stack[0].frame, stack[1].held});
        stack.front().cp->release();
        stack.pop_front();
        nodes = 0;
        if (failed.size() > 3000000) failed.clear();
    }

    void step(PlayLayer* pl) {
        BNode& top = stack.back();
        if (top.tried == 3) {                           // both choices exhausted: dead end
            bool cap = top.capHit;
            if (!cap) failed.insert(top.key);
            top.cp->release();
            stack.pop_back();
            if (stack.empty()) return fail(pl, "no path found");
            stack.back().capHit |= cap;
            return;
        }
        bool keep = !(top.tried & 1);                   // try "keep the button" first
        top.tried |= keep ? 1 : 2;
        bool act = keep ? top.held : !top.held;
        if (!keep && top.flips - stack.front().flips >= MAXFLIP) { top.capHit = true; return; }
        if (++nodes > LIMIT) return fail(pl, "stuck");

        restore(pl, top);
        chunk(pl, act);
        bestX = std::max(bestX, pl->m_player1->getPositionX());
        if (won) return finish(pl, act);
        if (dead) return;
        if (g_frame == top.frame) { if (++stall > 200) return fail(pl, "game not advancing"); }
        else stall = 0;
        if (failed.contains(keyOf(pl, act))) return;
        if (!snap(pl, act, top.flips + (act != top.held))) return fail(pl, "checkpoint failed");
        if (stack.size() > WINDOW + 1) commit();
    }

    void startSearch(PlayLayer* pl) {
        pending = false;
        auto warn = [](char const* t) { Notification::create(t, NotificationIcon::Error)->show(); };
        if (pl->m_isPlatformer) return warn("Pathfinder: platformer levels aren't supported");
        if (pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode)
            return warn("Pathfinder: 2-player split mode isn't supported");
        if (pl->m_isPracticeMode || pl->m_isTestMode) return warn("Pathfinder: use normal mode");

        dead = won = want = held = false;
        nodes = stall = 0;
        bestX = 0.f;
        stack.clear(); done.clear(); failed.clear();
        if (!snap(pl, false, 0)) return warn("Pathfinder: couldn't create a checkpoint");

        label = CCLabelBMFont::create("Pathfinder...", "bigFont.fnt");
        label->setScale(0.45f);
        label->setPosition(CCDirector::get()->getWinSize().width / 2, CCDirector::get()->getWinSize().height - 18);
        pl->addChild(label, 130);
        mute(true);
        running = true;
    }

    void work(PlayLayer* pl) {
        auto t0 = std::chrono::steady_clock::now();
        while (running) {
            step(pl);
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > BUDGET)
                break;
        }
        if (running && label) {
            float pct = pl->m_levelLength > 0 ? bestX / pl->m_levelLength * 100.f : 0.f;
            label->setString(fmt::format("Pathfinder {:.0f}%  depth {}  tries {}", pct, stack.size(), nodes).c_str());
        }
    }
}

namespace bot {
    bool busy() { return running; }
    bool injecting() { return inject; }

    void begin(PlayLayer*) {
        if (!armed) return;
        armed = false;
        pending = true;
    }

    void cancel() { teardown(); }

    void tick(GJBaseGameLayer* gl) {
        if (!running || want == held) return;
        held = want;
        inject = true;
        gl->handleButton(want, 1, true);
        inject = false;
    }
}

class $modify(BotGL, GJBaseGameLayer) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this && !inSim) {
            if (pending && this->m_started) startSearch(pl);
            if (running) return work(pl);          // the real game stands still while searching
        }
        GJBaseGameLayer::update(dt);
    }
};

class $modify(BotPL, PlayLayer) {
    void destroyPlayer(PlayerObject* p, GameObject* o) {
        if (running) { dead = true; return; }      // a search death is just a failed try
        if (botNoclip()) return;                   // noclip: ignore every hazard
        PlayLayer::destroyPlayer(p, o);
    }
    void levelComplete() {
        if (running) { won = true; return; }
        if (botSafeMode()) this->m_isTestMode = true;   // bot/macro/speed runs aren't saved or submitted
        PlayLayer::levelComplete();
    }
};
