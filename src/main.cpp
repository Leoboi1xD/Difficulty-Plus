#include <Geode/Geode.hpp>
#include <Geode/modify/LevelCell.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/StarInfoPopup.hpp>


#include <algorithm>
#include <cmath>
#include <array>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace dp {
enum class Tier {
    None = 0,
    Casual,
    Normal,
    Tough,
    Extreme,
};

struct TierInfo {
    char const* settingKey;
    char const* frameName;

    bool gameFrame;
};

static TierInfo const kTiers[] = {
    { "",               "",                          false },
    { "enable-casual",  "DP_casual_001.png",         false },
    { "enable-normal",  "difficulty_02_btn_001.png", true  },
    { "enable-tough",   "DP_tough_001.png",          false },
    { "enable-extreme", "DP_extreme_001.png",        false },
};

static Tier tierFor(GJGameLevel* level) {
    if (!level) return Tier::None;

    switch (level->m_stars.value()) {
        case 3:  return Tier::Casual;
        case 4:  return Tier::Normal;
        case 7:  return Tier::Tough;
        case 9:  return Tier::Extreme;
        default: return Tier::None;
    }
}

static std::string modFrame(char const* name) {
    std::string id(Mod::get()->getID());
    id += '/';
    id += name;
    return id;
}

static std::string frameID(TierInfo const& info) {
    if (info.gameFrame) return info.frameName;
    return modFrame(info.frameName);
}

static GJDifficultySprite* findDifficultySprite(CCNode* node) {
    if (!node) return nullptr;
    if (auto diff = typeinfo_cast<GJDifficultySprite*>(node)) return diff;

    for (int i = 0; i < static_cast<int>(node->getChildrenCount()); ++i) {
        if (auto found = findDifficultySprite(node->getChildByIndex(i))) return found;
    }
    return nullptr;
}

static void countCompletedByStars(int* counts, int size) {
    auto glm = GameLevelManager::sharedState();
    auto gsm = GameStatsManager::sharedState();
    if (!glm || !gsm || !glm->m_onlineLevels) return;

    for (auto [id, level] :
             CCDictionaryExt<std::string_view, GJGameLevel*>(glm->m_onlineLevels)) {
        if (!level) continue;
        if (!gsm->hasCompletedLevel(level)) continue;
        auto stars = level->m_stars.value();
        if (stars >= 0 && stars < size) counts[stars]++;
    }
}

static void splitTier(int total, int lowSeen, int highSeen, int& low, int& high) {
    auto seen = lowSeen + highSeen;
    if (total <= 0 || seen <= 0) {
        low = total > 0 ? total : 0;
        high = 0;
        return;
    }
    high = static_cast<int>(std::lround(static_cast<double>(total) * highSeen / seen));
    if (high < 0) high = 0;
    if (high > total) high = total;
    low = total - high;
}

static void swapFace(GJDifficultySprite* sprite, std::string const& id) {
    if (!sprite) return;

    auto frame = CCSpriteFrameCache::get()->spriteFrameByName(id.c_str());
    if (!frame) return;

    auto before = sprite->getContentSize();
    auto scaleX = sprite->getScaleX();
    auto scaleY = sprite->getScaleY();

    sprite->setDisplayFrame(frame);

    auto after = sprite->getContentSize();
    if (before.height > 0.f && after.height > 0.f) {
        auto factor = before.height / after.height;
        sprite->setScaleX(scaleX * factor);
        sprite->setScaleY(scaleY * factor);
    }
}

static void collectDifficultySprites(CCNode* node, std::vector<GJDifficultySprite*>& out) {
    if (!node) return;
    if (auto diff = typeinfo_cast<GJDifficultySprite*>(node)) {
        out.push_back(diff);
        return;
    }
    for (int i = 0; i < static_cast<int>(node->getChildrenCount()); ++i) {
        collectDifficultySprites(node->getChildByIndex(i), out);
    }
}

