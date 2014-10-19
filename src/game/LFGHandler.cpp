/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2014  MaNGOS project <http://getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "WorldSession.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Player.h"
#include "WorldPacket.h"
#include "ObjectMgr.h"
#include "World.h"

void WorldSession::HandleLfgJoinOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_LFG_JOIN");

    uint8 dungeonsCount, counter2;
    uint32 roles;
    
    std::string comment;
    std::set<uint32> dungeons;

    recv_data >> roles;                                     // lfg roles
    recv_data >> Unused<uint8>();                           // lua: GetLFGInfoLocal
    recv_data >> Unused<uint8>();                           // lua: GetLFGInfoLocal

    recv_data >> dungeonsCount;

    for (uint8 i = 0; i < dungeonsCount; ++i)
    {
        uint32 dungeonEntry;
        recv_data >> dungeonEntry;
        dungeons.insert((dungeonEntry & 0x00FFFFFF));        // just dungeon id
    }

    recv_data >> counter2;                                  // const count = 3, lua: GetLFGInfoLocal

    for (uint8 i = 0; i < counter2; ++i)
        recv_data >> Unused<uint8>();                       // lua: GetLFGInfoLocal

    recv_data >> comment;                                   // lfg comment

    sLFGMgr.JoinLFG(roles, dungeons, comment, GetPlayer()); // Attempt to join lfg system
    // SendLfgJoinResult(ERR_LFG_OK);
    // SendLfgUpdate(false, LFG_UPDATE_JOIN, dungeons[0]);
}

void WorldSession::HandleLfgLeaveOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("CMSG_LFG_LEAVE");
    
    Player* pPlayer = GetPlayer();
    
    ObjectGuid guid = pPlayer->GetObjectGuid();
    Group* pGroup = pPlayer->GetGroup();
    
    // If it's just one player they can leave, otherwise just the group leader
    //if (!pGroup || pGroup->IsLeader(guid))
    //    sLFGMgr.LeaveLFG();

    // SendLfgUpdate(false, LFG_UPDATE_LEAVE, 0);
}

void WorldSession::HandleSearchLfgJoinOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_LFG_SEARCH_JOIN");

    uint32 temp, entry;
    recv_data >> temp;

    entry = (temp & 0x00FFFFFF);
    // LfgType type = LfgType((temp >> 24) & 0x000000FF);

    // SendLfgSearchResults(type, entry);
}

void WorldSession::HandleSearchLfgLeaveOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_LFG_SEARCH_LEAVE");

    recv_data >> Unused<uint32>();                          // join id?
}

void WorldSession::HandleSetLfgCommentOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_SET_LFG_COMMENT");

    std::string comment;
    recv_data >> comment;
    DEBUG_LOG("LFG comment \"%s\"", comment.c_str());
}

