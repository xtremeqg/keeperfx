/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file config_settings.c
 *     List of language-specific strings support.
 * @par Purpose:
 *     Support of configuration files for game strings.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     19 Nov 2011 - 01 Aug 2012
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "config_settings.h"
#include "front_input.h"
#include "globals.h"
#include "sounds.h"
#include "bflib_basics.h"
#include "bflib_fileio.h"
#include "bflib_dernc.h"
#include "bflib_keybrd.h"
#include "bflib_video.h"
#include "frontmenu_options.h"
#include "config.h"
#include "engine_camera.h"
#include "game_merge.h"
#include "vidmode.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
unsigned char i_can_see_levels[] = {30, 45, 60, 254,};
struct GameSettings settings;
/******************************************************************************/
#ifdef __cplusplus
}
#endif
/******************************************************************************/
void setup_default_settings(void)
{
    settings.video_detail_level = 0;
    settings.video_shadows = 4;
    settings.view_distance = 3;
    settings.video_rotate_mode = 0;
    settings.video_textures = 1;
    settings.video_cluedo_mode = 0;
    settings.effects_volume = 127;
    settings.music_volume = 90;
    settings.roomflags_on = 1;
    settings.gamma_correction = 0;
    settings.switching_vidmodes_index = Lb_SCREEN_MODE_INVALID; // auto detect / mark unset
    settings.kbkeys[Gkey_MoveUp] = (struct GameKey) { KC_W, KMod_NONE};
    settings.kbkeys[Gkey_MoveDown] = (struct GameKey) { KC_S, KMod_NONE };
    settings.kbkeys[Gkey_MoveLeft] = (struct GameKey) { KC_A, KMod_NONE };
    settings.kbkeys[Gkey_MoveRight] = (struct GameKey) { KC_D, KMod_NONE };
    settings.kbkeys[Gkey_RotateMod] = (struct GameKey) { KC_LCONTROL, KMod_NONE };
    settings.kbkeys[Gkey_SpeedMod] = (struct GameKey) { KC_LSHIFT, KMod_NONE };
    settings.kbkeys[Gkey_RotateCW] = (struct GameKey) { KC_DELETE, KMod_NONE };
    settings.kbkeys[Gkey_RotateCCW] = (struct GameKey) { KC_PGDOWN, KMod_NONE };
    settings.kbkeys[Gkey_ZoomIn] = (struct GameKey) { KC_HOME, KMod_NONE };
    settings.kbkeys[Gkey_ZoomOut] = (struct GameKey) { KC_END, KMod_NONE };
    settings.kbkeys[Gkey_ZoomRoomTreasure] = (struct GameKey) { KC_T, KMod_NONE };
    settings.kbkeys[Gkey_ZoomRoomLibrary] = (struct GameKey) { KC_L, KMod_NONE };
    settings.kbkeys[Gkey_ZoomRoomLair] = (struct GameKey) { KC_L, KMod_SHIFT };
    settings.kbkeys[Gkey_ZoomRoomPrison] = (struct GameKey) { KC_P, KMod_SHIFT };
    settings.kbkeys[Gkey_ZoomRoomTorture] = (struct GameKey) { KC_T, KMod_ALT };
    settings.kbkeys[Gkey_ZoomRoomTraining] = (struct GameKey) { KC_T, KMod_SHIFT };
    settings.kbkeys[Gkey_ZoomRoomHeart] = (struct GameKey) { KC_H, KMod_NONE };
    settings.kbkeys[Gkey_ZoomRoomWorkshop] = (struct GameKey) { KC_W, KMod_ALT };
    settings.kbkeys[Gkey_ZoomRoomScavenger] = (struct GameKey) { KC_S, KMod_ALT };
    settings.kbkeys[Gkey_ZoomRoomTemple] = (struct GameKey) { KC_T, KMod_CONTROL };
    settings.kbkeys[Gkey_ZoomRoomGraveyard] = (struct GameKey) { KC_G, KMod_NONE };
    settings.kbkeys[Gkey_ZoomRoomBarracks] = (struct GameKey) { KC_B, KMod_NONE };
    settings.kbkeys[Gkey_ZoomRoomHatchery] = (struct GameKey) { KC_H, KMod_SHIFT };
    settings.kbkeys[Gkey_ZoomRoomGuardPost] = (struct GameKey) { KC_G, KMod_SHIFT };
    settings.kbkeys[Gkey_ZoomRoomBridge] = (struct GameKey) { KC_B, KMod_SHIFT };
    settings.kbkeys[Gkey_ZoomRoomPortal] = (struct GameKey) { KC_P, KMod_CONTROL };
    settings.kbkeys[Gkey_ZoomToFight] = (struct GameKey) { KC_F, KMod_NONE };
    settings.kbkeys[Gkey_ZoomCrAnnoyed] = (struct GameKey) { KC_A, KMod_ALT };
    settings.kbkeys[Gkey_CrtrContrlMod] = (struct GameKey) { KC_LSHIFT, KMod_NONE };
    settings.kbkeys[Gkey_CrtrQueryMod] = (struct GameKey) { KC_Q, KMod_NONE };
    settings.kbkeys[Gkey_DumpToOldPos] = (struct GameKey) { KC_BACK, KMod_NONE };
    settings.kbkeys[Gkey_TogglePause] = (struct GameKey) { KC_P, KMod_NONE };
    settings.kbkeys[Gkey_SwitchToMap] = (struct GameKey) { KC_M, KMod_NONE };
    settings.kbkeys[Gkey_ToggleMessage] = (struct GameKey) { KC_E, KMod_NONE };
    settings.kbkeys[Gkey_SnapCamera] = (struct GameKey) { KC_MOUSE3, KMod_NONE };
    settings.kbkeys[Gkey_BestRoomSpace] = (struct GameKey) { KC_LSHIFT, KMod_NONE };
    settings.kbkeys[Gkey_SquareRoomSpace] = (struct GameKey) { KC_LCONTROL, KMod_NONE };
    settings.kbkeys[Gkey_RoomSpaceIncSize] = (struct GameKey) { KC_MOUSEWHEEL_DOWN, KMod_NONE };
    settings.kbkeys[Gkey_RoomSpaceDecSize] = (struct GameKey) { KC_MOUSEWHEEL_UP, KMod_NONE };
    settings.kbkeys[Gkey_SellTrapOnSubtile] = (struct GameKey) { KC_LALT, KMod_NONE };
    settings.kbkeys[Gkey_TiltUp] = (struct GameKey) { KC_PGUP, KMod_SHIFT };
    settings.kbkeys[Gkey_TiltDown] = (struct GameKey) { KC_PGDOWN, KMod_SHIFT };
    settings.kbkeys[Gkey_TiltReset] = (struct GameKey) { KC_INSERT, KMod_SHIFT };
    settings.kbkeys[Gkey_Ascend] = (struct GameKey) { KC_X, KMod_NONE };
    settings.kbkeys[Gkey_Descend] = (struct GameKey) { KC_Z, KMod_NONE };
    settings.tooltips_on = true;
    settings.first_person_move_invert = 0;
    settings.first_person_move_sensitivity = 6;
    settings.minimap_zoom = 256;
    settings.isometric_view_zoom_level = 8192;
    settings.frontview_zoom_level = FRONTVIEW_CAMERA_ZOOM_MAX;
    settings.mentor_volume = 127;
    settings.master_volume = 127;
    settings.isometric_tilt = CAMERA_TILT_DEFAULT;
    save_settings();
    settings.switching_vidmodes_index = 0; // pick the first one
}

