#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <filesystem>
#include <random>

using namespace geode::prelude;

namespace {
    // Persists across levels/sessions so the cooldown isn't reset by leaving a level.
    int s_attemptsSinceTrigger = 1000000;

    std::mt19937& rng() {
        static std::mt19937 gen{std::random_device{}()};
        return gen;
    }

    float randf(float lo, float hi) {
        if (hi <= lo) return lo;
        return std::uniform_real_distribution<float>(lo, hi)(rng());
    }

    template <class T>
    T setting(char const* key) {
        return Mod::get()->getSettingValue<T>(key);
    }

    std::filesystem::path resolveAudioPath() {
        auto mod = Mod::get();
        std::error_code ec;

        auto custom = setting<std::filesystem::path>("custom-audio");
        if (!custom.empty()) {
            if (std::filesystem::exists(custom, ec)) return custom;
            log::warn("Custom audio '{}' doesn't exist, using the built-in sound", custom.string());
        }

        auto dir = mod->getResourcesDir();
        for (auto const& candidate : {
                 dir / "audio.ogg",
                 dir / mod->getID() / "audio.ogg",
                 dir.parent_path() / "audio.ogg",
             }) {
            if (std::filesystem::exists(candidate, ec)) return candidate;
        }
        return {};
    }
}

class $modify(MelonsPlayLayer, PlayLayer) {
    struct Fields {
        int bestAtStart = 0;
        bool rolled = false;      // already rolled the dice this attempt
        bool playing = false;     // audio currently playing
        bool musicPaused = false; // we paused the level music
        unsigned int endMs = 0;
        FMOD::Sound* sound = nullptr;
        FMOD::Channel* channel = nullptr;
    };

    // ---- helpers ---------------------------------------------------------

    int currentBest() {
        if (m_isPracticeMode) return m_level->m_practicePercent;
        return m_level->m_normalPercent.value();
    }

    bool startAudio() {
        auto f = m_fields.self();
        auto engine = FMODAudioEngine::get();
        if (!engine || !engine->m_system) return false;
        auto system = engine->m_system;

        auto path = resolveAudioPath();
        if (path.empty()) {
            log::error("No audio file found");
            return false;
        }

        FMOD::Sound* sound = nullptr;
        auto pathStr = path.string();
        if (system->createSound(pathStr.c_str(), FMOD_DEFAULT, nullptr, &sound) != FMOD_OK || !sound) {
            log::error("Failed to load audio '{}'", pathStr);
            return false;
        }

        unsigned int lenMs = 0;
        sound->getLength(&lenMs, FMOD_TIMEUNIT_MS);

        FMOD::Channel* channel = nullptr;
        if (system->playSound(sound, nullptr, true, &channel) != FMOD_OK || !channel) {
            log::error("Failed to play audio '{}'", pathStr);
            sound->release();
            return false;
        }

        unsigned int startMs = static_cast<unsigned int>(std::max(0.0, setting<double>("start-offset") * 1000.0));
        unsigned int endMs = static_cast<unsigned int>(std::max(0.0, setting<double>("end-offset") * 1000.0));
        if (lenMs > 0) {
            startMs = std::min(startMs, lenMs - 1);
            if (endMs == 0 || endMs > lenMs) endMs = lenMs;
        }
        if (endMs <= startMs) endMs = lenMs;

        if (startMs > 0) channel->setPosition(startMs, FMOD_TIMEUNIT_MS);
        channel->setVolume(static_cast<float>(setting<double>("volume")) / 100.f);

        if (setting<bool>("pause-music")) {
            if (auto bg = engine->m_backgroundMusicChannel) {
                bg->setPaused(true);
                f->musicPaused = true;
            }
        }

        channel->setPaused(false);

        f->sound = sound;
        f->channel = channel;
        f->endMs = endMs;
        f->playing = true;
        return true;
    }

