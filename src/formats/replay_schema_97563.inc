// Replay type schema for build 97563, transcribed from the published replay
// protocol specification for that exact build. Data only: the tables say what
// shape each type is and how wide its fields are; the walk over them lives in
// replay_events.cpp. Indices in the trailing comments are the schema's own type
// ids and are what kGameEvents and the named entry points refer to.
//
// kGameEvents stores each event's DISTINCTIVE part only: every name in this
// build's table is spelled NNet.Game.S<core>Event, so the shared prefix and the
// shared suffix are held once in replay_events.cpp and composed back on lookup.
//
// Two field names are spelled with "Publisher" where the specification names the
// publisher's trademark: m_isPublisherMap and m_hasNonPublisherExtensionMod. A
// field name is our label for a decoded value and nothing on the wire depends on
// it -- the versioned encoding matches fields by their numeric tag and the
// bit-packed encoding by position -- so the rename changes no byte that is read.
//
// Regenerating this for another build means transcribing that build's table;
// nothing here is inferred and nothing is shared between builds.

const FieldSpec kFields[] = {
    {"m_uint6", 3, 0},
    {"m_uint14", 4, 1},
    {"m_uint22", 5, 2},
    {"m_uint32", 6, 3},
    {"m_userId", 2, -1},
    {"m_flags", 10, 0},
    {"m_major", 10, 1},
    {"m_minor", 10, 2},
    {"m_revision", 10, 3},
    {"m_build", 6, 4},
    {"m_baseBuild", 6, 5},
    {"m_dataDeprecated", 15, 0},
    {"m_data", 16, 1},
    {"m_signature", 9, 0},
    {"m_version", 11, 1},
    {"m_type", 12, 2},
    {"m_elapsedGameLoops", 6, 3},
    {"m_useScaledTime", 13, 4},
    {"m_ngdpRootKey", 17, 5},
    {"m_dataBuildNum", 6, 6},
    {"m_replayCompatibilityHash", 17, 7},
    {"m_ngdpRootKeyIsDevData", 13, 8},
    {"m_region", 10, 0},
    {"m_programId", 19, 1},
    {"m_realm", 6, 2},
    {"m_name", 20, 3},
    {"m_id", 21, 4},
    {"m_a", 10, 0},
    {"m_r", 10, 1},
    {"m_g", 10, 2},
    {"m_b", 10, 3},
    {"m_name", 9, 0},
    {"m_toon", 22, 1},
    {"m_race", 9, 2},
    {"m_color", 23, 3},
    {"m_control", 10, 4},
    {"m_teamId", 1, 5},
    {"m_handicap", 6, 6},
    {"m_observe", 24, 7},
    {"m_result", 24, 8},
    {"m_workingSetSlotId", 25, 9},
    {"m_hero", 9, 10},
    {"m_file", 30, 0},
    {"m_playerList", 28, 0},
    {"m_title", 29, 1},
    {"m_difficulty", 9, 2},
    {"m_thumbnail", 31, 3},
    {"m_isPublisherMap", 13, 4},
    {"m_timeUTC", 32, 5},
    {"m_timeLocalOffset", 32, 6},
    {"m_restartAsTransitionMap", 33, 16},
    {"m_disableRecoverGame", 13, 17},
    {"m_description", 34, 7},
    {"m_imageFilePath", 30, 8},
    {"m_campaignIndex", 10, 15},
    {"m_mapFileName", 30, 9},
    {"m_cacheHandles", 37, 10},
    {"m_miniSave", 13, 11},
    {"m_gameSpeed", 12, 12},
    {"m_defaultDifficulty", 3, 13},
    {"m_modPaths", 39, 14},
    {"m_race", 25, -1},
    {"m_team", 25, -1},
    {"m_name", 9, -19},
    {"m_clanTag", 41, -18},
    {"m_clanLogo", 42, -17},
    {"m_highestLeague", 25, -16},
    {"m_combinedRaceLevels", 43, -15},
    {"m_randomSeed", 6, -14},
    {"m_racePreference", 44, -13},
    {"m_teamPreference", 45, -12},
    {"m_testMap", 13, -11},
    {"m_testAuto", 13, -10},
    {"m_examine", 13, -9},
    {"m_customInterface", 13, -8},
    {"m_testType", 6, -7},
    {"m_observe", 24, -6},
    {"m_hero", 46, -5},
    {"m_skin", 46, -4},
    {"m_mount", 46, -3},
    {"m_toonHandle", 20, -2},
    {"m_scaledRating", 48, -1},
    {"m_lockTeams", 13, -16},
    {"m_teamsTogether", 13, -15},
    {"m_advancedSharedControl", 13, -14},
    {"m_randomRaces", 13, -13},
    {"m_battleNet", 13, -12},
    {"m_amm", 13, -11},
    {"m_competitive", 13, -10},
    {"m_practice", 13, -9},
    {"m_cooperative", 13, -8},
    {"m_noVictoryOrDefeat", 13, -7},
    {"m_heroDuplicatesAllowed", 13, -6},
    {"m_fog", 24, -5},
    {"m_observers", 24, -4},
    {"m_userDifficulty", 24, -3},
    {"m_clientDebugFlags", 21, -2},
    {"m_buildCoachEnabled", 13, -1},
    {"m_allowedColors", 54, -6},
    {"m_allowedRaces", 55, -5},
    {"m_allowedDifficulty", 54, -4},
    {"m_allowedControls", 55, -3},
    {"m_allowedObserveTypes", 56, -2},
    {"m_allowedAIBuilds", 55, -1},
    {"m_randomValue", 6, -28},
    {"m_gameCacheName", 29, -27},
    {"m_gameOptions", 51, -26},
    {"m_gameSpeed", 12, -25},
    {"m_gameType", 12, -24},
    {"m_maxUsers", 2, -23},
    {"m_maxObservers", 2, -22},
    {"m_maxPlayers", 2, -21},
    {"m_maxTeams", 52, -20},
    {"m_maxColors", 3, -19},
    {"m_maxRaces", 53, -18},
    {"m_maxControls", 10, -17},
    {"m_mapSizeX", 10, -16},
    {"m_mapSizeY", 10, -15},
    {"m_mapFileSyncChecksum", 6, -14},
    {"m_mapFileName", 30, -13},
    {"m_mapAuthorName", 9, -12},
    {"m_modFileSyncChecksum", 6, -11},
    {"m_slotDescriptions", 58, -10},
    {"m_defaultDifficulty", 3, -9},
    {"m_defaultAIBuild", 10, -8},
    {"m_cacheHandles", 36, -7},
    {"m_hasExtensionMod", 13, -6},
    {"m_hasNonPublisherExtensionMod", 13, -5},
    {"m_isPublisherMap", 13, -4},
    {"m_isPremadeFFA", 13, -3},
    {"m_isCoopMode", 13, -2},
    {"m_isRealtimeMode", 13, -1},
    {"m_color", 61, -1},
    {"m_key", 6, -2},
    {"m_rewards", 64, -1},
    {"m_control", 10, -32},
    {"m_userId", 60, -31},
    {"m_teamId", 1, -30},
    {"m_colorPref", 62, -29},
    {"m_racePref", 44, -28},
    {"m_difficulty", 3, -27},
    {"m_aiBuild", 10, -26},
    {"m_handicap", 6, -25},
    {"m_observe", 24, -24},
    {"m_logoIndex", 6, -23},
    {"m_hero", 46, -22},
    {"m_skin", 46, -21},
    {"m_mount", 46, -20},
    {"m_artifacts", 63, -19},
    {"m_workingSetSlotId", 25, -18},
    {"m_rewards", 64, -17},
    {"m_toonHandle", 20, -16},
    {"m_licenses", 65, -15},
    {"m_tandemLeaderId", 60, -14},
    {"m_commander", 46, -13},
    {"m_commanderLevel", 6, -12},
    {"m_hasSilencePenalty", 13, -11},
    {"m_tandemId", 60, -10},
    {"m_commanderMasteryLevel", 6, -9},
    {"m_commanderMasteryTalents", 66, -8},
    {"m_trophyId", 6, -7},
    {"m_rewardOverrides", 68, -6},
    {"m_brutalPlusDifficulty", 6, -5},
    {"m_retryMutationIndexes", 66, -4},
    {"m_aCEnemyRace", 6, -3},
    {"m_aCEnemyWaveType", 6, -2},
    {"m_selectedCommanderPrestige", 6, -1},
    {"m_phase", 12, -11},
    {"m_maxUsers", 2, -10},
    {"m_maxObservers", 2, -9},
    {"m_slots", 70, -8},
    {"m_randomSeed", 6, -7},
    {"m_hostUserId", 60, -6},
    {"m_isSinglePlayer", 13, -5},
    {"m_pickedMapTag", 10, -4},
    {"m_gameDuration", 6, -3},
    {"m_defaultDifficulty", 3, -2},
    {"m_defaultAIBuild", 10, -1},
    {"m_userInitialData", 50, -3},
    {"m_gameDescription", 59, -2},
    {"m_lobbyState", 71, -1},
    {"m_syncLobbyState", 72, -1},
    {"m_name", 20, -6},
    {"m_name", 75, -6},
    {"m_name", 75, -8},
    {"m_type", 6, -7},
    {"m_data", 20, -6},
    {"m_type", 6, -8},
    {"m_name", 75, -7},
    {"m_data", 34, -6},
    {"m_signature", 79, -7},
    {"m_toonHandle", 20, -6},
    {"m_gameFullyDownloaded", 13, -19},
    {"m_developmentCheatsEnabled", 13, -18},
    {"m_testCheatsEnabled", 13, -17},
    {"m_multiplayerCheatsEnabled", 13, -16},
    {"m_syncChecksummingEnabled", 13, -15},
    {"m_isMapToMapTransition", 13, -14},
    {"m_debugPauseEnabled", 13, -13},
    {"m_useGalaxyAsserts", 13, -12},
    {"m_platformMac", 13, -11},
    {"m_cameraFollow", 13, -10},
    {"m_baseBuildNum", 6, -9},
    {"m_buildNum", 6, -8},
    {"m_versionFlags", 6, -7},
    {"m_hotkeyProfile", 46, -6},
    {"x", 83, -2},
    {"y", 83, -1},
    {"m_which", 12, -7},
    {"m_target", 84, -6},
    {"m_fileName", 30, -10},
    {"m_automatic", 13, -9},
    {"m_overwrite", 13, -8},
    {"m_name", 9, -7},
    {"m_description", 29, -6},
    {"m_sequence", 6, -6},
    {"x", 47, -2},
    {"y", 47, -1},
    {"m_point", 88, -4},
    {"m_time", 47, -3},
    {"m_verb", 29, -2},
    {"m_arguments", 29, -1},
    {"m_data", 89, -6},
    {"m_abilLink", 83, -3},
    {"m_abilCmdIndex", 2, -2},
    {"m_abilCmdData", 25, -1},
    {"x", 95, -3},
    {"y", 95, -2},
    {"z", 47, -1},
    {"m_targetUnitFlags", 83, -7},
    {"m_timer", 10, -6},
    {"m_tag", 6, -5},
    {"m_snapshotUnitLink", 83, -4},
    {"m_snapshotControlPlayerId", 60, -3},
    {"m_snapshotUpkeepPlayerId", 60, -2},
    {"m_snapshotPoint", 96, -1},
    {"None", 94, 0},
    {"TargetPoint", 96, 1},
    {"TargetUnit", 97, 2},
    {"Data", 6, 3},
    {"m_cmdFlags", 91, -11},
    {"m_abil", 93, -10},
    {"m_data", 98, -9},
    {"m_sequence", 99, -8},
    {"m_otherUnit", 43, -7},
    {"m_unitGroup", 43, -6},
    {"None", 94, 0},
    {"Mask", 102, 1},
    {"OneIndices", 103, 2},
    {"ZeroIndices", 103, 3},
    {"m_unitLink", 83, -4},
    {"m_subgroupPriority", 10, -3},
    {"m_intraSubgroupPriority", 10, -2},
    {"m_count", 101, -1},
    {"m_subgroupIndex", 101, -4},
    {"m_removeMask", 104, -3},
    {"m_addSubgroups", 106, -2},
    {"m_addUnitTags", 107, -1},
    {"m_controlGroupId", 1, -7},
    {"m_delta", 108, -6},
    {"m_controlGroupIndex", 1, -8},
    {"m_controlGroupUpdate", 12, -7},
    {"m_mask", 104, -6},
    {"m_count", 101, -6},
    {"m_subgroupCount", 101, -5},
    {"m_activeSubgroupIndex", 101, -4},
    {"m_unitTagsChecksum", 6, -3},
    {"m_subgroupIndicesChecksum", 6, -2},
    {"m_subgroupsChecksum", 6, -1},
    {"m_controlGroupId", 1, -7},
    {"m_selectionSyncData", 111, -6},
    {"m_recipientId", 1, -7},
    {"m_resources", 113, -6},
    {"m_chatMessage", 29, -6},
    {"x", 47, -3},
    {"y", 47, -2},
    {"z", 47, -1},
    {"m_beacon", 116, -14},
    {"m_ally", 116, -13},
    {"m_flags", 116, -12},
    {"m_build", 116, -11},
    {"m_targetUnitTag", 6, -10},
    {"m_targetUnitSnapshotUnitLink", 83, -9},
    {"m_targetUnitSnapshotUpkeepPlayerId", 116, -8},
    {"m_targetUnitSnapshotControlPlayerId", 116, -7},
    {"m_targetPoint", 117, -6},
    {"m_speed", 12, -6},
    {"m_delta", 116, -6},
    {"m_point", 88, -14},
    {"m_unit", 6, -13},
    {"m_unitLink", 83, -12},
    {"m_unitControlPlayerId", 60, -11},
    {"m_unitUpkeepPlayerId", 60, -10},
    {"m_unitPosition", 96, -9},
    {"m_unitIsUnderConstruction", 13, -8},
    {"m_pingedMinimap", 13, -7},
    {"m_option", 47, -6},
    {"m_verb", 29, -7},
    {"m_arguments", 29, -6},
    {"m_alliance", 6, -7},
    {"m_control", 6, -6},
    {"m_unitTag", 6, -6},
    {"m_unitTag", 6, -7},
    {"m_flags", 10, -6},
    {"m_conversationId", 47, -7},
    {"m_replyId", 47, -6},
    {"m_gameUserId", 1, -6},
    {"m_observe", 24, -5},
    {"m_name", 9, -4},
    {"m_toonHandle", 127, -3},
    {"m_clanTag", 41, -2},
    {"m_clanLogo", 42, -1},
    {"m_userInfos", 129, -7},
    {"m_method", 130, -6},
    {"m_purchaseItemId", 47, -6},
    {"m_difficultyLevel", 47, -6},
    {"None", 94, 0},
    {"Checked", 13, 1},
    {"ValueChanged", 6, 2},
    {"SelectionChanged", 47, 3},
    {"TextChanged", 30, 4},
    {"MouseButton", 6, 5},
    {"m_controlId", 47, -8},
    {"m_eventType", 47, -7},
    {"m_eventData", 134, -6},
    {"m_soundHash", 6, -7},
    {"m_length", 6, -6},
    {"m_soundHash", 137, -2},
    {"m_length", 137, -1},
    {"m_syncInfo", 138, -6},
    {"m_queryId", 83, -8},
    {"m_lengthMs", 6, -7},
    {"m_finishGameLoop", 6, -6},
    {"m_queryId", 83, -7},
    {"m_lengthMs", 6, -6},
    {"m_animWaitQueryId", 83, -6},
    {"m_sound", 6, -6},
    {"m_transmissionId", 47, -7},
    {"m_thread", 6, -6},
    {"m_transmissionId", 47, -6},
    {"m_target", 146, -11},
    {"m_distance", 147, -10},
    {"m_pitch", 147, -9},
    {"m_yaw", 147, -8},
    {"m_reason", 148, -7},
    {"m_follow", 13, -6},
    {"m_skipType", 130, -6},
    {"x", 151, -2},
    {"y", 151, -1},
    {"m_button", 6, -10},
    {"m_down", 13, -9},
    {"m_posUI", 152, -8},
    {"m_posWorld", 96, -7},
    {"m_flags", 116, -6},
    {"m_posUI", 152, -8},
    {"m_posWorld", 96, -7},
    {"m_flags", 116, -6},
    {"m_achievementLink", 83, -6},
    {"m_hotkey", 6, -7},
    {"m_down", 13, -6},
    {"m_abilLink", 83, -8},
    {"m_abilCmdIndex", 2, -7},
    {"m_state", 116, -6},
    {"m_soundtrack", 6, -6},
    {"m_planetId", 47, -6},
    {"m_key", 116, -7},
    {"m_flags", 116, -6},
    {"m_resources", 113, -6},
    {"m_fulfillRequestId", 47, -6},
    {"m_cancelRequestId", 47, -6},
    {"m_error", 47, -7},
    {"m_abil", 93, -6},
    {"m_researchItemId", 47, -6},
    {"m_mercenaryId", 47, -6},
    {"m_battleReportId", 47, -7},
    {"m_difficultyLevel", 47, -6},
    {"m_battleReportId", 47, -6},
    {"m_decrementSeconds", 47, -6},
    {"m_portraitId", 47, -6},
    {"m_functionName", 20, -6},
    {"m_result", 47, -6},
    {"m_gameMenuItemIndex", 47, -6},
    {"m_wheelSpin", 174, -7},
    {"m_flags", 116, -6},
    {"m_purchaseCategoryId", 47, -6},
    {"m_button", 83, -6},
    {"m_cutsceneId", 47, -7},
    {"m_bookmarkName", 20, -6},
    {"m_cutsceneId", 47, -6},
    {"m_cutsceneId", 47, -8},
    {"m_conversationLine", 20, -7},
    {"m_altConversationLine", 20, -6},
    {"m_cutsceneId", 47, -7},
    {"m_conversationLine", 20, -6},
    {"m_leaveReason", 1, -6},
    {"m_observe", 24, -12},
    {"m_name", 9, -11},
    {"m_toonHandle", 127, -10},
    {"m_clanTag", 41, -9},
    {"m_clanLogo", 42, -8},
    {"m_hijack", 13, -7},
    {"m_hijackCloneGameUserId", 60, -6},
    {"m_state", 24, -7},
    {"m_sequence", 184, -6},
    {"m_target", 96, -6},
    {"m_target", 97, -6},
    {"m_catalog", 10, -9},
    {"m_entry", 83, -8},
    {"m_field", 9, -7},
    {"m_value", 9, -6},
    {"m_index", 6, -6},
    {"m_shown", 13, -6},
    {"m_syncTime", 6, -6},
    {"m_recipient", 12, -3},
    {"m_string", 30, -2},
    {"m_recipient", 12, -3},
    {"m_point", 88, -2},
    {"m_progress", 47, -2},
    {"m_status", 24, -2},
    {"m_scoreValueMineralsCurrent", 47, 0},
    {"m_scoreValueVespeneCurrent", 47, 1},
    {"m_scoreValueMineralsCollectionRate", 47, 2},
    {"m_scoreValueVespeneCollectionRate", 47, 3},
    {"m_scoreValueWorkersActiveCount", 47, 4},
    {"m_scoreValueMineralsUsedInProgressArmy", 47, 5},
    {"m_scoreValueMineralsUsedInProgressEconomy", 47, 6},
    {"m_scoreValueMineralsUsedInProgressTechnology", 47, 7},
    {"m_scoreValueVespeneUsedInProgressArmy", 47, 8},
    {"m_scoreValueVespeneUsedInProgressEconomy", 47, 9},
    {"m_scoreValueVespeneUsedInProgressTechnology", 47, 10},
    {"m_scoreValueMineralsUsedCurrentArmy", 47, 11},
    {"m_scoreValueMineralsUsedCurrentEconomy", 47, 12},
    {"m_scoreValueMineralsUsedCurrentTechnology", 47, 13},
    {"m_scoreValueVespeneUsedCurrentArmy", 47, 14},
    {"m_scoreValueVespeneUsedCurrentEconomy", 47, 15},
    {"m_scoreValueVespeneUsedCurrentTechnology", 47, 16},
    {"m_scoreValueMineralsLostArmy", 47, 17},
    {"m_scoreValueMineralsLostEconomy", 47, 18},
    {"m_scoreValueMineralsLostTechnology", 47, 19},
    {"m_scoreValueVespeneLostArmy", 47, 20},
    {"m_scoreValueVespeneLostEconomy", 47, 21},
    {"m_scoreValueVespeneLostTechnology", 47, 22},
    {"m_scoreValueMineralsKilledArmy", 47, 23},
    {"m_scoreValueMineralsKilledEconomy", 47, 24},
    {"m_scoreValueMineralsKilledTechnology", 47, 25},
    {"m_scoreValueVespeneKilledArmy", 47, 26},
    {"m_scoreValueVespeneKilledEconomy", 47, 27},
    {"m_scoreValueVespeneKilledTechnology", 47, 28},
    {"m_scoreValueFoodUsed", 47, 29},
    {"m_scoreValueFoodMade", 47, 30},
    {"m_scoreValueMineralsUsedActiveForces", 47, 31},
    {"m_scoreValueVespeneUsedActiveForces", 47, 32},
    {"m_scoreValueMineralsFriendlyFireArmy", 47, 33},
    {"m_scoreValueMineralsFriendlyFireEconomy", 47, 34},
    {"m_scoreValueMineralsFriendlyFireTechnology", 47, 35},
    {"m_scoreValueVespeneFriendlyFireArmy", 47, 36},
    {"m_scoreValueVespeneFriendlyFireEconomy", 47, 37},
    {"m_scoreValueVespeneFriendlyFireTechnology", 47, 38},
    {"m_playerId", 1, 0},
    {"m_stats", 196, 1},
    {"m_unitTagIndex", 6, 0},
    {"m_unitTagRecycle", 6, 1},
    {"m_unitTypeName", 29, 2},
    {"m_controlPlayerId", 1, 3},
    {"m_upkeepPlayerId", 1, 4},
    {"m_x", 10, 5},
    {"m_y", 10, 6},
    {"m_creatorUnitTagIndex", 43, 7},
    {"m_creatorUnitTagRecycle", 43, 8},
    {"m_creatorAbilityName", 198, 9},
    {"m_unitTagIndex", 6, 0},
    {"m_unitTagRecycle", 6, 1},
    {"m_killerPlayerId", 60, 2},
    {"m_x", 10, 3},
    {"m_y", 10, 4},
    {"m_killerUnitTagIndex", 43, 5},
    {"m_killerUnitTagRecycle", 43, 6},
    {"m_unitTagIndex", 6, 0},
    {"m_unitTagRecycle", 6, 1},
    {"m_controlPlayerId", 1, 2},
    {"m_upkeepPlayerId", 1, 3},
    {"m_unitTagIndex", 6, 0},
    {"m_unitTagRecycle", 6, 1},
    {"m_unitTypeName", 29, 2},
    {"m_playerId", 1, 0},
    {"m_upgradeTypeName", 29, 1},
    {"m_count", 47, 2},
    {"m_unitTagIndex", 6, 0},
    {"m_unitTagRecycle", 6, 1},
    {"m_unitTypeName", 29, 2},
    {"m_controlPlayerId", 1, 3},
    {"m_upkeepPlayerId", 1, 4},
    {"m_x", 10, 5},
    {"m_y", 10, 6},
    {"m_unitTagIndex", 6, 0},
    {"m_unitTagRecycle", 6, 1},
    {"m_firstUnitIndex", 6, 0},
    {"m_items", 206, 1},
    {"m_playerId", 1, 0},
    {"m_type", 6, 1},
    {"m_userId", 43, 2},
    {"m_slotId", 43, 3},
};

