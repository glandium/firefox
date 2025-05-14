/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A namespace class for static content security utilities. */

#include "nsContentSecurityUtils.h"

#include "mozilla/Components.h"
#include "mozilla/dom/nsMixedContentBlocker.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/WorkerCommon.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "nsComponentManagerUtils.h"
#include "nsIContentSecurityPolicy.h"
#include "nsIChannel.h"
#include "nsIHttpChannel.h"
#include "nsIMultiPartChannel.h"
#include "nsIURI.h"
#include "nsITransfer.h"
#include "nsNetUtil.h"
#include "nsSandboxFlags.h"
#if defined(XP_WIN)
#  include "mozilla/WinHeaderOnlyUtils.h"
#  include "WinUtils.h"
#  include <wininet.h>
#endif

#include "FramingChecker.h"
#include "js/Array.h"  // JS::GetArrayLength
#include "js/ContextOptions.h"
#include "js/PropertyAndElement.h"  // JS_GetElement
#include "js/RegExp.h"
#include "js/RegExpFlags.h"           // JS::RegExpFlags
#include "js/friend/ErrorMessages.h"  // JSMSG_UNSAFE_FILENAME
#include "mozilla/ExtensionPolicyService.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/nsCSPContext.h"
#include "mozilla/glean/DomSecurityMetrics.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/StaticPrefs_extensions.h"
#include "mozilla/StaticPrefs_security.h"
#include "LoadInfo.h"
#include "nsIConsoleService.h"
#include "nsIStringBundle.h"

using namespace mozilla;
using namespace mozilla::dom;

extern mozilla::LazyLogModule sCSMLog;
extern Atomic<bool, mozilla::Relaxed> sJSHacksChecked;
extern Atomic<bool, mozilla::Relaxed> sJSHacksPresent;
extern Atomic<bool, mozilla::Relaxed> sCSSHacksChecked;
extern Atomic<bool, mozilla::Relaxed> sCSSHacksPresent;

// Helper function for IsConsideredSameOriginForURI which makes
// Principals of scheme 'http' return Principals of scheme 'https'.
static already_AddRefed<nsIPrincipal> MakeHTTPPrincipalHTTPS(
    nsIPrincipal* aPrincipal) {
  nsCOMPtr<nsIPrincipal> principal = aPrincipal;
  // if the principal is not http, then it can also not be upgraded
  // to https.
  if (!principal->SchemeIs("http")) {
    return principal.forget();
  }

  nsAutoCString spec;
  aPrincipal->GetAsciiSpec(spec);
  // replace http with https
  spec.ReplaceLiteral(0, 4, "https");

  nsCOMPtr<nsIURI> newURI;
  nsresult rv = NS_NewURI(getter_AddRefs(newURI), spec);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nullptr;
  }

  mozilla::OriginAttributes OA =
      BasePrincipal::Cast(aPrincipal)->OriginAttributesRef();

  principal = BasePrincipal::CreateContentPrincipal(newURI, OA);
  return principal.forget();
}

/* static */
bool nsContentSecurityUtils::IsConsideredSameOriginForURI(
    nsIPrincipal* aTriggeringPrincipal, nsIPrincipal* aResultPrincipal) {
  MOZ_ASSERT(aTriggeringPrincipal);
  MOZ_ASSERT(aResultPrincipal);
  // we only have to make sure that the following truth table holds:
  // aTriggeringPrincipal         | aResultPrincipal             | Result
  // ----------------------------------------------------------------
  // http://example.com/foo.html  | http://example.com/bar.html  | true
  // http://example.com/foo.html  | https://example.com/bar.html | true
  // https://example.com/foo.html | https://example.com/bar.html | true
  // https://example.com/foo.html | http://example.com/bar.html  | true

  // fast path if both principals are same-origin
  if (aTriggeringPrincipal->Equals(aResultPrincipal)) {
    return true;
  }

  // in case a principal uses a scheme of 'http' then we just upgrade to
  // 'https' and use the principal equals comparison operator to check
  // for same-origin.
  nsCOMPtr<nsIPrincipal> compareTriggeringPrincipal =
      MakeHTTPPrincipalHTTPS(aTriggeringPrincipal);

  nsCOMPtr<nsIPrincipal> compareResultPrincipal =
      MakeHTTPPrincipalHTTPS(aResultPrincipal);

  return compareTriggeringPrincipal->Equals(compareResultPrincipal);
}

/*
 * Performs a Regular Expression match, optionally returning the results.
 * This function is not safe to use OMT.
 *
 * @param aPattern      The regex pattern
 * @param aString       The string to compare against
 * @param aOnlyMatch    Whether we want match results or only a true/false for
 * the match
 * @param aMatchResult  Out param for whether or not the pattern matched
 * @param aRegexResults Out param for the matches of the regex, if requested
 * @returns nsresult indicating correct function operation or error
 */
nsresult RegexEval(const nsAString& aPattern, const nsAString& aString,
                   bool aOnlyMatch, bool& aMatchResult,
                   nsTArray<nsString>* aRegexResults = nullptr) {
  MOZ_ASSERT(NS_IsMainThread());
  aMatchResult = false;

  mozilla::dom::AutoJSAPI jsapi;
  jsapi.Init();

  JSContext* cx = jsapi.cx();
  mozilla::AutoDisableJSInterruptCallback disabler(cx);

  // We can use the junk scope here, because we're just using it for regexp
  // evaluation, not actual script execution, and we disable statics so that the
  // evaluation does not interact with the execution global.
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  JS::Rooted<JSObject*> regexp(
      cx, JS::NewUCRegExpObject(cx, aPattern.BeginReading(), aPattern.Length(),
                                JS::RegExpFlag::Unicode));
  if (!regexp) {
    return NS_ERROR_ILLEGAL_VALUE;
  }

  JS::Rooted<JS::Value> regexResult(cx, JS::NullValue());

  size_t index = 0;
  if (!JS::ExecuteRegExpNoStatics(cx, regexp, aString.BeginReading(),
                                  aString.Length(), &index, aOnlyMatch,
                                  &regexResult)) {
    return NS_ERROR_FAILURE;
  }

  if (regexResult.isNull()) {
    // On no match, ExecuteRegExpNoStatics returns Null
    return NS_OK;
  }
  if (aOnlyMatch) {
    // On match, with aOnlyMatch = true, ExecuteRegExpNoStatics returns boolean
    // true.
    MOZ_ASSERT(regexResult.isBoolean() && regexResult.toBoolean());
    aMatchResult = true;
    return NS_OK;
  }
  if (aRegexResults == nullptr) {
    return NS_ERROR_INVALID_ARG;
  }

  // Now we know we have a result, and we need to extract it so we can read it.
  uint32_t length;
  JS::Rooted<JSObject*> regexResultObj(cx, &regexResult.toObject());
  if (!JS::GetArrayLength(cx, regexResultObj, &length)) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  MOZ_LOG(sCSMLog, LogLevel::Verbose, ("Regex Matched %i strings", length));

  for (uint32_t i = 0; i < length; i++) {
    JS::Rooted<JS::Value> element(cx);
    if (!JS_GetElement(cx, regexResultObj, i, &element)) {
      return NS_ERROR_NO_CONTENT;
    }

    nsAutoJSString value;
    if (!value.init(cx, element)) {
      return NS_ERROR_NO_CONTENT;
    }

    MOZ_LOG(sCSMLog, LogLevel::Verbose,
            ("Regex Matching: %i: %s", i, NS_ConvertUTF16toUTF8(value).get()));
    aRegexResults->AppendElement(value);
  }

  aMatchResult = true;
  return NS_OK;
}

/*
 * MOZ_CRASH_UNSAFE_PRINTF has a sPrintfCrashReasonSize-sized buffer. We need
 * to make sure we don't exceed it.  These functions perform this check and
 * munge things for us.
 *
 */

/*
 * Destructively truncates a string to fit within the limit
 */
char* nsContentSecurityUtils::SmartFormatCrashString(const char* str) {
  return nsContentSecurityUtils::SmartFormatCrashString(strdup(str));
}

char* nsContentSecurityUtils::SmartFormatCrashString(char* str) {
  auto str_len = strlen(str);

  if (str_len > sPrintfCrashReasonSize) {
    str[sPrintfCrashReasonSize - 1] = '\0';
    str_len = strlen(str);
  }
  MOZ_RELEASE_ASSERT(sPrintfCrashReasonSize > str_len);

  return str;
}

/*
 * Destructively truncates two strings to fit within the limit.
 * format_string is a format string containing two %s entries
 * The second string will be truncated to the _last_ 25 characters
 * The first string will be truncated to the remaining limit.
 */
nsCString nsContentSecurityUtils::SmartFormatCrashString(
    const char* part1, const char* part2, const char* format_string) {
  return SmartFormatCrashString(strdup(part1), strdup(part2), format_string);
}

nsCString nsContentSecurityUtils::SmartFormatCrashString(
    char* part1, char* part2, const char* format_string) {
  auto part1_len = strlen(part1);
  auto part2_len = strlen(part2);

  auto constant_len = strlen(format_string) - 4;

  if (part1_len + part2_len + constant_len > sPrintfCrashReasonSize) {
    if (part2_len > 25) {
      part2 += (part2_len - 25);
    }
    part2_len = strlen(part2);

    part1[sPrintfCrashReasonSize - (constant_len + part2_len + 1)] = '\0';
    part1_len = strlen(part1);
  }
  MOZ_RELEASE_ASSERT(sPrintfCrashReasonSize >
                     constant_len + part1_len + part2_len);

  auto parts = nsPrintfCString(format_string, part1, part2);
  return std::move(parts);
}

/*
 * Telemetry Events extra data only supports 80 characters, so we optimize the
 * filename to be smaller and collect more data.
 */
nsCString OptimizeFileName(const nsAString& aFileName) {
  nsCString optimizedName;
  CopyUTF16toUTF8(aFileName, optimizedName);

  MOZ_LOG(sCSMLog, LogLevel::Verbose,
          ("Optimizing FileName: %s", optimizedName.get()));

  optimizedName.ReplaceSubstring(".xpi!"_ns, "!"_ns);
  optimizedName.ReplaceSubstring("shield.mozilla.org!"_ns, "s!"_ns);
  optimizedName.ReplaceSubstring("mozilla.org!"_ns, "m!"_ns);
  if (optimizedName.Length() > 80) {
    optimizedName.Truncate(80);
  }

  MOZ_LOG(sCSMLog, LogLevel::Verbose,
          ("Optimized FileName: %s", optimizedName.get()));
  return optimizedName;
}

static nsCString StripQueryRef(const nsACString& aFileName) {
  nsCString stripped(aFileName);
  int32_t i = stripped.FindCharInSet("#?"_ns);
  if (i != kNotFound) {
    stripped.Truncate(i);
  }
  return stripped;
}

/*
 * FilenameToFilenameType takes a fileName and returns a Pair of strings.
 * The First entry is a string indicating the type of fileName
 * The Second entry is a Maybe<string> that can contain additional details to
 * report.
 *
 * The reason we use strings (instead of an int/enum) is because the Telemetry
 * Events API only accepts strings.
 *
 * Function is a static member of the class to enable gtests.
 */
