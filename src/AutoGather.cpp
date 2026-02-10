/*
 * mod-auto-gather
 *
 * Auto-shows mining/herb nodes on minimap for players with gathering
 * professions, and auto-gathers those nodes when within range —
 * depositing loot directly into bags with normal skill progression.
 */

#include "Chat.h"
#include "Config.h"
#include "DataMap.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static bool     cfgEnable           = true;
static bool     cfgAnnounce         = true;
static bool     cfgAutoTrack        = true;
static bool     cfgAutoLoot         = true;
static float    cfgLootRange        = 10.0f;
static uint32   cfgScanIntervalMs   = 1000;
static bool     cfgAllowInCombat    = false;
static bool     cfgAllowWhileMounted = true;

// Tracking bit values resolved from DBC at startup
static uint32 herbTrackBit = 0;
static uint32 mineTrackBit = 0;

// ---------------------------------------------------------------------------
// Per-player timer state (stored on Player::CustomData)
// ---------------------------------------------------------------------------

struct AutoGatherPlayerData : public DataMap::Base
{
    uint32 scanTimer = 0;
};

// ---------------------------------------------------------------------------
// Gatherable node check functor (for GameObjectListSearcher)
// ---------------------------------------------------------------------------

class GatherableNodeInRange
{
public:
    GatherableNodeInRange(Player const* player, float range)
        : _player(player), _range(range) {}

    bool operator()(GameObject* go)
    {
        if (!go || !go->isSpawned() || go->getLootState() != GO_READY)
            return false;

        if (!_player->IsWithinDist(go, _range, false))
            return false;

        // Only chest-type game objects (herbs, ore veins, etc.)
        if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST)
            return false;

        uint32 lockId = go->GetGOInfo()->GetLockId();
        if (!lockId)
            return false;

        LockEntry const* lockEntry = sLockStore.LookupEntry(lockId);
        if (!lockEntry)
            return false;

        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lockEntry->Type[i] != LOCK_KEY_SKILL)
                continue;

            uint32 lockType = lockEntry->Index[i];
            if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
                continue;

            SkillType skillId = SkillByLockType(LockType(lockType));
            if (skillId == SKILL_NONE)
                continue;

            // Player must have the skill
            if (_player->GetSkillValue(skillId) == 0)
                continue;

            // Player skill must meet required level
            if (_player->GetSkillValue(skillId) < lockEntry->Skill[i])
                continue;

            return true;
        }

        return false;
    }

private:
    Player const* _player;
    float _range;
};

// ---------------------------------------------------------------------------
// Core: resolve lock info for a game object
// ---------------------------------------------------------------------------

