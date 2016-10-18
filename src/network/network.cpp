#pragma region Copyright (c) 2014-2016 OpenRCT2 Developers
/*****************************************************************************
 * OpenRCT2, an open source clone of Roller Coaster Tycoon 2.
 *
 * OpenRCT2 is the work of many authors, a full list can be found in contributors.md
 * For more information, visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * A full copy of the GNU General Public License can be found in licence.txt
 *****************************************************************************/
#pragma endregion

#include <SDL_platform.h>

#ifdef __WINDOWS__
	// winsock2 must be included before windows.h
	#include <winsock2.h>
#else
	#include <arpa/inet.h>
#endif

extern "C" {
#include "../openrct2.h"
#include "../platform/platform.h"
}

#include "network.h"

extern "C" {
}

rct_peep* _pickup_peep = 0;
int _pickup_peep_old_x = SPRITE_LOCATION_NULL;

#ifndef DISABLE_NETWORK

#include <cmath>
#include <cerrno>
#include <algorithm>
#include <set>
#include <string>

#include "../core/Console.hpp"
#include "../core/Json.hpp"
#include "../core/Math.hpp"
#include "../core/Path.hpp"
#include "../core/String.hpp"
#include "../core/Util.hpp"

extern "C" {
#include "../config.h"
#include "../game.h"
#include "../interface/chat.h"
#include "../interface/window.h"
#include "../interface/keyboard_shortcut.h"
#include "../localisation/date.h"
#include "../localisation/localisation.h"
#include "../management/finance.h"
#include "../network/http.h"
#include "../scenario.h"
#include "../windows/error.h"
#include "../util/util.h"
#include "../cheats.h"

#include "NetworkAction.h"

#include <openssl/evp.h> // just for OpenSSL_add_all_algorithms()
}

#pragma comment(lib, "Ws2_32.lib")

Network gNetwork;

enum {
	MASTER_SERVER_STATUS_OK = 200,
	MASTER_SERVER_STATUS_INVALID_TOKEN = 401,
	MASTER_SERVER_STATUS_SERVER_NOT_FOUND = 404,
	MASTER_SERVER_STATUS_INTERNAL_ERROR = 500
};

enum {
	SERVER_EVENT_PLAYER_JOINED,
	SERVER_EVENT_PLAYER_DISCONNECTED,
};

void network_chat_show_connected_message();
void network_chat_show_server_greeting();
static void network_get_keys_directory(utf8 *buffer, size_t bufferSize);
static void network_get_private_key_path(utf8 *buffer, size_t bufferSize, const utf8 * playerName);
static void network_get_public_key_path(utf8 *buffer, size_t bufferSize, const utf8 * playerName, const utf8 * hash);
static void network_get_keymap_path(utf8 *buffer, size_t bufferSize);

Network::Network()
{
	wsa_initialized = false;
	mode = NETWORK_MODE_NONE;
	status = NETWORK_STATUS_NONE;
	last_tick_sent_time = 0;
	last_ping_sent_time = 0;
	client_command_handlers.resize(NETWORK_COMMAND_MAX, 0);
	client_command_handlers[NETWORK_COMMAND_AUTH] = &Network::Client_Handle_AUTH;
	client_command_handlers[NETWORK_COMMAND_MAP] = &Network::Client_Handle_MAP;
	client_command_handlers[NETWORK_COMMAND_CHAT] = &Network::Client_Handle_CHAT;
	client_command_handlers[NETWORK_COMMAND_GAMECMD] = &Network::Client_Handle_GAMECMD;
	client_command_handlers[NETWORK_COMMAND_TICK] = &Network::Client_Handle_TICK;
	client_command_handlers[NETWORK_COMMAND_PLAYERLIST] = &Network::Client_Handle_PLAYERLIST;
	client_command_handlers[NETWORK_COMMAND_PING] = &Network::Client_Handle_PING;
	client_command_handlers[NETWORK_COMMAND_PINGLIST] = &Network::Client_Handle_PINGLIST;
	client_command_handlers[NETWORK_COMMAND_SETDISCONNECTMSG] = &Network::Client_Handle_SETDISCONNECTMSG;
	client_command_handlers[NETWORK_COMMAND_SHOWERROR] = &Network::Client_Handle_SHOWERROR;
	client_command_handlers[NETWORK_COMMAND_GROUPLIST] = &Network::Client_Handle_GROUPLIST;
	client_command_handlers[NETWORK_COMMAND_EVENT] = &Network::Client_Handle_EVENT;
	client_command_handlers[NETWORK_COMMAND_GAMEINFO] = &Network::Client_Handle_GAMEINFO;
	client_command_handlers[NETWORK_COMMAND_TOKEN] = &Network::Client_Handle_TOKEN;
	server_command_handlers.resize(NETWORK_COMMAND_MAX, 0);
	server_command_handlers[NETWORK_COMMAND_AUTH] = &Network::Server_Handle_AUTH;
	server_command_handlers[NETWORK_COMMAND_CHAT] = &Network::Server_Handle_CHAT;
	server_command_handlers[NETWORK_COMMAND_GAMECMD] = &Network::Server_Handle_GAMECMD;
	server_command_handlers[NETWORK_COMMAND_PING] = &Network::Server_Handle_PING;
	server_command_handlers[NETWORK_COMMAND_GAMEINFO] = &Network::Server_Handle_GAMEINFO;
	server_command_handlers[NETWORK_COMMAND_TOKEN] = &Network::Server_Handle_TOKEN;
	OpenSSL_add_all_algorithms();
}

Network::~Network()
{
	Close();
}

bool Network::Init()
{
#ifdef __WINDOWS__
	if (!wsa_initialized) {
		log_verbose("Initialising WSA");
		WSADATA wsa_data;
		if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
			log_error("Unable to initialise winsock.");
			return false;
		}
		wsa_initialized = true;
	}
#endif

	status = NETWORK_STATUS_READY;

	ServerName = std::string();
	ServerDescription = std::string();
	ServerGreeting = std::string();
	ServerProviderName = std::string();
	ServerProviderEmail = std::string();
	ServerProviderWebsite = std::string();
	return true;
}

void Network::Close()
{
	if (status == NETWORK_STATUS_NONE)
	{
		// Already closed. This prevents a call in ~Network() to gfx_invalidate_screen()
		// which may no longer be valid on Linux and would cause a segfault.
		return;
	}
	if (mode == NETWORK_MODE_CLIENT) {
		delete server_connection.Socket;
		server_connection.Socket = nullptr;
	} else if (mode == NETWORK_MODE_SERVER) {
		delete listening_socket;
		listening_socket = nullptr;
		delete _advertiser;
		_advertiser = nullptr;
	}

	mode = NETWORK_MODE_NONE;
	status = NETWORK_STATUS_NONE;
	_lastConnectStatus = SOCKET_STATUS_CLOSED;
	server_connection.AuthStatus = NETWORK_AUTH_NONE;
	server_connection.InboundPacket.Clear();
	server_connection.SetLastDisconnectReason(nullptr);

	client_connection_list.clear();
	game_command_queue.clear();
	player_list.clear();
	group_list.clear();

#ifdef __WINDOWS__
	if (wsa_initialized) {
		WSACleanup();
		wsa_initialized = false;
	}
#endif

	CloseChatLog();
	gfx_invalidate_screen();
}

bool Network::BeginClient(const char* host, unsigned short port)
{
	if (GetMode() != NETWORK_MODE_NONE) {
		return false;
	}

	Close();
	if (!Init())
		return false;

	mode = NETWORK_MODE_CLIENT;

	assert(server_connection.Socket == nullptr);
	server_connection.Socket = CreateTcpSocket();
	server_connection.Socket->ConnectAsync(host, port);
	status = NETWORK_STATUS_CONNECTING;
	_lastConnectStatus = SOCKET_STATUS_CLOSED;

	BeginChatLog();

	utf8 keyPath[MAX_PATH];
	network_get_private_key_path(keyPath, sizeof(keyPath), gConfigNetwork.player_name);
	if (!platform_file_exists(keyPath)) {
		Console::WriteLine("Generating key... This may take a while");
		Console::WriteLine("Need to collect enough entropy from the system");
		key.Generate();
		Console::WriteLine("Key generated, saving private bits as %s", keyPath);

		utf8 keysDirectory[MAX_PATH];
		network_get_keys_directory(keysDirectory, sizeof(keysDirectory));
		if (!platform_ensure_directory_exists(keysDirectory)) {
			log_error("Unable to create directory %s.", keysDirectory);
			return false;
		}

		SDL_RWops *privkey = SDL_RWFromFile(keyPath, "wb+");
		if (privkey == nullptr) {
			log_error("Unable to save private key at %s.", keyPath);
			return false;
		}
		key.SavePrivate(privkey);
		SDL_RWclose(privkey);

		const std::string hash = key.PublicKeyHash();
		const utf8 *publicKeyHash = hash.c_str();
		network_get_public_key_path(keyPath, sizeof(keyPath), gConfigNetwork.player_name, publicKeyHash);
		Console::WriteLine("Key generated, saving public bits as %s", keyPath);
		SDL_RWops *pubkey = SDL_RWFromFile(keyPath, "wb+");
		if (pubkey == nullptr) {
			log_error("Unable to save public key at %s.", keyPath);
			return false;
		}
		key.SavePublic(pubkey);
		SDL_RWclose(pubkey);
	} else {
		log_verbose("Loading key from %s", keyPath);
		SDL_RWops *privkey = SDL_RWFromFile(keyPath, "rb");
		if (privkey == nullptr) {
			log_error("Unable to read private key from %s.", keyPath);
			return false;
		}

		// LoadPrivate returns validity of loaded key
		bool ok = key.LoadPrivate(privkey);
		SDL_RWclose(privkey);
		// Don't store private key in memory when it's not in use.
		key.Unload();
		return ok;
	}

	return true;
}

bool Network::BeginServer(unsigned short port, const char* address)
{
	Close();
	if (!Init())
		return false;

	mode = NETWORK_MODE_SERVER;

	_userManager.Load();

	log_verbose("Begin listening for clients");

	assert(listening_socket == nullptr);
	listening_socket = CreateTcpSocket();
	try
	{
		listening_socket->Listen(address, port);
	}
	catch (Exception ex)
	{
		Console::Error::WriteLine(ex.GetMessage());
		Close();
		return false;
	}

	ServerName = String::ToStd(gConfigNetwork.server_name);
	ServerDescription = String::ToStd(gConfigNetwork.server_description);
	ServerGreeting = String::ToStd(gConfigNetwork.server_greeting);
	ServerProviderName = String::ToStd(gConfigNetwork.provider_name);
	ServerProviderEmail = String::ToStd(gConfigNetwork.provider_email);
	ServerProviderWebsite = String::ToStd(gConfigNetwork.provider_website);

	cheats_reset();
	LoadGroups();
	BeginChatLog();

	NetworkPlayer *player = AddPlayer(gConfigNetwork.player_name, "");
	player->flags |= NETWORK_PLAYER_FLAG_ISSERVER;
	player->group = 0;
	player_id = player->id;

	printf("Ready for clients...\n");
	network_chat_show_connected_message();
	network_chat_show_server_greeting();

	status = NETWORK_STATUS_CONNECTED;
	listening_port = port;
	if (gConfigNetwork.advertise) {
		_advertiser = CreateServerAdvertiser(listening_port);
	}

	return true;
}

