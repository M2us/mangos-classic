/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
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
 */

#include "Chat/Channel.h"
#include "Globals/ObjectMgr.h"
#include "World/World.h"
#include "Social/SocialMgr.h"
#include "Chat/Chat.h"

Channel::Channel(const std::string& name)
    : m_announce(true), m_moderate(false), m_name(name), m_flags(0), m_static(false), m_realmzone(false)
{
    // set special flags if built-in channel
    ChatChannelsEntry const* ch = GetChannelEntryFor(name);
    if (ch)                                                 // it's built-in channel
    {
        m_channelId = ch->ChannelID;                        // only built-in channel have channel id != 0
        m_announce = false;                                 // no join/leave announces

        m_flags |= CHANNEL_FLAG_GENERAL;                    // for all built-in channels

        if (ch->flags & CHANNEL_DBC_FLAG_TRADE)             // for trade channel
            m_flags |= CHANNEL_FLAG_TRADE;

        if (ch->flags & CHANNEL_DBC_FLAG_CITY_ONLY2)        // for city only channels
            m_flags |= CHANNEL_FLAG_CITY;

        if (ch->flags & CHANNEL_DBC_FLAG_LFG)               // for LFG channel
            m_flags |= CHANNEL_FLAG_LFG;
        else                                                // for all other channels
            m_flags |= CHANNEL_FLAG_NOT_LFG;

        m_realmzone = true;
    }
    else                                                    // it's custom channel
    {
        m_flags |= CHANNEL_FLAG_CUSTOM;
        m_realmzone = sObjectMgr.CheckPublicMessageLanguage(m_name);
    }
}

