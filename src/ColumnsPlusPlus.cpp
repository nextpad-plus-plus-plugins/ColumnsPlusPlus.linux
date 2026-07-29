// Columns++ for Notepad++ — macOS port.
// ColumnsPlusPlus.mm — plugin entry points (the 5 exported symbols), the menu
// (FuncItem array) and beNotified dispatch.
//
// Mirrors the Windows Plugin.cpp: the same command set in the same menu order,
// a notification re-entrancy guard (bypassNotifications), and the SCN_MODIFIED /
// SCN_UPDATEUI / SCN_ZOOM + NPPN_* routing that drives elastic tabstops. The
// per-command wrapper resolves the active Scintilla, suppresses notifications,
// runs the command, then re-enables notifications.

#include "CppData.h"

#include <cstring>

static ColumnsPlusPlusData data;

static bool bypassNotifications = false;
static bool startupOrShutdown   = true;

// SCN_ codes that aren't named in the host header.
#ifndef SCN_MODIFIED
#define SCN_MODIFIED 2008
#define SCN_UPDATEUI 2007
#define SCN_ZOOM     2018
#endif

// Run a command with the active Scintilla resolved and notifications suppressed.
static void cmdWrap(void (ColumnsPlusPlusData::*fn)()) {
    bypassNotifications = true;
    data.updateActiveScintilla();
    (data.*fn)();
    bypassNotifications = false;
}

// Menu definition. Order matches the Windows MenuDefinition. Separators have an
// empty _itemName (the host renders them as separators). Indices into funcItem[]
// are kept so we can record cmdIDs for the checkable items at NPPN_READY.
enum {
    M_ELASTIC = 0, M_PROFILE, M_SEP1,
    M_SEARCH, M_ADD, M_AVERAGE, M_CALC, M_TIMESTAMPS, M_SEP2,
    M_ALIGN_L, M_ALIGN_R, M_ALIGN_N, M_ALIGN_C, M_SEP3,
    M_SORT_AB, M_SORT_DB, M_SORT_AL, M_SORT_DL, M_SORT_AN, M_SORT_DN, M_SORT_C, M_SEP4,
    M_TABS2SP, M_SV2TABS, M_TABS2SV, M_SEP5,
    M_DECSEP, M_TIMEFMT, M_OPTIONS, M_SEP6,
    M_SEL_UP, M_SEL_LEFT, M_SEL_RIGHT, M_SEL_DOWN, M_SEL_ENCLOSE, M_SEL_EXTEND, M_SEP7,
    M_ABOUT,
    M_COUNT
};

static FuncItem funcItem[M_COUNT];

static void fnElastic()    { cmdWrap(&ColumnsPlusPlusData::toggleElasticEnabled); }
static void fnProfile()    { cmdWrap(&ColumnsPlusPlusData::showElasticProfile); }
static void fnSearch()     { cmdWrap(&ColumnsPlusPlusData::showSearchDialog); }
static void fnAdd()        { cmdWrap(&ColumnsPlusPlusData::addNumbers); }
static void fnAverage()    { cmdWrap(&ColumnsPlusPlusData::averageNumbers); }
static void fnCalc()       { cmdWrap(&ColumnsPlusPlusData::calculate); }
static void fnTimestamps() { cmdWrap(&ColumnsPlusPlusData::convertTimestamps); }
static void fnAlignL()     { cmdWrap(&ColumnsPlusPlusData::alignLeft); }
static void fnAlignR()     { cmdWrap(&ColumnsPlusPlusData::alignRight); }
static void fnAlignN()     { cmdWrap(&ColumnsPlusPlusData::alignNumeric); }
static void fnAlignC()     { cmdWrap(&ColumnsPlusPlusData::alignCustom); }
static void fnSortAB()     { cmdWrap(&ColumnsPlusPlusData::sortAscendingBinary); }
static void fnSortDB()     { cmdWrap(&ColumnsPlusPlusData::sortDescendingBinary); }
static void fnSortAL()     { cmdWrap(&ColumnsPlusPlusData::sortAscendingLocale); }
static void fnSortDL()     { cmdWrap(&ColumnsPlusPlusData::sortDescendingLocale); }
static void fnSortAN()     { cmdWrap(&ColumnsPlusPlusData::sortAscendingNumeric); }
static void fnSortDN()     { cmdWrap(&ColumnsPlusPlusData::sortDescendingNumeric); }
static void fnSortC()      { cmdWrap(&ColumnsPlusPlusData::sortCustom); }
static void fnTabs2Sp()    { cmdWrap(&ColumnsPlusPlusData::tabsToSpaces); }
static void fnSV2Tabs()    { cmdWrap(&ColumnsPlusPlusData::separatedValuesToTabs); }
static void fnTabs2SV()    { cmdWrap(&ColumnsPlusPlusData::tabsToSeparatedValues); }
static void fnDecSep()     { cmdWrap(&ColumnsPlusPlusData::toggleDecimalSeparator); }
static void fnTimeFmt()    { cmdWrap(&ColumnsPlusPlusData::showTimeFormatsDialog); }
static void fnOptions()    { cmdWrap(&ColumnsPlusPlusData::showOptionsDialog); }
static void fnAbout()      { cmdWrap(&ColumnsPlusPlusData::showAboutDialog); }
static void fnSelUp()      { cmdWrap(&ColumnsPlusPlusData::selectUp); }
static void fnSelLeft()    { cmdWrap(&ColumnsPlusPlusData::selectLeft); }
static void fnSelRight()   { cmdWrap(&ColumnsPlusPlusData::selectRight); }
static void fnSelDown()    { cmdWrap(&ColumnsPlusPlusData::selectDown); }
static void fnSelEnclose() { cmdWrap(&ColumnsPlusPlusData::selectEnclose); }
static void fnSelExtend()  { cmdWrap(&ColumnsPlusPlusData::selectExtend); }

