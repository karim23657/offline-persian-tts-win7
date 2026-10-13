/*
 * tts_gui.exe - a native Windows 7 GUI for the local Persian TTS.
 *
 * Design notes
 * ------------
 * Native Win32 rather than Tkinter on purpose: Windows 7 cannot run any Python
 * newer than 3.8, and requiring an interpreter would undercut the point of a
 * package that is meant to just run.  This links the sherpa-onnx C API
 * directly, so there is no extra dependency at all.
 *
 * What this file grew into, beyond "a text box and a button":
 *
 *  - Tabs: text+speech, dictionary, settings, model download.  The window is
 *    still one small native window; each tab is a page of child controls that
 *    is shown or hidden on switch.
 *
 *  - fa/en language switch.  Every visible string goes through S(id).  The
 *    Persian mode flips the window and the text box to RTL reading order and
 *    uses the Sahel font (embedded at build time via AddFontResourceW from
 *    sahel.ttf next to the exe); English uses Segoe UI and LTR.
 *
 *  - Model lifecycle: models are loaded once through the shared engine cache
 *    and reused for every generation.  Selecting a *different* model evicts
 *    the previous one (TtsEngineEvict) so two 63 MB sessions never sit in
 *    memory together by accident.  Preload happens in the background so the
 *    first Generate feels instant.
 *
 *  - A pronunciation dictionary (user.dict): whole-word substitution applied
 *    before synthesis.  This is the honest replacement for a "phoneme editor",
 *    which the sherpa-onnx C API simply does not offer (its `lexicon` field is
 *    ignored whenever `data_dir` is set - always the case for the piper models
 *    here).  Entries added in this tab are shared with the web server and
 *    say.exe because they all read the same package-root user.dict.
 *
 *  - A model download tab that reads the package-root models.json (same file
 *    the web UI shows) and fetches files with WinInet, honouring the "no
 *    internet on Win7 without hassle" reality: plain HTTPS, progress shown,
 *    cancel supported.
 *
 * Everything user-facing is wide (W) API.  Persian typed into the text box is
 * UTF-16 already, and it is converted to UTF-8 (what the sherpa-onnx API
 * documents) only at the boundary.  Synthesis runs on a worker thread so the
 * window never paints "not responding".
 */

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sherpa-onnx/c-api/c-api.h"
#include "tts_engine.h"
#include "dict.h"
#include "json_min.h"

#ifndef IDC_ARROW
#define IDC_ARROW 32512
#endif

/* ------------------------------------------------------------------ ids */

/* Tabs */
#define IDC_TABS        900

/* Page 0: text & speech */
#define ID_MODEL        1005
#define ID_SPEED        1008
#define ID_TEXT         1006
#define ID_GENERATE     1001
#define ID_PLAY         1002
#define ID_STOP         1003
#define ID_SAVEAS       1010
#define ID_OPEN         1004
#define ID_PROGRESS     1009
#define ID_STATUS       1007

/* Page 1: dictionary */
#define ID_DICT_HINT    1200
#define ID_DICT_WORD    1201
#define ID_DICT_REPL    1202
#define ID_DICT_ADD     1203
#define ID_DICT_DEL     1204
#define ID_DICT_LIST    1205

/* Page 2: settings */
#define ID_SET_LANG     1300
#define ID_SET_LANG_LBL 1301
#define ID_SET_SID      1302
#define ID_SET_SID_LBL  1303
#define ID_SET_ABOUT    1304
#define ID_SET_MODEL_L  1305   /* (re)select the model used on page 0 */

/* Page 3: model download */
#define ID_DL_LIST      1400
#define ID_DL_FETCH     1401
#define ID_DL_CANCEL    1402
#define ID_DL_REFRESH   1403
#define ID_DL_STATUS    1404
#define ID_DL_PROGRESS  1405
#define ID_DL_OPENMODELS 1406

/* Static-label IDs.  These must NOT be derived as "control id - 1": that
 * collides with neighbouring controls (e.g. ID_TEXT-1 == ID_MODEL) and makes
 * GetDlgItem() return the wrong window, which silently broke label layout and
 * per-tab visibility.  Every label gets its own explicit id instead. */
#define ID_MODEL_LBL    1041
#define ID_TEXT_LBL     1042
#define ID_DICT_WORD_LBL 1210
#define ID_DICT_REPL_LBL 1211
#define ID_DL_HINT      1410

/* Accelerator */
#define IDA_SELALL      2001
#define IDA_NEXTTAB     2002
#define IDA_PREVTAB     2003

/* Worker -> UI messages */
#define WM_APP_DONE     (WM_APP + 1)
#define WM_APP_FAIL     (WM_APP + 2)
#define WM_APP_PROG     (WM_APP + 3)
#define WM_APP_PRELOADED (WM_APP + 4)
#define WM_APP_DICT     (WM_APP + 5)
#define WM_APP_DL       (WM_APP + 6)

#define MAX_PATHLEN 4096
#define MAX_DL_ITEMS 128

/* ----------------------------------------------------------------- i18n */

enum {
    STR_NONE = 0,
    STR_TITLE, STR_TAB_TEXT, STR_TAB_DICT, STR_TAB_SETTINGS, STR_TAB_DL,
    STR_MODEL, STR_SPEED, STR_TEXT_LBL,
    STR_GEN, STR_PLAY, STR_STOP, STR_SAVEAS, STR_OPEN, STR_OPEN_MODELS,
    STR_READY, STR_LOADING_MODEL, STR_WARM, STR_DONE, STR_PLAYING,
    STR_NO_MODEL, STR_EMPTY_TEXT, STR_NO_TEXT_FILE, STR_FAIL, STR_CANCELLED,
    STR_CONVERT_ERR, STR_WRITE_ERR, STR_NO_AUDIO, STR_SAVE_FIRST,
    STR_DICT_HINT, STR_DICT_WORD, STR_DICT_REPL, STR_DICT_ADD, STR_DICT_DEL,
    STR_DICT_EMPTY, STR_DICT_SAVED, STR_DICT_REMOVED, STR_DICT_FULL,
    STR_SET_LANG, STR_SET_SID, STR_SET_ABOUT, STR_SET_MODEL,
    STR_DL_HINT, STR_DL_FETCH, STR_DL_CANCEL, STR_DL_REFRESH, STR_DL_STATUS,
    STR_DL_NONE, STR_DL_BUSY, STR_DL_FAIL, STR_DL_DONE, STR_DL_EXTRACTING,
    STR_DL_NOMODELSJSON, STR_DL_NOTAR, STR_DL_CANCELLED,
    STR_CLOSE_PROMPT, STR_ACC_H,
    STR__COUNT
};

/*
 * The two columns are fa and en.  Every new string must land in both: a
 * missing English string falls back to Persian, which is visible but never
 * crashes.
 */
