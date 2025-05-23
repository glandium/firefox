/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __nsLookAndFeel
#define __nsLookAndFeel

#include <windows.h>

#include "mozilla/EnumeratedArray.h"
#include "nsXPLookAndFeel.h"
#include "gfxFont.h"

/*
 * Gesture System Metrics
 */
#ifndef SM_DIGITIZER
#  define SM_DIGITIZER 94
#  define TABLET_CONFIG_NONE 0x00000000
#  define NID_INTEGRATED_TOUCH 0x00000001
#  define NID_EXTERNAL_TOUCH 0x00000002
#  define NID_INTEGRATED_PEN 0x00000004
#  define NID_EXTERNAL_PEN 0x00000008
#  define NID_MULTI_INPUT 0x00000040
#  define NID_READY 0x00000080
#endif

/*
 * Tablet mode detection
 */
#ifndef SM_SYSTEMDOCKED
#  define SM_CONVERTIBLESLATEMODE 0x00002003
#  define SM_SYSTEMDOCKED 0x00002004
#endif

/*
 * Color constant inclusive bounds for GetSysColor
 */
#define SYS_COLOR_MIN 0
#define SYS_COLOR_MAX 30
#define SYS_COLOR_COUNT (SYS_COLOR_MAX - SYS_COLOR_MIN + 1)

// Undocumented SPI, see bug 1712669 comment 4.
#define MOZ_SPI_CURSORSIZE 0x2028
#define MOZ_SPI_SETCURSORSIZE 0x2029

namespace mozilla::widget::WinRegistry {
class KeyWatcher;
}

enum class UXThemeClass : uint8_t {
  Button = 0,
  Edit,
  Toolbar,
  Progress,
  Tab,
  Trackbar,
  Combobox,
  Listview,
  Menu,
  NumClasses
};

// This class makes sure we don't attempt to open a theme if the previous
// loading attempt has failed because OpenThemeData is a heavy task and
// it's less likely that the API returns a different result.
class UXThemeHandle final {
  mozilla::Maybe<HANDLE> mHandle;

 public:
  UXThemeHandle() = default;
  ~UXThemeHandle();

  // Disallow copy and move
  UXThemeHandle(const UXThemeHandle&) = delete;
  UXThemeHandle(UXThemeHandle&&) = delete;
  UXThemeHandle& operator=(const UXThemeHandle&) = delete;
  UXThemeHandle& operator=(UXThemeHandle&&) = delete;

  operator HANDLE();
  void OpenOnce(LPCWSTR aClassList);
  void Close();
};

class nsLookAndFeel final : public nsXPLookAndFeel {
 public:
  nsLookAndFeel();
  virtual ~nsLookAndFeel();

  static HANDLE GetTheme(UXThemeClass);

  void NativeInit() final;
  void RefreshImpl() override;
  nsresult NativeGetInt(IntID, int32_t& aResult) override;
  nsresult NativeGetFloat(FloatID, float& aResult) override;
  nsresult NativeGetColor(ColorID, ColorScheme, nscolor& aResult) override;
  bool NativeGetFont(FontID aID, nsString& aFontName,
                     gfxFontStyle& aFontStyle) override;
  char16_t GetPasswordCharacterImpl() override;

  nsresult GetKeyboardLayoutImpl(nsACString& aLayout) override;

  bool NeedsMicaWorkaround() const {
    // If there's a custom accent inactive color, and "Show accent color on
    // titlebars and window borders" is set, that causes DWM to unconditionally
    // draw a titlebar.
    // See https://aka.ms/AAv5eie and bug 1954963.
    return mTitlebarColors.mUseAccent &&
           mTitlebarColors.mAccentInactive.isSome();
  }

 private:
  struct TitlebarColors {
    // NOTE: These are the DWM accent colors, which might not match the
    // UISettings/UWP accent color in some cases, see bug 1796730.
    mozilla::Maybe<nscolor> mAccent;
    mozilla::Maybe<nscolor> mAccentText;
    mozilla::Maybe<nscolor> mAccentInactive;
    mozilla::Maybe<nscolor> mAccentInactiveText;

    bool mUseAccent = false;

    struct Set {
      nscolor mBg = 0;
      nscolor mFg = 0;
      nscolor mBorder = 0;
    };

    Set mActiveLight;
    Set mActiveDark;

    Set mInactiveLight;
    Set mInactiveDark;

    const Set& Get(mozilla::ColorScheme aScheme, bool aActive) const {
      if (aScheme == mozilla::ColorScheme::Dark) {
        return aActive ? mActiveDark : mInactiveDark;
      }
      return aActive ? mActiveLight : mInactiveLight;
    }
  };

  TitlebarColors ComputeTitlebarColors();

  nscolor GetColorForSysColorIndex(int index);

  LookAndFeelFont GetLookAndFeelFontInternal(const LOGFONTW& aLogFont,
                                             bool aUseShellDlg);

  uint32_t SystemColorFilter();

  LookAndFeelFont GetLookAndFeelFont(LookAndFeel::FontID anID);

  // Cached colors and flags indicating success in their retrieval.
  mozilla::Maybe<nscolor> mColorMenuHoverText;

  mozilla::Maybe<nscolor> mDarkHighlight;
  mozilla::Maybe<nscolor> mDarkHighlightText;

  TitlebarColors mTitlebarColors;

  nscolor mColorAccent = 0;
  nscolor mColorAccentText = 0;

  nscolor mSysColorTable[SYS_COLOR_COUNT]{0};
  bool mHighContrastOn = false;

  mozilla::EnumeratedArray<UXThemeClass, UXThemeHandle,
                           size_t(UXThemeClass::NumClasses)>
      mThemeHandles;

  mozilla::UniquePtr<mozilla::widget::WinRegistry::KeyWatcher>
      mColorFilterWatcher;
  uint32_t mCurrentColorFilter = 0;

  bool mInitialized = false;
  void EnsureInit();
};

#endif
