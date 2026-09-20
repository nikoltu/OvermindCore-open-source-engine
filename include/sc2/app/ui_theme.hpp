#pragma once
#include <cstdint>

namespace sc2::app::theme {

struct Rgb {
    std::uint8_t r, g, b;
};

inline constexpr Rgb kGround{8, 2, 3};
inline constexpr Rgb kGroundDeep{11, 3, 4};
inline constexpr Rgb kGroundInk{10, 2, 3};
inline constexpr Rgb kVignetteCore{28, 7, 9};
inline constexpr Rgb kSurface{21, 6, 7};
inline constexpr Rgb kSurfaceDim{16, 4, 5};
inline constexpr Rgb kSurfaceCtl{26, 7, 9};
inline constexpr Rgb kSurfaceRaised{32, 9, 12};
inline constexpr Rgb kSurfaceHi{44, 14, 20};

inline constexpr Rgb kMenuBtnTop{40, 12, 16};
inline constexpr Rgb kMenuBtnBottom{28, 7, 9};

inline constexpr Rgb kBorderFaint{51, 16, 20};
inline constexpr Rgb kBorderRow{40, 12, 18};
inline constexpr Rgb kBorder{78, 26, 32};
inline constexpr Rgb kBorderInset{62, 20, 24};
inline constexpr Rgb kBorderCtl{106, 32, 40};
inline constexpr Rgb kBorderStrong{138, 44, 52};
inline constexpr Rgb kBorderSel{200, 48, 58};
inline constexpr Rgb kBorderAccent{255, 74, 52};

inline constexpr Rgb kAccent{255, 58, 42};
inline constexpr Rgb kAccentHot{255, 138, 120};
inline constexpr Rgb kAccentDeep{200, 48, 58};
inline constexpr Rgb kAccentPure{255, 42, 26};
inline constexpr Rgb kTextHi{255, 240, 236};
inline constexpr Rgb kTextOnPrimary{255, 240, 234};
inline constexpr Rgb kTextBody{242, 220, 218};
inline constexpr Rgb kTextCell{232, 188, 184};
inline constexpr Rgb kTextMuted{184, 146, 154};
inline constexpr Rgb kTextMuted2{168, 126, 134};
inline constexpr Rgb kTextFaint{168, 90, 98};
inline constexpr Rgb kWarn{232, 200, 106};
inline constexpr Rgb kOk{106, 222, 140};
inline constexpr Rgb kErr{217, 112, 112};

inline constexpr Rgb kPrimaryTop{160, 24, 24};
inline constexpr Rgb kPrimaryBottom{106, 14, 16};

inline constexpr int kNavBarH = 72;
inline constexpr int kFooterH = 44;
inline constexpr int kPagePad = 18;
inline constexpr int kBorderW = 1;
inline constexpr float kDisabledOpacity = 0.40f;

inline constexpr Rgb kBandGround{14, 4, 6};
inline constexpr Rgb kNavTop{18, 6, 8};
inline constexpr Rgb kNavBottom{8, 12, 20};
inline constexpr Rgb kSurfaceSel{60, 16, 22};
inline constexpr Rgb kSurfaceRow{26, 8, 11};
inline constexpr Rgb kBorderVersion{82, 28, 34};
inline constexpr Rgb kBorderSlot{92, 32, 40};
inline constexpr Rgb kBorderQuit{122, 64, 64};
inline constexpr Rgb kBorderWarn{138, 106, 48};
inline constexpr Rgb kTextDisabled{122, 64, 72};
inline constexpr Rgb kTextPlaceholder{138, 80, 88};
inline constexpr Rgb kTextIcon{232, 168, 164};
inline constexpr Rgb kProgressLow{138, 32, 40};

inline constexpr Rgb kVignetteMidCool{10, 15, 26};
inline constexpr Rgb kVignetteLobby{24, 6, 8};
inline constexpr Rgb kVignetteLoad{40, 12, 18};
inline constexpr Rgb kVignettePause{36, 16, 20};

inline constexpr Rgb kInsetHi{255, 140, 120};
inline constexpr Rgb kInsetPrimary{255, 150, 130};

inline constexpr std::uint8_t kAlphaBand = 235;
inline constexpr std::uint8_t kAlphaStrip = 230;
inline constexpr std::uint8_t kAlphaPanel = 217;
inline constexpr std::uint8_t kAlphaPanelDim = 204;
inline constexpr std::uint8_t kAlphaNavTop = 245;
inline constexpr std::uint8_t kAlphaNavBottom = 224;
inline constexpr std::uint8_t kAlphaScrim = 140;
inline constexpr std::uint8_t kAlphaVersion = 179;
inline constexpr std::uint8_t kAlphaCard = 230;
inline constexpr std::uint8_t kAlphaInsetBtn = 36;
inline constexpr std::uint8_t kAlphaInsetPrimary = 89;
inline constexpr std::uint8_t kAlphaCatActive = 128;

enum class Face : std::uint8_t { Standard = 0, HeaderExtended = 1 };
struct Type {
    int px;
    float track;
    Face face = Face::Standard;
    const char* style = nullptr;
};
inline constexpr Type kTypeWordmark{62, 0.14f, Face::HeaderExtended};
inline constexpr Type kTypeWordmarkAccent{62, 0.34f, Face::HeaderExtended};
inline constexpr Type kTypeWordmarkSm{44, 0.14f, Face::HeaderExtended};
inline constexpr Type kTypeWordmarkSmAccent{44, 0.32f, Face::HeaderExtended};
inline constexpr Type kTypeLoadTitle{40, 0.14f, Face::HeaderExtended, "GlueTitle"};
inline constexpr Type kTypeVs{34, 0.0f, Face::HeaderExtended};
inline constexpr Type kTypeCountdown{24, 0.0f, Face::Standard, "GlueTitleSmall"};
inline constexpr Type kTypeVersionBig{22, 0.10f};
inline constexpr Type kTypeDetailTitle{22, 0.0f, Face::HeaderExtended, "GlueTitle"};
inline constexpr Type kTypePanelTitle{20, 0.18f, Face::HeaderExtended, "GlueTitle"};
inline constexpr Type kTypeDialogTitle{20, 0.26f, Face::HeaderExtended, "GlueDialogTitle"};
inline constexpr Type kTypeSlotName{20, 0.0f, Face::Standard, "GlueLabel"};
inline constexpr Type kTypeSectionHead{17, 0.16f, Face::HeaderExtended, "GlueLabel"};
inline constexpr Type kTypePrimaryLg{16, 0.22f, Face::Standard, "GlueButton"};
inline constexpr Type kTypeNavItem{15, 0.14f, Face::HeaderExtended, "GlueButton"};
inline constexpr Type kTypeTileTitle{15, 0.10f, Face::Standard, "GlueLabel"};
inline constexpr Type kTypeSecondaryLg{15, 0.16f, Face::Standard, "GlueButton"};
inline constexpr Type kTypeMenuBtn{14, 0.18f, Face::Standard, "GlueButton"};
inline constexpr Type kTypeSubHead{14, 0.14f, Face::Standard, "GlueLabel"};
inline constexpr Type kTypePrimaryMd{14, 0.20f, Face::Standard, "GlueButton"};
inline constexpr Type kTypeScreenTitle{13, 0.20f, Face::HeaderExtended, "GlueTitle"};
inline constexpr Type kTypePrimarySm{13, 0.16f, Face::Standard, "GlueButton"};
inline constexpr Type kTypeSecondary{13, 0.12f, Face::Standard, "GlueButton"};
inline constexpr Type kTypeSecondaryMd{13, 0.14f, Face::Standard, "GlueButton"};
inline constexpr Type kTypeTab{13, 0.12f, Face::Standard, "GlueButton"};
inline constexpr Type kTypeBody{13, 0.0f, Face::Standard, "GlueDescription"};
inline constexpr Type kTypeCat{12, 0.10f, Face::Standard, "GlueButtonSmall"};
inline constexpr Type kTypeSmall{12, 0.0f, Face::Standard, "GlueDescription"};
inline constexpr Type kTypeVersionStage{12, 0.24f};
inline constexpr Type kTypeMicro{11, 0.0f, Face::Standard, "GlueDescription"};
inline constexpr Type kTypeGo{11, 0.10f, Face::Standard, "GlueValue"};
inline constexpr Type kTypeFine{10, 0.05f, Face::Standard, "GlueDescription"};
inline constexpr Type kTypeVersionFine{10, 0.04f};
inline constexpr Type kTypeCaption{9, 0.06f, Face::Standard, "GlueDescription"};

inline constexpr int kDesignRefW = 1920;
inline constexpr int kDesignRefH = 1080;
inline int scaled(int authored, std::uint32_t view_h) {
    const long long n = static_cast<long long>(authored) * static_cast<long long>(view_h);
    return static_cast<int>((n + kDesignRefH / 2) / kDesignRefH);
}

inline constexpr int kFlowStripH = 40;
inline constexpr int kScreenPad = 26;
inline constexpr int kNavItemPadX = 26;
inline constexpr int kNavDividerH = 44;

inline constexpr int kMenuBtnW = 320;
inline constexpr int kMenuBtnH = 46;
inline constexpr int kMenuBtnGap = 10;
inline constexpr int kDialogW = 420;
inline constexpr int kDialogPadX = 50;
inline constexpr int kDialogPadTop = 26;
inline constexpr int kDialogPadBottom = 30;
inline constexpr int kDialogTitleGap = 12;

inline constexpr int kPrimaryW = 220;
inline constexpr int kPrimaryH = 54;
inline constexpr int kActionGap = 16;
inline constexpr int kActionH = 48;
inline constexpr int kActionPadX = 30;

inline constexpr int kLobbySlotsTop = 100;
inline constexpr int kLobbySlotsW = 1240;
inline constexpr int kLobbyOptionsTop = 420;
inline constexpr int kLobbyActionsBottom = 34;
inline constexpr int kSlotRowGap = 6;
inline constexpr int kSlotPadX = 10;
inline constexpr int kSlotPadY = 7;
inline constexpr int kSlotGapX = 8;
inline constexpr int kPulldownH = 34;
inline constexpr int kPulldownPadX = 10;
inline constexpr int kColPlayer = 240;
inline constexpr int kColRace = 130;
inline constexpr int kColColor = 110;
inline constexpr int kColTeam = 90;
inline constexpr int kColHandicap = 120;
inline constexpr int kColAiDiff = 170;
inline constexpr int kColAiBuild = 190;
inline constexpr int kMapInfoW = 560;
inline constexpr int kMapInfoPad = 16;
inline constexpr int kMinimapBox = 220;
inline constexpr int kSwatch = 14;

inline constexpr int kEmblemLarge = 300;
inline constexpr int kEmblemGapLarge = 34;
inline constexpr int kMainEmblemLeft = 120;
inline constexpr int kMainEmblemTop = 150;
inline constexpr int kMainContentTop = 480;
inline constexpr int kMainContentW = 560;
inline constexpr int kVersionRight = 26;
inline constexpr int kVersionBottom = 64;
inline constexpr int kVersionPadX = 18;
inline constexpr int kVersionPadY = 12;
inline constexpr int kVersionRimW = 3;

inline constexpr int kEmblemSmall = 132;
inline constexpr int kEmblemGapSmall = 22;
inline constexpr int kHomeLeft = 100;
inline constexpr int kHomeEmblemTop = 108;
inline constexpr int kHomeCardTop = 300;
inline constexpr int kHomeCardW = 720;
inline constexpr int kHomeCardPadX = 26;
inline constexpr int kHomeCardPadY = 22;
inline constexpr int kHomeCardGap = 24;
inline constexpr int kHomeThumb = 120;
inline constexpr int kHomeTilesTop = 470;
inline constexpr int kTileW = 340;
inline constexpr int kTileH = 190;
inline constexpr int kTileGap = 18;
inline constexpr int kTilePadX = 20;
inline constexpr int kTilePadY = 18;

inline constexpr int kLoadTitleTop = 180;
inline constexpr int kLoadVsTop = 330;
inline constexpr int kLoadPlate = 84;
inline constexpr int kLoadVsGap = 60;
inline constexpr int kLoadPlateGap = 12;
inline constexpr int kLoadPlayerBarW = 200;
inline constexpr int kLoadPlayerBarH = 5;
inline constexpr int kLoadBarW = 760;
inline constexpr int kLoadBarH = 10;
inline constexpr int kLoadBarBottom = 200;
inline constexpr int kLoadTipBottom = 120;

inline constexpr int kOptionsW = 1400;
inline constexpr int kOptionsH = 880;
inline constexpr int kOptionsTop = 90;
inline constexpr int kOptionsRailW = 250;
inline constexpr int kOptionsRailPadY = 18;
inline constexpr int kCatPadX = 16;
inline constexpr int kCatPadY = 10;
inline constexpr int kCatRuleW = 2;
inline constexpr int kOptionsPadX = 34;
inline constexpr int kOptionsPadY = 22;
inline constexpr int kRowH = 40;

inline constexpr int kTabsTop = 56;
inline constexpr int kTabPadX = 22;
inline constexpr int kTabPadY = 9;
inline constexpr int kTabRuleH = 2;
inline constexpr int kCustomListTop = 116;
inline constexpr int kCustomListW = 1150;
inline constexpr int kCustomBottomGap = 96;
inline constexpr int kCustomDetailW = 690;
inline constexpr int kCustomDetailPad = 18;
inline constexpr int kCustomPreview = 280;
inline constexpr int kMapRowPadX = 12;
inline constexpr int kMapRowPadY = 8;
inline constexpr int kMapRowGap = 6;
inline constexpr int kMapThumb = 56;
inline constexpr int kFilterH = 36;

inline constexpr int kFooterMark = 20;

}