void Channel::Join(Player* player, const char* password)
{
    ObjectGuid guid = player->GetObjectGuid();

    WorldPacket data;
    if (IsOn(guid))
    {
        if (!IsConstant())                                  // non send error message for built-in channels
        {
            MakePlayerAlreadyMember(data, m_name, guid);
            SendToOne(data, guid);
        }
        return;
    }

    if (IsBanned(guid))
    {
        MakeBanned(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (m_password.length() > 0 && strcmp(password, m_password.c_str()))
    {
        MakeWrongPassword(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (player->GetGuildId() && (GetFlags() == 0x38))
        return;

    // join channel
    player->JoinedChannel(this);

    if (m_announce && (player->GetSession()->GetSecurity() < SEC_GAMEMASTER || !sWorld.getConfig(CONFIG_BOOL_CHANNEL_GM_JOIN_SILENTLY)))
    {
        MakeJoined(data, m_name, guid);
        SendToAll(data);
    }

    data.clear();

    PlayerInfo& pinfo = m_players[guid];
    pinfo.player = guid;
    pinfo.flags = MEMBER_FLAG_NONE;

    MakeYouJoined(data, m_name, *this);
    SendToOne(data, guid);

    // if no owner first logged will become
    if (!IsPublic() && !m_ownerGuid)
        SetOwner(guid, (m_players.size() > 1));

    // try to auto-convert this channel to static upon reaching treshold
    SetStatic(true);
}

void Channel::Leave(Player* player, bool send)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        if (send)
        {
            WorldPacket data;
            MakeNotMember(data, m_name);
            SendToOne(data, guid);
        }
        return;
    }

    // leave channel
    if (send)
    {
        WorldPacket data;
        MakeYouLeft(data, m_name, *this);
        SendToOne(data, guid);
        player->LeftChannel(this);
        data.clear();
    }

    bool changeowner = m_players[guid].IsOwner();

    m_players.erase(guid);
    if (m_announce && (player->GetSession()->GetSecurity() < SEC_GAMEMASTER || !sWorld.getConfig(CONFIG_BOOL_CHANNEL_GM_JOIN_SILENTLY)))
    {
        WorldPacket data;
        MakeLeft(data, m_name, guid);
        SendToAll(data);
    }

    if (changeowner && !IsPublic())
        SetOwner(SelectNewOwner(), (m_players.size() > 1));
}

void Channel::KickOrBan(Player* player, const char* targetName, bool ban)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (!m_players[guid].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(data, m_name);
        SendToOne(data, guid);
        return;
    }

    Player* target = sObjectMgr.GetPlayer(targetName);
    if (!target)
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    ObjectGuid targetGuid = target->GetObjectGuid();
    if (!IsOn(targetGuid))
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    bool changeowner = m_ownerGuid == targetGuid;

    if (player->GetSession()->GetSecurity() < SEC_GAMEMASTER && changeowner && guid != m_ownerGuid)
    {
        WorldPacket data;
        MakeNotOwner(data, m_name);
        SendToOne(data, guid);
        return;
    }

    // kick or ban player
    WorldPacket data;

    if (ban && !IsBanned(targetGuid))
    {
        m_banned.insert(targetGuid);
        MakePlayerBanned(data, m_name, targetGuid, guid);
    }
    else
        MakePlayerKicked(data, m_name, targetGuid, guid);

    SendToAll(data);
    m_players.erase(targetGuid);
    target->LeftChannel(this);

    if (changeowner && !IsPublic())
        SetOwner(SelectNewOwner(), (m_players.size() > 1));
}

void Channel::UnBan(Player* player, const char* targetName)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (!m_players[guid].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(data, m_name);
        SendToOne(data, guid);
        return;
    }

    Player* target = sObjectMgr.GetPlayer(targetName);
    if (!target)
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    ObjectGuid targetGuid = target->GetObjectGuid();
    if (!IsBanned(targetGuid))
    {
        WorldPacket data;
        MakePlayerNotBanned(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    // unban player
    m_banned.erase(targetGuid);

    WorldPacket data;
    MakePlayerUnbanned(data, m_name, targetGuid, guid);
    SendToAll(data);
}

void Channel::Password(Player* player, const char* password)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (!m_players[guid].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(data, m_name);
        SendToOne(data, guid);
        return;
    }

    // set channel password
    m_password = password;

    WorldPacket data;
    MakePasswordChanged(data, m_name, guid);
    SendToAll(data);
}

void Channel::SetMode(Player* player, const char* targetName, bool moderator, bool set)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (!m_players[guid].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(data, m_name);
        SendToOne(data, guid);
        return;
    }

    Player* target = sObjectMgr.GetPlayer(targetName);
    if (!target)
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    ObjectGuid targetGuid = target->GetObjectGuid();
    if (moderator && guid == m_ownerGuid && targetGuid == m_ownerGuid)
        return;

    if (!IsOn(targetGuid))
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    // allow make moderator from another team only if both is GMs
    // at this moment this only way to show channel post for GM from another team
    if ((player->GetSession()->GetSecurity() < SEC_GAMEMASTER || target->GetSession()->GetSecurity() < SEC_GAMEMASTER) &&
            player->GetTeam() != target->GetTeam() && !sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_CHANNEL))
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    if (m_ownerGuid == targetGuid && m_ownerGuid != guid)
    {
        WorldPacket data;
        MakeNotOwner(data, m_name);
        SendToOne(data, guid);
        return;
    }

    // set channel moderator
    if (moderator)
        SetModerator(targetGuid, set);
    else
        SetMute(targetGuid, set);
}

void Channel::SetOwner(Player* player, const char* targetName)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (player->GetSession()->GetSecurity() < SEC_GAMEMASTER && guid != m_ownerGuid)
    {
        WorldPacket data;
        MakeNotOwner(data, m_name);
        SendToOne(data, guid);
        return;
    }

    Player* target = sObjectMgr.GetPlayer(targetName);
    if (!target)
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    ObjectGuid targetGuid = target->GetObjectGuid();
    if (!IsOn(targetGuid))
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    if (target->GetTeam() != player->GetTeam() && !sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_CHANNEL))
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    // set channel owner
    SetOwner(targetGuid, (m_players.size() > 1));
}