void WorldSession::HandleLfgGetPlayerInfo(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_LFG_GET_PLAYER_INFO");
    
    Player* pPlayer = GetPlayer();
    uint32 level = pPlayer->getLevel();
    uint8 expansion = pPlayer->GetSession()->Expansion();
    
    dungeonEntries availableDungeons = sLFGMgr.FindRandomDungeonsForPlayer(level, expansion);
    dungeonForbidden lockedDungeons = sLFGMgr.FindRandomDungeonsNotForPlayer(pPlayer);
    
    uint32 availableSize = uint32(availableDungeons.size());
    uint32 forbiddenSize = uint32(lockedDungeons.size());
    
    DEBUG_LOG("Sending SMSG_LFG_PLAYER_INFO...");
    WorldPacket data(SMSG_LFG_PLAYER_INFO, 1+(availableSize*34)+4+(forbiddenSize*30));
    
    data << uint8(availableDungeons.size());                        // amount of available dungeons
    for (dungeonEntries::iterator it = availableDungeons.begin(); it != availableDungeons.end(); ++it)
    {
        DEBUG_LOG("Parsing a dungeon entry for packet");

        data << uint32(it->second);                                 // dungeon entry
        
        DungeonTypes type = sLFGMgr.GetDungeonType(it->first);      // dungeon type
        const DungeonFinderRewards* rewards = sObjectMgr.GetDungeonFinderRewards(level); // get xp and money rewards
        ItemRewards itemRewards = sLFGMgr.GetDungeonItemRewards(it->first, type);        // item rewards
        
        int32 multiplier;
        bool hasDoneToday = sLFGMgr.HasPlayerDoneDaily(pPlayer->GetGUIDLow(), type); // using variable to send later in packet
        (hasDoneToday) ? multiplier = 1 : multiplier = 2;                            // 2x base reward if first dungeon of the day
            
        data << uint8(hasDoneToday);
        data << uint32(multiplier*rewards->baseMonetaryReward);     // cash/gold reward
        data << uint32(((uint32)multiplier)*rewards->baseXPReward); // experience reward
        data << uint32(0); // null
        data << uint32(0); // null
        if (!hasDoneToday)
        {
            ItemPrototype const* pProto = ObjectMgr::GetItemPrototype(itemRewards.itemId);
            if (pProto)
            {
                data << uint8(1);                                // 1 item rewarded per dungeon
                data << uint32(itemRewards.itemId);              // entry of item
                data << uint32(pProto->DisplayInfoID);           // display id of item
                data << uint32(itemRewards.itemAmount);          // amount of item reward
            }
            else
                data << uint8(0);                                // couldn't find the item reward
        }
        else if (hasDoneToday && (type == DUNGEON_WOTLK_HEROIC)) // special case
        {
            if (ItemPrototype const* pProto = ObjectMgr::GetItemPrototype(WOTLK_SPECIAL_HEROIC_ITEM))
            {
                data << uint8(1);
                data << uint32(WOTLK_SPECIAL_HEROIC_ITEM);
                data << uint32(pProto->DisplayInfoID);
                data << uint32(WOTLK_SPECIAL_HEROIC_AMNT);
            }
            else
                data << uint8(0);
        }
        else
            data << uint8(0);
    }
    
    data << uint32(lockedDungeons.size());
    for (dungeonForbidden::iterator it = lockedDungeons.begin(); it != lockedDungeons.end(); ++it)
    {
        data << uint32(it->first);  // dungeon entry
        data << uint32(it->second); // reason for being locked
    }
    SendPacket(&data);
}

void WorldSession::HandleLfgGetPartyInfo(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_LFG_GET_PARTY_INFO");
    
    Player* pPlayer = GetPlayer();
    uint64 guid = pPlayer->GetObjectGuid().GetRawValue(); // send later in packet
    
    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
        return;
    
    partyForbidden groupMap;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* pGroupPlayer = itr->getSource();
        if (!pGroupPlayer)
            continue;
        
        uint64 pPlayerGuid = pGroupPlayer->GetObjectGuid().GetRawValue();
        if (pPlayerGuid != guid)
            groupMap[pPlayerGuid] = sLFGMgr.FindRandomDungeonsNotForPlayer(pGroupPlayer);
    }
    
    uint32 packetSize = 0;
    for (partyForbidden::iterator it = groupMap.begin(); it != groupMap.end(); ++it)
        packetSize += 12 + uint32(it->second.size()) * 8;
    
    DEBUG_LOG("Sending SMSG_LFG_PARTY_INFO...");
    WorldPacket data(SMSG_LFG_PARTY_INFO, packetSize);
    
    data << uint8(groupMap.size());
    for (partyForbidden::iterator it = groupMap.begin(); it != groupMap.end(); ++it)
    {
        dungeonForbidden dungeonInfo = it->second;
        
        data << uint64(it->first); // object guid of player
        data << uint32(dungeonInfo.size()); // amount of their locked dungeons
        
        for (dungeonForbidden::iterator itr = dungeonInfo.begin(); itr != dungeonInfo.end(); ++itr)
        {
            data << uint32(itr->first); // dungeon entry
            data << uint32(itr->second); // reason for dungeon being forbidden/locked
        }
    }
    SendPacket(&data);
}