static void applySprite(GJDifficultySprite* sprite, GJGameLevel* level) {
    if (!sprite) return;

    auto tier = tierFor(level);
    if (tier == Tier::None) return;

    auto const& info = kTiers[static_cast<size_t>(tier)];
    if (!Mod::get()->getSettingValue<bool>(info.settingKey)) return;

    auto id = frameID(info);
    auto frame = CCSpriteFrameCache::get()->spriteFrameByName(id.c_str());
    if (!frame) {
        log::warn("sprite frame \"{}\" is missing - was the spritesheet packed?", id);
        return;
    }

    auto before = sprite->getContentSize();
    auto scaleX = sprite->getScaleX();
    auto scaleY = sprite->getScaleY();

    sprite->setDisplayFrame(frame);

    auto after = sprite->getContentSize();
    if (before.height > 0.f && after.height > 0.f) {
        auto factor = before.height / after.height;
        sprite->setScaleX(scaleX * factor);
        sprite->setScaleY(scaleY * factor);

        auto shift = cocos2d::CCPoint(
            (after.width - before.width) / 2.f,
            (after.height - before.height) / 2.f
        );
        for (int i = 0; i < static_cast<int>(sprite->getChildrenCount()); ++i) {
            auto child = sprite->getChildByIndex(i);
            if (!child) continue;
            child->setPosition(child->getPosition() + shift);
            child->setScaleX(child->getScaleX() / factor);
            child->setScaleY(child->getScaleY() / factor);
        }
    }
}

struct FilterInfo {
    int stars;
    int tierDiff;
    char const* frame;
};

static FilterInfo const kFilters[] = {
    { 4, 3, nullptr              },
    { 7, 4, "DP_tough_001.png"   },
    { 9, 5, "DP_extreme_001.png" },
};

static int const kInsertAfter[] = { 2, 4, 5 };

static constexpr float kRowWiden = 1.05f;
static constexpr float kRowScaleBoost = 0.97f;

static constexpr int kFilterTagBase = 74000;

struct TierClaim {
    int diff;
    int stars;
};

static TierClaim const kTierClaims[] = {
    { 3, 4 },
    { 4, 7 },
    { 5, 9 },
};

static int g_uiFilter = 0;
static int g_activeFilter = 0;
static std::vector<int> g_excludedStars;
static std::string g_activeDiff;

static std::string filterFrameID(FilterInfo const& f) {
    if (!f.frame) return "difficulty_02_btn_001.png";
    return modFrame(f.frame);
}

static void refreshFilterRow(CCNode* menu) {
    if (!menu) return;
    for (int i = 0; i < static_cast<int>(menu->getChildrenCount()); ++i) {
        auto child = menu->getChildByIndex(i);
        if (!child) continue;
        auto tag = child->getTag();
        if (tag < kFilterTagBase || tag > kFilterTagBase + 99) continue;
        auto on = (tag - kFilterTagBase) == g_uiFilter;

        auto shade = on ? 255 : 145;
        for (int j = 0; j < static_cast<int>(child->getChildrenCount()); ++j) {
            if (auto spr = typeinfo_cast<CCSprite*>(child->getChildByIndex(j))) {
                spr->setColor({ static_cast<GLubyte>(shade),
                                static_cast<GLubyte>(shade),
                                static_cast<GLubyte>(shade) });
            }
        }
    }
}

static bool diffSelects(std::string const& diff, int value) {
    auto target = std::to_string(value);
    size_t pos = 0;
    while (true) {
        auto next = diff.find(',', pos);
        auto token = diff.substr(pos, next == std::string::npos
                                          ? std::string::npos : next - pos);
        if (token == target) return true;
        if (next == std::string::npos) return false;
        pos = next + 1;
    }
}

