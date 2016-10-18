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

#include "audio/audio.h"
#include "cheats.h"
#include "config.h"
#include "game.h"
#include "editor.h"
#include "world/footpath.h"
#include "input.h"
#include "localisation/localisation.h"
#include "interface/screenshot.h"
#include "interface/viewport.h"
#include "interface/widget.h"
#include "interface/window.h"
#include "management/finance.h"
#include "management/marketing.h"
#include "management/news_item.h"
#include "management/research.h"
#include "network/network.h"
#include "object.h"
#include "openrct2.h"
#include "peep/peep.h"
#include "peep/staff.h"
#include "platform/platform.h"
#include "rct1.h"
#include "ride/ride.h"
#include "ride/ride_ratings.h"
#include "ride/vehicle.h"
#include "ride/track.h"
#include "ride/track_design.h"
#include "scenario.h"
#include "title.h"
#include "util/sawyercoding.h"
#include "util/util.h"
#include "windows/error.h"
#include "windows/tooltip.h"
#include "world/climate.h"
#include "world/map_animation.h"
#include "world/park.h"
#include "world/scenery.h"
#include "world/sprite.h"
#include "world/water.h"

#define NUMBER_OF_AUTOSAVES_TO_KEEP 9

uint16 gTicksSinceLastUpdate;
uint32 gLastTickCount;
uint8 gGamePaused = 0;
int gGameSpeed = 1;
float gDayNightCycle = 0;
bool gInUpdateCode = false;
int gGameCommandNestLevel;
bool gGameCommandIsNetworked;

uint8 gUnk13CA740;
uint8 gUnk141F568;

#ifdef NO_RCT2
uint32 gCurrentTicks;
#endif

GAME_COMMAND_CALLBACK_POINTER* game_command_callback = 0;
GAME_COMMAND_CALLBACK_POINTER* game_command_callback_table[] = {
	0,
	game_command_callback_ride_construct_new,
	game_command_callback_ride_construct_placed_front,
	game_command_callback_ride_construct_placed_back,
	game_command_callback_ride_remove_track_piece,
	game_command_callback_place_banner,
	game_command_callback_place_ride_entrance_or_exit,
	game_command_callback_hire_new_staff_member,
	game_command_callback_pickup_guest,
	game_command_callback_pickup_staff,
};
int game_command_playerid = -1;

rct_string_id gGameCommandErrorTitle;
rct_string_id gGameCommandErrorText;
uint8 gErrorType;
rct_string_id gErrorStringId;

int game_command_callback_get_index(GAME_COMMAND_CALLBACK_POINTER* callback)
{
	for (int i = 0; i < countof(game_command_callback_table); i++ ) {
		if (game_command_callback_table[i] == callback) {
			return i;
		}
	}
	return 0;
}

GAME_COMMAND_CALLBACK_POINTER* game_command_callback_get_callback(int index)
{
	if (index < countof(game_command_callback_table)) {
		return game_command_callback_table[index];
	}
	return 0;
}

void game_increase_game_speed()
{
	gGameSpeed = min(gConfigGeneral.debugging_tools ? 5 : 4, gGameSpeed + 1);
	if (gGameSpeed == 5)
		gGameSpeed = 8;
	window_invalidate_by_class(WC_TOP_TOOLBAR);
}

void game_reduce_game_speed()
{
	gGameSpeed = max(1, gGameSpeed - 1);
	if (gGameSpeed == 7)
		gGameSpeed = 4;
	window_invalidate_by_class(WC_TOP_TOOLBAR);
}

/**
 *
 *  rct2: 0x0066B5C0 (part of 0x0066B3E8)
 */
void game_create_windows()
{
	window_main_open();
	window_top_toolbar_open();
	window_game_bottom_toolbar_open();
	window_resize_gui(gScreenWidth, gScreenHeight);
}

