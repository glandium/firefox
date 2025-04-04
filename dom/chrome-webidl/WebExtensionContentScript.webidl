/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

interface LoadInfo;
interface URI;
interface WindowProxy;

typedef (MatchPatternSet or sequence<DOMString>) MatchPatternSetOrStringSequence;
typedef (MatchGlob or UTF8String) MatchGlobOrString;

[ChromeOnly, Exposed=Window]
interface MozDocumentMatcher {
  [Throws]
  constructor(MozDocumentMatcherInit options);

  /**
   * Returns true if the script's match and exclude patterns match the given
   * URI, without reference to attributes such as `allFrames`.
   */
  boolean matchesURI(URI uri);

  /**
   * Returns true if the given window matches. This should be used to
   * determine whether to run a script in a window at load time. Use
   * ignorePermissions to match without origin permissions in MV3.
   */
  boolean matchesWindowGlobal(WindowGlobalChild windowGlobal,
                              optional boolean ignorePermissions = false);

  /**
   * If true, match all frames. If false, match only top-level frames.
   */
  [Constant]
  readonly attribute boolean allFrames;

  /**
   * If we can't check extension has permissions to access the URI upfront,
   * set the flag to perform the origin check at runtime, upon matching.
   * This is always true in MV3, where host permissions are optional.
   */
  [Constant]
  readonly attribute boolean checkPermissions;

  /**
   * If true, this causes us to match about:blank and about:srcdoc documents by
   * the URL of the inherit principal, usually the initial URL of the document
   * that triggered the navigation.
   * If false, we only match frames with an explicit matching URL.
   */
  [Constant]
  readonly attribute boolean matchAboutBlank;

  /**
   * If true, this causes us to match documents with opaque URLs (such as
   * about:blank, about:srcdoc, data:, blob:, and sandboxed versions thereof)
   * by the document principal's origin URI. In case of null principals, their
   * precursor is used for matching.
   * When true, matchAboutBlank is implicitly true.
   * If false, we only match frames with an explicit matching URL.
   */
  [Constant]
  readonly attribute boolean matchOriginAsFallback;

  /**
   * The outer window ID of the frame in which to run the script, or 0 if it
   * should run in the top-level frame. Should only be used for
   * dynamically-injected scripts.
   */
  [Constant]
  readonly attribute unsigned long long? frameID;

  /**
   * The set of match patterns for URIs of pages in which this script should
   * run. This attribute is mandatory, and is a prerequisite for all other
   * match patterns.
   */
  [Constant]
  readonly attribute MatchPatternSet matches;

  /**
   * A set of match patterns for URLs in which this script should not run,
   * even if they match other include patterns or globs.
   */
  [Constant]
  readonly attribute MatchPatternSet? excludeMatches;

  /**
   * Whether the matcher is used by the MV3 userScripts API.
   * If true, URLs are matched when they match "matches" OR "includeGlobs",
   * instead of the usual AND.
   */
  [Constant]
  readonly attribute boolean isUserScript;

  /**
   * The originAttributesPattern for which this script should be enabled for.
   */
  [Constant, Throws]
  readonly attribute any originAttributesPatterns;

  /**
   * The policy object for the extension that this matcher belongs to.
   */
  [Constant]
  readonly attribute WebExtensionPolicy? extension;
};

dictionary MozDocumentMatcherInit {
  boolean allFrames = false;

  boolean checkPermissions = false;

  sequence<OriginAttributesPatternDictionary>? originAttributesPatterns = null;

  boolean matchAboutBlank = false;

  boolean matchOriginAsFallback = false;

  unsigned long long? frameID = null;

  required MatchPatternSetOrStringSequence matches;

  MatchPatternSetOrStringSequence? excludeMatches = null;

  sequence<MatchGlobOrString>? includeGlobs = null;

  sequence<MatchGlobOrString>? excludeGlobs = null;

  boolean isUserScript = false;

  boolean hasActiveTabPermission = false;
};

/**
 * Describes the earliest point in the load cycle at which a script should
 * run.
 */
enum ContentScriptRunAt {
  /**
   * The point in the load cycle just after the document element has been
   * inserted, before any page scripts have been allowed to run.
   */
  "document_start",
  /**
   * The point after which the page DOM has fully loaded, but before all page
   * resources have necessarily been loaded. Corresponds approximately to the
   * DOMContentLoaded event.
   */
  "document_end",
  /**
   * The first point after the page and all of its resources has fully loaded
   * when the event loop is idle, and can run scripts without delaying a paint
   * event.
   */
  "document_idle",
};

/**
 * Describes the world where a script should run.
 */
enum ContentScriptExecutionWorld {
  /**
   * The default execution environment of content scripts.
   * The name refers to "isolated world", which is a concept from Chromium and
   * WebKit, used to enforce isolation of the JavaScript execution environments
   * of content scripts and web pages.
   *
   * Not supported when isUserScript=true.
   */
  "ISOLATED",
  /**
   * The execution environment of the web page.
   */
  "MAIN",
  /**
   * The execution environment of a sandbox running scripts registered through
   * the MV3 userScripts API.
   *
   * Only supported when isUserScript=true.
   */
  "USER_SCRIPT",
};

[ChromeOnly, Exposed=Window]
interface WebExtensionContentScript : MozDocumentMatcher {
  [Throws]
  constructor(WebExtensionPolicy extension,
              WebExtensionContentScriptInit options);

  /**
   * The earliest point in the load cycle at which this script should run. For
   * static content scripts, in extensions which were present at browser
   * startup, the browser makes every effort to make sure that the script runs
   * no later than this point in the load cycle. For dynamic content scripts,
   * and scripts from extensions installed during this session, the scripts
   * may run at a later point.
   */
  [Constant]
  readonly attribute ContentScriptRunAt runAt;

  /**
   * The world where the script should run.
   */
  [Constant]
  readonly attribute ContentScriptExecutionWorld world;

  /**
   * When world is "USER_SCRIPT", worldId can be used to specify a specific
   * extension-specific sandbox to execute in, instead of the default one.
   */
  [Constant]
  readonly attribute DOMString? worldId;

  /**
   * A set of paths, relative to the extension root, of CSS sheets to inject
   * into matching pages.
   */
  [Cached, Constant, Frozen]
  readonly attribute sequence<DOMString> cssPaths;

  /**
   * A set of paths, relative to the extension root, of JavaScript scripts to
   * execute in matching pages.
   */
  [Cached, Constant, Frozen]
  readonly attribute sequence<DOMString> jsPaths;
};

dictionary WebExtensionContentScriptInit : MozDocumentMatcherInit {
  ContentScriptRunAt runAt = "document_idle";

  ContentScriptExecutionWorld world = "ISOLATED";

  DOMString? worldId = null;

  sequence<DOMString> cssPaths = [];

  sequence<DOMString> jsPaths = [];
};