const TypeSpec kTypes[] = {
    {Kind::Int, 0, 7, 0, 0, 0},  // 0
    {Kind::Int, 0, 4, 0, 0, 0},  // 1
    {Kind::Int, 0, 5, 0, 0, 0},  // 2
    {Kind::Int, 0, 6, 0, 0, 0},  // 3
    {Kind::Int, 0, 14, 0, 0, 0},  // 4
    {Kind::Int, 0, 22, 0, 0, 0},  // 5
    {Kind::Int, 0, 32, 0, 0, 0},  // 6
    {Kind::Choice, 0, 2, 0, 0, 4},  // 7
    {Kind::Struct, 0, 0, 0, 4, 1},  // 8
    {Kind::Blob, 0, 8, 0, 5, 0},  // 9
    {Kind::Int, 0, 8, 0, 5, 0},  // 10
    {Kind::Struct, 0, 0, 0, 5, 6},  // 11
    {Kind::Int, 0, 3, 0, 11, 0},  // 12
    {Kind::Bool, 0, 0, 0, 11, 0},  // 13
    {Kind::Array, 16, 0, 10, 11, 0},  // 14
    {Kind::Optional, 0, 0, 14, 11, 0},  // 15
    {Kind::Blob, 16, 0, 0, 11, 0},  // 16
    {Kind::Struct, 0, 0, 0, 11, 2},  // 17
    {Kind::Struct, 0, 0, 0, 13, 9},  // 18
    {Kind::FourCC, 0, 0, 0, 22, 0},  // 19
    {Kind::Blob, 0, 7, 0, 22, 0},  // 20
    {Kind::Int, 0, 64, 0, 22, 0},  // 21
    {Kind::Struct, 0, 0, 0, 22, 5},  // 22
    {Kind::Struct, 0, 0, 0, 27, 4},  // 23
    {Kind::Int, 0, 2, 0, 31, 0},  // 24
    {Kind::Optional, 0, 0, 10, 31, 0},  // 25
    {Kind::Struct, 0, 0, 0, 31, 11},  // 26
    {Kind::Array, 0, 5, 26, 42, 0},  // 27
    {Kind::Optional, 0, 0, 27, 42, 0},  // 28
    {Kind::Blob, 0, 10, 0, 42, 0},  // 29
    {Kind::Blob, 0, 11, 0, 42, 0},  // 30
    {Kind::Struct, 0, 0, 0, 42, 1},  // 31
    {Kind::Int, INT64_MIN, 64, 0, 43, 0},  // 32
    {Kind::Optional, 0, 0, 13, 43, 0},  // 33
    {Kind::Blob, 0, 12, 0, 43, 0},  // 34
    {Kind::Blob, 40, 0, 0, 43, 0},  // 35
    {Kind::Array, 0, 6, 35, 43, 0},  // 36
    {Kind::Optional, 0, 0, 36, 43, 0},  // 37
    {Kind::Array, 0, 6, 30, 43, 0},  // 38
    {Kind::Optional, 0, 0, 38, 43, 0},  // 39
    {Kind::Struct, 0, 0, 0, 43, 18},  // 40
    {Kind::Optional, 0, 0, 9, 61, 0},  // 41
    {Kind::Optional, 0, 0, 35, 61, 0},  // 42
    {Kind::Optional, 0, 0, 6, 61, 0},  // 43
    {Kind::Struct, 0, 0, 0, 61, 1},  // 44
    {Kind::Struct, 0, 0, 0, 62, 1},  // 45
    {Kind::Blob, 0, 9, 0, 63, 0},  // 46
    {Kind::Int, -2147483648, 32, 0, 63, 0},  // 47
    {Kind::Optional, 0, 0, 47, 63, 0},  // 48
    {Kind::Struct, 0, 0, 0, 63, 19},  // 49
    {Kind::Array, 0, 5, 49, 82, 0},  // 50
    {Kind::Struct, 0, 0, 0, 82, 16},  // 51
    {Kind::Int, 1, 4, 0, 98, 0},  // 52
    {Kind::Int, 1, 8, 0, 98, 0},  // 53
    {Kind::BitArray, 0, 6, 0, 98, 0},  // 54
    {Kind::BitArray, 0, 8, 0, 98, 0},  // 55
    {Kind::BitArray, 0, 2, 0, 98, 0},  // 56
    {Kind::Struct, 0, 0, 0, 98, 6},  // 57
    {Kind::Array, 0, 5, 57, 104, 0},  // 58
    {Kind::Struct, 0, 0, 0, 104, 28},  // 59
    {Kind::Optional, 0, 0, 1, 132, 0},  // 60
    {Kind::Optional, 0, 0, 2, 132, 0},  // 61
    {Kind::Struct, 0, 0, 0, 132, 1},  // 62
    {Kind::Array, 0, 4, 46, 133, 0},  // 63
    {Kind::Array, 0, 17, 6, 133, 0},  // 64
    {Kind::Array, 0, 16, 6, 133, 0},  // 65
    {Kind::Array, 0, 3, 6, 133, 0},  // 66
    {Kind::Struct, 0, 0, 0, 133, 2},  // 67
    {Kind::Array, 0, 17, 67, 135, 0},  // 68
    {Kind::Struct, 0, 0, 0, 135, 32},  // 69
    {Kind::Array, 0, 5, 69, 167, 0},  // 70
    {Kind::Struct, 0, 0, 0, 167, 11},  // 71
    {Kind::Struct, 0, 0, 0, 178, 3},  // 72
    {Kind::Struct, 0, 0, 0, 181, 1},  // 73
    {Kind::Struct, 0, 0, 0, 182, 1},  // 74
    {Kind::Blob, 0, 6, 0, 183, 0},  // 75
    {Kind::Struct, 0, 0, 0, 183, 1},  // 76
    {Kind::Struct, 0, 0, 0, 184, 3},  // 77
    {Kind::Struct, 0, 0, 0, 187, 3},  // 78
    {Kind::Array, 0, 5, 10, 190, 0},  // 79
    {Kind::Struct, 0, 0, 0, 190, 2},  // 80
    {Kind::Struct, 0, 0, 0, 192, 14},  // 81
    {Kind::Struct, 0, 0, 0, 206, 0},  // 82
    {Kind::Int, 0, 16, 0, 206, 0},  // 83
    {Kind::Struct, 0, 0, 0, 206, 2},  // 84
    {Kind::Struct, 0, 0, 0, 208, 2},  // 85
    {Kind::Struct, 0, 0, 0, 210, 5},  // 86
    {Kind::Struct, 0, 0, 0, 215, 1},  // 87
    {Kind::Struct, 0, 0, 0, 216, 2},  // 88
    {Kind::Struct, 0, 0, 0, 218, 4},  // 89
    {Kind::Struct, 0, 0, 0, 222, 1},  // 90
    {Kind::Int, 0, 27, 0, 223, 0},  // 91
    {Kind::Struct, 0, 0, 0, 223, 3},  // 92
    {Kind::Optional, 0, 0, 92, 226, 0},  // 93
    {Kind::Null, 0, 0, 0, 226, 0},  // 94
    {Kind::Int, 0, 20, 0, 226, 0},  // 95
    {Kind::Struct, 0, 0, 0, 226, 3},  // 96
    {Kind::Struct, 0, 0, 0, 229, 7},  // 97
    {Kind::Choice, 0, 2, 0, 236, 4},  // 98
    {Kind::Int, 1, 32, 0, 240, 0},  // 99
    {Kind::Struct, 0, 0, 0, 240, 6},  // 100
    {Kind::Int, 0, 9, 0, 246, 0},  // 101
    {Kind::BitArray, 0, 9, 0, 246, 0},  // 102
    {Kind::Array, 0, 9, 101, 246, 0},  // 103
    {Kind::Choice, 0, 2, 0, 246, 4},  // 104
    {Kind::Struct, 0, 0, 0, 250, 4},  // 105
    {Kind::Array, 0, 9, 105, 254, 0},  // 106
    {Kind::Array, 0, 9, 6, 254, 0},  // 107
    {Kind::Struct, 0, 0, 0, 254, 4},  // 108
    {Kind::Struct, 0, 0, 0, 258, 2},  // 109
    {Kind::Struct, 0, 0, 0, 260, 3},  // 110
    {Kind::Struct, 0, 0, 0, 263, 6},  // 111
    {Kind::Struct, 0, 0, 0, 269, 2},  // 112
    {Kind::Array, 0, 3, 47, 271, 0},  // 113
    {Kind::Struct, 0, 0, 0, 271, 2},  // 114
    {Kind::Struct, 0, 0, 0, 273, 1},  // 115
    {Kind::Int, -128, 8, 0, 274, 0},  // 116
    {Kind::Struct, 0, 0, 0, 274, 3},  // 117
    {Kind::Struct, 0, 0, 0, 277, 9},  // 118
    {Kind::Struct, 0, 0, 0, 286, 1},  // 119
    {Kind::Struct, 0, 0, 0, 287, 1},  // 120
    {Kind::Struct, 0, 0, 0, 288, 9},  // 121
    {Kind::Struct, 0, 0, 0, 297, 2},  // 122
    {Kind::Struct, 0, 0, 0, 299, 2},  // 123
    {Kind::Struct, 0, 0, 0, 301, 1},  // 124
    {Kind::Struct, 0, 0, 0, 302, 2},  // 125
    {Kind::Struct, 0, 0, 0, 304, 2},  // 126
    {Kind::Optional, 0, 0, 20, 306, 0},  // 127
    {Kind::Struct, 0, 0, 0, 306, 6},  // 128
    {Kind::Array, 0, 5, 128, 312, 0},  // 129
    {Kind::Int, 0, 1, 0, 312, 0},  // 130
    {Kind::Struct, 0, 0, 0, 312, 2},  // 131
    {Kind::Struct, 0, 0, 0, 314, 1},  // 132
    {Kind::Struct, 0, 0, 0, 315, 1},  // 133
    {Kind::Choice, 0, 3, 0, 316, 6},  // 134
    {Kind::Struct, 0, 0, 0, 322, 3},  // 135
    {Kind::Struct, 0, 0, 0, 325, 2},  // 136
    {Kind::Array, 0, 7, 6, 327, 0},  // 137
    {Kind::Struct, 0, 0, 0, 327, 2},  // 138
    {Kind::Struct, 0, 0, 0, 329, 1},  // 139
    {Kind::Struct, 0, 0, 0, 330, 3},  // 140
    {Kind::Struct, 0, 0, 0, 333, 2},  // 141
    {Kind::Struct, 0, 0, 0, 335, 1},  // 142
    {Kind::Struct, 0, 0, 0, 336, 1},  // 143
    {Kind::Struct, 0, 0, 0, 337, 2},  // 144
    {Kind::Struct, 0, 0, 0, 339, 1},  // 145
    {Kind::Optional, 0, 0, 84, 340, 0},  // 146
    {Kind::Optional, 0, 0, 83, 340, 0},  // 147
    {Kind::Optional, 0, 0, 116, 340, 0},  // 148
    {Kind::Struct, 0, 0, 0, 340, 6},  // 149
    {Kind::Struct, 0, 0, 0, 346, 1},  // 150
    {Kind::Int, 0, 11, 0, 347, 0},  // 151
    {Kind::Struct, 0, 0, 0, 347, 2},  // 152
    {Kind::Struct, 0, 0, 0, 349, 5},  // 153
    {Kind::Struct, 0, 0, 0, 354, 3},  // 154
    {Kind::Struct, 0, 0, 0, 357, 1},  // 155
    {Kind::Struct, 0, 0, 0, 358, 2},  // 156
    {Kind::Struct, 0, 0, 0, 360, 3},  // 157
    {Kind::Struct, 0, 0, 0, 363, 1},  // 158
    {Kind::Struct, 0, 0, 0, 364, 1},  // 159
    {Kind::Struct, 0, 0, 0, 365, 2},  // 160
    {Kind::Struct, 0, 0, 0, 367, 1},  // 161
    {Kind::Struct, 0, 0, 0, 368, 1},  // 162
    {Kind::Struct, 0, 0, 0, 369, 1},  // 163
    {Kind::Struct, 0, 0, 0, 370, 2},  // 164
    {Kind::Struct, 0, 0, 0, 372, 1},  // 165
    {Kind::Struct, 0, 0, 0, 373, 1},  // 166
    {Kind::Struct, 0, 0, 0, 374, 2},  // 167
    {Kind::Struct, 0, 0, 0, 376, 1},  // 168
    {Kind::Struct, 0, 0, 0, 377, 1},  // 169
    {Kind::Struct, 0, 0, 0, 378, 1},  // 170
    {Kind::Struct, 0, 0, 0, 379, 1},  // 171
    {Kind::Struct, 0, 0, 0, 380, 1},  // 172
    {Kind::Struct, 0, 0, 0, 381, 1},  // 173
    {Kind::Int, -32768, 16, 0, 382, 0},  // 174
    {Kind::Struct, 0, 0, 0, 382, 2},  // 175
    {Kind::Struct, 0, 0, 0, 384, 1},  // 176
    {Kind::Struct, 0, 0, 0, 385, 1},  // 177
    {Kind::Struct, 0, 0, 0, 386, 2},  // 178
    {Kind::Struct, 0, 0, 0, 388, 1},  // 179
    {Kind::Struct, 0, 0, 0, 389, 3},  // 180
    {Kind::Struct, 0, 0, 0, 392, 2},  // 181
    {Kind::Struct, 0, 0, 0, 394, 1},  // 182
    {Kind::Struct, 0, 0, 0, 395, 7},  // 183
    {Kind::Optional, 0, 0, 99, 402, 0},  // 184
    {Kind::Struct, 0, 0, 0, 402, 2},  // 185
    {Kind::Struct, 0, 0, 0, 404, 1},  // 186
    {Kind::Struct, 0, 0, 0, 405, 1},  // 187
    {Kind::Struct, 0, 0, 0, 406, 4},  // 188
    {Kind::Struct, 0, 0, 0, 410, 1},  // 189
    {Kind::Struct, 0, 0, 0, 411, 1},  // 190
    {Kind::Struct, 0, 0, 0, 412, 1},  // 191
    {Kind::Struct, 0, 0, 0, 413, 2},  // 192
    {Kind::Struct, 0, 0, 0, 415, 2},  // 193
    {Kind::Struct, 0, 0, 0, 417, 1},  // 194
    {Kind::Struct, 0, 0, 0, 418, 1},  // 195
    {Kind::Struct, 0, 0, 0, 419, 39},  // 196
    {Kind::Struct, 0, 0, 0, 458, 2},  // 197
    {Kind::Optional, 0, 0, 29, 460, 0},  // 198
    {Kind::Struct, 0, 0, 0, 460, 10},  // 199
    {Kind::Struct, 0, 0, 0, 470, 7},  // 200
    {Kind::Struct, 0, 0, 0, 477, 4},  // 201
    {Kind::Struct, 0, 0, 0, 481, 3},  // 202
    {Kind::Struct, 0, 0, 0, 484, 3},  // 203
    {Kind::Struct, 0, 0, 0, 487, 7},  // 204
    {Kind::Struct, 0, 0, 0, 494, 2},  // 205
    {Kind::Array, 0, 10, 47, 496, 0},  // 206
    {Kind::Struct, 0, 0, 0, 496, 2},  // 207
    {Kind::Struct, 0, 0, 0, 498, 4},  // 208
};