void Channel::SendWhoOwner(Player* player) const
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    ObjectGuid ownerGuid = m_ownerGuid;

    if (const bool visibilityCheck = (player->GetSession()->GetSecurity() == SEC_PLAYER))
    {
        // PLAYER can't see MODERATOR, GAME MASTER, ADMINISTRATOR characters
        // MODERATOR, GAME MASTER, ADMINISTRATOR can see all
        const AccountTypes visibilityThreshold = AccountTypes(sWorld.getConfig(CONFIG_UINT32_GM_LEVEL_IN_WHO_LIST));

        if (Player* owner = sObjectMgr.GetPlayer(ownerGuid))
        {
            if (owner->GetSession()->GetSecurity() > visibilityThreshold || !owner->IsVisibleGloballyFor(player))
                ownerGuid = ObjectGuid();
        }
    }

    // send channel owner
    WorldPacket data;
    MakeChannelOwner(data, m_name, guid);
    SendToOne(data, guid);
}

void Channel::List(Player* player, bool/* display = false*/)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    // list players in channel
    WorldPacket data(SMSG_CHANNEL_LIST, 1 + (GetName().size() + 1) + 1 + 4 + m_players.size() * (8 + 1));
    data << GetName();                                      // channel name
    data << uint8(GetFlags());                              // channel flags?

    size_t countpos = data.wpos();
    data << uint32(0);                                      // size of list, placeholder

    // PLAYER can't see MODERATOR, GAME MASTER, ADMINISTRATOR characters
    // MODERATOR, GAME MASTER, ADMINISTRATOR can see all
    const bool visibilityCheck = (player->GetSession()->GetSecurity() == SEC_PLAYER);
    AccountTypes visibilityThreshold = AccountTypes(sWorld.getConfig(CONFIG_UINT32_GM_LEVEL_IN_WHO_LIST));

    uint32 count = 0;
    for (PlayerList::const_iterator i = m_players.begin(); i != m_players.end(); ++i)
    {
        if (Player* member = sObjectMgr.GetPlayer(i->first))
        {
            if (visibilityCheck && (member->GetSession()->GetSecurity() > visibilityThreshold || !member->IsVisibleGloballyFor(player)))
                continue;

            data << ObjectGuid(i->first);
            data << uint8(i->second.flags);                 // flags seems to be changed...
            ++count;
        }
    }

    data.put<uint32>(countpos, count);

    SendToOne(data, guid);
}

void Channel::Announce(Player* player)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (!m_players[guid].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(data, m_name);
        SendToOne(data, guid);
        return;
    }

    // toggle channel announcement
    m_announce = !m_announce;

    WorldPacket data;
    if (m_announce)
        MakeAnnouncementsOn(data, m_name, guid);
    else
        MakeAnnouncementsOff(data, m_name, guid);

    SendToAll(data);
}

void Channel::Moderate(Player* player)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (!m_players[guid].IsModerator() && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(data, m_name);
        SendToOne(data, guid);
        return;
    }

    // toggle channel moderation
    m_moderate = !m_moderate;

    WorldPacket data;
    if (m_moderate)
        MakeModerationOn(data, m_name, guid);
    else
        MakeModerationOff(data, m_name, guid);

    SendToAll(data);
}

