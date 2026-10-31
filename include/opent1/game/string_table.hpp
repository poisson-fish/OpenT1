#pragma once

#include <opent1/core/types.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::game {

// -----------------------------------------------------------------------------
// Legacy Darkstar / Fear Object & Set IDs and Types
// -----------------------------------------------------------------------------
inline constexpr sim::SimObjectId SimActionHandlerId = sim::ActionHandlerId;
inline constexpr sim::SimObjectId SimResourceObjectId = sim::ResourceObjectId;
inline constexpr sim::SimObjectId SimCanvasSetId = sim::CanvasSetId;
inline constexpr sim::SimObjectId SimLookupSetId = sim::LookupSetId;
inline constexpr sim::SimObjectId SimFrameEndNotifySetId = sim::FrameEndNotifySetId;
inline constexpr sim::SimObjectId SimRootContainerId = sim::RootContainerId;
inline constexpr sim::SimObjectId SimTerrainId = sim::TerrainId;
inline constexpr sim::SimObjectId SimCameraSetId = sim::CameraSetId;
inline constexpr sim::SimObjectId SimTimerSetId = sim::TimerSetId;
inline constexpr sim::SimObjectId SimRenderSetId = sim::RenderSetId;
inline constexpr sim::SimObjectId SimLightSetId = sim::LightSetId;
inline constexpr sim::SimObjectId SimCameraMountSetId = sim::CameraMountSetId;
inline constexpr sim::SimObjectId SimMissionGroupId = 28;
inline constexpr sim::SimObjectId SimCleanupSetId = 40;
inline constexpr sim::SimObjectId StarFieldId = 515;

inline constexpr UInt32 SimSkyType = SimDefaultObjectType;
inline constexpr UInt32 SimStarsType = SimDefaultObjectType;
inline constexpr UInt32 SimPlanetType = SimDefaultObjectType;

// -----------------------------------------------------------------------------
// Editor & Inspector Tag IDs (editor.strings.h / commoneditor.strings.h / fear.strings.h)
// -----------------------------------------------------------------------------
inline constexpr Int32 IDITG_SIMOBJECT_NAME = 110066;
inline constexpr Int32 IDITG_BMP_FILE_NAME = 110065;
inline constexpr Int32 IDITG_VISIBLE = 1100011;
inline constexpr Int32 IDITG_ACTIVE = 1100012;
inline constexpr Int32 IDITG_MATERIAL_LIST_TAG = 1100036;
inline constexpr Int32 IDITG_DML_FILE_NAME = 1100055;
inline constexpr Int32 IDITG_SKY_ROTATION = 1100200;
inline constexpr Int32 IDITG_SKY_TEXTUREINDEX_1 = 1100201;
inline constexpr Int32 IDITG_SKY_BACKGROUNDCOLOR = 1100218;
inline constexpr Int32 IDITG_SKY_AZIMUTH = 1100220;
inline constexpr Int32 IDITG_SKY_INCIDENCE = 1100221;
inline constexpr Int32 IDITG_SKY_SHADOWS = 1100222;
inline constexpr Int32 IDITG_SKY_LENSFLARE = 1100223;
inline constexpr Int32 IDITG_SKY_LIGHTINTENSITY = 1100224;
inline constexpr Int32 IDITG_SKY_LIGHTAMBIENT = 1100225;
inline constexpr Int32 IDITG_SKY_BRIGHTSTAR = 1100230;
inline constexpr Int32 IDITG_SKY_MEDIUMSTAR = 1100231;
inline constexpr Int32 IDITG_SKY_DIMSTAR = 1100232;
inline constexpr Int32 IDITG_SKY_NOSTARS = 1100233;
inline constexpr Int32 IDITG_SIZE = 1100234;
inline constexpr Int32 IDITG_DISTANCE = 1100235;
inline constexpr Int32 IDITG_STARS_IN_FRONT = 1100236;
inline constexpr Int32 IDITG_POSITION = 1100338;
inline constexpr Int32 IDITG_ROTATION = 1100339;
inline constexpr Int32 IDITG_FILENAME = 1100340;
inline constexpr Int32 IDITG_ISCONTAINER = 1100341;

inline constexpr Int32 IDCOMEDIT_ITG_DIMENSIONS = 1010004;

// Aliases used by Sky, StarField, Planet, Snowfall, Marker, Trigger, DropPointGroup
inline constexpr Int32 IDITG_SKY_DML_NAME = IDITG_DML_FILE_NAME;
inline constexpr Int32 IDITG_SKY_MATLIST_TAG = IDITG_MATERIAL_LIST_TAG;
inline constexpr Int32 IDITG_SKY_SOLID_COLOR = IDITG_SKY_BACKGROUNDCOLOR;
inline constexpr Int32 IDITG_SKY_HAZE_COLOR = IDITG_SKY_BACKGROUNDCOLOR + 1;
inline constexpr Int32 IDITG_SKY_SIZE = IDITG_SIZE;
inline constexpr Int32 IDITG_SKY_FEATURE_POS = IDITG_SKY_ROTATION;
inline constexpr Int32 IDITG_SKY_ROT_0 = IDITG_SKY_TEXTUREINDEX_1;