static const wchar_t *g_str[STR__COUNT][2] = {
    {NULL, NULL},
    /* STR_TITLE */
    {L"\x0648\x06cc\x0646\x37-tts  \x2014  \x06af\x062a\x0627\x0631 \x0641\x0627\x0631\x0633\x06cc",
     L"win7-tts  -  local Persian text to speech"},
    /* STR_TAB_TEXT */
    {L"\x0645\x062a\x0646 \x0648 \x0633\x062e\x062a",
     L"Text && speech"},
    /* STR_TAB_DICT */
    {L"\x0648\x0627\x0698\x0647\u200c\x0646\x0627\x0645\x0647",
     L"Dictionary"},
    /* STR_TAB_SETTINGS */
    {L"\x062a\x0646\x0638\x06cc\x0645\x0627\x062a",
     L"Settings"},
    /* STR_TAB_DL */
    {L"\x062f\x0627\x0646\x0644\x0648\x062f \x0645\x062f\x0644",
     L"Model download"},
    /* STR_MODEL */
    {L"\x0645\x062f\x0644", L"Model"},
    /* STR_SPEED */
    {L"\x0633\x0631\x0639\x062a", L"Speed"},
    /* STR_TEXT_LBL */
    {L"\x0645\x062a\x0646", L"Text"},
    /* STR_GEN */
    {L"\x0633\x062e\x062a \x0648 \x0630\x062e\x06cc\x0631\x0647\u200c\x06cc",
     L"Generate and save"},
    /* STR_PLAY */
    {L"\x067e\x062e\x0634", L"Play"},
    /* STR_STOP */
    {L"\x062a\x0648\x0642\x0641", L"Stop"},
    /* STR_SAVEAS */
    {L"\x0630\x062e\x06cc\x0631\x0647 \x062f\x0631\u200c", L"Save as..."},
    /* STR_OPEN */
    {L"\x0628\x0627\x0632 \x06a9\x0631\x062f\x0646 \u200c\x067e\x0648\x0634\x0647",
     L"Open folder"},
    /* STR_OPEN_MODELS */
    {L"\x067e\x0648\x0634\x0647 \x0645\x062f\x0644\u200c\x0647\x0627", L"Models folder"},
    /* STR_READY */
    {L"آماده. متن را بنویسید و روی دکمهٔ «ساخت و ذخیره» بزنید.",
     L"Ready. Type your text and press Generate."},
    /* STR_LOADING_MODEL */
    {L"\x062f\x0631 \x062d\x0627\x0644 \x0628\x0627\x0631\x06af\x0630\x0627\x0631\x06cc \x0645\x062f\x0644\u200c\u0627\x0646\x062f\x0627",
     L"Loading the model..."},
    /* STR_WARM */
    {L"\x0645\x062f\x0644 \x0622\x0645\x0627\x062f\x0647 \x0627\x0633\x062a \x0648 \u0628\x0631\x0627\x06cc \u0647\x0645\x0647 \u0633\x062e\x062a\u200c\x0647\x0627 \x0622\x0645\x0627\x062f\x0647 \x0627\x0633\x062a.",
     L"The model is warm - ready for instant speech."},
    /* STR_DONE */
    {L"\x0627\x0646\x062c\x0627\x0645 \u0634\x062f: %d \x062b\x0627\x0646\x06cc\x0647\u200c\x0627\x064b \u062f\x0631 %d Hz",
     L"Done: %d s of audio at %d Hz"},
    /* STR_PLAYING */
    {L"\x062f\x0631 \x062d\x0627\x0644 \u067e\x062e\x0634...", L"Playing..."},
    /* STR_NO_MODEL */
    {L"\x062d\x062f\x06cc \u0645\x062f\x0644\x06cc \u0646\x0635\x0628 \u0646\x0634\x062f\x0647 \x0627\x0633\x062a. \x0627\x0632 \u062a\x0627\x0628 \u062f\x0627\x0646\u0644\u0648\x062f \u06cc\x06a9\x06cc \u0631\x0627 \u0628\x06af\x06cc\x0631\x06cc\x062f.",
     L"No model is installed. Download one from the Model download tab."},
    /* STR_EMPTY_TEXT */
    {L"\x0627\x0648\x0644\x0627 \u06cc\x06a9 \u0645\x062a\x0646 \u0628\x0646\x0648\x06cc\x0633\x06cc\x062f.",
     L"Type some text first."},
    /* STR_NO_TEXT_FILE */
    {L"\x0641\x0627\x06cc\x0644 \u0645\x062a\x0646 \u067e\x06cc\x062f\x0627 \u0646\x0634\x062f.", L"The text file is empty."},
    /* STR_FAIL */
    {L"\x0633\x062e\x062a \u0627\x0646\x062c\x0627\x0645 \u0646\x0634\x062f.", L"Generation failed."},
    /* STR_CANCELLED */
    {L"\x0644\x063a\x0648 \u0634\x062f.", L"Cancelled."},
    /* STR_CONVERT_ERR */
    {L"\x062a\x062d\x0648\x06cc\x0644 \u0645\x062a\x0646 \u0645\x0645\x06a9\x0646 \u0646\x0634\x062f.",
     L"Could not convert the text."},
    /* STR_WRITE_ERR */
    {L"\x062b\x0628\x062a \u0641\x0627\x06cc\x0644 .wav \u0645\x0645\x06a9\x0646 \u0646\x0628\x0648\x062f.",
     L"Could not write the .wav file."},
    /* STR_NO_AUDIO */
    {L"\x0628\x0647 \u0635\x062f\x0627\x06cc \u0647\x06cc\x0686 \u062e\x0631\x0648\x062c\x06cc \u0646\x0634\x062f.",
     L"No audio was produced."},
    /* STR_SAVE_FIRST */
    {L"\x0627\x0648\x0644\x0627 \u0633\x062e\x062a\x200c\u0647\x0627\x06cc\x06cc \u0628\x0633\x0627\x0632\x06cc\x062f.",
     L"Generate some speech first."},
    /* STR_DICT_HINT */
    {L"\u0627\x06af\x0631 \u06a9\x0644\x0645\x0647\u200c\x0627\x06cc \u0628\x0627 \u0645\x062a\x0646 \u0635\x062d\x06cc\x062d \u062f\x0627\x0631\x06cc \u062a\x0648\x062c\x0647 \u06cc\x0627\x0641\x062a\x0647 \u0634\x0648\x062f \u0627\x0633\x062a\u060c \u0622\x0646 \u0631\x0627 \u0628\x0647 \u062c\x0627\x06cc\x06af\x0632\x06cc\x0646 \u0627\x0644\x0631\x0627 \u0633\x0627\x0632\x06cc \u0645\x06cc\u200c\x062f\x0647\u062f. \u0645\x062b\x0644\x0627\u0646 \u00ab\x06a9\x0627\x0645\x067e\x06cc\x0648\x062a\x0631\u00bb = \u00ab\x06a9\x0645\x067e\x06cc\x0648\x062a\x0631\u00bb\u060c \u0628\x0631\x0627\x06cc \u0635\x0631\x0627\x0641\x062a \u062a\x0644\x0648\x06cc \u0627\x0646\x06af\x0644\x06cc\xb7\x0633\x06cc\x062a\x06cc.",
     L"If a word is spoken the way you do not want, add it here with the way it should be said. Example: \"kompyuter\" = \"computer\", to fix loanword pronunciation."},
    /* STR_DICT_WORD */
    {L"\u06a9\x0644\x0645\x0647", L"Word"},
    /* STR_DICT_REPL */
    {L"\u062c\x0627\x06cc\x06af\x0632\x06cc\x0646", L"Said as"},
    /* STR_DICT_ADD */
    {L"\u0627\x0641\x0632\x0648\x062f\x0646", L"Add / update"},
    /* STR_DICT_DEL */
    {L"\u062d\x0630\x0641", L"Delete"},
    /* STR_DICT_EMPTY */
    {L"\u0648\x0627\x0698\x0647\u200c\u0646\x0627\x0645\x0647 \u062e\x0627\x0644\x06cc \u0627\x0633\x062a.", L"The dictionary is empty."},
    /* STR_DICT_SAVED */
    {L"\u0648\x0627\x0698\x0647\u200c\u0646\x0627\x0645\x0647 \u0630\x062e\x06cc\x0631\x0647 \u0634\x062f. \u0627\x06cc\x0646 \u0648\x0627\x0698\x0647 \u0628\x0631\x0627\x06cc \u0647\x0645\x0647 \u062c\x0644\x0633\u200c\u0647\x0627\x06cc \u0635\x062f\x0627 \u0628\x0647\u200c\u06a9\x0627\x0631 \u0628\x0631\x0646\x0627\x0645\x0647 \u0645\x06cc\u200c\u0631\x0648\x062f.",
     L"Entry saved. It applies to speech from all tools."},
    /* STR_DICT_REMOVED */
    {L"\u062d\x0630\x0641 \u0634\x062f.", L"Entry removed."},
    /* STR_DICT_FULL */
    {L"\u0641\x0647\x0631\x0633\x062a \u0627\x06cc\x0646 \u06a9\x0644\x0645\x0647 \u06cc\x0627 \u062c\x0627\x06cc\x06af\x0632\x06cc\x0646 \u062e\x0627\x0644\x06cc \u0627\x0633\x062a \u06cc\x0627 \u06a9\x0644\x0645\x0647 \u0637\x0648\x0644\x0627\x0646\x06cc \u0646\x06cc\x0633\x062a.",
     L"That word or replacement is too long, or the dictionary is full."},
    /* STR_SET_LANG */
    {L"\u0632\x0628\x0627\x0646", L"Language"},
    /* STR_SET_SID */
    {L"\u0633\x062e\x0646\x06af\x0648 (speaker id)", L"Speaker id"},
    /* STR_SET_ABOUT */
    {L"\u062f\x0631\x0628\u0627\x0631\x0647", L"About"},
    /* STR_SET_MODEL */
    {L"\u0645\x062f\x0644 \u0641\x0639\x0627\u0644", L"Active model"},
    /* STR_DL_HINT */
    {L"\u0647\x0645\x0647 \u0645\x062f\x0644\u200c\u0647\x0627 \u0627\x0632 \u0631\x0648\x06cc \u0641\x0627\x06cc\x0644 models.json \u0627\x06cc\x0646\x062c\x0627 \u0645\x06cc\u200c\u0634\x0648\x0646\x062f. \u0639\x0646\x0648\x0627\x0648\x06cc\x0646 \u0627\x0632 \u0627\x06cc\x0646\x062a\x0631\x0646\x062a \u0645\x06cc\u200c\u0631\x0648\x062f \u0648 \u0628\x0647 \u0641\x0648\x0644\x062f \u0645\x0648\x0642\x0639 \u0645\x062f\x0644\u060c \u0633\x067e\x0633 \u0645\x06cc\u200c\u0634\x0648\x062f.",
     L"Everything here comes from models.json. Requires internet; files land in the models folder, ready to use."},
    /* STR_DL_FETCH */
    {L"\x062f\x0627\x0646\u0644\u0648\x062f", L"Download"},
    /* STR_DL_CANCEL */
    {L"\x0644\x063a\x0648", L"Cancel"},
    /* STR_DL_REFRESH */
    {L"\x062a\x0627\x0632\x0647\u200c\u0633\x0627\x0632\x06cc", L"Refresh"},
    /* STR_DL_STATUS */
    {L"\x0648\x0636\x0639\x06cc\x062a", L"Status"},
    /* STR_DL_NONE */
    {L"\u062d\x062f\x06cc \u062f\x0627\u0646\u0644\u0648\x062f \u0645\x062c\x062a\x062f\x062f \u0646\x06cc\x0633\x062a.", L"Nothing selected."},
    /* STR_DL_BUSY */
    {L"\u062f\x0627\u0646\u0644\u0648\x062f \u062f\x0631 \u062d\x0627\u0644 \u0627\x0646\x062c\x0627\x0645 \u0627\u0633\x062a...", L"Download in progress..."},
    /* STR_DL_FAIL */
    {L"\u062e\x0637\x0627: %s", L"Error: %s"},
    /* STR_DL_DONE */
    {L"\u062f\x0627\u0646\u0644\u0648\x062f \u0627\x0646\x062c\x0627\x0645 \u0634\x062f: %s", L"Downloaded: %s"},
    /* STR_DL_EXTRACTING */
    {L"\u062f\x0631 \u062d\x0627\x0644 \u0628\x0627\x0632\x06af\u0634\x0627\x06cc\u06cc...", L"Extracting..."},
    /* STR_DL_NOMODELSJSON */
    {L"\u0641\x0627\x06cc\x0644 models.json \u067e\x06cc\x062f\x0627 \u0646\x0634\x062f. \u0627\x0632 \u0646\x0633\x062e\x0647 \u062c\x062f\x06cc\x062f \u0628\x0627\x0632\x06cc\x0627\x0628\x06cc \u06a9\x0646\x06cc\x062f.",
     L"models.json not found. You can still use the tools."},
    /* STR_DL_NOTAR */
    {L"\u062a\x0627\x0631 \u067e\x06cc\x062f\x0627 \u0646\x0634\x062f \u0648 \u0628\x0647 \u062c\x0627\x06cc\x06af\u0632\x06cc \u062f\x0633\x062a\x06cc \u0646\x06cc\x0633\x062a. \u062b\x0628\x062a \u0631\x0627 \u062f\x0631 \u067e\x0648\x0634\x0647 models \u0628\x06af\x06cc\x0631\x06cc\x062f.",
     L"tar.exe not found; the archive was downloaded but not extracted. Unzip it into models manually."},
    /* STR_DL_CANCELLED */
    {L"لغو شد.", L"Cancelled."},
    /* STR_CLOSE_PROMPT */
    {L"\u0627\x0632 \u0628\x0633\x062a\x0646 \u0648\x06cc\x0646\x067e\x06cc\x0633\x062a\x061f", L"Close win7-tts?"},
    /* STR_ACC_H */
    {L"Ctrl+A: \u0627\x0646\x062a\x062e\x0627\x0628 \u0647\x0645\x0647  \u00b7  Ctrl+Tab: \u062a\x063a\x06cc\x06cc\x0631 \u0632\x0628\x0627\x0646\x0647",
     L"Ctrl+A: select all  \u00b7  Ctrl+Tab: next tab"},
};