static bool starsRelabelled(int stars) {
    char const* key = nullptr;
    switch (stars) {
        case 3: key = "enable-casual";  break;
        case 4: key = "enable-normal";  break;
        case 7: key = "enable-tough";   break;
        case 9: key = "enable-extreme"; break;
        default: return false;
    }
    return Mod::get()->getSettingValue<bool>(key);
}

static bool listsMode() {
    auto gm = GameManager::sharedState();
    return gm && gm->m_levelSearchType != 0;
}

static bool isListSearch(GJSearchObject* search) {
    if (!search) return false;
    if (search->m_searchMode != 0) return true;
    switch (search->m_searchType) {
        case SearchType::LevelListsOnClick:
        case SearchType::MyLists:
        case SearchType::FavouriteLists:
            return true;
        default:
            return false;
    }
}

static bool narrowingActive(GJSearchObject* search) {
    if (!search || isListSearch(search)) return false;
    if (!g_activeFilter && g_excludedStars.empty()) return false;
    return std::string(search->m_difficulty.c_str()) == g_activeDiff;
}

static constexpr int kMaxExtraFetches = 4;
static constexpr unsigned int kServerPageSize = 10;
static constexpr unsigned int kTargetRows = 10;

static Ref<CCArray> g_accum = nullptr;
static std::string g_accumKey;
static int g_fetchCount = 0;
static int g_nextServerPage = 0;
static int g_accumDisplayPage = 0;

static bool g_cursorValid = false;
static std::string g_cursorKey;
static int g_cursorDisplayPage = 0;
static int g_cursorNextServerPage = 0;

static void resetFetch() {
    g_accum = nullptr;
    g_accumKey.clear();
    g_fetchCount = 0;
    g_nextServerPage = 0;
    g_accumDisplayPage = 0;
}

static std::string searchIdentity(GJSearchObject* search) {
    std::string id(search->m_difficulty.c_str());
    id += '|';
    id += search->m_searchQuery.c_str();
    return id;
}

static int resumeServerPage(GJSearchObject* search) {
    auto page = search->m_page;
    if (g_cursorValid && g_cursorKey == searchIdentity(search)
        && page == g_cursorDisplayPage + 1) {
        return g_cursorNextServerPage;
    }
    return page;
}

static CCArray* narrowLevels(GJSearchObject* search, CCArray* items) {
    if (!items || !narrowingActive(search)) return items;

    auto out = CCArray::create();
    for (unsigned int i = 0; i < items->count(); ++i) {
        auto obj = items->objectAtIndex(i);
        auto level = typeinfo_cast<GJGameLevel*>(obj);
        if (!level) {
            out->addObject(obj);
            continue;
        }
        auto stars = level->m_stars.value();
        auto keep = true;
        if (g_activeFilter) {
            keep = (stars == g_activeFilter);
        } else {
            for (auto excluded : g_excludedStars) {
                if (stars == excluded) { keep = false; break; }
            }
        }
        if (keep) out->addObject(obj);
    }
    return out;
}

static void applyTo(CCNode* root, GJGameLevel* level) {
    applySprite(findDifficultySprite(root), level);
}



}

class $modify(DPLevelCell, LevelCell) {
    void loadCustomLevelCell() {
        LevelCell::loadCustomLevelCell();
        dp::applyTo(this, m_level);
    }

    void loadFromLevel(GJGameLevel* level) {
        LevelCell::loadFromLevel(level);
        dp::applyTo(this, level);
    }
};

class $modify(DPLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        dp::applySprite(m_difficultySprite, level);
        return true;
    }

    void updateLabelValues() {
        LevelInfoLayer::updateLabelValues();
        dp::applySprite(m_difficultySprite, m_level);
    }
};

class $modify(DPLevelSearchLayer, LevelSearchLayer) {
    struct Fields {
        std::vector<cocos2d::CCPoint> m_origPos;
        std::vector<float> m_origScale;
        bool m_captured = false;
    };