static bool GetGatherInfo(GameObject* go, Player* player,
                          SkillType& outSkillId, uint32& outReqSkill)
{
    uint32 lockId = go->GetGOInfo()->GetLockId();
    if (!lockId)
        return false;

    LockEntry const* lockEntry = sLockStore.LookupEntry(lockId);
    if (!lockEntry)
        return false;

    for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
    {
        if (lockEntry->Type[i] != LOCK_KEY_SKILL)
            continue;

        uint32 lockType = lockEntry->Index[i];
        if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
            continue;

        SkillType skillId = SkillByLockType(LockType(lockType));
        if (skillId == SKILL_NONE)
            continue;

        if (player->GetSkillValue(skillId) == 0)
            continue;

        if (player->GetSkillValue(skillId) < lockEntry->Skill[i])
            continue;

        outSkillId = skillId;
        outReqSkill = lockEntry->Skill[i];
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Core: auto-gather a single node
// ---------------------------------------------------------------------------

static bool AutoGatherNode(Player* player, GameObject* go)
{
    // Re-validate state (could have changed since search)
    if (!go->isSpawned() || go->getLootState() != GO_READY)
        return false;

    if (!go->IsInMap(player) || !go->InSamePhase(player))
        return false;

    SkillType skillId = SKILL_NONE;
    uint32 reqSkill = 0;
    if (!GetGatherInfo(go, player, skillId, reqSkill))
        return false;

    // Generate loot
    uint32 lootId = go->GetGOInfo()->GetLootId();
    if (!lootId)
    {
        // No loot template — just despawn
        go->SetLootState(GO_JUST_DEACTIVATED);
        return true;
    }

    Loot loot;
    loot.FillLoot(lootId, LootTemplates_Gameobject, player, true);

    // Pre-check bag space for ALL items before committing
    for (auto const& li : loot.items)
    {
        if (li.is_looted)
            continue;

        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT,
                                                      dest, li.itemid, li.count);
        if (msg != EQUIP_ERR_OK)
        {
            // Can't fit loot — leave node intact
            return false;
        }
    }

    for (auto const& qi : loot.quest_items)
    {
        if (qi.is_looted)
            continue;

        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT,
                                                      dest, qi.itemid, qi.count);
        if (msg != EQUIP_ERR_OK)
            return false;
    }

    // Store regular items
    for (auto const& li : loot.items)
    {
        if (li.is_looted)
            continue;

        ItemPosCountVec dest;
        player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, li.itemid, li.count);

        Item* item = player->StoreNewItem(dest, li.itemid, true, li.randomPropertyId);
        if (item)
            player->SendNewItem(item, li.count, false, false, true);
    }

    // Store quest items
    for (auto const& qi : loot.quest_items)
    {
        if (qi.is_looted)
            continue;

        ItemPosCountVec dest;
        player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, qi.itemid, qi.count);

        Item* item = player->StoreNewItem(dest, qi.itemid, true, qi.randomPropertyId);
        if (item)
            player->SendNewItem(item, qi.count, false, false, true);
    }

    // Give gold if any
    if (loot.gold > 0)
    {
        player->ModifyMoney(loot.gold);
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, loot.gold);
    }

    // Skill-up (mirrors SpellEffects.cpp EffectOpenLock logic)
    if (skillId != SKILL_NONE)
    {
        uint32 pureSkillValue = player->GetPureSkillValue(skillId);
        if (pureSkillValue)
        {
            if (!go->IsInSkillupList(player->GetGUID()))
            {
                go->AddToSkillupList(player->GetGUID());
                player->UpdateGatherSkill(skillId, pureSkillValue, reqSkill);
            }
        }
    }

    // Despawn node (triggers normal respawn cycle)
    go->SetLootState(GO_JUST_DEACTIVATED);

    return true;
}

// ---------------------------------------------------------------------------
// Resolve tracking bit values from spell DBC data
// ---------------------------------------------------------------------------