int Network::GetMode()
{
	return mode;
}

int Network::GetStatus()
{
	return status;
}

int Network::GetAuthStatus()
{
	if (GetMode() == NETWORK_MODE_CLIENT) {
		return server_connection.AuthStatus;
	} else
	if (GetMode() == NETWORK_MODE_SERVER) {
		return NETWORK_AUTH_OK;
	}
	return NETWORK_AUTH_NONE;
}

uint32 Network::GetServerTick()
{
	return server_tick;
}

uint8 Network::GetPlayerID()
{
	return player_id;
}

void Network::Update()
{
	switch (GetMode()) {
	case NETWORK_MODE_SERVER:
		UpdateServer();
		break;
	case NETWORK_MODE_CLIENT:
		UpdateClient();
		break;
	}
}

void Network::UpdateServer()
{
	auto it = client_connection_list.begin();
	while (it != client_connection_list.end()) {
		if (!ProcessConnection(*(*it))) {
			RemoveClient((*it));
			it = client_connection_list.begin();
		} else {
			it++;
		}
	}
	if (SDL_TICKS_PASSED(SDL_GetTicks(), last_tick_sent_time + 25)) {
		Server_Send_TICK();
	}
	if (SDL_TICKS_PASSED(SDL_GetTicks(), last_ping_sent_time + 3000)) {
		Server_Send_PING();
		Server_Send_PINGLIST();
	}

	if (_advertiser != nullptr) {
		_advertiser->Update();
	}

	ITcpSocket * tcpSocket = listening_socket->Accept();
	if (tcpSocket != nullptr) {
		AddClient(tcpSocket);
	}
}

void Network::UpdateClient()
{
	switch(status){
	case NETWORK_STATUS_CONNECTING:
	{
		switch (server_connection.Socket->GetStatus()) {
		case SOCKET_STATUS_RESOLVING:
		{
			if (_lastConnectStatus != SOCKET_STATUS_RESOLVING)
			{
				_lastConnectStatus = SOCKET_STATUS_RESOLVING;
				char str_resolving[256];
				format_string(str_resolving, 256, STR_MULTIPLAYER_RESOLVING, NULL);
				window_network_status_open(str_resolving, []() -> void {
					gNetwork.Close();
				});
			}
			break;
		}
		case SOCKET_STATUS_CONNECTING:
		{
			if (_lastConnectStatus != SOCKET_STATUS_CONNECTING)
			{
				_lastConnectStatus = SOCKET_STATUS_CONNECTING;
				char str_connecting[256];
				format_string(str_connecting, 256, STR_MULTIPLAYER_CONNECTING, NULL);
				window_network_status_open(str_connecting, []() -> void {
					gNetwork.Close();
				});
				server_connect_time = SDL_GetTicks();
			}
			break;
		}
		case NETWORK_STATUS_CONNECTED:
		{
			status = NETWORK_STATUS_CONNECTED;
			server_connection.ResetLastPacketTime();
			Client_Send_TOKEN();
			char str_authenticating[256];
			format_string(str_authenticating, 256, STR_MULTIPLAYER_AUTHENTICATING, NULL);
			window_network_status_open(str_authenticating, []() -> void {
				gNetwork.Close();
			});
			break;
		}
		default:
		{
			const char * error = server_connection.Socket->GetError();
			if (error != nullptr) {
				Console::Error::WriteLine(error);
			}

			Close();
			window_network_status_close();
			window_error_open(STR_UNABLE_TO_CONNECT_TO_SERVER, STR_NONE);
			break;
		}
		}
		break;
	}
	case NETWORK_STATUS_CONNECTED:
	{
		if (!ProcessConnection(server_connection)) {
			// Do not show disconnect message window when password window closed/canceled
			if (server_connection.AuthStatus == NETWORK_AUTH_REQUIREPASSWORD) {
				window_network_status_close();
			} else {
				char str_disconnected[256];

				if (server_connection.GetLastDisconnectReason()) {
					const char * disconnect_reason = server_connection.GetLastDisconnectReason();
					format_string(str_disconnected, 256, STR_MULTIPLAYER_DISCONNECTED_WITH_REASON, &disconnect_reason);
				} else {
					format_string(str_disconnected, 256, STR_MULTIPLAYER_DISCONNECTED_NO_REASON, NULL);
				}

				window_network_status_open(str_disconnected, NULL);
			}
			Close();
		}
		ProcessGameCommandQueue();

		// Check synchronisation
		if (!_desynchronised && !CheckSRAND(gCurrentTicks, gScenarioSrand0)) {
			_desynchronised = true;
			char str_desync[256];
			format_string(str_desync, 256, STR_MULTIPLAYER_DESYNC, NULL);
			window_network_status_open(str_desync, NULL);
			if (!gConfigNetwork.stay_connected) {
				Close();
			}
		}
		break;
	}
	}
}

std::vector<std::unique_ptr<NetworkPlayer>>::iterator Network::GetPlayerIteratorByID(uint8 id)
{
	auto it = std::find_if(player_list.begin(), player_list.end(), [&id](std::unique_ptr<NetworkPlayer> const& player) { return player->id == id; });
	if (it != player_list.end()) {
		return it;
	}
	return player_list.end();
}

NetworkPlayer* Network::GetPlayerByID(uint8 id)
{
	auto it = GetPlayerIteratorByID(id);
	if (it != player_list.end()) {
		return it->get();
	}
	return nullptr;
}

std::vector<std::unique_ptr<NetworkGroup>>::iterator Network::GetGroupIteratorByID(uint8 id)
{
	auto it = std::find_if(group_list.begin(), group_list.end(), [&id](std::unique_ptr<NetworkGroup> const& group) { return group->Id == id; });
	if (it != group_list.end()) {
		return it;
	}
	return group_list.end();
}

NetworkGroup* Network::GetGroupByID(uint8 id)
{
	auto it = GetGroupIteratorByID(id);
	if (it != group_list.end()) {
		return it->get();
	}
	return nullptr;
}

const char* Network::FormatChat(NetworkPlayer* fromplayer, const char* text)
{
	static char formatted[1024];
	char* lineCh = formatted;
	formatted[0] = 0;
	if (fromplayer) {
		lineCh = utf8_write_codepoint(lineCh, FORMAT_OUTLINE);
		lineCh = utf8_write_codepoint(lineCh, FORMAT_BABYBLUE);
		safe_strcpy(lineCh, (const char *) fromplayer->name.c_str(), sizeof(formatted) - (lineCh - formatted));
		safe_strcat(lineCh, ": ", sizeof(formatted) - (lineCh - formatted));
		lineCh = strchr(lineCh, '\0');
	}
	lineCh = utf8_write_codepoint(lineCh, FORMAT_OUTLINE);
	lineCh = utf8_write_codepoint(lineCh, FORMAT_WHITE);
	char* ptrtext = lineCh;
	safe_strcpy(lineCh, text, 800);
	utf8_remove_format_codes((utf8*)ptrtext, true);
	return formatted;
}

void Network::SendPacketToClients(NetworkPacket& packet, bool front)
{
	for (auto it = client_connection_list.begin(); it != client_connection_list.end(); it++) {
		(*it)->QueuePacket(NetworkPacket::Duplicate(packet), front);
	}
}

bool Network::CheckSRAND(uint32 tick, uint32 srand0)
{
	if (server_srand0_tick == 0)
		return true;

	if (tick > server_srand0_tick) {
		server_srand0_tick = 0;
		return true;
	}

	if (tick == server_srand0_tick) {
		server_srand0_tick = 0;
		// Check that the server and client sprite hashes match
		const bool sprites_mismatch = server_sprite_hash[0] != '\0' && strcmp(sprite_checksum(), server_sprite_hash);
		// Check PRNG values and sprite hashes, if exist
		if ((srand0 != server_srand0) || sprites_mismatch) {
			return false;
		}
	}
	return true;
}

void Network::KickPlayer(int playerId)
{
	for(auto it = client_connection_list.begin(); it != client_connection_list.end(); it++) {
		if ((*it)->Player->id == playerId) {
			// Disconnect the client gracefully
			(*it)->SetLastDisconnectReason(STR_MULTIPLAYER_KICKED);
			char str_disconnect_msg[256];
			format_string(str_disconnect_msg, 256, STR_MULTIPLAYER_KICKED_REASON, NULL);
			Server_Send_SETDISCONNECTMSG(*(*it), str_disconnect_msg);
			(*it)->Socket->Disconnect();
			break;
		}
	}
}

void Network::SetPassword(const char* password)
{
	Network::password = password == nullptr ? "" : password;
}

void Network::ShutdownClient()
{
	if (GetMode() == NETWORK_MODE_CLIENT) {
		server_connection.Socket->Disconnect();
	}
}