static int g_lang = 0;   /* 0 = fa (default), 1 = en */

#define S(id) (g_str[(id)][g_lang ? 1 : 0])

/* --------------------------------------------------------------- state */

typedef struct {
    HWND hwnd;
    HWND tabs;
    HWND status;
    HWND progress;

    /* page 0 */
    HWND combo; HWND edit; HWND speed_lbl;
    HWND btn_gen; HWND btn_play; HWND btn_stop; HWND btn_saveas; HWND btn_open;

    /* page 1 */
    HWND dict_word; HWND dict_repl; HWND dict_list;
    HWND btn_dict_add; HWND btn_dict_del;

    /* page 2 */
    HWND set_lang; HWND set_sid; HWND set_about; HWND set_model;

    /* page 3 */
    HWND dl_list; HWND dl_status; HWND dl_progress;
    HWND btn_dl_fetch; HWND btn_dl_cancel; HWND btn_dl_refresh; HWND btn_dl_models;

    HFONT font;
    HFONT bold;
    HFONT header_font;

    wchar_t model_dir[MAX_PATHLEN];
    wchar_t wav_path[MAX_PATHLEN];
    int generating;
    int sid;
    int tab;
} App;

/* The selected model per combo item is stored by index -> path table. */
static wchar_t g_model_paths[MAX_DL_ITEMS][MAX_PATHLEN];
static int g_model_count = 0;

/* Download catalog, read from models.json once at startup. */
typedef struct {
    char name[128];
    char url[1024];
    char kind[16];
    char desc[256];
    unsigned long long size;
} DlItem;

static DlItem g_dl[MAX_DL_ITEMS];
static int g_dl_count = 0;
static volatile LONG g_dl_cancel = 0;

/* Payload handed from the worker thread to the UI thread. */
typedef struct {
    App *app;
    wchar_t wav_path[MAX_PATHLEN];
    char wav_narrow[MAX_PATHLEN];
    double seconds;
    int sample_rate;
    int sid;
    float speed;
    char text_utf8[1];              /* over-allocated: the real UTF-8 text follows */
} Job;

/* --------------------------------------------------------------- utf8  */

/* Convert UTF-16 to UTF-8; returns a malloc'd NUL-terminated buffer. */
static char *utf16_to_utf8(const wchar_t *in) {
    int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, NULL, 0, NULL, NULL);
    char *out;
    if (n <= 0) return NULL;
    out = (char *)malloc((size_t)n);
    if (!out) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, in, -1, out, n, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* Convert UTF-8 to UTF-16 into out (cap chars). Returns 0 on failure. */
static int utf8_to_utf16(const char *in, wchar_t *out, int cap) {
    if (!in || !out || cap <= 0) return 0;
    return MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap) > 0;
}

/* ------------------------------------------------------------- helpers */

/* <exe dir>\..\something, wide */
static void pkg_path(wchar_t *out, int cap, const wchar_t *rel) {
    GetModuleFileNameW(NULL, out, cap);
    {
        wchar_t *slash = wcsrchr(out, L'\\');
        if (slash) {
            *slash = L'\0';
            _snwprintf(out + wcslen(out), (size_t)cap - wcslen(out),
                       L"\\..\\%s", rel);
        }
    }
}

static void models_dir(wchar_t *out, int cap) {
    pkg_path(out, cap, L"models");
}