/* static */
FilenameTypeAndDetails nsContentSecurityUtils::FilenameToFilenameType(
    const nsACString& fileName, bool collectAdditionalExtensionData) {
  // These are strings because the Telemetry Events API only accepts strings
  static constexpr auto kChromeURI = "chromeuri"_ns;
  static constexpr auto kResourceURI = "resourceuri"_ns;
  static constexpr auto kBlobUri = "bloburi"_ns;
  static constexpr auto kDataUri = "dataurl"_ns;
  static constexpr auto kAboutUri = "abouturi"_ns;
  static constexpr auto kDataUriWebExtCStyle =
      "dataurl-extension-contentstyle"_ns;
  static constexpr auto kSingleString = "singlestring"_ns;
  static constexpr auto kMozillaExtensionFile = "mozillaextension_file"_ns;
  static constexpr auto kOtherExtensionFile = "otherextension_file"_ns;
  static constexpr auto kExtensionURI = "extension_uri"_ns;
  static constexpr auto kSuspectedUserChromeJS = "suspectedUserChromeJS"_ns;
#if defined(XP_WIN)
  static constexpr auto kSanitizedWindowsURL = "sanitizedWindowsURL"_ns;
  static constexpr auto kSanitizedWindowsPath = "sanitizedWindowsPath"_ns;
#endif
  static constexpr auto kOther = "other"_ns;
  static constexpr auto kOtherWorker = "other-on-worker"_ns;
  static constexpr auto kRegexFailure = "regexfailure"_ns;

  static constexpr auto kUCJSRegex = u"(.+).uc.js\\?*[0-9]*$"_ns;
  static constexpr auto kExtensionRegex = u"extensions/(.+)@(.+)!(.+)$"_ns;
  static constexpr auto kSingleFileRegex = u"^[a-zA-Z0-9.?]+$"_ns;

  if (fileName.IsEmpty()) {
    return FilenameTypeAndDetails(kOther, Nothing());
  }

  // resource:// and chrome://
  // These don't contain any user (profile) paths.
  if (StringBeginsWith(fileName, "chrome://"_ns)) {
    if (StringBeginsWith(fileName, "chrome://userscripts/"_ns) ||
        StringBeginsWith(fileName, "chrome://userchromejs/"_ns) ||
        StringBeginsWith(fileName, "chrome://user_chrome_files/"_ns) ||
        StringBeginsWith(fileName, "chrome://tabmix"_ns) ||
        StringBeginsWith(fileName, "chrome://searchwp/"_ns) ||
        StringBeginsWith(fileName, "chrome://custombuttons"_ns) ||
        StringBeginsWith(fileName, "chrome://tabgroups-resource/"_ns)) {
      return FilenameTypeAndDetails(kSuspectedUserChromeJS,
                                    Some(StripQueryRef(fileName)));
    }
    return FilenameTypeAndDetails(kChromeURI, Some(StripQueryRef(fileName)));
  }
  if (StringBeginsWith(fileName, "resource://"_ns)) {
    if (StringBeginsWith(fileName, "resource://usl-ucjs/"_ns) ||
        StringBeginsWith(fileName, "resource://sfm-ucjs/"_ns) ||
        StringBeginsWith(fileName, "resource://cpmanager-legacy/"_ns)) {
      return FilenameTypeAndDetails(kSuspectedUserChromeJS,
                                    Some(StripQueryRef(fileName)));
    }
    return FilenameTypeAndDetails(kResourceURI, Some(StripQueryRef(fileName)));
  }

  // blob: and data:
  if (StringBeginsWith(fileName, "blob:"_ns)) {
    return FilenameTypeAndDetails(kBlobUri, Nothing());
  }
  if (StringBeginsWith(fileName, "data:text/css;extension=style;"_ns)) {
    return FilenameTypeAndDetails(kDataUriWebExtCStyle, Nothing());
  }
  if (StringBeginsWith(fileName, "data:"_ns)) {
    return FilenameTypeAndDetails(kDataUri, Nothing());
  }

  // Can't do regex matching off-main-thread
  if (NS_IsMainThread()) {
    NS_ConvertUTF8toUTF16 fileNameA(fileName);
    // Extension as loaded via a file://
    bool regexMatch;
    nsTArray<nsString> regexResults;
    nsresult rv =
        RegexEval(kExtensionRegex, fileNameA,
                  /* aOnlyMatch = */ false, regexMatch, &regexResults);
    if (NS_FAILED(rv)) {
      return FilenameTypeAndDetails(kRegexFailure, Nothing());
    }
    if (regexMatch) {
      nsCString type = StringEndsWith(regexResults[2], u"mozilla.org.xpi"_ns)
                           ? kMozillaExtensionFile
                           : kOtherExtensionFile;
      const auto& extensionNameAndPath =
          Substring(regexResults[0], std::size("extensions/") - 1);
      return FilenameTypeAndDetails(
          type, Some(OptimizeFileName(extensionNameAndPath)));
    }

    // Single File
    rv = RegexEval(kSingleFileRegex, fileNameA, /* aOnlyMatch = */ true,
                   regexMatch);
    if (NS_FAILED(rv)) {
      return FilenameTypeAndDetails(kRegexFailure, Nothing());
    }
    if (regexMatch) {
      return FilenameTypeAndDetails(kSingleString, Some(nsCString(fileName)));
    }

    // Suspected userChromeJS script
    rv = RegexEval(kUCJSRegex, fileNameA, /* aOnlyMatch = */ true, regexMatch);
    if (NS_FAILED(rv)) {
      return FilenameTypeAndDetails(kRegexFailure, Nothing());
    }
    if (regexMatch) {
      return FilenameTypeAndDetails(kSuspectedUserChromeJS, Nothing());
    }
  }

  // Something loaded via an about:// URI.
  if (StringBeginsWith(fileName, "about:"_ns)) {
    return FilenameTypeAndDetails(kAboutUri, Some(StripQueryRef(fileName)));
  }

  // Something loaded via a moz-extension:// URI.
  if (StringBeginsWith(fileName, "moz-extension://"_ns)) {
    if (!collectAdditionalExtensionData) {
      return FilenameTypeAndDetails(kExtensionURI, Nothing());
    }

    nsAutoCString sanitizedPathAndScheme;
    sanitizedPathAndScheme.Append("moz-extension://["_ns);

    nsCOMPtr<nsIURI> uri;
    nsresult rv = NS_NewURI(getter_AddRefs(uri), fileName);
    if (NS_FAILED(rv)) {
      // Return after adding ://[ so we know we failed here.
      return FilenameTypeAndDetails(kExtensionURI,
                                    Some(sanitizedPathAndScheme));
    }

    mozilla::extensions::URLInfo url(uri);
    if (NS_IsMainThread()) {
      // EPS is only usable on main thread
      auto* policy =
          ExtensionPolicyService::GetSingleton().GetByHost(url.Host());
      if (policy) {
        nsString addOnId;
        policy->GetId(addOnId);

        sanitizedPathAndScheme.Append(NS_ConvertUTF16toUTF8(addOnId));
        sanitizedPathAndScheme.Append(": "_ns);
        sanitizedPathAndScheme.Append(NS_ConvertUTF16toUTF8(policy->Name()));
        sanitizedPathAndScheme.Append("]"_ns);

        if (policy->IsPrivileged()) {
          sanitizedPathAndScheme.Append("P=1"_ns);
        } else {
          sanitizedPathAndScheme.Append("P=0"_ns);
        }
      } else {
        sanitizedPathAndScheme.Append("failed finding addon by host]"_ns);
      }
    } else {
      sanitizedPathAndScheme.Append("can't get addon off main thread]"_ns);
    }

    sanitizedPathAndScheme.Append(url.FilePath());
    return FilenameTypeAndDetails(kExtensionURI, Some(sanitizedPathAndScheme));
  }

#if defined(XP_WIN)
  auto flags = mozilla::widget::WinUtils::PathTransformFlags::Default |
               mozilla::widget::WinUtils::PathTransformFlags::RequireFilePath;
  const NS_ConvertUTF8toUTF16 fileNameA(fileName);
  nsAutoString strSanitizedPath(fileNameA);
  if (widget::WinUtils::PreparePathForTelemetry(strSanitizedPath, flags)) {
    DWORD cchDecodedUrl = INTERNET_MAX_URL_LENGTH;
    WCHAR szOut[INTERNET_MAX_URL_LENGTH];
    HRESULT hr;
    SAFECALL_URLMON_FUNC(CoInternetParseUrl, fileNameA.get(), PARSE_SCHEMA, 0,
                         szOut, INTERNET_MAX_URL_LENGTH, &cchDecodedUrl, 0);
    if (hr == S_OK && cchDecodedUrl) {
      nsAutoString sanitizedPathAndScheme;
      sanitizedPathAndScheme.Append(szOut);
      if (sanitizedPathAndScheme == u"file"_ns) {
        sanitizedPathAndScheme.Append(u"://.../"_ns);
        sanitizedPathAndScheme.Append(strSanitizedPath);
      }
      return FilenameTypeAndDetails(
          kSanitizedWindowsURL,
          Some(NS_ConvertUTF16toUTF8(sanitizedPathAndScheme)));
    } else {
      return FilenameTypeAndDetails(
          kSanitizedWindowsPath, Some(NS_ConvertUTF16toUTF8(strSanitizedPath)));
    }
  }
#endif

  if (!NS_IsMainThread()) {
    return FilenameTypeAndDetails(kOtherWorker, Nothing());
  }
  return FilenameTypeAndDetails(kOther, Nothing());
}

#if defined(EARLY_BETA_OR_EARLIER)
// Crash String must be safe from a telemetry point of view.
// This will be ensured when this function is used.
void PossiblyCrash(const char* aPrefSuffix, const char* aUnsafeCrashString,
                   const nsCString& aSafeCrashString) {
  if (MOZ_UNLIKELY(!XRE_IsParentProcess())) {
    // We only crash in the parent (unfortunately) because it's
    // the only place we can be sure that our only-crash-once
    // pref-writing works.
    return;
  }
  if (!NS_IsMainThread()) {
    // Setting a pref off the main thread causes ContentParent to observe the
    // pref set, resulting in a Release Assertion when it tries to update the
    // child off main thread. So don't do any of this off main thread. (Which
    // is a bit of a blind spot for this purpose...)
    return;
  }

  nsCString previous_crashes("security.crash_tracking.");
  previous_crashes.Append(aPrefSuffix);
  previous_crashes.Append(".prevCrashes");

  nsCString max_crashes("security.crash_tracking.");
  max_crashes.Append(aPrefSuffix);
  max_crashes.Append(".maxCrashes");

  int32_t numberOfPreviousCrashes = 0;
  numberOfPreviousCrashes = Preferences::GetInt(previous_crashes.get(), 0);

  int32_t maxAllowableCrashes = 0;
  maxAllowableCrashes = Preferences::GetInt(max_crashes.get(), 0);

  if (numberOfPreviousCrashes >= maxAllowableCrashes) {
    return;
  }

  nsresult rv =
      Preferences::SetInt(previous_crashes.get(), ++numberOfPreviousCrashes);
  if (NS_FAILED(rv)) {
    return;
  }

  nsCOMPtr<nsIPrefService> prefsCom = Preferences::GetService();
  Preferences* prefs = static_cast<Preferences*>(prefsCom.get());

  if (!prefs->AllowOffMainThreadSave()) {
    // Do not crash if we can't save prefs off the main thread
    return;
  }

  rv = prefs->SavePrefFileBlocking();
  if (!NS_FAILED(rv)) {
    // We can only use this in local builds where we don't send stuff up to the
    // crash reporter because it has user private data.
    // MOZ_CRASH_UNSAFE_PRINTF("%s",
    //                        nsContentSecurityUtils::SmartFormatCrashString(aUnsafeCrashString));
    MOZ_CRASH_UNSAFE_PRINTF(
        "%s",
        nsContentSecurityUtils::SmartFormatCrashString(aSafeCrashString.get()));
  }
}
#endif