const EventSpec kGameEvents[] = {
    {5, 82, "UserFinishedLoadingSync"},
    {7, 81, "UserOptions"},
    {9, 74, "BankFile"},
    {10, 76, "BankSection"},
    {11, 77, "BankKey"},
    {12, 78, "BankValue"},
    {13, 80, "BankSignature"},
    {14, 85, "CameraSave"},
    {21, 86, "SaveGame"},
    {22, 82, "SaveGameDone"},
    {23, 82, "LoadGameDone"},
    {25, 87, "CommandManagerReset"},
    {26, 90, "GameCheat"},
    {27, 100, "Cmd"},
    {28, 109, "SelectionDelta"},
    {29, 110, "ControlGroupUpdate"},
    {30, 112, "SelectionSyncCheck"},
    {31, 114, "ResourceTrade"},
    {32, 115, "TriggerChatMessage"},
    {33, 118, "AICommunicate"},
    {34, 119, "SetAbsoluteGameSpeed"},
    {35, 120, "AddAbsoluteGameSpeed"},
    {36, 121, "TriggerPing"},
    {37, 122, "BroadcastCheat"},
    {38, 123, "Alliance"},
    {39, 124, "UnitClick"},
    {40, 125, "UnitHighlight"},
    {41, 126, "TriggerReplySelected"},
    {43, 131, "HijackReplayGame"},
    {44, 82, "TriggerSkipped"},
    {45, 136, "TriggerSoundLengthQuery"},
    {46, 143, "TriggerSoundOffset"},
    {47, 144, "TriggerTransmissionOffset"},
    {48, 145, "TriggerTransmissionComplete"},
    {49, 149, "CameraUpdate"},
    {50, 82, "TriggerAbortMission"},
    {51, 132, "TriggerPurchaseMade"},
    {52, 82, "TriggerPurchaseExit"},
    {53, 133, "TriggerPlanetMissionLaunched"},
    {54, 82, "TriggerPlanetPanelCanceled"},
    {55, 135, "TriggerDialogControl"},
    {56, 139, "TriggerSoundLengthSync"},
    {57, 150, "TriggerConversationSkipped"},
    {58, 153, "TriggerMouseClicked"},
    {59, 154, "TriggerMouseMoved"},
    {60, 155, "AchievementAwarded"},
    {61, 156, "TriggerHotkeyPressed"},
    {62, 157, "TriggerTargetModeUpdate"},
    {63, 82, "TriggerPlanetPanelReplay"},
    {64, 158, "TriggerSoundtrackDone"},
    {65, 159, "TriggerPlanetMissionSelected"},
    {66, 160, "TriggerKeyPressed"},
    {67, 171, "TriggerMovieFunction"},
    {68, 82, "TriggerPlanetPanelBirthComplete"},
    {69, 82, "TriggerPlanetPanelDeathComplete"},
    {70, 161, "ResourceRequest"},
    {71, 162, "ResourceRequestFulfill"},
    {72, 163, "ResourceRequestCancel"},
    {73, 82, "TriggerResearchPanelExit"},
    {74, 82, "TriggerResearchPanelPurchase"},
    {75, 165, "TriggerResearchPanelSelectionChanged"},
    {76, 164, "TriggerCommandError"},
    {77, 82, "TriggerMercenaryPanelExit"},
    {78, 82, "TriggerMercenaryPanelPurchase"},
    {79, 166, "TriggerMercenaryPanelSelectionChanged"},
    {80, 82, "TriggerVictoryPanelExit"},
    {81, 82, "TriggerBattleReportPanelExit"},
    {82, 167, "TriggerBattleReportPanelPlayMission"},
    {83, 168, "TriggerBattleReportPanelPlayScene"},
    {84, 168, "TriggerBattleReportPanelSelectionChanged"},
    {85, 133, "TriggerVictoryPanelPlayMissionAgain"},
    {86, 82, "TriggerMovieStarted"},
    {87, 82, "TriggerMovieFinished"},
    {88, 169, "DecrementGameTimeRemaining"},
    {89, 170, "TriggerPortraitLoaded"},
    {90, 172, "TriggerCustomDialogDismissed"},
    {91, 173, "TriggerGameMenuItemSelected"},
    {92, 175, "TriggerMouseWheel"},
    {93, 132, "TriggerPurchasePanelSelectedPurchaseItemChanged"},
    {94, 176, "TriggerPurchasePanelSelectedPurchaseCategoryChanged"},
    {95, 177, "TriggerButtonPressed"},
    {96, 82, "TriggerGameCreditsFinished"},
    {97, 178, "TriggerCutsceneBookmarkFired"},
    {98, 179, "TriggerCutsceneEndSceneFired"},
    {99, 180, "TriggerCutsceneConversationLine"},
    {100, 181, "TriggerCutsceneConversationLineMissing"},
    {101, 182, "GameUserLeave"},
    {102, 183, "GameUserJoin"},
    {103, 185, "CommandManagerState"},
    {104, 186, "CmdUpdateTargetPoint"},
    {105, 187, "CmdUpdateTargetUnit"},
    {106, 140, "TriggerAnimLengthQueryByName"},
    {107, 141, "TriggerAnimLengthQueryByProps"},
    {108, 142, "TriggerAnimOffset"},
    {109, 188, "CatalogModify"},
    {110, 189, "HeroTalentTreeSelected"},
    {111, 82, "TriggerProfilerLoggingFinished"},
    {112, 190, "HeroTalentTreeSelectionPanelToggled"},
    {116, 191, "SetSyncLoadingTime"},
    {117, 191, "SetSyncPlayingTime"},
    {118, 191, "PeerSetSyncLoadingTime"},
    {119, 191, "PeerSetSyncPlayingTime"},
};