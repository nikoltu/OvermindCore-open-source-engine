#pragma once

namespace sc2::app::shell::layout {

struct R {
    float x, y, w, h;
};
inline constexpr float kCanvasW = 2133.0f;
inline constexpr float kCanvasH = 1200.0f;

namespace m01_confirm {
inline constexpr R kScreenUnder60Scrim{0, 0, 2133, 1200};
inline constexpr R kDialog640x320{746, 440, 640, 320};
inline constexpr R kTitle{746, 480, 640, 32};
inline constexpr R kBody{794, 532, 544, 84};
inline constexpr R kCancel{794, 656, 264, 72};
inline constexpr R kConfirm{1074, 656, 264, 72};
}

namespace s00_boot {
inline constexpr R kEmblem256{938, 372, 256, 256};
inline constexpr R kIndeterminateBar400x8{866, 692, 400, 8};
}

namespace s01_game_data {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kFirstRunGameData{64, 28, 900, 40};
inline constexpr R kH1WhereAreThe{64, 128, 1200, 56};
inline constexpr R kStatusBlock{64, 224, 1200, 160};
inline constexpr R kPathField{64, 416, 720, 72};
inline constexpr R kBrowse{800, 416, 200, 72};
inline constexpr R kAutoDetect{1016, 416, 248, 72};
inline constexpr R kN7CheckRowsX{64, 520, 1200, 336};
inline constexpr R kGameVersionPanel{1328, 224, 741, 632};
inline constexpr R kQuit{64, 1032, 240, 64};
inline constexpr R kContinue{1669, 1016, 400, 96};
}

namespace s02_starting {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kStarting{64, 28, 600, 40};
inline constexpr R kH1LoadingGameData{566, 128, 1000, 56};
inline constexpr R kN6StageRowsX{566, 264, 1000, 384};
inline constexpr R kProgressBarDoneTotal{566, 720, 1000, 16};
inline constexpr R kStepLabel{566, 752, 700, 28};
inline constexpr R kCounterElapsed{1266, 752, 300, 28};
inline constexpr R kErrorBlock{566, 800, 1000, 160};
inline constexpr R kRetry{566, 992, 320, 80};
inline constexpr R kChangeGameFolder{906, 992, 400, 80};
inline constexpr R kQuit{1326, 992, 240, 80};
}

namespace s10_home {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kIdentityPlate{1709, 12, 360, 72};
inline constexpr R kEmblem{64, 20, 56, 56};
inline constexpr R kCrafterScWordmark{136, 28, 500, 40};
inline constexpr R kPlay{64, 256, 800, 160};
inline constexpr R kSkirmishSetup{64, 448, 800, 72};
inline constexpr R kReplaysAndSaves{64, 528, 800, 72};
inline constexpr R kProfile{64, 608, 800, 72};
inline constexpr R kSettings{64, 688, 800, 72};
inline constexpr R kLanGame{64, 768, 800, 72};
inline constexpr R kCampaign{64, 848, 800, 72};
inline constexpr R kQuit{64, 928, 800, 72};
inline constexpr R kSandbox{64, 1024, 240, 48};
inline constexpr R kAbout{320, 1024, 240, 48};
inline constexpr R kLastMatchCard{960, 256, 1109, 304};
inline constexpr R kMapThumb{984, 280, 256, 256};
inline constexpr R kVerdict{1264, 280, 780, 120};
inline constexpr R kWatchReplay{1264, 472, 320, 64};
inline constexpr R kRecordCard{960, 584, 541, 208};
inline constexpr R kGameDataCard{1528, 584, 541, 208};
inline constexpr R kNoticeBanner{960, 816, 1109, 72};
}

namespace s20_skirmish_setup {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kBack{0, 0, 96, 96};
inline constexpr R kSkirmish{112, 28, 600, 40};
inline constexpr R kIdentityPlate{1709, 12, 360, 72};
inline constexpr R kLabelMap{64, 128, 300, 24};
inline constexpr R kSearch{64, 160, 464, 64};
inline constexpr R kFolder{544, 160, 160, 64};
inline constexpr R kMapListViewport{64, 240, 624, 812};
inline constexpr R kMapRow{64, 240, 624, 64};
inline constexpr R kMapRow2{64, 308, 624, 64};
inline constexpr R kMapRow3{64, 376, 624, 64};
inline constexpr R kMapRow4{64, 444, 624, 64};
inline constexpr R kMapRow5{64, 512, 624, 64};
inline constexpr R kMapRow6{64, 580, 624, 64};
inline constexpr R kMapRow7{64, 648, 624, 64};
inline constexpr R kMapRow8{64, 716, 624, 64};
inline constexpr R kMapRow9{64, 784, 624, 64};
inline constexpr R kMapRow10{64, 852, 624, 64};
inline constexpr R kMapRow11{64, 920, 624, 64};
inline constexpr R kMapRow12{64, 988, 624, 64};
inline constexpr R kScrollbar{696, 240, 8, 812};
inline constexpr R kCount43MapsReading{64, 1068, 640, 28};
inline constexpr R kLabelPlayers{736, 128, 300, 24};
inline constexpr R kSlotCardYou{736, 160, 741, 248};
inline constexpr R kPortraitFrame126x192{760, 188, 126, 192};
inline constexpr R kNameTeamColourChip{910, 188, 543, 36};
inline constexpr R kRaceTerranZergProtoss{910, 300, 543, 64};
inline constexpr R kVs{736, 412, 741, 24};
inline constexpr R kSlotCardOpponent{736, 440, 741, 248};
inline constexpr R kAIAvatarFrame{760, 468, 126, 192};
inline constexpr R kAITeamColour{910, 468, 543, 32};
inline constexpr R kRaceTZP{910, 508, 543, 56};
inline constexpr R kMinus{910, 588, 64, 64};
inline constexpr R kDifficultyValue7Tier{982, 588, 399, 64};
inline constexpr R kPlus{1389, 588, 64, 64};
inline constexpr R kErrorNoticeBanner{736, 712, 741, 72};
inline constexpr R kLabelMatch{1509, 128, 300, 24};
inline constexpr R kMapPreviewWell{1509, 160, 560, 420};
inline constexpr R kMapName{1509, 596, 560, 36};
inline constexpr R kSize{1509, 640, 560, 28};
inline constexpr R kLabelGameSpeed{1509, 700, 300, 24};
inline constexpr R kSlowerSlowNormalFast{1509, 732, 560, 64};
inline constexpr R kSummaryMatchup{1509, 828, 560, 96};
inline constexpr R kWhyStartIsDisabled{1509, 944, 560, 28};
inline constexpr R kStartGame{1509, 984, 560, 112};
}

namespace s30_loading {
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kH1MapName{366, 112, 1400, 56};
inline constexpr R kLabel1v1Melee{666, 184, 800, 24};
inline constexpr R kPlayerCard{414, 264, 620, 308};
inline constexpr R kPortraitFrame170x258{438, 289, 170, 258};
inline constexpr R kName{632, 300, 380, 36};
inline constexpr R kRaceMark{632, 352, 48, 48};
inline constexpr R kRace{692, 360, 320, 28};
inline constexpr R kTeamColourBar{632, 424, 360, 8};
inline constexpr R kVs{1034, 392, 64, 56};
inline constexpr R kPlayerCard2{1098, 264, 620, 308};
inline constexpr R kAIAvatarFrame{1524, 289, 170, 258};
inline constexpr R kAI{1120, 300, 380, 36};
inline constexpr R kMapPreview240{946, 612, 240, 240};
inline constexpr R kTipLine{466, 884, 1200, 28};
inline constexpr R kProgressBarDoneTotal{466, 968, 1200, 16};
inline constexpr R kStageLabel{466, 1000, 800, 28};
inline constexpr R kNTotal{1266, 1000, 400, 28};
}

namespace s50_result {
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kDisplay{466, 88, 1200, 96};
inline constexpr R kMap{466, 208, 1200, 28};
inline constexpr R kYourCard{394, 264, 656, 308};
inline constexpr R kPortrait170x258{418, 289, 170, 258};
inline constexpr R kName{612, 300, 414, 36};
inline constexpr R kOpponentCard{1082, 264, 656, 308};
inline constexpr R kPortrait125x191{1106, 322, 125, 191};
inline constexpr R kName2{1255, 300, 460, 36};
inline constexpr R kScoreTable{394, 604, 1344, 300};
inline constexpr R kPlayAgain{350, 968, 400, 80};
inline constexpr R kChangeSetup{774, 968, 320, 80};
inline constexpr R kWatchReplay{1118, 968, 320, 80};
inline constexpr R kMainMenu{1462, 968, 320, 80};
}

namespace s51_score {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kBack{0, 0, 96, 96};
inline constexpr R kScore{112, 28, 600, 40};
inline constexpr R kIdentityPlate{1709, 12, 360, 72};
inline constexpr R kYourHeaderCard{394, 128, 656, 120};
inline constexpr R kOpponentHeaderCard{1082, 128, 656, 120};
inline constexpr R kFullScoreTable{394, 280, 1344, 720};
inline constexpr R kWatchAgain{1074, 1032, 320, 80};
inline constexpr R kDone{1418, 1032, 320, 80};
}

namespace s60_settings {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kBack{0, 0, 96, 96};
inline constexpr R kSettings{112, 28, 600, 40};
inline constexpr R kIdentityPlate{1709, 12, 360, 72};
inline constexpr R kGameplay{64, 112, 240, 64};
inline constexpr R kVideo{312, 112, 240, 64};
inline constexpr R kAudio{560, 112, 240, 64};
inline constexpr R kControls{808, 112, 240, 64};
inline constexpr R kGameData{1056, 112, 240, 64};
inline constexpr R kAuthoredPanels{1749, 120, 320, 48};
inline constexpr R kTabPage{64, 208, 1349, 904};
inline constexpr R kRowLabelLeft{88, 232, 1301, 64};
inline constexpr R kRowLabelLeft2{88, 304, 1301, 64};
inline constexpr R kRowLabelLeft3{88, 376, 1301, 64};
inline constexpr R kRowLabelLeft4{88, 448, 1301, 64};
inline constexpr R kRowLabelLeft5{88, 520, 1301, 64};
inline constexpr R kRowLabelLeft6{88, 592, 1301, 64};
inline constexpr R kRestoreDefaults{88, 1024, 320, 64};
inline constexpr R kHelpPane{1445, 208, 624, 904};
}

namespace s70_profile {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kBack{0, 0, 96, 96};
inline constexpr R kProfile{112, 28, 600, 40};
inline constexpr R kIdentityPlate{1709, 12, 360, 72};
inline constexpr R kPortraitFrame170x258{64, 160, 170, 258};
inline constexpr R kChangePortrait{64, 440, 320, 64};
inline constexpr R kLabelName{280, 160, 300, 24};
inline constexpr R kNameField{280, 192, 640, 72};
inline constexpr R kLabelPreferredRace{280, 296, 300, 24};
inline constexpr R kTerranZergProtossRandom{280, 328, 640, 64};
inline constexpr R kRecord{1024, 160, 1045, 258};
inline constexpr R kRecentMatchesList{1024, 450, 1045, 662};
}

namespace s71_portraits {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kBack{0, 0, 96, 96};
inline constexpr R kPortraits{112, 28, 600, 40};
inline constexpr R kIdentityPlate{1709, 12, 360, 72};
inline constexpr R kUnits{64, 112, 240, 64};
inline constexpr R kHeroes{312, 112, 240, 64};
inline constexpr R kAllTerranZergProtoss{592, 112, 616, 64};
inline constexpr R kShowingNOfM{1229, 130, 200, 28};
inline constexpr R kSearch{1445, 112, 624, 64};
inline constexpr R kTile104x158{64, 208, 104, 158};
inline constexpr R kRegion{177, 208, 104, 158};
inline constexpr R kRegion2{290, 208, 104, 158};
inline constexpr R kRegion3{403, 208, 104, 158};
inline constexpr R kRegion4{516, 208, 104, 158};
inline constexpr R kRegion5{629, 208, 104, 158};
inline constexpr R kRegion6{742, 208, 104, 158};
inline constexpr R kRegion7{855, 208, 104, 158};
inline constexpr R kRegion8{968, 208, 104, 158};
inline constexpr R kRegion9{1081, 208, 104, 158};
inline constexpr R kRegion10{1194, 208, 104, 158};
inline constexpr R kRegion11{1307, 208, 104, 158};
inline constexpr R kRegion12{64, 375, 104, 158};
inline constexpr R kRegion13{177, 375, 104, 158};
inline constexpr R kRegion14{290, 375, 104, 158};
inline constexpr R kRegion15{403, 375, 104, 158};
inline constexpr R kRegion16{516, 375, 104, 158};
inline constexpr R kRegion17{629, 375, 104, 158};
inline constexpr R kRegion18{742, 375, 104, 158};
inline constexpr R kRegion19{855, 375, 104, 158};
inline constexpr R kRegion20{968, 375, 104, 158};
inline constexpr R kRegion21{1081, 375, 104, 158};
inline constexpr R kRegion22{1194, 375, 104, 158};
inline constexpr R kRegion23{1307, 375, 104, 158};
inline constexpr R kRegion24{64, 542, 104, 158};
inline constexpr R kRegion25{177, 542, 104, 158};
inline constexpr R kRegion26{290, 542, 104, 158};
inline constexpr R kRegion27{403, 542, 104, 158};
inline constexpr R kRegion28{516, 542, 104, 158};
inline constexpr R kRegion29{629, 542, 104, 158};
inline constexpr R kRegion30{742, 542, 104, 158};
inline constexpr R kRegion31{855, 542, 104, 158};
inline constexpr R kRegion32{968, 542, 104, 158};
inline constexpr R kRegion33{1081, 542, 104, 158};
inline constexpr R kRegion34{1194, 542, 104, 158};
inline constexpr R kRegion35{1307, 542, 104, 158};
inline constexpr R kRegion36{64, 709, 104, 158};
inline constexpr R kRegion37{177, 709, 104, 158};
inline constexpr R kRegion38{290, 709, 104, 158};
inline constexpr R kRegion39{403, 709, 104, 158};
inline constexpr R kRegion40{516, 709, 104, 158};
inline constexpr R kRegion41{629, 709, 104, 158};
inline constexpr R kRegion42{742, 709, 104, 158};
inline constexpr R kRegion43{855, 709, 104, 158};
inline constexpr R kRegion44{968, 709, 104, 158};
inline constexpr R kRegion45{1081, 709, 104, 158};
inline constexpr R kRegion46{1194, 709, 104, 158};
inline constexpr R kRegion47{1307, 709, 104, 158};
inline constexpr R kRegion48{64, 876, 104, 158};
inline constexpr R kRegion49{177, 876, 104, 158};
inline constexpr R kRegion50{290, 876, 104, 158};
inline constexpr R kRegion51{403, 876, 104, 158};
inline constexpr R kRegion52{516, 876, 104, 158};
inline constexpr R kRegion53{629, 876, 104, 158};
inline constexpr R kRegion54{742, 876, 104, 158};
inline constexpr R kRegion55{855, 876, 104, 158};
inline constexpr R kRegion56{968, 876, 104, 158};
inline constexpr R kRegion57{1081, 876, 104, 158};
inline constexpr R kRegion58{1194, 876, 104, 158};
inline constexpr R kRegion59{1307, 876, 104, 158};
inline constexpr R kScrollbar{1421, 208, 8, 904};
inline constexpr R kPreviewPane{1445, 208, 624, 904};
inline constexpr R kN170x258{1672, 240, 170, 258};
inline constexpr R kPortraitName{1445, 522, 624, 36};
inline constexpr R kSet{1445, 562, 624, 28};
inline constexpr R kPlate{1560, 632, 47, 72};
inline constexpr R kList{1640, 632, 62, 94};
inline constexpr R kSetup{1736, 632, 126, 192};
inline constexpr R kUseRaceDefault{1445, 912, 624, 72};
inline constexpr R kSelect{1445, 1016, 624, 96};
}

namespace s80_replays {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kBack{0, 0, 96, 96};
inline constexpr R kReplaysAndSaves{112, 28, 600, 40};
inline constexpr R kIdentityPlate{1709, 12, 360, 72};
inline constexpr R kReplays{64, 112, 240, 64};
inline constexpr R kSavedGames{312, 112, 240, 64};
inline constexpr R kRowDateMapMatchup{64, 208, 1349, 72};
inline constexpr R kRegion{64, 284, 1349, 72};
inline constexpr R kRegion2{64, 360, 1349, 72};
inline constexpr R kRegion3{64, 436, 1349, 72};
inline constexpr R kRegion4{64, 512, 1349, 72};
inline constexpr R kRegion5{64, 588, 1349, 72};
inline constexpr R kRegion6{64, 664, 1349, 72};
inline constexpr R kRegion7{64, 740, 1349, 72};
inline constexpr R kRegion8{64, 816, 1349, 72};
inline constexpr R kRegion9{64, 892, 1349, 72};
inline constexpr R kRegion10{64, 968, 1349, 72};
inline constexpr R kScrollbar{1421, 208, 8, 904};
inline constexpr R kMapPreview{1445, 208, 624, 468};
inline constexpr R kMeta{1445, 700, 624, 160};
inline constexpr R kDelete{1445, 928, 300, 64};
inline constexpr R kOpenFolder{1769, 928, 300, 64};
inline constexpr R kWatchResume{1445, 1016, 624, 96};
}

namespace s82_replay_bar {
inline constexpr R kReplayBarOverThe{0, 0, 2133, 64};
inline constexpr R kReplay{24, 18, 540, 28};
inline constexpr R kPositionTickTotal{600, 28, 933, 8};
inline constexpr R kMmSsMmSs{1549, 18, 160, 28};
inline constexpr R kLeave{1909, 8, 200, 48};
}

namespace s90_about {
inline constexpr R kTopBar{0, 0, 2133, 96};
inline constexpr R kHintBar{0, 1144, 2133, 56};
inline constexpr R kBack{0, 0, 96, 96};
inline constexpr R kAbout{112, 28, 600, 40};
inline constexpr R kIdentityPlate{1709, 12, 360, 72};
inline constexpr R kLicenceNotices{64, 128, 1349, 984};
inline constexpr R kEmblem256{1445, 128, 624, 984};
}

}