void Channel::Say(Player* player, const char* text, uint32 lang)
{
    if (!text)
        return;

    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (m_players[guid].IsMuted() ||
            (GetChannelId() == CHANNEL_ID_LOCAL_DEFENSE && player->GetHonorRankInfo().visualRank < SPEAK_IN_LOCALDEFENSE_RANK) ||
            (GetChannelId() == CHANNEL_ID_WORLD_DEFENSE && player->GetHonorRankInfo().visualRank < SPEAK_IN_WORLDDEFENSE_RANK))
    {
        WorldPacket data;
        MakeMuted(data, m_name);
        SendToOne(data, guid);
        return;
    }

    const bool moderator = m_players[guid].IsModerator();

    if (m_moderate && !moderator && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        WorldPacket data;
        MakeNotModerator(data, m_name);
        SendToOne(data, guid);
        return;
    }

    if (uint32 restriction = sWorld.getConfig(CONFIG_UINT32_CHANNEL_RESTRICTED_LANGUAGE_MODE))
    {
        bool restricted = false;

        switch (restriction)
        {
            case 1: restricted = IsConstant();                  break;
            case 2: restricted = (IsPublic() && m_realmzone);   break;
            case 3: restricted = true;                          break;
        }

        if (restricted && !sObjectMgr.CheckPublicMessageLanguage(text))
        {
            WorldPacket data;
            MakeMuted(data, m_name);
            SendToOne(data, guid);
            return;
        }
    }

    // send channel message
    if (sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_CHANNEL))
        lang = LANG_UNIVERSAL;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, text, Language(lang), player->GetChatTag(), guid, player->GetName(), ObjectGuid(), "", m_name.c_str(), player->GetHonorRankInfo().rank);
    SendMessage(data, (moderator ? ObjectGuid() : guid));
}

void Channel::Invite(Player* player, const char* targetName)
{
    ObjectGuid guid = player->GetObjectGuid();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(data, m_name);
        SendToOne(data, guid);
        return;
    }

    Player* target = sObjectMgr.GetPlayer(targetName);
    if (!target)
    {
        WorldPacket data;
        MakePlayerNotFound(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    ObjectGuid targetGuid = target->GetObjectGuid();
    if (IsOn(targetGuid))
    {
        WorldPacket data;
        MakePlayerAlreadyMember(data, m_name, targetGuid);
        SendToOne(data, guid);
        return;
    }

    if (IsBanned(targetGuid))
    {
        WorldPacket data;
        MakePlayerInviteBanned(data, m_name, targetName);
        SendToOne(data, guid);
        return;
    }

    if (target->GetTeam() != player->GetTeam() && !sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_CHANNEL))
    {
        WorldPacket data;
        MakeInviteWrongFaction(data, m_name);
        SendToOne(data, guid);
        return;
    }

    // invite player
    WorldPacket data;
    if (!target->GetSocial()->HasIgnore(guid))
    {
        MakeInvite(data, m_name, guid);
        SendToOne(data, targetGuid);
        data.clear();
    }

    MakePlayerInvited(data, m_name, targetName);
    SendToOne(data, guid);
}

void Channel::SendToOne(WorldPacket const& data, ObjectGuid receiver) const
{
    if (Player* player = sObjectMgr.GetPlayer(receiver))
        player->GetSession()->SendPacket(data);
}

void Channel::SendToAll(WorldPacket const& data) const
{
    for (PlayerList::const_iterator i = m_players.begin(); i != m_players.end(); ++i)
        SendToOne(data, i->first);
}

void Channel::SendMessage(WorldPacket const& data, ObjectGuid sender) const
{
    for (PlayerList::const_iterator i = m_players.begin(); i != m_players.end(); ++i)
        if (Player* plr = sObjectMgr.GetPlayer(i->first))
            if (!sender || !plr->GetSocial()->HasIgnore(sender))
                plr->GetSession()->SendPacket(data);
}

void Channel::MakeNotifyPacket(WorldPacket& data, const std::string& channel, ChatNotify type)
{
    data.Initialize(SMSG_CHANNEL_NOTIFY, 1 + channel.size() + 1);
    data << uint8(type);
    data << channel;
}

void Channel::MakeJoined(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_JOINED_NOTICE);
    data << guid;
}

void Channel::MakeLeft(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_LEFT_NOTICE);
    data << guid;
}

void Channel::MakeYouJoined(WorldPacket& data, const std::string& channel, const Channel& which)
{
    MakeNotifyPacket(data, channel, CHAT_YOU_JOINED_NOTICE);
    data << uint8(which.GetFlags());
    data << uint32(which.GetChannelId());
    data << uint32(0);                                      // channel index (when split occurs due to player count)
}