/**
*
*  rct2: 0x006838BD
*/
void update_palette_effects()
{
	rct_water_type* water_type = (rct_water_type*)object_entry_groups[OBJECT_TYPE_WATER].chunks[0];

	if (gClimateLightningFlash == 1) {
		// change palette to lighter colour during lightning
		int palette = 1532;

		if ((intptr_t)water_type != -1) {
			palette = water_type->image_id;
		}
		rct_g1_element g1_element = g1Elements[palette];
		int xoffset = g1_element.x_offset;
		xoffset = xoffset * 4;
		uint8 *paletteOffset = gGamePalette + xoffset;
		for (int i = 0; i < g1_element.width; i++) {
			paletteOffset[(i * 4) + 0] = -((0xFF - g1_element.offset[(i * 3) + 0]) / 2) - 1;
			paletteOffset[(i * 4) + 1] = -((0xFF - g1_element.offset[(i * 3) + 1]) / 2) - 1;
			paletteOffset[(i * 4) + 2] = -((0xFF - g1_element.offset[(i * 3) + 2]) / 2) - 1;
		}
		platform_update_palette(gGamePalette, 10, 236);
		gClimateLightningFlash++;
	} else {
		if (gClimateLightningFlash == 2) {
			// change palette back to normal after lightning
			int palette = 1532;

			if ((intptr_t)water_type != -1) {
				palette = water_type->image_id;
			}

			rct_g1_element g1_element = g1Elements[palette];
			int xoffset = g1_element.x_offset;
			xoffset = xoffset * 4;
			uint8 *paletteOffset = gGamePalette + xoffset;
			for (int i = 0; i < g1_element.width; i++) {
				paletteOffset[(i * 4) + 0] = g1_element.offset[(i * 3) + 0];
				paletteOffset[(i * 4) + 1] = g1_element.offset[(i * 3) + 1];
				paletteOffset[(i * 4) + 2] = g1_element.offset[(i * 3) + 2];
			}
		}

		// animate the water/lava/chain movement palette
		int q = 0;
		extern const sint32 WeatherColours[4];
		int weather_colour = WeatherColours[gClimateCurrentWeatherGloom];
		if (weather_colour != -1 && gConfigGeneral.render_weather_gloom) {
			q = 1;
			if (weather_colour != 0x2000031) {
				q = 2;
			}
		}
		uint32 j = gPaletteEffectFrame;
		j = (((uint16)((~j / 2) * 128) * 15) >> 16);
		int p = 1533;
		if ((intptr_t)water_type != -1) {
			p = water_type->var_06;
		}
		rct_g1_element g1_element = g1Elements[q + p];
		uint8* vs = &g1_element.offset[j * 3];
		uint8* vd = &gGamePalette[230 * 4];
		int n = 5;
		for (int i = 0; i < n; i++) {
			vd[0] = vs[0];
			vd[1] = vs[1];
			vd[2] = vs[2];
			vs += 9;
			if (vs >= &g1_element.offset[9 * n]) {
				vs -= 9 * n;
			}
			vd += 4;
		}

		p = 1536;
		if ((intptr_t)water_type != -1) {
			p = water_type->var_0A;
		}
		g1_element = g1Elements[q + p];
		vs = &g1_element.offset[j * 3];
		n = 5;
		for (int i = 0; i < n; i++) {
			vd[0] = vs[0];
			vd[1] = vs[1];
			vd[2] = vs[2];
			vs += 9;
			if (vs >= &g1_element.offset[9 * n]) {
				vs -= 9 * n;
			}
			vd += 4;
		}

		j = ((uint16)(gPaletteEffectFrame * -960) * 3) >> 16;
		p = 1539;
		g1_element = g1Elements[q + p];
		vs = &g1_element.offset[j * 3];
		vd += 12;
		n = 3;
		for (int i = 0; i < n; i++) {
			vd[0] = vs[0];
			vd[1] = vs[1];
			vd[2] = vs[2];
			vs += 3;
			if (vs >= &g1_element.offset[3 * n]) {
				vs -= 3 * n;
			}
			vd += 4;
		}

		platform_update_palette(gGamePalette, 230, 16);
		if (gClimateLightningFlash == 2) {
			platform_update_palette(gGamePalette, 10, 236);
			gClimateLightningFlash = 0;
		}
	}
}

void game_update()
{
	int i, numUpdates;

	// 0x006E3AEC // screen_game_process_mouse_input();
	screenshot_check();
	game_handle_keyboard_input();

	// Determine how many times we need to update the game
	if (gGameSpeed > 1) {
		numUpdates = 1 << (gGameSpeed - 1);
	} else {
		numUpdates = gTicksSinceLastUpdate / 31;
		numUpdates = clamp(1, numUpdates, 4);
	}

	if (network_get_mode() == NETWORK_MODE_CLIENT && network_get_status() == NETWORK_STATUS_CONNECTED && network_get_authstatus() == NETWORK_AUTH_OK) {
		if (network_get_server_tick() - gCurrentTicks >= 10) {
			// make sure client doesn't fall behind the server too much
			numUpdates += 10;
		}
	}

	if (game_is_paused()) {
		numUpdates = 0;
		// Update the animation list. Note this does not
		// increment the map animation.
		map_animation_invalidate_all();
	}

	// Update the game one or more times
	for (i = 0; i < numUpdates; i++) {
		game_logic_update();

		if (gGameSpeed > 1)
			continue;

		if (gInputState == INPUT_STATE_RESET ||
			gInputState == INPUT_STATE_NORMAL
		) {
			if (gInputFlags & INPUT_FLAG_VIEWPORT_SCROLLING) {
				gInputFlags &= ~INPUT_FLAG_VIEWPORT_SCROLLING;
				break;
			}
		} else {
			break;
		}
	}

	// Always perform autosave check, even when paused
	scenario_autosave_check();

	window_dispatch_update_all();

	gGameCommandNestLevel = 0;

	gInputFlags &= ~INPUT_FLAG_VIEWPORT_SCROLLING;

	// the flickering frequency is reduced by 4, compared to the original
	// it was done due to inability to reproduce original frequency
	// and decision that the original one looks too fast
	if (gCurrentTicks % 4 == 0)
		gWindowMapFlashingFlags ^= (1 << 15);

	// Handle guest map flashing
	gWindowMapFlashingFlags &= ~(1 << 1);
	if (gWindowMapFlashingFlags & (1 << 0))
		gWindowMapFlashingFlags |= (1 << 1);
	gWindowMapFlashingFlags &= ~(1 << 0);

	// Handle staff map flashing
	gWindowMapFlashingFlags &= ~(1 << 3);
	if (gWindowMapFlashingFlags & (1 << 2))
		gWindowMapFlashingFlags |= (1 << 3);
	gWindowMapFlashingFlags &= ~(1 << 2);

	window_map_tooltip_update_visibility();

	// Input
	gUnk141F568 = gUnk13CA740;
	game_handle_input();
}