    bool init(int type) {
        if (!LevelSearchLayer::init(type)) return false;
        this->rebuildRow();
        return true;
    }

    void onSearchMode(CCObject* sender) {
        LevelSearchLayer::onSearchMode(sender);
        this->rebuildRow();
    }

    void rebuildRow() {
        auto sprites = m_difficultySprites;
        if (!sprites || sprites->count() < 6) return;

        std::vector<CCNode*> btns;
        for (unsigned int i = 0; i < sprites->count(); ++i) {
            auto spr = typeinfo_cast<CCNode*>(sprites->objectAtIndex(i));
            if (auto b = spr ? spr->getParent() : nullptr) btns.push_back(b);
        }
        if (btns.size() < 6) return;

        std::sort(btns.begin(), btns.end(), [](CCNode* a, CCNode* b) {
            return a->getPositionX() < b->getPositionX();
        });

        auto menu = typeinfo_cast<CCMenu*>(btns.front()->getParent());
        if (!menu) return;

        if (!m_fields->m_captured) {
            for (auto b : btns) {
                m_fields->m_origPos.push_back(b->getPosition());
                m_fields->m_origScale.push_back(b->getScale());
            }
            m_fields->m_captured = true;
        }

        std::vector<CCNode*> ours;
        for (int i = 0; i < static_cast<int>(menu->getChildrenCount()); ++i) {
            auto child = menu->getChildByIndex(i);
            if (!child) continue;
            auto tag = child->getTag();
            if (tag >= dp::kFilterTagBase && tag <= dp::kFilterTagBase + 99) {
                ours.push_back(child);
            }
        }
        for (auto child : ours) child->removeFromParent();

        for (size_t i = 0; i < btns.size() && i < m_fields->m_origPos.size(); ++i) {
            btns[i]->setPosition(m_fields->m_origPos[i]);
            btns[i]->setScale(m_fields->m_origScale[i]);
        }

        auto lists = dp::listsMode();

        if (btns.size() > 2) {
            auto wantCasual = !lists && dp::starsRelabelled(3);
            auto id = wantCasual ? dp::modFrame("DP_casual_001.png")
                                 : std::string("difficulty_02_btn_001.png");
            if (auto frame = CCSpriteFrameCache::get()->spriteFrameByName(id.c_str())) {
                for (int j = 0; j < static_cast<int>(btns[2]->getChildrenCount()); ++j) {
                    if (auto spr = typeinfo_cast<CCSprite*>(btns[2]->getChildByIndex(j))) {
                        spr->setDisplayFrame(frame);
                        break;
                    }
                }
            }
        }

        if (lists) return;

        if (dp::g_uiFilter && !dp::starsRelabelled(dp::g_uiFilter)) {
            dp::g_uiFilter = 0;
        }

        auto sampleSpr = typeinfo_cast<CCNode*>(sprites->objectAtIndex(0));
        auto sprScale = sampleSpr ? sampleSpr->getScale() : 1.f;
        auto baseBtnScale = m_fields->m_origScale.empty()
                                ? btns.front()->getScale()
                                : m_fields->m_origScale.front();
        auto minX = btns.front()->getPositionX();
        auto maxX = btns.back()->getPositionX();
        auto y = btns.front()->getPositionY();
        auto vanillaCount = static_cast<int>(btns.size());

        auto extraCount = static_cast<int>(sizeof(dp::kFilters) / sizeof(dp::kFilters[0]));
        std::vector<CCNode*> row;
        for (int i = 0; i < vanillaCount; ++i) {
            row.push_back(btns[i]);
            for (int k = 0; k < extraCount; ++k) {
                if (dp::kInsertAfter[k] != i) continue;
                auto const& f = dp::kFilters[k];
                if (!dp::starsRelabelled(f.stars)) continue;
                auto frame = CCSpriteFrameCache::get()->spriteFrameByName(
                    dp::filterFrameID(f).c_str()
                );
                if (!frame) continue;
                auto spr = CCSprite::createWithSpriteFrame(frame);
                if (!spr) continue;
                spr->setScale(sprScale);
                auto btn = CCMenuItemSpriteExtra::create(
                    spr, this, menu_selector(DPLevelSearchLayer::onStarFilter)
                );
                if (!btn) continue;
                btn->setTag(dp::kFilterTagBase + f.stars);
                menu->addChild(btn);
                row.push_back(btn);
            }
        }
        if (static_cast<int>(row.size()) <= vanillaCount) return;

        auto total = static_cast<int>(row.size());
        auto centerX = (minX + maxX) / 2.f;
        auto halfSpan = (maxX - minX) / 2.f * dp::kRowWiden;
        auto left = centerX - halfSpan;
        auto step = (total > 1) ? (halfSpan * 2.f) / static_cast<float>(total - 1) : 0.f;

        auto shrink = std::min(
            1.f,
            static_cast<float>(vanillaCount) / static_cast<float>(total) * dp::kRowScaleBoost
        );

        for (int i = 0; i < total; ++i) {
            row[i]->setScale(baseBtnScale * shrink);
            row[i]->setPosition({ left + step * i, y });
        }

        dp::refreshFilterRow(menu);
    }