class EvalUsageNotificationRunnable final : public Runnable {
 public:
  EvalUsageNotificationRunnable(bool aIsSystemPrincipal,
                                const nsACString& aFileName, uint64_t aWindowID,
                                uint32_t aLineNumber, uint32_t aColumnNumber)
      : mozilla::Runnable("EvalUsageNotificationRunnable"),
        mIsSystemPrincipal(aIsSystemPrincipal),
        mFileName(aFileName),
        mWindowID(aWindowID),
        mLineNumber(aLineNumber),
        mColumnNumber(aColumnNumber) {}

  NS_IMETHOD Run() override {
    nsContentSecurityUtils::NotifyEvalUsage(
        mIsSystemPrincipal, mFileName, mWindowID, mLineNumber, mColumnNumber);
    return NS_OK;
  }

  void Revoke() {}

 private:
  bool mIsSystemPrincipal;
  nsCString mFileName;
  uint64_t mWindowID;
  uint32_t mLineNumber;
  uint32_t mColumnNumber;
};

/* static */
bool nsContentSecurityUtils::IsEvalAllowed(JSContext* cx,
                                           bool aIsSystemPrincipal,
                                           const nsAString& aScript) {
  // This allowlist contains files that are permanently allowed to use
  // eval()-like functions. It will ideally be restricted to files that are
  // exclusively used in testing contexts.
  static nsLiteralCString evalAllowlist[] = {
      // Test-only third-party library
      "resource://testing-common/sinon-7.2.7.js"_ns,
      // Test-only utility
      "resource://testing-common/content-task.js"_ns,

      // Tracked by Bug 1584605
      "resource://gre/modules/translations/cld-worker.js"_ns,

      // require.js implements a script loader for workers. It uses eval
      // to load the script; but injection is only possible in situations
      // that you could otherwise control script that gets executed, so
      // it is okay to allow eval() as it adds no additional attack surface.
      // Bug 1584564 tracks requiring safe usage of require.js
      "resource://gre/modules/workers/require.js"_ns,

      // The profiler's symbolication code uses a wasm module to extract symbols
      // from the binary files result of local builds.
      // See bug 1777479
      "resource://devtools/client/performance-new/shared/symbolication.sys.mjs"_ns,

      // The Browser Toolbox/Console
      "debugger"_ns,
  };

  // We also permit two specific idioms in eval()-like contexts. We'd like to
  // elminate these too; but there are in-the-wild Mozilla privileged extensions
  // that use them.
  static constexpr auto sAllowedEval1 = u"this"_ns;
  static constexpr auto sAllowedEval2 =
      u"function anonymous(\n) {\nreturn this\n}"_ns;

  if (MOZ_LIKELY(!aIsSystemPrincipal && !XRE_IsE10sParentProcess())) {
    // We restrict eval in the system principal and parent process.
    // Other uses (like web content and null principal) are allowed.
    return true;
  }

  if (JS::ContextOptionsRef(cx).disableEvalSecurityChecks()) {
    MOZ_LOG(sCSMLog, LogLevel::Debug,
            ("Allowing eval() because this JSContext was set to allow it"));
    return true;
  }

  if (StaticPrefs::
          security_allow_unsafe_dangerous_privileged_evil_eval_AtStartup()) {
    MOZ_LOG(
        sCSMLog, LogLevel::Debug,
        ("Allowing eval() because "
         "security.allow_unsafe_dangerous_priviliged_evil_eval is enabled."));
    return true;
  }

  if (aIsSystemPrincipal &&
      StaticPrefs::security_allow_eval_with_system_principal()) {
    MOZ_LOG(sCSMLog, LogLevel::Debug,
            ("Allowing eval() with System Principal because allowing pref is "
             "enabled"));
    return true;
  }

  if (XRE_IsE10sParentProcess() &&
      StaticPrefs::security_allow_eval_in_parent_process()) {
    MOZ_LOG(sCSMLog, LogLevel::Debug,
            ("Allowing eval() in parent process because allowing pref is "
             "enabled"));
    return true;
  }

  DetectJsHacks();
  if (MOZ_UNLIKELY(sJSHacksPresent)) {
    MOZ_LOG(
        sCSMLog, LogLevel::Debug,
        ("Allowing eval() %s because some "
         "JS hacks may be present.",
         (aIsSystemPrincipal ? "with System Principal" : "in parent process")));
    return true;
  }

  if (XRE_IsE10sParentProcess() &&
      !StaticPrefs::extensions_webextensions_remote()) {
    MOZ_LOG(sCSMLog, LogLevel::Debug,
            ("Allowing eval() in parent process because the web extension "
             "process is disabled"));
    return true;
  }

  // We permit these two common idioms to get access to the global JS object
  if (!aScript.IsEmpty() &&
      (aScript == sAllowedEval1 || aScript == sAllowedEval2)) {
    MOZ_LOG(
        sCSMLog, LogLevel::Debug,
        ("Allowing eval() %s because a key string is "
         "provided",
         (aIsSystemPrincipal ? "with System Principal" : "in parent process")));
    return true;
  }

  // Check the allowlist for the provided filename. getFilename is a helper
  // function
  auto location = JSCallingLocation::Get(cx);
  const nsCString& fileName = location.FileName();
  for (const nsLiteralCString& allowlistEntry : evalAllowlist) {
    // checking if current filename begins with entry, because JS Engine
    // gives us additional stuff for code inside eval or Function ctor
    // e.g., "require.js > Function"
    if (StringBeginsWith(fileName, allowlistEntry)) {
      MOZ_LOG(sCSMLog, LogLevel::Debug,
              ("Allowing eval() %s because the containing "
               "file is in the allowlist",
               (aIsSystemPrincipal ? "with System Principal"
                                   : "in parent process")));
      return true;
    }
  }

  // Send Telemetry and Log to the Console
  uint64_t windowID = nsJSUtils::GetCurrentlyRunningCodeInnerWindowID(cx);
  if (NS_IsMainThread()) {
    nsContentSecurityUtils::NotifyEvalUsage(aIsSystemPrincipal, fileName,
                                            windowID, location.mLine,
                                            location.mColumn);
  } else {
    auto runnable = new EvalUsageNotificationRunnable(
        aIsSystemPrincipal, fileName, windowID, location.mLine,
        location.mColumn);
    NS_DispatchToMainThread(runnable);
  }

  // Log to MOZ_LOG
  MOZ_LOG(sCSMLog, LogLevel::Error,
          ("Blocking eval() %s from file %s and script "
           "provided %s",
           (aIsSystemPrincipal ? "with System Principal" : "in parent process"),
           fileName.get(), NS_ConvertUTF16toUTF8(aScript).get()));

  // Maybe Crash
#if defined(DEBUG) || defined(FUZZING)
  auto crashString = nsContentSecurityUtils::SmartFormatCrashString(
      NS_ConvertUTF16toUTF8(aScript).get(), fileName.get(),
      (aIsSystemPrincipal
           ? "Blocking eval() with System Principal with script %s from file %s"
           : "Blocking eval() in parent process with script %s from file %s"));
  MOZ_CRASH_UNSAFE_PRINTF("%s", crashString.get());
#endif

  return false;
}

/* static */
void nsContentSecurityUtils::NotifyEvalUsage(bool aIsSystemPrincipal,
                                             const nsACString& aFileName,
                                             uint64_t aWindowID,
                                             uint32_t aLineNumber,
                                             uint32_t aColumnNumber) {
  FilenameTypeAndDetails fileNameTypeAndDetails =
      FilenameToFilenameType(aFileName, false);
  auto fileinfo = fileNameTypeAndDetails.second;
  auto value = Some(fileNameTypeAndDetails.first);
  if (aIsSystemPrincipal) {
    glean::security::EvalUsageSystemContextExtra extra = {
        .fileinfo = fileinfo,
        .value = value,
    };
    glean::security::eval_usage_system_context.Record(Some(extra));
  } else {
    glean::security::EvalUsageParentProcessExtra extra = {
        .fileinfo = fileinfo,
        .value = value,
    };
    glean::security::eval_usage_parent_process.Record(Some(extra));
  }

  // Report an error to console
  nsCOMPtr<nsIConsoleService> console(
      do_GetService(NS_CONSOLESERVICE_CONTRACTID));
  if (!console) {
    return;
  }
  nsCOMPtr<nsIScriptError> error(do_CreateInstance(NS_SCRIPTERROR_CONTRACTID));
  if (!error) {
    return;
  }
  nsCOMPtr<nsIStringBundle> bundle;
  nsCOMPtr<nsIStringBundleService> stringService =
      mozilla::components::StringBundle::Service();
  if (!stringService) {
    return;
  }
  stringService->CreateBundle(
      "chrome://global/locale/security/security.properties",
      getter_AddRefs(bundle));
  if (!bundle) {
    return;
  }
  nsAutoString message;
  NS_ConvertUTF8toUTF16 fileNameA(aFileName);
  AutoTArray<nsString, 1> formatStrings = {fileNameA};
  nsresult rv = bundle->FormatStringFromName("RestrictBrowserEvalUsage",
                                             formatStrings, message);
  if (NS_FAILED(rv)) {
    return;
  }

  rv = error->InitWithWindowID(message, aFileName, aLineNumber, aColumnNumber,
                               nsIScriptError::errorFlag, "BrowserEvalUsage",
                               aWindowID, true /* From chrome context */);
  if (NS_FAILED(rv)) {
    return;
  }
  console->LogMessage(error);
}

// If we detect that one of the relevant prefs has been changed, reset
// sJSHacksChecked to cause us to re-evaluate all the pref values.
// This will stop us from crashing because a user enabled one of these
// prefs during a session and then triggered the JavaScript load mitigation
// (which can cause a crash).
class JSHackPrefObserver final {
 public:
  JSHackPrefObserver() = default;
  static void PrefChanged(const char* aPref, void* aData);

 protected:
  ~JSHackPrefObserver() = default;
};

// static
void JSHackPrefObserver::PrefChanged(const char* aPref, void* aData) {
  sJSHacksChecked = false;
}

static bool sJSHackObserverAdded = false;

