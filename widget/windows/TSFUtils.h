/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TSFUtils_h
#define TSFUtils_h

#include <ostream>

#include <msctf.h>
#include <textstor.h>

#include "mozilla/Maybe.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Result.h"
#include "mozilla/TextRange.h"
#include "mozilla/ToString.h"
#include "nsPrintfCString.h"
#include "nsString.h"
#include "nsTArray.h"

// GUID_PROP_INPUTSCOPE is declared in inputscope.h using INIT_GUID.
// With initguid.h, we get its instance instead of extern declaration.
#ifdef INPUTSCOPE_INIT_GUID
#  include <initguid.h>
#endif
#ifdef TEXTATTRS_INIT_GUID
#  include <tsattrs.h>
#endif
#include <inputscope.h>

// TSF InputScope, for earlier SDK 8
#define IS_SEARCH static_cast<InputScope>(50)

namespace mozilla::widget {

class TSFUtils final {
 public:
  TSFUtils() = delete;
  ~TSFUtils() = delete;

  // TODO: GUID_PROP_URL has not been declared in the SDK yet.  We should drop
  // this after it's released by a new SDK and it becomes the minimum supported
  // SDK version.
  constexpr static const GUID sGUID_PROP_URL = {
      0xd5138268,
      0xa1bf,
      0x4308,
      {0xbc, 0xbf, 0x2e, 0x73, 0x93, 0x98, 0xe2, 0x34}};

  constexpr static TsViewCookie sDefaultView = 1;

  /**
   * Returns true if the Windows may have a crash bug at handling
   * ITfTextStoreACP::GetSelection() returns error.
   */
  [[nodiscard]] static bool DoNotReturnErrorFromGetSelection();

  struct MOZ_STACK_CLASS AutoRangeExtant {
    explicit AutoRangeExtant(ITfRange* aRange);

    [[nodiscard]] bool isErr() const { return FAILED(mHR); }
    [[nodiscard]] bool isOk() const { return SUCCEEDED(mHR); }
    [[nodiscard]] LONG End() const { return mStart + mLength; }

    LONG mStart = 0;
    LONG mLength = 0;
    HRESULT mHR = E_NOT_SET;
  };

  /**
   * Get TextRangeType corresponding to aDisplayAttr.
   */
  [[nodiscard]] static TextRangeType GetTextRangeType(
      TF_DISPLAYATTRIBUTE& aDisplayAttr);

  /**
   * Get nscolor corresponding to aTSFColor.
   */
  [[nodiscard]] static Maybe<nscolor> GetColor(const TF_DA_COLOR& aTSFColor);

  /**
   * Get LineStyle corresponding to aTSFLineStyle.
   */
  [[nodiscard]] static Maybe<TextRangeStyle::LineStyle> GetLineStyle(
      TF_DA_LINESTYLE aTSFLineStyle);

  /**
   * Returns true if active TIP or IME is a black listed one and we should
   * set input scope of URL bar to IS_DEFAULT rather than IS_URL.
   */
  [[nodiscard]] static bool ShouldSetInputScopeOfURLBarToDefault();

  // Support retrieving attributes.
  // TODO: We should support RightToLeft, perhaps.
  enum AttrIndex {
    // Used for result of GetRequestedAttrIndex()
    NotSupported = -1,

    // Supported attributes
    InputScope = 0,
    DocumentURL,
    TextVerticalWriting,
    TextOrientation,

    // Count of the supported attributes
    NUM_OF_SUPPORTED_ATTRS
  };

  /**
   * Return AttrIndex fo aAttrID.
   */
  [[nodiscard]] static AttrIndex GetRequestedAttrIndex(
      const TS_ATTRID& aAttrID);

  /**
   * Return TS_ATTRID for aIndex.
   */
  [[nodiscard]] static TS_ATTRID GetAttrID(AttrIndex aIndex);

  /**
   * Get compartment instance.
   */
  [[nodiscard]] static Result<RefPtr<ITfCompartment>, bool> GetCompartment(
      IUnknown* pUnk, const GUID& aID);