/* Every subdirectory of models\ with a tokens.txt + .onnx is offered. */
static int scan_models(wchar_t paths[][MAX_PATHLEN], int max, App *app) {
    wchar_t base[MAX_PATHLEN];
    wchar_t pattern[MAX_PATHLEN];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;

    models_dir(base, MAX_PATHLEN);
    _snwprintf(pattern, MAX_PATHLEN, L"%s\\*", base);

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (n >= max) break;
            _snwprintf(paths[n], MAX_PATHLEN, L"%s\\%s", base, fd.cFileName);
            {
                wchar_t probe[MAX_PATHLEN];
                _snwprintf(probe, MAX_PATHLEN, L"%s\\tokens.txt", paths[n]);
                if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
                    n++;
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    (void)app;
    return n;
}

/* Fill the model combo from the scan, preserving the selection if possible. */
static void populate_models(App *app) {
    LRESULT sel = SendMessageW(app->combo, CB_GETCURSEL, 0, 0);
    wchar_t keep[MAX_PATHLEN];
    int i;

    keep[0] = 0;
    if (sel != CB_ERR) {
        wcscpy(keep, g_model_paths[sel]);
    }

    SendMessageW(app->combo, CB_RESETCONTENT, 0, 0);
    g_model_count = scan_models(g_model_paths, MAX_DL_ITEMS, app);
    for (i = 0; i < g_model_count; i++) {
        wchar_t *slash = wcsrchr(g_model_paths[i], L'\\');
        SendMessageW(app->combo, CB_ADDSTRING, 0, (LPARAM)(slash ? slash + 1 : g_model_paths[i]));
    }

    if (g_model_count > 0) {
        LRESULT idx = CB_ERR;
        if (keep[0]) {
            for (i = 0; i < g_model_count; i++) {
                if (wcscmp(g_model_paths[i], keep) == 0) { idx = i; break; }
            }
        }
        if (idx == CB_ERR) {
            idx = SendMessageW(app->combo, CB_FINDSTRINGEXACT, -1,
                               (LPARAM)L"vits-piper-fa_IR-gyro-medium");
        }
        if (idx == CB_ERR) idx = 0;
        SendMessageW(app->combo, CB_SETCURSEL, (WPARAM)idx, 0);
        wcscpy(app->model_dir, g_model_paths[idx]);
    }
}

/* -------------------------------------------------------- dictionary */

static void dict_refresh_list(App *app) {
    int i, n = DictCount();

    SendMessageW(app->dict_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        const DictEntry *e = DictGet(i);
        wchar_t word[DICT_MAX_WORD * 4], repl[DICT_MAX_REPL * 4], line[DICT_MAX_WORD * 4 + DICT_MAX_REPL * 4 + 8];
        if (!e) break;
        utf8_to_utf16(e->word, word, (int)(sizeof(word) / sizeof(word[0])));
        utf8_to_utf16(e->repl, repl, (int)(sizeof(repl) / sizeof(repl[0])));
        _snwprintf(line, (int)(sizeof(line) / sizeof(line[0])), L"%s  →  %s", word, repl);
        SendMessageW(app->dict_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (n == 0) {
        SendMessageW(app->dict_list, LB_ADDSTRING, 0, (LPARAM)S(STR_DICT_EMPTY));
    }
}

static void dict_load(App *app) {
    wchar_t path[MAX_PATHLEN];
    pkg_path(path, MAX_PATHLEN, L"user.dict");
    DictLoad(path);
    dict_refresh_list(app);
}

static void dict_save(App *app) {
    wchar_t path[MAX_PATHLEN];
    pkg_path(path, MAX_PATHLEN, L"user.dict");
    if (DictSave(path) == 0) {
        SetWindowTextW(app->status, S(STR_DICT_SAVED));
        dict_refresh_list(app);
    }
}

/* -------------------------------------------------------- worker bits */

typedef struct {
    App *app;
    float *samples;
    int count;
    int cap;
} ProgressCtx;

static void on_samples(const float *samples, int count, float progress, void *user) {
    ProgressCtx *ctx = (ProgressCtx *)user;
    if (count > 0) {
        if (ctx->count + count > ctx->cap) {
            int cap = ctx->cap ? ctx->cap : 65536;
            float *grown;
            while (cap < ctx->count + count) cap *= 2;
            grown = (float *)realloc(ctx->samples, (size_t)cap * sizeof(float));
            if (grown) {
                ctx->samples = grown;
                ctx->cap = cap;
            }
        }
        if (ctx->count + count <= ctx->cap) {
            memcpy(ctx->samples + ctx->count, samples, (size_t)count * sizeof(float));
            ctx->count += count;
        }
    }
    PostMessageW(ctx->app->hwnd, WM_APP_PROG, (WPARAM)(int)(progress * 100.0f), 0);
}

static DWORD WINAPI worker(LPVOID param) {
    Job *job = (Job *)param;
    TtsEngine *engine;
    ProgressCtx pctx;
    wchar_t werr[512];
    int n_samples = 0;
    char err[512];

    memset(&pctx, 0, sizeof(pctx));
    pctx.app = job->app;

    engine = TtsEngineAcquire(job->app->model_dir, werr, 512);
    if (!engine) {
        PostMessageW(job->app->hwnd, WM_APP_FAIL, 0, (LPARAM)werr);
        free(job);
        return 1;
    }

    if (!TtsEngineGenerate(engine, job->text_utf8, job->speed, job->sid,
                           on_samples, &pctx, &n_samples, err, sizeof(err))) {
        TtsEngineTouch(engine);
        TtsEngineRelease();
        PostMessageW(job->app->hwnd, WM_APP_FAIL, 0, (LPARAM)err);
        free(job);
        return 1;
    }

    job->sample_rate = TtsEngineSampleRate(job->app->model_dir);
    if (job->sample_rate <= 0) job->sample_rate = 22050;
    if (n_samples <= 0) n_samples = pctx.count;
    job->seconds = (double)n_samples / (double)job->sample_rate;

    TtsEngineTouch(engine);
    TtsEngineRelease();

    if (!SherpaOnnxWriteWave(pctx.samples, n_samples, job->sample_rate,
                             job->wav_narrow)) {
        free(pctx.samples);
        PostMessageW(job->app->hwnd, WM_APP_FAIL, 0, (LPARAM)err);
        free(job);
        return 1;
    }
    free(pctx.samples);

    PostMessageW(job->app->hwnd, WM_APP_DONE, 0, (LPARAM)job);
    return 0;
}

static DWORD WINAPI preload_thread(LPVOID param) {
    App *app = (App *)param;
    wchar_t werr[512];
    TtsEngine *engine = TtsEngineAcquire(app->model_dir, werr, 512);
    if (engine) TtsEngineTouch(engine);
    TtsEngineRelease();
    PostMessageW(app->hwnd, WM_APP_PRELOADED, 0, 0);
    return 0;
}

/* Evict the previously selected model so only the current one is resident. */
static void on_model_changed(App *app, const wchar_t *old_dir) {
    if (old_dir[0] && wcscmp(old_dir, app->model_dir) != 0) {
        TtsEngineEvict(old_dir);
    }
    CreateThread(NULL, 0, preload_thread, app, 0, NULL);
}

/* ------------------------------------------------------------ catalog */

/* Read models.json (package root) and fill g_dl. Returns item count. */
static int catalog_load(void) {
    wchar_t path[MAX_PATHLEN];
    HANDLE h;
    LARGE_INTEGER sz;
    char *buf;
    DWORD got = 0;
    const char *p;
    int n = 0;

    g_dl_count = 0;
    pkg_path(path, MAX_PATHLEN, L"models.json");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 4 * 1024 * 1024) { CloseHandle(h); return 0; }
    buf = (char *)malloc((size_t)sz.QuadPart + 1);
    if (!buf) { CloseHandle(h); return 0; }
    if (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL)) {
        free(buf); CloseHandle(h); return 0;
    }
    CloseHandle(h);
    buf[got] = 0;

    /* The items array: [{...},{...}]. Walk objects one at a time. */
    p = strstr(buf, "\"items\"");
    if (p) {
        p = strchr(p, '[');
        if (p) {
            p++;
            while (*p && n < MAX_DL_ITEMS) {
                const char *start = strchr(p, '{');
                const char *end;
                if (!start) break;
                end = strchr(start, '}');
                if (!end) break;
                {
                    /* Extract this object's fields with json_min helpers. */
                    char obj[2048];
                    size_t len = (size_t)(end - start + 1);
                    if (len >= sizeof(obj)) { p = end + 1; continue; }
                    memcpy(obj, start, len);
                    obj[len] = 0;
                    if (json_get_string(obj, "name", g_dl[n].name, sizeof(g_dl[n].name)) &&
                        json_get_string(obj, "url", g_dl[n].url, sizeof(g_dl[n].url))) {
                        if (!json_get_string(obj, "kind", g_dl[n].kind, sizeof(g_dl[n].kind)))
                            strcpy(g_dl[n].kind, "file");
                        if (!json_get_string(obj, "desc", g_dl[n].desc, sizeof(g_dl[n].desc)))
                            g_dl[n].desc[0] = 0;
                        {
                            double szd = 0;
                            json_get_double(obj, "size", &szd);
                            g_dl[n].size = (unsigned long long)szd;
                        }
                        n++;
                    }
                    p = end + 1;
                }
            }
        }
    }
    free(buf);
    g_dl_count = n;
    return n;
}

static void catalog_refresh_list(App *app) {
    int i;

    /* Empty the list view. */
    while (SendMessageW(app->dl_list, LVM_DELETEITEM, 0, 0) == TRUE) { }

    for (i = 0; i < g_dl_count; i++) {
        LVITEMW li;
        wchar_t name[256], size[64], desc[512];
        utf8_to_utf16(g_dl[i].name, name, 256);
        utf8_to_utf16(g_dl[i].desc, desc, 512);
        if (g_dl[i].size > 0) {
            double mb = (double)g_dl[i].size / (1024.0 * 1024.0);
            _snwprintf(size, 64, L"%.1f MB", mb);
        } else {
            wcscpy(size, L"?");
        }
        memset(&li, 0, sizeof(li));
        li.mask = LVIF_TEXT;
        li.iItem = i;
        li.pszText = name;
        li.iSubItem = 0;
        SendMessageW(app->dl_list, LVM_INSERTITEMW, 0, (LPARAM)&li);
        li.iSubItem = 1; li.pszText = size;
        SendMessageW(app->dl_list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&li);
        li.iSubItem = 2; li.pszText = desc;
        SendMessageW(app->dl_list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&li);
    }
}

/* ---------------------------------------------------------- downloader */

static int download_one(App *app, const DlItem *item, wchar_t *out_wide, int out_cap) {
    HINTERNET hinet, hurl;
    wchar_t dest[MAX_PATHLEN];
    wchar_t dest_part[MAX_PATHLEN];
    HANDLE fh;
    char buf[64 * 1024];
    DWORD nread;
    unsigned long long total = 0, done = 0;
    int ok = 1;

    /* Where does this go?  name is relative to models\ */
    models_dir(dest, MAX_PATHLEN);
    _snwprintf(dest + wcslen(dest), MAX_PATHLEN - wcslen(dest), L"\\%hs", item->name);
    _snwprintf(dest_part, MAX_PATHLEN, L"%s.part", dest);
    if (out_wide && out_cap > 0) {
        _snwprintf(out_wide, out_cap, L"%s", dest);
    }

    hinet = InternetOpenW(L"win7-tts", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hinet) return 0;

    {
        wchar_t wurl[1024];
        utf8_to_utf16(item->url, wurl, 1024);
        hurl = InternetOpenUrlW(hinet, wurl, NULL, 0,
                                INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    }
    if (!hurl) { InternetCloseHandle(hinet); return 0; }

    /* Content-Length when the server sends it. */
    {
        DWORD clen = 0, clensz = sizeof(clen);
        if (HttpQueryInfoW(hurl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
                           &clen, &clensz, NULL)) {
            total = clen;
        }
    }

    fh = CreateFileW(dest_part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(hurl);
        InternetCloseHandle(hinet);
        return 0;
    }

    for (;;) {
        if (InterlockedCompareExchange(&g_dl_cancel, 0, 0)) { ok = 0; break; }
        if (!InternetReadFile(hurl, buf, sizeof(buf), &nread)) { ok = 0; break; }
        if (nread == 0) break;
        {
            DWORD wrote = 0;
            if (!WriteFile(fh, buf, nread, &wrote, NULL) || wrote != nread) { ok = 0; break; }
        }
        done += nread;
        PostMessageW(app->hwnd, WM_APP_DL, (WPARAM)done, (LPARAM)total);
    }

    CloseHandle(fh);
    InternetCloseHandle(hurl);
    InternetCloseHandle(hinet);

    if (ok) {
        /* Replace the old file atomically-ish. */
        DeleteFileW(dest);
        if (!MoveFileW(dest_part, dest)) {
            ok = 0;
        }
    } else {
        DeleteFileW(dest_part);
    }
    return ok;
}

/* Extract a .tar.bz2 with tar.exe (ships with Win10+; absent on Win7). */
static int extract_archive(const wchar_t *archive) {
    wchar_t cmdline[MAX_PATHLEN + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    if (!SearchPathW(NULL, L"tar.exe", NULL, MAX_PATHLEN, NULL, NULL)) return -1;

    _snwprintf(cmdline, MAX_PATHLEN + 64, L"tar.exe -xjf \"%s\" -C models", archive);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 60 * 1000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0 ? 1 : 0;
}

static DWORD WINAPI dl_worker(LPVOID param) {
    App *app = (App *)param;
    int i, n_ok = 0;
    wchar_t msg[512];

    for (i = 0; i < g_dl_count; i++) {
        if (InterlockedCompareExchange(&g_dl_cancel, 0, 0)) break;
        {
            wchar_t dest[MAX_PATHLEN];
            _snwprintf(msg, 512, L"%hs", g_dl[i].name);
            SetWindowTextW(app->dl_status, msg);
            if (download_one(app, &g_dl[i], dest, MAX_PATHLEN)) {
                n_ok++;
                if (strstr(g_dl[i].name, ".tar.bz2") != NULL) {
                    SetWindowTextW(app->dl_status, S(STR_DL_EXTRACTING));
                    extract_archive(dest);
                }
            }
        }
    }

    if (n_ok > 0) {
        _snwprintf(msg, 512, S(STR_DL_DONE), n_ok);
        SetWindowTextW(app->dl_status, msg);
        PostMessageW(app->hwnd, WM_APP_DL, 0, 0);
    } else {
        SetWindowTextW(app->dl_status, S(STR_DL_CANCELLED));
    }
    return 0;
}

/* --------------------------------------------------------------- font */

/* Load sahel.ttf (package root) so Persian renders with a real Persian font. */
static HFONT make_font(int bold_weight) {
    wchar_t name[64];
    int use_sahel = 0;

    if (g_lang == 0) {   /* Persian */
        wchar_t path[MAX_PATHLEN];
        pkg_path(path, MAX_PATHLEN, L"sahel.ttf");
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            /* Register it for this process (no install needed). */
            AddFontResourceW(path);
            wcscpy(name, L"Sahel");
            use_sahel = 1;
        }
    }
    if (!use_sahel) wcscpy(name, L"Segoe UI");

    return CreateFontW(-16, 0, 0, 0, bold_weight ? FW_BOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, name);
}

static void apply_fonts_to_children(HWND parent, HFONT font) {
    HWND c = GetWindow(parent, GW_CHILD);
    while (c) {
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        c = GetWindow(c, GW_HWNDNEXT);
    }
}

/* --------------------------------------------------------------- layout */

static void set_rtl_style(HWND hwnd, int rtl) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (rtl) ex |= WS_EX_RTLREADING; else ex &= ~WS_EX_RTLREADING;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
}

static HWND make_label(HWND parent, const wchar_t *text, int id) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
}