/* static */
void nsContentSecurityUtils::DetectJsHacks() {
  // We can only perform the check of this preference on the Main Thread
  // (because a String-based preference check is only safe on Main Thread.)
  // In theory, it would be possible that a separate thread could get here
  // before the main thread, resulting in the other thread not being able to
  // perform this check, but the odds of that are small (and probably zero.)
  if (!NS_IsMainThread()) {
    return;
  }

  // If the pref service isn't available, do nothing and re-do this later.
  if (!Preferences::IsServiceAvailable()) {
    return;
  }

  // No need to check again.
  if (MOZ_LIKELY(sJSHacksChecked || sJSHacksPresent)) {
    return;
  }

  static const char* kObservedPrefs[] = {
      "xpinstall.signatures.required", "general.config.filename",
      "autoadmin.global_config_url", "autoadmin.failover_to_cached", nullptr};
  if (MOZ_UNLIKELY(!sJSHackObserverAdded)) {
    Preferences::RegisterCallbacks(JSHackPrefObserver::PrefChanged,
                                   kObservedPrefs);
    sJSHackObserverAdded = true;
  }

  nsresult rv;
  sJSHacksChecked = true;

  // This preference is required by bootstrapLoader.xpi, which is an
  // alternate way to load legacy-style extensions. It only works on
  // DevEdition/Nightly.
  bool xpinstallSignatures;
  rv = Preferences::GetBool("xpinstall.signatures.required",
                            &xpinstallSignatures, PrefValueKind::Default);
  if (!NS_FAILED(rv) && !xpinstallSignatures) {
    sJSHacksPresent = true;
    return;
  }
  rv = Preferences::GetBool("xpinstall.signatures.required",
                            &xpinstallSignatures, PrefValueKind::User);
  if (!NS_FAILED(rv) && !xpinstallSignatures) {
    sJSHacksPresent = true;
    return;
  }

  if (Preferences::HasDefaultValue("general.config.filename")) {
    sJSHacksPresent = true;
    return;
  }
  if (Preferences::HasUserValue("general.config.filename")) {
    sJSHacksPresent = true;
    return;
  }
  if (Preferences::HasDefaultValue("autoadmin.global_config_url")) {
    sJSHacksPresent = true;
    return;
  }
  if (Preferences::HasUserValue("autoadmin.global_config_url")) {
    sJSHacksPresent = true;
    return;
  }

  bool failOverToCache;
  rv = Preferences::GetBool("autoadmin.failover_to_cached", &failOverToCache,
                            PrefValueKind::Default);
  if (!NS_FAILED(rv) && failOverToCache) {
    sJSHacksPresent = true;
    return;
  }
  rv = Preferences::GetBool("autoadmin.failover_to_cached", &failOverToCache,
                            PrefValueKind::User);
  if (!NS_FAILED(rv) && failOverToCache) {
    sJSHacksPresent = true;
  }
}

/* static */
void nsContentSecurityUtils::DetectCssHacks() {
  // We can only perform the check of this preference on the Main Thread
  // It's possible that this function may therefore race and we expect the
  // caller to ensure that the checks have actually happened.
  if (!NS_IsMainThread()) {
    return;
  }

  // If the pref service isn't available, do nothing and re-do this later.
  if (!Preferences::IsServiceAvailable()) {
    return;
  }

  // No need to check again.
  if (MOZ_LIKELY(sCSSHacksChecked || sCSSHacksPresent)) {
    return;
  }

  // This preference is a bool to see if userChrome css is loaded
  bool customStylesPresent = Preferences::GetBool(
      "toolkit.legacyUserProfileCustomizations.stylesheets", false);
  if (customStylesPresent) {
    sCSSHacksPresent = true;
  }

  sCSSHacksChecked = true;
}

/* static */
nsresult nsContentSecurityUtils::GetHttpChannelFromPotentialMultiPart(
    nsIChannel* aChannel, nsIHttpChannel** aHttpChannel) {
  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(aChannel);
  if (httpChannel) {
    httpChannel.forget(aHttpChannel);
    return NS_OK;
  }

  nsCOMPtr<nsIMultiPartChannel> multipart = do_QueryInterface(aChannel);
  if (!multipart) {
    *aHttpChannel = nullptr;
    return NS_OK;
  }

  nsCOMPtr<nsIChannel> baseChannel;
  nsresult rv = multipart->GetBaseChannel(getter_AddRefs(baseChannel));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  httpChannel = do_QueryInterface(baseChannel);
  httpChannel.forget(aHttpChannel);

  return NS_OK;
}