static void ResolveTrackingBits()
{
    // Find Herbs = spell 2383
    SpellInfo const* herbSpell = sSpellMgr->GetSpellInfo(2383);
    if (herbSpell)
        herbTrackBit = uint32(1) << (herbSpell->GetEffect(EFFECT_0).MiscValue - 1);

    // Find Minerals = spell 2580
    SpellInfo const* mineSpell = sSpellMgr->GetSpellInfo(2580);
    if (mineSpell)
        mineTrackBit = uint32(1) << (mineSpell->GetEffect(EFFECT_0).MiscValue - 1);

    LOG_INFO("module", "mod-auto-gather: Herb tracking bit = 0x{:08X}, Mine tracking bit = 0x{:08X}",
             herbTrackBit, mineTrackBit);
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

class AutoGatherWorldScript : public WorldScript
{
public:
    AutoGatherWorldScript() : WorldScript("AutoGatherWorldScript",
        {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP}) {}

    void OnAfterConfigLoad(bool reload) override
    {
        cfgEnable           = sConfigMgr->GetOption<bool>("AutoGather.Enable", true);
        cfgAnnounce         = sConfigMgr->GetOption<bool>("AutoGather.Announce", true);
        cfgAutoTrack        = sConfigMgr->GetOption<bool>("AutoGather.AutoTrack", true);
        cfgAutoLoot         = sConfigMgr->GetOption<bool>("AutoGather.AutoLoot", true);
        cfgLootRange        = sConfigMgr->GetOption<float>("AutoGather.LootRange", 10.0f);
        cfgScanIntervalMs   = sConfigMgr->GetOption<uint32>("AutoGather.ScanIntervalMs", 1000);
        cfgAllowInCombat    = sConfigMgr->GetOption<bool>("AutoGather.AllowInCombat", false);
        cfgAllowWhileMounted = sConfigMgr->GetOption<bool>("AutoGather.AllowWhileMounted", true);

        // On reload, spell data is available so we can re-resolve tracking bits
        if (reload)
            ResolveTrackingBits();
    }

    // OnStartup fires after all DBC/spell data is loaded (Main.cpp),
    // unlike OnAfterConfigLoad which fires before DBC loading (World.cpp:299 vs 412).
    void OnStartup() override
    {
        ResolveTrackingBits();
    }
};

class AutoGatherPlayerScript : public PlayerScript
{
public:
    AutoGatherPlayerScript() : PlayerScript("AutoGatherPlayerScript",
        {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE}) {}

    void OnPlayerLogin(Player* player) override
    {
        if (!cfgEnable)
            return;

        if (cfgAnnounce)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff00[Auto-Gather]|r Module active. Gathering nodes will be auto-collected.");
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (!cfgEnable)
            return;

        auto* data = player->CustomData.GetDefault<AutoGatherPlayerData>("AutoGather");

        if (data->scanTimer < p_time)
        {
            data->scanTimer = cfgScanIntervalMs;
            DoScan(player);
        }
        else
            data->scanTimer -= p_time;
    }

private:
    static bool PlayerHasGatheringSkill(Player* player, SkillType skill)
    {
        return player->GetSkillValue(skill) > 0;
    }

    static void DoScan(Player* player)
    {
        if (!player->IsAlive())
            return;

        if (!cfgAllowInCombat && player->IsInCombat())
            return;

        if (player->IsFlying() || player->IsInFlight())
            return;

        if (player->GetVehicle())
            return;

        if (player->IsNonMeleeSpellCast(false))
            return;

        if (player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
            return;

        bool hasHerbalism = PlayerHasGatheringSkill(player, SKILL_HERBALISM);
        bool hasMining    = PlayerHasGatheringSkill(player, SKILL_MINING);

        // --- Minimap tracking ---
        if (cfgAutoTrack)
        {
            if (hasHerbalism && herbTrackBit &&
                !player->HasFlag(PLAYER_TRACK_RESOURCES, herbTrackBit))
                player->SetFlag(PLAYER_TRACK_RESOURCES, herbTrackBit);
            else if (!hasHerbalism && herbTrackBit &&
                     player->HasFlag(PLAYER_TRACK_RESOURCES, herbTrackBit))
                player->RemoveFlag(PLAYER_TRACK_RESOURCES, herbTrackBit);

            if (hasMining && mineTrackBit &&
                !player->HasFlag(PLAYER_TRACK_RESOURCES, mineTrackBit))
                player->SetFlag(PLAYER_TRACK_RESOURCES, mineTrackBit);
            else if (!hasMining && mineTrackBit &&
                     player->HasFlag(PLAYER_TRACK_RESOURCES, mineTrackBit))
                player->RemoveFlag(PLAYER_TRACK_RESOURCES, mineTrackBit);
        }

        if (!hasHerbalism && !hasMining)
            return;

        // --- Auto-loot ---
        if (!cfgAutoLoot)
            return;

        if (!cfgAllowWhileMounted && player->IsMounted())
            return;

        std::list<GameObject*> nodes;
        GatherableNodeInRange check(player, cfgLootRange);
        Acore::GameObjectListSearcher<GatherableNodeInRange> searcher(player, nodes, check);
        Cell::VisitObjects(player, searcher, cfgLootRange);

        for (GameObject* go : nodes)
        {
            if (AutoGatherNode(player, go))
                break; // One node per scan cycle
        }
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void AddSC_auto_gather()
{
    new AutoGatherWorldScript();
    new AutoGatherPlayerScript();
}