void game_logic_update()
{
	///////////////////////////
	gInUpdateCode = true;
	///////////////////////////
	network_update();
	if (network_get_mode() == NETWORK_MODE_CLIENT && network_get_status() == NETWORK_STATUS_CONNECTED && network_get_authstatus() == NETWORK_AUTH_OK) {
		if (gCurrentTicks >= network_get_server_tick()) {
			// don't run past the server
			return;
		}
	}
	gCurrentTicks++;
	gScenarioTicks++;
	gScreenAge++;
	if (gScreenAge == 0)
		gScreenAge--;

	sub_68B089();
	scenario_update();
	climate_update();
	map_update_tiles();
	// Temporarily remove provisional paths to prevent peep from interacting with them
	map_remove_provisional_elements();
	map_update_path_wide_flags();
	peep_update_all();
	map_restore_provisional_elements();
	vehicle_update_all();
	sprite_misc_update_all();
	ride_update_all();
	park_update();
	research_update();
	ride_ratings_update_all();
	ride_measurements_update();
	news_item_update_current();
	///////////////////////////
	gInUpdateCode = false;
	///////////////////////////

	map_animation_invalidate_all();
	vehicle_sounds_update();
	peep_update_crowd_noise();
	climate_update_sound();
	editor_open_windows_for_current_step();

	gSavedAge++;

	// Update windows
	//window_dispatch_update_all();

	if (gErrorType != ERROR_TYPE_NONE) {
		rct_string_id title_text = STR_UNABLE_TO_LOAD_FILE;
		rct_string_id body_text = gErrorStringId;
		if (gErrorType == ERROR_TYPE_GENERIC) {
			title_text = gErrorStringId;
			body_text = 0xFFFF;
		}
		gErrorType = ERROR_TYPE_NONE;

		window_error_open(title_text, body_text);
	}
}

/**
 *
 *  rct2: 0x0069C62C
 *
 * @param cost (ebp)
 */
static int game_check_affordability(int cost)
{
	if (cost <= 0)return cost;
	if (gUnk141F568 & 0xF0) return cost;
	if (cost <= (sint32)(DECRYPT_MONEY(gCashEncrypted)))return cost;

	set_format_arg(0, uint32, cost);

	gGameCommandErrorText = STR_NOT_ENOUGH_CASH_REQUIRES;
	return MONEY32_UNDEFINED;
}

/**
 *
 *  rct2: 0x006677F2
 *
 * @param flags (ebx)
 * @param command (esi)
 */
int game_do_command(int eax, int ebx, int ecx, int edx, int esi, int edi, int ebp)
{
	return game_do_command_p(esi, &eax, &ebx, &ecx, &edx, &esi, &edi, &ebp);
}