nsresult CheckCSPFrameAncestorPolicy(nsIChannel* aChannel,
                                     nsIContentSecurityPolicy** aOutCSP) {
  MOZ_ASSERT(aChannel);

  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
  ExtContentPolicyType contentType = loadInfo->GetExternalContentPolicyType();
  // frame-ancestor check only makes sense for subdocument and object loads,
  // if this is not a load of such type, there is nothing to do here.
  if (contentType != ExtContentPolicy::TYPE_SUBDOCUMENT &&
      contentType != ExtContentPolicy::TYPE_OBJECT) {
    return NS_OK;
  }

  // CSP can only hang off an http channel, if this channel is not
  // an http channel then there is nothing to do here,
  // except with add-ons, where the CSP is stored in a WebExtensionPolicy.
  nsCOMPtr<nsIHttpChannel> httpChannel;
  nsresult rv = nsContentSecurityUtils::GetHttpChannelFromPotentialMultiPart(
      aChannel, getter_AddRefs(httpChannel));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsAutoCString tCspHeaderValue, tCspROHeaderValue;
  if (httpChannel) {
    Unused << httpChannel->GetResponseHeader("content-security-policy"_ns,
                                             tCspHeaderValue);

    Unused << httpChannel->GetResponseHeader(
        "content-security-policy-report-only"_ns, tCspROHeaderValue);

    // if there are no CSP values, then there is nothing to do here.
    if (tCspHeaderValue.IsEmpty() && tCspROHeaderValue.IsEmpty()) {
      return NS_OK;
    }
  }

  nsCOMPtr<nsIPrincipal> resultPrincipal;
  rv = nsContentUtils::GetSecurityManager()->GetChannelResultPrincipal(
      aChannel, getter_AddRefs(resultPrincipal));
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<extensions::WebExtensionPolicy> addonPolicy;
  if (!httpChannel) {
    addonPolicy = BasePrincipal::Cast(resultPrincipal)->AddonPolicy();
    if (!addonPolicy) {
      // Neither a HTTP channel, nor a moz-extension:-resource.
      // CSP is not supported.
      return NS_OK;
    }
  }

  RefPtr<nsCSPContext> csp = new nsCSPContext();
  // This CSPContext is only used for checking frame-ancestors, we
  // will parse the CSP again anyway. (Unless this blocks the load, but
  // parser warnings aren't really important in that case)
  csp->SuppressParserLogMessages();

  nsCOMPtr<nsIURI> selfURI;
  nsAutoCString referrerSpec;
  if (httpChannel) {
    aChannel->GetURI(getter_AddRefs(selfURI));
    nsCOMPtr<nsIReferrerInfo> referrerInfo = httpChannel->GetReferrerInfo();
    if (referrerInfo) {
      referrerInfo->GetComputedReferrerSpec(referrerSpec);
    }
  } else {
    // aChannel::GetURI would return the jar: or file:-URI for extensions.
    // Use the "final" URI to get the actual moz-extension:-URL.
    NS_GetFinalChannelURI(aChannel, getter_AddRefs(selfURI));
  }

  uint64_t innerWindowID = loadInfo->GetInnerWindowID();

  rv = csp->SetRequestContextWithPrincipal(resultPrincipal, selfURI,
                                           referrerSpec, innerWindowID);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (addonPolicy) {
    csp->AppendPolicy(addonPolicy->BaseCSP(), false, false);
    csp->AppendPolicy(addonPolicy->ExtensionPageCSP(), false, false);
  } else {
    NS_ConvertASCIItoUTF16 cspHeaderValue(tCspHeaderValue);
    NS_ConvertASCIItoUTF16 cspROHeaderValue(tCspROHeaderValue);

    // ----- if there's a full-strength CSP header, apply it.
    if (!cspHeaderValue.IsEmpty()) {
      rv = CSP_AppendCSPFromHeader(csp, cspHeaderValue, false);
      NS_ENSURE_SUCCESS(rv, rv);
    }

    // ----- if there's a report-only CSP header, apply it.
    if (!cspROHeaderValue.IsEmpty()) {
      rv = CSP_AppendCSPFromHeader(csp, cspROHeaderValue, true);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  // ----- Enforce frame-ancestor policy on any applied policies
  bool safeAncestry = false;
  // PermitsAncestry sends violation reports when necessary
  rv = csp->PermitsAncestry(loadInfo, &safeAncestry);

  if (NS_FAILED(rv) || !safeAncestry) {
    // stop!  ERROR page!
    return NS_ERROR_CSP_FRAME_ANCESTOR_VIOLATION;
  }

  // return the CSP for x-frame-options check
  csp.forget(aOutCSP);

  return NS_OK;
}

void EnforceCSPFrameAncestorPolicy(nsIChannel* aChannel,
                                   const nsresult& aError) {
  if (aError == NS_ERROR_CSP_FRAME_ANCESTOR_VIOLATION) {
    aChannel->Cancel(NS_ERROR_CSP_FRAME_ANCESTOR_VIOLATION);
  }
}

void EnforceXFrameOptionsCheck(nsIChannel* aChannel,
                               nsIContentSecurityPolicy* aCsp) {
  MOZ_ASSERT(aChannel);
  bool isFrameOptionsIgnored = false;
  // check for XFO options
  // XFO checks can be skipped if there are frame ancestors
  if (!FramingChecker::CheckFrameOptions(aChannel, aCsp,
                                         isFrameOptionsIgnored)) {
    // stop!  ERROR page!
    aChannel->Cancel(NS_ERROR_XFO_VIOLATION);
  }

  if (isFrameOptionsIgnored) {
    // log warning to console that xfo is ignored because of CSP
    nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
    uint64_t innerWindowID = loadInfo->GetInnerWindowID();
    bool privateWindow = loadInfo->GetOriginAttributes().IsPrivateBrowsing();
    AutoTArray<nsString, 2> params = {u"x-frame-options"_ns,
                                      u"frame-ancestors"_ns};
    CSP_LogLocalizedStr("IgnoringSrcBecauseOfDirective", params,
                        ""_ns,   // no sourcefile
                        u""_ns,  // no scriptsample
                        0,       // no linenumber
                        1,       // no columnnumber
                        nsIScriptError::warningFlag,
                        "IgnoringSrcBecauseOfDirective"_ns, innerWindowID,
                        privateWindow);
  }
}

/* static */
void nsContentSecurityUtils::PerformCSPFrameAncestorAndXFOCheck(
    nsIChannel* aChannel) {
  nsCOMPtr<nsIContentSecurityPolicy> csp;
  nsresult rv = CheckCSPFrameAncestorPolicy(aChannel, getter_AddRefs(csp));

  if (NS_FAILED(rv)) {
    EnforceCSPFrameAncestorPolicy(aChannel, rv);
    return;
  }

  // X-Frame-Options needs to be enforced after CSP frame-ancestors
  // checks because if frame-ancestors is present, then x-frame-options
  // will be discarded
  EnforceXFrameOptionsCheck(aChannel, csp);
}
/* static */
bool nsContentSecurityUtils::CheckCSPFrameAncestorAndXFO(nsIChannel* aChannel) {
  nsCOMPtr<nsIContentSecurityPolicy> csp;
  nsresult rv = CheckCSPFrameAncestorPolicy(aChannel, getter_AddRefs(csp));

  if (NS_FAILED(rv)) {
    return false;
  }

  bool isFrameOptionsIgnored = false;

  return FramingChecker::CheckFrameOptions(aChannel, csp,
                                           isFrameOptionsIgnored);
}

// https://w3c.github.io/webappsec-csp/#is-element-nonceable
/* static */
nsString nsContentSecurityUtils::GetIsElementNonceableNonce(
    const Element& aElement) {
  // Step 1. If element does not have an attribute named "nonce", return "Not
  // Nonceable".
  nsString nonce;
  if (nsString* cspNonce =
          static_cast<nsString*>(aElement.GetProperty(nsGkAtoms::nonce))) {
    nonce = *cspNonce;
  }
  if (nonce.IsEmpty()) {
    return nonce;
  }

  // Step 2. If element is a script element, then for each attribute of
  // element’s attribute list:
  if (nsCOMPtr<nsIScriptElement> script =
          do_QueryInterface(const_cast<Element*>(&aElement))) {
    auto containsScriptOrStyle = [](const nsAString& aStr) {
      return aStr.LowerCaseFindASCII("<script") != kNotFound ||
             aStr.LowerCaseFindASCII("<style") != kNotFound;
    };

    nsString value;
    uint32_t i = 0;
    while (BorrowedAttrInfo info = aElement.GetAttrInfoAt(i++)) {
      // Step 2.1. If attribute’s name contains an ASCII case-insensitive match
      // for "<script" or "<style", return "Not Nonceable".
      const nsAttrName* name = info.mName;
      if (nsAtom* prefix = name->GetPrefix()) {
        if (containsScriptOrStyle(nsDependentAtomString(prefix))) {
          return EmptyString();
        }
      }
      if (containsScriptOrStyle(nsDependentAtomString(name->LocalName()))) {
        return EmptyString();
      }

      // Step 2.2. If attribute’s value contains an ASCII case-insensitive match
      // for "<script" or "<style", return "Not Nonceable".
      info.mValue->ToString(value);
      if (containsScriptOrStyle(value)) {
        return EmptyString();
      }
    }
  }

  // Step 3. If element had a duplicate-attribute parse error during
  // tokenization, return "Not Nonceable".
  if (aElement.HasFlag(ELEMENT_PARSER_HAD_DUPLICATE_ATTR_ERROR)) {
    return EmptyString();
  }

  // Step 4. Return "Nonceable".
  return nonce;
}

#if defined(DEBUG)

#  include "mozilla/dom/nsCSPContext.h"

// The follow lists define the exceptions to the usual default list
// of allowed CSP sources for internal pages. The default list
// allows chrome: and resource: URLs for everything, with the exception
// of object-src.
//
// Generally adding something to these lists should be seen as a bad
// sign, but it is obviously impossible for some pages, e.g.
// those that are meant to include content from the web.
//
// Do note: We will _never_ allow any additional source for scripts
// (script-src, script-src-elem, script-src-attr, worker-src)

// style-src data:
//  This is more or less the same as allowing arbitrary inline styles.
static nsLiteralCString sStyleSrcDataAllowList[] = {
    "about:preferences"_ns, "about:settings"_ns,
    // STOP! Do not add anything to this list.
};
// style-src 'unsafe-inline'
static nsLiteralCString sStyleSrcUnsafeInlineAllowList[] = {
    // Bug 1579160: Remove 'unsafe-inline' from style-src within
    // about:preferences
    "about:preferences"_ns,
    "about:settings"_ns,
    // Bug 1571346: Remove 'unsafe-inline' from style-src within about:addons
    "about:addons"_ns,
    // Bug 1584485: Remove 'unsafe-inline' from style-src within:
    // * about:newtab
    // * about:welcome
    // * about:home
    "about:newtab"_ns,
    "about:welcome"_ns,
    "about:home"_ns,
    "chrome://browser/content/pageinfo/pageInfo.xhtml"_ns,
    "chrome://browser/content/places/bookmarkProperties.xhtml"_ns,
    "chrome://browser/content/places/bookmarksSidebar.xhtml"_ns,
    "chrome://browser/content/places/historySidebar.xhtml"_ns,
    "chrome://browser/content/places/interactionsViewer.html"_ns,
    "chrome://browser/content/places/places.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/applicationManager.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/browserLanguages.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/clearSiteData.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/colors.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/connection.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/containers.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/dohExceptions.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/fonts.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/languages.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/permissions.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/selectBookmark.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/siteDataSettings.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/sitePermissions.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/syncChooseWhatToSync.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/translations.xhtml"_ns,
    "chrome://browser/content/preferences/fxaPairDevice.xhtml"_ns,
    "chrome://browser/content/safeMode.xhtml"_ns,
    "chrome://browser/content/sanitize.xhtml"_ns,
    "chrome://browser/content/sanitize_v2.xhtml"_ns,
    "chrome://browser/content/search/addEngine.xhtml"_ns,
    "chrome://browser/content/setDesktopBackground.xhtml"_ns,
    "chrome://browser/content/spotlight.html"_ns,
    "chrome://devtools/content/debugger/index.html"_ns,
    "chrome://devtools/content/framework/browser-toolbox/window.html"_ns,
    "chrome://devtools/content/framework/toolbox-options.html"_ns,
    "chrome://devtools/content/framework/toolbox-window.xhtml"_ns,
    "chrome://devtools/content/inspector/index.xhtml"_ns,
    "chrome://devtools/content/inspector/markup/markup.xhtml"_ns,
    "chrome://devtools/content/memory/index.xhtml"_ns,
    "chrome://devtools/content/shared/sourceeditor/codemirror/cmiframe.html"_ns,
    "chrome://formautofill/content/manageAddresses.xhtml"_ns,
    "chrome://formautofill/content/manageCreditCards.xhtml"_ns,
    "chrome://gfxsanity/content/sanityparent.html"_ns,
    "chrome://gfxsanity/content/sanitytest.html"_ns,
    "chrome://global/content/commonDialog.xhtml"_ns,
    "chrome://global/content/resetProfileProgress.xhtml"_ns,
    "chrome://layoutdebug/content/layoutdebug.xhtml"_ns,
    "chrome://mozapps/content/downloads/unknownContentType.xhtml"_ns,
    "chrome://mozapps/content/handling/appChooser.xhtml"_ns,
    "chrome://mozapps/content/preferences/changemp.xhtml"_ns,
    "chrome://mozapps/content/preferences/removemp.xhtml"_ns,
    "chrome://mozapps/content/profile/profileDowngrade.xhtml"_ns,
    "chrome://mozapps/content/profile/profileSelection.xhtml"_ns,
    "chrome://mozapps/content/profile/createProfileWizard.xhtml"_ns,
    "chrome://mozapps/content/update/history.xhtml"_ns,
    "chrome://mozapps/content/update/updateElevation.xhtml"_ns,
    "chrome://pippki/content/certManager.xhtml"_ns,
    "chrome://pippki/content/changepassword.xhtml"_ns,
    "chrome://pippki/content/deletecert.xhtml"_ns,
    "chrome://pippki/content/device_manager.xhtml"_ns,
    "chrome://pippki/content/downloadcert.xhtml"_ns,
    "chrome://pippki/content/editcacert.xhtml"_ns,
    "chrome://pippki/content/load_device.xhtml"_ns,
    "chrome://pippki/content/setp12password.xhtml"_ns,
};
// img-src data: blob:
static nsLiteralCString sImgSrcDataBlobAllowList[] = {
    "about:debugging"_ns,
    "about:devtools-toolbox"_ns,
    "about:firefoxview"_ns,
    "about:home"_ns,
    "about:inference"_ns,
    "about:logins"_ns,
    "about:newtab"_ns,
    "about:preferences"_ns,
    "about:privatebrowsing"_ns,
    "about:processes"_ns,
    "about:protections"_ns,
    "about:reader"_ns,
    "about:sessionrestore"_ns,
    "about:settings"_ns,
    "about:shoppingsidebar"_ns,
    "about:test-about-content-search-ui"_ns,
    "about:welcome"_ns,
    "chrome://browser/content/aboutDialog.xhtml"_ns,
    "chrome://browser/content/aboutlogins/aboutLogins.html"_ns,
    "chrome://browser/content/genai/chat.html"_ns,
    "chrome://browser/content/pageinfo/pageInfo.xhtml"_ns,
    "chrome://browser/content/places/bookmarksSidebar.xhtml"_ns,
    "chrome://browser/content/places/places.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/permissions.xhtml"_ns,
    "chrome://browser/content/preferences/fxaPairDevice.xhtml"_ns,
    "chrome://browser/content/screenshots/screenshots-preview.html"_ns,
    "chrome://browser/content/shopping/shopping.html"_ns,
    "chrome://browser/content/sidebar/sidebar-customize.html"_ns,
    "chrome://browser/content/sidebar/sidebar-history.html"_ns,
    "chrome://browser/content/sidebar/sidebar-syncedtabs.html"_ns,
    "chrome://browser/content/spotlight.html"_ns,
    "chrome://browser/content/syncedtabs/sidebar.xhtml"_ns,
    "chrome://browser/content/webext-panels.xhtml"_ns,
    "chrome://devtools/content/application/index.html"_ns,
    "chrome://devtools/content/framework/browser-toolbox/window.html"_ns,
    "chrome://devtools/content/framework/toolbox-window.xhtml"_ns,
    "chrome://devtools/content/inspector/index.xhtml"_ns,
    "chrome://devtools/content/inspector/markup/markup.xhtml"_ns,
    "chrome://devtools/content/netmonitor/index.html"_ns,
    "chrome://devtools/content/responsive/toolbar.xhtml"_ns,
    "chrome://devtools/content/shared/sourceeditor/codemirror/cmiframe.html"_ns,
    "chrome://devtools/content/webconsole/index.html"_ns,
    "chrome://global/content/print.html"_ns,
};
// img-src https:
static nsLiteralCString sImgSrcHttpsAllowList[] = {
    "about:addons"_ns,
    "about:debugging"_ns,
    "about:home"_ns,
    "about:newtab"_ns,
    "about:preferences"_ns,
    "about:settings"_ns,
    "about:welcome"_ns,
    "chrome://devtools/content/application/index.html"_ns,
    "chrome://devtools/content/framework/browser-toolbox/window.html"_ns,
    "chrome://devtools/content/framework/toolbox-window.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/applicationManager.xhtml"_ns,
    "chrome://global/content/alerts/alert.xhtml"_ns,
    "chrome://mozapps/content/handling/appChooser.xhtml"_ns,
};
// img-src http:
//  UNSAFE! Do not use.
static nsLiteralCString sImgSrcHttpAllowList[] = {
    "about:addons"_ns,
    "chrome://devtools/content/application/index.html"_ns,
    "chrome://devtools/content/framework/browser-toolbox/window.html"_ns,
    "chrome://devtools/content/framework/toolbox-window.xhtml"_ns,
    "chrome://browser/content/preferences/dialogs/applicationManager.xhtml"_ns,
    "chrome://mozapps/content/handling/appChooser.xhtml"_ns,
    // STOP! Do not add anything to this list.
};
// img-src jar: file:
//  UNSAFE! Do not use.
static nsLiteralCString sImgSrcAddonsAllowList[] = {
    "about:addons"_ns,
    // STOP! Do not add anything to this list.
};
// img-src *
//  UNSAFE! Allows loading everything.
static nsLiteralCString sImgSrcWildcardAllowList[] = {
    "about:reader"_ns, "chrome://browser/content/pageinfo/pageInfo.xhtml"_ns,
    "chrome://browser/content/syncedtabs/sidebar.xhtml"_ns,
    // STOP! Do not add anything to this list.
};
// img-src https://example.org
//  Any https host source.
static nsLiteralCString sImgSrcHttpsHostAllowList[] = {
    "about:logins"_ns,
    "about:pocket-home"_ns,
    "about:pocket-saved"_ns,
    "chrome://browser/content/aboutlogins/aboutLogins.html"_ns,
    "chrome://browser/content/spotlight.html"_ns,
};
// media-src data: blob:
static nsLiteralCString sMediaSrcDataBlobAllowList[] = {
    "chrome://browser/content/pageinfo/pageInfo.xhtml"_ns,
};
// media-src *
//  UNSAFE! Allows loading everything.
static nsLiteralCString sMediaSrcWildcardAllowList[] = {
    "about:reader"_ns, "chrome://browser/content/pageinfo/pageInfo.xhtml"_ns,
    // STOP! Do not add anything to this list.
};
// media-src https://example.org
//  Any https host source.
static nsLiteralCString sMediaSrcHttpsHostAllowList[] = {"about:welcome"_ns};
// connect-src https:
static nsLiteralCString sConnectSrcHttpsAllowList[] = {
    "about:addons"_ns,
    "about:home"_ns,
    "about:newtab"_ns,
    "about:welcome"_ns,
};
// connect-src data: http:
//  UNSAFE! Do not use.
static nsLiteralCString sConnectSrcAddonsAllowList[] = {
    "about:addons"_ns,
    // STOP! Do not add anything to this list.
};
// connect-src https://example.org
//  Any https host source.
static nsLiteralCString sConnectSrcHttpsHostAllowList[] = {"about:logging"_ns};

class DisallowingVisitor : public nsCSPSrcVisitor {
 public:
  DisallowingVisitor(CSPDirective aDirective, nsACString& aURL)
      : mDirective(aDirective), mURL(aURL) {}

  bool visit(const nsCSPPolicy* aPolicy) {
    return aPolicy->visitDirectiveSrcs(mDirective, this);
  }

  bool visitSchemeSrc(const nsCSPSchemeSrc& src) override {
    Assert(src);
    return false;
  };

  bool visitHostSrc(const nsCSPHostSrc& src) override {
    Assert(src);
    return false;
  };

  bool visitKeywordSrc(const nsCSPKeywordSrc& src) override {
    // Using the 'none' keyword doesn't allow anything.
    if (src.isKeyword(CSPKeyword::CSP_NONE)) {
      return true;
    }

    Assert(src);
    return false;
  }

  bool visitNonceSrc(const nsCSPNonceSrc& src) override {
    Assert(src);
    return false;
  };

  bool visitHashSrc(const nsCSPHashSrc& src) override {
    Assert(src);
    return false;
  };

 protected:
  bool CheckAllowList(Span<nsLiteralCString> aList) {
    for (const nsLiteralCString& entry : aList) {
      // please note that we perform a substring match here on purpose,
      // so we don't have to deal and parse out all the query arguments
      // the various about pages rely on.
      if (StringBeginsWith(mURL, entry)) {
        return true;
      }
    }

    return false;
  }

  void Assert(const nsCSPBaseSrc& aSrc) {
    nsAutoString srcStr;
    aSrc.toString(srcStr);
    NS_ConvertUTF16toUTF8 srcStrUtf8(srcStr);

    MOZ_CRASH_UNSAFE_PRINTF(
        "Page %s must not contain a CSP with the "
        "directive %s that includes %s",
        mURL.get(), CSP_CSPDirectiveToString(mDirective), srcStrUtf8.get());
  }

  CSPDirective mDirective;
  nsCString mURL;
};

// Only allows loads from chrome:, moz-src: and resource: URLs:
class AllowBuiltinSrcVisitor : public DisallowingVisitor {
 public:
  AllowBuiltinSrcVisitor(CSPDirective aDirective, nsACString& aURL)
      : DisallowingVisitor(aDirective, aURL) {}

  bool visitSchemeSrc(const nsCSPSchemeSrc& src) override {
    nsAutoString scheme;
    src.getScheme(scheme);
    if (scheme == u"chrome"_ns || scheme == u"moz-src" ||
        scheme == u"resource"_ns) {
      return true;
    }

    return DisallowingVisitor::visitSchemeSrc(src);
  }

 protected:
  bool VisitHostSrcWithWildcardAndHttpsHostAllowLists(
      const nsCSPHostSrc& aSrc, const Span<nsLiteralCString> aWildcard,
      const Span<nsLiteralCString> aHttpsHost) {
    nsAutoString str;
    aSrc.toString(str);

    if (str.EqualsLiteral("*")) {
      if (CheckAllowList(aWildcard)) {
        return true;
      }
    } else {
      MOZ_ASSERT(StringBeginsWith(str, u"https://"_ns),
                 "Must use https: for host sources!");
      MOZ_ASSERT(!FindInReadable(u"*"_ns, str),
                 "Can not include wildcard in host sources!");
      if (CheckAllowList(aHttpsHost)) {
        return true;
      }
    }

    return DisallowingVisitor::visitHostSrc(aSrc);
  }
};

class StyleSrcVisitor : public AllowBuiltinSrcVisitor {
 public:
  StyleSrcVisitor(CSPDirective aDirective, nsACString& aURL)
      : AllowBuiltinSrcVisitor(aDirective, aURL) {
    MOZ_ASSERT(aDirective == CSPDirective::STYLE_SRC_DIRECTIVE);
  }

  bool visitSchemeSrc(const nsCSPSchemeSrc& src) override {
    nsAutoString scheme;
    src.getScheme(scheme);

    if (scheme == u"data"_ns) {
      if (CheckAllowList(Span(sStyleSrcDataAllowList))) {
        return true;
      }
    }

    return AllowBuiltinSrcVisitor::visitSchemeSrc(src);
  }

  bool visitKeywordSrc(const nsCSPKeywordSrc& src) override {
    if (src.isKeyword(CSPKeyword::CSP_UNSAFE_INLINE)) {
      if (CheckAllowList(Span(sStyleSrcUnsafeInlineAllowList))) {
        return true;
      }
    }

    return AllowBuiltinSrcVisitor::visitKeywordSrc(src);
  }
};

class ImgSrcVisitor : public AllowBuiltinSrcVisitor {
 public:
  ImgSrcVisitor(CSPDirective aDirective, nsACString& aURL)
      : AllowBuiltinSrcVisitor(aDirective, aURL) {
    MOZ_ASSERT(aDirective == CSPDirective::IMG_SRC_DIRECTIVE);
  }

  bool visitSchemeSrc(const nsCSPSchemeSrc& src) override {
    nsAutoString scheme;
    src.getScheme(scheme);

    // moz-icon is used for loading known favicons.
    if (scheme == u"moz-icon"_ns) {
      return true;
    }

    // data: and blob: can be used to decode arbitrary images.
    if (scheme == u"data"_ns || scheme == u"blob") {
      if (CheckAllowList(sImgSrcDataBlobAllowList)) {
        return true;
      }
    }

    if (scheme == u"https"_ns) {
      if (CheckAllowList(Span(sImgSrcHttpsAllowList))) {
        return true;
      }
    }

    if (scheme == u"http"_ns) {
      if (CheckAllowList(Span(sImgSrcHttpAllowList))) {
        return true;
      }
    }

    if (scheme == u"jar"_ns || scheme == u"file"_ns) {
      if (CheckAllowList(Span(sImgSrcAddonsAllowList))) {
        return true;
      }
    }

    return AllowBuiltinSrcVisitor::visitSchemeSrc(src);
  }

  bool visitHostSrc(const nsCSPHostSrc& src) override {
    return VisitHostSrcWithWildcardAndHttpsHostAllowLists(
        src, sImgSrcWildcardAllowList, sImgSrcHttpsHostAllowList);
  }
};

class MediaSrcVisitor : public AllowBuiltinSrcVisitor {
 public:
  MediaSrcVisitor(CSPDirective aDirective, nsACString& aURL)
      : AllowBuiltinSrcVisitor(aDirective, aURL) {
    MOZ_ASSERT(aDirective == CSPDirective::MEDIA_SRC_DIRECTIVE);
  }

  bool visitSchemeSrc(const nsCSPSchemeSrc& src) override {
    nsAutoString scheme;
    src.getScheme(scheme);

    // data: and blob: can be used to decode arbitrary media.
    if (scheme == u"data"_ns || scheme == u"blob") {
      if (CheckAllowList(sMediaSrcDataBlobAllowList)) {
        return true;
      }
    }

    return AllowBuiltinSrcVisitor::visitSchemeSrc(src);
  }

  bool visitHostSrc(const nsCSPHostSrc& src) override {
    return VisitHostSrcWithWildcardAndHttpsHostAllowLists(
        src, sMediaSrcWildcardAllowList, sMediaSrcHttpsHostAllowList);
  }
};

class ConnectSrcVisitor : public AllowBuiltinSrcVisitor {
 public:
  ConnectSrcVisitor(CSPDirective aDirective, nsACString& aURL)
      : AllowBuiltinSrcVisitor(aDirective, aURL) {
    MOZ_ASSERT(aDirective == CSPDirective::CONNECT_SRC_DIRECTIVE);
  }

  bool visitSchemeSrc(const nsCSPSchemeSrc& src) override {
    nsAutoString scheme;
    src.getScheme(scheme);

    if (scheme == u"https"_ns) {
      if (CheckAllowList(Span(sConnectSrcHttpsAllowList))) {
        return true;
      }
    }

    if (scheme == u"data"_ns || scheme == u"http") {
      if (CheckAllowList(Span(sConnectSrcAddonsAllowList))) {
        return true;
      }
    }

    return AllowBuiltinSrcVisitor::visitSchemeSrc(src);
  }

  bool visitHostSrc(const nsCSPHostSrc& src) override {
    return VisitHostSrcWithWildcardAndHttpsHostAllowLists(
        src, nullptr, sConnectSrcHttpsHostAllowList);
  }
};

class AddonSrcVisitor : public AllowBuiltinSrcVisitor {
 public:
  AddonSrcVisitor(CSPDirective aDirective, nsACString& aURL)
      : AllowBuiltinSrcVisitor(aDirective, aURL) {
    MOZ_ASSERT(aDirective == CSPDirective::DEFAULT_SRC_DIRECTIVE ||
               aDirective == CSPDirective::SCRIPT_SRC_DIRECTIVE);
  }

  bool visitHostSrc(const nsCSPHostSrc& src) override {
    nsAutoString str;
    src.toString(str);
    if (str == u"'self'"_ns) {
      return true;
    }
    return AllowBuiltinSrcVisitor::visitHostSrc(src);
  }

  bool visitHashSrc(const nsCSPHashSrc& src) override {
    if (mDirective == CSPDirective::SCRIPT_SRC_DIRECTIVE) {
      return true;
    }
    return AllowBuiltinSrcVisitor::visitHashSrc(src);
  }
};

#  define CHECK_DIR(DIR, VISITOR)                                           \
    do {                                                                    \
      VISITOR visitor(CSPDirective::DIR, spec);                             \
      /* We don't assert here, because we know that the default fallback is \
       * secure. */                                                         \
      visitor.visit(policy);                                                \
    } while (false)

/* static */
void nsContentSecurityUtils::AssertAboutPageHasCSP(Document* aDocument) {
  // We want to get to a point where all about: pages ship with a CSP. This
  // assertion ensures that we can not deploy new about: pages without a CSP.
  // Please note that any about: page should not use inline JS or inline CSS,
  // and instead should load JS and CSS from an external file (*.js, *.css)
  // which allows us to apply a strong CSP omitting 'unsafe-inline'. Ideally,
  // the CSP allows precisely the resources that need to be loaded; but it
  // should at least be as strong as:
  // <meta http-equiv="Content-Security-Policy" content="default-src chrome:;
  // object-src 'none'"/>

  // This is a data document, created using DOMParser or
  // document.implementation.createDocument() or such, not an about: page which
  // is loaded as a web page.
  if (aDocument->IsLoadedAsData()) {
    return;
  }

  // Check if we should skip the assertion
  if (StaticPrefs::dom_security_skip_about_page_has_csp_assert()) {
    return;
  }

  // Check if we are loading an about: URI at all
  nsCOMPtr<nsIURI> documentURI = aDocument->GetDocumentURI();
  if (!documentURI->SchemeIs("about")) {
    return;
  }

  nsCSPContext* csp = static_cast<nsCSPContext*>(aDocument->GetCsp());
  bool foundDefaultSrc = false;
  uint32_t policyCount = 0;
  if (csp) {
    csp->GetPolicyCount(&policyCount);
    for (uint32_t i = 0; i < policyCount; i++) {
      const nsCSPPolicy* policy = csp->GetPolicy(i);

      foundDefaultSrc =
          policy->hasDirective(CSPDirective::DEFAULT_SRC_DIRECTIVE);
      if (foundDefaultSrc) {
        break;
      }
    }
  }

  // Check if we should skip the allowlist and assert right away. Please note
  // that this pref can and should only be set for automated testing.
  if (StaticPrefs::dom_security_skip_about_page_csp_allowlist_and_assert()) {
    NS_ASSERTION(foundDefaultSrc, "about: page must have a CSP");
    return;
  }

  nsAutoCString spec;
  documentURI->GetSpec(spec);
  ToLowerCase(spec);

  // This allowlist contains about: pages that are permanently allowed to
  // render without a CSP applied.
  static nsLiteralCString sAllowedAboutPagesWithNoCSP[] = {
      // about:blank is a special about page -> no CSP
      "about:blank"_ns,
      // about:srcdoc is a special about page -> no CSP
      "about:srcdoc"_ns,
      // about:sync-log displays plain text only -> no CSP
      "about:sync-log"_ns,
      // about:logo just displays the firefox logo -> no CSP
      "about:logo"_ns,
      // about:sync is a special mozilla-signed developer addon with low usage
      // ->
      // no CSP
      "about:sync"_ns,
#  if defined(ANDROID)
      "about:config"_ns,
#  endif
  };

  for (const nsLiteralCString& allowlistEntry : sAllowedAboutPagesWithNoCSP) {
    // please note that we perform a substring match here on purpose,
    // so we don't have to deal and parse out all the query arguments
    // the various about pages rely on.
    if (StringBeginsWith(spec, allowlistEntry)) {
      return;
    }
  }

  if (aDocument->IsExtensionPage()) {
    // Extensions have two CSP policies applied where the baseline CSP
    // includes 'unsafe-eval' and 'unsafe-inline', hence we only
    // make sure the second CSP is more restrictive.
    //
    // Extension CSPs look quite different to other pages, so for now we just
    // assert some basic security properties.
    MOZ_ASSERT(policyCount == 2,
               "about: page from extension should have two CSP");
    const nsCSPPolicy* policy = csp->GetPolicy(1);

    {
      AddonSrcVisitor visitor(CSPDirective::DEFAULT_SRC_DIRECTIVE, spec);
      if (!visitor.visit(policy)) {
        MOZ_ASSERT(false, "about: page must contain a secure default-src");
      }
    }

    {
      DisallowingVisitor visitor(CSPDirective::OBJECT_SRC_DIRECTIVE, spec);
      if (!visitor.visit(policy)) {
        MOZ_ASSERT(
            false,
            "about: page must contain a secure object-src 'none'; directive");
      }
    }

    CHECK_DIR(SCRIPT_SRC_DIRECTIVE, AddonSrcVisitor);

    nsTArray<nsString> directiveNames;
    policy->getDirectiveNames(directiveNames);
    for (nsString dir : directiveNames) {
      MOZ_ASSERT(!dir.EqualsLiteral("script-src-elem") &&
                 !dir.EqualsLiteral("script-src-attr"));
    }

    return;
  }

  MOZ_ASSERT(policyCount == 1, "about: page should have exactly one CSP");

  const nsCSPPolicy* policy = csp->GetPolicy(0);
  {
    AllowBuiltinSrcVisitor visitor(CSPDirective::DEFAULT_SRC_DIRECTIVE, spec);
    if (!visitor.visit(policy)) {
      MOZ_ASSERT(false, "about: page must contain a secure default-src");
    }
  }

  {
    DisallowingVisitor visitor(CSPDirective::OBJECT_SRC_DIRECTIVE, spec);
    if (!visitor.visit(policy)) {
      MOZ_ASSERT(
          false,
          "about: page must contain a secure object-src 'none'; directive");
    }
  }

  CHECK_DIR(SCRIPT_SRC_DIRECTIVE, AllowBuiltinSrcVisitor);
  CHECK_DIR(STYLE_SRC_DIRECTIVE, StyleSrcVisitor);
  CHECK_DIR(IMG_SRC_DIRECTIVE, ImgSrcVisitor);
  CHECK_DIR(MEDIA_SRC_DIRECTIVE, MediaSrcVisitor);
  CHECK_DIR(CONNECT_SRC_DIRECTIVE, ConnectSrcVisitor);

  // Make sure we have a checker for all the directives that are being used.
  nsTArray<nsString> directiveNames;
  policy->getDirectiveNames(directiveNames);
  for (nsString dir : directiveNames) {
    if (dir.EqualsLiteral("default-src") || dir.EqualsLiteral("object-src") ||
        dir.EqualsLiteral("script-src") || dir.EqualsLiteral("style-src") ||
        dir.EqualsLiteral("img-src") || dir.EqualsLiteral("media-src") ||
        dir.EqualsLiteral("connect-src")) {
      continue;
    }

    NS_WARNING(
        nsPrintfCString(
            "Page %s must not contain a CSP with the unchecked directive %s",
            spec.get(), NS_ConvertUTF16toUTF8(dir).get())
            .get());
    MOZ_ASSERT(false, "Unchecked CSP directive found on internal page.");
  }
}

/* static */
void nsContentSecurityUtils::AssertChromePageHasCSP(Document* aDocument) {
  nsCOMPtr<nsIURI> documentURI = aDocument->GetDocumentURI();
  if (!documentURI->SchemeIs("chrome")) {
    return;
  }

  // We load a lot of SVG images from chrome:.
  if (aDocument->IsBeingUsedAsImage() || aDocument->IsLoadedAsData()) {
    return;
  }

  nsAutoCString spec;
  documentURI->GetSpec(spec);

  nsCOMPtr<nsIContentSecurityPolicy> csp = aDocument->GetCsp();
  uint32_t count = 0;
  if (csp) {
    static_cast<nsCSPContext*>(csp.get())->GetPolicyCount(&count);
  }
  if (count != 0) {
    MOZ_ASSERT(count == 1, "chrome: pages should have exactly one CSP");

    // Both of these have a known weaker policy that differs
    // from all other chrome: pages.
    if (StringBeginsWith(spec, "chrome://browser/content/browser.xhtml"_ns) ||
        StringBeginsWith(spec,
                         "chrome://browser/content/hiddenWindowMac.xhtml"_ns)) {
      return;
    }

    // Thunderbird's CSP does not pass these checks.
#  ifndef MOZ_THUNDERBIRD
    const nsCSPPolicy* policy =
        static_cast<nsCSPContext*>(csp.get())->GetPolicy(0);
    {
      AllowBuiltinSrcVisitor visitor(CSPDirective::DEFAULT_SRC_DIRECTIVE, spec);
      if (!visitor.visit(policy)) {
        MOZ_CRASH_UNSAFE_PRINTF(
            "Document (%s) CSP does not have a default-src!", spec.get());
      }
    }

    CHECK_DIR(SCRIPT_SRC_DIRECTIVE, AllowBuiltinSrcVisitor);
    // If the policy being checked does not have an explicit |script-src-attr|
    // directive, nsCSPPolicy::visitDirectiveSrcs will fallback to using the
    // |script-src| directive, but not default-src.
    // This means we can't use DisallowingVisitor here, because the script-src
    // fallback will usually contain at least a chrome: source.
    // This is not a problem from a security perspective, because inline scripts
    // are not loaded from an URL and thus still disallowed.
    CHECK_DIR(SCRIPT_SRC_ATTR_DIRECTIVE, AllowBuiltinSrcVisitor);
    CHECK_DIR(STYLE_SRC_DIRECTIVE, StyleSrcVisitor);
    CHECK_DIR(IMG_SRC_DIRECTIVE, ImgSrcVisitor);
    CHECK_DIR(MEDIA_SRC_DIRECTIVE, MediaSrcVisitor);
    // For now we don't require chrome: pages to have a `object-src 'none'`
    // directive.
    CHECK_DIR(OBJECT_SRC_DIRECTIVE, DisallowingVisitor);

    nsTArray<nsString> directiveNames;
    policy->getDirectiveNames(directiveNames);
    for (nsString dir : directiveNames) {
      if (dir.EqualsLiteral("default-src") || dir.EqualsLiteral("script-src") ||
          dir.EqualsLiteral("script-src-attr") ||
          dir.EqualsLiteral("style-src") || dir.EqualsLiteral("img-src") ||
          dir.EqualsLiteral("media-src") || dir.EqualsLiteral("object-src")) {
        continue;
      }

      MOZ_CRASH_UNSAFE_PRINTF(
          "Document (%s) must not contain a CSP with the unchecked directive "
          "%s",
          spec.get(), NS_ConvertUTF16toUTF8(dir).get());
    }
#  endif
    return;
  }

  // TODO These are injecting scripts so it cannot be blocked without
  // further coordination.
  if (StringBeginsWith(spec, "chrome://remote/content/marionette/"_ns)) {
    return;
  }

  if (xpc::IsInAutomation()) {
    // Test files
    static nsLiteralCString sAllowedTestPathsWithNoCSP[] = {
        "chrome://mochikit/"_ns,
        "chrome://mochitests/"_ns,
        "chrome://pageloader/content/pageloader.xhtml"_ns,
        "chrome://reftest/"_ns,
    };

    for (const nsLiteralCString& entry : sAllowedTestPathsWithNoCSP) {
      if (StringBeginsWith(spec, entry)) {
        return;
      }
    }
  }

  // CSP for browser.xhtml has been disabled
  if (spec.EqualsLiteral("chrome://browser/content/browser.xhtml") &&
      !StaticPrefs::security_browser_xhtml_csp_enabled()) {
    return;
  }

  MOZ_CRASH_UNSAFE_PRINTF("Document (%s) does not have a CSP!", spec.get());
}

#  undef CHECK_DIR

#endif

/* static */
bool nsContentSecurityUtils::ValidateScriptFilename(JSContext* cx,
                                                    const char* aFilename) {
  // If the pref is permissive, allow everything
  if (StaticPrefs::security_allow_parent_unrestricted_js_loads()) {
    return true;
  }

  // If we're not in the parent process allow everything (presently)
  if (!XRE_IsE10sParentProcess()) {
    return true;
  }

  // If we have allowed eval (because of a user configuration or more
  // likely a test has requested it), and the script is an eval, allow it.
  nsDependentCString filename(aFilename);
  if (StaticPrefs::security_allow_eval_with_system_principal() ||
      StaticPrefs::security_allow_eval_in_parent_process()) {
    if (StringEndsWith(filename, "> eval"_ns)) {
      return true;
    }
  }

  DetectJsHacks();

  if (MOZ_UNLIKELY(!sJSHacksChecked)) {
    MOZ_LOG(
        sCSMLog, LogLevel::Debug,
        ("Allowing a javascript load of %s because "
         "we have not yet been able to determine if JS hacks may be present",
         aFilename));
    return true;
  }

  if (MOZ_UNLIKELY(sJSHacksPresent)) {
    MOZ_LOG(sCSMLog, LogLevel::Debug,
            ("Allowing a javascript load of %s because "
             "some JS hacks may be present",
             aFilename));
    return true;
  }

  if (XRE_IsE10sParentProcess() &&
      !StaticPrefs::extensions_webextensions_remote()) {
    MOZ_LOG(sCSMLog, LogLevel::Debug,
            ("Allowing a javascript load of %s because the web extension "
             "process is disabled.",
             aFilename));
    return true;
  }

  if (StringBeginsWith(filename, "chrome://"_ns)) {
    // If it's a chrome:// url, allow it
    return true;
  }
  if (StringBeginsWith(filename, "resource://"_ns)) {
    // If it's a resource:// url, allow it
    return true;
  }
  if (StringBeginsWith(filename, "moz-src://"_ns)) {
    // If it's a moz-src:// url, allow it
    return true;
  }
  if (StringBeginsWith(filename, "file://"_ns)) {
    // We will temporarily allow all file:// URIs through for now
    return true;
  }
  if (StringBeginsWith(filename, "jar:file://"_ns)) {
    // We will temporarily allow all jar URIs through for now
    return true;
  }
  if (filename.Equals("about:sync-log"_ns)) {
    // about:sync-log runs in the parent process and displays a directory
    // listing. The listing has inline javascript that executes on load.
    return true;
  }

  if (StringBeginsWith(filename, "moz-extension://"_ns)) {
    nsCOMPtr<nsIURI> uri;
    nsresult rv = NS_NewURI(getter_AddRefs(uri), aFilename);
    if (!NS_FAILED(rv) && NS_IsMainThread()) {
      mozilla::extensions::URLInfo url(uri);
      auto* policy =
          ExtensionPolicyService::GetSingleton().GetByHost(url.Host());

      if (policy && policy->IsPrivileged()) {
        MOZ_LOG(sCSMLog, LogLevel::Debug,
                ("Allowing a javascript load of %s because the web extension "
                 "it is associated with is privileged.",
                 aFilename));
        return true;
      }
    }
  } else if (!NS_IsMainThread()) {
    WorkerPrivate* workerPrivate = GetWorkerPrivateFromContext(cx);
    if (workerPrivate && workerPrivate->IsPrivilegedAddonGlobal()) {
      MOZ_LOG(sCSMLog, LogLevel::Debug,
              ("Allowing a javascript load of %s because the web extension "
               "it is associated with is privileged.",
               aFilename));
      return true;
    }
  }

  auto kAllowedFilenamesPrefix = {
      // Until 371900 is fixed, we need to do something about about:downloads
      // and this is the most reasonable. See 1727770
      "about:downloads"_ns,
      // We think this is the same problem as about:downloads
      "about:preferences"_ns, "about:settings"_ns,
      // Browser console will give a filename of 'debugger' See 1763943
      // Sometimes it's 'debugger eager eval code', other times just 'debugger
      // eval code'
      "debugger"_ns};

  for (auto allowedFilenamePrefix : kAllowedFilenamesPrefix) {
    if (StringBeginsWith(filename, allowedFilenamePrefix)) {
      return true;
    }
  }

  // Log to MOZ_LOG
  MOZ_LOG(sCSMLog, LogLevel::Error,
          ("ValidateScriptFilename Failed: %s\n", aFilename));

  FilenameTypeAndDetails fileNameTypeAndDetails =
      FilenameToFilenameType(filename, true);

  glean::security::JavascriptLoadParentProcessExtra extra = {
      .fileinfo = fileNameTypeAndDetails.second,
      .value = Some(fileNameTypeAndDetails.first),
  };
  glean::security::javascript_load_parent_process.Record(Some(extra));

#if defined(DEBUG) || defined(FUZZING)
  auto crashString = nsContentSecurityUtils::SmartFormatCrashString(
      aFilename,
      fileNameTypeAndDetails.second.isSome()
          ? fileNameTypeAndDetails.second.value().get()
          : "(None)",
      "Blocking a script load %s from file %s");
  MOZ_CRASH_UNSAFE_PRINTF("%s", crashString.get());
#elif defined(EARLY_BETA_OR_EARLIER)
  // Cause a crash (if we've never crashed before and we can ensure we won't do
  // it again.)
  // The details in the second arg, passed to UNSAFE_PRINTF, are also included
  // in Event Telemetry and have received data review.
  if (fileNameTypeAndDetails.second.isSome()) {
    PossiblyCrash("js_load_1", aFilename,
                  fileNameTypeAndDetails.second.value());
  } else {
    PossiblyCrash("js_load_1", aFilename, "(None)"_ns);
  }
#endif

  // Presently we are only enforcing restrictions for the script filename
  // on Nightly.  On all channels we are reporting Telemetry. In the future we
  // will assert in debug builds and return false to prevent execution in
  // non-debug builds.
#ifdef NIGHTLY_BUILD
  return false;
#else
  return true;
#endif
}

/* static */
void nsContentSecurityUtils::LogMessageToConsole(nsIHttpChannel* aChannel,
                                                 const char* aMsg) {
  nsCOMPtr<nsIURI> uri;
  nsresult rv = aChannel->GetURI(getter_AddRefs(uri));
  if (NS_FAILED(rv)) {
    return;
  }

  uint64_t windowID = 0;
  rv = aChannel->GetTopLevelContentWindowId(&windowID);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return;
  }
  if (!windowID) {
    nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
    loadInfo->GetInnerWindowID(&windowID);
  }

  nsAutoString localizedMsg;
  nsAutoCString spec;
  uri->GetSpec(spec);
  AutoTArray<nsString, 1> params = {NS_ConvertUTF8toUTF16(spec)};
  rv = nsContentUtils::FormatLocalizedString(
      nsContentUtils::eSECURITY_PROPERTIES, aMsg, params, localizedMsg);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return;
  }

  nsContentUtils::ReportToConsoleByWindowID(
      localizedMsg, nsIScriptError::warningFlag, "Security"_ns, windowID,
      SourceLocation{uri.get()});
}