    void stopAudio() {
        auto f = m_fields.self();
        if (f->channel) {
            f->channel->stop();
            f->channel = nullptr;
        }
        if (f->sound) {
            f->sound->release();
            f->sound = nullptr;
        }
        if (f->musicPaused) {
            if (auto engine = FMODAudioEngine::get()) {
                if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->setPaused(false);
            }
            f->musicPaused = false;
        }
        f->playing = false;
    }

    bool audioFinished() {
        auto f = m_fields.self();
        if (!f->channel) return true;

        bool isPlaying = false;
        if (f->channel->isPlaying(&isPlaying) != FMOD_OK || !isPlaying) return true;

        unsigned int pos = 0;
        if (f->channel->getPosition(&pos, FMOD_TIMEUNIT_MS) == FMOD_OK && f->endMs > 0 && pos >= f->endMs) {
            return true;
        }
        return false;
    }

    void flashScreen() {
        auto c = setting<ccColor3B>("flash-color");
        auto size = CCDirector::get()->getWinSize();
        auto layer = CCLayerColor::create({c.r, c.g, c.b, 200}, size.width, size.height);
        layer->setZOrder(10000);
        this->addChild(layer);
        layer->runAction(CCSequence::create(CCFadeOut::create(0.5f), CCRemoveSelf::create(), nullptr));
    }

    void triggerAudio() {
        if (!startAudio()) return;
        s_attemptsSinceTrigger = 0;

        if (setting<bool>("flash")) flashScreen();
        if (setting<bool>("notify")) {
            Notification::create("Melons Death triggered!", NotificationIcon::None, 1.5f)->show();
        }
    }

    void onAudioEnded() {
        bool kill = setting<bool>("kill-on-end");
        stopAudio();
        if (kill && m_player1 && !m_player1->m_isDead) {
            this->destroyPlayer(m_player1, m_anticheatSpike);
        }
    }

    void checkNewBest() {
        auto f = m_fields.self();
        if (f->rolled) return;
        if (m_isTestMode || m_level->isPlatformer()) return;
        if (!m_player1 || m_player1->m_isDead) return;

        int pct = static_cast<int>(this->getCurrentPercent());
        if (pct >= 100 || pct <= f->bestAtStart) return; // not a new best (yet)
        if (pct < setting<int64_t>("min-percent")) return;

        // From here on we've reached a "new best" for this attempt: roll exactly once.
        f->rolled = true;

        if (!setting<bool>("enabled")) return;
        if (m_isPracticeMode && !setting<bool>("practice")) return;
        if (s_attemptsSinceTrigger <= setting<int64_t>("cooldown")) return;

        float chance;
        if (setting<std::string>("chance-mode") == "Randomized %") {
            float lo = static_cast<float>(setting<double>("random-min"));
            float hi = static_cast<float>(setting<double>("random-max"));
            if (lo > hi) std::swap(lo, hi);
            chance = randf(lo, hi);
        }
        else {
            chance = static_cast<float>(setting<double>("chance"));
        }

        if (randf(0.f, 100.f) < chance) this->triggerAudio();
    }

    // ---- hooks -----------------------------------------------------------

    void resetLevel() {
        this->stopAudio();
        PlayLayer::resetLevel();

        auto f = m_fields.self();
        f->rolled = false;
        f->bestAtStart = this->currentBest();
        s_attemptsSinceTrigger++;
    }

    void update(float dt) {
        auto f = m_fields.self();

        if (f->playing) {
            if (this->audioFinished()) {
                this->onAudioEnded();
            }
            else if (setting<bool>("freeze")) {
                return; // skip the game update = icon & level frozen until audio ends
            }
        }

        PlayLayer::update(dt);

        if (!f->playing) this->checkNewBest();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // If you die by yourself while the audio is playing (freeze off), cut the audio.
        if (m_fields->playing) this->stopAudio();
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        this->stopAudio();
        PlayLayer::levelComplete();
    }

    void onQuit() {
        this->stopAudio();
        PlayLayer::onQuit();
    }
};