/**
*
*  rct2: 0x006677F2 with pointers as arguments
*
* @param flags (ebx)
* @param command (esi)
*/
int game_do_command_p(int command, int *eax, int *ebx, int *ecx, int *edx, int *esi, int *edi, int *ebp)
{
	int cost, flags, insufficientFunds;
	int original_ebx, original_edx, original_esi, original_edi, original_ebp;

	*esi = command;
	original_ebx = *ebx;
	original_edx = *edx;
	original_esi = *esi;
	original_edi = *edi;
	original_ebp = *ebp;

	if (command >= countof(new_game_command_table)) {
		return MONEY32_UNDEFINED;
	}

	flags = *ebx;

	if (gGameCommandNestLevel == 0) {
		gGameCommandErrorText = STR_NONE;
		gGameCommandIsNetworked = (flags & GAME_COMMAND_FLAG_NETWORKED) != 0;
	}
	
	// Increment nest count
	gGameCommandNestLevel++;

	// Remove ghost scenery so it doesn't interfere with incoming network command
	if ((flags & GAME_COMMAND_FLAG_NETWORKED) && !(flags & GAME_COMMAND_FLAG_GHOST) &&
		(command == GAME_COMMAND_PLACE_FENCE ||
		command == GAME_COMMAND_PLACE_SCENERY ||
		command == GAME_COMMAND_PLACE_LARGE_SCENERY ||
		command == GAME_COMMAND_PLACE_BANNER ||
		command == GAME_COMMAND_PLACE_PATH)) {
		scenery_remove_ghost_tool_placement();
	}

	if (game_command_playerid == -1) {
		game_command_playerid = network_get_current_player_id();
	}

	*ebx &= ~GAME_COMMAND_FLAG_APPLY;

	// First call for validity and price check
	new_game_command_table[command](eax, ebx, ecx, edx, esi, edi, ebp);
	cost = *ebx;

	if (cost != MONEY32_UNDEFINED) {
		// Check funds
		insufficientFunds = 0;
		if (gGameCommandNestLevel == 1 && !(flags & GAME_COMMAND_FLAG_2) && !(flags & GAME_COMMAND_FLAG_5) && cost != 0)
			insufficientFunds = game_check_affordability(cost);

		if (insufficientFunds != MONEY32_UNDEFINED) {
			*ebx = original_ebx;
			*edx = original_edx;
			*esi = original_esi;
			*edi = original_edi;
			*ebp = original_ebp;

			if (!(flags & GAME_COMMAND_FLAG_APPLY)) {
				// Decrement nest count
				gGameCommandNestLevel--;
				return cost;
			}

			if (network_get_mode() != NETWORK_MODE_NONE && !(flags & GAME_COMMAND_FLAG_NETWORKED) && !(flags & GAME_COMMAND_FLAG_GHOST) && !(flags & GAME_COMMAND_FLAG_5) && gGameCommandNestLevel == 1 /* Send only top-level commands */) {
				if (command != GAME_COMMAND_LOAD_OR_QUIT) { // Disable these commands over the network
					network_send_gamecmd(*eax, *ebx, *ecx, *edx, *esi, *edi, *ebp, game_command_callback_get_index(game_command_callback));
					if (network_get_mode() == NETWORK_MODE_CLIENT) { // Client sent the command to the server, do not run it locally, just return.  It will run when server sends it
						game_command_callback = 0;
						// Decrement nest count
						gGameCommandNestLevel--;
						return cost;
					}
				}
			}

			// Second call to actually perform the operation
			new_game_command_table[command](eax, ebx, ecx, edx, esi, edi, ebp);

			// Do the callback (required for multiplayer to work correctly), but only for top level commands
			if (gGameCommandNestLevel == 1) {
				if (game_command_callback && !(flags & GAME_COMMAND_FLAG_GHOST)) {
					game_command_callback(*eax, *ebx, *ecx, *edx, *esi, *edi, *ebp);
					game_command_callback = 0;
				}
			}

			game_command_playerid = -1;

			*edx = *ebx;

			if (*edx != MONEY32_UNDEFINED && *edx < cost)
				cost = *edx;

			// Decrement nest count
			gGameCommandNestLevel--;
			if (gGameCommandNestLevel != 0)
				return cost;

			//
			if (!(flags & 0x20)) {
				// Update money balance
				finance_payment(cost, gCommandExpenditureType);
				if (gUnk141F568 == gUnk13CA740) {
					// Create a +/- money text effect
					if (cost != 0)
						money_effect_create(cost);
				}
			}

			if (network_get_mode() == NETWORK_MODE_SERVER && !(flags & GAME_COMMAND_FLAG_NETWORKED) && !(flags & GAME_COMMAND_FLAG_GHOST)) {
				network_set_player_last_action(network_get_player_index(network_get_current_player_id()), command);
				network_add_player_money_spent(network_get_current_player_id(), cost);
			}

			return cost;
		}
	}

	// Error occurred

	// Decrement nest count
	gGameCommandNestLevel--;
	
	// Clear the game command callback to prevent the next command triggering it
	game_command_callback = 0;

	// Show error window
	if (gGameCommandNestLevel == 0 && (flags & GAME_COMMAND_FLAG_APPLY) && gUnk141F568 == gUnk13CA740 && !(flags & GAME_COMMAND_FLAG_ALLOW_DURING_PAUSED) && !(flags & GAME_COMMAND_FLAG_NETWORKED))
		window_error_open(gGameCommandErrorTitle, gGameCommandErrorText);

	return MONEY32_UNDEFINED;
}