std::string Network::GenerateAdvertiseKey()
{
	// Generate a string of 16 random hex characters (64-integer key as a hex formatted string)
	static char hexChars[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
	char key[17];
	for (int i = 0; i < 16; i++) {
		int hexCharIndex = util_rand() % Util::CountOf(hexChars);
		key[i] = hexChars[hexCharIndex];
	}
	key[Util::CountOf(key) - 1] = 0;

	return key;
}

const char *Network::GetMasterServerUrl()
{
	if (str_is_null_or_empty(gConfigNetwork.master_server_url)) {
		return OPENRCT2_MASTER_SERVER_URL;
	} else {
		return gConfigNetwork.master_server_url;
	}
}

NetworkGroup* Network::AddGroup()
{
	NetworkGroup* addedgroup = nullptr;
	int newid = -1;
	// Find first unused group id
	for (int id = 0; id < 255; id++) {
		if (std::find_if(group_list.begin(), group_list.end(), [&id](std::unique_ptr<NetworkGroup> const& group) {
						 return group->Id == id;
			}) == group_list.end()) {
			newid = id;
			break;
		}
	}
	if (newid != -1) {
		std::unique_ptr<NetworkGroup> group(new NetworkGroup); // change to make_unique in c++14
		group->Id = newid;
		group->SetName("Group #" + std::to_string(newid));
		addedgroup = group.get();
		group_list.push_back(std::move(group));
	}
	return addedgroup;
}

void Network::RemoveGroup(uint8 id)
{
	auto group = GetGroupIteratorByID(id);
	if (group != group_list.end()) {
		group_list.erase(group);
	}

	if (GetMode() == NETWORK_MODE_SERVER) {
		_userManager.UnsetUsersOfGroup(id);
		_userManager.Save();
	}
}

uint8 Network::GetGroupIDByHash(const std::string &keyhash)
{
	const NetworkUser * networkUser = _userManager.GetUserByHash(keyhash);

	uint8 groupId = GetDefaultGroup();
	if (networkUser != nullptr && networkUser->GroupId.HasValue()) {
		const uint8 assignedGroup = networkUser->GroupId.GetValue();
		if (GetGroupByID(assignedGroup) != nullptr) {
			groupId = assignedGroup;
		} else {
			log_warning("User %s is assigned to non-existent group %u. Assigning to default group (%u)",
						keyhash.c_str(), assignedGroup, groupId);
		}
	}
	return groupId;
}

uint8 Network::GetDefaultGroup()
{
	return default_group;
}

void Network::SetDefaultGroup(uint8 id)
{
	if (GetGroupByID(id)) {
		default_group = id;
	}
}

void Network::SaveGroups()
{
	if (GetMode() == NETWORK_MODE_SERVER) {
		utf8 path[MAX_PATH];

		platform_get_user_directory(path, NULL, sizeof(path));
		safe_strcat_path(path, "groups.json", sizeof(path));

		json_t * jsonGroupsCfg = json_object();
		json_t * jsonGroups = json_array();
		for (auto it = group_list.begin(); it != group_list.end(); it++) {
			json_array_append_new(jsonGroups, (*it)->ToJson());
		}
		json_object_set_new(jsonGroupsCfg, "default_group", json_integer(default_group));
		json_object_set_new(jsonGroupsCfg, "groups", jsonGroups);
		try
		{
			Json::WriteToFile(path, jsonGroupsCfg, JSON_INDENT(4) | JSON_PRESERVE_ORDER);
		}
		catch (Exception ex)
		{
			log_error("Unable to save %s: %s", path, ex.GetMessage());
		}

		json_decref(jsonGroupsCfg);
	}
}

void Network::SetupDefaultGroups()
{
	std::unique_ptr<NetworkGroup> admin(new NetworkGroup()); // change to make_unique in c++14
	admin->SetName("Admin");
	admin->ActionsAllowed.fill(0xFF);
	admin->Id = 0;
	group_list.push_back(std::move(admin));
	std::unique_ptr<NetworkGroup> spectator(new NetworkGroup()); // change to make_unique in c++14
	spectator->SetName("Spectator");
	spectator->ToggleActionPermission(0); // Chat
	spectator->Id = 1;
	group_list.push_back(std::move(spectator));
	std::unique_ptr<NetworkGroup> user(new NetworkGroup()); // change to make_unique in c++14
	user->SetName("User");
	user->ActionsAllowed.fill(0xFF);
	user->ToggleActionPermission(15); // Kick Player
	user->ToggleActionPermission(16); // Modify Groups
	user->ToggleActionPermission(17); // Set Player Group
	user->ToggleActionPermission(18); // Cheat
	user->Id = 2;
	group_list.push_back(std::move(user));
	SetDefaultGroup(1);
}

void Network::LoadGroups()
{
	group_list.clear();

	utf8 path[MAX_PATH];

	platform_get_user_directory(path, NULL, sizeof(path));
	safe_strcat_path(path, "groups.json", sizeof(path));

	json_t * json = nullptr;
	if (platform_file_exists(path)) {
		try {
			json = Json::ReadFromFile(path);
		} catch (const Exception &e) {
			log_error("Failed to read %s as JSON. Setting default groups. %s", path, e.GetMessage());
		}
	}

	if (json == nullptr) {
		SetupDefaultGroups();
	} else {
		json_t * json_groups = json_object_get(json, "groups");
		size_t groupCount = (size_t)json_array_size(json_groups);
		for (size_t i = 0; i < groupCount; i++) {
			json_t * jsonGroup = json_array_get(json_groups, i);

			std::unique_ptr<NetworkGroup> newgroup(new NetworkGroup(NetworkGroup::FromJson(jsonGroup))); // change to make_unique in c++14
			group_list.push_back(std::move(newgroup));
		}
		json_t * jsonDefaultGroup = json_object_get(json, "default_group");
		default_group = (uint8)json_integer_value(jsonDefaultGroup);
		if (GetGroupByID(default_group) == nullptr) {
			default_group = 0;
		}
		json_decref(json);
	}
}

void Network::BeginChatLog()
{
	utf8 filename[32];
	time_t timer;
	struct tm * tmInfo;
	time(&timer);
	tmInfo = localtime(&timer);
	strftime(filename, sizeof(filename), "%Y%m%d-%H%M%S.txt", tmInfo);

	utf8 path[MAX_PATH];
	platform_get_user_directory(path, "chatlogs", sizeof(path));
	Path::Append(path, sizeof(path), filename);

	_chatLogPath = std::string(path);
}

void Network::AppendChatLog(const utf8 *text)
{
	if (!gConfigNetwork.log_chat) {
		return;
	}

	const utf8 *chatLogPath = _chatLogPath.c_str();

	utf8 directory[MAX_PATH];
	Path::GetDirectory(directory, sizeof(directory), chatLogPath);
	if (platform_ensure_directory_exists(directory)) {
		_chatLogStream = SDL_RWFromFile(chatLogPath, "a");
		if (_chatLogStream != nullptr) {
			utf8 buffer[256];

			time_t timer;
			struct tm * tmInfo;
			time(&timer);
			tmInfo = localtime(&timer);
			strftime(buffer, sizeof(buffer), "[%Y/%m/%d %H:%M:%S] ", tmInfo);

			String::Append(buffer, sizeof(buffer), text);
			utf8_remove_formatting(buffer, false);
			String::Append(buffer, sizeof(buffer), PLATFORM_NEWLINE);

			SDL_RWwrite(_chatLogStream, buffer, strlen(buffer), 1);
			SDL_RWclose(_chatLogStream);
		}
	}
}

void Network::CloseChatLog()
{
}

void Network::Client_Send_TOKEN()
{
	log_verbose("requesting token");
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_TOKEN;
	server_connection.AuthStatus = NETWORK_AUTH_REQUESTED;
	server_connection.QueuePacket(std::move(packet));
}

void Network::Client_Send_AUTH(const char* name, const char* password, const char* pubkey, const char *sig, size_t sigsize)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_AUTH;
	packet->WriteString(NETWORK_STREAM_ID);
	packet->WriteString(name);
	packet->WriteString(password);
	packet->WriteString(pubkey);
	assert(sigsize <= (size_t)UINT32_MAX);
	*packet << (uint32)sigsize;
	packet->Write((const uint8 *)sig, sigsize);
	server_connection.AuthStatus = NETWORK_AUTH_REQUESTED;
	server_connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_TOKEN(NetworkConnection& connection)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_TOKEN << (uint32)connection.Challenge.size();
	packet->Write(connection.Challenge.data(), connection.Challenge.size());
	connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_AUTH(NetworkConnection& connection)
{
	uint8 new_playerid = 0;
	if (connection.Player) {
		new_playerid = connection.Player->id;
	}
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_AUTH << (uint32)connection.AuthStatus << (uint8)new_playerid;
	if (connection.AuthStatus == NETWORK_AUTH_BADVERSION) {
		packet->WriteString(NETWORK_STREAM_ID);
	}
	connection.QueuePacket(std::move(packet));
	if (connection.AuthStatus != NETWORK_AUTH_OK && connection.AuthStatus != NETWORK_AUTH_REQUIREPASSWORD) {
		connection.SendQueuedPackets();
		connection.Socket->Disconnect();
	}
}

void Network::Server_Send_MAP(NetworkConnection* connection)
{
	bool RLEState = gUseRLE;
	gUseRLE = false;
	FILE* temp = tmpfile();
	if (!temp) {
		log_warning("Failed to create temporary file to save map.");
		return;
	}
	SDL_RWops* rw = SDL_RWFromFP(temp, SDL_TRUE);
	scenario_save_network(rw);
	gUseRLE = RLEState;
	int size = (int)SDL_RWtell(rw);
	std::vector<uint8> buffer(size);
	SDL_RWseek(rw, 0, RW_SEEK_SET);
	if (SDL_RWread(rw, &buffer[0], size, 1) == 0) {
		log_warning("Failed to read temporary map file into memory.");
		SDL_RWclose(rw);
		return;
	}
	size_t chunksize = 65000;
	size_t out_size = size;
	unsigned char *compressed = util_zlib_deflate(&buffer[0], size, &out_size);
	unsigned char *header;
	if (compressed != NULL)
	{
		header = (unsigned char *)_strdup("open2_sv6_zlib");
		size_t header_len = strlen((char *)header) + 1; // account for null terminator
		header = (unsigned char *)realloc(header, header_len + out_size);
		if (header == nullptr) {
			log_error("Failed to allocate %u bytes.", header_len + out_size);
			connection->SetLastDisconnectReason(STR_MULTIPLAYER_CONNECTION_CLOSED);
			connection->Socket->Disconnect();
			free(compressed);
			return;
		}
		memcpy(&header[header_len], compressed, out_size);
		out_size += header_len;
		free(compressed);
		log_verbose("Sending map of size %u bytes, compressed to %u bytes", size, out_size);
	} else {
		log_warning("Failed to compress the data, falling back to non-compressed sv6.");
		header = (unsigned char *)malloc(size);
		if (header == nullptr) {
			log_error("Failed to allocate %u bytes.", size);
			connection->SetLastDisconnectReason(STR_MULTIPLAYER_CONNECTION_CLOSED);
			connection->Socket->Disconnect();
			return;
		}
		out_size = size;
		memcpy(header, &buffer[0], size);
	}
	for (size_t i = 0; i < out_size; i += chunksize) {
		size_t datasize = Math::Min(chunksize, out_size - i);
		std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
		*packet << (uint32)NETWORK_COMMAND_MAP << (uint32)out_size << (uint32)i;
		packet->Write(&header[i], datasize);
		if (connection) {
			connection->QueuePacket(std::move(packet));
		} else {
			SendPacketToClients(*packet);
		}
	}
	free(header);
	SDL_RWclose(rw);
}

void Network::Client_Send_CHAT(const char* text)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_CHAT;
	packet->WriteString(text);
	server_connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_CHAT(const char* text)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_CHAT;
	packet->WriteString(text);
	SendPacketToClients(*packet);
}