/* Width in pixels of `s` rendered in `font` on `wnd` (for sizing buttons to
 * their label).  Returns 0 if it cannot be measured. */
static int text_width(HWND wnd, HFONT font, const wchar_t *s) {
    HDC dc;
    HGDIOBJ old = NULL;
    SIZE sz;
    sz.cx = sz.cy = 0;
    if (!s || !*s) return 0;
    dc = GetDC(wnd);
    if (!dc) return 0;
    if (font) old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    if (font) SelectObject(dc, old);
    ReleaseDC(wnd, dc);
    return sz.cx;
}

/* Height of one tab header row (so the tab control can be a thin strip). */
static int tab_header_height(HWND tabs) {
    RECT r;
    memset(&r, 0, sizeof(r));
    if (SendMessageW(tabs, TCM_GETITEMRECT, 0, (LPARAM)&r) && r.bottom > r.top)
        return (r.bottom - r.top) + 8;   /* a little padding below the labels */
    return 30;
}

/* Lay out the whole window: the tab control is a header strip at the top, and
 * the selected page's controls fill the area between it and the status bar.
 * `width`/`height` are the client-area size.  Buttons are sized to the widest
 * label in their row so nothing is truncated and the row looks even. */
static void layout(App *app, int width, int height) {
    const int margin = 12;
    int tab_h, page_top, bottom;
    int y;
    int bh = 30, gap = 8;   /* button height / gap */

    if (!app->tabs || width < 200 || height < 200) return;

    /* The tab control is just the header strip. */
    tab_h = tab_header_height(app->tabs);
    MoveWindow(app->tabs, margin, margin, width - margin * 2, tab_h, TRUE);
    page_top = margin + tab_h + 6;
    bottom = height - margin - 22;        /* leave room for the status bar */
    if (bottom < page_top + 60) bottom = page_top + 60;

    switch (app->tab) {
    case 0:   /* ------------------------------------------------ text & speech */
        y = page_top;
        MoveWindow(GetDlgItem(app->hwnd, ID_MODEL_LBL), margin, y + 3, 60, 20, TRUE);
        MoveWindow(app->combo, margin + 62, y, width - margin * 2 - 62, 240, TRUE);
        y += 30;
        MoveWindow(GetDlgItem(app->hwnd, ID_TEXT_LBL), margin, y + 3, 60, 20, TRUE);
        {
            int speed_y = bottom - 10 - bh - 8 - 20;
            int btn_y   = bottom - 10 - bh;
            int edit_top = y;
            int edit_h = speed_y - edit_top - 8;
            if (edit_h < 40) edit_h = 40;
            MoveWindow(app->edit, margin + 62, edit_top,
                       width - margin * 2 - 62, edit_h, TRUE);
            MoveWindow(app->speed_lbl, margin + 62, speed_y + 3, 240, 20, TRUE);
            /* Equal-width buttons sized to the longest label. */
            {
                HWND btns[5] = {app->btn_gen, app->btn_play, app->btn_stop,
                                app->btn_saveas, app->btn_open};
                const wchar_t *labels[5];
                int i, bw, x = margin;
                labels[0] = S(STR_GEN); labels[1] = S(STR_PLAY);
                labels[2] = S(STR_STOP); labels[3] = S(STR_SAVEAS);
                labels[4] = S(STR_OPEN);
                bw = 0;
                for (i = 0; i < 5; i++) {
                    int w = text_width(app->hwnd, app->font, labels[i]) + 28;
                    if (w > bw) bw = w;
                }
                if (bw < 96) bw = 96;
                for (i = 0; i < 5; i++)
                    MoveWindow(btns[i], x + i * (bw + gap), btn_y, bw, bh, TRUE);
            }
            MoveWindow(app->progress, margin, bottom - 10,
                       width - margin * 2, 10, TRUE);
        }
        break;

    case 1:   /* ------------------------------------------------------------ dict */
        y = page_top;
        MoveWindow(GetDlgItem(app->hwnd, ID_DICT_HINT), margin, y,
                   width - margin * 2, 52, TRUE);
        y += 56;
        MoveWindow(GetDlgItem(app->hwnd, ID_DICT_WORD_LBL), margin, y + 3, 70, 20, TRUE);
        MoveWindow(app->dict_word, margin + 72, y, 170, 24, TRUE);
        MoveWindow(GetDlgItem(app->hwnd, ID_DICT_REPL_LBL), margin + 250, y + 3, 70, 20, TRUE);
        MoveWindow(app->dict_repl, margin + 322, y, 200, 24, TRUE);
        {
            int bw = text_width(app->hwnd, app->font, S(STR_DICT_ADD)) + 28;
            int bw2 = text_width(app->hwnd, app->font, S(STR_DICT_DEL)) + 28;
            if (bw < bw2) bw = bw2;
            if (bw < 110) bw = 110;
            MoveWindow(app->btn_dict_add, width - margin - bw, y, bw, 26, TRUE);
            y += 30;
            MoveWindow(app->btn_dict_del, width - margin - bw, y, bw, 26, TRUE);
        }
        y += 34;
        MoveWindow(app->dict_list, margin, y, width - margin * 2,
                   bottom - y, TRUE);
        break;

    case 2:   /* --------------------------------------------------------- settings */
        y = page_top;
        MoveWindow(GetDlgItem(app->hwnd, ID_SET_LANG_LBL), margin, y + 3, 110, 20, TRUE);
        MoveWindow(app->set_lang, margin + 112, y, 140, 220, TRUE);
        y += 32;
        MoveWindow(GetDlgItem(app->hwnd, ID_SET_SID_LBL), margin, y + 3, 110, 20, TRUE);
        MoveWindow(app->set_sid, margin + 112, y, 70, 24, TRUE);
        y += 34;
        {
            int bw = text_width(app->hwnd, app->font, S(STR_SET_MODEL)) + 28;
            if (bw < 200) bw = 200;
            MoveWindow(app->set_model, margin, y, bw, 30, TRUE);
        }
        y += 40;
        MoveWindow(app->set_about, margin, y, width - margin * 2, bottom - y, TRUE);
        break;

    case 3:   /* ---------------------------------------------------------- download */
        y = page_top;
        MoveWindow(GetDlgItem(app->hwnd, ID_DL_HINT), margin, y,
                   width - margin * 2, 40, TRUE);
        y += 44;
        {
            int list_h = bottom - 10 - bh - 8 - 10 - 20 - y;
            if (list_h < 60) list_h = 60;
            MoveWindow(app->dl_list, margin, y, width - margin * 2, list_h, TRUE);
        }
        {
            HWND btns[4] = {app->btn_dl_fetch, app->btn_dl_cancel,
                            app->btn_dl_refresh, app->btn_dl_models};
            const wchar_t *labels[4];
            int i, bw, x = margin;
            labels[0] = S(STR_DL_FETCH); labels[1] = S(STR_DL_CANCEL);
            labels[2] = S(STR_DL_REFRESH); labels[3] = S(STR_OPEN_MODELS);
            bw = 0;
            for (i = 0; i < 4; i++) {
                int w = text_width(app->hwnd, app->font, labels[i]) + 28;
                if (w > bw) bw = w;
            }
            if (bw < 110) bw = 110;
            for (i = 0; i < 4; i++)
                MoveWindow(btns[i], x + i * (bw + gap), bottom - 10 - bh, bw, bh, TRUE);
        }
        MoveWindow(app->dl_progress, margin, bottom - 10 - bh - 8 - 10,
                   width - margin * 2, 10, TRUE);
        MoveWindow(app->dl_status, margin, bottom - 10 - bh - 8 - 10 - 10 - 20,
                   width - margin * 2, 20, TRUE);
        break;
    }
}

/* ------------------------------------------------------------------ tabs */

/* Switch to tab `sel`, relayout, and show only that page's controls.  This is
 * the single place that knows which control belongs to which page; it is called
 * from the tab control's TCN_SELCHANGE *and* from the Ctrl+Tab accelerators
 * (TCM_SETCURSEL does not itself raise TCN_SELCHANGE, so the accelerators call
 * this explicitly after moving the selection). */