static void setItem(int i, const char* name, void(*fn)()) {
    memset(&funcItem[i], 0, sizeof(FuncItem));
    if (name) strncpy(funcItem[i]._itemName, name, NPP_MENU_ITEM_SIZE - 1);
    funcItem[i]._pFunc = fn;
}

static void buildMenu() {
    setItem(M_ELASTIC,   "Elastic tabstops", fnElastic);
    setItem(M_PROFILE,   "Profile...", fnProfile);
    setItem(M_SEP1,      "-", nullptr);
    setItem(M_SEARCH,    "Search...", fnSearch);
    setItem(M_ADD,       "Add numbers...", fnAdd);
    setItem(M_AVERAGE,   "Average numbers...", fnAverage);
    setItem(M_CALC,      "Calculate...", fnCalc);
    setItem(M_TIMESTAMPS,"Timestamps...", fnTimestamps);
    setItem(M_SEP2,      "-", nullptr);
    setItem(M_ALIGN_L,   "Align left", fnAlignL);
    setItem(M_ALIGN_R,   "Align right", fnAlignR);
    setItem(M_ALIGN_N,   "Align numeric", fnAlignN);
    setItem(M_ALIGN_C,   "Align...", fnAlignC);
    setItem(M_SEP3,      "-", nullptr);
    setItem(M_SORT_AB,   "Sort ascending (binary)", fnSortAB);
    setItem(M_SORT_DB,   "Sort descending (binary)", fnSortDB);
    setItem(M_SORT_AL,   "Sort ascending (locale)", fnSortAL);
    setItem(M_SORT_DL,   "Sort descending (locale)", fnSortDL);
    setItem(M_SORT_AN,   "Sort ascending (numeric)", fnSortAN);
    setItem(M_SORT_DN,   "Sort descending (numeric)", fnSortDN);
    setItem(M_SORT_C,    "Sort...", fnSortC);
    setItem(M_SEP4,      "-", nullptr);
    setItem(M_TABS2SP,   "Convert tabs to spaces", fnTabs2Sp);
    setItem(M_SV2TABS,   "Convert separated values to tabs...", fnSV2Tabs);
    setItem(M_TABS2SV,   "Convert tabs to separated values...", fnTabs2SV);
    setItem(M_SEP5,      "-", nullptr);
    setItem(M_DECSEP,    "Decimal separator is comma", fnDecSep);
    setItem(M_TIMEFMT,   "Time formats...", fnTimeFmt);
    setItem(M_OPTIONS,   "Options...", fnOptions);
    setItem(M_SEP6,      "-", nullptr);
    setItem(M_SEL_UP,      "Select Up", fnSelUp);
    setItem(M_SEL_LEFT,    "Select Left", fnSelLeft);
    setItem(M_SEL_RIGHT,   "Select Right", fnSelRight);
    setItem(M_SEL_DOWN,    "Select Down", fnSelDown);
    setItem(M_SEL_ENCLOSE, "Enclose Selection", fnSelEnclose);
    setItem(M_SEL_EXTEND,  "Extend Selection", fnSelExtend);
    setItem(M_SEP7,      "-", nullptr);
    setItem(M_ABOUT,     "Help/About...", fnAbout);
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------
extern "C" NPP_EXPORT void setInfo(LinuxHostNppData hostData) {
    cpBridgeInit(&hostData);
    data.nppData._nppHandle             = kHandleNpp;
    data.nppData._scintillaMainHandle   = kHandleScintillaMain;
    data.nppData._scintillaSecondHandle = kHandleScintillaSub;
    data.nppData._sendMessage           = cpSendMessage;
    data.activeScintilla = data.nppData._scintillaMainHandle;
    data.initializeBuiltinElasticTabstopsProfiles();
    data.loadConfiguration();
    buildMenu();
}

extern "C" NPP_EXPORT const char* getName() { return "Columns++"; }

extern "C" NPP_EXPORT FuncItem* getFuncsArray(int* n) {
    *n = M_COUNT;
    return funcItem;
}

extern "C" NPP_EXPORT void beNotified(SCNotification* np) {
    if (bypassNotifications) return;
    bypassNotifications = true;

    switch (np->nmhdr.code) {

    case SCN_MODIFIED:
        if (!startupOrShutdown) data.scnModified(np);
        break;

    case SCN_UPDATEUI:
        if (!startupOrShutdown) data.scnUpdateUI(np);
        break;

    case SCN_ZOOM:
        if (!startupOrShutdown) data.scnZoom(np);
        break;

    case NPPN_READY:
        data.aboutMenuItem            = funcItem[M_ABOUT]._cmdID;
        data.decimalSeparatorMenuItem = funcItem[M_DECSEP]._cmdID;
        data.elasticEnabledMenuItem   = funcItem[M_ELASTIC]._cmdID;
        startupOrShutdown = false;
        data.updateActiveScintilla();
        data.bufferActivated();
        break;

    case NPPN_BUFFERACTIVATED:
        if (!startupOrShutdown) {
            data.updateActiveScintilla();
            data.bufferActivated();
        }
        break;

    case NPPN_FILESAVED:
        if (!startupOrShutdown) data.fileSaved();
        break;

    case NPPN_SHUTDOWN:
        startupOrShutdown = true;
        data.saveConfiguration();
        break;

    default:
        break;
    }

    bypassNotifications = false;
}

extern "C" NPP_EXPORT intptr_t messageProc(uint32_t, uintptr_t, intptr_t) { return 1; }

extern "C" NPP_EXPORT int isUnicode() { return 1; }