void Network::Client_Send_GAMECMD(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 callback)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_GAMECMD << (uint32)gCurrentTicks << eax << (ebx | GAME_COMMAND_FLAG_NETWORKED)
			<< ecx << edx << esi << edi << ebp << callback;
	server_connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_GAMECMD(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 playerid, uint8 callback)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_GAMECMD << (uint32)gCurrentTicks << eax << (ebx | GAME_COMMAND_FLAG_NETWORKED)
			<< ecx << edx << esi << edi << ebp << playerid << callback;
	SendPacketToClients(*packet);
}

void Network::Server_Send_TICK()
{
	last_tick_sent_time = SDL_GetTicks();
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_TICK << (uint32)gCurrentTicks << (uint32)gScenarioSrand0;
	uint32 flags = 0;
	// Simple counter which limits how often a sprite checksum gets sent.
	// This can get somewhat expensive, so we don't want to push it every tick in release,
	// but debug version can check more often.
#ifdef DEBUG
	static int checksum_counter = 0;
	checksum_counter++;
	if (checksum_counter >= 100) {
		checksum_counter = 0;
		flags |= NETWORK_TICK_FLAG_CHECKSUMS;
	}
#endif
	// Send flags always, so we can understand packet structure on the other end,
	// and allow for some expansion.
	*packet << flags;
	if (flags & NETWORK_TICK_FLAG_CHECKSUMS) {
		packet->WriteString(sprite_checksum());
	}
	SendPacketToClients(*packet);
}

void Network::Server_Send_PLAYERLIST()
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_PLAYERLIST << (uint8)player_list.size();
	for (unsigned int i = 0; i < player_list.size(); i++) {
		player_list[i]->Write(*packet);
	}
	SendPacketToClients(*packet);
}

void Network::Client_Send_PING()
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_PING;
	server_connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_PING()
{
	last_ping_sent_time = SDL_GetTicks();
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_PING;
	for (auto it = client_connection_list.begin(); it != client_connection_list.end(); it++) {
		(*it)->PingTime = SDL_GetTicks();
	}
	SendPacketToClients(*packet, true);
}

void Network::Server_Send_PINGLIST()
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_PINGLIST << (uint8)player_list.size();
	for (unsigned int i = 0; i < player_list.size(); i++) {
		*packet << player_list[i]->id << player_list[i]->ping;
	}
	SendPacketToClients(*packet);
}

void Network::Server_Send_SETDISCONNECTMSG(NetworkConnection& connection, const char* msg)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_SETDISCONNECTMSG;
	packet->WriteString(msg);
	connection.QueuePacket(std::move(packet));
	connection.SendQueuedPackets();
}

void Network::Server_Send_GAMEINFO(NetworkConnection& connection)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_GAMEINFO;
#ifndef DISABLE_HTTP
	json_t* obj = json_object();
	json_object_set_new(obj, "name", json_string(gConfigNetwork.server_name));
	json_object_set_new(obj, "requiresPassword", json_boolean(password.size() > 0));
	json_object_set_new(obj, "version", json_string(NETWORK_STREAM_ID));
	json_object_set_new(obj, "players", json_integer(player_list.size()));
	json_object_set_new(obj, "maxPlayers", json_integer(gConfigNetwork.maxplayers));
	json_object_set_new(obj, "description", json_string(gConfigNetwork.server_description));
	json_object_set_new(obj, "greeting", json_string(gConfigNetwork.server_greeting));
	json_object_set_new(obj, "dedicated", json_boolean(gOpenRCT2Headless));

	// Provider details
	json_t* jsonProvider = json_object();
	json_object_set_new(jsonProvider, "name", json_string(gConfigNetwork.provider_name));
	json_object_set_new(jsonProvider, "email", json_string(gConfigNetwork.provider_email));
	json_object_set_new(jsonProvider, "website", json_string(gConfigNetwork.provider_website));
	json_object_set_new(obj, "provider", jsonProvider);

	packet->WriteString(json_dumps(obj, 0));
	json_decref(obj);
#endif
	connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_SHOWERROR(NetworkConnection& connection, rct_string_id title, rct_string_id message)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_SHOWERROR << title << message;
	connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_GROUPLIST(NetworkConnection& connection)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_GROUPLIST << (uint8)group_list.size() << default_group;
	for (unsigned int i = 0; i < group_list.size(); i++) {
		group_list[i]->Write(*packet);
	}
	connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_EVENT_PLAYER_JOINED(const char *playerName)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_EVENT;
	*packet << (uint16)SERVER_EVENT_PLAYER_JOINED;
	packet->WriteString(playerName);
	SendPacketToClients(*packet);
}

void Network::Server_Send_EVENT_PLAYER_DISCONNECTED(const char *playerName, const char *reason)
{
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_EVENT;
	*packet << (uint16)SERVER_EVENT_PLAYER_DISCONNECTED;
	packet->WriteString(playerName);
	packet->WriteString(reason);
	SendPacketToClients(*packet);
}

bool Network::ProcessConnection(NetworkConnection& connection)
{
	int packetStatus;
	do {
		packetStatus = connection.ReadPacket();
		switch(packetStatus) {
		case NETWORK_READPACKET_DISCONNECTED:
			// closed connection or network error
			if (!connection.GetLastDisconnectReason()) {
				connection.SetLastDisconnectReason(STR_MULTIPLAYER_CONNECTION_CLOSED);
			}
			return false;
			break;
		case NETWORK_READPACKET_SUCCESS:
			// done reading in packet
			ProcessPacket(connection, connection.InboundPacket);
			if (connection.Socket == nullptr) {
				return false;
			}
			break;
		case NETWORK_READPACKET_MORE_DATA:
			// more data required to be read
			break;
		case NETWORK_READPACKET_NO_DATA:
			// could not read anything from socket
			break;
		}
	} while (packetStatus == NETWORK_READPACKET_MORE_DATA || packetStatus == NETWORK_READPACKET_SUCCESS);
	connection.SendQueuedPackets();
	if (!connection.ReceivedPacketRecently()) {
		if (!connection.GetLastDisconnectReason()) {
			connection.SetLastDisconnectReason(STR_MULTIPLAYER_NO_DATA);
		}
		return false;
	}
	return true;
}

void Network::ProcessPacket(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 command;
	packet >> command;
	if (command < NETWORK_COMMAND_MAX) {
		switch (gNetwork.GetMode()) {
		case NETWORK_MODE_SERVER:
			if (server_command_handlers[command]) {
				if (connection.AuthStatus == NETWORK_AUTH_OK || !packet.CommandRequiresAuth()) {
					(this->*server_command_handlers[command])(connection, packet);
				}
			}
			break;
		case NETWORK_MODE_CLIENT:
			if (client_command_handlers[command]) {
				(this->*client_command_handlers[command])(connection, packet);
			}
			break;
		}
	}
	packet.Clear();
}

void Network::ProcessGameCommandQueue()
{
	while (game_command_queue.begin() != game_command_queue.end() && game_command_queue.begin()->tick == gCurrentTicks) {
		// run all the game commands at the current tick
		const GameCommand& gc = (*game_command_queue.begin());
		if (GetPlayerID() == gc.playerid) {
			game_command_callback = game_command_callback_get_callback(gc.callback);
		}
		game_command_playerid = gc.playerid;
		int command = gc.esi;
		money32 cost = game_do_command_p(command, (int*)&gc.eax, (int*)&gc.ebx, (int*)&gc.ecx, (int*)&gc.edx, (int*)&gc.esi, (int*)&gc.edi, (int*)&gc.ebp);
		if (cost != MONEY32_UNDEFINED) {
			NetworkPlayer* player = GetPlayerByID(gc.playerid);
			if (player) {
				player->last_action = NetworkActions::FindCommand(command);
				player->last_action_time = SDL_GetTicks();
				player->AddMoneySpent(cost);
			}
		}
		game_command_queue.erase(game_command_queue.begin());
	}
}

void Network::AddClient(ITcpSocket * socket)
{
	auto connection = std::unique_ptr<NetworkConnection>(new NetworkConnection);  // change to make_unique in c++14
	connection->Socket = socket;
	client_connection_list.push_back(std::move(connection));
}

void Network::RemoveClient(std::unique_ptr<NetworkConnection>& connection)
{
	NetworkPlayer* connection_player = connection->Player;
	if (connection_player) {
		char text[256];
		const char * has_disconnected_args[2] = {
				(char *) connection_player->name.c_str(),
				connection->GetLastDisconnectReason()
		};
		if (has_disconnected_args[1]) {
			format_string(text, 256, STR_MULTIPLAYER_PLAYER_HAS_DISCONNECTED_WITH_REASON, has_disconnected_args);
		} else {
			format_string(text, 256, STR_MULTIPLAYER_PLAYER_HAS_DISCONNECTED_NO_REASON, &(has_disconnected_args[0]));
		}

		chat_history_add(text);
		rct_peep* pickup_peep = network_get_pickup_peep(connection_player->id);
		if(pickup_peep) {
			game_command_playerid = connection_player->id;
			game_do_command(0, GAME_COMMAND_FLAG_APPLY, 1, 0, pickup_peep->type == PEEP_TYPE_GUEST ? GAME_COMMAND_PICKUP_GUEST : GAME_COMMAND_PICKUP_STAFF, network_get_pickup_peep_old_x(connection_player->id), 0);
		}
		gNetwork.Server_Send_EVENT_PLAYER_DISCONNECTED((char*)connection_player->name.c_str(), connection->GetLastDisconnectReason());
	}
	player_list.erase(std::remove_if(player_list.begin(), player_list.end(), [connection_player](std::unique_ptr<NetworkPlayer>& player){
						  return player.get() == connection_player;
					  }), player_list.end());
	client_connection_list.remove(connection);
	Server_Send_PLAYERLIST();
}

NetworkPlayer* Network::AddPlayer(const utf8 *name, const std::string &keyhash)
{
	NetworkPlayer* addedplayer = nullptr;
	int newid = -1;
	if (GetMode() == NETWORK_MODE_SERVER) {
		// Find first unused player id
		for (int id = 0; id < 255; id++) {
			if (std::find_if(player_list.begin(), player_list.end(), [&id](std::unique_ptr<NetworkPlayer> const& player) {
							 return player->id == id;
				}) == player_list.end()) {
				newid = id;
				break;
			}
		}
	} else {
		newid = 0;
	}
	if (newid != -1) {
		std::unique_ptr<NetworkPlayer> player;
		if (GetMode() == NETWORK_MODE_SERVER) {
			// Load keys host may have added manually
			_userManager.Load();

			// Check if the key is registered
			const NetworkUser * networkUser = _userManager.GetUserByHash(keyhash);

			player = std::unique_ptr<NetworkPlayer>(new NetworkPlayer); // change to make_unique in c++14
			player->id = newid;
			player->keyhash = keyhash;
			if (networkUser == nullptr) {
				player->group = GetDefaultGroup();
				if (!String::IsNullOrEmpty(name)) {
					player->SetName(MakePlayerNameUnique(std::string(name)));
				}
			} else {
				player->group = networkUser->GroupId.GetValueOrDefault(GetDefaultGroup());
				player->SetName(networkUser->Name);
			}
		} else {
			player = std::unique_ptr<NetworkPlayer>(new NetworkPlayer); // change to make_unique in c++14
			player->id = newid;
			player->group = GetDefaultGroup();
			player->SetName(name);
		}

		addedplayer = player.get();
		player_list.push_back(std::move(player));
	}
	return addedplayer;
}