    void onStarFilter(CCObject* sender) {
        auto btn = typeinfo_cast<CCNode*>(sender);
        if (!btn) return;
        auto stars = btn->getTag() - dp::kFilterTagBase;

        dp::g_uiFilter = (dp::g_uiFilter == stars) ? 0 : stars;
        dp::refreshFilterRow(btn->getParent());
    }

    GJSearchObject* getSearchObject(SearchType type, gd::string query) {
        auto obj = LevelSearchLayer::getSearchObject(type, query);
        if (!obj) return obj;

        dp::g_activeFilter = 0;
        dp::g_excludedStars.clear();
        dp::g_activeDiff.clear();

        if (dp::g_uiFilter && !dp::starsRelabelled(dp::g_uiFilter)) {
            dp::g_uiFilter = 0;
        }

        if (dp::isListSearch(obj)) return obj;

        if (dp::g_uiFilter) {
            for (auto const& f : dp::kFilters) {
                if (f.stars != dp::g_uiFilter) continue;
                auto diff = std::to_string(f.tierDiff);
                obj->m_difficulty = diff;
                dp::g_activeFilter = f.stars;
                dp::g_activeDiff = diff;
                break;
            }
            return obj;
        }

        auto diff = std::string(obj->m_difficulty.c_str());
        for (auto const& claim : dp::kTierClaims) {
            if (!dp::diffSelects(diff, claim.diff)) continue;
            if (!dp::starsRelabelled(claim.stars)) continue;
            dp::g_excludedStars.push_back(claim.stars);
        }
        if (!dp::g_excludedStars.empty()) dp::g_activeDiff = diff;

        return obj;
    }
};

class $modify(DPLevelBrowserLayer, LevelBrowserLayer) {
    void setupLevelBrowser(CCArray* items) {
        LevelBrowserLayer::setupLevelBrowser(dp::narrowLevels(m_searchObject, items));
    }