void pause_toggle()
{
	gGamePaused ^= GAME_PAUSED_NORMAL;
	window_invalidate_by_class(WC_TOP_TOOLBAR);
	if (gGamePaused & GAME_PAUSED_NORMAL) {
		audio_pause_sounds();
		audio_unpause_sounds();
	} else {
		audio_unpause_sounds();
	}
}

bool game_is_paused()
{
	return gGamePaused != 0;
}

bool game_is_not_paused()
{
	return gGamePaused == 0;
}

/**
 *
 *  rct2: 0x00667C15
 */
void game_pause_toggle(int *eax, int *ebx, int *ecx, int *edx, int *esi, int *edi, int *ebp)
{
	if (*ebx & GAME_COMMAND_FLAG_APPLY)
		pause_toggle();

	*ebx = 0;
}

/**
 *
 *  rct2: 0x0066DB5F
 */
static void game_load_or_quit(int *eax, int *ebx, int *ecx, int *edx, int *esi, int *edi, int *ebp)
{
	if (*ebx & GAME_COMMAND_FLAG_APPLY) {
		switch (*edx & 0xFF) {
		case 0:
			gSavePromptMode = *edi & 0xFF;
			window_save_prompt_open();
			break;
		case 1:
			window_close_by_class(WC_SAVE_PROMPT);
			break;
		default:
			game_load_or_quit_no_save_prompt();
			break;
		}
	}
	*ebx = 0;
}

/**
 *
 *  rct2: 0x0066DC0F
 */
static void load_landscape()
{
	window_loadsave_open(LOADSAVETYPE_LOAD | LOADSAVETYPE_LANDSCAPE, NULL);
}

static void utf8_to_rct2_self(char *buffer, size_t length)
{
	char tempBuffer[512];
	utf8_to_rct2(tempBuffer, buffer);
	
	size_t i = 0;
	const char *src = tempBuffer;
	char *dst = buffer;
	while (*src != 0 && i < length - 1) {
		if (*src == (char)0xFF) {
			if (i < length - 3) {
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
			} else {
				break;
			}
			i += 3;
		} else {
			*dst++ = *src++;
			i++;
		}
	}
	do {
		*dst++ = '\0';
		i++;
	} while (i < length);
}

static void rct2_to_utf8_self(char *buffer, size_t length)
{
	char tempBuffer[512];
	if (length > 0) {
		rct2_to_utf8(tempBuffer, buffer);
		safe_strcpy(buffer, tempBuffer, length);
	}
}

/**
 * Converts all the user strings and news item strings to UTF-8.
 */
void game_convert_strings_to_utf8()
{
	// Scenario details
	rct2_to_utf8_self(gScenarioCompletedBy, 32);
	rct2_to_utf8_self(gScenarioName, 64);
	rct2_to_utf8_self(gScenarioDetails, 256);

	// User strings
	for (int i = 0; i < MAX_USER_STRINGS; i++) {
		utf8 *userString = &gUserStrings[i * USER_STRING_MAX_LENGTH];

		if (!str_is_null_or_empty(userString)) {
			rct2_to_utf8_self(userString, 32);
			utf8_remove_formatting(userString, true);
		}
	}

	// News items
	for (int i = 0; i < MAX_NEWS_ITEMS; i++) {
		rct_news_item *newsItem = news_item_get(i);

		if (!str_is_null_or_empty(newsItem->text)) {
			rct2_to_utf8_self(newsItem->text, 256);
		}
	}
}

/**
 * Converts all the user strings and news item strings to RCT2 encoding.
 */
void game_convert_strings_to_rct2(rct_s6_data *s6)
{
	// Scenario details
	utf8_to_rct2_self(s6->scenario_completed_name, sizeof(s6->scenario_completed_name));
	utf8_to_rct2_self(s6->scenario_name, sizeof(s6->scenario_name));
	utf8_to_rct2_self(s6->scenario_description, sizeof(s6->scenario_description));

	// User strings
	for (int i = 0; i < MAX_USER_STRINGS; i++) {
		char *userString = &s6->custom_strings[i * USER_STRING_MAX_LENGTH];

		if (!str_is_null_or_empty(userString)) {
			utf8_to_rct2_self(userString, 32);
		}
	}

	// News items
	for (int i = 0; i < MAX_NEWS_ITEMS; i++) {
		rct_news_item *newsItem = &s6->news_items[i];

		if (!str_is_null_or_empty(newsItem->text)) {
			utf8_to_rct2_self(newsItem->text, 256);
		}
	}
}