std::string Network::MakePlayerNameUnique(const std::string &name)
{
	// Note: Player names are case-insensitive

	std::string new_name = name.substr(0, 31);
	int counter = 1;
	bool unique;
	do {
		unique = true;

		// Check if there is already a player with this name in the server
		for (const auto &player : player_list) {
			if (String::Equals(player->name.c_str(), new_name.c_str(), true)) {
				unique = false;
				break;
			}
		}

		if (unique) {
			// Check if there is already a registered player with this name
			if (_userManager.GetUserByName(new_name) != nullptr) {
				unique = false;
			}
		}

		if (!unique) {
			// Increment name counter
			counter++;
			new_name = name.substr(0, 31) + " #" + std::to_string(counter);
		}
	} while (!unique);
	return new_name;
}

void Network::Client_Handle_TOKEN(NetworkConnection& connection, NetworkPacket& packet)
{
	utf8 keyPath[MAX_PATH];
	network_get_private_key_path(keyPath, sizeof(keyPath), gConfigNetwork.player_name);
	if (!platform_file_exists(keyPath)) {
		log_error("Key file (%s) was not found. Restart client to re-generate it.", keyPath);
		return;
	}
	SDL_RWops *privkey = SDL_RWFromFile(keyPath, "rb");
	bool ok = key.LoadPrivate(privkey);
	SDL_RWclose(privkey);
	if (!ok) {
		log_error("Failed to load key %s", keyPath);
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_VERIFICATION_FAILURE);
		connection.Socket->Disconnect();
		return;
	}
	uint32 challenge_size;
	packet >> challenge_size;
	const char *challenge = (const char *)packet.Read(challenge_size);
	size_t sigsize;
	char *signature;
	const std::string pubkey = key.PublicKeyString();
	this->challenge.resize(challenge_size);
	memcpy(this->challenge.data(), challenge, challenge_size);
	ok = key.Sign(this->challenge.data(), this->challenge.size(), &signature, &sigsize);
	if (!ok) {
		log_error("Failed to sign server's challenge.");
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_VERIFICATION_FAILURE);
		connection.Socket->Disconnect();
		return;
	}
	// Don't keep private key in memory. There's no need and it may get leaked
	// when process dump gets collected at some point in future.
	key.Unload();
	Client_Send_AUTH(gConfigNetwork.player_name, "", pubkey.c_str(), signature, sigsize);
	delete [] signature;
}

void Network::Client_Handle_AUTH(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 auth_status;
	packet >> auth_status >> (uint8&)player_id;
	connection.AuthStatus = (NETWORK_AUTH)auth_status;
	switch(connection.AuthStatus) {
	case NETWORK_AUTH_OK:
		Client_Send_GAMEINFO();
		break;
	case NETWORK_AUTH_BADNAME:
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_BAD_PLAYER_NAME);
		connection.Socket->Disconnect();
		break;
	case NETWORK_AUTH_BADVERSION:
	{
		const char *version = packet.ReadString();
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_INCORRECT_SOFTWARE_VERSION, &version);
		connection.Socket->Disconnect();
		break;
	}
	case NETWORK_AUTH_BADPASSWORD:
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_BAD_PASSWORD);
		connection.Socket->Disconnect();
		break;
	case NETWORK_AUTH_VERIFICATIONFAILURE:
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_VERIFICATION_FAILURE);
		connection.Socket->Disconnect();
		break;
	case NETWORK_AUTH_FULL:
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_SERVER_FULL);
		connection.Socket->Disconnect();
		break;
	case NETWORK_AUTH_REQUIREPASSWORD:
		window_network_status_open_password();
		break;
	case NETWORK_AUTH_UNKNOWN_KEY_DISALLOWED:
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_UNKNOWN_KEY_DISALLOWED);
		connection.Socket->Disconnect();
		break;
	default:
		connection.SetLastDisconnectReason(STR_MULTIPLAYER_INCORRECT_SOFTWARE_VERSION);
		connection.Socket->Disconnect();
		break;
	}
}

void Network::Server_Client_Joined(const char* name, const std::string &keyhash, NetworkConnection& connection)
{
	NetworkPlayer* player = AddPlayer(name, keyhash);
	connection.Player = player;
	if (player) {
		char text[256];
		const char * player_name = (const char *) player->name.c_str();
		format_string(text, 256, STR_MULTIPLAYER_PLAYER_HAS_JOINED_THE_GAME, &player_name);
		chat_history_add(text);
		Server_Send_MAP(&connection);
		gNetwork.Server_Send_EVENT_PLAYER_JOINED(player_name);
		Server_Send_GROUPLIST(connection);
		Server_Send_PLAYERLIST();
	}
}

void Network::Server_Handle_TOKEN(NetworkConnection& connection, NetworkPacket& packet)
{
	uint8 token_size = 10 + (rand() & 0x7f);
	connection.Challenge.resize(token_size);
	for (int i = 0; i < token_size; i++) {
		connection.Challenge[i] = (uint8)(rand() & 0xff);
	}
	Server_Send_TOKEN(connection);
}

void Network::Server_Handle_AUTH(NetworkConnection& connection, NetworkPacket& packet)
{
	if (connection.AuthStatus != NETWORK_AUTH_OK) {
		const char* gameversion = packet.ReadString();
		const char* name = packet.ReadString();
		const char* password = packet.ReadString();
		const char *pubkey = (const char *)packet.ReadString();
		uint32 sigsize;
		packet >> sigsize;
		if (pubkey == nullptr) {
			connection.AuthStatus = NETWORK_AUTH_VERIFICATIONFAILURE;
		} else {
			const char *signature = (const char *)packet.Read(sigsize);
			SDL_RWops *pubkey_rw = SDL_RWFromConstMem(pubkey, (int)strlen(pubkey));
			if (signature == nullptr || pubkey_rw == nullptr) {
				connection.AuthStatus = NETWORK_AUTH_VERIFICATIONFAILURE;
				log_verbose("Signature verification failed, invalid data!");
			} else {
				connection.Key.LoadPublic(pubkey_rw);
				SDL_RWclose(pubkey_rw);
				bool verified = connection.Key.Verify(connection.Challenge.data(), connection.Challenge.size(), signature, sigsize);
				const std::string hash = connection.Key.PublicKeyHash();
				if (verified) {
					connection.AuthStatus = NETWORK_AUTH_VERIFIED;
					log_verbose("Signature verification ok. Hash %s", hash.c_str());
				} else {
					connection.AuthStatus = NETWORK_AUTH_VERIFICATIONFAILURE;
					log_verbose("Signature verification failed!");
				}
				if (gConfigNetwork.known_keys_only && _userManager.GetUserByHash(hash) == nullptr) {
					connection.AuthStatus = NETWORK_AUTH_UNKNOWN_KEY_DISALLOWED;
				}
			}
		}

		bool passwordless = false;
		if (connection.AuthStatus == NETWORK_AUTH_VERIFIED) {
			const NetworkGroup * group = GetGroupByID(GetGroupIDByHash(connection.Key.PublicKeyHash()));
			size_t actionIndex = NetworkActions::FindCommandByPermissionName("PERMISSION_PASSWORDLESS_LOGIN");
			passwordless = group->CanPerformAction(actionIndex);
		}
		if (!gameversion || strcmp(gameversion, NETWORK_STREAM_ID) != 0) {
			connection.AuthStatus = NETWORK_AUTH_BADVERSION;
		} else
		if (!name) {
			connection.AuthStatus = NETWORK_AUTH_BADNAME;
		} else
		if (!passwordless) {
			if ((!password || strlen(password) == 0) && Network::password.size() > 0) {
				connection.AuthStatus = NETWORK_AUTH_REQUIREPASSWORD;
			} else
			if (password && Network::password != password) {
				connection.AuthStatus = NETWORK_AUTH_BADPASSWORD;
			}
		}

		if (gConfigNetwork.maxplayers <= player_list.size()) {
			connection.AuthStatus = NETWORK_AUTH_FULL;
		} else
		if (connection.AuthStatus == NETWORK_AUTH_VERIFIED) {
			connection.AuthStatus = NETWORK_AUTH_OK;
			const std::string hash = connection.Key.PublicKeyHash();
			Server_Client_Joined(name, hash, connection);
		} else
		if (connection.AuthStatus != NETWORK_AUTH_REQUIREPASSWORD) {
			log_error("Unknown failure (%d) while authenticating client", connection.AuthStatus);
		}
		Server_Send_AUTH(connection);
	}
}

void Network::Client_Handle_MAP(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 size, offset;
	packet >> size >> offset;
	int chunksize = (int)(packet.size - packet.read);
	if (chunksize <= 0) {
		return;
	}
	if (size > chunk_buffer.size()) {
		chunk_buffer.resize(size);
	}
	char str_downloading_map[256];
	unsigned int downloading_map_args[2] = {(offset + chunksize) / 1024, size / 1024};
	format_string(str_downloading_map, 256, STR_MULTIPLAYER_DOWNLOADING_MAP, downloading_map_args);
	window_network_status_open(str_downloading_map, []() -> void {
		gNetwork.Close();
	});
	memcpy(&chunk_buffer[offset], (void*)packet.Read(chunksize), chunksize);
	if (offset + chunksize == size) {
		window_network_status_close();
		bool has_to_free = false;
		unsigned char *data = &chunk_buffer[0];
		size_t data_size = size;
		// zlib-compressed
		if (strcmp("open2_sv6_zlib", (char *)&chunk_buffer[0]) == 0)
		{
			log_verbose("Received zlib-compressed sv6 map");
			has_to_free = true;
			size_t header_len = strlen("open2_sv6_zlib") + 1;
			data = util_zlib_inflate(&chunk_buffer[header_len], size - header_len, &data_size);
			if (data == NULL)
			{
				log_warning("Failed to decompress data sent from server.");
				Close();
				return;
			}
		} else {
			log_verbose("Assuming received map is in plain sv6 format");
		}
		SDL_RWops* rw = SDL_RWFromMem(data, (int)data_size);
		if (game_load_network(rw)) {
			game_load_init();
			game_command_queue.clear();
			server_tick = gCurrentTicks;
			server_srand0_tick = 0;
			// window_network_status_open("Loaded new map from network");
			_desynchronised = false;
			gFirstTimeSave = 1;

			// Notify user he is now online and which shortcut key enables chat
			network_chat_show_connected_message();
		}
		else
		{
			//Something went wrong, game is not loaded. Return to main screen.
			game_do_command(0, GAME_COMMAND_FLAG_APPLY, 0, 0, GAME_COMMAND_LOAD_OR_QUIT, 1, 0);
		}
		SDL_RWclose(rw);
		if (has_to_free)
		{
			free(data);
		}
	}
}