    void loadLevelsFinished(CCArray* levels, char const* key, int type) {
        if (!dp::narrowingActive(m_searchObject)) {
            dp::resetFetch();
            LevelBrowserLayer::loadLevelsFinished(levels, key, type);
            return;
        }

        if (!dp::g_accum) {
            dp::g_accum = CCArray::create();
            dp::g_accumKey = key ? key : "";
            dp::g_fetchCount = 0;
            dp::g_accumDisplayPage = m_searchObject->m_page;

            auto resume = dp::resumeServerPage(m_searchObject);
            if (resume != dp::g_accumDisplayPage) {
                if (auto obj = m_searchObject->getPageObject(resume)) {
                    dp::g_nextServerPage = resume + 1;
                    dp::g_fetchCount++;
                    GameLevelManager::sharedState()->getOnlineLevels(obj);
                    return;
                }
            }
            dp::g_nextServerPage = dp::g_accumDisplayPage + 1;
        }
        if (auto kept = dp::narrowLevels(m_searchObject, levels)) {
            dp::g_accum->addObjectsFromArray(kept);
        }

        auto rawCount = levels ? levels->count() : 0u;
        auto shortPage = dp::g_accum->count() < dp::kTargetRows;
        auto serverHasMore = rawCount >= dp::kServerPageSize;

        if (shortPage && serverHasMore && dp::g_fetchCount < dp::kMaxExtraFetches) {
            if (auto next = m_searchObject->getPageObject(dp::g_nextServerPage)) {
                dp::g_fetchCount++;
                dp::g_nextServerPage++;

                GameLevelManager::sharedState()->getOnlineLevels(next);
                return;
            }
        }

        dp::g_cursorValid = true;
        dp::g_cursorKey = dp::searchIdentity(m_searchObject);
        dp::g_cursorDisplayPage = dp::g_accumDisplayPage;
        dp::g_cursorNextServerPage = dp::g_nextServerPage;

        auto out = dp::g_accum;
        auto outKey = dp::g_accumKey;
        dp::resetFetch();
        LevelBrowserLayer::loadLevelsFinished(out, outKey.c_str(), type);
    }

    void loadLevelsFailed(char const* key, int type) {
        if (dp::g_accum && dp::g_accum->count() > 0) {
            auto out = dp::g_accum;
            auto outKey = dp::g_accumKey;
            dp::resetFetch();
            LevelBrowserLayer::loadLevelsFinished(out, outKey.c_str(), type);
            return;
        }
        dp::resetFetch();
        LevelBrowserLayer::loadLevelsFailed(key, type);
    }
};

