/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A namespace class for static content security utilities. */

#ifndef nsContentSecurityUtils_h___
#define nsContentSecurityUtils_h___

#include <utility>
#include "mozilla/Maybe.h"
#include "nsStringFwd.h"

struct JSContext;
class nsIChannel;
class nsIHttpChannel;
class nsIPrincipal;
class NS_ConvertUTF8toUTF16;

namespace mozilla::dom {
class Document;
class Element;
}  // namespace mozilla::dom

using FilenameTypeAndDetails = std::pair<nsCString, mozilla::Maybe<nsCString>>;

class nsContentSecurityUtils {
 public:
  // CSPs upgrade-insecure-requests directive applies to same origin top level
  // navigations. Using the SOP would return false for the case when an https
  // page triggers and http page to load, even though that http page would be
  // upgraded to https later. Hence we have to use that custom function instead
  // of simply calling aTriggeringPrincipal->Equals(aResultPrincipal).
  static bool IsConsideredSameOriginForURI(nsIPrincipal* aTriggeringPrincipal,
                                           nsIPrincipal* aResultPrincipal);

  static bool IsEvalAllowed(JSContext* cx, bool aIsSystemPrincipal,
                            const nsAString& aScript);
  static void NotifyEvalUsage(bool aIsSystemPrincipal,
                              const nsACString& aFileName, uint64_t aWindowID,
                              uint32_t aLineNumber, uint32_t aColumnNumber);

  // Helper function for various checks:
  // This function detects profiles with userChrome.js or extension signatures
  // disabled. We can't/won't enforce strong security for people with those
  // hacks. The function will cache its result.
  static void DetectJsHacks();
  // Helper function for detecting custom agent styles
  static void DetectCssHacks();

  // Helper function to query the HTTP Channel of a potential
  // multi-part channel. Mostly used for querying response headers
  static nsresult GetHttpChannelFromPotentialMultiPart(
      nsIChannel* aChannel, nsIHttpChannel** aHttpChannel);

  // Helper function which performs the following framing checks
  // * CSP frame-ancestors
  // * x-frame-options
  // If any of the two disallows framing, the channel will be cancelled.
  static void PerformCSPFrameAncestorAndXFOCheck(nsIChannel* aChannel);

  // Helper function which just checks if the channel violates any:
  // 1. CSP frame-ancestors properties
  // 2. x-frame-options
  static bool CheckCSPFrameAncestorAndXFO(nsIChannel* aChannel);

  // Implements https://w3c.github.io/webappsec-csp/#is-element-nonceable.
  //
  // Returns an empty nonce for elements without a nonce OR when a potential
  // dangling markup attack was detected.
  static nsString GetIsElementNonceableNonce(
      const mozilla::dom::Element& aElement);

  // Helper function to Check if a Download is allowed;
  static long ClassifyDownload(nsIChannel* aChannel,
                               const nsAutoCString& aMimeTypeGuess);

  // Public only for testing
  static FilenameTypeAndDetails FilenameToFilenameType(
      const nsACString& fileName, bool collectAdditionalExtensionData);
  static char* SmartFormatCrashString(const char* str);
  static char* SmartFormatCrashString(char* str);
  static nsCString SmartFormatCrashString(const char* part1, const char* part2,
                                          const char* format_string);
  static nsCString SmartFormatCrashString(char* part1, char* part2,
                                          const char* format_string);

#if defined(DEBUG)
  static void AssertAboutPageHasCSP(mozilla::dom::Document* aDocument);
  static void AssertChromePageHasCSP(mozilla::dom::Document* aDocument);
#endif

  static bool ValidateScriptFilename(JSContext* cx, const char* aFilename);
  // Helper Function to Post a message to the corresponding JS-Console
  static void LogMessageToConsole(nsIHttpChannel* aChannel, const char* aMsg);
};

#endif /* nsContentSecurityUtils_h___ */