static void select_tab(App *app, int sel) {
    RECT rc;
    if (sel < 0) sel = 0;
    if (sel > 3) sel = 3;
    app->tab = sel;
    GetClientRect(app->hwnd, &rc);
    layout(app, rc.right, rc.bottom);

    /* Page controls, per tab. */
    {
        HWND page0[] = {app->combo, app->edit, app->speed_lbl, app->btn_gen,
                        app->btn_play, app->btn_stop, app->btn_saveas,
                        app->btn_open, app->progress};
        HWND page1[] = {app->dict_word, app->dict_repl, app->dict_list,
                        app->btn_dict_add, app->btn_dict_del};
        HWND page2[] = {app->set_lang, app->set_sid, app->set_about,
                        app->set_model};
        HWND page3[] = {app->dl_list, app->dl_status, app->dl_progress,
                        app->btn_dl_fetch, app->btn_dl_cancel,
                        app->btn_dl_refresh, app->btn_dl_models};
        HWND *pages[4];
        int counts[4];
        int p, k;
        pages[0] = page0; counts[0] = (int)(sizeof(page0) / sizeof(page0[0]));
        pages[1] = page1; counts[1] = (int)(sizeof(page1) / sizeof(page1[0]));
        pages[2] = page2; counts[2] = (int)(sizeof(page2) / sizeof(page2[0]));
        pages[3] = page3; counts[3] = (int)(sizeof(page3) / sizeof(page3[0]));
        for (p = 0; p < 4; p++)
            for (k = 0; k < counts[p]; k++)
                ShowWindow(pages[p][k], p == sel ? SW_SHOW : SW_HIDE);
    }
    /* Static labels follow their tab too. */
    {
        int ids0[] = {ID_MODEL_LBL, ID_TEXT_LBL};
        int ids1[] = {ID_DICT_HINT, ID_DICT_WORD_LBL, ID_DICT_REPL_LBL};
        int ids2[] = {ID_SET_LANG_LBL, ID_SET_SID_LBL};
        int ids3[] = {ID_DL_HINT};
        int q;
        for (q = 0; q < 2; q++) ShowWindow(GetDlgItem(app->hwnd, ids0[q]), sel == 0 ? SW_SHOW : SW_HIDE);
        for (q = 0; q < 3; q++) ShowWindow(GetDlgItem(app->hwnd, ids1[q]), sel == 1 ? SW_SHOW : SW_HIDE);
        for (q = 0; q < 2; q++) ShowWindow(GetDlgItem(app->hwnd, ids2[q]), sel == 2 ? SW_SHOW : SW_HIDE);
        for (q = 0; q < 1; q++) ShowWindow(GetDlgItem(app->hwnd, ids3[q]), sel == 3 ? SW_SHOW : SW_HIDE);
    }
    /* Give the page's first sensible focus target the focus. */
    {
        HWND focus = NULL;
        if (sel == 0) focus = app->edit;
        else if (sel == 1) focus = app->dict_word;
        else if (sel == 2) focus = app->set_lang;
        else if (sel == 3) focus = app->dl_list;
        if (focus) SetFocus(focus);
    }
}

/* -------------------------------------------------------- window proc */