/* static */
long nsContentSecurityUtils::ClassifyDownload(
    nsIChannel* aChannel, const nsAutoCString& aMimeTypeGuess) {
  MOZ_ASSERT(aChannel, "IsDownloadAllowed without channel?");

  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();

  nsCOMPtr<nsIURI> contentLocation;
  aChannel->GetURI(getter_AddRefs(contentLocation));

  nsCOMPtr<nsIPrincipal> loadingPrincipal = loadInfo->GetLoadingPrincipal();
  if (!loadingPrincipal) {
    loadingPrincipal = loadInfo->TriggeringPrincipal();
  }
  // Creating a fake Loadinfo that is just used for the MCB check.
  nsCOMPtr<nsILoadInfo> secCheckLoadInfo = new mozilla::net::LoadInfo(
      loadingPrincipal, loadInfo->TriggeringPrincipal(), nullptr,
      nsILoadInfo::SEC_ONLY_FOR_EXPLICIT_CONTENTSEC_CHECK,
      nsIContentPolicy::TYPE_FETCH);
  // Disable HTTPS-Only checks for that loadinfo. This is required because
  // otherwise nsMixedContentBlocker::ShouldLoad would assume that the request
  // is safe, because HTTPS-Only is handling it.
  secCheckLoadInfo->SetHttpsOnlyStatus(nsILoadInfo::HTTPS_ONLY_EXEMPT);

  int16_t decission = nsIContentPolicy::ACCEPT;
  nsMixedContentBlocker::ShouldLoad(false,  //  aHadInsecureImageRedirect
                                    contentLocation,   //  aContentLocation,
                                    secCheckLoadInfo,  //  aLoadinfo
                                    false,             //  aReportError
                                    &decission         // aDecision
  );

  if (StaticPrefs::dom_block_download_insecure() &&
      decission != nsIContentPolicy::ACCEPT) {
    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(aChannel);
    if (httpChannel) {
      LogMessageToConsole(httpChannel, "MixedContentBlockedDownload");
    }
    return nsITransfer::DOWNLOAD_POTENTIALLY_UNSAFE;
  }

  if (loadInfo->TriggeringPrincipal()->IsSystemPrincipal()) {
    return nsITransfer::DOWNLOAD_ACCEPTABLE;
  }

  uint32_t triggeringFlags = loadInfo->GetTriggeringSandboxFlags();
  uint32_t currentflags = loadInfo->GetSandboxFlags();

  if ((triggeringFlags & SANDBOXED_ALLOW_DOWNLOADS) ||
      (currentflags & SANDBOXED_ALLOW_DOWNLOADS)) {
    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(aChannel);
    if (httpChannel) {
      LogMessageToConsole(httpChannel, "IframeSandboxBlockedDownload");
    }
    return nsITransfer::DOWNLOAD_FORBIDDEN;
  }
  return nsITransfer::DOWNLOAD_ACCEPTABLE;
}