  /**
   * Mark aContent as keyboard disabled.
   *
   * @return true if succeeded, otherwise, false.
   */
  static bool MarkContextAsKeyboardDisabled(DWORD aClientId,
                                            ITfContext* aContext);

  /**
   * Mark aContext as empty.
   *
   * @return true if succeeded, otherwise, false.
   */
  static bool MarkContextAsEmpty(DWORD aClientId, ITfContext* aContext);

  static const char* BoolToChar(bool aValue) {
    return aValue ? "true" : "false";
  }
  static const char* MouseButtonToChar(int16_t aButton);
  static const char* CommonHRESULTToChar(HRESULT);
  static const char* HRESULTToChar(HRESULT);
};

class AutoFlagsCString : public nsAutoCString {
 protected:
  void AppendFlag(const nsCString& aFlagStr) { AppendFlag(aFlagStr.get()); }
  void AppendFlag(const char* aFlagStr) {
    if (!IsEmpty()) {
      AppendLiteral(" | ");
    }
    Append(aFlagStr);
  }
};

class AutoFindFlagsCString final : public AutoFlagsCString {
 public:
  explicit AutoFindFlagsCString(DWORD);
};
class AutoACPFromPointFlagsCString final : public AutoFlagsCString {
 public:
  explicit AutoACPFromPointFlagsCString(DWORD);
};
class AutoSinkMasksCString final : public AutoFlagsCString {
 public:
  explicit AutoSinkMasksCString(DWORD);
};
class AutoLockFlagsCString final : public AutoFlagsCString {
 public:
  explicit AutoLockFlagsCString(DWORD);
};

class AutoClsidCString final : public nsAutoCString {
 public:
  explicit AutoClsidCString(REFGUID);
};
class AutoRawGuidCString : public nsAutoCString {
 public:
  explicit AutoRawGuidCString(REFGUID aGUID) { AssignRawGuid(aGUID); }

 protected:
  AutoRawGuidCString() = default;
  void AssignRawGuid(REFGUID);
};
class AutoGuidCString final : public AutoRawGuidCString {
 public:
  explicit AutoGuidCString(REFGUID);
};
class AutoRiidCString final : public nsAutoCString {
 public:
  explicit AutoRiidCString(REFIID);
};

class AutoMouseButtonsCString final : public AutoFlagsCString {
 public:
  explicit AutoMouseButtonsCString(int16_t aButtons);
};

class AutoInputScopesCString : public AutoFlagsCString {
 public:
  explicit AutoInputScopesCString(const nsTArray<InputScope>& aList);
};

class AutoEscapedUTF8String final : public NS_ConvertUTF16toUTF8 {
 public:
  explicit AutoEscapedUTF8String(const nsAString& aString)
      : NS_ConvertUTF16toUTF8(aString) {
    Escape();
  }
  explicit AutoEscapedUTF8String(const char16ptr_t aString)
      : NS_ConvertUTF16toUTF8(aString) {
    Escape();
  }
  AutoEscapedUTF8String(const char16ptr_t aString, uint32_t aLength)
      : NS_ConvertUTF16toUTF8(aString, aLength) {
    Escape();
  }

 private:
  void Escape() {
    ReplaceSubstring("\r", "\\r");
    ReplaceSubstring("\n", "\\n");
    ReplaceSubstring("\t", "\\t");
  }
};

}  // namespace mozilla::widget

inline std::ostream& operator<<(std::ostream& aStream,
                                const TS_SELECTIONSTYLE& aSelectionStyle) {
  const char* ase = "Unknown";
  switch (aSelectionStyle.ase) {
    case TS_AE_START:
      ase = "TS_AE_START";
      break;
    case TS_AE_END:
      ase = "TS_AE_END";
      break;
    case TS_AE_NONE:
      ase = "TS_AE_NONE";
      break;
  }
  return aStream << "{ ase=" << ase << ", fInterimChar="
                 << (aSelectionStyle.fInterimChar ? "TRUE" : "FALSE") << " }";
}