static void apply_language(App *app);

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App *app = (App *)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        app = (App *)cs->lpCreateParams;
        app->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);

        app->font = make_font(0);
        app->bold = make_font(1);

        /* Tabs */
        app->tabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                    0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)IDC_TABS,
                                    cs->hInstance, NULL);
        /* Insert the four tab items (placeholder text; apply_language below
         * overwrites it with the localized labels).  TCM_SETITEMW alone does
         * nothing if the item does not exist yet, so they must be inserted. */
        {
            TCITEMW ti;
            int t;
            memset(&ti, 0, sizeof(ti));
            ti.mask = TCIF_TEXT;
            ti.pszText = (wchar_t *)L"";
            for (t = 0; t < 4; t++)
                SendMessageW(app->tabs, TCM_INSERTITEMW, (WPARAM)t, (LPARAM)&ti);
        }

        /* --- page 0: text & speech --- */
        make_label(hwnd, S(STR_MODEL), ID_MODEL_LBL);
        app->combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                                     0, 0, 200, 400, hwnd, (HMENU)(INT_PTR)ID_MODEL,
                                     cs->hInstance, NULL);
        make_label(hwnd, S(STR_TEXT_LBL), ID_TEXT_LBL);
        app->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOHSCROLL |
                                    ES_AUTOVSCROLL | ES_WANTRETURN,
                                    0, 0, 200, 200, hwnd, (HMENU)(INT_PTR)ID_TEXT,
                                    cs->hInstance, NULL);
        /* Default Persian text so the window is never empty. */
        SetWindowTextW(app->edit,
                       L"سلام! این یک آزمایش کوتاه از کتاب گفتار فارسی است؛ "
                       L"توانی به هر زبان می‌گوید تغییر کنید؛ "
                       L"اگر کلمه‌ای را اشتباه می‌گوید، آن را در واژه‌نامه اصلاح کنید.");

        app->speed_lbl = make_label(hwnd, L"1.00x", ID_SPEED + 100);

        app->btn_gen = CreateWindowExW(0, L"BUTTON", S(STR_GEN),
                                       WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_GENERATE,
                                       cs->hInstance, NULL);
        app->btn_play = CreateWindowExW(0, L"BUTTON", S(STR_PLAY),
                                        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_PLAY,
                                        cs->hInstance, NULL);
        app->btn_stop = CreateWindowExW(0, L"BUTTON", S(STR_STOP),
                                        WS_CHILD | BS_PUSHBUTTON,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_STOP,
                                        cs->hInstance, NULL);
        app->btn_saveas = CreateWindowExW(0, L"BUTTON", S(STR_SAVEAS),
                                          WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_SAVEAS,
                                          cs->hInstance, NULL);
        app->btn_open = CreateWindowExW(0, L"BUTTON", S(STR_OPEN),
                                        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_OPEN,
                                        cs->hInstance, NULL);

        app->progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                        WS_CHILD | PBS_SMOOTH,
                                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_PROGRESS,
                                        cs->hInstance, NULL);
        SendMessageW(app->progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        /* --- page 1: dictionary --- */
        {
            HWND hint = make_label(hwnd, S(STR_DICT_HINT), ID_DICT_HINT);
            (void)hint;
        }
        make_label(hwnd, S(STR_DICT_WORD), ID_DICT_WORD_LBL);
        app->dict_word = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                         0, 0, 160, 24, hwnd, (HMENU)(INT_PTR)ID_DICT_WORD,
                                         cs->hInstance, NULL);
        make_label(hwnd, S(STR_DICT_REPL), ID_DICT_REPL_LBL);
        app->dict_repl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                         0, 0, 200, 24, hwnd, (HMENU)(INT_PTR)ID_DICT_REPL,
                                         cs->hInstance, NULL);
        app->btn_dict_add = CreateWindowExW(0, L"BUTTON", S(STR_DICT_ADD),
                                            WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_DICT_ADD,
                                            cs->hInstance, NULL);
        app->btn_dict_del = CreateWindowExW(0, L"BUTTON", S(STR_DICT_DEL),
                                            WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_DICT_DEL,
                                            cs->hInstance, NULL);
        app->dict_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                         WS_CHILD | WS_VSCROLL | LBS_NOTIFY,
                                         0, 0, 200, 200, hwnd, (HMENU)(INT_PTR)ID_DICT_LIST,
                                         cs->hInstance, NULL);

        /* --- page 2: settings --- */
        make_label(hwnd, S(STR_SET_LANG), ID_SET_LANG_LBL);
        app->set_lang = CreateWindowExW(0, L"COMBOBOX", L"",
                                        WS_CHILD | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                                        0, 0, 120, 200, hwnd, (HMENU)(INT_PTR)ID_SET_LANG,
                                        cs->hInstance, NULL);
        SendMessageW(app->set_lang, CB_ADDSTRING, 0, (LPARAM)L"\u0641\u0627\u0631\u0633\u06cc");
        SendMessageW(app->set_lang, CB_ADDSTRING, 0, (LPARAM)L"English");
        SendMessageW(app->set_lang, CB_SETCURSEL, (WPARAM)g_lang, 0);
        make_label(hwnd, S(STR_SET_SID), ID_SET_SID_LBL);
        app->set_sid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                                       WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                                       0, 0, 60, 24, hwnd, (HMENU)(INT_PTR)ID_SET_SID,
                                       cs->hInstance, NULL);
        app->set_model = CreateWindowExW(0, L"BUTTON", S(STR_SET_MODEL),
                                         WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_SET_MODEL_L,
                                         cs->hInstance, NULL);
        app->set_about = make_label(hwnd,
                                    L"win7-tts 1.1.0\n"
                                    L"sherpa-onnx v1.13.8 \u00b7 offline \u00b7 no internet needed\n"
                                    L"github.com/karim23657/offline-persian-tts-win7",
                                    ID_SET_ABOUT);

        /* --- page 3: model download --- */
        make_label(hwnd, S(STR_DL_HINT), ID_DL_HINT);
        {
            HWND hdr = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
                                       0, 0, 200, 200, hwnd, (HMENU)(INT_PTR)ID_DL_LIST,
                                       cs->hInstance, NULL);
            app->dl_list = hdr;
            {
                LVCOLUMNW col;
                memset(&col, 0, sizeof(col));
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.pszText = (wchar_t *)L"\u0646\u0627\u0645";
                col.cx = 220;
                SendMessageW(app->dl_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
                col.pszText = (wchar_t *)L"\u062d\u062c\u0645";
                col.cx = 80;
                SendMessageW(app->dl_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
                col.pszText = (wchar_t *)L"\u062a\u0648\u0636\u06cc\u062d";
                col.cx = 320;
                SendMessageW(app->dl_list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
            }
        }
        app->btn_dl_fetch = CreateWindowExW(0, L"BUTTON", S(STR_DL_FETCH),
                                            WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_DL_FETCH,
                                            cs->hInstance, NULL);
        app->btn_dl_cancel = CreateWindowExW(0, L"BUTTON", S(STR_DL_CANCEL),
                                             WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                             0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_DL_CANCEL,
                                             cs->hInstance, NULL);
        app->btn_dl_refresh = CreateWindowExW(0, L"BUTTON", S(STR_DL_REFRESH),
                                              WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                              0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_DL_REFRESH,
                                              cs->hInstance, NULL);
        app->btn_dl_models = CreateWindowExW(0, L"BUTTON", S(STR_OPEN_MODELS),
                                             WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                             0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_DL_OPENMODELS,
                                             cs->hInstance, NULL);
        app->dl_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                           WS_CHILD | PBS_SMOOTH,
                                           0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_DL_PROGRESS,
                                           cs->hInstance, NULL);
        SendMessageW(app->dl_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        app->dl_status = make_label(hwnd, S(STR_READY), ID_DL_STATUS);

        /* Status bar */
        app->status = CreateWindowExW(0, L"msctls_statusbar32", L"",
                                      WS_CHILD | WS_VISIBLE | SBT_TOOLTIPS,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_STATUS,
                                      cs->hInstance, NULL);

        TtsEngineInit();
        populate_models(app);
        dict_load(app);
        catalog_load();
        catalog_refresh_list(app);

        apply_language(app);

        /* Tab 0 is selected by default; select_tab() lays out the page, shows
         * only its controls and hides the rest, and focuses the text box. */
        SendMessageW(app->tabs, TCM_SETCURSEL, 0, 0);
        select_tab(app, 0);
        SetWindowTextW(app->status, S(STR_READY));
        return 0;
    }

    case WM_COMMAND:
        if (!app) break;
        switch (LOWORD(wp)) {
        case IDA_NEXTTAB:
        case IDA_PREVTAB: {
            /* Ctrl+Tab / Ctrl+Shift+Tab: move between the four tabs, wrapping. */
            int cur = (int)SendMessageW(app->tabs, TCM_GETCURSEL, 0, 0);
            int nxt = (LOWORD(wp) == IDA_NEXTTAB) ? (cur + 1) % 4 : (cur + 3) % 4;
            SendMessageW(app->tabs, TCM_SETCURSEL, (WPARAM)nxt, 0);
            select_tab(app, nxt);
            return 0;
        }
        case IDA_SELALL: {
            /* Ctrl+A: select all in whichever child has the focus.  Each message
             * is ignored by control types that do not understand it, so sending
             * both an edit-select and a list-select is safe. */
            HWND foc = GetFocus();
            if (foc) {
                SendMessageW(foc, EM_SETSEL, 0, -1);   /* edits: all text   */
                SendMessageW(foc, LB_SETSEL, TRUE, -1); /* listboxes: all   */
            }
            return 0;
        }
        case ID_GENERATE: {
            /* Kick off generation. */
            Job *job;
            char *utf8;
            int len;
            LRESULT idx = SendMessageW(app->combo, CB_GETCURSEL, 0, 0);

            if (idx == CB_ERR) {
                MessageBoxW(hwnd, S(STR_NO_MODEL), S(STR_TITLE), MB_ICONWARNING);
                return 0;
            }
            wcscpy(app->model_dir, g_model_paths[idx]);

            len = GetWindowTextLengthW(app->edit);
            if (len <= 0) {
                MessageBoxW(hwnd, S(STR_EMPTY_TEXT), S(STR_TITLE), MB_ICONINFORMATION);
                SetFocus(app->edit);
                return 0;
            }
            {
                wchar_t *text = (wchar_t *)malloc((size_t)(len + 1) * sizeof(wchar_t));
                if (!text) return 0;
                GetWindowTextW(app->edit, text, len + 1);
                utf8 = utf16_to_utf8(text);
                free(text);
            }
            if (!utf8) {
                MessageBoxW(hwnd, S(STR_CONVERT_ERR), S(STR_TITLE), MB_ICONERROR);
                return 0;
            }
            /* Drop a UTF-8 BOM. */
            if ((unsigned char)utf8[0] == 0xEF && (unsigned char)utf8[1] == 0xBB &&
                (unsigned char)utf8[2] == 0xBF) {
                memmove(utf8, utf8 + 3, strlen(utf8 + 3) + 1);
            }

            /* Apply the dictionary before synthesis. */
            if (DictCount() > 0) {
                size_t cap = strlen(utf8) + 1024;
                char *tmp = (char *)malloc(cap);
                if (tmp) {
                    snprintf(tmp, cap, "%s", utf8);
                    if (DictApply(tmp, cap) > 0) {
                        free(utf8);
                        utf8 = tmp;
                    } else {
                        free(tmp);
                    }
                }
            }

            /* Output path: exe dir\out.wav unless "Save as" chose one. */
            if (!app->wav_path[0]) {
                wchar_t exe[MAX_PATHLEN];
                GetModuleFileNameW(NULL, exe, MAX_PATHLEN);
                {
                    wchar_t *slash = wcsrchr(exe, L'\\');
                    if (slash) *slash = L'\0'; else wcscpy(exe, L".");
                }
                _snwprintf(app->wav_path, MAX_PATHLEN, L"%s\\out.wav", exe);
            }

            job = (Job *)malloc(sizeof(Job) + strlen(utf8) + 1);
            if (!job) { free(utf8); return 0; }
            memset(job, 0, sizeof(Job));
            job->app = app;
            wcscpy(job->wav_path, app->wav_path);
            {
                wchar_t sidbuf[32];
                GetWindowTextW(app->set_sid, sidbuf, 32);
                job->sid = _wtoi(sidbuf);
            }
            job->speed = 1.0f;
            strcpy(job->text_utf8, utf8);
            free(utf8);

            if (!WideCharToMultiByte(CP_UTF8, 0, job->wav_path, -1, job->wav_narrow,
                                     MAX_PATHLEN, NULL, NULL)) {
                free(job);
                MessageBoxW(hwnd, S(STR_CONVERT_ERR), S(STR_TITLE), MB_ICONERROR);
                return 0;
            }

            app->generating = 1;
            EnableWindow(app->btn_gen, FALSE);
            EnableWindow(app->combo, FALSE);
            EnableWindow(app->edit, FALSE);
            ShowWindow(app->btn_stop, SW_SHOW);
            SendMessageW(app->progress, PBM_SETPOS, 0, 0);
            SetWindowTextW(app->status, S(STR_LOADING_MODEL));

            if (CreateThread(NULL, 0, worker, job, 0, NULL) == NULL) {
                free(job);
                app->generating = 0;
                EnableWindow(app->btn_gen, TRUE);
                EnableWindow(app->combo, TRUE);
                EnableWindow(app->edit, TRUE);
                ShowWindow(app->btn_stop, SW_HIDE);
            }
            return 0;
        }
        case ID_PLAY:
            if (GetFileAttributesW(app->wav_path) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(hwnd, S(STR_SAVE_FIRST), S(STR_TITLE), MB_ICONINFORMATION);
                return 0;
            }
            if (PlaySoundW(app->wav_path, NULL, SND_FILENAME | SND_ASYNC)) {
                SetWindowTextW(app->status, S(STR_PLAYING));
            }
            return 0;
        case ID_STOP:
            if (app->generating) TtsEngineRequestCancel();
            PlaySoundW(NULL, NULL, SND_PURGE);
            return 0;
        case ID_SAVEAS: {
            wchar_t name[MAX_PATHLEN];
            OPENFILENAMEW ofn;
            if (app->wav_path[0]) wcscpy(name, app->wav_path);
            else wcscpy(name, L"out.wav");
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Wave audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
            ofn.lpstrFile = name;
            ofn.nMaxFile = MAX_PATHLEN;
            ofn.lpstrTitle = L"Save as";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                wcscpy(app->wav_path, name);
                SetWindowTextW(app->status, L"OK");
            }
            return 0;
        }
        case ID_OPEN: {
            wchar_t cmd[MAX_PATHLEN + 64];
            _snwprintf(cmd, MAX_PATHLEN + 64, L"/select,\"%s\"", app->wav_path);
            ShellExecuteW(hwnd, L"open", L"explorer.exe", cmd, NULL, SW_SHOWNORMAL);
            return 0;
        }
        case ID_MODEL:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT idx = SendMessageW(app->combo, CB_GETCURSEL, 0, 0);
                if (idx != CB_ERR) {
                    wchar_t old[MAX_PATHLEN];
                    wcscpy(old, app->model_dir);
                    wcscpy(app->model_dir, g_model_paths[idx]);
                    on_model_changed(app, old);
                }
            }
            return 0;
        case ID_DICT_ADD: {
            char word[DICT_MAX_WORD], repl[DICT_MAX_REPL];
            wchar_t wbuf[DICT_MAX_WORD];
            GetWindowTextW(app->dict_word, wbuf, DICT_MAX_WORD);
            {
                char *u = utf16_to_utf8(wbuf);
                if (u) { snprintf(word, sizeof(word), "%s", u); free(u); }
                else word[0] = 0;
            }
            GetWindowTextW(app->dict_repl, wbuf, DICT_MAX_REPL);
            {
                char *u = utf16_to_utf8(wbuf);
                if (u) { snprintf(repl, sizeof(repl), "%s", u); free(u); }
                else repl[0] = 0;
            }
            if (!word[0] || !repl[0]) return 0;
            if (!DictSet(word, repl)) {
                MessageBoxW(hwnd, S(STR_DICT_FULL), S(STR_TITLE), MB_ICONWARNING);
                return 0;
            }
            dict_save(app);
            SetWindowTextW(app->dict_word, L"");
            SetWindowTextW(app->dict_repl, L"");
            return 0;
        }
        case ID_DICT_DEL: {
            LRESULT sel = SendMessageW(app->dict_list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                const DictEntry *e = DictGet((int)sel);
                if (e) {
                    DictRemove(e->word);
                    dict_save(app);
                    SetWindowTextW(app->status, S(STR_DICT_REMOVED));
                }
            }
            return 0;
        }
        case ID_SET_LANG:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT sel = SendMessageW(app->set_lang, CB_GETCURSEL, 0, 0);
                g_lang = (sel == 1) ? 1 : 0;
                apply_language(app);
            }
            return 0;
        case ID_SET_MODEL_L:
            /* Jump back to tab 0 and re-apply the model choice there. */
            SendMessageW(app->tabs, TCM_SETCURSEL, 0, 0);
            select_tab(app, 0);
            return 0;
        case ID_DL_FETCH: {
            LRESULT sel = ListView_GetNextItem(app->dl_list, -1, LVNI_SELECTED);
            if (sel == -1) {
                SetWindowTextW(app->dl_status, S(STR_DL_NONE));
                return 0;
            }
            InterlockedExchange(&g_dl_cancel, 0);
            EnableWindow(app->btn_dl_fetch, FALSE);
            SetWindowTextW(app->dl_status, S(STR_DL_BUSY));
            SendMessageW(app->dl_progress, PBM_SETPOS, 0, 0);
            CreateThread(NULL, 0, dl_worker, app, 0, NULL);
            return 0;
        }
        case ID_DL_CANCEL:
            InterlockedExchange(&g_dl_cancel, 1);
            SetWindowTextW(app->dl_status, S(STR_CANCELLED));
            return 0;
        case ID_DL_REFRESH:
            catalog_load();
            catalog_refresh_list(app);
            return 0;
        case ID_DL_OPENMODELS: {
            wchar_t dir[MAX_PATHLEN];
            models_dir(dir, MAX_PATHLEN);
            ShellExecuteW(hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (!app) break;
        if (nm->idFrom == IDC_TABS && nm->code == TCN_SELCHANGE) {
            select_tab(app, (int)SendMessageW(app->tabs, TCM_GETCURSEL, 0, 0));
            return 0;
        }
        break;
    }

    case WM_APP_PROG:
        if (!app) break;
        SendMessageW(app->progress, PBM_SETPOS, wp, 0);
        return 0;

    case WM_APP_PRELOADED:
        if (!app) break;
        if (!app->generating) SetWindowTextW(app->status, S(STR_WARM));
        return 0;

    case WM_APP_DONE: {
        Job *job = (Job *)lp;
        wchar_t msg[MAX_PATHLEN + 128];
        if (!app || !job) break;
        SendMessageW(app->progress, PBM_SETPOS, 100, 0);
        _snwprintf(msg, MAX_PATHLEN + 128, S(STR_DONE),
                   (int)job->seconds, job->sample_rate);
        SetWindowTextW(app->status, msg);
        app->generating = 0;
        EnableWindow(app->btn_gen, TRUE);
        EnableWindow(app->combo, TRUE);
        EnableWindow(app->edit, TRUE);
        ShowWindow(app->btn_stop, SW_HIDE);
        if (PlaySoundW(job->wav_path, NULL, SND_FILENAME | SND_ASYNC)) {
            /* Keep the status honest: say "Playing". */
        }
        free(job);
        return 0;
    }

    case WM_APP_FAIL: {
        const wchar_t *why = (const wchar_t *)lp;
        if (!app) break;
        app->generating = 0;
        EnableWindow(app->btn_gen, TRUE);
        EnableWindow(app->combo, TRUE);
        EnableWindow(app->edit, TRUE);
        ShowWindow(app->btn_stop, SW_HIDE);
        SendMessageW(app->progress, PBM_SETPOS, 0, 0);
        SetWindowTextW(app->status, S(STR_FAIL));
        if (why && why[0]) MessageBoxW(hwnd, why, S(STR_TITLE), MB_ICONERROR);
        return 0;
    }

    case WM_APP_DL: {
        if (!app) break;
        if (lp > 0) {
            int pct = (int)((double)wp * 100.0 / (double)lp);
            SendMessageW(app->dl_progress, PBM_SETPOS, (WPARAM)pct, 0);
        } else {
            /* Done: models may have changed. */
            SendMessageW(app->dl_progress, PBM_SETPOS, 100, 0);
            EnableWindow(app->btn_dl_fetch, TRUE);
            populate_models(app);
        }
        return 0;
    }

    case WM_APP_DICT:
        if (app) dict_refresh_list(app);
        return 0;

    case WM_SIZE:
        if (app) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            layout(app, rc.right, rc.bottom);
        }
        return 0;

    case WM_CLOSE:
        PlaySoundW(NULL, NULL, SND_PURGE);
        if (MessageBoxW(hwnd, S(STR_CLOSE_PROMPT), S(STR_TITLE),
                        MB_ICONQUESTION | MB_YESNO) == IDYES) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        if (app) {
            if (app->font) DeleteObject(app->font);
            if (app->bold) DeleteObject(app->bold);
            /* Remove the font resource we added. */
            if (g_lang == 0) {
                wchar_t path[MAX_PATHLEN];
                pkg_path(path, MAX_PATHLEN, L"sahel.ttf");
                RemoveFontResourceW(path);
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------ language switch */

static void apply_language(App *app) {
    int rtl = (g_lang == 0);
    wchar_t buf[512];

    SetWindowTextW(app->hwnd, S(STR_TITLE));

    /* Tab headers */
    {
        TCITEMW ti;
        int i;
        const wchar_t *titles[4];
        titles[0] = S(STR_TAB_TEXT);
        titles[1] = S(STR_TAB_DICT);
        titles[2] = S(STR_TAB_SETTINGS);
        titles[3] = S(STR_TAB_DL);
        for (i = 0; i < 4; i++) {
            memset(&ti, 0, sizeof(ti));
            ti.mask = TCIF_TEXT;
            ti.pszText = (wchar_t *)titles[i];
            SendMessageW(app->tabs, TCM_SETITEMW, (WPARAM)i, (LPARAM)&ti);
        }
    }

    /* Page 0 */
    SetWindowTextW(GetDlgItem(app->hwnd, ID_MODEL_LBL), S(STR_MODEL));
    SetWindowTextW(GetDlgItem(app->hwnd, ID_TEXT_LBL), S(STR_TEXT_LBL));
    SetWindowTextW(app->btn_gen, S(STR_GEN));
    SetWindowTextW(app->btn_play, S(STR_PLAY));
    SetWindowTextW(app->btn_stop, S(STR_STOP));
    SetWindowTextW(app->btn_saveas, S(STR_SAVEAS));
    SetWindowTextW(app->btn_open, S(STR_OPEN));

    /* Page 1 */
    SetWindowTextW(GetDlgItem(app->hwnd, ID_DICT_HINT), S(STR_DICT_HINT));
    SetWindowTextW(GetDlgItem(app->hwnd, ID_DICT_WORD_LBL), S(STR_DICT_WORD));
    SetWindowTextW(GetDlgItem(app->hwnd, ID_DICT_REPL_LBL), S(STR_DICT_REPL));
    SetWindowTextW(app->btn_dict_add, S(STR_DICT_ADD));
    SetWindowTextW(app->btn_dict_del, S(STR_DICT_DEL));

    /* Page 2 */
    SetWindowTextW(GetDlgItem(app->hwnd, ID_SET_LANG_LBL), S(STR_SET_LANG));
    SetWindowTextW(GetDlgItem(app->hwnd, ID_SET_SID_LBL), S(STR_SET_SID));
    SetWindowTextW(app->set_model, S(STR_SET_MODEL));
    SetWindowTextW(app->set_about,
                   g_lang ? L"win7-tts 1.1.0\n"
                            L"sherpa-onnx v1.13.8 - offline - no internet needed\n"
                            L"github.com/karim23657/offline-persian-tts-win7"
                          : L"win7-tts 1.1.0\n"
                            L"sherpa-onnx v1.13.8 - offline - no internet needed\n"
                            L"github.com/karim23657/offline-persian-tts-win7");

    /* Page 3 */
    SetWindowTextW(GetDlgItem(app->hwnd, ID_DL_HINT), S(STR_DL_HINT));
    SetWindowTextW(app->btn_dl_fetch, S(STR_DL_FETCH));
    SetWindowTextW(app->btn_dl_cancel, S(STR_DL_CANCEL));
    SetWindowTextW(app->btn_dl_refresh, S(STR_DL_REFRESH));
    SetWindowTextW(app->btn_dl_models, S(STR_OPEN_MODELS));

    /* RTL for the text box + all edits in Persian mode. */
    set_rtl_style(app->edit, rtl);
    set_rtl_style(app->dict_word, rtl);
    set_rtl_style(app->dict_repl, rtl);

    /* Rebuild the font (Sahel for Persian, Segoe UI for English). */
    if (app->font) { DeleteObject(app->font); app->font = NULL; }
    if (app->bold) { DeleteObject(app->bold); app->bold = NULL; }
    app->font = make_font(0);
    app->bold = make_font(1);
    apply_fonts_to_children(app->hwnd, app->font);

    /* Re-render the status line so the language change is visible. */
    if (app->generating) {
        SetWindowTextW(app->status, S(STR_LOADING_MODEL));
    } else {
        SetWindowTextW(app->status, S(STR_READY));
    }

    /* Update the settings language combo to match. */
    SendMessageW(app->set_lang, CB_SETCURSEL, (WPARAM)g_lang, 0);

    (void)buf;
}

/* ---------------------------------------------------------- entry point */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    WNDCLASSEXW wc;
    App app;
    MSG msg;
    HACCEL accel;
    ACCEL acc[3];

    (void)prev;
    (void)cmdline;
    memset(&app, 0, sizeof(app));

    {
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_TAB_CLASSES |
                                                ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
    }
    SetProcessDPIAware();

    /* Ctrl+A = select all; Ctrl+Tab / Ctrl+Shift+Tab = next / previous tab. */
    acc[0].fVirt = FVIRTKEY | FCONTROL;
    acc[0].key = 'A';
    acc[0].cmd = IDA_SELALL;
    acc[1].fVirt = FVIRTKEY | FCONTROL;
    acc[1].key = VK_TAB;
    acc[1].cmd = IDA_NEXTTAB;
    acc[2].fVirt = FVIRTKEY | FCONTROL | FSHIFT;
    acc[2].key = VK_TAB;
    acc[2].cmd = IDA_PREVTAB;
    accel = CreateAcceleratorTableW(acc, 3);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Win7TtsGui";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    app.hwnd = CreateWindowExW(0, wc.lpszClassName, S(STR_TITLE),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, 860, 620,
                               NULL, NULL, inst, &app);
    if (!app.hwnd) return 1;

    ShowWindow(app.hwnd, show);
    UpdateWindow(app.hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(app.hwnd, accel, &msg) &&
            !IsDialogMessageW(app.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (accel) DestroyAcceleratorTable(accel);
    return (int)msg.wParam;
}