class $modify(DPStarInfoPopup, StarInfoPopup) {
    bool init(int autos, int easies, int normals, int hards, int harders,
              int insanes, int dailies, int gauntlets, int maps, bool platformer) {
        if (!StarInfoPopup::init(autos, easies, normals, hards, harders,
                                 insanes, dailies, gauntlets, maps, platformer)) {
            return false;
        }

        std::vector<GJDifficultySprite*> faces;
        dp::collectDifficultySprites(this, faces);
        if (faces.size() < 6) return true;

        std::sort(faces.begin(), faces.end(),
                  [](GJDifficultySprite* a, GJDifficultySprite* b) {
                      return a->getPositionX() < b->getPositionX();
                  });

        auto parent = faces[0]->getParent();
        if (!parent) return true;

        auto faceY = faces[0]->getPositionY();
        auto faceScale = faces[0]->getScale();

        std::vector<CCLabelBMFont*> labels;
        for (int i = 0; i < static_cast<int>(parent->getChildrenCount()); ++i) {
            auto label = typeinfo_cast<CCLabelBMFont*>(parent->getChildByIndex(i));
            if (!label) continue;
            for (auto face : faces) {
                if (std::fabs(label->getPositionX() - face->getPositionX()) < 2.f &&
                    label->getPositionY() < faceY) {
                    labels.push_back(label);
                    break;
                }
            }
        }
        if (labels.size() != faces.size()) return true;

        std::sort(labels.begin(), labels.end(),
                  [](CCLabelBMFont* a, CCLabelBMFont* b) {
                      return a->getPositionX() < b->getPositionX();
                  });

        auto labelDrop = faceY - labels[0]->getPositionY();
        auto labelScale = labels[0]->getScale();

        int seen[16] = {};
        dp::countCompletedByStars(seen, 16);

        int n4 = 0, n5 = 0, n6 = 0, n7 = 0, n8 = 0, n9 = 0;
        dp::splitTier(hards,   seen[4], seen[5], n4, n5);
        dp::splitTier(harders, seen[6], seen[7], n6, n7);
        dp::splitTier(insanes, seen[8], seen[9], n8, n9);

        struct Slot {
            int stars;
            int count;
            int reuse;
            char const* frame;
        };

        Slot slots[] = {
            { 1, autos,   0,  nullptr                     },
            { 2, easies,  1,  nullptr                     },
            { 3, normals, 2,  "DP_casual_001.png"         },
            { 4, n4,     -1,  "difficulty_02_btn_001.png" },
            { 5, n5,      3,  nullptr                     },
            { 6, n6,      4,  nullptr                     },
            { 7, n7,     -1,  "DP_tough_001.png"          },
            { 8, n8,      5,  nullptr                     },
            { 9, n9,     -1,  "DP_extreme_001.png"        },
        };
        auto total = static_cast<int>(sizeof(slots) / sizeof(slots[0]));
        auto topCount = (total + 1) / 2;

        auto minX = faces.front()->getPositionX();
        auto maxX = faces.back()->getPositionX();
        auto centerX = (minX + maxX) / 2.f;
        auto step = (maxX - minX) / static_cast<float>(faces.size() - 1);

        auto topY = faceY + 30.f;
        auto bottomY = faceY - 45.f;

        // Two rows need more height than the panel was built for.
        if (auto bg = typeinfo_cast<CCScale9Sprite*>(
                this->getChildByIDRecursive("background"))) {
            auto size = bg->getContentSize();
            bg->setContentSize({ size.width, size.height + 45.f });
        }
        if (auto title = this->getChildByIDRecursive("classic-title")) {
            title->setPositionY(title->getPositionY() + 22.f);
        }
        if (auto ok = this->getChildByIDRecursive("ok-button")) {
            ok->setPositionY(ok->getPositionY() - 22.f);
        }

        for (int i = 0; i < total; ++i) {
            auto const& slot = slots[i];

            auto inTop = i < topCount;
            auto countInRow = inTop ? topCount : total - topCount;
            auto indexInRow = inTop ? i : i - topCount;
            auto rowLeft = centerX - step * (countInRow - 1) / 2.f;
            auto x = rowLeft + step * indexInRow;
            auto y = inTop ? topY : bottomY;

            CCNode* face = nullptr;
            if (slot.reuse >= 0) {
                face = faces[slot.reuse];
                if (slot.frame && dp::starsRelabelled(slot.stars)) {
                    dp::swapFace(faces[slot.reuse], dp::modFrame(slot.frame));
                }
            } else {
                if (!dp::starsRelabelled(slot.stars)) continue;
                auto id = std::string(slot.frame).rfind("DP_", 0) == 0
                              ? dp::modFrame(slot.frame)
                              : std::string(slot.frame);
                auto frame = CCSpriteFrameCache::get()->spriteFrameByName(id.c_str());
                if (!frame) continue;
                auto spr = CCSprite::createWithSpriteFrame(frame);
                if (!spr) continue;
                spr->setScale(faceScale);
                parent->addChild(spr);
                face = spr;

                // goldFont, not bigFont - that is what makes GD's numbers gold.
                auto label = CCLabelBMFont::create(std::to_string(slot.count).c_str(),
                                                   "goldFont.fnt");
                if (label) {
                    label->setScale(labelScale);
                    label->setPosition({ x, y - labelDrop });
                    parent->addChild(label);
                }
            }

            if (!face) continue;
            face->setPosition({ x, y });
            face->setScale(faceScale);

            if (slot.reuse >= 0) {
                auto label = labels[slot.reuse];
                label->setString(std::to_string(slot.count).c_str());
                label->setPosition({ x, y - labelDrop });
                label->setScale(labelScale);
            }
        }

        return true;
    }
};

$on_mod(Loaded) {
    log::info("Difficulty Plus loaded - Casual (3*), Normal (4*), Tough (7*), Extreme (9*)");
}