TbBool load_settings(void)
{
    SYNCDBG(6,"Starting");
    char* fname = prepare_file_path(FGrp_Save, "settings.dat");
    long len = LbFileLengthRnc(fname);
    if (len == sizeof(struct GameSettings))
    {
      if (LbFileLoadAt(fname, &settings) == sizeof(struct GameSettings))
      {
          // sanity checks
          settings.video_shadows = clamp(settings.video_shadows, 0, 3);
          settings.view_distance = clamp(settings.view_distance, 0, 3);
          settings.video_rotate_mode = clamp(settings.video_rotate_mode, 0, 2);
          settings.video_textures = clamp(settings.video_textures, 0, 1);
          settings.video_cluedo_mode = clamp(settings.video_cluedo_mode, 0, 1);
          settings.effects_volume = clamp(settings.effects_volume, 0, 127);
          settings.music_volume = clamp(settings.music_volume, 0, 127);
          settings.gamma_correction = clamp(settings.gamma_correction, 0, GAMMA_LEVELS_COUNT);
          settings.switching_vidmodes_index = clamp(settings.switching_vidmodes_index, 0, MAX_GAME_VIDMODE_COUNT);
          settings.first_person_move_sensitivity = clamp(settings.first_person_move_sensitivity, 0, 1000);
          settings.minimap_zoom = clamp(settings.minimap_zoom, 256, 2048);
          settings.isometric_view_zoom_level = clamp(settings.isometric_view_zoom_level, CAMERA_ZOOM_MIN, CAMERA_ZOOM_MAX);
          settings.frontview_zoom_level = clamp(settings.frontview_zoom_level, FRONTVIEW_CAMERA_ZOOM_MIN, FRONTVIEW_CAMERA_ZOOM_MAX);
          settings.mentor_volume = clamp(settings.mentor_volume, 0, 127);
          settings.master_volume = 127; // there is currently no way for users to set the master volume
          settings.isometric_tilt = clamp(settings.isometric_tilt, CAMERA_TILT_MIN, CAMERA_TILT_MAX);
          return true;
      }
    }
    setup_default_settings();
    LbFileSaveAt(fname, &settings, sizeof(struct GameSettings));
    return false;
}

short save_settings(void)
{
    char* fname = prepare_file_path(FGrp_Save, "settings.dat");
    LbFileSaveAt(fname, &settings, sizeof(struct GameSettings));
    return true;
}

int get_max_i_can_see_from_settings(void)
{
    return i_can_see_levels[settings.view_distance % 4];
}
/******************************************************************************/