void WorldSession::HandleLfgGetStatus(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_LFG_GET_STATUS");
}

void WorldSession::HandleLfgSetRoles(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_LFG_SET_ROLES");
    
    Player* pPlayer = GetPlayer();
    
    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)                                           // role check/set is only done in groups
        return;
    
    uint8 roles;
    recv_data >> roles;
    
    sLFGMgr.PerformRoleCheck(pPlayer, pGroup, roles);      // handle the rules/logic in lfgmgr
}

void WorldSession::SendLfgSearchResults(LfgType type, uint32 entry)
{
    WorldPacket data(SMSG_LFG_SEARCH_RESULTS);
    data << uint32(type);                                   // type
    data << uint32(entry);                                  // entry from LFGDungeons.dbc

    uint8 isGuidsPresent = 0;
    data << uint8(isGuidsPresent);
    if (isGuidsPresent)
    {
        uint32 guids_count = 0;
        data << uint32(guids_count);
        for (uint32 i = 0; i < guids_count; ++i)
        {
            data << uint64(0);                              // player/group guid
        }
    }

    uint32 groups_count = 1;
    data << uint32(groups_count);                           // groups count
    data << uint32(groups_count);                           // groups count (total?)

    for (uint32 i = 0; i < groups_count; ++i)
    {
        data << uint64(1);                                  // group guid

        uint32 flags = 0x92;
        data << uint32(flags);                              // flags

        if (flags & 0x2)
        {
            data << uint8(0);                               // comment string, max len 256
        }

        if (flags & 0x10)
        {
            for (uint32 j = 0; j < 3; ++j)
                data << uint8(0);                           // roles
        }

        if (flags & 0x80)
        {
            data << uint64(0);                              // instance guid
            data << uint32(0);                              // completed encounters
        }
    }

    // TODO: Guard Player map
    HashMapHolder<Player>::MapType const& players = sObjectAccessor.GetPlayers();
    uint32 playersSize = players.size();
    data << uint32(playersSize);                            // players count
    data << uint32(playersSize);                            // players count (total?)

    for (HashMapHolder<Player>::MapType::const_iterator iter = players.begin(); iter != players.end(); ++iter)
    {
        Player* plr = iter->second;

        if (!plr || plr->GetTeam() != _player->GetTeam())
            continue;

        if (!plr->IsInWorld())
            continue;

        data << plr->GetObjectGuid();                       // guid

        uint32 flags = 0xFF;
        data << uint32(flags);                              // flags

        if (flags & 0x1)
        {
            data << uint8(plr->getLevel());
            data << uint8(plr->getClass());
            data << uint8(plr->getRace());

            for (uint32 i = 0; i < 3; ++i)
                data << uint8(0);                           // talent spec x/x/x

            data << uint32(0);                              // armor
            data << uint32(0);                              // spd/heal
            data << uint32(0);                              // spd/heal
            data << uint32(0);                              // HasteMelee
            data << uint32(0);                              // HasteRanged
            data << uint32(0);                              // HasteSpell
            data << float(0);                               // MP5
            data << float(0);                               // MP5 Combat
            data << uint32(0);                              // AttackPower
            data << uint32(0);                              // Agility
            data << uint32(0);                              // Health
            data << uint32(0);                              // Mana
            data << uint32(0);                              // Unk1
            data << float(0);                               // Unk2
            data << uint32(0);                              // Defence
            data << uint32(0);                              // Dodge
            data << uint32(0);                              // Block
            data << uint32(0);                              // Parry
            data << uint32(0);                              // Crit
            data << uint32(0);                              // Expertise
        }

        if (flags & 0x2)
            data << "";                                     // comment

        if (flags & 0x4)
            data << uint8(0);                               // group leader

        if (flags & 0x8)
            data << uint64(1);                              // group guid

        if (flags & 0x10)
            data << uint8(0);                               // roles

        if (flags & 0x20)
            data << uint32(plr->GetZoneId());               // areaid

        if (flags & 0x40)
            data << uint8(0);                               // status

        if (flags & 0x80)
        {
            data << uint64(0);                              // instance guid
            data << uint32(0);                              // completed encounters
        }
    }

    SendPacket(&data);
}