void Network::Client_Handle_CHAT(NetworkConnection& connection, NetworkPacket& packet)
{
	const char* text = packet.ReadString();
	if (text) {
		chat_history_add(text);
	}
}

void Network::Server_Handle_CHAT(NetworkConnection& connection, NetworkPacket& packet)
{
	if (connection.Player) {
		NetworkGroup* group = GetGroupByID(connection.Player->group);
		if (!group || (group && !group->CanPerformCommand(-1))) {
			return;
		}
	}
	const char* text = packet.ReadString();
	if (text) {
		const char* formatted = FormatChat(connection.Player, text);
		chat_history_add(formatted);
		Server_Send_CHAT(formatted);
	}
}

void Network::Client_Handle_GAMECMD(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 tick;
	uint32 args[7];
	uint8 playerid;
	uint8 callback;
	packet >> tick >> args[0] >> args[1] >> args[2] >> args[3] >> args[4] >> args[5] >> args[6] >> playerid >> callback;

	GameCommand gc = GameCommand(tick, args, playerid, callback);
	game_command_queue.insert(gc);
}

void Network::Server_Handle_GAMECMD(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 tick;
	uint32 args[7];
	uint8 playerid;
	uint8 callback;

	if (!connection.Player) {
		return;
	}

	playerid = connection.Player->id;

	packet >> tick >> args[0] >> args[1] >> args[2] >> args[3] >> args[4] >> args[5] >> args[6] >> callback;

	int commandCommand = args[4];

	int ticks = SDL_GetTicks(); //tick count is different by time last_action_time is set, keep same value.
	
	// Check if player's group permission allows command to run
	NetworkGroup* group = GetGroupByID(connection.Player->group);
	if (!group || (group && !group->CanPerformCommand(commandCommand))) {
		Server_Send_SHOWERROR(connection, STR_CANT_DO_THIS, STR_PERMISSION_DENIED);
		return;
	}
	// In case someone modifies the code / memory to enable cluster build,
	// require a small delay in between placing scenery to provide some security, as 
	// cluster mode is a for loop that runs the place_scenery code multiple times.
	if (commandCommand == GAME_COMMAND_PLACE_SCENERY) {
		if ((ticks - connection.Player->last_action_time) < 20) {
			if (!(group->CanPerformCommand(-2))) {
				Server_Send_SHOWERROR(connection, STR_CANT_DO_THIS, STR_CANT_DO_THIS);
				return;
			}
		}
	}
	// Don't let clients send pause or quit
	if (commandCommand == GAME_COMMAND_TOGGLE_PAUSE ||
		commandCommand == GAME_COMMAND_LOAD_OR_QUIT
	) {
		return;
	}

	// Set this to reference inside of game command functions
	game_command_playerid = playerid;
	// Run game command, and if it is successful send to clients
	money32 cost = game_do_command(args[0], args[1] | GAME_COMMAND_FLAG_NETWORKED, args[2], args[3], args[4], args[5], args[6]);
	if (cost == MONEY32_UNDEFINED) {
		return;
	}

	connection.Player->last_action = NetworkActions::FindCommand(commandCommand);
	connection.Player->last_action_time = SDL_GetTicks();
	connection.Player->AddMoneySpent(cost);
	Server_Send_GAMECMD(args[0], args[1], args[2], args[3], args[4], args[5], args[6], playerid, callback);
}

void Network::Client_Handle_TICK(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 srand0;
	uint32 flags;
	// Note: older server version may not advertise flags at all.
	// NetworkPacket will return 0, if trying to read past end of buffer,
	// so flags == 0 is expected in such cases.
	packet >> server_tick >> srand0 >> flags;
	if (server_srand0_tick == 0) {
		server_srand0 = srand0;
		server_srand0_tick = server_tick;
		server_sprite_hash[0] = '\0';
		if (flags & NETWORK_TICK_FLAG_CHECKSUMS)
		{
			const char* text = packet.ReadString();
			if (text != nullptr)
			{
				safe_strcpy(server_sprite_hash, text, sizeof(server_sprite_hash));
			}
		}
	}
}

void Network::Client_Handle_PLAYERLIST(NetworkConnection& connection, NetworkPacket& packet)
{
	uint8 size;
	packet >> size;
	std::vector<uint8> ids;
	for (unsigned int i = 0; i < size; i++) {
		NetworkPlayer tempplayer;
		tempplayer.Read(packet);
		ids.push_back(tempplayer.id);
		if (!GetPlayerByID(tempplayer.id)) {
			NetworkPlayer* player = AddPlayer("", "");
			if (player) {
				*player = tempplayer;
				if (player->flags & NETWORK_PLAYER_FLAG_ISSERVER) {
					server_connection.Player = player;
				}
			}
		}
	}
	// Remove any players that are not in newly received list
	auto it = player_list.begin();
	while (it != player_list.end()) {
		if (std::find(ids.begin(), ids.end(), (*it)->id) == ids.end()) {
			it = player_list.erase(it);
		} else {
			it++;
		}
	}
}

void Network::Client_Handle_PING(NetworkConnection& connection, NetworkPacket& packet)
{
	Client_Send_PING();
}

void Network::Server_Handle_PING(NetworkConnection& connection, NetworkPacket& packet)
{
	int ping = SDL_GetTicks() - connection.PingTime;
	if (ping < 0) {
		ping = 0;
	}
	if (connection.Player) {
		connection.Player->ping = ping;
		window_invalidate_by_number(WC_PLAYER, connection.Player->id);
	}
}

void Network::Client_Handle_PINGLIST(NetworkConnection& connection, NetworkPacket& packet)
{
	uint8 size;
	packet >> size;
	for (unsigned int i = 0; i < size; i++) {
		uint8 id;
		uint16 ping;
		packet >> id >> ping;
		NetworkPlayer* player = GetPlayerByID(id);
		if (player) {
			player->ping = ping;
		}
	}
	window_invalidate_by_class(WC_PLAYER);
}

void Network::Client_Handle_SETDISCONNECTMSG(NetworkConnection& connection, NetworkPacket& packet)
{
	static std::string msg;
	const char* disconnectmsg = packet.ReadString();
	if (disconnectmsg) {
		msg = disconnectmsg;
		connection.SetLastDisconnectReason(msg.c_str());
	}
}

void Network::Server_Handle_GAMEINFO(NetworkConnection& connection, NetworkPacket& packet)
{
	Server_Send_GAMEINFO(connection);
}

void Network::Client_Handle_SHOWERROR(NetworkConnection& connection, NetworkPacket& packet)
{
	rct_string_id title, message;
	packet >> title >> message;
	window_error_open(title, message);
}

void Network::Client_Handle_GROUPLIST(NetworkConnection& connection, NetworkPacket& packet)
{
	group_list.clear();
	uint8 size;
	packet >> size >> default_group;
	for (unsigned int i = 0; i < size; i++) {
		NetworkGroup group;
		group.Read(packet);
		std::unique_ptr<NetworkGroup> newgroup(new NetworkGroup(group)); // change to make_unique in c++14
		group_list.push_back(std::move(newgroup));
	}
}

void Network::Client_Handle_EVENT(NetworkConnection& connection, NetworkPacket& packet)
{
	char text[256];
	uint16 eventType;
	packet >> eventType;
	switch (eventType) {
	case SERVER_EVENT_PLAYER_JOINED:
	{
		const char *playerName = packet.ReadString();
		format_string(text, 256, STR_MULTIPLAYER_PLAYER_HAS_JOINED_THE_GAME, &playerName);
		chat_history_add(text);
		break;
	}
	case SERVER_EVENT_PLAYER_DISCONNECTED:
	{
		const char *playerName = packet.ReadString();
		const char *reason = packet.ReadString();
		const char *args[] = { playerName, reason };
		if (str_is_null_or_empty(reason)) {
			format_string(text, 256, STR_MULTIPLAYER_PLAYER_HAS_DISCONNECTED_NO_REASON, args);
		} else {
			format_string(text, 256, STR_MULTIPLAYER_PLAYER_HAS_DISCONNECTED_WITH_REASON, args);
		}
		chat_history_add(text);
		break;
	}
	}
}