// OpenRCT2 workaround to recalculate some values which are saved redundantly in the save to fix corrupted files.
// For example recalculate guest count by looking at all the guests instead of trusting the value in the file.
void game_fix_save_vars() {

	// Recalculates peep count after loading a save to fix corrupted files
	rct_peep* peep;
	uint16 spriteIndex;
	uint16 peepCount = 0;
	FOR_ALL_GUESTS(spriteIndex, peep) {
		if(!peep->outside_of_park)
			peepCount++;
	}

	gNumGuestsInPark = peepCount;

	peep_sort();

	// Fixes broken saves where a surface element could be null
	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			rct_map_element *mapElement = map_get_surface_element_at(x, y);

			if (mapElement == NULL)
			{
				log_error("Null map element at x = %d and y = %d. Fixing...", x, y);
				mapElement = map_element_insert(x, y, 14, 0);
				if (mapElement == NULL) {
					log_error("Unable to fix: Map element limit reached.");
					return;
				}
			}
		}
	}

	// Fix invalid research items
	for (int i = 0; i < 500; i++) {
		rct_research_item *researchItem = &gResearchItems[i];
		if (researchItem->entryIndex == RESEARCHED_ITEMS_SEPARATOR) continue;
		if (researchItem->entryIndex == RESEARCHED_ITEMS_END) continue;
		if (researchItem->entryIndex == RESEARCHED_ITEMS_END_2) break;
		if (researchItem->entryIndex & 0x10000) {
			uint8 entryIndex = researchItem->entryIndex & 0xFF;
			rct_ride_entry *rideEntry = get_ride_entry(entryIndex);
			if (rideEntry == NULL || rideEntry == (rct_ride_entry*)-1) {
				research_remove(researchItem->entryIndex);
				i--;
			}
		} else {
			uint8 entryIndex = researchItem->entryIndex;
			rct_scenery_set_entry *sceneryGroupEntry = get_scenery_group_entry(entryIndex);
			if (sceneryGroupEntry == NULL || sceneryGroupEntry == (rct_scenery_set_entry*)-1) {
				research_remove(researchItem->entryIndex);
				i--;
			}
		}
	}
}

/**
 *
 *  rct2: 0x00675E1B
 */
bool game_load_save(const utf8 *path)
{
	log_verbose("loading saved game, %s", path);

	safe_strcpy((char*)gRCT2AddressSavedGamesPath2, path, MAX_PATH);

	safe_strcpy(gScenarioSavePath, path, MAX_PATH);

	SDL_RWops* rw = SDL_RWFromFile(path, "rb");
	if (rw == NULL) {
		log_error("unable to open %s", path);
		gErrorType = ERROR_TYPE_FILE_LOAD;
		gGameCommandErrorTitle = STR_FILE_CONTAINS_INVALID_DATA;
		return false;
	}

	uint32 extension_type = get_file_extension_type(path);
	bool result = false;

	if (extension_type == FILE_EXTENSION_SV6) {
		result = game_load_sv6(rw);
	} else if (extension_type == FILE_EXTENSION_SV4) {
		result = rct1_load_saved_game(path);
		if (result)
			gFirstTimeSave = 1;
	}

	SDL_RWclose(rw);

	if (result) {
		if (network_get_mode() == NETWORK_MODE_CLIENT) {
			network_close();
		}
		game_load_init();
		if (network_get_mode() == NETWORK_MODE_SERVER) {
			network_send_map();
		}
		return true;
	} else {
		// If loading the SV6 or SV4 failed, the current park state will be corrupted
		// so just go back to the title screen.
		title_load();
		return false;
	}
}

void game_load_init()
{
	rct_window *mainWindow;

	gScreenFlags = SCREEN_FLAGS_PLAYING;
	audio_stop_all_music_and_sounds();
	viewport_init_all();
	game_create_windows();
	mainWindow = window_get_main();

	mainWindow->viewport_target_sprite = -1;
	mainWindow->saved_view_x = gSavedViewX;
	mainWindow->saved_view_y = gSavedViewY;
	uint8 zoomDifference = gSavedViewZoom - mainWindow->viewport->zoom;
	mainWindow->viewport->zoom = gSavedViewZoom;
	gCurrentRotation = gSavedViewRotation;
	if (zoomDifference != 0) {
		if (zoomDifference < 0) {
			zoomDifference = -zoomDifference;
			mainWindow->viewport->view_width >>= zoomDifference;
			mainWindow->viewport->view_height >>= zoomDifference;
		} else {
			mainWindow->viewport->view_width <<= zoomDifference;
			mainWindow->viewport->view_height <<= zoomDifference;
		}
	}
	mainWindow->saved_view_x -= mainWindow->viewport->view_width >> 1;
	mainWindow->saved_view_y -= mainWindow->viewport->view_height >> 1;
	window_invalidate(mainWindow);

	if (network_get_mode() != NETWORK_MODE_CLIENT)
	{
		reset_sprite_spatial_index();
	}
	reset_all_sprite_quadrant_placements();
	scenery_set_default_placement_configuration();
	window_new_ride_init_vars();
	gWindowUpdateTicks = 0;

	load_palette();
	gfx_invalidate_screen();
	window_update_all();

	gGameSpeed = 1;
}