inline constexpr Int32 IDITG_STARS_VISIBLE = IDITG_VISIBLE;
inline constexpr Int32 IDITG_STARS_BOTTOM_VISIBLE = IDITG_SKY_NOSTARS;
inline constexpr Int32 IDITG_STARS_FRONT_OF_SKY = IDITG_STARS_IN_FRONT;
inline constexpr Int32 IDITG_STARS_COLOR1 = IDITG_SKY_BRIGHTSTAR;
inline constexpr Int32 IDITG_STARS_COLOR2 = IDITG_SKY_MEDIUMSTAR;
inline constexpr Int32 IDITG_STARS_COLOR3 = IDITG_SKY_DIMSTAR;

inline constexpr Int32 IDITG_PLANET_FILE_NAME = IDITG_BMP_FILE_NAME;
inline constexpr Int32 IDITG_PLANET_AZIMUTH = IDITG_SKY_AZIMUTH;
inline constexpr Int32 IDITG_PLANET_INCIDENCE = IDITG_SKY_INCIDENCE;
inline constexpr Int32 IDITG_PLANET_SIZE = IDITG_SIZE;
inline constexpr Int32 IDITG_PLANET_DISTANCE = IDITG_DISTANCE;
inline constexpr Int32 IDITG_PLANET_LENS_FLARE = IDITG_SKY_LENSFLARE;
inline constexpr Int32 IDITG_PLANET_CAST_SHADOWS = IDITG_SKY_SHADOWS;
inline constexpr Int32 IDITG_PLANET_INTENSITY = IDITG_SKY_LIGHTINTENSITY;
inline constexpr Int32 IDITG_PLANET_AMBIENT = IDITG_SKY_LIGHTAMBIENT;

inline constexpr Int32 IDITG_SNOW_INTENSITY = IDITG_SKY_LIGHTINTENSITY;
inline constexpr Int32 IDITG_SNOW_WIND = IDITG_POSITION;
inline constexpr Int32 IDITG_SNOW_SUSPEND_RENDER = IDITG_VISIBLE;
inline constexpr Int32 IDITG_SNOW_RAIN = IDITG_ACTIVE;

inline constexpr Int32 IDITG_POS = IDITG_POSITION;
inline constexpr Int32 IDITG_DIM = IDCOMEDIT_ITG_DIMENSIONS;
inline constexpr Int32 IDITG_RANDOM = IDITG_ACTIVE;

// -----------------------------------------------------------------------------
// GUI & Font Tag IDs (darkstar.strings.h / fear.strings.h)
// -----------------------------------------------------------------------------
inline constexpr Int32 IDGUI_FLYINGCAM = 100114;
inline constexpr Int32 IDFNT_EDITOR = 159996;
inline constexpr Int32 IDFNT_EDITOR_HILITE = 159997;
inline constexpr Int32 IDFNT_FONT_DEFAULT = 159998;

// -----------------------------------------------------------------------------
// Additional Fear Action IDs (fear.strings.h)
// -----------------------------------------------------------------------------
inline constexpr Int32 IDACTION_SHOTGUN = 220013;
inline constexpr Int32 IDACTION_PLASMA = 220014;
inline constexpr Int32 IDACTION_ROCKET = 220015;
inline constexpr Int32 IDACTION_GRENADE = 220016;
inline constexpr Int32 IDACTION_VIEW = 220017;
inline constexpr Int32 IDACTION_FIRE2 = 220018;
inline constexpr Int32 IDACTION_BREAK1 = 220019;
inline constexpr Int32 IDACTION_CHAINGUN = 220020;
inline constexpr Int32 IDACTION_SNIPER = 220021;
inline constexpr Int32 IDACTION_TOGGLE = 220100;
inline constexpr Int32 IDACTION_TURN_ON = 220101;
inline constexpr Int32 IDACTION_TURN_OFF = 220102;
inline constexpr Int32 IDACTION_EMPTY = 220103;
inline constexpr Int32 IDACTION_RUN = 220104;
inline constexpr Int32 IDACTION_STRAFE = 220105;
inline constexpr Int32 IDACTION_USEITEM = 220106;
inline constexpr Int32 IDACTION_JET = 220107;
inline constexpr Int32 IDACTION_SEND_VALUE = 220108;
inline constexpr Int32 IDACTION_WAVE = 220109;
inline constexpr Int32 IDACTION_CROUCH = 220110;
inline constexpr Int32 IDACTION_STAND = 220111;
inline constexpr Int32 IDACTION_TOGGLE_CROUCH = 220112;
inline constexpr Int32 IDACTION_SNIPER_FOV = 220113;
inline constexpr Int32 IDACTION_INC_SNIPER_FOV = 220114;
inline constexpr Int32 IDACTION_USE_PACK = 220115;
inline constexpr Int32 IDACTION_MINE = 220116;
inline constexpr Int32 IDACTION_OBSERVER_NEXT = 220212;
inline constexpr Int32 IDACTION_OBSERVER_PREV = 220213;
inline constexpr Int32 IDACTION_OBSERVER_TOGGLE = 220214;
inline constexpr Int32 IDACTION_LOOKUP = 220221;
inline constexpr Int32 IDACTION_LOOKDOWN = 220222;
inline constexpr Int32 IDACTION_TURNLEFT = 220223;
inline constexpr Int32 IDACTION_TURNRIGHT = 220224;
inline constexpr Int32 IDACTION_PITCHSPEED = 220225;
inline constexpr Int32 IDACTION_YAWSPEED = 220226;
inline constexpr Int32 IDACTION_ME_MOD1 = 220227;
inline constexpr Int32 IDACTION_ME_MOD2 = 220228;
inline constexpr Int32 IDACTION_ME_MOD3 = 220229;
inline constexpr Int32 IDACTION_CENTERVIEW = 220230;

} // namespace opent1::game