inline std::ostream& operator<<(std::ostream& aStream,
                                const TS_SELECTION_ACP& aACP) {
  return aStream << "{ acpStart=" << aACP.acpStart << ", acpEnd=" << aACP.acpEnd
                 << ", style=" << mozilla::ToString(aACP.style).c_str() << " }";
}

inline std::ostream& operator<<(std::ostream& aStream,
                                const TsRunType& aRunType) {
  switch (aRunType) {
    case TS_RT_PLAIN:
      return aStream << "TS_RT_PLAIN";
    case TS_RT_HIDDEN:
      return aStream << "TS_RT_HIDDEN";
    case TS_RT_OPAQUE:
      return aStream << "TS_RT_OPAQUE";
    default:
      return aStream << nsPrintfCString("Unknown(%08X)",
                                        static_cast<int32_t>(aRunType));
  }
}

inline std::ostream& operator<<(std::ostream& aStream,
                                const TF_DA_COLOR& aColor) {
  switch (aColor.type) {
    case TF_CT_NONE:
      return aStream << "TF_CT_NONE";
    case TF_CT_SYSCOLOR:
      return aStream << nsPrintfCString("TF_CT_SYSCOLOR, nIndex:0x%08X",
                                        static_cast<int32_t>(aColor.nIndex));
    case TF_CT_COLORREF:
      return aStream << nsPrintfCString("TF_CT_COLORREF, cr:0x%08X",
                                        static_cast<int32_t>(aColor.cr));
      break;
    default:
      return aStream << nsPrintfCString("Unknown(%08X)",
                                        static_cast<int32_t>(aColor.type));
  }
}

inline std::ostream& operator<<(std::ostream& aStream,
                                const TF_DA_LINESTYLE& aLineStyle) {
  switch (aLineStyle) {
    case TF_LS_NONE:
      return aStream << "TF_LS_NONE";
    case TF_LS_SOLID:
      return aStream << "TF_LS_SOLID";
    case TF_LS_DOT:
      return aStream << "TF_LS_DOT";
    case TF_LS_DASH:
      return aStream << "TF_LS_DASH";
    case TF_LS_SQUIGGLE:
      return aStream << "TF_LS_SQUIGGLE";
    default:
      return aStream << nsPrintfCString("Unknown(%08X)",
                                        static_cast<int32_t>(aLineStyle));
  }
}

inline std::ostream& operator<<(std::ostream& aStream,
                                const TF_DA_ATTR_INFO& aAttr) {
  switch (aAttr) {
    case TF_ATTR_INPUT:
      return aStream << "TF_ATTR_INPUT";
    case TF_ATTR_TARGET_CONVERTED:
      return aStream << "TF_ATTR_TARGET_CONVERTED";
    case TF_ATTR_CONVERTED:
      return aStream << "TF_ATTR_CONVERTED";
    case TF_ATTR_TARGET_NOTCONVERTED:
      return aStream << "TF_ATTR_TARGET_NOTCONVERTED";
    case TF_ATTR_INPUT_ERROR:
      return aStream << "TF_ATTR_INPUT_ERROR";
    case TF_ATTR_FIXEDCONVERTED:
      return aStream << "TF_ATTR_FIXEDCONVERTED";
    case TF_ATTR_OTHER:
      return aStream << "TF_ATTR_OTHER";
    default:
      return aStream << nsPrintfCString("Unknown(%08X)",
                                        static_cast<int32_t>(aAttr));
  }
}

inline std::ostream& operator<<(std::ostream& aStream,
                                const TF_DISPLAYATTRIBUTE& aDispAttr) {
  return aStream << "{ crText:{" << aDispAttr.crText << " }, crBk:{ "
                 << aDispAttr.crBk << " }, lsStyle: " << aDispAttr.lsStyle
                 << ", fBoldLine: "
                 << mozilla::widget::TSFUtils::BoolToChar(aDispAttr.fBoldLine)
                 << ", crLine:{ " << aDispAttr.crLine
                 << " }, bAttr: " << aDispAttr.bAttr << " }";
}

#endif  // #ifndef TSFUtils_h