/**
 *
 *  rct2: 0x0069E9A7
 * Call after a rotation or loading of a save to reset sprite quadrants
 */
void reset_all_sprite_quadrant_placements()
{
	for (size_t i = 0; i < MAX_SPRITES; i++) {
		rct_sprite *spr = get_sprite(i);
		if (spr->unknown.sprite_identifier != SPRITE_IDENTIFIER_NULL) {
			sprite_move(spr->unknown.x, spr->unknown.y, spr->unknown.z, spr);
		}
	}
}

void save_game()
{
	if (!gFirstTimeSave) {
		log_verbose("Saving to %s", gScenarioSavePath);

		SDL_RWops* rw = SDL_RWFromFile(gScenarioSavePath, "wb+");
		if (rw != NULL) {
			scenario_save(rw, 0x80000000 | (gConfigGeneral.save_plugin_data ? 1 : 0));
			log_verbose("Saved to %s", gScenarioSavePath);
			SDL_RWclose(rw);

			// Setting screen age to zero, so no prompt will pop up when closing the
			// game shortly after saving.
			gScreenAge = 0;
		}
	} else {
		save_game_as();
	}
}
void save_game_as()
{
	char name[MAX_PATH];
	safe_strcpy(name, path_get_filename(gScenarioSavePath), MAX_PATH);
	path_remove_extension(name);
	window_loadsave_open(LOADSAVETYPE_SAVE | LOADSAVETYPE_GAME, name);
}

static int compare_autosave_file_paths (const void * a, const void * b ) {
	return strcmp(*(char **)a, *(char **)b);
}

static void limit_autosave_count(const size_t numberOfFilesToKeep)
{
	int fileEnumHandle = 0;

	size_t autosavesCount = 0;
	size_t numAutosavesToDelete = 0;

	file_info fileInfo;
	
	utf8 filter[MAX_PATH];
	
	utf8 **autosaveFiles = NULL;
	
	size_t i=0;
	
	platform_get_user_directory(filter, "save", sizeof(filter));
	safe_strcat_path(filter, "autosave_*.sv6", sizeof(filter));
	
	// At first, count how many autosaves there are
	fileEnumHandle = platform_enumerate_files_begin(filter);
	while (platform_enumerate_files_next(fileEnumHandle, &fileInfo)) {
		autosavesCount++;
	}
	platform_enumerate_files_end(fileEnumHandle);
	
	// If there are fewer autosaves than the number of files to keep we don't need to delete anything
	if(autosavesCount <= numberOfFilesToKeep) {
		return;
	}
	
	autosaveFiles = (utf8**) malloc(sizeof(utf8*) * autosavesCount);
	
	fileEnumHandle = platform_enumerate_files_begin(filter);
	for(i = 0; i < autosavesCount; i++) {
		autosaveFiles[i] = (utf8*)malloc(sizeof(utf8) * MAX_PATH);
		memset(autosaveFiles[i], 0, sizeof(utf8) * MAX_PATH);
		
		if(platform_enumerate_files_next(fileEnumHandle, &fileInfo)) {
			platform_get_user_directory(autosaveFiles[i], "save", sizeof(utf8) * MAX_PATH);
			safe_strcat_path(autosaveFiles[i], fileInfo.path, sizeof(utf8) * MAX_PATH);
		}
	}
	platform_enumerate_files_end(fileEnumHandle);

	qsort (autosaveFiles, autosavesCount, sizeof (char*), compare_autosave_file_paths);

	// calculate how many saves we need to delete.
	numAutosavesToDelete = autosavesCount - numberOfFilesToKeep;
	
	i=0;
	while (numAutosavesToDelete > 0) {
		platform_file_delete(autosaveFiles[i]);
		
		i++;
		numAutosavesToDelete--;
	}
	
	
	for(i = 0; i < autosavesCount; i++) {
		free(autosaveFiles[i]);
	}
	
	free(autosaveFiles);
}