void Channel::MakeYouLeft(WorldPacket& data, const std::string& channel, const Channel&/* which*/)
{
    MakeNotifyPacket(data, channel, CHAT_YOU_LEFT_NOTICE);
}

void Channel::MakeWrongPassword(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_WRONG_PASSWORD_NOTICE);
}

void Channel::MakeNotMember(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_NOT_MEMBER_NOTICE);
}

void Channel::MakeNotModerator(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_NOT_MODERATOR_NOTICE);
}

void Channel::MakePasswordChanged(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_PASSWORD_CHANGED_NOTICE);
    data << guid;
}

void Channel::MakeOwnerChanged(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_OWNER_CHANGED_NOTICE);
    data << guid;
}

void Channel::MakePlayerNotFound(WorldPacket& data, const std::string& channel, const std::string& name)
{
    MakeNotifyPacket(data, channel, CHAT_PLAYER_NOT_FOUND_NOTICE);
    data << name;
}

void Channel::MakeNotOwner(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_NOT_OWNER_NOTICE);
}

void Channel::MakeChannelOwner(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    std::string name;

    if (!guid)
        name = "Nobody";
    else if (!sObjectMgr.GetPlayerNameByGUID(guid, name) || name.empty())
        name = "PLAYER_NOT_FOUND";

    MakeNotifyPacket(data, channel, CHAT_CHANNEL_OWNER_NOTICE);
    data << name;
}

void Channel::MakeModeChange(WorldPacket& data, const std::string& channel, const ObjectGuid& guid, uint8 oldflags, uint8 newFlags)
{
    MakeNotifyPacket(data, channel, CHAT_MODE_CHANGE_NOTICE);
    data << guid;
    data << uint8(oldflags);
    data << uint8(newFlags);
}

void Channel::MakeAnnouncementsOn(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_ANNOUNCEMENTS_ON_NOTICE);
    data << guid;
}

void Channel::MakeAnnouncementsOff(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_ANNOUNCEMENTS_OFF_NOTICE);
    data << guid;
}

void Channel::MakeModerationOn(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_MODERATION_ON_NOTICE);
    data << guid;
}

void Channel::MakeModerationOff(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_MODERATION_OFF_NOTICE);
    data << guid;
}

void Channel::MakeMuted(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_MUTED_NOTICE);
}

void Channel::MakePlayerKicked(WorldPacket& data, const std::string& channel, const ObjectGuid& target, const ObjectGuid& source)
{
    MakeNotifyPacket(data, channel, CHAT_PLAYER_KICKED_NOTICE);
    data << target;
    data << source;
}

void Channel::MakeBanned(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_BANNED_NOTICE);
}

void Channel::MakePlayerBanned(WorldPacket& data, const std::string& channel, const ObjectGuid& target, const ObjectGuid& source)
{
    MakeNotifyPacket(data, channel, CHAT_PLAYER_BANNED_NOTICE);
    data << target;
    data << source;
}

void Channel::MakePlayerUnbanned(WorldPacket& data, const std::string& channel, const ObjectGuid& target, const ObjectGuid& source)
{
    MakeNotifyPacket(data, channel, CHAT_PLAYER_UNBANNED_NOTICE);
    data << target;
    data << source;
}

void Channel::MakePlayerNotBanned(WorldPacket& data, const std::string& channel, const std::string& name)
{
    MakeNotifyPacket(data, channel, CHAT_PLAYER_NOT_BANNED_NOTICE);
    data << name;
}

void Channel::MakePlayerAlreadyMember(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_PLAYER_ALREADY_MEMBER_NOTICE);
    data << guid;
}

void Channel::MakeInvite(WorldPacket& data, const std::string& channel, const ObjectGuid& guid)
{
    MakeNotifyPacket(data, channel, CHAT_INVITE_NOTICE);
    data << guid;
}

void Channel::MakeInviteWrongFaction(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_INVITE_WRONG_FACTION_NOTICE);
}

void Channel::MakeWrongFaction(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_WRONG_FACTION_NOTICE);
}