void WorldSession::SendLfgJoinResult(LfgJoinResult result, LFGState state, partyForbidden const& lockedDungeons)
{
    uint32 packetSize = 0;
    for (partyForbidden::const_iterator it = lockedDungeons.begin(); it != lockedDungeons.end(); ++it)
        packetSize += 12 + uint32(it->second.size()) * 8;
    
    WorldPacket data(SMSG_LFG_JOIN_RESULT, packetSize);
    data << uint32(result);
    data << uint32(state);
    
    if (!lockedDungeons.empty())
    {
        for (partyForbidden::const_iterator it = lockedDungeons.begin(); it != lockedDungeons.end(); ++it)
        {
            dungeonForbidden dungeonInfo = it->second;
        
            data << uint64(it->first); // object guid of player
            data << uint32(dungeonInfo.size()); // amount of their locked dungeons
        
            for (dungeonForbidden::iterator itr = dungeonInfo.begin(); itr != dungeonInfo.end(); ++itr)
            {
                data << uint32(itr->first); // dungeon entry
                data << uint32(itr->second); // reason for dungeon being forbidden/locked
            }
        }
    }

    SendPacket(&data);
}

void WorldSession::SendLfgUpdate(bool isGroup, LFGPlayerStatus status)
{
    uint8 dungeonSize = uint8(status.dungeonList.size());
    
    bool isQueued, joinLFG;
    if (!isGroup)
        switch (status.updateType)
        {
            case LFG_UPDATE_JOIN:
            case LFG_UPDATE_ADDED_TO_QUEUE:
                isQueued = true;
                break;
            case LFG_UPDATE_STATUS:
                isQueued = (status.state == LFG_STATE_QUEUED);
                break;
            default:
                isQueued = false;
                break;
        }
    else
        switch (status.updateType)
        {
            case LFG_UPDATE_ADDED_TO_QUEUE:
                isQueued = true;
            case LFG_UPDATE_PROPOSAL_BEGIN:
                joinLFG = true;
                break;
            case LFG_UPDATE_STATUS:
                isQueued = (status.state == LFG_STATE_QUEUED);
                joinLFG = status.state != LFG_STATE_ROLECHECK && status.state != LFG_STATE_NONE;
                break;
        }
    
    WorldPacket data(isGroup ? SMSG_LFG_UPDATE_PARTY : SMSG_LFG_UPDATE_PLAYER, 0);
    data << uint8(status.updateType);

    data << uint8(dungeonSize > 0);

    if (extra)
    {
        if (isGroup)
            data << uint8(joinLFG);
        data << uint8(isQueued);
        data << uint8(0);
        data << uint8(0);

        if (isGroup)
        {
            data << uint8(0);
            for (uint32 i = 0; i < 3; ++i)
                data << uint8(0);
        }

        data << uint8(dungeonSize);
        for (std::set<uint32>::iterator it = status.dungeonList.begin(); it != status.dungeonList.end(); ++it)
            data << uint32(*it);
            
        data << status.comment;
    }
    SendPacket(&data);
}

void WorldSession::SendLfgQueueStatus(LFGQueueStatus const& status)
{
    WorldPacket data(SMSG_LFG_QUEUE_STATUS);
    
    data << uint32(status.dungeonID);
    data << int32(status.playerAvgWaitTime);
    data << int32(status.avgWaitTime);
    data << int32(status.tankAvgWaitTime);
    data << int32(status.healerAvgWaitTime);
    data << int32(status.dpsAvgWaitTime);
    data << uint8(status.neededTanks);
    data << uint8(status.neededHeals);
    data << uint8(status.neededDps);
    data << uint32(status.timeSpentInQueue);
    
    SendPacket(&data);
}