void game_autosave()
{
	utf8 path[MAX_PATH];
	utf8 backupPath[MAX_PATH];
	utf8 timeName[34];

	// retrieve current time
	rct2_date currentDate;
	platform_get_date_local(&currentDate);
	rct2_time currentTime;
	platform_get_time_local(&currentTime);

	snprintf(timeName, 34, "autosave_%d-%02d-%02d_%02d-%02d-%02d.sv6", currentDate.year, currentDate.month, currentDate.day, currentTime.hour, currentTime.minute,currentTime.second);

	limit_autosave_count(NUMBER_OF_AUTOSAVES_TO_KEEP);

	platform_get_user_directory(path, "save", sizeof(path));
	safe_strcpy(backupPath, path, sizeof(backupPath));

	safe_strcat_path(path, timeName, sizeof(path));
	safe_strcat_path(backupPath, "autosave.sv6.bak", sizeof(backupPath));

	if (platform_file_exists(path)) {
		platform_file_copy(path, backupPath, true);
	}

	SDL_RWops* rw = SDL_RWFromFile(path, "wb+");
	if (rw != NULL) {
		scenario_save(rw, 0x80000000);
		SDL_RWclose(rw);
	}
}

/**
*
*  rct2: 0x006E3838
*/
void rct2_exit_reason(rct_string_id title, rct_string_id body){
	// Before this would set a quit message

	char exit_title[255];
	format_string(exit_title, 256, title, 0);

	char exit_body[255];
	format_string(exit_body, 256, body, 0);

	log_error(exit_title);
	log_error(exit_body);

	rct2_exit();
}


/**
 *
 *  rct2: 0x006E3879
 */
void rct2_exit()
{
	openrct2_finish();
}

/**
 *
 *  rct2: 0x0066DB79
 */
void game_load_or_quit_no_save_prompt()
{
	switch (gSavePromptMode) {
	case PM_SAVE_BEFORE_LOAD:
		game_do_command(0, 1, 0, 1, GAME_COMMAND_LOAD_OR_QUIT, 0, 0);
		tool_cancel();
		if (gScreenFlags & SCREEN_FLAGS_SCENARIO_EDITOR) {
			load_landscape();
		} else {
			window_loadsave_open(LOADSAVETYPE_LOAD | LOADSAVETYPE_GAME, NULL);
		}
		break;
	case PM_SAVE_BEFORE_QUIT:
		game_do_command(0, 1, 0, 1, GAME_COMMAND_LOAD_OR_QUIT, 0, 0);
		tool_cancel();
		if (gInputFlags & INPUT_FLAG_5) {
			gInputFlags &= ~INPUT_FLAG_5;
		}
		gGameSpeed = 1;
		gFirstTimeSave = 1;
		title_load();
		break;
	default:
		rct2_exit();
		break;
	}
}

GAME_COMMAND_POINTER* new_game_command_table[GAME_COMMAND_COUNT] = {
	game_command_set_ride_appearance,
	game_command_set_land_height,
	game_pause_toggle,
	game_command_place_track,
	game_command_remove_track,
	game_load_or_quit,
	game_command_create_ride,
	game_command_demolish_ride,
	game_command_set_ride_status,
	game_command_set_ride_vehicles,
	game_command_set_ride_name,
	game_command_set_ride_setting,
	game_command_place_ride_entrance_or_exit,
	game_command_remove_ride_entrance_or_exit,
	game_command_remove_scenery,
	game_command_place_scenery,
	game_command_set_water_height,
	game_command_place_footpath,
	game_command_place_footpath_from_track,
	game_command_remove_footpath,
	game_command_change_surface_style,
	game_command_set_ride_price,
	game_command_set_guest_name,
	game_command_set_staff_name,
	game_command_raise_land,
	game_command_lower_land,
	game_command_smooth_land,
	game_command_raise_water,
	game_command_lower_water,
	game_command_set_brakes_speed,
	game_command_hire_new_staff_member,
	game_command_set_staff_patrol,
	game_command_fire_staff_member,
	game_command_set_staff_order,
	game_command_set_park_name,
	game_command_set_park_open,
	game_command_buy_land_rights,
	game_command_place_park_entrance,
	game_command_remove_park_entrance,
	game_command_set_maze_track,
	game_command_set_park_entrance_fee,
	game_command_update_staff_colour,
	game_command_place_fence,
	game_command_remove_fence,
	game_command_place_large_scenery,
	game_command_remove_large_scenery,
	game_command_set_current_loan,
	game_command_set_research_funding,
	game_command_place_track_design,
	game_command_start_campaign,
	game_command_place_maze_design,
	game_command_place_banner,
	game_command_remove_banner,
	game_command_set_scenery_colour,
	game_command_set_fence_colour,
	game_command_set_large_scenery_colour,
	game_command_set_banner_colour,
	game_command_set_land_ownership,
	game_command_clear_scenery,
	game_command_set_banner_name,
	game_command_set_sign_name,
	game_command_set_banner_style,
	game_command_set_sign_style,
	game_command_set_player_group,
	game_command_modify_groups,
	game_command_kick_player,
	game_command_cheat,
	game_command_pickup_guest,
	game_command_pickup_staff,
	game_command_balloon_press,
};