void Network::Client_Send_GAMEINFO()
{
	log_verbose("requesting gameinfo");
	std::unique_ptr<NetworkPacket> packet(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_GAMEINFO;
	server_connection.QueuePacket(std::move(packet));
}

static std::string json_stdstring_value(const json_t * string)
{
	const char * cstr = json_string_value(string);
	return cstr == nullptr ? std::string() : std::string(cstr);
}

void Network::Client_Handle_GAMEINFO(NetworkConnection& connection, NetworkPacket& packet)
{
	const char * jsonString = packet.ReadString();

	json_error_t error;
	json_t *root = json_loads(jsonString, 0, &error);

	ServerName = json_stdstring_value(json_object_get(root, "name"));
	ServerDescription = json_stdstring_value(json_object_get(root, "description"));
	ServerGreeting = json_stdstring_value(json_object_get(root, "greeting"));

	json_t *jsonProvider = json_object_get(root, "provider");
	if (jsonProvider != nullptr) {
		ServerProviderName = json_stdstring_value(json_object_get(jsonProvider, "name"));
		ServerProviderEmail = json_stdstring_value(json_object_get(jsonProvider, "email"));
		ServerProviderWebsite = json_stdstring_value(json_object_get(jsonProvider, "website"));
	}
	json_decref(root);

	network_chat_show_server_greeting();
}

namespace Convert
{
	uint16 HostToNetwork(uint16 value)
	{
		return htons(value);
	}

	uint16 NetworkToHost(uint16 value)
	{
		return ntohs(value);
	}
}

int network_init()
{
	return gNetwork.Init();
}

void network_close()
{
	gNetwork.Close();
}

void network_shutdown_client()
{
	gNetwork.ShutdownClient();
}

int network_begin_client(const char *host, int port)
{
	return gNetwork.BeginClient(host, port);
}

int network_begin_server(int port)
{
	return gNetwork.BeginServer(port);
}

void network_update()
{
	gNetwork.Update();
}

int network_get_mode()
{
	return gNetwork.GetMode();
}

int network_get_status()
{
	return gNetwork.GetStatus();
}

int network_get_authstatus()
{
	return gNetwork.GetAuthStatus();
}

uint32 network_get_server_tick()
{
	return gNetwork.GetServerTick();
}

uint8 network_get_current_player_id()
{
	return gNetwork.GetPlayerID();
}

int network_get_num_players()
{
	return (int)gNetwork.player_list.size();
}

const char* network_get_player_name(unsigned int index)
{
	return (const char*)gNetwork.player_list[index]->name.c_str();
}

uint32 network_get_player_flags(unsigned int index)
{
	return gNetwork.player_list[index]->flags;
}

int network_get_player_ping(unsigned int index)
{
	return gNetwork.player_list[index]->ping;
}

int network_get_player_id(unsigned int index)
{
	return gNetwork.player_list[index]->id;
}

money32 network_get_player_money_spent(unsigned int index)
{
	return gNetwork.player_list[index]->money_spent;
}

void network_add_player_money_spent(unsigned int index, money32 cost)
{
	gNetwork.player_list[index]->AddMoneySpent(cost);
}

int network_get_player_last_action(unsigned int index, int time)
{
	if (time && SDL_TICKS_PASSED(SDL_GetTicks(), gNetwork.player_list[index]->last_action_time + time)) {
		return -999;
	}
	return gNetwork.player_list[index]->last_action;
}

void network_set_player_last_action(unsigned int index, int command)
{
	gNetwork.player_list[index]->last_action = NetworkActions::FindCommand(command);
	gNetwork.player_list[index]->last_action_time = SDL_GetTicks();
}

rct_xyz16 network_get_player_last_action_coord(unsigned int index)
{
	return gNetwork.player_list[index]->last_action_coord;
}

void network_set_player_last_action_coord(unsigned int index, rct_xyz16 coord)
{
	if (index < gNetwork.player_list.size()) {
		gNetwork.player_list[index]->last_action_coord = coord;
	}
}

unsigned int network_get_player_commands_ran(unsigned int index)
{
	return gNetwork.player_list[index]->commands_ran;
}

int network_get_player_index(uint8 id)
{
	auto it = gNetwork.GetPlayerIteratorByID(id);
	if(it == gNetwork.player_list.end()){
		return -1;
	}
	return (int)(gNetwork.GetPlayerIteratorByID(id) - gNetwork.player_list.begin());
}

uint8 network_get_player_group(unsigned int index)
{
	return gNetwork.player_list[index]->group;
}

void network_set_player_group(unsigned int index, unsigned int groupindex)
{
	gNetwork.player_list[index]->group = gNetwork.group_list[groupindex]->Id;
}

int network_get_group_index(uint8 id)
{
	auto it = gNetwork.GetGroupIteratorByID(id);
	if(it == gNetwork.group_list.end()){
		return -1;
	}
	return (int)(gNetwork.GetGroupIteratorByID(id) - gNetwork.group_list.begin());
}

uint8 network_get_group_id(unsigned int index)
{
	return gNetwork.group_list[index]->Id;
}

int network_get_num_groups()
{
	return (int)gNetwork.group_list.size();
}

const char* network_get_group_name(unsigned int index)
{
	return gNetwork.group_list[index]->GetName().c_str();
}

void network_chat_show_connected_message()
{
	char templateBuffer[128];
	char *templateString = templateBuffer;
	keyboard_shortcut_format_string(templateBuffer, 128, gShortcutKeys[SHORTCUT_OPEN_CHAT_WINDOW]);
	utf8 buffer[256];
	NetworkPlayer server;
	server.name = "Server";
	format_string(buffer, 256, STR_MULTIPLAYER_CONNECTED_CHAT_HINT, &templateString);
	const char *formatted = Network::FormatChat(&server, buffer);
	chat_history_add(formatted);
}

// Display server greeting if one exists
void network_chat_show_server_greeting()
{
	const char* greeting = gConfigNetwork.server_greeting;
	if (!str_is_null_or_empty(greeting)) {
		static char greeting_formatted[CHAT_INPUT_SIZE];
		char* lineCh = greeting_formatted;
		greeting_formatted[0] = 0;
		lineCh = utf8_write_codepoint(lineCh, FORMAT_OUTLINE);
		lineCh = utf8_write_codepoint(lineCh, FORMAT_GREEN);
		char* ptrtext = lineCh;
		safe_strcpy(lineCh, greeting, CHAT_INPUT_SIZE - 24); // Limit to 1000 characters so we don't overflow the buffer
		utf8_remove_format_codes((utf8*)ptrtext, true);
		chat_history_add(greeting_formatted);
	}
}

void game_command_set_player_group(int* eax, int* ebx, int* ecx, int* edx, int* esi, int* edi, int* ebp)
{
	uint8 playerid = (uint8)*ecx;
	uint8 groupid = (uint8)*edx;
	NetworkPlayer* player = gNetwork.GetPlayerByID(playerid);
	NetworkGroup* fromgroup = gNetwork.GetGroupByID(game_command_playerid);
	if (!player) {
		gGameCommandErrorTitle = STR_CANT_DO_THIS;
		gGameCommandErrorText = STR_NONE;
		*ebx = MONEY32_UNDEFINED;
		return;
	}
	if (!gNetwork.GetGroupByID(groupid)) {
		gGameCommandErrorTitle = STR_CANT_DO_THIS;
		gGameCommandErrorText = STR_NONE;
		*ebx = MONEY32_UNDEFINED;
		return;
	}
	if (player->flags & NETWORK_PLAYER_FLAG_ISSERVER) {
		gGameCommandErrorTitle = STR_CANT_CHANGE_GROUP_THAT_THE_HOST_BELONGS_TO;
		gGameCommandErrorText = STR_NONE;
		*ebx = MONEY32_UNDEFINED;
		return;
	}
	if (groupid == 0 && fromgroup && fromgroup->Id != 0) {
		gGameCommandErrorTitle = STR_CANT_SET_TO_THIS_GROUP;
		gGameCommandErrorText = STR_NONE;
		*ebx = MONEY32_UNDEFINED;
		return;
	}
	if (*ebx & GAME_COMMAND_FLAG_APPLY) {
		player->group = groupid;

		if (network_get_mode() == NETWORK_MODE_SERVER) {
			// Add or update saved user
			NetworkUserManager *userManager = &gNetwork._userManager;
			NetworkUser *networkUser = userManager->GetOrAddUser(player->keyhash);
			networkUser->GroupId = groupid;
			networkUser->Name = player->name;
			userManager->Save();
		}

		window_invalidate_by_number(WC_PLAYER, playerid);
	}
	*ebx = 0;
}

void game_command_modify_groups(int *eax, int *ebx, int *ecx, int *edx, int *esi, int *edi, int *ebp)
{
	uint8 action = (uint8)*eax;
	uint8 groupid = (uint8)(*eax >> 8);
	uint8 nameChunkIndex = (uint8)(*eax >> 16);

	char oldName[128];
	static char newName[128];

	switch (action)
	{
	case 0:{ // add group
		if (*ebx & GAME_COMMAND_FLAG_APPLY) {
			NetworkGroup* newgroup = gNetwork.AddGroup();
			if (!newgroup) {
				gGameCommandErrorTitle = STR_CANT_DO_THIS;
				gGameCommandErrorText = STR_NONE;
				*ebx = MONEY32_UNDEFINED;
				return;
			}
		}
	}break;
	case 1:{ // remove group
		if (groupid == 0) {
			gGameCommandErrorTitle = STR_THIS_GROUP_CANNOT_BE_MODIFIED;
			gGameCommandErrorText = STR_NONE;
			*ebx = MONEY32_UNDEFINED;
			return;
		}
		for (auto it = gNetwork.player_list.begin(); it != gNetwork.player_list.end(); it++) {
			if((*it)->group == groupid) {
				gGameCommandErrorTitle = STR_CANT_REMOVE_GROUP_THAT_PLAYERS_BELONG_TO;
				gGameCommandErrorText = STR_NONE;
				*ebx = MONEY32_UNDEFINED;
				return;
			}
		}
		if (*ebx & GAME_COMMAND_FLAG_APPLY) {
			gNetwork.RemoveGroup(groupid);
		}
	}break;
	case 2:{ // set permissions
		int index = *ecx;
		bool all = *edx & 1;
		bool allvalue = (*edx >> 1) & 1;
		if (groupid == 0) { // cant change admin group permissions
			gGameCommandErrorTitle = STR_THIS_GROUP_CANNOT_BE_MODIFIED;
			gGameCommandErrorText = STR_NONE;
			*ebx = MONEY32_UNDEFINED;
			return;
		}
		NetworkGroup* mygroup = nullptr;
		NetworkPlayer* player = gNetwork.GetPlayerByID(game_command_playerid);
		if (player && !all) {
			mygroup = gNetwork.GetGroupByID(player->group);
			if (!mygroup || (mygroup && !mygroup->CanPerformAction(index))) {
				gGameCommandErrorTitle = STR_CANT_MODIFY_PERMISSION_THAT_YOU_DO_NOT_HAVE_YOURSELF;
				gGameCommandErrorText = STR_NONE;
				*ebx = MONEY32_UNDEFINED;
				return;
			}
		}
		if (*ebx & GAME_COMMAND_FLAG_APPLY) {
			NetworkGroup* group = gNetwork.GetGroupByID(groupid);
			if (group) {
				if (all) {
					if (mygroup) {
						if (allvalue) {
							group->ActionsAllowed = mygroup->ActionsAllowed;
						} else {
							group->ActionsAllowed.fill(0x00);
						}
					}
				} else {
					group->ToggleActionPermission(index);
				}
			}
		}
	}break;
	case 3:{ // set group name
		size_t nameChunkOffset = nameChunkIndex - 1;
		if (nameChunkIndex == 0)
			nameChunkOffset = 2;
		nameChunkOffset *= 12;
		nameChunkOffset = (std::min)(nameChunkOffset, Util::CountOf(newName) - 12);
		memcpy((void*)((uintptr_t)newName + (uintptr_t)nameChunkOffset + 0), edx, sizeof(uint32));
		memcpy((void*)((uintptr_t)newName + (uintptr_t)nameChunkOffset + 4), ebp, sizeof(uint32));
		memcpy((void*)((uintptr_t)newName + (uintptr_t)nameChunkOffset + 8), edi, sizeof(uint32));

		if (nameChunkIndex != 0) {
			*ebx = 0;
			return;
		}

		if (strcmp(oldName, newName) == 0) {
			*ebx = 0;
			return;
		}

		if (newName[0] == 0) {
			gGameCommandErrorTitle = STR_CANT_RENAME_GROUP;
			gGameCommandErrorText = STR_INVALID_GROUP_NAME;
			*ebx = MONEY32_UNDEFINED;
			return;
		}

		if (*ebx & GAME_COMMAND_FLAG_APPLY) {
			NetworkGroup* group = gNetwork.GetGroupByID(groupid);
			if (group) {
				group->SetName(newName);
			}
		}
	}break;
	case 4:{ // set default group
		if (groupid == 0) {
			gGameCommandErrorTitle = STR_CANT_SET_TO_THIS_GROUP;
			gGameCommandErrorText = STR_NONE;
			*ebx = MONEY32_UNDEFINED;
			return;
		}
		if (*ebx & GAME_COMMAND_FLAG_APPLY) {
			gNetwork.SetDefaultGroup(groupid);
		}
	}break;
	}

	gNetwork.SaveGroups();

	*ebx = 0;
}

void game_command_kick_player(int *eax, int *ebx, int *ecx, int *edx, int *esi, int *edi, int *ebp)
{
	uint8 playerid = (uint8)*eax;
	NetworkPlayer* player = gNetwork.GetPlayerByID(playerid);
	if (player && player->flags & NETWORK_PLAYER_FLAG_ISSERVER) {
		gGameCommandErrorTitle = STR_CANT_KICK_THE_HOST;
		gGameCommandErrorText = STR_NONE;
		*ebx = MONEY32_UNDEFINED;
		return;
	}
	if (*ebx & GAME_COMMAND_FLAG_APPLY) {
		if (gNetwork.GetMode() == NETWORK_MODE_SERVER) {
			gNetwork.KickPlayer(playerid);

			NetworkUserManager * networkUserManager = &gNetwork._userManager;
			networkUserManager->Load();
			networkUserManager->RemoveUser(player->keyhash);
			networkUserManager->Save();
		}
	}
	*ebx = 0;
}

uint8 network_get_default_group()
{
	return gNetwork.GetDefaultGroup();
}

int network_get_num_actions()
{
	return (int)NetworkActions::Actions.size();
}

rct_string_id network_get_action_name_string_id(unsigned int index)
{
	if (index < NetworkActions::Actions.size())
	{
		return NetworkActions::Actions[index].Name;
	} else {
		return STR_NONE;
	}
}

int network_can_perform_action(unsigned int groupindex, unsigned int index)
{
	return gNetwork.group_list[groupindex]->CanPerformAction(index);
}

int network_can_perform_command(unsigned int groupindex, unsigned int index) 
{
	return gNetwork.group_list[groupindex]->CanPerformCommand(index);
}

void network_set_pickup_peep(uint8 playerid, rct_peep* peep)
{
	gNetwork.GetMode() == NETWORK_MODE_NONE ? _pickup_peep = peep : gNetwork.GetPlayerByID(playerid)->pickup_peep = peep;
}

rct_peep* network_get_pickup_peep(uint8 playerid)
{
	return gNetwork.GetMode() == NETWORK_MODE_NONE ? _pickup_peep : gNetwork.GetPlayerByID(playerid)->pickup_peep;
}

void network_set_pickup_peep_old_x(uint8 playerid, int x)
{
	gNetwork.GetMode() == NETWORK_MODE_NONE ? _pickup_peep_old_x = x : gNetwork.GetPlayerByID(playerid)->pickup_peep_old_x = x;
}

int network_get_pickup_peep_old_x(uint8 playerid)
{
	return gNetwork.GetMode() == NETWORK_MODE_NONE ? _pickup_peep_old_x : gNetwork.GetPlayerByID(playerid)->pickup_peep_old_x;
}

int network_get_current_player_group_index()
{
	return network_get_group_index(gNetwork.GetPlayerByID(gNetwork.GetPlayerID())->group);
}

void network_send_map()
{
	gNetwork.Server_Send_MAP();
}

void network_send_chat(const char* text)
{
	if (gNetwork.GetMode() == NETWORK_MODE_CLIENT) {
		gNetwork.Client_Send_CHAT(text);
	} else
	if (gNetwork.GetMode() == NETWORK_MODE_SERVER) {
		NetworkPlayer* player = gNetwork.GetPlayerByID(gNetwork.GetPlayerID());
		const char* formatted = gNetwork.FormatChat(player, text);
		chat_history_add(formatted);
		gNetwork.Server_Send_CHAT(formatted);
	}
}

void network_send_gamecmd(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 callback)
{
	switch (gNetwork.GetMode()) {
	case NETWORK_MODE_SERVER:
		gNetwork.Server_Send_GAMECMD(eax, ebx, ecx, edx, esi, edi, ebp, gNetwork.GetPlayerID(), callback);
		break;
	case NETWORK_MODE_CLIENT:
		gNetwork.Client_Send_GAMECMD(eax, ebx, ecx, edx, esi, edi, ebp, callback);
		break;
	}
}

void network_send_password(const char* password)
{
	utf8 keyPath[MAX_PATH];
	network_get_private_key_path(keyPath, sizeof(keyPath), gConfigNetwork.player_name);
	if (!platform_file_exists(keyPath)) {
		log_error("Private key %s missing! Restart the game to generate it.", keyPath);
		return;
	}
	SDL_RWops *privkey = SDL_RWFromFile(keyPath, "rb");
	gNetwork.key.LoadPrivate(privkey);
	const std::string pubkey = gNetwork.key.PublicKeyString();
	size_t sigsize;
	char *signature;
	gNetwork.key.Sign(gNetwork.challenge.data(), gNetwork.challenge.size(), &signature, &sigsize);
	// Don't keep private key in memory. There's no need and it may get leaked
	// when process dump gets collected at some point in future.
	gNetwork.key.Unload();
	gNetwork.Client_Send_AUTH(gConfigNetwork.player_name, password, pubkey.c_str(), signature, sigsize);
	delete [] signature;
}

void network_set_password(const char* password)
{
	gNetwork.SetPassword(password);
}

void network_append_chat_log(const utf8 *text)
{
	gNetwork.AppendChatLog(text);
}

static void network_get_keys_directory(utf8 *buffer, size_t bufferSize)
{
	platform_get_user_directory(buffer, "keys", bufferSize);
}

static void network_get_private_key_path(utf8 *buffer, size_t bufferSize, const utf8 * playerName)
{
	network_get_keys_directory(buffer, bufferSize);
	Path::Append(buffer, bufferSize, playerName);
	String::Append(buffer, bufferSize, ".privkey");
}

static void network_get_public_key_path(utf8 *buffer, size_t bufferSize, const utf8 * playerName, const utf8 * hash)
{
	network_get_keys_directory(buffer, bufferSize);
	Path::Append(buffer, bufferSize, playerName);
	String::Append(buffer, bufferSize, "-");
	String::Append(buffer, bufferSize, hash);
	String::Append(buffer, bufferSize, ".pubkey");
}

static void network_get_keymap_path(utf8 *buffer, size_t bufferSize)
{
	platform_get_user_directory(buffer, NULL, bufferSize);
	Path::Append(buffer, bufferSize, "keymappings.json");
}

const utf8 * network_get_server_name() { return gNetwork.ServerName.c_str(); }
const utf8 * network_get_server_description() { return gNetwork.ServerDescription.c_str(); }
const utf8 * network_get_server_greeting() { return gNetwork.ServerGreeting.c_str(); }
const utf8 * network_get_server_provider_name() { return gNetwork.ServerProviderName.c_str(); }
const utf8 * network_get_server_provider_email() { return gNetwork.ServerProviderEmail.c_str(); }
const utf8 * network_get_server_provider_website() { return gNetwork.ServerProviderWebsite.c_str(); }

#else
int network_get_mode() { return NETWORK_MODE_NONE; }
int network_get_status() { return NETWORK_STATUS_NONE; }
int network_get_authstatus() { return NETWORK_AUTH_NONE; }
uint32 network_get_server_tick() { return gCurrentTicks; }
void network_send_gamecmd(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 callback) {}
void network_send_map() {}
void network_update() {}
int network_begin_client(const char *host, int port) { return 1; }
int network_begin_server(int port) { return 1; }
int network_get_num_players() { return 1; }
const char* network_get_player_name(unsigned int index) { return "local (OpenRCT2 compiled without MP)"; }
uint32 network_get_player_flags(unsigned int index) { return 0; }
int network_get_player_ping(unsigned int index) { return 0; }
int network_get_player_id(unsigned int index) { return 0; }
money32 network_get_player_money_spent(unsigned int index) { return MONEY(0, 0); }
void network_add_player_money_spent(unsigned int index, money32 cost) { }
int network_get_player_last_action(unsigned int index, int time) { return -999; }
void network_set_player_last_action(unsigned int index, int command) { }
rct_xyz16 network_get_player_last_action_coord(unsigned int index) { return {0, 0, 0}; }
void network_set_player_last_action_coord(unsigned int index, rct_xyz16 coord) { }
unsigned int network_get_player_commands_ran(unsigned int index) { return 0; }
int network_get_player_index(uint8 id) { return -1; }
uint8 network_get_player_group(unsigned int index) { return 0; }
void network_set_player_group(unsigned int index, unsigned int groupindex) { }
int network_get_group_index(uint8 id) { return -1; }
uint8 network_get_group_id(unsigned int index) { return 0; }
int network_get_num_groups() { return 0; }
const char* network_get_group_name(unsigned int index) { return ""; };
void game_command_set_player_group(int* eax, int* ebx, int* ecx, int* edx, int* esi, int* edi, int* ebp) { }
void game_command_modify_groups(int* eax, int* ebx, int* ecx, int* edx, int* esi, int* edi, int* ebp) { }
void game_command_kick_player(int* eax, int* ebx, int* ecx, int* edx, int* esi, int* edi, int* ebp) { }
uint8 network_get_default_group() { return 0; }
int network_get_num_actions() { return 0; }
rct_string_id network_get_action_name_string_id(unsigned int index) { return -1; }
int network_can_perform_action(unsigned int groupindex, unsigned int index) { return 0; }
int network_can_perform_command(unsigned int groupindex, unsigned int index) { return 0; }
void network_set_pickup_peep(uint8 playerid, rct_peep* peep) { _pickup_peep = peep; }
rct_peep* network_get_pickup_peep(uint8 playerid) { return _pickup_peep; }
void network_set_pickup_peep_old_x(uint8 playerid, int x) { _pickup_peep_old_x = x; }
int network_get_pickup_peep_old_x(uint8 playerid) { return _pickup_peep_old_x; }
void network_send_chat(const char* text) {}
void network_send_password(const char* password) {}
void network_close() {}
void network_shutdown_client() {}
void network_set_password(const char* password) {}
uint8 network_get_current_player_id() { return 0; }
int network_get_current_player_group_index() { return 0; }
void network_append_chat_log(const utf8 *text) { }
const utf8 * network_get_server_name() { return nullptr; }
const utf8 * network_get_server_description() { return nullptr; }
const utf8 * network_get_server_greeting() { return nullptr; }
const utf8 * network_get_server_provider_name() { return nullptr; }
const utf8 * network_get_server_provider_email() { return nullptr; }
const utf8 * network_get_server_provider_website() { return nullptr; }
#endif /* DISABLE_NETWORK */