void Channel::MakeInvalidName(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_INVALID_NAME_NOTICE);
}

void Channel::MakeNotModerated(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_NOT_MODERATED_NOTICE);
}

void Channel::MakePlayerInvited(WorldPacket& data, const std::string& channel, const std::string& name)
{
    MakeNotifyPacket(data, channel, CHAT_PLAYER_INVITED_NOTICE);
    data << name;
}

void Channel::MakePlayerInviteBanned(WorldPacket& data, const std::string& channel, const std::string& name)
{
    MakeNotifyPacket(data, channel, CHAT_PLAYER_INVITE_BANNED_NOTICE);
    data << name;
}

void Channel::MakeThrottled(WorldPacket& data, const std::string& channel)
{
    MakeNotifyPacket(data, channel, CHAT_THROTTLED_NOTICE);
}

ObjectGuid Channel::SelectNewOwner() const
{
    // Prioritise moderators for owner appointment
    for (auto itr = m_players.begin(); itr != m_players.end(); ++itr)
    {
        if ((*itr).second.IsModerator())
            return (*itr).second.player;
    }

    return (m_players.empty() ? ObjectGuid() : m_players.begin()->second.player);
}

void Channel::SetOwner(ObjectGuid guid, bool exclaim)
{
    if (m_ownerGuid)
    {
        // [] will re-add player after it possible removed
        PlayerList::iterator p_itr = m_players.find(m_ownerGuid);
        if (p_itr != m_players.end())
        {
            // old owner retains own moderator powers on transfer to another player only
            p_itr->second.SetModerator(bool(guid));

            uint8 oldFlag = p_itr->second.flags;
            p_itr->second.SetOwner(false);

            WorldPacket data;
            MakeModeChange(data, m_name, guid, oldFlag, GetPlayerFlags(guid));
            SendToAll(data);
        }
    }

    m_ownerGuid = guid;

    if (m_ownerGuid)
    {
        // new owner receives moderator powers as well
        m_players[m_ownerGuid].SetModerator(true);

        uint8 oldFlag = GetPlayerFlags(m_ownerGuid);
        m_players[m_ownerGuid].SetOwner(true);

        WorldPacket data;
        MakeModeChange(data, m_name, guid, oldFlag, GetPlayerFlags(guid));
        SendToAll(data);
    }

    if (exclaim)
    {
        WorldPacket data;
        MakeOwnerChanged(data, m_name, guid);
        SendToAll(data);
    }
}

bool Channel::SetStatic(bool state, bool command/* = false*/)
{
    // Only custom channels can be set to static
    if (IsConstant() || !HasFlag(CHANNEL_FLAG_CUSTOM) || m_static == state)
        return false;

    // This conversion cannot be performed if password is set - it has to be removed first
    if (state && !m_password.empty())
        return false;

    // Treshold for auto-conversion
    const uint32 treshold = sWorld.getConfig(CONFIG_UINT32_CHANNEL_STATIC_AUTO_TRESHOLD);

    // Detect auto-converion and reaching treshold
    if (!command && (!treshold || state != (GetNumPlayers() >= treshold)))
        return false;

    // Strip moderator privileges
    if (state)
    {
        for (PlayerList::const_iterator i = m_players.begin(); i != m_players.end(); ++i)
        {
            if (i->second.IsModerator())
                SetModerator(i->second.player, false);
        }
    }

    // Unset/Set channel owner
    if (state == bool(m_ownerGuid))
        SetOwner(state ? ObjectGuid() : SelectNewOwner());

    // Disable premoderation mode on conversion to static
    if (state && m_moderate)
    {
        m_moderate = false;

        WorldPacket data;
        MakeModerationOff(data, m_name, ObjectGuid());
        SendToAll(data);
    }

    // Disable announcements on conversion to static
    if (state && m_announce)
    {
        m_announce = false;

        WorldPacket data;
        MakeAnnouncementsOff(data, m_name, ObjectGuid());
        SendToAll(data);
    }

    m_static = state;

    return true;
}
