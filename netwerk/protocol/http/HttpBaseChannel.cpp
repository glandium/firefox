/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "mozilla/net/HttpBaseChannel.h"

#include <algorithm>
#include <utility>

#include "HttpBaseChannel.h"
#include "HttpLog.h"
#include "LoadInfo.h"
#include "ReferrerInfo.h"
#include "mozIRemoteLazyInputStream.h"
#include "mozIThirdPartyUtil.h"
#include "mozilla/LoadInfo.h"
#include "mozilla/AntiTrackingUtils.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/BinarySearch.h"
#include "mozilla/ConsoleReportCollector.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/InputStreamLengthHelper.h"
#include "mozilla/Mutex.h"
#include "mozilla/NullPrincipal.h"
#include "mozilla/PermissionManager.h"
#include "mozilla/Components.h"
#include "mozilla/StaticPrefs_browser.h"
#include "mozilla/StaticPrefs_fission.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/StaticPrefs_security.h"
#include "mozilla/glean/NetwerkProtocolHttpMetrics.h"
#include "mozilla/Telemetry.h"
#include "mozilla/Tokenizer.h"
#include "mozilla/browser/NimbusFeatures.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/FetchPriority.h"
#include "mozilla/dom/nsHTTPSOnlyUtils.h"
#include "mozilla/dom/nsMixedContentBlocker.h"
#include "mozilla/dom/Performance.h"
#include "mozilla/dom/PerformanceStorage.h"
#include "mozilla/dom/ProcessIsolation.h"
#include "mozilla/dom/RequestBinding.h"
#include "mozilla/dom/WindowGlobalParent.h"
#include "mozilla/net/OpaqueResponseUtils.h"
#include "mozilla/net/UrlClassifierCommon.h"
#include "mozilla/net/UrlClassifierFeatureFactory.h"
#include "nsBufferedStreams.h"
#include "nsCOMPtr.h"
#include "nsCRT.h"
#include "nsContentSecurityManager.h"
#include "nsContentSecurityUtils.h"
#include "nsContentUtils.h"
#include "nsDebug.h"
#include "nsEscape.h"
#include "nsGlobalWindowInner.h"
#include "nsGlobalWindowOuter.h"
#include "nsHttpChannel.h"
#include "nsHTTPCompressConv.h"
#include "nsHttpHandler.h"
#include "nsICacheInfoChannel.h"
#include "nsICachingChannel.h"
#include "nsIChannelEventSink.h"
#include "nsIConsoleService.h"
#include "nsIContentPolicy.h"
#include "nsICookieService.h"
#include "nsIDOMWindowUtils.h"
#include "nsIDocShell.h"
#include "nsIDNSService.h"
#include "nsIEncodedChannel.h"
#include "nsIHttpHeaderVisitor.h"
#include "nsILoadGroupChild.h"
#include "nsIMIMEInputStream.h"
#include "nsIMultiplexInputStream.h"
#include "nsIMutableArray.h"
#include "nsINetworkInterceptController.h"
#include "nsIObserverService.h"
#include "nsIPrincipal.h"
#include "nsIProtocolProxyService.h"
#include "nsIScriptError.h"
#include "nsIScriptSecurityManager.h"
#include "nsISecurityConsoleMessage.h"
#include "nsISeekableStream.h"
#include "nsIStorageStream.h"
#include "nsIStreamConverterService.h"
#include "nsITimedChannel.h"
#include "nsITransportSecurityInfo.h"
#include "nsIURIMutator.h"
#include "nsMimeTypes.h"
#include "nsNetCID.h"
#include "nsNetUtil.h"
#include "nsPIDOMWindow.h"
#include "nsProxyRelease.h"
#include "nsReadableUtils.h"
#include "nsRedirectHistoryEntry.h"
#include "nsServerTiming.h"
#include "nsStreamListenerWrapper.h"
#include "nsStreamUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsURLHelper.h"
#include "mozilla/RemoteLazyInputStreamChild.h"
#include "mozilla/net/SFVService.h"
#include "mozilla/dom/ContentChild.h"
#include "nsQueryObject.h"

using mozilla::dom::RequestMode;

#define LOGORB(msg, ...)                \
  MOZ_LOG(GetORBLog(), LogLevel::Debug, \
          ("%s: %p " msg, __func__, this, ##__VA_ARGS__))

namespace mozilla {
namespace net {

static bool IsHeaderBlacklistedForRedirectCopy(nsHttpAtom const& aHeader) {
  // IMPORTANT: keep this list ASCII-code sorted
  static nsHttpAtomLiteral const* blackList[] = {
      &nsHttp::Accept,
      &nsHttp::Accept_Encoding,
      &nsHttp::Accept_Language,
      &nsHttp::Alternate_Service_Used,
      &nsHttp::Authentication,
      &nsHttp::Authorization,
      &nsHttp::Connection,
      &nsHttp::Content_Length,
      &nsHttp::Cookie,
      &nsHttp::Host,
      &nsHttp::If,
      &nsHttp::If_Match,
      &nsHttp::If_Modified_Since,
      &nsHttp::If_None_Match,
      &nsHttp::If_None_Match_Any,
      &nsHttp::If_Range,
      &nsHttp::If_Unmodified_Since,
      &nsHttp::Proxy_Authenticate,
      &nsHttp::Proxy_Authorization,
      &nsHttp::Range,
      &nsHttp::TE,
      &nsHttp::Transfer_Encoding,
      &nsHttp::Upgrade,
      &nsHttp::User_Agent,
      &nsHttp::WWW_Authenticate};

  class HttpAtomComparator {
    nsHttpAtom const& mTarget;

   public:
    explicit HttpAtomComparator(nsHttpAtom const& aTarget) : mTarget(aTarget) {}
    int operator()(nsHttpAtom const* aVal) const {
      if (mTarget == *aVal) {
        return 0;
      }
      return strcmp(mTarget.get(), aVal->get());
    }
    int operator()(nsHttpAtomLiteral const* aVal) const {
      if (mTarget == *aVal) {
        return 0;
      }
      return strcmp(mTarget.get(), aVal->get());
    }
  };

  size_t unused;
  return BinarySearchIf(blackList, 0, std::size(blackList),
                        HttpAtomComparator(aHeader), &unused);
}

class AddHeadersToChannelVisitor final : public nsIHttpHeaderVisitor {
 public:
  NS_DECL_ISUPPORTS

  explicit AddHeadersToChannelVisitor(nsIHttpChannel* aChannel)
      : mChannel(aChannel) {}

  NS_IMETHOD VisitHeader(const nsACString& aHeader,
                         const nsACString& aValue) override {
    nsHttpAtom atom = nsHttp::ResolveAtom(aHeader);
    if (!IsHeaderBlacklistedForRedirectCopy(atom)) {
      DebugOnly<nsresult> rv =
          mChannel->SetRequestHeader(aHeader, aValue, false);
      MOZ_ASSERT(NS_SUCCEEDED(rv));
    }
    return NS_OK;
  }

 private:
  ~AddHeadersToChannelVisitor() = default;

  nsCOMPtr<nsIHttpChannel> mChannel;
};

NS_IMPL_ISUPPORTS(AddHeadersToChannelVisitor, nsIHttpHeaderVisitor)

static OpaqueResponseFilterFetch ConfiguredFilterFetchResponseBehaviour() {
  uint32_t pref = StaticPrefs::
      browser_opaqueResponseBlocking_filterFetchResponse_DoNotUseDirectly();
  if (NS_WARN_IF(pref >
                 static_cast<uint32_t>(OpaqueResponseFilterFetch::All))) {
    return OpaqueResponseFilterFetch::All;
  }

  return static_cast<OpaqueResponseFilterFetch>(pref);
}

HttpBaseChannel::HttpBaseChannel()
    : mReportCollector(new ConsoleReportCollector()),
      mHttpHandler(gHttpHandler),
      mChannelCreationTime(0),
      mComputedCrossOriginOpenerPolicy(nsILoadInfo::OPENER_POLICY_UNSAFE_NONE),
      mStartPos(UINT64_MAX),
      mTransferSize(0),
      mRequestSize(0),
      mDecodedBodySize(0),
      mSupportsHTTP3(false),
      mEncodedBodySize(0),
      mRequestContextID(0),
      mContentWindowId(0),
      mBrowserId(0),
      mAltDataLength(-1),
      mChannelId(0),
      mReqContentLength(0U),
      mStatus(NS_OK),
      mCanceled(false),
      mFirstPartyClassificationFlags(0),
      mThirdPartyClassificationFlags(0),
      mLoadFlags(LOAD_NORMAL),
      mCaps(0),
      mClassOfService(0, false),
      mTlsFlags(0),
      mSuspendCount(0),
      mInitialRwin(0),
      mProxyResolveFlags(0),
      mContentDispositionHint(UINT32_MAX),
      mRequestMode(RequestMode::No_cors),
      mRedirectMode(nsIHttpChannelInternal::REDIRECT_MODE_FOLLOW),
      mLastRedirectFlags(0),
      mPriority(PRIORITY_NORMAL),
      mRedirectionLimit(gHttpHandler->RedirectionLimit()),
      mRedirectCount(0),
      mInternalRedirectCount(0),
      mCachedOpaqueResponseBlockingPref(
          StaticPrefs::browser_opaqueResponseBlocking()),
      mChannelBlockedByOpaqueResponse(false),
      mDummyChannelForCachedResource(false),
      mHasContentDecompressed(false),
      mRenderBlocking(false) {
  StoreApplyConversion(true);
  StoreAllowSTS(true);
  StoreTracingEnabled(true);
  StoreReportTiming(true);
  StoreAllowSpdy(true);
  StoreAllowHttp3(true);
  StoreAllowAltSvc(true);
  StoreResponseTimeoutEnabled(true);
  StoreAllRedirectsSameOrigin(true);
  StoreAllRedirectsPassTimingAllowCheck(true);
  StoreUpgradableToSecure(true);
  StoreIsUserAgentHeaderModified(false);

  this->mSelfAddr.inet = {};
  this->mPeerAddr.inet = {};
  LOG(("Creating HttpBaseChannel @%p\n", this));

  // Subfields of unions cannot be targeted in an initializer list.
#ifdef MOZ_VALGRIND
  // Zero the entire unions so that Valgrind doesn't complain when we send them
  // to another process.
  memset(&mSelfAddr, 0, sizeof(NetAddr));
  memset(&mPeerAddr, 0, sizeof(NetAddr));
#endif
  mSelfAddr.raw.family = PR_AF_UNSPEC;
  mPeerAddr.raw.family = PR_AF_UNSPEC;
}

HttpBaseChannel::~HttpBaseChannel() {
  LOG(("Destroying HttpBaseChannel @%p\n", this));

  // Make sure we don't leak
  CleanRedirectCacheChainIfNecessary();

  ReleaseMainThreadOnlyReferences();
}

namespace {  // anon

class NonTailRemover : public nsISupports {
  NS_DECL_THREADSAFE_ISUPPORTS

  explicit NonTailRemover(nsIRequestContext* rc) : mRequestContext(rc) {}

 private:
  virtual ~NonTailRemover() {
    MOZ_ASSERT(NS_IsMainThread());
    mRequestContext->RemoveNonTailRequest();
  }

  nsCOMPtr<nsIRequestContext> mRequestContext;
};

NS_IMPL_ISUPPORTS0(NonTailRemover)

}  // namespace

void HttpBaseChannel::ReleaseMainThreadOnlyReferences() {
  if (NS_IsMainThread()) {
    // Already on main thread, let dtor to
    // take care of releasing references
    RemoveAsNonTailRequest();
    return;
  }

  nsTArray<nsCOMPtr<nsISupports>> arrayToRelease;
  arrayToRelease.AppendElement(mLoadGroup.forget());
  arrayToRelease.AppendElement(mLoadInfo.forget());
  arrayToRelease.AppendElement(mCallbacks.forget());
  arrayToRelease.AppendElement(mProgressSink.forget());
  arrayToRelease.AppendElement(mPrincipal.forget());
  arrayToRelease.AppendElement(mListener.forget());
  arrayToRelease.AppendElement(mCompressListener.forget());
  arrayToRelease.AppendElement(mORB.forget());

  if (LoadAddedAsNonTailRequest()) {
    // RemoveNonTailRequest() on our request context must be called on the main
    // thread
    MOZ_RELEASE_ASSERT(mRequestContext,
                       "Someone released rc or set flags w/o having it?");

    nsCOMPtr<nsISupports> nonTailRemover(new NonTailRemover(mRequestContext));
    arrayToRelease.AppendElement(nonTailRemover.forget());
  }

  NS_DispatchToMainThread(new ProxyReleaseRunnable(std::move(arrayToRelease)));
}

void HttpBaseChannel::AddClassificationFlags(uint32_t aClassificationFlags,
                                             bool aIsThirdParty) {
  LOG(
      ("HttpBaseChannel::AddClassificationFlags classificationFlags=%d "
       "thirdparty=%d %p",
       aClassificationFlags, static_cast<int>(aIsThirdParty), this));

  if (aIsThirdParty) {
    mThirdPartyClassificationFlags |= aClassificationFlags;
  } else {
    mFirstPartyClassificationFlags |= aClassificationFlags;
  }
}

static bool isSecureOrTrustworthyURL(nsIURI* aURI) {
  return aURI->SchemeIs("https") ||
         (StaticPrefs::network_http_encoding_trustworthy_is_https() &&
          nsMixedContentBlocker::IsPotentiallyTrustworthyLoopbackURL(aURI));
}

nsresult HttpBaseChannel::Init(nsIURI* aURI, uint32_t aCaps,
                               nsProxyInfo* aProxyInfo,
                               uint32_t aProxyResolveFlags, nsIURI* aProxyURI,
                               uint64_t aChannelId,
                               ExtContentPolicyType aContentPolicyType,
                               nsILoadInfo* aLoadInfo) {
  LOG1(("HttpBaseChannel::Init [this=%p]\n", this));

  MOZ_ASSERT(aURI, "null uri");

  mURI = aURI;
  mOriginalURI = aURI;
  mDocumentURI = nullptr;
  mCaps = aCaps;
  mProxyResolveFlags = aProxyResolveFlags;
  mProxyURI = aProxyURI;
  mChannelId = aChannelId;
  mLoadInfo = aLoadInfo;

  // Construct connection info object
  nsAutoCString host;
  int32_t port = -1;
  bool isHTTPS = isSecureOrTrustworthyURL(mURI);

  nsresult rv = mURI->GetAsciiHost(host);
  if (NS_FAILED(rv)) return rv;

  // Reject the URL if it doesn't specify a host
  if (host.IsEmpty()) return NS_ERROR_MALFORMED_URI;

  rv = mURI->GetPort(&port);
  if (NS_FAILED(rv)) return rv;

  LOG1(("host=%s port=%d\n", host.get(), port));

  rv = mURI->GetAsciiSpec(mSpec);
  if (NS_FAILED(rv)) return rv;
  LOG1(("uri=%s\n", mSpec.get()));

  // Assert default request method
  MOZ_ASSERT(mRequestHead.EqualsMethod(nsHttpRequestHead::kMethod_Get));

  // Set request headers
  nsAutoCString hostLine;
  rv = nsHttpHandler::GenerateHostPort(host, port, hostLine);
  if (NS_FAILED(rv)) return rv;

  rv = mRequestHead.SetHeader(nsHttp::Host, hostLine);
  if (NS_FAILED(rv)) return rv;

  rv = gHttpHandler->AddStandardRequestHeaders(
      &mRequestHead, isHTTPS, aContentPolicyType,
      nsContentUtils::ShouldResistFingerprinting(this,
                                                 RFPTarget::HttpUserAgent));
  if (NS_FAILED(rv)) return rv;

  nsAutoCString type;
  if (aProxyInfo && NS_SUCCEEDED(aProxyInfo->GetType(type)) &&
      !type.EqualsLiteral("unknown")) {
    mProxyInfo = aProxyInfo;
  }

  mCurrentThread = GetCurrentSerialEventTarget();
  return rv;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsISupports
//-----------------------------------------------------------------------------

NS_IMPL_ADDREF(HttpBaseChannel)
NS_IMPL_RELEASE(HttpBaseChannel)

NS_INTERFACE_MAP_BEGIN(HttpBaseChannel)
  NS_INTERFACE_MAP_ENTRY(nsIRequest)
  NS_INTERFACE_MAP_ENTRY(nsIChannel)
  NS_INTERFACE_MAP_ENTRY(nsIIdentChannel)
  NS_INTERFACE_MAP_ENTRY(nsIEncodedChannel)
  NS_INTERFACE_MAP_ENTRY(nsIHttpChannel)
  NS_INTERFACE_MAP_ENTRY(nsIHttpChannelInternal)
  NS_INTERFACE_MAP_ENTRY(nsIForcePendingChannel)
  NS_INTERFACE_MAP_ENTRY(nsIUploadChannel)
  NS_INTERFACE_MAP_ENTRY(nsIFormPOSTActionChannel)
  NS_INTERFACE_MAP_ENTRY(nsIUploadChannel2)
  NS_INTERFACE_MAP_ENTRY(nsISupportsPriority)
  NS_INTERFACE_MAP_ENTRY(nsITraceableChannel)
  NS_INTERFACE_MAP_ENTRY(nsIPrivateBrowsingChannel)
  NS_INTERFACE_MAP_ENTRY(nsITimedChannel)
  NS_INTERFACE_MAP_ENTRY(nsIConsoleReportCollector)
  NS_INTERFACE_MAP_ENTRY(nsIThrottledInputChannel)
  NS_INTERFACE_MAP_ENTRY(nsIClassifiedChannel)
  NS_INTERFACE_MAP_ENTRY_CONCRETE(HttpBaseChannel)
NS_INTERFACE_MAP_END_INHERITING(nsHashPropertyBag)

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIRequest
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::GetName(nsACString& aName) {
  aName = mSpec;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::IsPending(bool* aIsPending) {
  NS_ENSURE_ARG_POINTER(aIsPending);
  *aIsPending = LoadIsPending() || LoadForcePending();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetStatus(nsresult* aStatus) {
  NS_ENSURE_ARG_POINTER(aStatus);
  *aStatus = mStatus;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLoadGroup(nsILoadGroup** aLoadGroup) {
  NS_ENSURE_ARG_POINTER(aLoadGroup);
  *aLoadGroup = do_AddRef(mLoadGroup).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetLoadGroup(nsILoadGroup* aLoadGroup) {
  MOZ_ASSERT(NS_IsMainThread(), "Should only be called on the main thread.");

  if (!CanSetLoadGroup(aLoadGroup)) {
    return NS_ERROR_FAILURE;
  }

  mLoadGroup = aLoadGroup;
  mProgressSink = nullptr;
  UpdatePrivateBrowsing();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLoadFlags(nsLoadFlags* aLoadFlags) {
  NS_ENSURE_ARG_POINTER(aLoadFlags);
  *aLoadFlags = mLoadFlags;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetLoadFlags(nsLoadFlags aLoadFlags) {
  mLoadFlags = aLoadFlags;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetTRRMode(nsIRequest::TRRMode* aTRRMode) {
  if (!LoadIsOCSP()) {
    return GetTRRModeImpl(aTRRMode);
  }

  nsCOMPtr<nsIDNSService> dns = do_GetService(NS_DNSSERVICE_CONTRACTID);
  nsIDNSService::ResolverMode trrMode = nsIDNSService::MODE_NATIVEONLY;
  // If this is an OCSP channel, and the global TRR mode is TRR_ONLY (3)
  // then we set the mode for this channel as TRR_DISABLED_MODE.
  // We do this to prevent a TRR service channel's OCSP validation from
  // blocking DNS resolution completely.
  if (dns && NS_SUCCEEDED(dns->GetCurrentTrrMode(&trrMode)) &&
      trrMode == nsIDNSService::MODE_TRRONLY) {
    *aTRRMode = nsIRequest::TRR_DISABLED_MODE;
    return NS_OK;
  }

  return GetTRRModeImpl(aTRRMode);
}

NS_IMETHODIMP
HttpBaseChannel::SetTRRMode(nsIRequest::TRRMode aTRRMode) {
  return SetTRRModeImpl(aTRRMode);
}

NS_IMETHODIMP
HttpBaseChannel::SetDocshellUserAgentOverride() {
  RefPtr<dom::BrowsingContext> bc;
  MOZ_ALWAYS_SUCCEEDS(mLoadInfo->GetBrowsingContext(getter_AddRefs(bc)));
  if (!bc) {
    return NS_OK;
  }

  nsAutoString customUserAgent;
  bc->GetCustomUserAgent(customUserAgent);
  if (customUserAgent.IsEmpty() || customUserAgent.IsVoid()) {
    return NS_OK;
  }

  NS_ConvertUTF16toUTF8 utf8CustomUserAgent(customUserAgent);
  nsresult rv = SetRequestHeaderInternal(
      "User-Agent"_ns, utf8CustomUserAgent, false,
      nsHttpHeaderArray::eVarietyRequestEnforceDefault);
  if (NS_FAILED(rv)) {
    return rv;
  }

  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::GetOriginalURI(nsIURI** aOriginalURI) {
  NS_ENSURE_ARG_POINTER(aOriginalURI);
  *aOriginalURI = do_AddRef(mOriginalURI).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetOriginalURI(nsIURI* aOriginalURI) {
  ENSURE_CALLED_BEFORE_CONNECT();

  NS_ENSURE_ARG_POINTER(aOriginalURI);
  mOriginalURI = aOriginalURI;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetURI(nsIURI** aURI) {
  NS_ENSURE_ARG_POINTER(aURI);
  *aURI = do_AddRef(mURI).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetOwner(nsISupports** aOwner) {
  NS_ENSURE_ARG_POINTER(aOwner);
  *aOwner = do_AddRef(mOwner).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetOwner(nsISupports* aOwner) {
  mOwner = aOwner;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetLoadInfo(nsILoadInfo* aLoadInfo) {
  MOZ_RELEASE_ASSERT(aLoadInfo, "loadinfo can't be null");
  mLoadInfo = aLoadInfo;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLoadInfo(nsILoadInfo** aLoadInfo) {
  *aLoadInfo = do_AddRef(mLoadInfo).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetIsDocument(bool* aIsDocument) {
  return NS_GetIsDocumentChannel(this, aIsDocument);
}

NS_IMETHODIMP
HttpBaseChannel::GetNotificationCallbacks(nsIInterfaceRequestor** aCallbacks) {
  *aCallbacks = do_AddRef(mCallbacks).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetNotificationCallbacks(nsIInterfaceRequestor* aCallbacks) {
  MOZ_ASSERT(NS_IsMainThread(), "Should only be called on the main thread.");

  if (!CanSetCallbacks(aCallbacks)) {
    return NS_ERROR_FAILURE;
  }

  mCallbacks = aCallbacks;
  mProgressSink = nullptr;

  UpdatePrivateBrowsing();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetContentType(nsACString& aContentType) {
  if (!mResponseHead) {
    aContentType.Truncate();
    return NS_ERROR_NOT_AVAILABLE;
  }

  mResponseHead->ContentType(aContentType);
  if (!aContentType.IsEmpty()) {
    return NS_OK;
  }

  aContentType.AssignLiteral(UNKNOWN_CONTENT_TYPE);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetContentType(const nsACString& aContentType) {
  if (mListener || LoadWasOpened() || mDummyChannelForCachedResource) {
    if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;

    nsAutoCString contentTypeBuf, charsetBuf;
    bool hadCharset;
    net_ParseContentType(aContentType, contentTypeBuf, charsetBuf, &hadCharset);

    mResponseHead->SetContentType(contentTypeBuf);

    // take care not to stomp on an existing charset
    if (hadCharset) mResponseHead->SetContentCharset(charsetBuf);

  } else {
    // We are being given a content-type hint.
    bool dummy;
    net_ParseContentType(aContentType, mContentTypeHint, mContentCharsetHint,
                         &dummy);
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetContentCharset(nsACString& aContentCharset) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;

  mResponseHead->ContentCharset(aContentCharset);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetContentCharset(const nsACString& aContentCharset) {
  if (mListener) {
    if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;

    mResponseHead->SetContentCharset(aContentCharset);
  } else {
    // Charset hint
    mContentCharsetHint = aContentCharset;
  }
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetContentDisposition(uint32_t* aContentDisposition) {
  // See bug 1658877. If mContentDispositionHint is already
  // DISPOSITION_ATTACHMENT, it means this channel is created from a
  // download attribute. In this case, we should prefer the value from the
  // download attribute rather than the value in content disposition header.
  // DISPOSITION_FORCE_INLINE is used to explicitly set inline, used by
  // the pdf reader when loading a attachment pdf without having to
  // download it.
  if (mContentDispositionHint == nsIChannel::DISPOSITION_ATTACHMENT ||
      mContentDispositionHint == nsIChannel::DISPOSITION_FORCE_INLINE) {
    *aContentDisposition = mContentDispositionHint;
    return NS_OK;
  }

  nsresult rv;
  nsCString header;

  rv = GetContentDispositionHeader(header);
  if (NS_FAILED(rv)) {
    if (mContentDispositionHint == UINT32_MAX) return rv;

    *aContentDisposition = mContentDispositionHint;
    return NS_OK;
  }

  *aContentDisposition = NS_GetContentDispositionFromHeader(header, this);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetContentDisposition(uint32_t aContentDisposition) {
  mContentDispositionHint = aContentDisposition;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetContentDispositionFilename(
    nsAString& aContentDispositionFilename) {
  aContentDispositionFilename.Truncate();
  nsresult rv;
  nsCString header;

  rv = GetContentDispositionHeader(header);
  if (NS_SUCCEEDED(rv)) {
    rv = NS_GetFilenameFromDisposition(aContentDispositionFilename, header);
  }

  // If we failed to get the filename from header, we should use
  // mContentDispositionFilename, since mContentDispositionFilename is set from
  // the download attribute.
  if (NS_FAILED(rv)) {
    if (!mContentDispositionFilename) {
      return rv;
    }

    aContentDispositionFilename = *mContentDispositionFilename;
    return NS_OK;
  }

  return rv;
}

NS_IMETHODIMP
HttpBaseChannel::SetContentDispositionFilename(
    const nsAString& aContentDispositionFilename) {
  mContentDispositionFilename =
      MakeUnique<nsString>(aContentDispositionFilename);

  // For safety reasons ensure the filename doesn't contain null characters and
  // replace them with underscores. We may later pass the extension to system
  // MIME APIs that expect null terminated strings.
  mContentDispositionFilename->ReplaceChar(char16_t(0), '_');

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetContentDispositionHeader(
    nsACString& aContentDispositionHeader) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;

  nsresult rv = mResponseHead->GetHeader(nsHttp::Content_Disposition,
                                         aContentDispositionHeader);
  if (NS_FAILED(rv) || aContentDispositionHeader.IsEmpty()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetContentLength(int64_t* aContentLength) {
  NS_ENSURE_ARG_POINTER(aContentLength);

  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;

  if (LoadDeliveringAltData()) {
    MOZ_ASSERT(!mAvailableCachedAltDataType.IsEmpty());
    *aContentLength = mAltDataLength;
    return NS_OK;
  }

  *aContentLength = mResponseHead->ContentLength();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetContentLength(int64_t value) {
  if (!mDummyChannelForCachedResource) {
    MOZ_ASSERT_UNREACHABLE("HttpBaseChannel::SetContentLength");
    return NS_ERROR_NOT_IMPLEMENTED;
  }
  MOZ_ASSERT(mResponseHead);
  mResponseHead->SetContentLength(value);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::Open(nsIInputStream** aStream) {
  if (!gHttpHandler->Active()) {
    LOG(("HttpBaseChannel::Open after HTTP shutdown..."));
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsCOMPtr<nsIStreamListener> listener;
  nsresult rv =
      nsContentSecurityManager::doContentSecurityCheck(this, listener);
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ENSURE_TRUE(!LoadWasOpened(), NS_ERROR_IN_PROGRESS);

  if (!gHttpHandler->Active()) {
    LOG(("HttpBaseChannel::Open after HTTP shutdown..."));
    return NS_ERROR_NOT_AVAILABLE;
  }

  return NS_ImplementChannelOpen(this, aStream);
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIUploadChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::GetUploadStream(nsIInputStream** stream) {
  NS_ENSURE_ARG_POINTER(stream);
  *stream = do_AddRef(mUploadStream).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetUploadStream(nsIInputStream* stream,
                                 const nsACString& contentTypeArg,
                                 int64_t contentLength) {
  // NOTE: for backwards compatibility and for compatibility with old style
  // plugins, |stream| may include headers, specifically Content-Type and
  // Content-Length headers.  in this case, |contentType| and |contentLength|
  // would be unspecified.  this is traditionally the case of a POST request,
  // and so we select POST as the request method if contentType and
  // contentLength are unspecified.

  if (stream) {
    nsAutoCString method;
    bool hasHeaders = false;

    // This method and ExplicitSetUploadStream mean different things by "empty
    // content type string".  This method means "no header", but
    // ExplicitSetUploadStream means "header with empty value".  So we have to
    // massage the contentType argument into the form ExplicitSetUploadStream
    // expects.
    nsCOMPtr<nsIMIMEInputStream> mimeStream;
    nsCString contentType(contentTypeArg);
    if (contentType.IsEmpty()) {
      contentType.SetIsVoid(true);
      method = "POST"_ns;

      // MIME streams are a special case, and include headers which need to be
      // copied to the channel.
      mimeStream = do_QueryInterface(stream);
      if (mimeStream) {
        // Copy non-origin related headers to the channel.
        nsCOMPtr<nsIHttpHeaderVisitor> visitor =
            new AddHeadersToChannelVisitor(this);
        mimeStream->VisitHeaders(visitor);

        return ExplicitSetUploadStream(stream, contentType, contentLength,
                                       method, hasHeaders);
      }

      hasHeaders = true;
    } else {
      method = "PUT"_ns;

      MOZ_ASSERT(
          NS_FAILED(CallQueryInterface(stream, getter_AddRefs(mimeStream))),
          "nsIMIMEInputStream should not be set with an explicit content type");
    }
    return ExplicitSetUploadStream(stream, contentType, contentLength, method,
                                   hasHeaders);
  }

  // if stream is null, ExplicitSetUploadStream returns error.
  // So we need special case for GET method.
  StoreUploadStreamHasHeaders(false);
  SetRequestMethod("GET"_ns);  // revert to GET request
  mUploadStream = nullptr;
  return NS_OK;
}

namespace {

class MIMEHeaderCopyVisitor final : public nsIHttpHeaderVisitor {
 public:
  explicit MIMEHeaderCopyVisitor(nsIMIMEInputStream* aDest) : mDest(aDest) {}

  NS_DECL_ISUPPORTS
  NS_IMETHOD VisitHeader(const nsACString& aName,
                         const nsACString& aValue) override {
    return mDest->AddHeader(PromiseFlatCString(aName).get(),
                            PromiseFlatCString(aValue).get());
  }

 private:
  ~MIMEHeaderCopyVisitor() = default;

  nsCOMPtr<nsIMIMEInputStream> mDest;
};

NS_IMPL_ISUPPORTS(MIMEHeaderCopyVisitor, nsIHttpHeaderVisitor)

static void NormalizeCopyComplete(void* aClosure, nsresult aStatus) {
#ifdef DEBUG
  // Called on the STS thread by NS_AsyncCopy
  nsCOMPtr<nsIEventTarget> sts =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
  bool result = false;
  sts->IsOnCurrentThread(&result);
  MOZ_ASSERT(result, "Should only be called on the STS thread.");
#endif

  RefPtr<GenericPromise::Private> ready =
      already_AddRefed(static_cast<GenericPromise::Private*>(aClosure));
  if (NS_SUCCEEDED(aStatus)) {
    ready->Resolve(true, __func__);
  } else {
    ready->Reject(aStatus, __func__);
  }
}

// Normalize the upload stream for a HTTP channel, so that is one of the
// expected and compatible types. Components like WebExtensions and DevTools
// expect that upload streams in the parent process are cloneable, seekable, and
// synchronous to read, which this function helps guarantee somewhat efficiently
// and without loss of information.
//
// If the replacement stream outparameter is not initialized to `nullptr`, the
// returned stream should be used instead of `aUploadStream` as the upload
// stream for the HTTP channel, and the previous stream should not be touched
// again.
//
// If aReadyPromise is non-nullptr after the function is called, it is a promise
// which should be awaited before continuing to `AsyncOpen` the HTTP channel,
// as the replacement stream will not be ready until it is resolved.
static nsresult NormalizeUploadStream(nsIInputStream* aUploadStream,
                                      nsIInputStream** aReplacementStream,
                                      GenericPromise** aReadyPromise) {
  MOZ_ASSERT(XRE_IsParentProcess());

  *aReplacementStream = nullptr;
  *aReadyPromise = nullptr;

  // Unwrap RemoteLazyInputStream and normalize the contents as we're in the
  // parent process.
  if (nsCOMPtr<mozIRemoteLazyInputStream> lazyStream =
          do_QueryInterface(aUploadStream)) {
    nsCOMPtr<nsIInputStream> internal;
    if (NS_SUCCEEDED(
            lazyStream->TakeInternalStream(getter_AddRefs(internal)))) {
      nsCOMPtr<nsIInputStream> replacement;
      nsresult rv = NormalizeUploadStream(internal, getter_AddRefs(replacement),
                                          aReadyPromise);
      NS_ENSURE_SUCCESS(rv, rv);

      if (replacement) {
        replacement.forget(aReplacementStream);
      } else {
        internal.forget(aReplacementStream);
      }
      return NS_OK;
    }
  }

  // Preserve MIME information on the stream when normalizing.
  if (nsCOMPtr<nsIMIMEInputStream> mime = do_QueryInterface(aUploadStream)) {
    nsCOMPtr<nsIInputStream> data;
    nsresult rv = mime->GetData(getter_AddRefs(data));
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIInputStream> replacement;
    rv =
        NormalizeUploadStream(data, getter_AddRefs(replacement), aReadyPromise);
    NS_ENSURE_SUCCESS(rv, rv);

    if (replacement) {
      nsCOMPtr<nsIMIMEInputStream> replacementMime(
          do_CreateInstance("@mozilla.org/network/mime-input-stream;1", &rv));
      NS_ENSURE_SUCCESS(rv, rv);

      nsCOMPtr<nsIHttpHeaderVisitor> visitor =
          new MIMEHeaderCopyVisitor(replacementMime);
      rv = mime->VisitHeaders(visitor);
      NS_ENSURE_SUCCESS(rv, rv);

      rv = replacementMime->SetData(replacement);
      NS_ENSURE_SUCCESS(rv, rv);

      replacementMime.forget(aReplacementStream);
    }
    return NS_OK;
  }

  // Preserve "real" buffered input streams which wrap data (i.e. are backed by
  // nsBufferedInputStream), but normalize the wrapped stream.
  if (nsCOMPtr<nsIBufferedInputStream> buffered =
          do_QueryInterface(aUploadStream)) {
    nsCOMPtr<nsIInputStream> data;
    if (NS_SUCCEEDED(buffered->GetData(getter_AddRefs(data)))) {
      nsCOMPtr<nsIInputStream> replacement;
      nsresult rv = NormalizeUploadStream(data, getter_AddRefs(replacement),
                                          aReadyPromise);
      NS_ENSURE_SUCCESS(rv, rv);
      if (replacement) {
        // This buffer size should be kept in sync with HTMLFormSubmission.
        rv = NS_NewBufferedInputStream(aReplacementStream, replacement.forget(),
                                       8192);
        NS_ENSURE_SUCCESS(rv, rv);
      }
      return NS_OK;
    }
  }

  // Preserve multiplex input streams, normalizing each individual inner stream
  // to avoid unnecessary copying.
  if (nsCOMPtr<nsIMultiplexInputStream> multiplex =
          do_QueryInterface(aUploadStream)) {
    uint32_t count = multiplex->GetCount();
    nsTArray<nsCOMPtr<nsIInputStream>> streams(count);
    nsTArray<RefPtr<GenericPromise>> promises(count);
    bool replace = false;
    for (uint32_t i = 0; i < count; ++i) {
      nsCOMPtr<nsIInputStream> inner;
      nsresult rv = multiplex->GetStream(i, getter_AddRefs(inner));
      NS_ENSURE_SUCCESS(rv, rv);

      RefPtr<GenericPromise> promise;
      nsCOMPtr<nsIInputStream> replacement;
      rv = NormalizeUploadStream(inner, getter_AddRefs(replacement),
                                 getter_AddRefs(promise));
      NS_ENSURE_SUCCESS(rv, rv);
      if (promise) {
        promises.AppendElement(promise);
      }
      if (replacement) {
        streams.AppendElement(replacement);
        replace = true;
      } else {
        streams.AppendElement(inner);
      }
    }

    // If any of the inner streams needed to be replaced, replace the entire
    // nsIMultiplexInputStream.
    if (replace) {
      nsresult rv;
      nsCOMPtr<nsIMultiplexInputStream> replacement =
          do_CreateInstance("@mozilla.org/io/multiplex-input-stream;1", &rv);
      NS_ENSURE_SUCCESS(rv, rv);
      for (auto& stream : streams) {
        rv = replacement->AppendStream(stream);
        NS_ENSURE_SUCCESS(rv, rv);
      }

      MOZ_ALWAYS_SUCCEEDS(CallQueryInterface(replacement, aReplacementStream));
    }

    // Wait for all inner promises to settle before resolving the final promise.
    if (!promises.IsEmpty()) {
      RefPtr<GenericPromise> ready =
          GenericPromise::AllSettled(GetCurrentSerialEventTarget(), promises)
              ->Then(GetCurrentSerialEventTarget(), __func__,
                     [](GenericPromise::AllSettledPromiseType::
                            ResolveOrRejectValue&& aResults)
                         -> RefPtr<GenericPromise> {
                       MOZ_ASSERT(aResults.IsResolve(),
                                  "AllSettled never rejects");
                       for (auto& result : aResults.ResolveValue()) {
                         if (result.IsReject()) {
                           return GenericPromise::CreateAndReject(
                               result.RejectValue(), __func__);
                         }
                       }
                       return GenericPromise::CreateAndResolve(true, __func__);
                     });
      ready.forget(aReadyPromise);
    }
    return NS_OK;
  }

  // If the stream is cloneable, seekable and non-async, we can allow it.  Async
  // input streams can cause issues, as various consumers of input streams
  // expect the payload to be synchronous and `Available()` to be the length of
  // the stream, which is not true for asynchronous streams.
  nsCOMPtr<nsIAsyncInputStream> async = do_QueryInterface(aUploadStream);
  nsCOMPtr<nsISeekableStream> seekable = do_QueryInterface(aUploadStream);
  if (NS_InputStreamIsCloneable(aUploadStream) && seekable && !async) {
    return NS_OK;
  }

  // Asynchronously copy our non-normalized stream into a StorageStream so that
  // it is seekable, cloneable, and synchronous once the copy completes.

  NS_WARNING("Upload Stream is being copied into StorageStream");

  nsCOMPtr<nsIStorageStream> storageStream;
  nsresult rv =
      NS_NewStorageStream(4096, UINT32_MAX, getter_AddRefs(storageStream));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIOutputStream> sink;
  rv = storageStream->GetOutputStream(0, getter_AddRefs(sink));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIInputStream> replacementStream;
  rv = storageStream->NewInputStream(0, getter_AddRefs(replacementStream));
  NS_ENSURE_SUCCESS(rv, rv);

  // Ensure the source stream is buffered before starting the copy so we can use
  // ReadSegments, as nsStorageStream doesn't implement WriteSegments.
  nsCOMPtr<nsIInputStream> source = aUploadStream;
  if (!NS_InputStreamIsBuffered(aUploadStream)) {
    nsCOMPtr<nsIInputStream> bufferedSource;
    rv = NS_NewBufferedInputStream(getter_AddRefs(bufferedSource),
                                   source.forget(), 4096);
    NS_ENSURE_SUCCESS(rv, rv);
    source = bufferedSource.forget();
  }

  // Perform an AsyncCopy into the input stream on the STS.
  nsCOMPtr<nsIEventTarget> target =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
  RefPtr<GenericPromise::Private> ready = new GenericPromise::Private(__func__);
  rv = NS_AsyncCopy(source, sink, target, NS_ASYNCCOPY_VIA_READSEGMENTS, 4096,
                    NormalizeCopyComplete, do_AddRef(ready).take());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    ready.get()->Release();
    return rv;
  }

  replacementStream.forget(aReplacementStream);
  ready.forget(aReadyPromise);
  return NS_OK;
}

}  // anonymous namespace

NS_IMETHODIMP
HttpBaseChannel::CloneUploadStream(int64_t* aContentLength,
                                   nsIInputStream** aClonedStream) {
  NS_ENSURE_ARG_POINTER(aContentLength);
  NS_ENSURE_ARG_POINTER(aClonedStream);
  *aClonedStream = nullptr;

  if (!XRE_IsParentProcess()) {
    NS_WARNING("CloneUploadStream is only supported in the parent process");
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (!mUploadStream) {
    return NS_OK;
  }

  nsCOMPtr<nsIInputStream> clonedStream;
  nsresult rv =
      NS_CloneInputStream(mUploadStream, getter_AddRefs(clonedStream));
  NS_ENSURE_SUCCESS(rv, rv);

  clonedStream.forget(aClonedStream);

  *aContentLength = mReqContentLength;
  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIUploadChannel2
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::ExplicitSetUploadStream(nsIInputStream* aStream,
                                         const nsACString& aContentType,
                                         int64_t aContentLength,
                                         const nsACString& aMethod,
                                         bool aStreamHasHeaders) {
  // Ensure stream is set and method is valid
  NS_ENSURE_TRUE(aStream, NS_ERROR_FAILURE);

  {
    DebugOnly<nsCOMPtr<nsIMIMEInputStream>> mimeStream;
    MOZ_ASSERT(
        !aStreamHasHeaders || NS_FAILED(CallQueryInterface(
                                  aStream, getter_AddRefs(mimeStream.value))),
        "nsIMIMEInputStream should not include headers");
  }

  nsresult rv = SetRequestMethod(aMethod);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!aStreamHasHeaders && !aContentType.IsVoid()) {
    if (aContentType.IsEmpty()) {
      SetEmptyRequestHeader("Content-Type"_ns);
    } else {
      SetRequestHeader("Content-Type"_ns, aContentType, false);
    }
  }

  StoreUploadStreamHasHeaders(aStreamHasHeaders);

  return InternalSetUploadStream(aStream, aContentLength, !aStreamHasHeaders);
}

nsresult HttpBaseChannel::InternalSetUploadStream(
    nsIInputStream* aUploadStream, int64_t aContentLength,
    bool aSetContentLengthHeader) {
  // If we're not on the main thread, such as for TRR, the content length must
  // be provided, as we can't normalize our upload stream.
  if (!NS_IsMainThread()) {
    if (aContentLength < 0) {
      MOZ_ASSERT_UNREACHABLE(
          "Upload content length must be explicit off-main-thread");
      return NS_ERROR_INVALID_ARG;
    }

    nsCOMPtr<nsISeekableStream> seekable = do_QueryInterface(aUploadStream);
    if (!NS_InputStreamIsCloneable(aUploadStream) || !seekable) {
      MOZ_ASSERT_UNREACHABLE(
          "Upload stream must be cloneable & seekable off-main-thread");
      return NS_ERROR_INVALID_ARG;
    }

    mUploadStream = aUploadStream;
    ExplicitSetUploadStreamLength(aContentLength, aSetContentLengthHeader);
    return NS_OK;
  }

  // Normalize the upload stream we're provided to ensure that it is cloneable,
  // seekable, and synchronous when in the parent process.
  //
  // This might be an async operation, in which case ready will be returned and
  // resolved when the operation is complete.
  nsCOMPtr<nsIInputStream> replacement;
  RefPtr<GenericPromise> ready;
  if (XRE_IsParentProcess()) {
    nsresult rv = NormalizeUploadStream(
        aUploadStream, getter_AddRefs(replacement), getter_AddRefs(ready));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  mUploadStream = replacement ? replacement.get() : aUploadStream;

  // Once the upload stream is ready, fetch its length before proceeding with
  // AsyncOpen.
  auto onReady = [self = RefPtr{this}, aContentLength, aSetContentLengthHeader,
                  stream = mUploadStream]() {
    auto setLengthAndResume = [self, aSetContentLengthHeader](int64_t aLength) {
      self->StorePendingUploadStreamNormalization(false);
      self->ExplicitSetUploadStreamLength(aLength >= 0 ? aLength : 0,
                                          aSetContentLengthHeader);
      self->MaybeResumeAsyncOpen();
    };

    if (aContentLength >= 0) {
      setLengthAndResume(aContentLength);
      return;
    }

    int64_t length;
    if (InputStreamLengthHelper::GetSyncLength(stream, &length)) {
      setLengthAndResume(length);
      return;
    }

    InputStreamLengthHelper::GetAsyncLength(stream, setLengthAndResume);
  };
  StorePendingUploadStreamNormalization(true);

  // Resolve onReady synchronously unless a promise is returned.
  if (ready) {
    ready->Then(GetCurrentSerialEventTarget(), __func__,
                [onReady = std::move(onReady)](
                    GenericPromise::ResolveOrRejectValue&&) { onReady(); });
  } else {
    onReady();
  }
  return NS_OK;
}

void HttpBaseChannel::ExplicitSetUploadStreamLength(
    uint64_t aContentLength, bool aSetContentLengthHeader) {
  // We already have the content length. We don't need to determinate it.
  mReqContentLength = aContentLength;

  if (!aSetContentLengthHeader) {
    return;
  }

  nsAutoCString header;
  header.AssignLiteral("Content-Length");

  // Maybe the content-length header has been already set.
  nsAutoCString value;
  nsresult rv = GetRequestHeader(header, value);
  if (NS_SUCCEEDED(rv) && !value.IsEmpty()) {
    return;
  }

  nsAutoCString contentLengthStr;
  contentLengthStr.AppendInt(aContentLength);
  SetRequestHeader(header, contentLengthStr, false);
}

NS_IMETHODIMP
HttpBaseChannel::GetUploadStreamHasHeaders(bool* hasHeaders) {
  NS_ENSURE_ARG(hasHeaders);

  *hasHeaders = LoadUploadStreamHasHeaders();
  return NS_OK;
}

bool HttpBaseChannel::MaybeWaitForUploadStreamNormalization(
    nsIStreamListener* aListener, nsISupports* aContext) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!LoadAsyncOpenWaitingForStreamNormalization(),
             "AsyncOpen() called twice?");

  if (!LoadPendingUploadStreamNormalization()) {
    return false;
  }

  mListener = aListener;
  StoreAsyncOpenWaitingForStreamNormalization(true);
  return true;
}

void HttpBaseChannel::MaybeResumeAsyncOpen() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!LoadPendingUploadStreamNormalization());

  if (!LoadAsyncOpenWaitingForStreamNormalization()) {
    return;
  }

  nsCOMPtr<nsIStreamListener> listener;
  listener.swap(mListener);

  StoreAsyncOpenWaitingForStreamNormalization(false);

  nsresult rv = AsyncOpen(listener);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    DoAsyncAbort(rv);
  }
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIEncodedChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::GetApplyConversion(bool* value) {
  *value = LoadApplyConversion();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetApplyConversion(bool value) {
  LOG(("HttpBaseChannel::SetApplyConversion [this=%p value=%d]\n", this,
       value));
  StoreApplyConversion(value);
  return NS_OK;
}

nsresult HttpBaseChannel::DoApplyContentConversions(
    nsIStreamListener* aNextListener, nsIStreamListener** aNewNextListener) {
  return DoApplyContentConversions(aNextListener, aNewNextListener, nullptr);
}

// create a listener chain that looks like this
// http-channel -> decompressor (n times) -> InterceptFailedOnSTop ->
// channel-creator-listener
//
// we need to do this because not every decompressor has fully streamed output
// so may need a call to OnStopRequest to identify its completion state.. and if
// it creates an error there the channel status code needs to be updated before
// calling the terminal listener. Having the decompress do it via cancel() means
// channels cannot effectively be used in two contexts (specifically this one
// and a peek context for sniffing)
//
class InterceptFailedOnStop : public nsIThreadRetargetableStreamListener {
  virtual ~InterceptFailedOnStop() = default;
  nsCOMPtr<nsIStreamListener> mNext;
  HttpBaseChannel* mChannel;

 public:
  InterceptFailedOnStop(nsIStreamListener* arg, HttpBaseChannel* chan)
      : mNext(arg), mChannel(chan) {}
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSITHREADRETARGETABLESTREAMLISTENER

  NS_IMETHOD OnStartRequest(nsIRequest* aRequest) override {
    return mNext->OnStartRequest(aRequest);
  }

  NS_IMETHOD OnStopRequest(nsIRequest* aRequest,
                           nsresult aStatusCode) override {
    if (NS_FAILED(aStatusCode) && NS_SUCCEEDED(mChannel->mStatus)) {
      LOG(("HttpBaseChannel::InterceptFailedOnStop %p seting status %" PRIx32,
           mChannel, static_cast<uint32_t>(aStatusCode)));
      mChannel->mStatus = aStatusCode;
    }
    return mNext->OnStopRequest(aRequest, aStatusCode);
  }

  NS_IMETHOD OnDataAvailable(nsIRequest* aRequest, nsIInputStream* aInputStream,
                             uint64_t aOffset, uint32_t aCount) override {
    return mNext->OnDataAvailable(aRequest, aInputStream, aOffset, aCount);
  }
};

NS_IMPL_ADDREF(InterceptFailedOnStop)
NS_IMPL_RELEASE(InterceptFailedOnStop)

NS_INTERFACE_MAP_BEGIN(InterceptFailedOnStop)
  NS_INTERFACE_MAP_ENTRY(nsIStreamListener)
  NS_INTERFACE_MAP_ENTRY(nsIRequestObserver)
  NS_INTERFACE_MAP_ENTRY(nsIThreadRetargetableStreamListener)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIRequestObserver)
NS_INTERFACE_MAP_END

NS_IMETHODIMP
InterceptFailedOnStop::CheckListenerChain() {
  nsCOMPtr<nsIThreadRetargetableStreamListener> listener =
      do_QueryInterface(mNext);
  if (!listener) {
    return NS_ERROR_NO_INTERFACE;
  }

  return listener->CheckListenerChain();
}

NS_IMETHODIMP
InterceptFailedOnStop::OnDataFinished(nsresult aStatus) {
  nsCOMPtr<nsIThreadRetargetableStreamListener> listener =
      do_QueryInterface(mNext);
  if (listener) {
    return listener->OnDataFinished(aStatus);
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::DoApplyContentConversions(nsIStreamListener* aNextListener,
                                           nsIStreamListener** aNewNextListener,
                                           nsISupports* aCtxt) {
  *aNewNextListener = nullptr;
  if (!mResponseHead || !aNextListener) {
    return NS_OK;
  }

  LOG(("HttpBaseChannel::DoApplyContentConversions [this=%p]\n", this));

  if (!LoadApplyConversion()) {
    LOG(("not applying conversion per ApplyConversion\n"));
    return NS_OK;
  }

  if (LoadHasAppliedConversion()) {
    LOG(("not applying conversion because HasAppliedConversion is true\n"));
    return NS_OK;
  }

  if (LoadDeliveringAltData()) {
    MOZ_ASSERT(!mAvailableCachedAltDataType.IsEmpty());
    LOG(("not applying conversion because delivering alt-data\n"));
    return NS_OK;
  }

  nsAutoCString contentEncoding;
  nsresult rv =
      mResponseHead->GetHeader(nsHttp::Content_Encoding, contentEncoding);
  if (NS_FAILED(rv) || contentEncoding.IsEmpty()) return NS_OK;

  nsCOMPtr<nsIStreamListener> nextListener =
      new InterceptFailedOnStop(aNextListener, this);

  // The encodings are listed in the order they were applied
  // (see rfc 2616 section 14.11), so they need to removed in reverse
  // order. This is accomplished because the converter chain ends up
  // being a stack with the last converter created being the first one
  // to accept the raw network data.

  char* cePtr = contentEncoding.BeginWriting();
  uint32_t count = 0;
  while (char* val = nsCRT::strtok(cePtr, HTTP_LWS ",", &cePtr)) {
    if (++count > 16) {
      // That's ridiculous. We only understand 2 different ones :)
      // but for compatibility with old code, we will just carry on without
      // removing the encodings
      LOG(("Too many Content-Encodings. Ignoring remainder.\n"));
      break;
    }

    if (gHttpHandler->IsAcceptableEncoding(val,
                                           isSecureOrTrustworthyURL(mURI))) {
      RefPtr<nsHTTPCompressConv> converter = new nsHTTPCompressConv();
      nsAutoCString from(val);
      ToLowerCase(from);
      rv = converter->AsyncConvertData(from.get(), "uncompressed", nextListener,
                                       aCtxt);
      if (NS_FAILED(rv)) {
        LOG(("Unexpected failure of AsyncConvertData %s\n", val));
        return rv;
      }

      LOG(("converter removed '%s' content-encoding\n", val));
      if (Telemetry::CanRecordPrereleaseData()) {
        int mode = 0;
        if (from.EqualsLiteral("gzip") || from.EqualsLiteral("x-gzip")) {
          mode = 1;
        } else if (from.EqualsLiteral("deflate") ||
                   from.EqualsLiteral("x-deflate")) {
          mode = 2;
        } else if (from.EqualsLiteral("br")) {
          mode = 3;
        } else if (from.EqualsLiteral("zstd")) {
          mode = 4;
        }
        glean::http::content_encoding.AccumulateSingleSample(mode);
      }
      nextListener = converter;
    } else {
      if (val) LOG(("Unknown content encoding '%s', ignoring\n", val));
    }
  }
  *aNewNextListener = do_AddRef(nextListener).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetContentEncodings(nsIUTF8StringEnumerator** aEncodings) {
  if (!mResponseHead) {
    *aEncodings = nullptr;
    return NS_OK;
  }

  nsAutoCString encoding;
  Unused << mResponseHead->GetHeader(nsHttp::Content_Encoding, encoding);
  if (encoding.IsEmpty()) {
    *aEncodings = nullptr;
    return NS_OK;
  }
  RefPtr<nsContentEncodings> enumerator =
      new nsContentEncodings(this, encoding.get());
  enumerator.forget(aEncodings);
  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsContentEncodings <public>
//-----------------------------------------------------------------------------

HttpBaseChannel::nsContentEncodings::nsContentEncodings(
    nsIHttpChannel* aChannel, const char* aEncodingHeader)
    : mEncodingHeader(aEncodingHeader), mChannel(aChannel), mReady(false) {
  mCurEnd = aEncodingHeader + strlen(aEncodingHeader);
  mCurStart = mCurEnd;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsContentEncodings::nsISimpleEnumerator
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::nsContentEncodings::HasMore(bool* aMoreEncodings) {
  if (mReady) {
    *aMoreEncodings = true;
    return NS_OK;
  }

  nsresult rv = PrepareForNext();
  *aMoreEncodings = NS_SUCCEEDED(rv);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::nsContentEncodings::GetNext(nsACString& aNextEncoding) {
  aNextEncoding.Truncate();
  if (!mReady) {
    nsresult rv = PrepareForNext();
    if (NS_FAILED(rv)) {
      return NS_ERROR_FAILURE;
    }
  }

  const nsACString& encoding = Substring(mCurStart, mCurEnd);

  nsACString::const_iterator start, end;
  encoding.BeginReading(start);
  encoding.EndReading(end);

  bool haveType = false;
  if (CaseInsensitiveFindInReadable("gzip"_ns, start, end)) {
    aNextEncoding.AssignLiteral(APPLICATION_GZIP);
    haveType = true;
  }

  if (!haveType) {
    encoding.BeginReading(start);
    if (CaseInsensitiveFindInReadable("compress"_ns, start, end)) {
      aNextEncoding.AssignLiteral(APPLICATION_COMPRESS);
      haveType = true;
    }
  }

  if (!haveType) {
    encoding.BeginReading(start);
    if (CaseInsensitiveFindInReadable("deflate"_ns, start, end)) {
      aNextEncoding.AssignLiteral(APPLICATION_ZIP);
      haveType = true;
    }
  }

  if (!haveType) {
    encoding.BeginReading(start);
    if (CaseInsensitiveFindInReadable("br"_ns, start, end)) {
      aNextEncoding.AssignLiteral(APPLICATION_BROTLI);
      haveType = true;
    }
  }

  if (!haveType) {
    encoding.BeginReading(start);
    if (CaseInsensitiveFindInReadable("zstd"_ns, start, end)) {
      aNextEncoding.AssignLiteral(APPLICATION_ZSTD);
      haveType = true;
    }
  }

  // Prepare to fetch the next encoding
  mCurEnd = mCurStart;
  mReady = false;

  if (haveType) return NS_OK;

  NS_WARNING("Unknown encoding type");
  return NS_ERROR_FAILURE;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsContentEncodings::nsISupports
//-----------------------------------------------------------------------------

NS_IMPL_ISUPPORTS(HttpBaseChannel::nsContentEncodings, nsIUTF8StringEnumerator,
                  nsIStringEnumerator)

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsContentEncodings <private>
//-----------------------------------------------------------------------------

nsresult HttpBaseChannel::nsContentEncodings::PrepareForNext(void) {
  MOZ_ASSERT(mCurStart == mCurEnd, "Indeterminate state");

  // At this point both mCurStart and mCurEnd point to somewhere
  // past the end of the next thing we want to return

  while (mCurEnd != mEncodingHeader) {
    --mCurEnd;
    if (*mCurEnd != ',' && !nsCRT::IsAsciiSpace(*mCurEnd)) break;
  }
  if (mCurEnd == mEncodingHeader) {
    return NS_ERROR_NOT_AVAILABLE;  // no more encodings
  }
  ++mCurEnd;

  // At this point mCurEnd points to the first char _after_ the
  // header we want.  Furthermore, mCurEnd - 1 != mEncodingHeader

  mCurStart = mCurEnd - 1;
  while (mCurStart != mEncodingHeader && *mCurStart != ',' &&
         !nsCRT::IsAsciiSpace(*mCurStart)) {
    --mCurStart;
  }
  if (*mCurStart == ',' || nsCRT::IsAsciiSpace(*mCurStart)) {
    ++mCurStart;  // we stopped because of a weird char, so move up one
  }

  // At this point mCurStart and mCurEnd bracket the encoding string
  // we want.  Check that it's not "identity"
  if (Substring(mCurStart, mCurEnd)
          .Equals("identity", nsCaseInsensitiveCStringComparator)) {
    mCurEnd = mCurStart;
    return PrepareForNext();
  }

  mReady = true;
  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIHttpChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::GetChannelId(uint64_t* aChannelId) {
  NS_ENSURE_ARG_POINTER(aChannelId);
  *aChannelId = mChannelId;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetChannelId(uint64_t aChannelId) {
  mChannelId = aChannelId;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::GetTopLevelContentWindowId(uint64_t* aWindowId) {
  if (!mContentWindowId) {
    nsCOMPtr<nsILoadContext> loadContext;
    GetCallback(loadContext);
    if (loadContext) {
      nsCOMPtr<mozIDOMWindowProxy> topWindow;
      loadContext->GetTopWindow(getter_AddRefs(topWindow));
      if (topWindow) {
        if (nsPIDOMWindowInner* inner =
                nsPIDOMWindowOuter::From(topWindow)->GetCurrentInnerWindow()) {
          mContentWindowId = inner->WindowID();
        }
      }
    }
  }
  *aWindowId = mContentWindowId;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::SetBrowserId(uint64_t aId) {
  mBrowserId = aId;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::GetBrowserId(uint64_t* aId) {
  EnsureBrowserId();
  *aId = mBrowserId;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::SetTopLevelContentWindowId(uint64_t aWindowId) {
  mContentWindowId = aWindowId;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::IsThirdPartyTrackingResource(bool* aIsTrackingResource) {
  MOZ_ASSERT(
      !(mFirstPartyClassificationFlags && mThirdPartyClassificationFlags));
  *aIsTrackingResource = UrlClassifierCommon::IsTrackingClassificationFlag(
      mThirdPartyClassificationFlags,
      mLoadInfo->GetOriginAttributes().IsPrivateBrowsing());
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::IsThirdPartySocialTrackingResource(
    bool* aIsThirdPartySocialTrackingResource) {
  MOZ_ASSERT(!mFirstPartyClassificationFlags ||
             !mThirdPartyClassificationFlags);
  *aIsThirdPartySocialTrackingResource =
      UrlClassifierCommon::IsSocialTrackingClassificationFlag(
          mThirdPartyClassificationFlags);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetClassificationFlags(uint32_t* aFlags) {
  if (mThirdPartyClassificationFlags) {
    *aFlags = mThirdPartyClassificationFlags;
  } else {
    *aFlags = mFirstPartyClassificationFlags;
  }
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetFirstPartyClassificationFlags(uint32_t* aFlags) {
  *aFlags = mFirstPartyClassificationFlags;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetThirdPartyClassificationFlags(uint32_t* aFlags) {
  *aFlags = mThirdPartyClassificationFlags;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetTransferSize(uint64_t* aTransferSize) {
  MutexAutoLock lock(mOnDataFinishedMutex);
  *aTransferSize = mTransferSize;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestSize(uint64_t* aRequestSize) {
  *aRequestSize = mRequestSize;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetDecodedBodySize(uint64_t* aDecodedBodySize) {
  *aDecodedBodySize = mDecodedBodySize;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetEncodedBodySize(uint64_t* aEncodedBodySize) {
  MutexAutoLock lock(mOnDataFinishedMutex);
  *aEncodedBodySize = mEncodedBodySize;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetSupportsHTTP3(bool* aSupportsHTTP3) {
  *aSupportsHTTP3 = mSupportsHTTP3;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetHasHTTPSRR(bool* aHasHTTPSRR) {
  *aHasHTTPSRR = LoadHasHTTPSRR();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestMethod(nsACString& aMethod) {
  mRequestHead.Method(aMethod);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRequestMethod(const nsACString& aMethod) {
  ENSURE_CALLED_BEFORE_CONNECT();

  mLoadInfo->SetIsGETRequest(aMethod.Equals("GET"));

  const nsCString& flatMethod = PromiseFlatCString(aMethod);

  // Method names are restricted to valid HTTP tokens.
  if (!nsHttp::IsValidToken(flatMethod)) return NS_ERROR_INVALID_ARG;

  mRequestHead.SetMethod(flatMethod);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetReferrerInfo(nsIReferrerInfo** aReferrerInfo) {
  NS_ENSURE_ARG_POINTER(aReferrerInfo);
  *aReferrerInfo = do_AddRef(mReferrerInfo).take();
  return NS_OK;
}

nsresult HttpBaseChannel::SetReferrerInfoInternal(
    nsIReferrerInfo* aReferrerInfo, bool aClone, bool aCompute,
    bool aRespectBeforeConnect) {
  LOG(
      ("HttpBaseChannel::SetReferrerInfoInternal [this=%p aClone(%d) "
       "aCompute(%d)]\n",
       this, aClone, aCompute));
  if (aRespectBeforeConnect) {
    ENSURE_CALLED_BEFORE_CONNECT();
  }

  mReferrerInfo = aReferrerInfo;

  // clear existing referrer, if any
  nsresult rv = ClearReferrerHeader();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (!mReferrerInfo) {
    return NS_OK;
  }

  if (aClone) {
    mReferrerInfo = static_cast<dom::ReferrerInfo*>(aReferrerInfo)->Clone();
  }

  dom::ReferrerInfo* referrerInfo =
      static_cast<dom::ReferrerInfo*>(mReferrerInfo.get());

  // Don't set referrerInfo if it has not been initialized.
  if (!referrerInfo->IsInitialized()) {
    mReferrerInfo = nullptr;
    return NS_ERROR_NOT_INITIALIZED;
  }

  if (aClone) {
    // Record the telemetry once we set the referrer info to the channel
    // successfully.
    referrerInfo->RecordTelemetry(this);
  }

  if (aCompute) {
    rv = referrerInfo->ComputeReferrer(this);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  nsCOMPtr<nsIURI> computedReferrer = mReferrerInfo->GetComputedReferrer();
  if (!computedReferrer) {
    return NS_OK;
  }

  nsAutoCString spec;
  rv = computedReferrer->GetSpec(spec);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return SetReferrerHeader(spec, aRespectBeforeConnect);
}

NS_IMETHODIMP
HttpBaseChannel::SetReferrerInfo(nsIReferrerInfo* aReferrerInfo) {
  return SetReferrerInfoInternal(aReferrerInfo, true, true, true);
}

NS_IMETHODIMP
HttpBaseChannel::SetReferrerInfoWithoutClone(nsIReferrerInfo* aReferrerInfo) {
  return SetReferrerInfoInternal(aReferrerInfo, false, true, true);
}

// Return the channel's proxy URI, or if it doesn't exist, the
// channel's main URI.
NS_IMETHODIMP
HttpBaseChannel::GetProxyURI(nsIURI** aOut) {
  NS_ENSURE_ARG_POINTER(aOut);
  nsCOMPtr<nsIURI> result(mProxyURI);
  result.forget(aOut);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestHeader(const nsACString& aHeader,
                                  nsACString& aValue) {
  aValue.Truncate();

  // XXX might be better to search the header list directly instead of
  // hitting the http atom hash table.
  nsHttpAtom atom = nsHttp::ResolveAtom(aHeader);
  if (!atom) return NS_ERROR_NOT_AVAILABLE;

  return mRequestHead.GetHeader(atom, aValue);
}

NS_IMETHODIMP
HttpBaseChannel::SetRequestHeader(const nsACString& aHeader,
                                  const nsACString& aValue, bool aMerge) {
  return SetRequestHeaderInternal(aHeader, aValue, aMerge,
                                  nsHttpHeaderArray::eVarietyRequestOverride);
}

nsresult HttpBaseChannel::SetRequestHeaderInternal(
    const nsACString& aHeader, const nsACString& aValue, bool aMerge,
    nsHttpHeaderArray::HeaderVariety aVariety) {
  const nsCString& flatHeader = PromiseFlatCString(aHeader);
  const nsCString& flatValue = PromiseFlatCString(aValue);

  LOG(
      ("HttpBaseChannel::SetRequestHeader [this=%p header=\"%s\" value=\"%s\" "
       "merge=%u]\n",
       this, flatHeader.get(), flatValue.get(), aMerge));

  // Verify header names are valid HTTP tokens and header values are reasonably
  // close to whats allowed in RFC 2616.
  if (!nsHttp::IsValidToken(flatHeader) ||
      !nsHttp::IsReasonableHeaderValue(flatValue)) {
    return NS_ERROR_INVALID_ARG;
  }

  // Mark that the User-Agent header has been modified.
  if (nsHttp::ResolveAtom(aHeader) == nsHttp::User_Agent) {
    StoreIsUserAgentHeaderModified(true);
  }

  return mRequestHead.SetHeader(aHeader, flatValue, aMerge);
}

NS_IMETHODIMP
HttpBaseChannel::SetNewReferrerInfo(const nsACString& aUrl,
                                    nsIReferrerInfo::ReferrerPolicyIDL aPolicy,
                                    bool aSendReferrer) {
  nsresult rv;
  // Create URI from string
  nsCOMPtr<nsIURI> aURI;
  rv = NS_NewURI(getter_AddRefs(aURI), aUrl);
  NS_ENSURE_SUCCESS(rv, rv);
  // Create new ReferrerInfo and initialize it.
  nsCOMPtr<nsIReferrerInfo> referrerInfo = new mozilla::dom::ReferrerInfo();
  rv = referrerInfo->Init(aPolicy, aSendReferrer, aURI);
  NS_ENSURE_SUCCESS(rv, rv);
  // Set ReferrerInfo
  return SetReferrerInfo(referrerInfo);
}

NS_IMETHODIMP
HttpBaseChannel::SetEmptyRequestHeader(const nsACString& aHeader) {
  const nsCString& flatHeader = PromiseFlatCString(aHeader);

  LOG(("HttpBaseChannel::SetEmptyRequestHeader [this=%p header=\"%s\"]\n", this,
       flatHeader.get()));

  // Verify header names are valid HTTP tokens and header values are reasonably
  // close to whats allowed in RFC 2616.
  if (!nsHttp::IsValidToken(flatHeader)) {
    return NS_ERROR_INVALID_ARG;
  }

  // Mark that the User-Agent header has been modified.
  if (nsHttp::ResolveAtom(aHeader) == nsHttp::User_Agent) {
    StoreIsUserAgentHeaderModified(true);
  }

  return mRequestHead.SetEmptyHeader(aHeader);
}

NS_IMETHODIMP
HttpBaseChannel::VisitRequestHeaders(nsIHttpHeaderVisitor* visitor) {
  return mRequestHead.VisitHeaders(visitor);
}

NS_IMETHODIMP
HttpBaseChannel::VisitNonDefaultRequestHeaders(nsIHttpHeaderVisitor* visitor) {
  return mRequestHead.VisitHeaders(visitor,
                                   nsHttpHeaderArray::eFilterSkipDefault);
}

NS_IMETHODIMP
HttpBaseChannel::GetResponseHeader(const nsACString& header,
                                   nsACString& value) {
  value.Truncate();

  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;

  nsHttpAtom atom = nsHttp::ResolveAtom(header);
  if (!atom) return NS_ERROR_NOT_AVAILABLE;

  return mResponseHead->GetHeader(atom, value);
}

NS_IMETHODIMP
HttpBaseChannel::SetResponseHeader(const nsACString& header,
                                   const nsACString& value, bool merge) {
  LOG(
      ("HttpBaseChannel::SetResponseHeader [this=%p header=\"%s\" value=\"%s\" "
       "merge=%u]\n",
       this, PromiseFlatCString(header).get(), PromiseFlatCString(value).get(),
       merge));

  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;

  nsHttpAtom atom = nsHttp::ResolveAtom(header);
  if (!atom) return NS_ERROR_NOT_AVAILABLE;

  // these response headers must not be changed
  if (atom == nsHttp::Content_Type || atom == nsHttp::Content_Length ||
      atom == nsHttp::Content_Encoding || atom == nsHttp::Trailer ||
      atom == nsHttp::Transfer_Encoding) {
    return NS_ERROR_ILLEGAL_VALUE;
  }

  StoreResponseHeadersModified(true);

  return mResponseHead->SetHeader(header, value, merge);
}

NS_IMETHODIMP
HttpBaseChannel::VisitResponseHeaders(nsIHttpHeaderVisitor* visitor) {
  if (!mResponseHead) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  return mResponseHead->VisitHeaders(visitor,
                                     nsHttpHeaderArray::eFilterResponse);
}

NS_IMETHODIMP
HttpBaseChannel::GetOriginalResponseHeader(const nsACString& aHeader,
                                           nsIHttpHeaderVisitor* aVisitor) {
  if (!mResponseHead) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsHttpAtom atom = nsHttp::ResolveAtom(aHeader);
  if (!atom) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return mResponseHead->GetOriginalHeader(atom, aVisitor);
}

NS_IMETHODIMP
HttpBaseChannel::VisitOriginalResponseHeaders(nsIHttpHeaderVisitor* aVisitor) {
  if (!mResponseHead) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return mResponseHead->VisitHeaders(
      aVisitor, nsHttpHeaderArray::eFilterResponseOriginal);
}

NS_IMETHODIMP
HttpBaseChannel::GetAllowSTS(bool* value) {
  NS_ENSURE_ARG_POINTER(value);
  *value = LoadAllowSTS();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetAllowSTS(bool value) {
  ENSURE_CALLED_BEFORE_CONNECT();
  StoreAllowSTS(value);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetIsOCSP(bool* value) {
  NS_ENSURE_ARG_POINTER(value);
  *value = LoadIsOCSP();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetIsOCSP(bool value) {
  ENSURE_CALLED_BEFORE_CONNECT();
  StoreIsOCSP(value);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetIsUserAgentHeaderModified(bool* value) {
  NS_ENSURE_ARG_POINTER(value);
  *value = LoadIsUserAgentHeaderModified();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetIsUserAgentHeaderModified(bool value) {
  StoreIsUserAgentHeaderModified(value);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRedirectionLimit(uint32_t* value) {
  NS_ENSURE_ARG_POINTER(value);
  *value = mRedirectionLimit;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRedirectionLimit(uint32_t value) {
  ENSURE_CALLED_BEFORE_CONNECT();

  mRedirectionLimit = std::min<uint32_t>(value, 0xff);
  return NS_OK;
}

nsresult HttpBaseChannel::OverrideSecurityInfo(
    nsITransportSecurityInfo* aSecurityInfo) {
  MOZ_ASSERT(!mSecurityInfo,
             "This can only be called when we don't have a security info "
             "object already");
  MOZ_RELEASE_ASSERT(
      aSecurityInfo,
      "This can only be called with a valid security info object");
  MOZ_ASSERT(!BypassServiceWorker(),
             "This can only be called on channels that are not bypassing "
             "interception");
  MOZ_ASSERT(LoadResponseCouldBeSynthesized(),
             "This can only be called on channels that can be intercepted");
  if (mSecurityInfo) {
    LOG(
        ("HttpBaseChannel::OverrideSecurityInfo mSecurityInfo is null! "
         "[this=%p]\n",
         this));
    return NS_ERROR_UNEXPECTED;
  }
  if (!LoadResponseCouldBeSynthesized()) {
    LOG(
        ("HttpBaseChannel::OverrideSecurityInfo channel cannot be intercepted! "
         "[this=%p]\n",
         this));
    return NS_ERROR_UNEXPECTED;
  }

  mSecurityInfo = aSecurityInfo;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::IsNoStoreResponse(bool* value) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;
  *value = mResponseHead->NoStore();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::IsNoCacheResponse(bool* value) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;
  *value = mResponseHead->NoCache();
  if (!*value) *value = mResponseHead->ExpiresInPast();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::IsPrivateResponse(bool* value) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;
  *value = mResponseHead->Private();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetResponseStatus(uint32_t* aValue) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;
  *aValue = mResponseHead->Status();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetResponseStatusText(nsACString& aValue) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;
  nsAutoCString version;
  // https://fetch.spec.whatwg.org :
  // Responses over an HTTP/2 connection will always have the empty byte
  // sequence as status message as HTTP/2 does not support them.
  if (NS_WARN_IF(NS_FAILED(GetProtocolVersion(version))) ||
      !version.EqualsLiteral("h2")) {
    mResponseHead->StatusText(aValue);
  }
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestSucceeded(bool* aValue) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;
  uint32_t status = mResponseHead->Status();
  *aValue = (status / 100 == 2);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::RedirectTo(nsIURI* targetURI) {
  NS_ENSURE_ARG(targetURI);

  nsAutoCString spec;
  targetURI->GetAsciiSpec(spec);
  LOG(("HttpBaseChannel::RedirectTo [this=%p, uri=%s]", this, spec.get()));
  LogCallingScriptLocation(this);

  // We cannot redirect after OnStartRequest of the listener
  // has been called, since to redirect we have to switch channels
  // and the dance with OnStartRequest et al has to start over.
  // This would break the nsIStreamListener contract.
  NS_ENSURE_FALSE(LoadOnStartRequestCalled(), NS_ERROR_NOT_AVAILABLE);

  // The first parameter is the URI we would like to redirect to
  // The second parameter should default to false if normal redirect
  mAPIRedirectTo =
      Some(mozilla::MakeCompactPair(nsCOMPtr<nsIURI>(targetURI), false));

  // Only Web Extensions are allowed to redirect a channel to a data:
  // URI. To avoid any bypasses after the channel was flagged by
  // the WebRequst API, we are dropping the flag here.
  mLoadInfo->SetAllowInsecureRedirectToDataURI(false);

  // We may want to rewrite origin allowance, hence we need an
  // artificial response head.
  if (!mResponseHead) {
    mResponseHead.reset(new nsHttpResponseHead());
  }
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::TransparentRedirectTo(nsIURI* targetURI) {
  LOG(("HttpBaseChannel::TransparentRedirectTo [this=%p]", this));
  RedirectTo(targetURI);
  MOZ_ASSERT(mAPIRedirectTo, "How did this happen?");
  mAPIRedirectTo->second() = true;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::UpgradeToSecure() {
  // Upgrades are handled internally between http-on-modify-request and
  // http-on-before-connect, which means upgrades are only possible during
  // on-modify, or WebRequest.onBeforeRequest in Web Extensions.  Once we are
  // past the code path where upgrades are handled, attempting an upgrade
  // will throw an error.
  NS_ENSURE_TRUE(LoadUpgradableToSecure(), NS_ERROR_NOT_AVAILABLE);

  StoreUpgradeToSecure(true);
  // todo: Currently UpgradeToSecure() is called only by web extensions, if
  // that ever changes, we need to update the following telemetry collection
  // to reflect any future changes.
  mLoadInfo->SetHttpsUpgradeTelemetry(nsILoadInfo::WEB_EXTENSION_UPGRADE);

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestObserversCalled(bool* aCalled) {
  NS_ENSURE_ARG_POINTER(aCalled);
  *aCalled = LoadRequestObserversCalled();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRequestObserversCalled(bool aCalled) {
  StoreRequestObserversCalled(aCalled);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestContextID(uint64_t* aRCID) {
  NS_ENSURE_ARG_POINTER(aRCID);
  *aRCID = mRequestContextID;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRequestContextID(uint64_t aRCID) {
  mRequestContextID = aRCID;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetIsMainDocumentChannel(bool* aValue) {
  NS_ENSURE_ARG_POINTER(aValue);
  *aValue = IsNavigation();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetIsMainDocumentChannel(bool aValue) {
  StoreForceMainDocumentChannel(aValue);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetProtocolVersion(nsACString& aProtocolVersion) {
  // Try to use ALPN if available and if it is not for a proxy, i.e if an
  // https proxy was not used or if https proxy was used but the connection to
  // the origin server is also https. In the case, an https proxy was used and
  // the connection to the origin server was http, mSecurityInfo will be from
  // the proxy.
  if (!mConnectionInfo || !mConnectionInfo->UsingHttpsProxy() ||
      mConnectionInfo->EndToEndSSL()) {
    nsAutoCString protocol;
    if (mSecurityInfo &&
        NS_SUCCEEDED(mSecurityInfo->GetNegotiatedNPN(protocol)) &&
        !protocol.IsEmpty()) {
      // The negotiated protocol was not empty so we can use it.
      aProtocolVersion = protocol;
      return NS_OK;
    }
  }

  if (mResponseHead) {
    HttpVersion version = mResponseHead->Version();
    aProtocolVersion.Assign(nsHttp::GetProtocolVersion(version));
    return NS_OK;
  }

  return NS_ERROR_NOT_AVAILABLE;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIHttpChannelInternal
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::SetTopWindowURIIfUnknown(nsIURI* aTopWindowURI) {
  if (!aTopWindowURI) {
    return NS_ERROR_INVALID_ARG;
  }

  if (mTopWindowURI) {
    LOG(
        ("HttpChannelBase::SetTopWindowURIIfUnknown [this=%p] "
         "mTopWindowURI is already set.\n",
         this));
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIURI> topWindowURI;
  Unused << GetTopWindowURI(getter_AddRefs(topWindowURI));

  // Don't modify |mTopWindowURI| if we can get one from GetTopWindowURI().
  if (topWindowURI) {
    LOG(
        ("HttpChannelBase::SetTopWindowURIIfUnknown [this=%p] "
         "Return an error since we got a top window uri.\n",
         this));
    return NS_ERROR_FAILURE;
  }

  mTopWindowURI = aTopWindowURI;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetTopWindowURI(nsIURI** aTopWindowURI) {
  nsCOMPtr<nsIURI> uriBeingLoaded =
      AntiTrackingUtils::MaybeGetDocumentURIBeingLoaded(this);
  return GetTopWindowURI(uriBeingLoaded, aTopWindowURI);
}

nsresult HttpBaseChannel::GetTopWindowURI(nsIURI* aURIBeingLoaded,
                                          nsIURI** aTopWindowURI) {
  nsresult rv = NS_OK;
  nsCOMPtr<mozIThirdPartyUtil> util;
  // Only compute the top window URI once. In e10s, this must be computed in the
  // child. The parent gets the top window URI through HttpChannelOpenArgs.
  if (!mTopWindowURI) {
    util = components::ThirdPartyUtil::Service();
    if (!util) {
      return NS_ERROR_NOT_AVAILABLE;
    }
    nsCOMPtr<mozIDOMWindowProxy> win;
    rv = util->GetTopWindowForChannel(this, aURIBeingLoaded,
                                      getter_AddRefs(win));
    if (NS_SUCCEEDED(rv)) {
      rv = util->GetURIFromWindow(win, getter_AddRefs(mTopWindowURI));
#if DEBUG
      if (mTopWindowURI) {
        nsCString spec;
        if (NS_SUCCEEDED(mTopWindowURI->GetSpec(spec))) {
          LOG(("HttpChannelBase::Setting topwindow URI spec %s [this=%p]\n",
               spec.get(), this));
        }
      }
#endif
    }
  }
  *aTopWindowURI = do_AddRef(mTopWindowURI).take();
  return rv;
}

NS_IMETHODIMP
HttpBaseChannel::GetDocumentURI(nsIURI** aDocumentURI) {
  NS_ENSURE_ARG_POINTER(aDocumentURI);
  *aDocumentURI = do_AddRef(mDocumentURI).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetDocumentURI(nsIURI* aDocumentURI) {
  ENSURE_CALLED_BEFORE_CONNECT();
  mDocumentURI = aDocumentURI;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestVersion(uint32_t* major, uint32_t* minor) {
  HttpVersion version = mRequestHead.Version();

  if (major) {
    *major = static_cast<uint32_t>(version) / 10;
  }
  if (minor) {
    *minor = static_cast<uint32_t>(version) % 10;
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetResponseVersion(uint32_t* major, uint32_t* minor) {
  if (!mResponseHead) {
    *major = *minor = 0;  // we should at least be kind about it
    return NS_ERROR_NOT_AVAILABLE;
  }

  HttpVersion version = mResponseHead->Version();

  if (major) {
    *major = static_cast<uint32_t>(version) / 10;
  }
  if (minor) {
    *minor = static_cast<uint32_t>(version) % 10;
  }

  return NS_OK;
}

bool HttpBaseChannel::IsBrowsingContextDiscarded() const {
  // If there is no loadGroup attached to the current channel, we check the
  // global private browsing state for the private channel instead. For
  // non-private channel, we will always return false here.
  //
  // Note that we can only access the global private browsing state in the
  // parent process. So, we will fallback to just return false in the content
  // process.
  if (!mLoadGroup) {
    if (!XRE_IsParentProcess()) {
      return false;
    }

    return mLoadInfo->GetOriginAttributes().IsPrivateBrowsing() &&
           !dom::CanonicalBrowsingContext::IsPrivateBrowsingActive();
  }

  return mLoadGroup->GetIsBrowsingContextDiscarded();
}

// https://mikewest.github.io/corpp/#process-navigation-response
nsresult HttpBaseChannel::ProcessCrossOriginEmbedderPolicyHeader() {
  nsresult rv;
  if (!StaticPrefs::browser_tabs_remote_useCrossOriginEmbedderPolicy()) {
    return NS_OK;
  }

  // Only consider Cross-Origin-Embedder-Policy for document loads.
  if (mLoadInfo->GetExternalContentPolicyType() !=
          ExtContentPolicy::TYPE_DOCUMENT &&
      mLoadInfo->GetExternalContentPolicyType() !=
          ExtContentPolicy::TYPE_SUBDOCUMENT) {
    return NS_OK;
  }

  nsILoadInfo::CrossOriginEmbedderPolicy resultPolicy =
      nsILoadInfo::EMBEDDER_POLICY_NULL;
  bool isCoepCredentiallessEnabled;
  rv = mLoadInfo->GetIsOriginTrialCoepCredentiallessEnabledForTopLevel(
      &isCoepCredentiallessEnabled);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = GetResponseEmbedderPolicy(isCoepCredentiallessEnabled, &resultPolicy);
  if (NS_FAILED(rv)) {
    return NS_OK;
  }

  // https://html.spec.whatwg.org/multipage/origin.html#coep
  if (mLoadInfo->GetExternalContentPolicyType() ==
          ExtContentPolicy::TYPE_SUBDOCUMENT &&
      !nsHttpChannel::IsRedirectStatus(mResponseHead->Status()) &&
      mLoadInfo->GetLoadingEmbedderPolicy() !=
          nsILoadInfo::EMBEDDER_POLICY_NULL &&
      resultPolicy != nsILoadInfo::EMBEDDER_POLICY_REQUIRE_CORP &&
      resultPolicy != nsILoadInfo::EMBEDDER_POLICY_CREDENTIALLESS) {
    return NS_ERROR_DOM_COEP_FAILED;
  }

  return NS_OK;
}

// https://mikewest.github.io/corpp/#corp-check
nsresult HttpBaseChannel::ProcessCrossOriginResourcePolicyHeader() {
  // Fetch 4.5.9
  dom::RequestMode requestMode;
  MOZ_ALWAYS_SUCCEEDS(GetRequestMode(&requestMode));
  // XXX this seems wrong per spec? What about navigate
  if (requestMode != RequestMode::No_cors) {
    return NS_OK;
  }

  // We only apply this for resources.
  auto extContentPolicyType = mLoadInfo->GetExternalContentPolicyType();
  if (extContentPolicyType == ExtContentPolicy::TYPE_DOCUMENT ||
      extContentPolicyType == ExtContentPolicy::TYPE_WEBSOCKET ||
      extContentPolicyType == ExtContentPolicy::TYPE_SAVEAS_DOWNLOAD) {
    return NS_OK;
  }

  if (extContentPolicyType == ExtContentPolicy::TYPE_SUBDOCUMENT) {
    // COEP pref off, skip CORP checking for subdocument.
    if (!StaticPrefs::browser_tabs_remote_useCrossOriginEmbedderPolicy()) {
      return NS_OK;
    }
    // COEP 3.2.1.2 when request targets a nested browsing context then embedder
    // policy value is "unsafe-none", then return allowed.
    if (mLoadInfo->GetLoadingEmbedderPolicy() ==
        nsILoadInfo::EMBEDDER_POLICY_NULL) {
      return NS_OK;
    }
  }

  MOZ_ASSERT(mLoadInfo->GetLoadingPrincipal(),
             "Resources should always have a LoadingPrincipal");
  if (!mResponseHead) {
    return NS_OK;
  }

  if (mLoadInfo->GetLoadingPrincipal()->IsSystemPrincipal()) {
    return NS_OK;
  }

  nsAutoCString content;
  Unused << mResponseHead->GetHeader(nsHttp::Cross_Origin_Resource_Policy,
                                     content);

  if (StaticPrefs::browser_tabs_remote_useCrossOriginEmbedderPolicy()) {
    if (content.IsEmpty()) {
      if (mLoadInfo->GetLoadingEmbedderPolicy() ==
          nsILoadInfo::EMBEDDER_POLICY_CREDENTIALLESS) {
        bool requestIncludesCredentials = false;
        nsresult rv = GetCorsIncludeCredentials(&requestIncludesCredentials);
        if (NS_FAILED(rv)) {
          return NS_OK;
        }
        // COEP: Set policy to `same-origin` if: response’s
        // request-includes-credentials is true, or forNavigation is true.
        if (requestIncludesCredentials ||
            extContentPolicyType == ExtContentPolicyType::TYPE_SUBDOCUMENT) {
          content = "same-origin"_ns;
        }
      } else if (mLoadInfo->GetLoadingEmbedderPolicy() ==
                 nsILoadInfo::EMBEDDER_POLICY_REQUIRE_CORP) {
        // COEP 3.2.1.6 If policy is null, and embedder policy is
        // "require-corp", set policy to "same-origin". Note that we treat
        // invalid value as "cross-origin", which spec indicates. We might want
        // to make that stricter.
        content = "same-origin"_ns;
      }
    }
  }

  if (content.IsEmpty()) {
    return NS_OK;
  }

  nsCOMPtr<nsIPrincipal> channelOrigin;
  nsContentUtils::GetSecurityManager()->GetChannelResultPrincipal(
      this, getter_AddRefs(channelOrigin));

  // Cross-Origin-Resource-Policy = %s"same-origin" / %s"same-site" /
  // %s"cross-origin"
  if (content.EqualsLiteral("same-origin")) {
    if (!channelOrigin->Equals(mLoadInfo->GetLoadingPrincipal())) {
      return NS_ERROR_DOM_CORP_FAILED;
    }
    return NS_OK;
  }
  if (content.EqualsLiteral("same-site")) {
    nsAutoCString documentBaseDomain;
    nsAutoCString resourceBaseDomain;
    mLoadInfo->GetLoadingPrincipal()->GetBaseDomain(documentBaseDomain);
    channelOrigin->GetBaseDomain(resourceBaseDomain);
    if (documentBaseDomain != resourceBaseDomain) {
      return NS_ERROR_DOM_CORP_FAILED;
    }

    nsCOMPtr<nsIURI> resourceURI = channelOrigin->GetURI();
    if (!mLoadInfo->GetLoadingPrincipal()->SchemeIs("https") &&
        resourceURI->SchemeIs("https")) {
      return NS_ERROR_DOM_CORP_FAILED;
    }

    return NS_OK;
  }

  return NS_OK;
}

// See https://gist.github.com/annevk/6f2dd8c79c77123f39797f6bdac43f3e
// This method runs steps 1-4 of the algorithm to compare
// cross-origin-opener policies
static bool CompareCrossOriginOpenerPolicies(
    nsILoadInfo::CrossOriginOpenerPolicy documentPolicy,
    nsIPrincipal* documentOrigin,
    nsILoadInfo::CrossOriginOpenerPolicy resultPolicy,
    nsIPrincipal* resultOrigin) {
  if (documentPolicy == nsILoadInfo::OPENER_POLICY_UNSAFE_NONE &&
      resultPolicy == nsILoadInfo::OPENER_POLICY_UNSAFE_NONE) {
    return true;
  }

  if (documentPolicy == nsILoadInfo::OPENER_POLICY_UNSAFE_NONE ||
      resultPolicy == nsILoadInfo::OPENER_POLICY_UNSAFE_NONE) {
    return false;
  }

  if (documentPolicy == resultPolicy && documentOrigin->Equals(resultOrigin)) {
    return true;
  }

  return false;
}

// This runs steps 1-5 of the algorithm when navigating a top level document.
// See https://gist.github.com/annevk/6f2dd8c79c77123f39797f6bdac43f3e
nsresult HttpBaseChannel::ComputeCrossOriginOpenerPolicyMismatch() {
  MOZ_ASSERT(XRE_IsParentProcess());

  StoreHasCrossOriginOpenerPolicyMismatch(false);
  if (!StaticPrefs::browser_tabs_remote_useCrossOriginOpenerPolicy()) {
    return NS_OK;
  }

  // Only consider Cross-Origin-Opener-Policy for toplevel document loads.
  if (mLoadInfo->GetExternalContentPolicyType() !=
      ExtContentPolicy::TYPE_DOCUMENT) {
    return NS_OK;
  }

  // Maybe the channel failed and we have no response head?
  if (!mResponseHead) {
    // Not having a response head is not a hard failure at the point where
    // this method is called.
    return NS_OK;
  }

  RefPtr<mozilla::dom::BrowsingContext> ctx;
  mLoadInfo->GetBrowsingContext(getter_AddRefs(ctx));

  // In xpcshell-tests we don't always have a browsingContext
  if (!ctx) {
    return NS_OK;
  }

  nsCOMPtr<nsIPrincipal> resultOrigin;
  nsContentUtils::GetSecurityManager()->GetChannelResultPrincipal(
      this, getter_AddRefs(resultOrigin));

  // Get the policy of the active document, and the policy for the result.
  nsILoadInfo::CrossOriginOpenerPolicy documentPolicy = ctx->GetOpenerPolicy();
  nsILoadInfo::CrossOriginOpenerPolicy resultPolicy =
      nsILoadInfo::OPENER_POLICY_UNSAFE_NONE;
  Unused << ComputeCrossOriginOpenerPolicy(documentPolicy, &resultPolicy);
  mComputedCrossOriginOpenerPolicy = resultPolicy;

  // Add a permission to mark this site as high-value into the permission DB.
  if (resultPolicy != nsILoadInfo::OPENER_POLICY_UNSAFE_NONE) {
    mozilla::dom::AddHighValuePermission(
        resultOrigin, mozilla::dom::kHighValueCOOPPermission);
  }

  // If bc's popup sandboxing flag set is not empty and potentialCOOP is
  // non-null, then navigate bc to a network error and abort these steps.
  if (resultPolicy != nsILoadInfo::OPENER_POLICY_UNSAFE_NONE &&
      mLoadInfo->GetSandboxFlags()) {
    LOG((
        "HttpBaseChannel::ComputeCrossOriginOpenerPolicyMismatch network error "
        "for non empty sandboxing and non null COOP"));
    return NS_ERROR_DOM_COOP_FAILED;
  }

  // In xpcshell-tests we don't always have a current window global
  RefPtr<mozilla::dom::WindowGlobalParent> currentWindowGlobal =
      ctx->Canonical()->GetCurrentWindowGlobal();
  if (!currentWindowGlobal) {
    return NS_OK;
  }

  // We use the top window principal as the documentOrigin
  nsCOMPtr<nsIPrincipal> documentOrigin =
      currentWindowGlobal->DocumentPrincipal();

  bool compareResult = CompareCrossOriginOpenerPolicies(
      documentPolicy, documentOrigin, resultPolicy, resultOrigin);

  if (LOG_ENABLED()) {
    LOG(
        ("HttpBaseChannel::HasCrossOriginOpenerPolicyMismatch - "
         "doc:%d result:%d - compare:%d\n",
         documentPolicy, resultPolicy, compareResult));
    nsAutoCString docOrigin("(null)");
    nsCOMPtr<nsIURI> uri = documentOrigin->GetURI();
    if (uri) {
      uri->GetSpec(docOrigin);
    }
    nsAutoCString resOrigin("(null)");
    uri = resultOrigin->GetURI();
    if (uri) {
      uri->GetSpec(resOrigin);
    }
    LOG(("doc origin:%s - res origin: %s\n", docOrigin.get(), resOrigin.get()));
  }

  if (compareResult) {
    return NS_OK;
  }

  // If one of the following is false:
  //   - document's policy is same-origin-allow-popups
  //   - resultPolicy is null
  //   - doc is the initial about:blank document
  // then we have a mismatch.

  if (documentPolicy != nsILoadInfo::OPENER_POLICY_SAME_ORIGIN_ALLOW_POPUPS) {
    StoreHasCrossOriginOpenerPolicyMismatch(true);
    return NS_OK;
  }

  if (resultPolicy != nsILoadInfo::OPENER_POLICY_UNSAFE_NONE) {
    StoreHasCrossOriginOpenerPolicyMismatch(true);
    return NS_OK;
  }

  if (!currentWindowGlobal->IsInitialDocument()) {
    StoreHasCrossOriginOpenerPolicyMismatch(true);
    return NS_OK;
  }

  return NS_OK;
}

nsresult HttpBaseChannel::ProcessCrossOriginSecurityHeaders() {
  StoreProcessCrossOriginSecurityHeadersCalled(true);
  nsresult rv = ProcessCrossOriginEmbedderPolicyHeader();
  if (NS_FAILED(rv)) {
    return rv;
  }
  rv = ProcessCrossOriginResourcePolicyHeader();
  if (NS_FAILED(rv)) {
    return rv;
  }
  return ComputeCrossOriginOpenerPolicyMismatch();
}

enum class Report { Error, Warning };

// Helper Function to report messages to the console when the loaded
// script had a wrong MIME type.
void ReportMimeTypeMismatch(HttpBaseChannel* aChannel, const char* aMessageName,
                            nsIURI* aURI, const nsACString& aContentType,
                            Report report) {
  NS_ConvertUTF8toUTF16 spec(aURI->GetSpecOrDefault());
  NS_ConvertUTF8toUTF16 contentType(aContentType);

  aChannel->LogMimeTypeMismatch(nsCString(aMessageName),
                                report == Report::Warning, spec, contentType);
}

// Check and potentially enforce X-Content-Type-Options: nosniff
nsresult ProcessXCTO(HttpBaseChannel* aChannel, nsIURI* aURI,
                     nsHttpResponseHead* aResponseHead,
                     nsILoadInfo* aLoadInfo) {
  if (!aURI || !aResponseHead || !aLoadInfo) {
    // if there is no uri, no response head or no loadInfo, then there is
    // nothing to do
    return NS_OK;
  }

  // 1) Query the XCTO header and check if 'nosniff' is the first value.
  nsAutoCString contentTypeOptionsHeader;
  if (!aResponseHead->GetContentTypeOptionsHeader(contentTypeOptionsHeader)) {
    // if failed to get XCTO header, then there is nothing to do.
    return NS_OK;
  }

  // let's compare the header (ignoring case)
  // e.g. "NoSniFF" -> "nosniff"
  // if it's not 'nosniff' then there is nothing to do here
  if (!contentTypeOptionsHeader.EqualsIgnoreCase("nosniff")) {
    // since we are getting here, the XCTO header was sent;
    // a non matching value most likely means a mistake happenend;
    // e.g. sending 'nosnif' instead of 'nosniff', let's log a warning.
    AutoTArray<nsString, 1> params;
    CopyUTF8toUTF16(contentTypeOptionsHeader, *params.AppendElement());
    RefPtr<dom::Document> doc;
    aLoadInfo->GetLoadingDocument(getter_AddRefs(doc));
    nsContentUtils::ReportToConsole(nsIScriptError::warningFlag, "XCTO"_ns, doc,
                                    nsContentUtils::eSECURITY_PROPERTIES,
                                    "XCTOHeaderValueMissing", params);
    return NS_OK;
  }

  // 2) Query the content type from the channel
  nsAutoCString contentType;
  aResponseHead->ContentType(contentType);

  // 3) Compare the expected MIME type with the actual type
  if (aLoadInfo->GetExternalContentPolicyType() ==
      ExtContentPolicy::TYPE_STYLESHEET) {
    if (contentType.EqualsLiteral(TEXT_CSS)) {
      return NS_OK;
    }
    ReportMimeTypeMismatch(aChannel, "MimeTypeMismatch2", aURI, contentType,
                           Report::Error);
    return NS_ERROR_CORRUPTED_CONTENT;
  }

  if (aLoadInfo->GetExternalContentPolicyType() ==
      ExtContentPolicy::TYPE_SCRIPT) {
    if (nsContentUtils::IsJavascriptMIMEType(
            NS_ConvertUTF8toUTF16(contentType))) {
      return NS_OK;
    }
    ReportMimeTypeMismatch(aChannel, "MimeTypeMismatch2", aURI, contentType,
                           Report::Error);
    return NS_ERROR_CORRUPTED_CONTENT;
  }

  auto policyType = aLoadInfo->GetExternalContentPolicyType();
  if (policyType == ExtContentPolicy::TYPE_DOCUMENT ||
      policyType == ExtContentPolicy::TYPE_SUBDOCUMENT) {
    // If the header XCTO nosniff is set for any browsing context, then
    // we set the skipContentSniffing flag on the Loadinfo. Within
    // GetMIMETypeFromContent we then bail early and do not do any sniffing.
    aLoadInfo->SetSkipContentSniffing(true);
    return NS_OK;
  }

  return NS_OK;
}

// Ensure that a load of type script has correct MIME type
nsresult EnsureMIMEOfScript(HttpBaseChannel* aChannel, nsIURI* aURI,
                            nsHttpResponseHead* aResponseHead,
                            nsILoadInfo* aLoadInfo) {
  if (!aURI || !aResponseHead || !aLoadInfo) {
    // if there is no uri, no response head or no loadInfo, then there is
    // nothing to do
    return NS_OK;
  }

  if (aLoadInfo->GetExternalContentPolicyType() !=
      ExtContentPolicy::TYPE_SCRIPT) {
    // if this is not a script load, then there is nothing to do
    return NS_OK;
  }

  nsAutoCString contentType;
  aResponseHead->ContentType(contentType);
  NS_ConvertUTF8toUTF16 typeString(contentType);

  if (nsContentUtils::IsJavascriptMIMEType(typeString)) {
    // script load has type script
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eJavascript)
        .Add();
    return NS_OK;
  }

  nsContentPolicyType internalType = aLoadInfo->InternalContentPolicyType();
  bool isModule =
      internalType == nsIContentPolicy::TYPE_INTERNAL_MODULE ||
      internalType == nsIContentPolicy::TYPE_INTERNAL_MODULE_PRELOAD;

  if (isModule && nsContentUtils::IsJsonMimeType(typeString)) {
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eJavascript)
        .Add();
    return NS_OK;
  }

  switch (aLoadInfo->InternalContentPolicyType()) {
    case nsIContentPolicy::TYPE_SCRIPT:
    case nsIContentPolicy::TYPE_INTERNAL_SCRIPT:
    case nsIContentPolicy::TYPE_INTERNAL_SCRIPT_PRELOAD:
    case nsIContentPolicy::TYPE_INTERNAL_MODULE:
    case nsIContentPolicy::TYPE_INTERNAL_MODULE_PRELOAD:
    case nsIContentPolicy::TYPE_INTERNAL_CHROMEUTILS_COMPILED_SCRIPT:
    case nsIContentPolicy::TYPE_INTERNAL_FRAME_MESSAGEMANAGER_SCRIPT:
      glean::http::script_block_incorrect_mime
          .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eScriptLoad)
          .Add();
      break;
    case nsIContentPolicy::TYPE_INTERNAL_WORKER:
    case nsIContentPolicy::TYPE_INTERNAL_WORKER_STATIC_MODULE:
    case nsIContentPolicy::TYPE_INTERNAL_SHARED_WORKER:
      glean::http::script_block_incorrect_mime
          .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eWorkerLoad)
          .Add();
      break;
    case nsIContentPolicy::TYPE_INTERNAL_SERVICE_WORKER:
      glean::http::script_block_incorrect_mime
          .EnumGet(
              glean::http::ScriptBlockIncorrectMimeLabel::eServiceworkerLoad)
          .Add();
      break;
    case nsIContentPolicy::TYPE_INTERNAL_WORKER_IMPORT_SCRIPTS:
      glean::http::script_block_incorrect_mime
          .EnumGet(
              glean::http::ScriptBlockIncorrectMimeLabel::eImportscriptLoad)
          .Add();
      break;
    case nsIContentPolicy::TYPE_INTERNAL_AUDIOWORKLET:
    case nsIContentPolicy::TYPE_INTERNAL_PAINTWORKLET:
      glean::http::script_block_incorrect_mime
          .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eWorkletLoad)
          .Add();
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("unexpected script type");
      break;
  }

  if (aLoadInfo->GetLoadingPrincipal()->IsSameOrigin(aURI)) {
    // same origin
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eSameOrigin)
        .Add();
  } else {
    bool cors = false;
    nsAutoCString corsOrigin;
    nsresult rv = aResponseHead->GetHeader(
        nsHttp::ResolveAtom("Access-Control-Allow-Origin"_ns), corsOrigin);
    if (NS_SUCCEEDED(rv)) {
      if (corsOrigin.Equals("*")) {
        cors = true;
      } else {
        nsCOMPtr<nsIURI> corsOriginURI;
        rv = NS_NewURI(getter_AddRefs(corsOriginURI), corsOrigin);
        if (NS_SUCCEEDED(rv)) {
          if (aLoadInfo->GetLoadingPrincipal()->IsSameOrigin(corsOriginURI)) {
            cors = true;
          }
        }
      }
    }
    if (cors) {
      // cors origin
      glean::http::script_block_incorrect_mime
          .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eCorsOrigin)
          .Add();
    } else {
      // cross origin
      glean::http::script_block_incorrect_mime
          .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eCrossOrigin)
          .Add();
    }
  }

  bool block = false;
  if (StringBeginsWith(contentType, "image/"_ns)) {
    // script load has type image
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eImage)
        .Add();
    block = true;
  } else if (StringBeginsWith(contentType, "audio/"_ns)) {
    // script load has type audio
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eAudio)
        .Add();
    block = true;
  } else if (StringBeginsWith(contentType, "video/"_ns)) {
    // script load has type video
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eVideo)
        .Add();
    block = true;
  } else if (StringBeginsWith(contentType, "text/csv"_ns)) {
    // script load has type text/csv
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eTextCsv)
        .Add();
    block = true;
  }

  if (block) {
    ReportMimeTypeMismatch(aChannel, "BlockScriptWithWrongMimeType2", aURI,
                           contentType, Report::Error);
    return NS_ERROR_CORRUPTED_CONTENT;
  }

  if (StringBeginsWith(contentType, "text/plain"_ns)) {
    // script load has type text/plain
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eTextPlain)
        .Add();
  } else if (StringBeginsWith(contentType, "text/xml"_ns)) {
    // script load has type text/xml
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eTextXml)
        .Add();
  } else if (StringBeginsWith(contentType, "application/octet-stream"_ns)) {
    // script load has type application/octet-stream
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eAppOctetStream)
        .Add();
  } else if (StringBeginsWith(contentType, "application/xml"_ns)) {
    // script load has type application/xml
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eAppXml)
        .Add();
  } else if (StringBeginsWith(contentType, "application/json"_ns)) {
    // script load has type application/json
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eAppJson)
        .Add();
  } else if (StringBeginsWith(contentType, "text/json"_ns)) {
    // script load has type text/json
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eTextJson)
        .Add();
  } else if (StringBeginsWith(contentType, "text/html"_ns)) {
    // script load has type text/html
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eTextHtml)
        .Add();
  } else if (contentType.IsEmpty()) {
    // script load has no type
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eEmpty)
        .Add();
  } else {
    // script load has unknown type
    glean::http::script_block_incorrect_mime
        .EnumGet(glean::http::ScriptBlockIncorrectMimeLabel::eUnknown)
        .Add();
  }

  // We restrict importScripts() in worker code to JavaScript MIME types.
  if (internalType == nsIContentPolicy::TYPE_INTERNAL_WORKER_IMPORT_SCRIPTS ||
      internalType == nsIContentPolicy::TYPE_INTERNAL_WORKER_STATIC_MODULE) {
    ReportMimeTypeMismatch(aChannel, "BlockImportScriptsWithWrongMimeType",
                           aURI, contentType, Report::Error);
    return NS_ERROR_CORRUPTED_CONTENT;
  }

  if (internalType == nsIContentPolicy::TYPE_INTERNAL_WORKER ||
      internalType == nsIContentPolicy::TYPE_INTERNAL_SHARED_WORKER) {
    // Do not block the load if the feature is not enabled.
    if (!StaticPrefs::security_block_Worker_with_wrong_mime()) {
      return NS_OK;
    }

    ReportMimeTypeMismatch(aChannel, "BlockWorkerWithWrongMimeType", aURI,
                           contentType, Report::Error);
    return NS_ERROR_CORRUPTED_CONTENT;
  }

  // ES6 modules require a strict MIME type check.
  if (isModule) {
    ReportMimeTypeMismatch(aChannel, "BlockModuleWithWrongMimeType", aURI,
                           contentType, Report::Error);
    return NS_ERROR_CORRUPTED_CONTENT;
  }

  return NS_OK;
}

// Warn when a load of type script uses a wrong MIME type and
// wasn't blocked by EnsureMIMEOfScript or ProcessXCTO.
void WarnWrongMIMEOfScript(HttpBaseChannel* aChannel, nsIURI* aURI,
                           nsHttpResponseHead* aResponseHead,
                           nsILoadInfo* aLoadInfo) {
  if (!aURI || !aResponseHead || !aLoadInfo) {
    // If there is no uri, no response head or no loadInfo, then there is
    // nothing to do.
    return;
  }

  if (aLoadInfo->GetExternalContentPolicyType() !=
      ExtContentPolicy::TYPE_SCRIPT) {
    // If this is not a script load, then there is nothing to do.
    return;
  }

  bool succeeded;
  MOZ_ALWAYS_SUCCEEDS(aChannel->GetRequestSucceeded(&succeeded));
  if (!succeeded) {
    // Do not warn for failed loads: HTTP error pages are usually in HTML.
    return;
  }

  nsAutoCString contentType;
  aResponseHead->ContentType(contentType);
  NS_ConvertUTF8toUTF16 typeString(contentType);

  if (nsContentUtils::IsJavascriptMIMEType(typeString)) {
    return;
  }

  nsContentPolicyType internalType = aLoadInfo->InternalContentPolicyType();
  bool isModule =
      internalType == nsIContentPolicy::TYPE_INTERNAL_MODULE ||
      internalType == nsIContentPolicy::TYPE_INTERNAL_MODULE_PRELOAD;
  if (isModule && nsContentUtils::IsJsonMimeType(typeString)) {
    return;
  }

  ReportMimeTypeMismatch(aChannel, "WarnScriptWithWrongMimeType", aURI,
                         contentType, Report::Warning);
}

nsresult HttpBaseChannel::ValidateMIMEType() {
  nsresult rv = EnsureMIMEOfScript(this, mURI, mResponseHead.get(), mLoadInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  rv = ProcessXCTO(this, mURI, mResponseHead.get(), mLoadInfo);
  if (NS_FAILED(rv)) {
    return rv;
  }

  WarnWrongMIMEOfScript(this, mURI, mResponseHead.get(), mLoadInfo);
  return NS_OK;
}

bool HttpBaseChannel::ShouldFilterOpaqueResponse(
    OpaqueResponseFilterFetch aFilterType) const {
  MOZ_ASSERT(ShouldBlockOpaqueResponse());

  if (!mLoadInfo || ConfiguredFilterFetchResponseBehaviour() != aFilterType) {
    return false;
  }

  // We should filter a response in the parent if it is opaque and is the result
  // of a fetch() function from the Fetch specification.
  return mLoadInfo->InternalContentPolicyType() == nsIContentPolicy::TYPE_FETCH;
}

bool HttpBaseChannel::ShouldBlockOpaqueResponse() const {
  if (!mURI || !mResponseHead || !mLoadInfo) {
    // if there is no uri, no response head or no loadInfo, then there is
    // nothing to do
    LOGORB("No block: no mURI, mResponseHead, or mLoadInfo");
    return false;
  }

  nsCOMPtr<nsIPrincipal> principal = mLoadInfo->GetLoadingPrincipal();
  if (!principal || principal->IsSystemPrincipal()) {
    // If it's a top-level load or a system principal, then there is nothing to
    // do.
    LOGORB("No block: top-level load or system principal");
    return false;
  }

  // Check if the response is a opaque response, which means requestMode should
  // be RequestMode::No_cors and responseType should be ResponseType::Opaque.
  nsContentPolicyType contentPolicy = mLoadInfo->InternalContentPolicyType();

  // Skip the RequestMode would be RequestMode::Navigate
  if (contentPolicy == nsIContentPolicy::TYPE_DOCUMENT ||
      contentPolicy == nsIContentPolicy::TYPE_SUBDOCUMENT ||
      contentPolicy == nsIContentPolicy::TYPE_INTERNAL_FRAME ||
      contentPolicy == nsIContentPolicy::TYPE_INTERNAL_IFRAME ||
      // Skip the RequestMode would be RequestMode::Same_origin
      contentPolicy == nsIContentPolicy::TYPE_INTERNAL_WORKER ||
      contentPolicy == nsIContentPolicy::TYPE_INTERNAL_SHARED_WORKER) {
    return false;
  }

  uint32_t securityMode = mLoadInfo->GetSecurityMode();
  // Skip when RequestMode would not be RequestMode::no_cors
  if (securityMode !=
          nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_INHERITS_SEC_CONTEXT &&
      securityMode != nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL) {
    LOGORB("No block: not no_cors requests");
    return false;
  }

  // Only continue when ResponseType would be ResponseType::Opaque
  if (mLoadInfo->GetTainting() != mozilla::LoadTainting::Opaque) {
    LOGORB("No block: not opaque response");
    return false;
  }

  auto extContentPolicyType = mLoadInfo->GetExternalContentPolicyType();
  if (extContentPolicyType == ExtContentPolicy::TYPE_OBJECT ||
      extContentPolicyType == ExtContentPolicy::TYPE_OBJECT_SUBREQUEST ||
      extContentPolicyType == ExtContentPolicy::TYPE_WEBSOCKET ||
      extContentPolicyType == ExtContentPolicy::TYPE_SAVEAS_DOWNLOAD) {
    LOGORB("No block: object || websocket request || save as download");
    return false;
  }

  // Ignore the request from object or embed elements
  if (mLoadInfo->GetIsFromObjectOrEmbed()) {
    LOGORB("No block: Request From <object> or <embed>");
    return false;
  }

  // Exclude no_cors System XHR
  if (extContentPolicyType == ExtContentPolicy::TYPE_XMLHTTPREQUEST) {
    if (securityMode ==
        nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_INHERITS_SEC_CONTEXT) {
      LOGORB("No block: System XHR");
      return false;
    }
  }

  // Exclude no_cors web-identity
  if (extContentPolicyType == ExtContentPolicy::TYPE_WEB_IDENTITY) {
    if (securityMode ==
        nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_INHERITS_SEC_CONTEXT) {
      printf("Allowing ORB for web-identity\n");
      LOGORB("No block: System web-identity");
      return false;
    }
  }

  uint32_t httpsOnlyStatus = mLoadInfo->GetHttpsOnlyStatus();
  if (httpsOnlyStatus & nsILoadInfo::HTTPS_ONLY_BYPASS_ORB) {
    LOGORB("No block: HTTPS_ONLY_BYPASS_ORB");
    return false;
  }

  bool isInDevToolsContext;
  mLoadInfo->GetIsInDevToolsContext(&isInDevToolsContext);
  if (isInDevToolsContext) {
    LOGORB("No block: Request created by devtools");
    return false;
  }

  return true;
}

OpaqueResponse HttpBaseChannel::BlockOrFilterOpaqueResponse(
    OpaqueResponseBlocker* aORB, const nsAString& aReason,
    const OpaqueResponseBlockedTelemetryReason aTelemetryReason,
    const char* aFormat, ...) {
  NimbusFeatures::RecordExposureEvent("opaqueResponseBlocking"_ns, true);

  const bool shouldFilter =
      ShouldFilterOpaqueResponse(OpaqueResponseFilterFetch::BlockedByORB);

  if (MOZ_UNLIKELY(MOZ_LOG_TEST(GetORBLog(), LogLevel::Debug))) {
    va_list ap;
    va_start(ap, aFormat);
    nsVprintfCString logString(aFormat, ap);
    va_end(ap);

    LOGORB("%s: %s", shouldFilter ? "Filtered" : "Blocked", logString.get());
  }

  if (shouldFilter) {
    glean::orb::block_initiator
        .EnumGet(glean::orb::BlockInitiatorLabel::eFilteredFetch)
        .Add();
    // The existence of `mORB` depends on `BlockOrFilterOpaqueResponse` being
    // called before or after sniffing has completed.
    // Another requirement is that `OpaqueResponseFilter` must come after
    // `OpaqueResponseBlocker`, which is why in the case of having an
    // `OpaqueResponseBlocker` we let it handle creating an
    // `OpaqueResponseFilter`.
    if (aORB) {
      MOZ_DIAGNOSTIC_ASSERT(!mORB || aORB == mORB);
      aORB->FilterResponse();
    } else {
      mListener = new OpaqueResponseFilter(mListener);
    }
    return OpaqueResponse::Allow;
  }

  LogORBError(aReason, aTelemetryReason);
  return OpaqueResponse::Block;
}

// The specification for ORB is currently being written:
// https://whatpr.org/fetch/1442.html#orb-algorithm
// The `opaque-response-safelist check` is implemented in:
// * `HttpBaseChannel::PerformOpaqueResponseSafelistCheckBeforeSniff`
// * `nsHttpChannel::DisableIsOpaqueResponseAllowedAfterSniffCheck`
// * `HttpBaseChannel::PerformOpaqueResponseSafelistCheckAfterSniff`
// * `OpaqueResponseBlocker::ValidateJavaScript`
OpaqueResponse
HttpBaseChannel::PerformOpaqueResponseSafelistCheckBeforeSniff() {
  MOZ_ASSERT(XRE_IsParentProcess());

  // https://whatpr.org/fetch/1442.html#http-fetch, step 6.4
  if (!ShouldBlockOpaqueResponse()) {
    return OpaqueResponse::Allow;
  }

  // Regardless of if ORB is enabled or not, we check if we should filter the
  // response in the parent. This way data won't reach a content process that
  // will create a filtered `Response` object. This is enabled when
  // 'browser.opaqueResponseBlocking.filterFetchResponse' is
  // `OpaqueResponseFilterFetch::All`.
  // See https://fetch.spec.whatwg.org/#concept-filtered-response-opaque
  if (ShouldFilterOpaqueResponse(OpaqueResponseFilterFetch::All)) {
    mListener = new OpaqueResponseFilter(mListener);

    // If we're filtering a response in the parent, there will be no data to
    // determine if it should be blocked or not so the only option we have is to
    // allow it.
    return OpaqueResponse::Allow;
  }

  if (!mCachedOpaqueResponseBlockingPref) {
    return OpaqueResponse::Allow;
  }

  // If ORB is enabled, we check if we should filter the response in the parent.
  // This way data won't reach a content process that will create a filtered
  // `Response` object. We allow ORB to determine if the response should be
  // blocked or filtered, but regardless no data should reach the content
  // process. This is enabled when
  // 'browser.opaqueResponseBlocking.filterFetchResponse' is
  // `OpaqueResponseFilterFetch::AllowedByORB`.
  // See https://fetch.spec.whatwg.org/#concept-filtered-response-opaque
  if (ShouldFilterOpaqueResponse(OpaqueResponseFilterFetch::AllowedByORB)) {
    mListener = new OpaqueResponseFilter(mListener);
  }

  glean::opaque_response_blocking::cross_origin_opaque_response_count.Add(1);

  PROFILER_MARKER_TEXT("ORB safelist check", NETWORK, {}, "Before sniff"_ns);

  // https://whatpr.org/fetch/1442.html#orb-algorithm
  // Step 1
  nsAutoCString contentType;
  mResponseHead->ContentType(contentType);

  // Step 2
  nsAutoCString contentTypeOptionsHeader;
  bool nosniff =
      mResponseHead->GetContentTypeOptionsHeader(contentTypeOptionsHeader) &&
      contentTypeOptionsHeader.EqualsIgnoreCase("nosniff");

  // Step 3
  switch (GetOpaqueResponseBlockedReason(contentType, mResponseHead->Status(),
                                         nosniff)) {
    case OpaqueResponseBlockedReason::ALLOWED_SAFE_LISTED:
      // Step 3.1
      return OpaqueResponse::Allow;
    case OpaqueResponseBlockedReason::ALLOWED_SAFE_LISTED_SPEC_BREAKING:
      LOGORB("Allowed %s in a spec breaking way", contentType.get());
      return OpaqueResponse::Allow;
    case OpaqueResponseBlockedReason::BLOCKED_BLOCKLISTED_NEVER_SNIFFED:
      return BlockOrFilterOpaqueResponse(
          mORB, u"mimeType is an opaque-blocklisted-never-sniffed MIME type"_ns,
          OpaqueResponseBlockedTelemetryReason::eMimeNeverSniffed,
          "BLOCKED_BLOCKLISTED_NEVER_SNIFFED");
    case OpaqueResponseBlockedReason::BLOCKED_206_AND_BLOCKLISTED:
      // Step 3.3
      return BlockOrFilterOpaqueResponse(
          mORB,
          u"response's status is 206 and mimeType is an opaque-blocklisted MIME type"_ns,
          OpaqueResponseBlockedTelemetryReason::eResp206Blclisted,
          "BLOCKED_206_AND_BLOCKEDLISTED");
    case OpaqueResponseBlockedReason::
        BLOCKED_NOSNIFF_AND_EITHER_BLOCKLISTED_OR_TEXTPLAIN:
      // Step 3.4
      return BlockOrFilterOpaqueResponse(
          mORB,
          u"nosniff is true and mimeType is an opaque-blocklisted MIME type or its essence is 'text/plain'"_ns,
          OpaqueResponseBlockedTelemetryReason::eNosniffBlcOrTextp,
          "BLOCKED_NOSNIFF_AND_EITHER_BLOCKLISTED_OR_TEXTPLAIN");
    default:
      break;
  }

  // Step 4
  // If it's a media subsequent request, we assume that it will only be made
  // after a successful initial request.
  bool isMediaRequest;
  mLoadInfo->GetIsMediaRequest(&isMediaRequest);
  if (isMediaRequest) {
    bool isMediaInitialRequest;
    mLoadInfo->GetIsMediaInitialRequest(&isMediaInitialRequest);
    if (!isMediaInitialRequest) {
      return OpaqueResponse::Allow;
    }
  }

  // Step 5
  if (mResponseHead->Status() == 206 &&
      !IsFirstPartialResponse(*mResponseHead)) {
    return BlockOrFilterOpaqueResponse(
        mORB, u"response status is 206 and not first partial response"_ns,
        OpaqueResponseBlockedTelemetryReason::eResp206Blclisted,
        "Is not a valid partial response given 0");
  }

  // Setup for steps 6, 7, 8 and 10.
  // Steps 6 and 7 are handled by the sniffer framework.
  // Steps 8 and 10 by are handled by
  // `nsHttpChannel::DisableIsOpaqueResponseAllowedAfterSniffCheck`
  if (mLoadFlags & nsIChannel::LOAD_CALL_CONTENT_SNIFFERS) {
    mSnifferCategoryType = SnifferCategoryType::All;
  } else {
    mSnifferCategoryType = SnifferCategoryType::OpaqueResponseBlocking;
  }

  mLoadFlags |= (nsIChannel::LOAD_CALL_CONTENT_SNIFFERS |
                 nsIChannel::LOAD_MEDIA_SNIFFER_OVERRIDES_CONTENT_TYPE);

  // Install an input stream listener that performs ORB checks that depend on
  // inspecting the incoming data. It is crucial that `OnStartRequest` is called
  // on this listener either after sniffing is completed or that we skip
  // sniffing, otherwise `OpaqueResponseBlocker` will allow responses that it
  // shouldn't.
  mORB = new OpaqueResponseBlocker(mListener, this, contentType, nosniff);
  mListener = mORB;

  nsAutoCString contentEncoding;
  nsresult rv =
      mResponseHead->GetHeader(nsHttp::Content_Encoding, contentEncoding);

  if (NS_SUCCEEDED(rv) && !contentEncoding.IsEmpty()) {
    return OpaqueResponse::SniffCompressed;
  }
  mLoadFlags |= (nsIChannel::LOAD_CALL_CONTENT_SNIFFERS |
                 nsIChannel::LOAD_MEDIA_SNIFFER_OVERRIDES_CONTENT_TYPE);
  return OpaqueResponse::Sniff;
}

// The specification for ORB is currently being written:
// https://whatpr.org/fetch/1442.html#orb-algorithm
// The `opaque-response-safelist check` is implemented in:
// * `HttpBaseChannel::PerformOpaqueResponseSafelistCheckBeforeSniff`
// * `nsHttpChannel::DisableIsOpaqueResponseAllowedAfterSniffCheck`
// * `HttpBaseChannel::PerformOpaqueResponseSafelistCheckAfterSniff`
// * `OpaqueResponseBlocker::ValidateJavaScript`
OpaqueResponse HttpBaseChannel::PerformOpaqueResponseSafelistCheckAfterSniff(
    const nsACString& aContentType, bool aNoSniff) {
  PROFILER_MARKER_TEXT("ORB safelist check", NETWORK, {}, "After sniff"_ns);

  // https://whatpr.org/fetch/1442.html#orb-algorithm
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(mCachedOpaqueResponseBlockingPref);

  // Step 9
  bool isMediaRequest;
  mLoadInfo->GetIsMediaRequest(&isMediaRequest);
  if (isMediaRequest) {
    return BlockOrFilterOpaqueResponse(
        mORB, u"after sniff: media request"_ns,
        OpaqueResponseBlockedTelemetryReason::eAfterSniffMedia,
        "media request");
  }

  // Step 11
  if (aNoSniff) {
    return BlockOrFilterOpaqueResponse(
        mORB, u"after sniff: nosniff is true"_ns,
        OpaqueResponseBlockedTelemetryReason::eAfterSniffNosniff, "nosniff");
  }

  // Step 12
  if (mResponseHead &&
      (mResponseHead->Status() < 200 || mResponseHead->Status() > 299)) {
    return BlockOrFilterOpaqueResponse(
        mORB, u"after sniff: status code is not in allowed range"_ns,
        OpaqueResponseBlockedTelemetryReason::eAfterSniffStaCode,
        "status code (%d) is not allowed", mResponseHead->Status());
  }

  // Step 13
  if (!mResponseHead || aContentType.IsEmpty()) {
    LOGORB("Allowed: mimeType is failure");
    return OpaqueResponse::Allow;
  }

  // Step 14
  if (StringBeginsWith(aContentType, "image/"_ns) ||
      StringBeginsWith(aContentType, "video/"_ns) ||
      StringBeginsWith(aContentType, "audio/"_ns)) {
    return BlockOrFilterOpaqueResponse(
        mORB,
        u"after sniff: content-type declares image/video/audio, but sniffing fails"_ns,
        OpaqueResponseBlockedTelemetryReason::eAfterSniffCtFail,
        "ContentType is image/video/audio");
  }

  return OpaqueResponse::Sniff;
}

bool HttpBaseChannel::NeedOpaqueResponseAllowedCheckAfterSniff() const {
  return mORB ? mORB->IsSniffing() : false;
}

void HttpBaseChannel::BlockOpaqueResponseAfterSniff(
    const nsAString& aReason,
    const OpaqueResponseBlockedTelemetryReason aTelemetryReason) {
  MOZ_DIAGNOSTIC_ASSERT(mORB);
  LogORBError(aReason, aTelemetryReason);
  mORB->BlockResponse(this, NS_BINDING_ABORTED);
}

void HttpBaseChannel::AllowOpaqueResponseAfterSniff() {
  MOZ_DIAGNOSTIC_ASSERT(mORB);
  mORB->AllowResponse();
}

void HttpBaseChannel::SetChannelBlockedByOpaqueResponse() {
  mChannelBlockedByOpaqueResponse = true;

  RefPtr<dom::BrowsingContext> browsingContext =
      dom::BrowsingContext::GetCurrentTopByBrowserId(mBrowserId);
  if (!browsingContext) {
    return;
  }

  dom::WindowContext* windowContext = browsingContext->GetTopWindowContext();
  if (windowContext) {
    windowContext->Canonical()->SetShouldReportHasBlockedOpaqueResponse(
        mLoadInfo->InternalContentPolicyType());
  }
}

NS_IMETHODIMP
HttpBaseChannel::SetCookieHeaders(const nsTArray<nsCString>& aCookieHeaders) {
  if (mLoadFlags & LOAD_ANONYMOUS) return NS_OK;

  if (IsBrowsingContextDiscarded()) {
    return NS_OK;
  }

  // empty header isn't an error
  if (aCookieHeaders.IsEmpty()) {
    return NS_OK;
  }

  nsICookieService* cs = gHttpHandler->GetCookieService();
  NS_ENSURE_TRUE(cs, NS_ERROR_FAILURE);

  for (const nsCString& cookieHeader : aCookieHeaders) {
    nsresult rv = cs->SetCookieStringFromHttp(mURI, cookieHeader, this);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetThirdPartyFlags(uint32_t* aFlags) {
  *aFlags = LoadThirdPartyFlags();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetThirdPartyFlags(uint32_t aFlags) {
  ENSURE_CALLED_BEFORE_ASYNC_OPEN();

  StoreThirdPartyFlags(aFlags);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetForceAllowThirdPartyCookie(bool* aForce) {
  *aForce = !!(LoadThirdPartyFlags() &
               nsIHttpChannelInternal::THIRD_PARTY_FORCE_ALLOW);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetForceAllowThirdPartyCookie(bool aForce) {
  ENSURE_CALLED_BEFORE_ASYNC_OPEN();

  if (aForce) {
    StoreThirdPartyFlags(LoadThirdPartyFlags() |
                         nsIHttpChannelInternal::THIRD_PARTY_FORCE_ALLOW);
  } else {
    StoreThirdPartyFlags(LoadThirdPartyFlags() &
                         ~nsIHttpChannelInternal::THIRD_PARTY_FORCE_ALLOW);
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetCanceled(bool* aCanceled) {
  *aCanceled = mCanceled;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetChannelIsForDownload(bool* aChannelIsForDownload) {
  *aChannelIsForDownload = LoadChannelIsForDownload();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetChannelIsForDownload(bool aChannelIsForDownload) {
  StoreChannelIsForDownload(aChannelIsForDownload);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetCacheKeysRedirectChain(nsTArray<nsCString>* cacheKeys) {
  auto RedirectedCachekeys = mRedirectedCachekeys.Lock();
  auto& ref = RedirectedCachekeys.ref();
  ref = WrapUnique(cacheKeys);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLocalAddress(nsACString& addr) {
  if (mSelfAddr.raw.family == PR_AF_UNSPEC) return NS_ERROR_NOT_AVAILABLE;

  addr.SetLength(kIPv6CStrBufSize);
  mSelfAddr.ToStringBuffer(addr.BeginWriting(), kIPv6CStrBufSize);
  addr.SetLength(strlen(addr.BeginReading()));

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::TakeAllSecurityMessages(
    nsCOMArray<nsISecurityConsoleMessage>& aMessages) {
  MOZ_ASSERT(NS_IsMainThread());

  aMessages.Clear();
  for (const auto& pair : mSecurityConsoleMessages) {
    nsresult rv;
    nsCOMPtr<nsISecurityConsoleMessage> message =
        do_CreateInstance(NS_SECURITY_CONSOLE_MESSAGE_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    message->SetTag(pair.first);
    message->SetCategory(pair.second);
    aMessages.AppendElement(message);
  }

  MOZ_ASSERT(mSecurityConsoleMessages.Length() == aMessages.Length());
  mSecurityConsoleMessages.Clear();

  return NS_OK;
}

/* Please use this method with care. This can cause the message
 * queue to grow large and cause the channel to take up a lot
 * of memory. Use only static string messages and do not add
 * server side data to the queue, as that can be large.
 * Add only a limited number of messages to the queue to keep
 * the channel size down and do so only in rare erroneous situations.
 * More information can be found here:
 * https://bugzilla.mozilla.org/show_bug.cgi?id=846918
 */
nsresult HttpBaseChannel::AddSecurityMessage(
    const nsAString& aMessageTag, const nsAString& aMessageCategory) {
  MOZ_ASSERT(NS_IsMainThread());

  nsresult rv;

  // nsSecurityConsoleMessage is not thread-safe refcounted.
  // Delay the object construction until requested.
  // See TakeAllSecurityMessages()
  std::pair<nsString, nsString> pair(aMessageTag, aMessageCategory);
  mSecurityConsoleMessages.AppendElement(std::move(pair));

  nsCOMPtr<nsIConsoleService> console(
      do_GetService(NS_CONSOLESERVICE_CONTRACTID));
  if (!console) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsILoadInfo> loadInfo = LoadInfo();

  auto innerWindowID = loadInfo->GetInnerWindowID();

  nsAutoString errorText;
  rv = nsContentUtils::GetLocalizedString(
      nsContentUtils::eSECURITY_PROPERTIES,
      NS_ConvertUTF16toUTF8(aMessageTag).get(), errorText);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIScriptError> error(do_CreateInstance(NS_SCRIPTERROR_CONTRACTID));
  error->InitWithSourceURI(errorText, mURI, 0, 0, nsIScriptError::warningFlag,
                           NS_ConvertUTF16toUTF8(aMessageCategory),
                           innerWindowID);

  console->LogMessage(error);

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLocalPort(int32_t* port) {
  NS_ENSURE_ARG_POINTER(port);

  if (mSelfAddr.raw.family == PR_AF_INET) {
    *port = (int32_t)ntohs(mSelfAddr.inet.port);
  } else if (mSelfAddr.raw.family == PR_AF_INET6) {
    *port = (int32_t)ntohs(mSelfAddr.inet6.port);
  } else {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRemoteAddress(nsACString& addr) {
  if (mPeerAddr.raw.family == PR_AF_UNSPEC) return NS_ERROR_NOT_AVAILABLE;

  addr.SetLength(kIPv6CStrBufSize);
  mPeerAddr.ToStringBuffer(addr.BeginWriting(), kIPv6CStrBufSize);
  addr.SetLength(strlen(addr.BeginReading()));

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRemotePort(int32_t* port) {
  NS_ENSURE_ARG_POINTER(port);

  if (mPeerAddr.raw.family == PR_AF_INET) {
    *port = (int32_t)ntohs(mPeerAddr.inet.port);
  } else if (mPeerAddr.raw.family == PR_AF_INET6) {
    *port = (int32_t)ntohs(mPeerAddr.inet6.port);
  } else {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::HTTPUpgrade(const nsACString& aProtocolName,
                             nsIHttpUpgradeListener* aListener) {
  NS_ENSURE_ARG(!aProtocolName.IsEmpty());
  NS_ENSURE_ARG_POINTER(aListener);

  mUpgradeProtocol = aProtocolName;
  mUpgradeProtocolCallback = aListener;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetOnlyConnect(bool* aOnlyConnect) {
  NS_ENSURE_ARG_POINTER(aOnlyConnect);

  *aOnlyConnect = mCaps & NS_HTTP_CONNECT_ONLY;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetConnectOnly(bool aTlsTunnel) {
  ENSURE_CALLED_BEFORE_CONNECT();

  if (!mUpgradeProtocolCallback) {
    return NS_ERROR_FAILURE;
  }

  mCaps |= NS_HTTP_CONNECT_ONLY;
  if (aTlsTunnel) {
    mCaps |= NS_HTTP_TLS_TUNNEL;
  }
  mProxyResolveFlags = nsIProtocolProxyService::RESOLVE_PREFER_HTTPS_PROXY |
                       nsIProtocolProxyService::RESOLVE_ALWAYS_TUNNEL;
  return SetLoadFlags(nsIRequest::INHIBIT_CACHING | nsIChannel::LOAD_ANONYMOUS |
                      nsIRequest::LOAD_BYPASS_CACHE |
                      nsIChannel::LOAD_BYPASS_SERVICE_WORKER);
}

NS_IMETHODIMP
HttpBaseChannel::GetAllowSpdy(bool* aAllowSpdy) {
  NS_ENSURE_ARG_POINTER(aAllowSpdy);

  *aAllowSpdy = LoadAllowSpdy();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetAllowSpdy(bool aAllowSpdy) {
  StoreAllowSpdy(aAllowSpdy);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetAllowHttp3(bool* aAllowHttp3) {
  NS_ENSURE_ARG_POINTER(aAllowHttp3);

  *aAllowHttp3 = LoadAllowHttp3();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetAllowHttp3(bool aAllowHttp3) {
  StoreAllowHttp3(aAllowHttp3);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetAllowAltSvc(bool* aAllowAltSvc) {
  NS_ENSURE_ARG_POINTER(aAllowAltSvc);

  *aAllowAltSvc = LoadAllowAltSvc();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetAllowAltSvc(bool aAllowAltSvc) {
  StoreAllowAltSvc(aAllowAltSvc);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetBeConservative(bool* aBeConservative) {
  NS_ENSURE_ARG_POINTER(aBeConservative);

  *aBeConservative = LoadBeConservative();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetBeConservative(bool aBeConservative) {
  StoreBeConservative(aBeConservative);
  return NS_OK;
}

bool HttpBaseChannel::BypassProxy() {
  return StaticPrefs::network_proxy_allow_bypass() && LoadBypassProxy();
}

NS_IMETHODIMP
HttpBaseChannel::GetBypassProxy(bool* aBypassProxy) {
  NS_ENSURE_ARG_POINTER(aBypassProxy);

  *aBypassProxy = BypassProxy();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetBypassProxy(bool aBypassProxy) {
  if (StaticPrefs::network_proxy_allow_bypass()) {
    StoreBypassProxy(aBypassProxy);
  } else {
    NS_WARNING("bypassProxy set but network.proxy.allow_bypass is disabled");
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetIsTRRServiceChannel(bool* aIsTRRServiceChannel) {
  NS_ENSURE_ARG_POINTER(aIsTRRServiceChannel);

  *aIsTRRServiceChannel = LoadIsTRRServiceChannel();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetIsTRRServiceChannel(bool aIsTRRServiceChannel) {
  StoreIsTRRServiceChannel(aIsTRRServiceChannel);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetIsResolvedByTRR(bool* aResolvedByTRR) {
  NS_ENSURE_ARG_POINTER(aResolvedByTRR);
  *aResolvedByTRR = LoadResolvedByTRR();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetEffectiveTRRMode(nsIRequest::TRRMode* aEffectiveTRRMode) {
  *aEffectiveTRRMode = mEffectiveTRRMode;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetTrrSkipReason(nsITRRSkipReason::value* aTrrSkipReason) {
  *aTrrSkipReason = mTRRSkipReason;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetIsLoadedBySocketProcess(bool* aResult) {
  NS_ENSURE_ARG_POINTER(aResult);
  *aResult = LoadLoadedBySocketProcess();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetTlsFlags(uint32_t* aTlsFlags) {
  NS_ENSURE_ARG_POINTER(aTlsFlags);

  *aTlsFlags = mTlsFlags;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetTlsFlags(uint32_t aTlsFlags) {
  mTlsFlags = aTlsFlags;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetApiRedirectToURI(nsIURI** aResult) {
  if (!mAPIRedirectTo) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  NS_ENSURE_ARG_POINTER(aResult);
  *aResult = do_AddRef(mAPIRedirectTo->first()).take();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetResponseTimeoutEnabled(bool* aEnable) {
  if (NS_WARN_IF(!aEnable)) {
    return NS_ERROR_NULL_POINTER;
  }
  *aEnable = LoadResponseTimeoutEnabled();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetResponseTimeoutEnabled(bool aEnable) {
  StoreResponseTimeoutEnabled(aEnable);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetInitialRwin(uint32_t* aRwin) {
  if (NS_WARN_IF(!aRwin)) {
    return NS_ERROR_NULL_POINTER;
  }
  *aRwin = mInitialRwin;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetInitialRwin(uint32_t aRwin) {
  ENSURE_CALLED_BEFORE_CONNECT();
  mInitialRwin = aRwin;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::ForcePending(bool aForcePending) {
  StoreForcePending(aForcePending);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLastModifiedTime(PRTime* lastModifiedTime) {
  if (!mResponseHead) return NS_ERROR_NOT_AVAILABLE;
  uint32_t lastMod;
  nsresult rv = mResponseHead->GetLastModifiedValue(&lastMod);
  NS_ENSURE_SUCCESS(rv, rv);
  *lastModifiedTime = lastMod;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetCorsIncludeCredentials(bool* aInclude) {
  *aInclude = LoadCorsIncludeCredentials();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetCorsIncludeCredentials(bool aInclude) {
  StoreCorsIncludeCredentials(aInclude);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestMode(RequestMode* aMode) {
  *aMode = mRequestMode;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRequestMode(RequestMode aMode) {
  mRequestMode = aMode;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRedirectMode(uint32_t* aMode) {
  *aMode = mRedirectMode;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRedirectMode(uint32_t aMode) {
  mRedirectMode = aMode;
  return NS_OK;
}

namespace {

bool ContainsAllFlags(uint32_t aLoadFlags, uint32_t aMask) {
  return (aLoadFlags & aMask) == aMask;
}

}  // anonymous namespace

NS_IMETHODIMP
HttpBaseChannel::GetFetchCacheMode(uint32_t* aFetchCacheMode) {
  NS_ENSURE_ARG_POINTER(aFetchCacheMode);

  // Otherwise try to guess an appropriate cache mode from the load flags.
  if (ContainsAllFlags(mLoadFlags, INHIBIT_CACHING | LOAD_BYPASS_CACHE)) {
    *aFetchCacheMode = nsIHttpChannelInternal::FETCH_CACHE_MODE_NO_STORE;
  } else if (ContainsAllFlags(mLoadFlags, LOAD_BYPASS_CACHE)) {
    *aFetchCacheMode = nsIHttpChannelInternal::FETCH_CACHE_MODE_RELOAD;
  } else if (ContainsAllFlags(mLoadFlags, VALIDATE_ALWAYS) ||
             LoadForceValidateCacheContent()) {
    *aFetchCacheMode = nsIHttpChannelInternal::FETCH_CACHE_MODE_NO_CACHE;
  } else if (ContainsAllFlags(
                 mLoadFlags,
                 VALIDATE_NEVER | nsICachingChannel::LOAD_ONLY_FROM_CACHE)) {
    *aFetchCacheMode = nsIHttpChannelInternal::FETCH_CACHE_MODE_ONLY_IF_CACHED;
  } else if (ContainsAllFlags(mLoadFlags, VALIDATE_NEVER)) {
    *aFetchCacheMode = nsIHttpChannelInternal::FETCH_CACHE_MODE_FORCE_CACHE;
  } else {
    *aFetchCacheMode = nsIHttpChannelInternal::FETCH_CACHE_MODE_DEFAULT;
  }

  return NS_OK;
}

namespace {

void SetCacheFlags(uint32_t& aLoadFlags, uint32_t aFlags) {
  // First, clear any possible cache related flags.
  uint32_t allPossibleFlags =
      nsIRequest::INHIBIT_CACHING | nsIRequest::LOAD_BYPASS_CACHE |
      nsIRequest::VALIDATE_ALWAYS | nsIRequest::LOAD_FROM_CACHE |
      nsICachingChannel::LOAD_ONLY_FROM_CACHE;
  aLoadFlags &= ~allPossibleFlags;

  // Then set the new flags.
  aLoadFlags |= aFlags;
}

}  // anonymous namespace

NS_IMETHODIMP
HttpBaseChannel::SetFetchCacheMode(uint32_t aFetchCacheMode) {
  ENSURE_CALLED_BEFORE_CONNECT();

  // Now, set the load flags that implement each cache mode.
  switch (aFetchCacheMode) {
    case nsIHttpChannelInternal::FETCH_CACHE_MODE_DEFAULT:
      // The "default" mode means to use the http cache normally and
      // respect any http cache-control headers.  We effectively want
      // to clear our cache related load flags.
      SetCacheFlags(mLoadFlags, 0);
      break;
    case nsIHttpChannelInternal::FETCH_CACHE_MODE_NO_STORE:
      // no-store means don't consult the cache on the way to the network, and
      // don't store the response in the cache even if it's cacheable.
      SetCacheFlags(mLoadFlags, INHIBIT_CACHING | LOAD_BYPASS_CACHE);
      break;
    case nsIHttpChannelInternal::FETCH_CACHE_MODE_RELOAD:
      // reload means don't consult the cache on the way to the network, but
      // do store the response in the cache if possible.
      SetCacheFlags(mLoadFlags, LOAD_BYPASS_CACHE);
      break;
    case nsIHttpChannelInternal::FETCH_CACHE_MODE_NO_CACHE:
      // no-cache means always validate what's in the cache.
      SetCacheFlags(mLoadFlags, VALIDATE_ALWAYS);
      break;
    case nsIHttpChannelInternal::FETCH_CACHE_MODE_FORCE_CACHE:
      // force-cache means don't validate unless if the response would vary.
      SetCacheFlags(mLoadFlags, VALIDATE_NEVER);
      break;
    case nsIHttpChannelInternal::FETCH_CACHE_MODE_ONLY_IF_CACHED:
      // only-if-cached means only from cache, no network, no validation,
      // generate a network error if the document was't in the cache. The
      // privacy implications of these flags (making it fast/easy to check if
      // the user has things in their cache without any network traffic side
      // effects) are addressed in the Request constructor which
      // enforces/requires same-origin request mode.
      SetCacheFlags(mLoadFlags,
                    VALIDATE_NEVER | nsICachingChannel::LOAD_ONLY_FROM_CACHE);
      break;
  }

#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  uint32_t finalMode = 0;
  MOZ_ALWAYS_SUCCEEDS(GetFetchCacheMode(&finalMode));
  MOZ_DIAGNOSTIC_ASSERT(finalMode == aFetchCacheMode);
#endif  // MOZ_DIAGNOSTIC_ASSERT_ENABLED

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetIntegrityMetadata(const nsAString& aIntegrityMetadata) {
  mIntegrityMetadata = aIntegrityMetadata;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetIntegrityMetadata(nsAString& aIntegrityMetadata) {
  aIntegrityMetadata = mIntegrityMetadata;
  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsISupportsPriority
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::GetPriority(int32_t* value) {
  *value = mPriority;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::AdjustPriority(int32_t delta) {
  return SetPriority(mPriority + delta);
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIResumableChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::GetEntityID(nsACString& aEntityID) {
  // Don't return an entity ID for Non-GET requests which require
  // additional data
  if (!mRequestHead.IsGet()) {
    return NS_ERROR_NOT_RESUMABLE;
  }

  uint64_t size = UINT64_MAX;
  nsAutoCString etag, lastmod;
  if (mResponseHead) {
    // Don't return an entity if the server sent the following header:
    // Accept-Ranges: none
    // Not sending the Accept-Ranges header means we can still try
    // sending range requests.
    nsAutoCString acceptRanges;
    Unused << mResponseHead->GetHeader(nsHttp::Accept_Ranges, acceptRanges);
    if (!acceptRanges.IsEmpty() &&
        !nsHttp::FindToken(acceptRanges.get(), "bytes",
                           HTTP_HEADER_VALUE_SEPS)) {
      return NS_ERROR_NOT_RESUMABLE;
    }

    size = mResponseHead->TotalEntitySize();
    Unused << mResponseHead->GetHeader(nsHttp::Last_Modified, lastmod);
    Unused << mResponseHead->GetHeader(nsHttp::ETag, etag);
  }
  nsCString entityID;
  NS_EscapeURL(etag.BeginReading(), etag.Length(),
               esc_AlwaysCopy | esc_FileBaseName | esc_Forced, entityID);
  entityID.Append('/');
  entityID.AppendInt(int64_t(size));
  entityID.Append('/');
  entityID.Append(lastmod);
  // NOTE: Appending lastmod as the last part avoids having to escape it

  aEntityID = entityID;

  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIConsoleReportCollector
//-----------------------------------------------------------------------------

void HttpBaseChannel::AddConsoleReport(
    uint32_t aErrorFlags, const nsACString& aCategory,
    nsContentUtils::PropertiesFile aPropertiesFile,
    const nsACString& aSourceFileURI, uint32_t aLineNumber,
    uint32_t aColumnNumber, const nsACString& aMessageName,
    const nsTArray<nsString>& aStringParams) {
  mReportCollector->AddConsoleReport(aErrorFlags, aCategory, aPropertiesFile,
                                     aSourceFileURI, aLineNumber, aColumnNumber,
                                     aMessageName, aStringParams);

  // If this channel is already part of a loadGroup, we can flush this console
  // report immediately.
  HttpBaseChannel::MaybeFlushConsoleReports();
}

void HttpBaseChannel::FlushReportsToConsole(uint64_t aInnerWindowID,
                                            ReportAction aAction) {
  mReportCollector->FlushReportsToConsole(aInnerWindowID, aAction);
}

void HttpBaseChannel::FlushReportsToConsoleForServiceWorkerScope(
    const nsACString& aScope, ReportAction aAction) {
  mReportCollector->FlushReportsToConsoleForServiceWorkerScope(aScope, aAction);
}

void HttpBaseChannel::FlushConsoleReports(dom::Document* aDocument,
                                          ReportAction aAction) {
  mReportCollector->FlushConsoleReports(aDocument, aAction);
}

void HttpBaseChannel::FlushConsoleReports(nsILoadGroup* aLoadGroup,
                                          ReportAction aAction) {
  mReportCollector->FlushConsoleReports(aLoadGroup, aAction);
}

void HttpBaseChannel::FlushConsoleReports(
    nsIConsoleReportCollector* aCollector) {
  mReportCollector->FlushConsoleReports(aCollector);
}

void HttpBaseChannel::StealConsoleReports(
    nsTArray<net::ConsoleReportCollected>& aReports) {
  mReportCollector->StealConsoleReports(aReports);
}

void HttpBaseChannel::ClearConsoleReports() {
  mReportCollector->ClearConsoleReports();
}

bool HttpBaseChannel::IsNavigation() {
  return LoadForceMainDocumentChannel() || (mLoadFlags & LOAD_DOCUMENT_URI);
}

bool HttpBaseChannel::BypassServiceWorker() const {
  return mLoadFlags & LOAD_BYPASS_SERVICE_WORKER;
}

bool HttpBaseChannel::ShouldIntercept(nsIURI* aURI) {
  nsCOMPtr<nsINetworkInterceptController> controller;
  GetCallback(controller);
  bool shouldIntercept = false;

  if (!StaticPrefs::dom_serviceWorkers_enabled()) {
    return false;
  }

  // We should never intercept internal redirects.  The ServiceWorker code
  // can trigger interntal redirects as the result of a FetchEvent.  If
  // we re-intercept then an infinite loop can occur.
  //
  // Its also important that we do not set the LOAD_BYPASS_SERVICE_WORKER
  // flag because an internal redirect occurs.  Its possible that another
  // interception should occur after the internal redirect.  For example,
  // if the ServiceWorker chooses not to call respondWith() the channel
  // will be reset with an internal redirect.  If the request is a navigation
  // and the network then triggers a redirect its possible the new URL
  // should be intercepted again.
  //
  // Note, HSTS upgrade redirects are often treated the same as internal
  // redirects.  In this case, however, we intentionally allow interception
  // of HSTS upgrade redirects.  This matches the expected spec behavior and
  // does not run the risk of infinite loops as described above.
  bool internalRedirect =
      mLastRedirectFlags & nsIChannelEventSink::REDIRECT_INTERNAL;

  if (controller && mLoadInfo && !BypassServiceWorker() && !internalRedirect) {
    nsresult rv = controller->ShouldPrepareForIntercept(
        aURI ? aURI : mURI.get(), this, &shouldIntercept);
    if (NS_FAILED(rv)) {
      return false;
    }
  }
  return shouldIntercept;
}

void HttpBaseChannel::AddAsNonTailRequest() {
  MOZ_ASSERT(NS_IsMainThread());

  if (EnsureRequestContext()) {
    LOG((
        "HttpBaseChannel::AddAsNonTailRequest this=%p, rc=%p, already added=%d",
        this, mRequestContext.get(), (bool)LoadAddedAsNonTailRequest()));

    if (!LoadAddedAsNonTailRequest()) {
      mRequestContext->AddNonTailRequest();
      StoreAddedAsNonTailRequest(true);
    }
  }
}

void HttpBaseChannel::RemoveAsNonTailRequest() {
  MOZ_ASSERT(NS_IsMainThread());

  if (mRequestContext) {
    LOG(
        ("HttpBaseChannel::RemoveAsNonTailRequest this=%p, rc=%p, already "
         "added=%d",
         this, mRequestContext.get(), (bool)LoadAddedAsNonTailRequest()));

    if (LoadAddedAsNonTailRequest()) {
      mRequestContext->RemoveNonTailRequest();
      StoreAddedAsNonTailRequest(false);
    }
  }
}

#ifdef DEBUG
void HttpBaseChannel::AssertPrivateBrowsingId() {
  nsCOMPtr<nsILoadContext> loadContext;
  NS_QueryNotificationCallbacks(this, loadContext);

  if (!loadContext) {
    return;
  }

  // We skip testing of favicon loading here since it could be triggered by XUL
  // image which uses SystemPrincipal. The SystemPrincpal doesn't have
  // mPrivateBrowsingId.
  if (mLoadInfo->GetLoadingPrincipal() &&
      mLoadInfo->GetLoadingPrincipal()->IsSystemPrincipal() &&
      mLoadInfo->InternalContentPolicyType() ==
          nsIContentPolicy::TYPE_INTERNAL_IMAGE_FAVICON) {
    return;
  }

  OriginAttributes docShellAttrs;
  loadContext->GetOriginAttributes(docShellAttrs);
  MOZ_ASSERT(mLoadInfo->GetOriginAttributes().mPrivateBrowsingId ==
                 docShellAttrs.mPrivateBrowsingId,
             "PrivateBrowsingId values are not the same between LoadInfo and "
             "LoadContext.");
}
#endif

already_AddRefed<nsILoadInfo> HttpBaseChannel::CloneLoadInfoForRedirect(
    nsIURI* aNewURI, uint32_t aRedirectFlags) {
  // make a copy of the loadinfo, append to the redirectchain
  // this will be set on the newly created channel for the redirect target.
  nsCOMPtr<nsILoadInfo> newLoadInfo =
      static_cast<mozilla::net::LoadInfo*>(mLoadInfo.get())->Clone();

  ExtContentPolicyType contentPolicyType =
      mLoadInfo->GetExternalContentPolicyType();
  if (contentPolicyType == ExtContentPolicy::TYPE_DOCUMENT ||
      contentPolicyType == ExtContentPolicy::TYPE_SUBDOCUMENT) {
    // Reset PrincipalToInherit to a null principal. We'll credit the the
    // redirecting resource's result principal as the new principal's precursor.
    // This means that a data: URI will end up loading in a process based on the
    // redirected-from URI.
    nsCOMPtr<nsIPrincipal> redirectPrincipal;
    nsContentUtils::GetSecurityManager()->GetChannelResultPrincipal(
        this, getter_AddRefs(redirectPrincipal));
    nsCOMPtr<nsIPrincipal> nullPrincipalToInherit =
        NullPrincipal::CreateWithInheritedAttributes(redirectPrincipal);
    newLoadInfo->SetPrincipalToInherit(nullPrincipalToInherit);
  }

  bool isTopLevelDoc = newLoadInfo->GetExternalContentPolicyType() ==
                       ExtContentPolicy::TYPE_DOCUMENT;

  if (isTopLevelDoc) {
    // re-compute the origin attributes of the loadInfo if it's top-level load.
    nsCOMPtr<nsILoadContext> loadContext;
    NS_QueryNotificationCallbacks(this, loadContext);
    OriginAttributes docShellAttrs;
    if (loadContext) {
      loadContext->GetOriginAttributes(docShellAttrs);
    }

    OriginAttributes attrs = newLoadInfo->GetOriginAttributes();

    MOZ_ASSERT(
        docShellAttrs.mUserContextId == attrs.mUserContextId,
        "docshell and necko should have the same userContextId attribute.");
    MOZ_ASSERT(
        docShellAttrs.mPrivateBrowsingId == attrs.mPrivateBrowsingId,
        "docshell and necko should have the same privateBrowsingId attribute.");
    MOZ_ASSERT(docShellAttrs.mGeckoViewSessionContextId ==
                   attrs.mGeckoViewSessionContextId,
               "docshell and necko should have the same "
               "geckoViewSessionContextId attribute");

    attrs = docShellAttrs;
    attrs.SetFirstPartyDomain(true, aNewURI);
    newLoadInfo->SetOriginAttributes(attrs);

    // re-compute the upgrade insecure requests bit for document navigations
    // since it should only apply to same-origin navigations (redirects).
    // we only do this if the CSP of the triggering element (the cspToInherit)
    // uses 'upgrade-insecure-requests', otherwise UIR does not apply.
    nsCOMPtr<nsIContentSecurityPolicy> csp = newLoadInfo->GetCspToInherit();
    if (csp) {
      bool upgradeInsecureRequests = false;
      csp->GetUpgradeInsecureRequests(&upgradeInsecureRequests);
      if (upgradeInsecureRequests) {
        nsCOMPtr<nsIPrincipal> resultPrincipal =
            BasePrincipal::CreateContentPrincipal(
                aNewURI, newLoadInfo->GetOriginAttributes());
        bool isConsideredSameOriginforUIR =
            nsContentSecurityUtils::IsConsideredSameOriginForURI(
                newLoadInfo->TriggeringPrincipal(), resultPrincipal);
        static_cast<mozilla::net::LoadInfo*>(newLoadInfo.get())
            ->SetUpgradeInsecureRequests(isConsideredSameOriginforUIR);
      }
    }
  }

  // Leave empty, we want a 'clean ground' when creating the new channel.
  // This will be ensured to be either set by the protocol handler or set
  // to the redirect target URI properly after the channel creation.
  newLoadInfo->SetResultPrincipalURI(nullptr);

  bool isInternalRedirect =
      (aRedirectFlags & (nsIChannelEventSink::REDIRECT_INTERNAL |
                         nsIChannelEventSink::REDIRECT_STS_UPGRADE));

  // Reset our sandboxed null principal ID when cloning loadInfo for an
  // externally visible redirect.
  if (!isInternalRedirect) {
    // If we've redirected from http to something that isn't, clear
    // the "external" flag, as loads that now go to other apps should be
    // allowed to go ahead and not trip infinite-loop protection
    // (see bug 1717314 for context).
    if (!net::SchemeIsHttpOrHttps(aNewURI)) {
      newLoadInfo->SetLoadTriggeredFromExternal(false);
    }
    newLoadInfo->ResetSandboxedNullPrincipalID();

    if (isTopLevelDoc) {
      // Reset HTTPS-first and -only status on http redirect. To not
      // unexpectedly downgrade requests that weren't upgraded via HTTPS-First
      // (Bug 1904238).
      Unused << newLoadInfo->SetHttpsOnlyStatus(
          nsILoadInfo::HTTPS_ONLY_UNINITIALIZED);

      // Reset schemeless status flag to prevent schemeless HTTPS-First from
      // repeatedly trying to upgrade loads that get downgraded again from the
      // server by a redirect (Bug 1937386).
      Unused << newLoadInfo->SetSchemelessInput(
          nsILoadInfo::SchemelessInputTypeUnset);
    }
  }

  newLoadInfo->AppendRedirectHistoryEntry(this, isInternalRedirect);

  return newLoadInfo.forget();
}

//-----------------------------------------------------------------------------
// nsHttpChannel::nsITraceableChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::SetNewListener(nsIStreamListener* aListener,
                                bool aMustApplyContentConversion,
                                nsIStreamListener** _retval) {
  LOG((
      "HttpBaseChannel::SetNewListener [this=%p, mListener=%p, newListener=%p]",
      this, mListener.get(), aListener));

  if (!LoadTracingEnabled()) return NS_ERROR_FAILURE;

  NS_ENSURE_STATE(mListener);
  NS_ENSURE_ARG_POINTER(aListener);

  nsCOMPtr<nsIStreamListener> wrapper = new nsStreamListenerWrapper(mListener);

  wrapper.forget(_retval);
  mListener = aListener;
  if (aMustApplyContentConversion) {
    StoreListenerRequiresContentConversion(true);
  }
  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel helpers
//-----------------------------------------------------------------------------

void HttpBaseChannel::ReleaseListeners() {
  MOZ_ASSERT(mCurrentThread->IsOnCurrentThread(),
             "Should only be called on the current thread");

  mListener = nullptr;
  mCallbacks = nullptr;
  mProgressSink = nullptr;
  mCompressListener = nullptr;
  mORB = nullptr;
}

void HttpBaseChannel::DoNotifyListener() {
  LOG(("HttpBaseChannel::DoNotifyListener this=%p", this));

  // In case nsHttpChannel::OnStartRequest wasn't called (e.g. due to flag
  // LOAD_ONLY_IF_MODIFIED) we want to set AfterOnStartRequestBegun to true
  // before notifying listener.
  if (!LoadAfterOnStartRequestBegun()) {
    StoreAfterOnStartRequestBegun(true);
  }

  if (mListener && !LoadOnStartRequestCalled()) {
    nsCOMPtr<nsIStreamListener> listener = mListener;
    StoreOnStartRequestCalled(true);
    listener->OnStartRequest(this);
  }
  StoreOnStartRequestCalled(true);

  // Make sure IsPending is set to false. At this moment we are done from
  // the point of view of our consumer and we have to report our self
  // as not-pending.
  StoreIsPending(false);

  // notify "http-on-before-stop-request" observers
  gHttpHandler->OnBeforeStopRequest(this);

  if (mListener && !LoadOnStopRequestCalled()) {
    nsCOMPtr<nsIStreamListener> listener = mListener;
    StoreOnStopRequestCalled(true);
    listener->OnStopRequest(this, mStatus);
  }
  StoreOnStopRequestCalled(true);

  // notify "http-on-stop-request" observers
  gHttpHandler->OnStopRequest(this);

  // This channel has finished its job, potentially release any tail-blocked
  // requests with this.
  RemoveAsNonTailRequest();

  // We have to make sure to drop the references to listeners and callbacks
  // no longer needed.
  ReleaseListeners();

  DoNotifyListenerCleanup();

  // If this is a navigation, then we must let the docshell flush the reports
  // to the console later.  The LoadDocument() is pointing at the detached
  // document that started the navigation.  We want to show the reports on the
  // new document.  Otherwise the console is wiped and the user never sees
  // the information.
  if (!IsNavigation()) {
    if (mLoadGroup) {
      FlushConsoleReports(mLoadGroup);
    } else {
      RefPtr<dom::Document> doc;
      mLoadInfo->GetLoadingDocument(getter_AddRefs(doc));
      FlushConsoleReports(doc);
    }
  }
}

void HttpBaseChannel::AddCookiesToRequest() {
  if (mLoadFlags & LOAD_ANONYMOUS) {
    return;
  }

  bool useCookieService = (XRE_IsParentProcess());
  nsAutoCString cookie;
  if (useCookieService) {
    nsICookieService* cs = gHttpHandler->GetCookieService();
    if (cs) {
      cs->GetCookieStringFromHttp(mURI, this, cookie);
    }

    if (cookie.IsEmpty()) {
      cookie = mUserSetCookieHeader;
    } else if (!mUserSetCookieHeader.IsEmpty()) {
      cookie.AppendLiteral("; ");
      cookie.Append(mUserSetCookieHeader);
    }
  } else {
    cookie = mUserSetCookieHeader;
  }

  // If we are in the child process, we want the parent seeing any
  // cookie headers that might have been set by SetRequestHeader()
  SetRequestHeader(nsHttp::Cookie.val(), cookie, false);
}

/* static */
void HttpBaseChannel::PropagateReferenceIfNeeded(
    nsIURI* aURI, nsCOMPtr<nsIURI>& aRedirectURI) {
  bool hasRef = false;
  nsresult rv = aRedirectURI->GetHasRef(&hasRef);
  if (NS_SUCCEEDED(rv) && !hasRef) {
    nsAutoCString ref;
    aURI->GetRef(ref);
    if (!ref.IsEmpty()) {
      // NOTE: SetRef will fail if mRedirectURI is immutable
      // (e.g. an about: URI)... Oh well.
      Unused << NS_MutateURI(aRedirectURI).SetRef(ref).Finalize(aRedirectURI);
    }
  }
}

bool HttpBaseChannel::ShouldRewriteRedirectToGET(
    uint32_t httpStatus, nsHttpRequestHead::ParsedMethodType method) {
  // for 301 and 302, only rewrite POST
  if (httpStatus == 301 || httpStatus == 302) {
    return method == nsHttpRequestHead::kMethod_Post;
  }

  // rewrite for 303 unless it was HEAD
  if (httpStatus == 303) return method != nsHttpRequestHead::kMethod_Head;

  // otherwise, such as for 307, do not rewrite
  return false;
}

NS_IMETHODIMP
HttpBaseChannel::ShouldStripRequestBodyHeader(const nsACString& aMethod,
                                              bool* aResult) {
  *aResult = false;
  uint32_t httpStatus = 0;
  if (NS_FAILED(GetResponseStatus(&httpStatus))) {
    return NS_OK;
  }

  nsAutoCString method(aMethod);
  nsHttpRequestHead::ParsedMethodType parsedMethod;
  nsHttpRequestHead::ParseMethod(method, parsedMethod);
  // Fetch 4.4.11, which is slightly different than the perserved method
  // algrorithm: strip request-body-header for GET->GET redirection for 303.
  *aResult =
      ShouldRewriteRedirectToGET(httpStatus, parsedMethod) &&
      !(httpStatus == 303 && parsedMethod == nsHttpRequestHead::kMethod_Get);

  return NS_OK;
}

HttpBaseChannel::ReplacementChannelConfig
HttpBaseChannel::CloneReplacementChannelConfig(bool aPreserveMethod,
                                               uint32_t aRedirectFlags,
                                               ReplacementReason aReason) {
  ReplacementChannelConfig config;
  config.redirectFlags = aRedirectFlags;
  config.classOfService = mClassOfService;

  if (mPrivateBrowsingOverriden) {
    config.privateBrowsing = Some(mPrivateBrowsing);
  }

  if (mReferrerInfo) {
    // When cloning for a document channel replacement (parent process
    // copying values for a new content process channel), this happens after
    // OnStartRequest so we have the headers for the response available.
    // We don't want to apply them to the referrer for the channel though,
    // since that is the referrer for the current document, and the header
    // should only apply to navigations from the current document.
    if (aReason == ReplacementReason::DocumentChannel) {
      config.referrerInfo = mReferrerInfo;
    } else {
      dom::ReferrerPolicy referrerPolicy = dom::ReferrerPolicy::_empty;
      nsAutoCString tRPHeaderCValue;
      Unused << GetResponseHeader("referrer-policy"_ns, tRPHeaderCValue);
      NS_ConvertUTF8toUTF16 tRPHeaderValue(tRPHeaderCValue);

      if (!tRPHeaderValue.IsEmpty()) {
        referrerPolicy =
            dom::ReferrerInfo::ReferrerPolicyFromHeaderString(tRPHeaderValue);
      }

      // In case we are here because an upgrade happened through mixed content
      // upgrading, CSP upgrade-insecure-requests, HTTPS-Only or HTTPS-First, we
      // have to recalculate the referrer based on the original referrer to
      // account for the different scheme. This does NOT apply to HSTS.
      // See Bug 1857894 and order of https://fetch.spec.whatwg.org/#main-fetch.
      // Otherwise, if we have a new referrer policy, we want to recalculate the
      // referrer based on the old computed referrer (Bug 1678545).
      bool wasNonHSTSUpgrade =
          (aRedirectFlags & nsIChannelEventSink::REDIRECT_STS_UPGRADE) &&
          (!mLoadInfo->GetHstsStatus());
      if (wasNonHSTSUpgrade) {
        nsCOMPtr<nsIURI> referrer = mReferrerInfo->GetOriginalReferrer();
        config.referrerInfo =
            new dom::ReferrerInfo(referrer, mReferrerInfo->ReferrerPolicy(),
                                  mReferrerInfo->GetSendReferrer());
      } else if (referrerPolicy != dom::ReferrerPolicy::_empty) {
        nsCOMPtr<nsIURI> referrer = mReferrerInfo->GetComputedReferrer();
        config.referrerInfo = new dom::ReferrerInfo(
            referrer, referrerPolicy, mReferrerInfo->GetSendReferrer());
      } else {
        config.referrerInfo = mReferrerInfo;
      }
    }
  }

  nsCOMPtr<nsITimedChannel> oldTimedChannel(
      do_QueryInterface(static_cast<nsIHttpChannel*>(this)));
  if (oldTimedChannel) {
    config.timedChannelInfo = Some(dom::TimedChannelInfo());
    config.timedChannelInfo->redirectCount() = mRedirectCount;
    config.timedChannelInfo->internalRedirectCount() = mInternalRedirectCount;
    config.timedChannelInfo->asyncOpen() = mAsyncOpenTime;
    config.timedChannelInfo->channelCreation() = mChannelCreationTimestamp;
    config.timedChannelInfo->redirectStart() = mRedirectStartTimeStamp;
    config.timedChannelInfo->redirectEnd() = mRedirectEndTimeStamp;
    config.timedChannelInfo->initiatorType() = mInitiatorType;
    config.timedChannelInfo->allRedirectsSameOrigin() =
        LoadAllRedirectsSameOrigin();
    config.timedChannelInfo->allRedirectsPassTimingAllowCheck() =
        LoadAllRedirectsPassTimingAllowCheck();
    // Execute the timing allow check to determine whether
    // to report the redirect timing info
    nsCOMPtr<nsILoadInfo> loadInfo = LoadInfo();
    // TYPE_DOCUMENT loads don't have a loadingPrincipal, so we can't set
    // AllRedirectsPassTimingAllowCheck on them.
    if (loadInfo->GetExternalContentPolicyType() !=
        ExtContentPolicy::TYPE_DOCUMENT) {
      nsCOMPtr<nsIPrincipal> principal = loadInfo->GetLoadingPrincipal();
      config.timedChannelInfo->timingAllowCheckForPrincipal() =
          Some(oldTimedChannel->TimingAllowCheck(principal));
    }

    config.timedChannelInfo->allRedirectsPassTimingAllowCheck() =
        LoadAllRedirectsPassTimingAllowCheck();
    config.timedChannelInfo->launchServiceWorkerStart() =
        mLaunchServiceWorkerStart;
    config.timedChannelInfo->launchServiceWorkerEnd() = mLaunchServiceWorkerEnd;
    config.timedChannelInfo->dispatchFetchEventStart() =
        mDispatchFetchEventStart;
    config.timedChannelInfo->dispatchFetchEventEnd() = mDispatchFetchEventEnd;
    config.timedChannelInfo->handleFetchEventStart() = mHandleFetchEventStart;
    config.timedChannelInfo->handleFetchEventEnd() = mHandleFetchEventEnd;
    config.timedChannelInfo->responseStart() =
        mTransactionTimings.responseStart;
    config.timedChannelInfo->responseEnd() = mTransactionTimings.responseEnd;
  }

  if (aPreserveMethod) {
    // since preserveMethod is true, we need to ensure that the appropriate
    // request method gets set on the channel, regardless of whether or not
    // we set the upload stream above. This means SetRequestMethod() will
    // be called twice if ExplicitSetUploadStream() gets called above.

    nsAutoCString method;
    mRequestHead.Method(method);
    config.method = Some(method);

    if (mUploadStream) {
      // rewind upload stream
      nsCOMPtr<nsISeekableStream> seekable = do_QueryInterface(mUploadStream);
      if (seekable) {
        seekable->Seek(nsISeekableStream::NS_SEEK_SET, 0);
      }
      config.uploadStream = mUploadStream;
    }
    config.uploadStreamLength = mReqContentLength;
    config.uploadStreamHasHeaders = LoadUploadStreamHasHeaders();

    nsAutoCString contentType;
    nsresult rv = mRequestHead.GetHeader(nsHttp::Content_Type, contentType);
    if (NS_SUCCEEDED(rv)) {
      config.contentType = Some(contentType);
    }

    nsAutoCString contentLength;
    rv = mRequestHead.GetHeader(nsHttp::Content_Length, contentLength);
    if (NS_SUCCEEDED(rv)) {
      config.contentLength = Some(contentLength);
    }
  }

  return config;
}

/* static */ void HttpBaseChannel::ConfigureReplacementChannel(
    nsIChannel* newChannel, const ReplacementChannelConfig& config,
    ReplacementReason aReason) {
  nsCOMPtr<nsIClassOfService> cos(do_QueryInterface(newChannel));
  if (cos) {
    cos->SetClassOfService(config.classOfService);
  }

  // Try to preserve the privacy bit if it has been overridden
  if (config.privateBrowsing) {
    nsCOMPtr<nsIPrivateBrowsingChannel> newPBChannel =
        do_QueryInterface(newChannel);
    if (newPBChannel) {
      newPBChannel->SetPrivate(*config.privateBrowsing);
    }
  }

  // Transfer the timing data (if we are dealing with an nsITimedChannel).
  nsCOMPtr<nsITimedChannel> newTimedChannel(do_QueryInterface(newChannel));
  if (config.timedChannelInfo && newTimedChannel) {
    // If we're an internal redirect, or a document channel replacement,
    // then we shouldn't record any new timing for this and just copy
    // over the existing values.
    bool shouldHideTiming = aReason != ReplacementReason::Redirect;
    if (shouldHideTiming) {
      newTimedChannel->SetRedirectCount(
          config.timedChannelInfo->redirectCount());
      int32_t newCount = config.timedChannelInfo->internalRedirectCount() + 1;
      newTimedChannel->SetInternalRedirectCount(std::max(
          newCount, static_cast<int32_t>(
                        config.timedChannelInfo->internalRedirectCount())));
    } else {
      int32_t newCount = config.timedChannelInfo->redirectCount() + 1;
      newTimedChannel->SetRedirectCount(std::max(
          newCount,
          static_cast<int32_t>(config.timedChannelInfo->redirectCount())));
      newTimedChannel->SetInternalRedirectCount(
          config.timedChannelInfo->internalRedirectCount());
    }

    if (shouldHideTiming) {
      if (!config.timedChannelInfo->channelCreation().IsNull()) {
        newTimedChannel->SetChannelCreation(
            config.timedChannelInfo->channelCreation());
      }

      if (!config.timedChannelInfo->asyncOpen().IsNull()) {
        newTimedChannel->SetAsyncOpen(config.timedChannelInfo->asyncOpen());
      }
    }

    // If the RedirectStart is null, we will use the AsyncOpen value of the
    // previous channel (this is the first redirect in the redirects chain).
    if (config.timedChannelInfo->redirectStart().IsNull()) {
      // Only do this for real redirects.  Internal redirects should be hidden.
      if (!shouldHideTiming) {
        newTimedChannel->SetRedirectStart(config.timedChannelInfo->asyncOpen());
      }
    } else {
      newTimedChannel->SetRedirectStart(
          config.timedChannelInfo->redirectStart());
    }

    // For internal redirects just propagate the last redirect end time
    // forward.  Otherwise the new redirect end time is the last response
    // end time.
    TimeStamp newRedirectEnd;
    if (shouldHideTiming) {
      newRedirectEnd = config.timedChannelInfo->redirectEnd();
    } else if (!config.timedChannelInfo->responseEnd().IsNull()) {
      newRedirectEnd = config.timedChannelInfo->responseEnd();
    } else {
      newRedirectEnd = TimeStamp::Now();
    }
    newTimedChannel->SetRedirectEnd(newRedirectEnd);

    newTimedChannel->SetInitiatorType(config.timedChannelInfo->initiatorType());

    nsCOMPtr<nsILoadInfo> loadInfo = newChannel->LoadInfo();
    MOZ_ASSERT(loadInfo);

    newTimedChannel->SetAllRedirectsSameOrigin(
        config.timedChannelInfo->allRedirectsSameOrigin());

    if (config.timedChannelInfo->timingAllowCheckForPrincipal()) {
      newTimedChannel->SetAllRedirectsPassTimingAllowCheck(
          config.timedChannelInfo->allRedirectsPassTimingAllowCheck() &&
          *config.timedChannelInfo->timingAllowCheckForPrincipal());
    }

    // Propagate service worker measurements across redirects.  The
    // PeformanceResourceTiming.workerStart API expects to see the
    // worker start time after a redirect.
    newTimedChannel->SetLaunchServiceWorkerStart(
        config.timedChannelInfo->launchServiceWorkerStart());
    newTimedChannel->SetLaunchServiceWorkerEnd(
        config.timedChannelInfo->launchServiceWorkerEnd());
    newTimedChannel->SetDispatchFetchEventStart(
        config.timedChannelInfo->dispatchFetchEventStart());
    newTimedChannel->SetDispatchFetchEventEnd(
        config.timedChannelInfo->dispatchFetchEventEnd());
    newTimedChannel->SetHandleFetchEventStart(
        config.timedChannelInfo->handleFetchEventStart());
    newTimedChannel->SetHandleFetchEventEnd(
        config.timedChannelInfo->handleFetchEventEnd());
  }

  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(newChannel);
  if (!httpChannel) {
    return;  // no other options to set
  }

  if (config.uploadStream) {
    nsCOMPtr<nsIUploadChannel> uploadChannel = do_QueryInterface(httpChannel);
    nsCOMPtr<nsIUploadChannel2> uploadChannel2 = do_QueryInterface(httpChannel);
    if (uploadChannel2 || uploadChannel) {
      // replicate original call to SetUploadStream...
      if (uploadChannel2) {
        const nsACString& ctype =
            config.contentType ? *config.contentType : VoidCString();
        // If header is not present mRequestHead.HasHeaderValue will truncated
        // it.  But we want to end up with a void string, not an empty string,
        // because ExplicitSetUploadStream treats the former as "no header" and
        // the latter as "header with empty string value".

        const nsACString& method =
            config.method ? *config.method : VoidCString();

        uploadChannel2->ExplicitSetUploadStream(
            config.uploadStream, ctype, config.uploadStreamLength, method,
            config.uploadStreamHasHeaders);
      } else {
        if (config.uploadStreamHasHeaders) {
          uploadChannel->SetUploadStream(config.uploadStream, ""_ns,
                                         config.uploadStreamLength);
        } else {
          nsAutoCString ctype;
          if (config.contentType) {
            ctype = *config.contentType;
          } else {
            ctype = "application/octet-stream"_ns;
          }
          if (config.contentLength && !config.contentLength->IsEmpty()) {
            uploadChannel->SetUploadStream(
                config.uploadStream, ctype,
                nsCRT::atoll(config.contentLength->get()));
          }
        }
      }
    }
  }

  if (config.referrerInfo) {
    DebugOnly<nsresult> success{};
    success = httpChannel->SetReferrerInfo(config.referrerInfo);
    MOZ_ASSERT(NS_SUCCEEDED(success));
  }

  if (config.method) {
    DebugOnly<nsresult> rv = httpChannel->SetRequestMethod(*config.method);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }
}

HttpBaseChannel::ReplacementChannelConfig::ReplacementChannelConfig(
    const dom::ReplacementChannelConfigInit& aInit) {
  redirectFlags = aInit.redirectFlags();
  classOfService = aInit.classOfService();
  privateBrowsing = aInit.privateBrowsing();
  method = aInit.method();
  referrerInfo = aInit.referrerInfo();
  timedChannelInfo = aInit.timedChannelInfo();
  uploadStream = aInit.uploadStream();
  uploadStreamLength = aInit.uploadStreamLength();
  uploadStreamHasHeaders = aInit.uploadStreamHasHeaders();
  contentType = aInit.contentType();
  contentLength = aInit.contentLength();
}

dom::ReplacementChannelConfigInit
HttpBaseChannel::ReplacementChannelConfig::Serialize() {
  dom::ReplacementChannelConfigInit config;
  config.redirectFlags() = redirectFlags;
  config.classOfService() = classOfService;
  config.privateBrowsing() = privateBrowsing;
  config.method() = method;
  config.referrerInfo() = referrerInfo;
  config.timedChannelInfo() = timedChannelInfo;
  config.uploadStream() =
      uploadStream ? RemoteLazyInputStream::WrapStream(uploadStream) : nullptr;
  config.uploadStreamLength() = uploadStreamLength;
  config.uploadStreamHasHeaders() = uploadStreamHasHeaders;
  config.contentType() = contentType;
  config.contentLength() = contentLength;

  return config;
}

nsresult HttpBaseChannel::SetupReplacementChannel(nsIURI* newURI,
                                                  nsIChannel* newChannel,
                                                  bool preserveMethod,
                                                  uint32_t redirectFlags) {
  nsresult rv;

  LOG(
      ("HttpBaseChannel::SetupReplacementChannel "
       "[this=%p newChannel=%p preserveMethod=%d]",
       this, newChannel, preserveMethod));

  // Ensure the channel's loadInfo's result principal URI so that it's
  // either non-null or updated to the redirect target URI.
  // We must do this because in case the loadInfo's result principal URI
  // is null, it would be taken from OriginalURI of the channel.  But we
  // overwrite it with the whole redirect chain first URI before opening
  // the target channel, hence the information would be lost.
  // If the protocol handler that created the channel wants to use
  // the originalURI of the channel as the principal URI, this fulfills
  // that request - newURI is the original URI of the channel.
  nsCOMPtr<nsILoadInfo> newLoadInfo = newChannel->LoadInfo();
  nsCOMPtr<nsIURI> resultPrincipalURI;
  rv = newLoadInfo->GetResultPrincipalURI(getter_AddRefs(resultPrincipalURI));
  NS_ENSURE_SUCCESS(rv, rv);
  if (!resultPrincipalURI) {
    rv = newLoadInfo->SetResultPrincipalURI(newURI);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsLoadFlags loadFlags = mLoadFlags;
  loadFlags |= LOAD_REPLACE;

  // if the original channel was using SSL and this channel is not using
  // SSL, then no need to inhibit persistent caching.  however, if the
  // original channel was not using SSL and has INHIBIT_PERSISTENT_CACHING
  // set, then allow the flag to apply to the redirected channel as well.
  // since we force set INHIBIT_PERSISTENT_CACHING on all HTTPS channels,
  // we only need to check if the original channel was using SSL.
  if (mURI->SchemeIs("https")) {
    loadFlags &= ~INHIBIT_PERSISTENT_CACHING;
  }

  newChannel->SetLoadFlags(loadFlags);

  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(newChannel);

  ReplacementReason redirectType =
      redirectFlags & (nsIChannelEventSink::REDIRECT_INTERNAL |
                       nsIChannelEventSink::REDIRECT_TRANSPARENT)
          ? ReplacementReason::InternalRedirect
          : ReplacementReason::Redirect;
  ReplacementChannelConfig config = CloneReplacementChannelConfig(
      preserveMethod, redirectFlags, redirectType);
  ConfigureReplacementChannel(newChannel, config, redirectType);

  // Check whether or not this was a cross-domain redirect.
  nsCOMPtr<nsITimedChannel> newTimedChannel(do_QueryInterface(newChannel));
  bool sameOriginWithOriginalUri = SameOriginWithOriginalUri(newURI);
  if (config.timedChannelInfo && newTimedChannel) {
    newTimedChannel->SetAllRedirectsSameOrigin(
        config.timedChannelInfo->allRedirectsSameOrigin() &&
        sameOriginWithOriginalUri);
  }

  newChannel->SetLoadGroup(mLoadGroup);
  newChannel->SetNotificationCallbacks(mCallbacks);
  // TODO: create tests for cross-origin redirect in bug 1662896.
  if (sameOriginWithOriginalUri) {
    newChannel->SetContentDisposition(mContentDispositionHint);
    if (mContentDispositionFilename) {
      newChannel->SetContentDispositionFilename(*mContentDispositionFilename);
    }
  }

  if (!httpChannel) return NS_OK;  // no other options to set

  // Preserve the CORS preflight information.
  nsCOMPtr<nsIHttpChannelInternal> httpInternal = do_QueryInterface(newChannel);
  if (httpInternal) {
    httpInternal->SetLastRedirectFlags(redirectFlags);

    if (LoadRequireCORSPreflight()) {
      httpInternal->SetCorsPreflightParameters(mUnsafeHeaders, false, false);
    }
  }

  // convey the LoadAllowSTS() flags
  rv = httpChannel->SetAllowSTS(LoadAllowSTS());
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  // convey the Accept header value
  {
    nsAutoCString oldAcceptValue;
    nsresult hasHeader = mRequestHead.GetHeader(nsHttp::Accept, oldAcceptValue);
    if (NS_SUCCEEDED(hasHeader)) {
      rv = httpChannel->SetRequestHeader("Accept"_ns, oldAcceptValue, false);
      MOZ_ASSERT(NS_SUCCEEDED(rv));
    }
  }

  // convey the User-Agent header value
  // since we might be setting custom user agent from DevTools.
  if (httpInternal && mRequestMode == RequestMode::No_cors &&
      redirectType == ReplacementReason::Redirect) {
    nsAutoCString oldUserAgent;
    nsresult hasHeader =
        mRequestHead.GetHeader(nsHttp::User_Agent, oldUserAgent);
    if (NS_SUCCEEDED(hasHeader)) {
      rv = httpChannel->SetRequestHeader("User-Agent"_ns, oldUserAgent, false);
      MOZ_ASSERT(NS_SUCCEEDED(rv));
    }
  }

  // convery the IsUserAgentHeaderModified value.
  if (httpInternal) {
    rv = httpInternal->SetIsUserAgentHeaderModified(
        LoadIsUserAgentHeaderModified());
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }

  // share the request context - see bug 1236650
  rv = httpChannel->SetRequestContextID(mRequestContextID);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  // When on the parent process, the channel can't attempt to get it itself.
  // When on the child process, it would be waste to query it again.
  rv = httpChannel->SetBrowserId(mBrowserId);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  // Not setting this flag would break carrying permissions down to the child
  // process when the channel is artificially forced to be a main document load.
  rv = httpChannel->SetIsMainDocumentChannel(LoadForceMainDocumentChannel());
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  // Preserve the loading order
  nsCOMPtr<nsISupportsPriority> p = do_QueryInterface(newChannel);
  if (p) {
    p->SetPriority(mPriority);
  }

  if (httpInternal) {
    // Convey third party cookie, conservative, and spdy flags.
    rv = httpInternal->SetThirdPartyFlags(LoadThirdPartyFlags());
    MOZ_ASSERT(NS_SUCCEEDED(rv));
    rv = httpInternal->SetAllowSpdy(LoadAllowSpdy());
    MOZ_ASSERT(NS_SUCCEEDED(rv));
    rv = httpInternal->SetAllowHttp3(LoadAllowHttp3());
    MOZ_ASSERT(NS_SUCCEEDED(rv));
    rv = httpInternal->SetAllowAltSvc(LoadAllowAltSvc());
    MOZ_ASSERT(NS_SUCCEEDED(rv));
    rv = httpInternal->SetBeConservative(LoadBeConservative());
    MOZ_ASSERT(NS_SUCCEEDED(rv));
    rv = httpInternal->SetIsTRRServiceChannel(LoadIsTRRServiceChannel());
    MOZ_ASSERT(NS_SUCCEEDED(rv));
    rv = httpInternal->SetTlsFlags(mTlsFlags);
    MOZ_ASSERT(NS_SUCCEEDED(rv));

    // Ensure the type of realChannel involves all types it may redirect to.
    // Such as nsHttpChannel and InterceptedChannel.
    // Even thought InterceptedChannel itself doesn't require these information,
    // it may still be necessary for the following redirections.
    // E.g. nsHttpChannel -> InterceptedChannel -> nsHttpChannel
    RefPtr<HttpBaseChannel> realChannel;
    CallQueryInterface(newChannel, realChannel.StartAssignment());
    if (realChannel) {
      realChannel->SetTopWindowURI(mTopWindowURI);

      realChannel->StoreTaintedOriginFlag(
          ShouldTaintReplacementChannelOrigin(newChannel, redirectFlags));
    }

    // update the DocumentURI indicator since we are being redirected.
    // if this was a top-level document channel, then the new channel
    // should have its mDocumentURI point to newURI; otherwise, we
    // just need to pass along our mDocumentURI to the new channel.
    if (newURI && (mURI == mDocumentURI)) {
      rv = httpInternal->SetDocumentURI(newURI);
    } else {
      rv = httpInternal->SetDocumentURI(mDocumentURI);
    }
    MOZ_ASSERT(NS_SUCCEEDED(rv));

    // if there is a chain of keys for redirect-responses we transfer it to
    // the new channel (see bug #561276)
    {
      auto redirectedCachekeys = mRedirectedCachekeys.Lock();
      auto& ref = redirectedCachekeys.ref();
      if (ref) {
        LOG(
            ("HttpBaseChannel::SetupReplacementChannel "
             "[this=%p] transferring chain of redirect cache-keys",
             this));
        rv = httpInternal->SetCacheKeysRedirectChain(ref.release());
        MOZ_ASSERT(NS_SUCCEEDED(rv));
      }
    }

    // Preserve Request mode.
    rv = httpInternal->SetRequestMode(mRequestMode);
    MOZ_ASSERT(NS_SUCCEEDED(rv));

    // Preserve Redirect mode flag.
    rv = httpInternal->SetRedirectMode(mRedirectMode);
    MOZ_ASSERT(NS_SUCCEEDED(rv));

    // Preserve Integrity metadata.
    rv = httpInternal->SetIntegrityMetadata(mIntegrityMetadata);
    MOZ_ASSERT(NS_SUCCEEDED(rv));

    httpInternal->SetAltDataForChild(LoadAltDataForChild());
    if (LoadDisableAltDataCache()) {
      httpInternal->DisableAltDataCache();
    }
  }

  // transfer any properties
  nsCOMPtr<nsIWritablePropertyBag> bag(do_QueryInterface(newChannel));
  if (bag) {
    for (const auto& entry : mPropertyHash) {
      bag->SetProperty(entry.GetKey(), entry.GetWeak());
    }
  }

  // Pass the preferred alt-data type on to the new channel.
  nsCOMPtr<nsICacheInfoChannel> cacheInfoChan(do_QueryInterface(newChannel));
  if (cacheInfoChan) {
    for (auto& data : mPreferredCachedAltDataTypes) {
      cacheInfoChan->PreferAlternativeDataType(data.type(), data.contentType(),
                                               data.deliverAltData());
    }

    if (LoadForceValidateCacheContent()) {
      Unused << cacheInfoChan->SetForceValidateCacheContent(true);
    }
  }

  if (redirectFlags & (nsIChannelEventSink::REDIRECT_INTERNAL |
                       nsIChannelEventSink::REDIRECT_STS_UPGRADE)) {
    // Copy non-origin related headers to the new channel.
    nsCOMPtr<nsIHttpHeaderVisitor> visitor =
        new AddHeadersToChannelVisitor(httpChannel);
    rv = mRequestHead.VisitHeaders(visitor);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }

  // we need to strip Authentication headers for cross-origin requests
  // Ref: https://fetch.spec.whatwg.org/#http-redirect-fetch
  nsAutoCString authHeader;
  if (NS_SUCCEEDED(
          httpChannel->GetRequestHeader("Authorization"_ns, authHeader)) &&
      NS_ShouldRemoveAuthHeaderOnRedirect(static_cast<nsIChannel*>(this),
                                          newChannel, redirectFlags)) {
    rv = httpChannel->SetRequestHeader("Authorization"_ns, ""_ns, false);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }

  return NS_OK;
}

// check whether the new channel is of same origin as the current channel
bool HttpBaseChannel::IsNewChannelSameOrigin(nsIChannel* aNewChannel) {
  bool isSameOrigin = false;
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();

  if (!ssm) {
    return false;
  }

  nsCOMPtr<nsIURI> newURI;
  NS_GetFinalChannelURI(aNewChannel, getter_AddRefs(newURI));

  nsresult rv = ssm->CheckSameOriginURI(newURI, mURI, false, false);
  if (NS_SUCCEEDED(rv)) {
    isSameOrigin = true;
  }

  return isSameOrigin;
}

bool HttpBaseChannel::ShouldTaintReplacementChannelOrigin(
    nsIChannel* aNewChannel, uint32_t aRedirectFlags) {
  if (LoadTaintedOriginFlag()) {
    return true;
  }

  if (NS_IsInternalSameURIRedirect(this, aNewChannel, aRedirectFlags) ||
      NS_IsHSTSUpgradeRedirect(this, aNewChannel, aRedirectFlags)) {
    return false;
  }

  // If new channel is not of same origin we need to taint unless
  // mURI <-> mOriginalURI/LoadingPrincipal are same origin.
  if (IsNewChannelSameOrigin(aNewChannel)) {
    return false;
  }

  nsresult rv;

  if (mLoadInfo->GetLoadingPrincipal()) {
    bool sameOrigin = false;
    rv = mLoadInfo->GetLoadingPrincipal()->IsSameOrigin(mURI, &sameOrigin);
    if (NS_FAILED(rv)) {
      return true;
    }
    return !sameOrigin;
  }
  if (!mOriginalURI) {
    return true;
  }

  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  if (!ssm) {
    return true;
  }

  rv = ssm->CheckSameOriginURI(mOriginalURI, mURI, false, false);
  return NS_FAILED(rv);
}

// Redirect Tracking
bool HttpBaseChannel::SameOriginWithOriginalUri(nsIURI* aURI) {
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  bool isPrivateWin = mLoadInfo->GetOriginAttributes().IsPrivateBrowsing();
  nsresult rv =
      ssm->CheckSameOriginURI(aURI, mOriginalURI, false, isPrivateWin);
  return (NS_SUCCEEDED(rv));
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIClassifiedChannel

NS_IMETHODIMP
HttpBaseChannel::GetMatchedList(nsACString& aList) {
  aList = mMatchedList;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetMatchedProvider(nsACString& aProvider) {
  aProvider = mMatchedProvider;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetMatchedFullHash(nsACString& aFullHash) {
  aFullHash = mMatchedFullHash;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetMatchedInfo(const nsACString& aList,
                                const nsACString& aProvider,
                                const nsACString& aFullHash) {
  NS_ENSURE_ARG(!aList.IsEmpty());

  mMatchedList = aList;
  mMatchedProvider = aProvider;
  mMatchedFullHash = aFullHash;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetMatchedTrackingLists(nsTArray<nsCString>& aLists) {
  aLists = mMatchedTrackingLists.Clone();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetMatchedTrackingFullHashes(
    nsTArray<nsCString>& aFullHashes) {
  aFullHashes = mMatchedTrackingFullHashes.Clone();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetMatchedTrackingInfo(
    const nsTArray<nsCString>& aLists, const nsTArray<nsCString>& aFullHashes) {
  NS_ENSURE_ARG(!aLists.IsEmpty());
  // aFullHashes can be empty for non hash-matching algorithm, for example,
  // host based test entries in preference.

  mMatchedTrackingLists = aLists.Clone();
  mMatchedTrackingFullHashes = aFullHashes.Clone();
  return NS_OK;
}
//-----------------------------------------------------------------------------
// HttpBaseChannel::nsITimedChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::GetChannelCreation(TimeStamp* _retval) {
  *_retval = mChannelCreationTimestamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetChannelCreation(TimeStamp aValue) {
  MOZ_DIAGNOSTIC_ASSERT(!aValue.IsNull());
  TimeDuration adjust = aValue - mChannelCreationTimestamp;
  mChannelCreationTimestamp = aValue;
  mChannelCreationTime += (PRTime)adjust.ToMicroseconds();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetAsyncOpen(TimeStamp* _retval) {
  *_retval = mAsyncOpenTime;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetAsyncOpen(TimeStamp aValue) {
  MOZ_DIAGNOSTIC_ASSERT(!aValue.IsNull());
  mAsyncOpenTime = aValue;
  StoreAsyncOpenTimeOverriden(true);
  return NS_OK;
}

/**
 * @return the number of redirects. There is no check for cross-domain
 * redirects. This check must be done by the consumers.
 */
NS_IMETHODIMP
HttpBaseChannel::GetRedirectCount(uint8_t* aRedirectCount) {
  *aRedirectCount = mRedirectCount;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRedirectCount(uint8_t aRedirectCount) {
  mRedirectCount = aRedirectCount;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetInternalRedirectCount(uint8_t* aRedirectCount) {
  *aRedirectCount = mInternalRedirectCount;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetInternalRedirectCount(uint8_t aRedirectCount) {
  mInternalRedirectCount = aRedirectCount;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRedirectStart(TimeStamp* _retval) {
  *_retval = mRedirectStartTimeStamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRedirectStart(TimeStamp aRedirectStart) {
  mRedirectStartTimeStamp = aRedirectStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRedirectEnd(TimeStamp* _retval) {
  *_retval = mRedirectEndTimeStamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRedirectEnd(TimeStamp aRedirectEnd) {
  mRedirectEndTimeStamp = aRedirectEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetAllRedirectsSameOrigin(bool* aAllRedirectsSameOrigin) {
  *aAllRedirectsSameOrigin = LoadAllRedirectsSameOrigin();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetAllRedirectsSameOrigin(bool aAllRedirectsSameOrigin) {
  StoreAllRedirectsSameOrigin(aAllRedirectsSameOrigin);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetAllRedirectsPassTimingAllowCheck(bool* aPassesCheck) {
  *aPassesCheck = LoadAllRedirectsPassTimingAllowCheck();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetAllRedirectsPassTimingAllowCheck(bool aPassesCheck) {
  StoreAllRedirectsPassTimingAllowCheck(aPassesCheck);
  return NS_OK;
}

// https://fetch.spec.whatwg.org/#cors-check
bool HttpBaseChannel::PerformCORSCheck() {
  // Step 1
  // Let origin be the result of getting `Access-Control-Allow-Origin`
  // from response’s header list.
  nsAutoCString origin;
  nsresult rv = GetResponseHeader("Access-Control-Allow-Origin"_ns, origin);

  // Step 2
  // If origin is null, then return failure. (Note: null, not 'null').
  if (NS_FAILED(rv) || origin.IsVoid()) {
    return false;
  }

  // Step 3
  // If request’s credentials mode is not "include"
  // and origin is `*`, then return success.
  uint32_t cookiePolicy = mLoadInfo->GetCookiePolicy();
  if (cookiePolicy != nsILoadInfo::SEC_COOKIES_INCLUDE &&
      origin.EqualsLiteral("*")) {
    return true;
  }

  // Step 4
  // If the result of byte-serializing a request origin
  // with request is not origin, then return failure.
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  nsCOMPtr<nsIPrincipal> resourcePrincipal;
  rv = ssm->GetChannelURIPrincipal(this, getter_AddRefs(resourcePrincipal));
  if (NS_FAILED(rv) || !resourcePrincipal) {
    return false;
  }
  nsAutoCString serializedOrigin;
  nsContentSecurityManager::GetSerializedOrigin(
      mLoadInfo->TriggeringPrincipal(), resourcePrincipal, serializedOrigin,
      mLoadInfo);
  if (!serializedOrigin.Equals(origin)) {
    return false;
  }

  // Step 5
  // If request’s credentials mode is not "include", then return success.
  if (cookiePolicy != nsILoadInfo::SEC_COOKIES_INCLUDE) {
    return true;
  }

  // Step 6
  // Let credentials be the result of getting
  // `Access-Control-Allow-Credentials` from response’s header list.
  nsAutoCString credentials;
  rv = GetResponseHeader("Access-Control-Allow-Credentials"_ns, credentials);

  // Step 7 and 8
  // If credentials is `true`, then return success.
  // (else) return failure.
  return NS_SUCCEEDED(rv) && credentials.EqualsLiteral("true");
}

NS_IMETHODIMP
HttpBaseChannel::BodyInfoAccessAllowedCheck(nsIPrincipal* aOrigin,
                                            BodyInfoAccess* _retval) {
  // Per the Fetch spec, https://fetch.spec.whatwg.org/#response-body-info,
  // the bodyInfo for Resource Timing and Navigation Timing info consists of
  // encoded size, decoded size, and content type. It is however made opaque
  // whenever the response is turned into a network error, which sets its
  // bodyInfo to its default values (sizes=0, content-type="").

  // Case 1:
  // "no-cors" -> Upon success, fetch will return an opaque filtered response.
  // An opaque(-redirect) filtered response is a filtered response
  //   whose ... body info is a new response body info.
  auto tainting = mLoadInfo->GetTainting();
  if (tainting == mozilla::LoadTainting::Opaque) {
    *_retval = BodyInfoAccess::DISALLOWED;
    return NS_OK;
  }

  // Case 2:
  // If request’s response tainting is "cors" and a CORS check for request
  // and response returns failure, then return a network error.
  if (tainting == mozilla::LoadTainting::CORS && !PerformCORSCheck()) {
    *_retval = BodyInfoAccess::DISALLOWED;
    return NS_OK;
  }

  // Otherwise:
  // The fetch response handover, given a fetch params fetchParams
  //    and a response response, run these steps:
  // processResponseEndOfBody:
  // - If fetchParams’s request’s mode is not "navigate" or response’s
  //   has-cross-origin-redirects is false:
  //   - Let mimeType be the result of extracting a MIME type from
  //     response’s header list.
  //   - If mimeType is not failure, then set bodyInfo’s content type to the
  //     result of minimizing a supported MIME type given mimeType.
  dom::RequestMode requestMode;
  MOZ_ALWAYS_SUCCEEDS(GetRequestMode(&requestMode));
  if (requestMode != RequestMode::Navigate || LoadAllRedirectsSameOrigin()) {
    *_retval = BodyInfoAccess::ALLOW_ALL;
    return NS_OK;
  }

  *_retval = BodyInfoAccess::ALLOW_SIZES;
  return NS_OK;
}

// https://fetch.spec.whatwg.org/#tao-check
NS_IMETHODIMP
HttpBaseChannel::TimingAllowCheck(nsIPrincipal* aOrigin, bool* _retval) {
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  nsCOMPtr<nsIPrincipal> resourcePrincipal;
  nsresult rv =
      ssm->GetChannelURIPrincipal(this, getter_AddRefs(resourcePrincipal));
  if (NS_FAILED(rv) || !resourcePrincipal || !aOrigin) {
    *_retval = false;
    return NS_OK;
  }

  bool sameOrigin = false;
  rv = resourcePrincipal->Equals(aOrigin, &sameOrigin);

  nsAutoCString serializedOrigin;
  nsContentSecurityManager::GetSerializedOrigin(aOrigin, resourcePrincipal,
                                                serializedOrigin, mLoadInfo);

  // All redirects are same origin
  if (sameOrigin && (!serializedOrigin.IsEmpty() &&
                     !serializedOrigin.EqualsLiteral("null"))) {
    *_retval = true;
    return NS_OK;
  }

  nsAutoCString headerValue;
  rv = GetResponseHeader("Timing-Allow-Origin"_ns, headerValue);
  if (NS_FAILED(rv)) {
    *_retval = false;
    return NS_OK;
  }

  Tokenizer p(headerValue);
  Tokenizer::Token t;

  p.Record();
  nsAutoCString headerItem;
  while (p.Next(t)) {
    if (t.Type() == Tokenizer::TOKEN_EOF ||
        t.Equals(Tokenizer::Token::Char(','))) {
      p.Claim(headerItem);
      nsHttp::TrimHTTPWhitespace(headerItem, headerItem);
      // If the list item contains a case-sensitive match for the value of the
      // origin, or a wildcard, return pass
      if (headerItem == serializedOrigin || headerItem == "*") {
        *_retval = true;
        return NS_OK;
      }
      // We start recording again for the following items in the list
      p.Record();
    }
  }

  *_retval = false;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLaunchServiceWorkerStart(TimeStamp* _retval) {
  MOZ_ASSERT(_retval);
  *_retval = mLaunchServiceWorkerStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetLaunchServiceWorkerStart(TimeStamp aTimeStamp) {
  mLaunchServiceWorkerStart = aTimeStamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLaunchServiceWorkerEnd(TimeStamp* _retval) {
  MOZ_ASSERT(_retval);
  *_retval = mLaunchServiceWorkerEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetLaunchServiceWorkerEnd(TimeStamp aTimeStamp) {
  mLaunchServiceWorkerEnd = aTimeStamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetDispatchFetchEventStart(TimeStamp* _retval) {
  MOZ_ASSERT(_retval);
  *_retval = mDispatchFetchEventStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetDispatchFetchEventStart(TimeStamp aTimeStamp) {
  mDispatchFetchEventStart = aTimeStamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetDispatchFetchEventEnd(TimeStamp* _retval) {
  MOZ_ASSERT(_retval);
  *_retval = mDispatchFetchEventEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetDispatchFetchEventEnd(TimeStamp aTimeStamp) {
  mDispatchFetchEventEnd = aTimeStamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetHandleFetchEventStart(TimeStamp* _retval) {
  MOZ_ASSERT(_retval);
  *_retval = mHandleFetchEventStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetHandleFetchEventStart(TimeStamp aTimeStamp) {
  mHandleFetchEventStart = aTimeStamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetHandleFetchEventEnd(TimeStamp* _retval) {
  MOZ_ASSERT(_retval);
  *_retval = mHandleFetchEventEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetHandleFetchEventEnd(TimeStamp aTimeStamp) {
  mHandleFetchEventEnd = aTimeStamp;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetDomainLookupStart(TimeStamp* _retval) {
  *_retval = mTransactionTimings.domainLookupStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetDomainLookupEnd(TimeStamp* _retval) {
  *_retval = mTransactionTimings.domainLookupEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetConnectStart(TimeStamp* _retval) {
  *_retval = mTransactionTimings.connectStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetTcpConnectEnd(TimeStamp* _retval) {
  *_retval = mTransactionTimings.tcpConnectEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetSecureConnectionStart(TimeStamp* _retval) {
  *_retval = mTransactionTimings.secureConnectionStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetConnectEnd(TimeStamp* _retval) {
  *_retval = mTransactionTimings.connectEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRequestStart(TimeStamp* _retval) {
  *_retval = mTransactionTimings.requestStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetResponseStart(TimeStamp* _retval) {
  *_retval = mTransactionTimings.responseStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetResponseEnd(TimeStamp* _retval) {
  *_retval = mTransactionTimings.responseEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetCacheReadStart(TimeStamp* _retval) {
  *_retval = mCacheReadStart;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetCacheReadEnd(TimeStamp* _retval) {
  *_retval = mCacheReadEnd;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetTransactionPending(TimeStamp* _retval) {
  *_retval = mTransactionTimings.transactionPending;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetInitiatorType(nsAString& aInitiatorType) {
  aInitiatorType = mInitiatorType;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetInitiatorType(const nsAString& aInitiatorType) {
  mInitiatorType = aInitiatorType;
  return NS_OK;
}

#define IMPL_TIMING_ATTR(name)                                           \
  NS_IMETHODIMP                                                          \
  HttpBaseChannel::Get##name##Time(PRTime* _retval) {                    \
    TimeStamp stamp;                                                     \
    Get##name(&stamp);                                                   \
    if (stamp.IsNull()) {                                                \
      *_retval = 0;                                                      \
      return NS_OK;                                                      \
    }                                                                    \
    *_retval =                                                           \
        mChannelCreationTime +                                           \
        (PRTime)((stamp - mChannelCreationTimestamp).ToSeconds() * 1e6); \
    return NS_OK;                                                        \
  }

IMPL_TIMING_ATTR(ChannelCreation)
IMPL_TIMING_ATTR(AsyncOpen)
IMPL_TIMING_ATTR(LaunchServiceWorkerStart)
IMPL_TIMING_ATTR(LaunchServiceWorkerEnd)
IMPL_TIMING_ATTR(DispatchFetchEventStart)
IMPL_TIMING_ATTR(DispatchFetchEventEnd)
IMPL_TIMING_ATTR(HandleFetchEventStart)
IMPL_TIMING_ATTR(HandleFetchEventEnd)
IMPL_TIMING_ATTR(DomainLookupStart)
IMPL_TIMING_ATTR(DomainLookupEnd)
IMPL_TIMING_ATTR(ConnectStart)
IMPL_TIMING_ATTR(TcpConnectEnd)
IMPL_TIMING_ATTR(SecureConnectionStart)
IMPL_TIMING_ATTR(ConnectEnd)
IMPL_TIMING_ATTR(RequestStart)
IMPL_TIMING_ATTR(ResponseStart)
IMPL_TIMING_ATTR(ResponseEnd)
IMPL_TIMING_ATTR(CacheReadStart)
IMPL_TIMING_ATTR(CacheReadEnd)
IMPL_TIMING_ATTR(RedirectStart)
IMPL_TIMING_ATTR(RedirectEnd)
IMPL_TIMING_ATTR(TransactionPending)

#undef IMPL_TIMING_ATTR

void HttpBaseChannel::MaybeReportTimingData() {
  // There is no point in continuing, since the performance object in the parent
  // isn't the same as the one in the child which will be reporting resource
  // performance.
  if (XRE_IsE10sParentProcess()) {
    return;
  }

  // Devtools can create fetch requests on behalf the content document.
  // If we don't exclude these requests, they'd also be reported
  // to the content document.
  bool isInDevToolsContext;
  mLoadInfo->GetIsInDevToolsContext(&isInDevToolsContext);
  if (isInDevToolsContext) {
    return;
  }

  mozilla::dom::PerformanceStorage* documentPerformance =
      mLoadInfo->GetPerformanceStorage();
  if (documentPerformance) {
    documentPerformance->AddEntry(this, this);
    return;
  }

  if (!nsGlobalWindowInner::GetInnerWindowWithId(
          mLoadInfo->GetInnerWindowID())) {
    // The inner window is in a different process.
    dom::ContentChild* child = dom::ContentChild::GetSingleton();

    if (!child) {
      return;
    }
    nsAutoString initiatorType;
    nsAutoString entryName;

    UniquePtr<dom::PerformanceTimingData> performanceTimingData(
        dom::PerformanceTimingData::Create(this, this, 0, initiatorType,
                                           entryName));
    if (!performanceTimingData) {
      return;
    }

    LoadInfoArgs loadInfoArgs;
    mozilla::ipc::LoadInfoToLoadInfoArgs(mLoadInfo, &loadInfoArgs);
    child->SendReportFrameTimingData(loadInfoArgs, entryName, initiatorType,
                                     std::move(performanceTimingData));
  }
}

NS_IMETHODIMP
HttpBaseChannel::SetReportResourceTiming(bool enabled) {
  StoreReportTiming(enabled);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetReportResourceTiming(bool* _retval) {
  *_retval = LoadReportTiming();
  return NS_OK;
}

nsIURI* HttpBaseChannel::GetReferringPage() {
  nsCOMPtr<nsPIDOMWindowInner> pDomWindow = GetInnerDOMWindow();
  if (!pDomWindow) {
    return nullptr;
  }
  return pDomWindow->GetDocumentURI();
}

nsPIDOMWindowInner* HttpBaseChannel::GetInnerDOMWindow() {
  nsCOMPtr<nsILoadContext> loadContext;
  NS_QueryNotificationCallbacks(this, loadContext);
  if (!loadContext) {
    return nullptr;
  }
  nsCOMPtr<mozIDOMWindowProxy> domWindow;
  loadContext->GetAssociatedWindow(getter_AddRefs(domWindow));
  if (!domWindow) {
    return nullptr;
  }
  auto* pDomWindow = nsPIDOMWindowOuter::From(domWindow);
  if (!pDomWindow) {
    return nullptr;
  }
  nsCOMPtr<nsPIDOMWindowInner> innerWindow =
      pDomWindow->GetCurrentInnerWindow();
  if (!innerWindow) {
    return nullptr;
  }

  return innerWindow;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIThrottledInputChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpBaseChannel::SetThrottleQueue(nsIInputChannelThrottleQueue* aQueue) {
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_FAILURE;
  }

  mThrottleQueue = aQueue;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetThrottleQueue(nsIInputChannelThrottleQueue** aQueue) {
  NS_ENSURE_ARG_POINTER(aQueue);
  nsCOMPtr<nsIInputChannelThrottleQueue> queue = mThrottleQueue;
  queue.forget(aQueue);
  return NS_OK;
}

//------------------------------------------------------------------------------

bool HttpBaseChannel::EnsureRequestContextID() {
  if (mRequestContextID) {
    // Already have a request context ID, no need to do the rest of this work
    LOG(("HttpBaseChannel::EnsureRequestContextID this=%p id=%" PRIx64, this,
         mRequestContextID));
    return true;
  }

  // Find the loadgroup at the end of the chain in order
  // to make sure all channels derived from the load group
  // use the same connection scope.
  nsCOMPtr<nsILoadGroupChild> childLoadGroup = do_QueryInterface(mLoadGroup);
  if (!childLoadGroup) {
    return false;
  }

  nsCOMPtr<nsILoadGroup> rootLoadGroup;
  childLoadGroup->GetRootLoadGroup(getter_AddRefs(rootLoadGroup));
  if (!rootLoadGroup) {
    return false;
  }

  // Set the load group connection scope on this channel and its transaction
  rootLoadGroup->GetRequestContextID(&mRequestContextID);

  LOG(("HttpBaseChannel::EnsureRequestContextID this=%p id=%" PRIx64, this,
       mRequestContextID));

  return true;
}

bool HttpBaseChannel::EnsureRequestContext() {
  if (mRequestContext) {
    // Already have a request context, no need to do the rest of this work
    return true;
  }

  if (!EnsureRequestContextID()) {
    return false;
  }

  nsIRequestContextService* rcsvc = gHttpHandler->GetRequestContextService();
  if (!rcsvc) {
    return false;
  }

  rcsvc->GetRequestContext(mRequestContextID, getter_AddRefs(mRequestContext));
  return static_cast<bool>(mRequestContext);
}

void HttpBaseChannel::EnsureBrowserId() {
  if (mBrowserId) {
    return;
  }

  RefPtr<dom::BrowsingContext> bc;
  MOZ_ALWAYS_SUCCEEDS(mLoadInfo->GetBrowsingContext(getter_AddRefs(bc)));

  if (bc) {
    mBrowserId = bc->GetBrowserId();
  }
}

void HttpBaseChannel::SetCorsPreflightParameters(
    const nsTArray<nsCString>& aUnsafeHeaders,
    bool aShouldStripRequestBodyHeader, bool aShouldStripAuthHeader) {
  MOZ_RELEASE_ASSERT(!LoadRequestObserversCalled());

  StoreRequireCORSPreflight(true);
  mUnsafeHeaders = aUnsafeHeaders.Clone();
  if (aShouldStripRequestBodyHeader || aShouldStripAuthHeader) {
    mUnsafeHeaders.RemoveElementsBy([&](const nsCString& aHeader) {
      return (aShouldStripRequestBodyHeader &&
              (aHeader.LowerCaseEqualsASCII("content-type") ||
               aHeader.LowerCaseEqualsASCII("content-encoding") ||
               aHeader.LowerCaseEqualsASCII("content-language") ||
               aHeader.LowerCaseEqualsASCII("content-location"))) ||
             (aShouldStripAuthHeader &&
              aHeader.LowerCaseEqualsASCII("authorization"));
    });
  }
}

void HttpBaseChannel::SetAltDataForChild(bool aIsForChild) {
  StoreAltDataForChild(aIsForChild);
}

NS_IMETHODIMP
HttpBaseChannel::GetBlockAuthPrompt(bool* aValue) {
  if (!aValue) {
    return NS_ERROR_FAILURE;
  }

  *aValue = LoadBlockAuthPrompt();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetBlockAuthPrompt(bool aValue) {
  ENSURE_CALLED_BEFORE_CONNECT();

  StoreBlockAuthPrompt(aValue);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetConnectionInfoHashKey(nsACString& aConnectionInfoHashKey) {
  if (!mConnectionInfo) {
    return NS_ERROR_FAILURE;
  }
  aConnectionInfoHashKey.Assign(mConnectionInfo->HashKey());
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetLastRedirectFlags(uint32_t* aValue) {
  NS_ENSURE_ARG(aValue);
  *aValue = mLastRedirectFlags;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetLastRedirectFlags(uint32_t aValue) {
  mLastRedirectFlags = aValue;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetNavigationStartTimeStamp(TimeStamp* aTimeStamp) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
HttpBaseChannel::SetNavigationStartTimeStamp(TimeStamp aTimeStamp) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsresult HttpBaseChannel::CheckRedirectLimit(nsIURI* aNewURI,
                                             uint32_t aRedirectFlags) const {
  if (aRedirectFlags & nsIChannelEventSink::REDIRECT_INTERNAL) {
    // for internal redirect due to auth retry we do not have any limit
    // as we might restrict the number of times a user might retry
    // authentication
    if (aRedirectFlags & nsIChannelEventSink::REDIRECT_AUTH_RETRY) {
      return NS_OK;
    }
    // Some platform features, like Service Workers, depend on internal
    // redirects.  We should allow some number of internal redirects above
    // and beyond the normal redirect limit so these features continue
    // to work.
    static const int8_t kMinInternalRedirects = 5;

    if (mInternalRedirectCount >= (mRedirectionLimit + kMinInternalRedirects)) {
      LOG(("internal redirection limit reached!\n"));
      return NS_ERROR_REDIRECT_LOOP;
    }
    return NS_OK;
  }

  MOZ_ASSERT(aRedirectFlags & (nsIChannelEventSink::REDIRECT_TEMPORARY |
                               nsIChannelEventSink::REDIRECT_PERMANENT |
                               nsIChannelEventSink::REDIRECT_STS_UPGRADE));

  if (mRedirectCount >= mRedirectionLimit) {
    LOG(("redirection limit reached!\n"));
    return NS_ERROR_REDIRECT_LOOP;
  }

  // in case https-only mode is enabled which upgrades top-level requests to
  // https and the page answers with a redirect (meta, 302, win.location, ...)
  // then this method can break the cycle which causes the https-only exception
  // page to appear. Note that https-first mode breaks upgrade downgrade endless
  // loops within ShouldUpgradeHttpsFirstRequest because https-first does not
  // display an exception page but needs a soft fallback/downgrade.
  if (nsHTTPSOnlyUtils::IsUpgradeDowngradeEndlessLoop(
          mURI, aNewURI, mLoadInfo,
          {nsHTTPSOnlyUtils::UpgradeDowngradeEndlessLoopOptions::
               EnforceForHTTPSOnlyMode})) {
    // Mark that we didn't upgrade to https due to loop detection in https-only
    // mode to show https-only error page. We know that we are in https-only
    // mode, because we passed `EnforceForHTTPSOnlyMode` to
    // `IsUpgradeDowngradeEndlessLoop`. In other words we upgrade the request
    // with https-only mode, but then immediately cancel the request.
    uint32_t httpsOnlyStatus = mLoadInfo->GetHttpsOnlyStatus();
    if (httpsOnlyStatus & nsILoadInfo::HTTPS_ONLY_UNINITIALIZED) {
      httpsOnlyStatus ^= nsILoadInfo::HTTPS_ONLY_UNINITIALIZED;
      httpsOnlyStatus |=
          nsILoadInfo::HTTPS_ONLY_UPGRADED_LISTENER_NOT_REGISTERED;
      mLoadInfo->SetHttpsOnlyStatus(httpsOnlyStatus);
    }

    LOG(("upgrade downgrade redirect loop!\n"));
    return NS_ERROR_REDIRECT_LOOP;
  }
  // in case of http-first mode we want to add an exception to disable the
  // upgrade behavior if we have upgrade-downgrade loop to break the loop and
  // load the http request next
  if (mozilla::StaticPrefs::
          dom_security_https_first_add_exception_on_failure() &&
      nsHTTPSOnlyUtils::IsUpgradeDowngradeEndlessLoop(
          mURI, aNewURI, mLoadInfo,
          {nsHTTPSOnlyUtils::UpgradeDowngradeEndlessLoopOptions::
               EnforceForHTTPSFirstMode})) {
    nsHTTPSOnlyUtils::AddHTTPSFirstException(mURI, mLoadInfo);
  }

  return NS_OK;
}

// NOTE: This function duplicates code from nsBaseChannel. This will go away
// once HTTP uses nsBaseChannel (part of bug 312760)
/* static */
void HttpBaseChannel::CallTypeSniffers(void* aClosure, const uint8_t* aData,
                                       uint32_t aCount) {
  nsIChannel* chan = static_cast<nsIChannel*>(aClosure);
  const char* snifferType = [chan]() {
    if (RefPtr<nsHttpChannel> httpChannel = do_QueryObject(chan)) {
      switch (httpChannel->GetSnifferCategoryType()) {
        case SnifferCategoryType::NetContent:
          return NS_CONTENT_SNIFFER_CATEGORY;
        case SnifferCategoryType::OpaqueResponseBlocking:
          return NS_ORB_SNIFFER_CATEGORY;
        case SnifferCategoryType::All:
          return NS_CONTENT_AND_ORB_SNIFFER_CATEGORY;
        default:
          MOZ_ASSERT_UNREACHABLE("Unexpected SnifferCategoryType!");
      }
    }

    return NS_CONTENT_SNIFFER_CATEGORY;
  }();

  nsAutoCString newType;
  NS_SniffContent(snifferType, chan, aData, aCount, newType);
  if (!newType.IsEmpty()) {
    chan->SetContentType(newType);
  }
}

template <class T>
static void ParseServerTimingHeader(
    const UniquePtr<T>& aHeader, nsTArray<nsCOMPtr<nsIServerTiming>>& aOutput) {
  if (!aHeader) {
    return;
  }

  nsAutoCString serverTimingHeader;
  Unused << aHeader->GetHeader(nsHttp::Server_Timing, serverTimingHeader);
  if (serverTimingHeader.IsEmpty()) {
    return;
  }

  ServerTimingParser parser(serverTimingHeader);
  parser.Parse();

  nsTArray<nsCOMPtr<nsIServerTiming>> array = parser.TakeServerTimingHeaders();
  aOutput.AppendElements(array);
}

NS_IMETHODIMP
HttpBaseChannel::GetServerTiming(nsIArray** aServerTiming) {
  nsresult rv;
  NS_ENSURE_ARG_POINTER(aServerTiming);

  nsCOMPtr<nsIMutableArray> array = do_CreateInstance(NS_ARRAY_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsTArray<nsCOMPtr<nsIServerTiming>> data;
  rv = GetNativeServerTiming(data);
  NS_ENSURE_SUCCESS(rv, rv);

  for (const auto& entry : data) {
    array->AppendElement(entry);
  }

  array.forget(aServerTiming);
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetNativeServerTiming(
    nsTArray<nsCOMPtr<nsIServerTiming>>& aServerTiming) {
  aServerTiming.Clear();

  if (nsContentUtils::ComputeIsSecureContext(this)) {
    ParseServerTimingHeader(mResponseHead, aServerTiming);
    ParseServerTimingHeader(mResponseTrailers, aServerTiming);
  }

  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::CancelByURLClassifier(nsresult aErrorCode) {
  MOZ_ASSERT(
      UrlClassifierFeatureFactory::IsClassifierBlockingErrorCode(aErrorCode));
  return Cancel(aErrorCode);
}

NS_IMETHODIMP HttpBaseChannel::SetIPv4Disabled() {
  mCaps |= NS_HTTP_DISABLE_IPV4;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::SetIPv6Disabled() {
  mCaps |= NS_HTTP_DISABLE_IPV6;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::GetResponseEmbedderPolicy(
    bool aIsOriginTrialCoepCredentiallessEnabled,
    nsILoadInfo::CrossOriginEmbedderPolicy* aOutPolicy) {
  *aOutPolicy = nsILoadInfo::EMBEDDER_POLICY_NULL;
  if (!mResponseHead) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (!nsContentUtils::ComputeIsSecureContext(this)) {
    // Feature is only available for secure contexts.
    return NS_OK;
  }

  nsAutoCString content;
  Unused << mResponseHead->GetHeader(nsHttp::Cross_Origin_Embedder_Policy,
                                     content);
  *aOutPolicy = NS_GetCrossOriginEmbedderPolicyFromHeader(
      content, aIsOriginTrialCoepCredentiallessEnabled);
  return NS_OK;
}

// Obtain a cross-origin opener-policy from a response response and a
// cross-origin opener policy initiator.
// https://gist.github.com/annevk/6f2dd8c79c77123f39797f6bdac43f3e
NS_IMETHODIMP HttpBaseChannel::ComputeCrossOriginOpenerPolicy(
    nsILoadInfo::CrossOriginOpenerPolicy aInitiatorPolicy,
    nsILoadInfo::CrossOriginOpenerPolicy* aOutPolicy) {
  MOZ_ASSERT(aOutPolicy);
  *aOutPolicy = nsILoadInfo::OPENER_POLICY_UNSAFE_NONE;

  if (!mResponseHead) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // COOP headers are ignored for insecure-context loads.
  if (!nsContentUtils::ComputeIsSecureContext(this)) {
    return NS_OK;
  }

  nsAutoCString openerPolicy;
  Unused << mResponseHead->GetHeader(nsHttp::Cross_Origin_Opener_Policy,
                                     openerPolicy);

  // Cross-Origin-Opener-Policy = %s"same-origin" /
  //                              %s"same-origin-allow-popups" /
  //                              %s"unsafe-none"; case-sensitive

  nsCOMPtr<nsISFVService> sfv = GetSFVService();

  nsCOMPtr<nsISFVItem> item;
  nsresult rv = sfv->ParseItem(openerPolicy, getter_AddRefs(item));
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsISFVBareItem> value;
  rv = item->GetValue(getter_AddRefs(value));
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsISFVToken> token = do_QueryInterface(value);
  if (!token) {
    return NS_ERROR_UNEXPECTED;
  }

  rv = token->GetValue(openerPolicy);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsILoadInfo::CrossOriginOpenerPolicy policy =
      nsILoadInfo::OPENER_POLICY_UNSAFE_NONE;

  if (openerPolicy.EqualsLiteral("same-origin")) {
    policy = nsILoadInfo::OPENER_POLICY_SAME_ORIGIN;
  } else if (openerPolicy.EqualsLiteral("same-origin-allow-popups")) {
    policy = nsILoadInfo::OPENER_POLICY_SAME_ORIGIN_ALLOW_POPUPS;
  }
  if (policy == nsILoadInfo::OPENER_POLICY_SAME_ORIGIN) {
    nsILoadInfo::CrossOriginEmbedderPolicy coep =
        nsILoadInfo::EMBEDDER_POLICY_NULL;
    bool isCoepCredentiallessEnabled;
    rv = mLoadInfo->GetIsOriginTrialCoepCredentiallessEnabledForTopLevel(
        &isCoepCredentiallessEnabled);
    if (!isCoepCredentiallessEnabled) {
      nsAutoCString originTrialToken;
      Unused << mResponseHead->GetHeader(nsHttp::OriginTrial, originTrialToken);
      if (!originTrialToken.IsEmpty()) {
        nsCOMPtr<nsIPrincipal> resultPrincipal;
        rv = nsContentUtils::GetSecurityManager()->GetChannelResultPrincipal(
            this, getter_AddRefs(resultPrincipal));
        if (!NS_WARN_IF(NS_FAILED(rv))) {
          OriginTrials trials;
          trials.UpdateFromToken(NS_ConvertASCIItoUTF16(originTrialToken),
                                 resultPrincipal);
          if (trials.IsEnabled(OriginTrial::CoepCredentialless)) {
            isCoepCredentiallessEnabled = true;
          }
        }
      }
    }

    NS_ENSURE_SUCCESS(rv, rv);
    if (NS_SUCCEEDED(
            GetResponseEmbedderPolicy(isCoepCredentiallessEnabled, &coep)) &&
        (coep == nsILoadInfo::EMBEDDER_POLICY_REQUIRE_CORP ||
         coep == nsILoadInfo::EMBEDDER_POLICY_CREDENTIALLESS)) {
      policy =
          nsILoadInfo::OPENER_POLICY_SAME_ORIGIN_EMBEDDER_POLICY_REQUIRE_CORP;
    }
  }

  *aOutPolicy = policy;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetCrossOriginOpenerPolicy(
    nsILoadInfo::CrossOriginOpenerPolicy* aPolicy) {
  MOZ_ASSERT(aPolicy);
  if (!aPolicy) {
    return NS_ERROR_INVALID_ARG;
  }
  // If this method is called before OnStartRequest (ie. before we call
  // ComputeCrossOriginOpenerPolicy) or if we were unable to compute the
  // policy we'll throw an error.
  if (!LoadOnStartRequestCalled()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  *aPolicy = mComputedCrossOriginOpenerPolicy;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::HasCrossOriginOpenerPolicyMismatch(bool* aIsMismatch) {
  // This should only be called in parent process.
  MOZ_ASSERT(XRE_IsParentProcess());
  *aIsMismatch = LoadHasCrossOriginOpenerPolicyMismatch();
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetOriginAgentClusterHeader(bool* aValue) {
  MOZ_ASSERT(XRE_IsParentProcess());
  if (!mResponseHead) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsAutoCString content;
  nsresult rv = mResponseHead->GetHeader(nsHttp::OriginAgentCluster, content);
  if (NS_FAILED(rv)) {
    return rv;
  }

  // Origin-Agent-Cluster = <boolean>
  nsCOMPtr<nsISFVService> sfv = GetSFVService();
  nsCOMPtr<nsISFVItem> item;
  rv = sfv->ParseItem(content, getter_AddRefs(item));
  if (NS_FAILED(rv)) {
    return rv;
  }
  nsCOMPtr<nsISFVBareItem> value;
  rv = item->GetValue(getter_AddRefs(value));
  if (NS_FAILED(rv)) {
    return rv;
  }
  nsCOMPtr<nsISFVBool> flag = do_QueryInterface(value);
  if (!flag) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  return flag->GetValue(aValue);
}

void HttpBaseChannel::MaybeFlushConsoleReports() {
  // Flush if we have a known window ID.
  if (mLoadInfo->GetInnerWindowID() > 0) {
    FlushReportsToConsole(mLoadInfo->GetInnerWindowID());
    return;
  }

  // If this channel is part of a loadGroup, we can flush the console reports
  // immediately.
  nsCOMPtr<nsILoadGroup> loadGroup;
  nsresult rv = GetLoadGroup(getter_AddRefs(loadGroup));
  if (NS_SUCCEEDED(rv) && loadGroup) {
    FlushConsoleReports(loadGroup);
  }
}

void HttpBaseChannel::DoDiagnosticAssertWhenOnStopNotCalledOnDestroy() {}

bool HttpBaseChannel::Http3Allowed() const {
  bool isDirectOrNoProxy =
      mProxyInfo ? static_cast<nsProxyInfo*>(mProxyInfo.get())->IsDirect()
                 : true;
  return !mUpgradeProtocolCallback && isDirectOrNoProxy &&
         !(mCaps & NS_HTTP_BE_CONSERVATIVE) && !LoadBeConservative() &&
         LoadAllowHttp3();
}

UniquePtr<nsHttpResponseHead>
HttpBaseChannel::MaybeCloneResponseHeadForCachedResource() {
  if (!mResponseHead) {
    return nullptr;
  }

  return MakeUnique<nsHttpResponseHead>(*mResponseHead);
}

void HttpBaseChannel::SetDummyChannelForCachedResource(
    const nsHttpResponseHead* aMaybeResponseHead /* = nullptr */) {
  mDummyChannelForCachedResource = true;
  MOZ_ASSERT(!mResponseHead,
             "SetDummyChannelForCachedResource should only be called once");
  if (aMaybeResponseHead) {
    mResponseHead = MakeUnique<nsHttpResponseHead>(*aMaybeResponseHead);
  } else {
    mResponseHead = MakeUnique<nsHttpResponseHead>();
  }
}

void HttpBaseChannel::SetEarlyHints(
    nsTArray<EarlyHintConnectArgs>&& aEarlyHints) {
  mEarlyHints = std::move(aEarlyHints);
}

nsTArray<EarlyHintConnectArgs>&& HttpBaseChannel::TakeEarlyHints() {
  return std::move(mEarlyHints);
}

NS_IMETHODIMP
HttpBaseChannel::SetEarlyHintPreloaderId(uint64_t aEarlyHintPreloaderId) {
  mEarlyHintPreloaderId = aEarlyHintPreloaderId;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetEarlyHintPreloaderId(uint64_t* aEarlyHintPreloaderId) {
  NS_ENSURE_ARG_POINTER(aEarlyHintPreloaderId);
  *aEarlyHintPreloaderId = mEarlyHintPreloaderId;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetClassicScriptHintCharset(
    const nsAString& aClassicScriptHintCharset) {
  mClassicScriptHintCharset = aClassicScriptHintCharset;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::GetClassicScriptHintCharset(
    nsAString& aClassicScriptHintCharset) {
  aClassicScriptHintCharset = mClassicScriptHintCharset;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::SetDocumentCharacterSet(
    const nsAString& aDocumentCharacterSet) {
  mDocumentCharacterSet = aDocumentCharacterSet;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::GetDocumentCharacterSet(
    nsAString& aDocumentCharacterSet) {
  aDocumentCharacterSet = mDocumentCharacterSet;
  return NS_OK;
}

void HttpBaseChannel::SetConnectionInfo(nsHttpConnectionInfo* aCI) {
  mConnectionInfo = aCI ? aCI->Clone() : nullptr;
}

NS_IMETHODIMP
HttpBaseChannel::GetIsProxyUsed(bool* aIsProxyUsed) {
  if (mProxyInfo) {
    if (!static_cast<nsProxyInfo*>(mProxyInfo.get())->IsDirect()) {
      StoreIsProxyUsed(true);
    }
  }
  *aIsProxyUsed = LoadIsProxyUsed();
  return NS_OK;
}

static void CollectORBBlockTelemetry(
    const OpaqueResponseBlockedTelemetryReason aTelemetryReason,
    ExtContentPolicy aPolicy) {
  glean::orb::block_reason.EnumGet(aTelemetryReason).Add();

  switch (aPolicy) {
    case ExtContentPolicy::TYPE_INVALID:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eInvalid)
          .Add();
      break;
    case ExtContentPolicy::TYPE_OTHER:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eOther)
          .Add();
      break;
    case ExtContentPolicy::TYPE_FETCH:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eBlockedFetch)
          .Add();
      break;
    case ExtContentPolicy::TYPE_SCRIPT:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eScript)
          .Add();
      break;
    case ExtContentPolicy::TYPE_JSON:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eJson)
          .Add();
      break;
    case ExtContentPolicy::TYPE_IMAGE:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eImage)
          .Add();
      break;
    case ExtContentPolicy::TYPE_STYLESHEET:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eStylesheet)
          .Add();
      break;
    case ExtContentPolicy::TYPE_XMLHTTPREQUEST:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eXmlhttprequest)
          .Add();
      break;
    case ExtContentPolicy::TYPE_DTD:
      glean::orb::block_initiator.EnumGet(glean::orb::BlockInitiatorLabel::eDtd)
          .Add();
      break;
    case ExtContentPolicy::TYPE_FONT:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eFont)
          .Add();
      break;
    case ExtContentPolicy::TYPE_MEDIA:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eMedia)
          .Add();
      break;
    case ExtContentPolicy::TYPE_CSP_REPORT:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eCspReport)
          .Add();
      break;
    case ExtContentPolicy::TYPE_XSLT:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eXslt)
          .Add();
      break;
    case ExtContentPolicy::TYPE_IMAGESET:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eImageset)
          .Add();
      break;
    case ExtContentPolicy::TYPE_WEB_MANIFEST:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eWebManifest)
          .Add();
      break;
    case ExtContentPolicy::TYPE_SPECULATIVE:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eSpeculative)
          .Add();
      break;
    case ExtContentPolicy::TYPE_UA_FONT:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eUaFont)
          .Add();
      break;
    case ExtContentPolicy::TYPE_PROXIED_WEBRTC_MEDIA:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eProxiedWebrtcMedia)
          .Add();
      break;
    case ExtContentPolicy::TYPE_PING:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::ePing)
          .Add();
      break;
    case ExtContentPolicy::TYPE_BEACON:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eBeacon)
          .Add();
      break;
    case ExtContentPolicy::TYPE_WEB_TRANSPORT:
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eWebTransport)
          .Add();
      break;
    case ExtContentPolicy::TYPE_WEB_IDENTITY:
      // Don't bother extending the telemetry for this.
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eOther)
          .Add();
      break;
    case ExtContentPolicy::TYPE_DOCUMENT:
    case ExtContentPolicy::TYPE_SUBDOCUMENT:
    case ExtContentPolicy::TYPE_OBJECT:
    case ExtContentPolicy::TYPE_OBJECT_SUBREQUEST:
    case ExtContentPolicy::TYPE_WEBSOCKET:
    case ExtContentPolicy::TYPE_SAVEAS_DOWNLOAD:
      MOZ_ASSERT_UNREACHABLE("Shouldn't block this type");
      // DOCUMENT, SUBDOCUMENT, OBJECT, OBJECT_SUBREQUEST,
      // WEBSOCKET and SAVEAS_DOWNLOAD are excluded from ORB
      glean::orb::block_initiator
          .EnumGet(glean::orb::BlockInitiatorLabel::eExcluded)
          .Add();
      break;
      // Do not add default: so that compilers can catch the missing case.
  }
}

void HttpBaseChannel::LogORBError(
    const nsAString& aReason,
    const OpaqueResponseBlockedTelemetryReason aTelemetryReason) {
  auto policy = mLoadInfo->GetExternalContentPolicyType();
  CollectORBBlockTelemetry(aTelemetryReason, policy);

  // Blocking `ExtContentPolicy::TYPE_BEACON` isn't web observable, so keep
  // quiet in the console about blocking it.
  if (policy == ExtContentPolicy::TYPE_BEACON) {
    return;
  }

  RefPtr<dom::Document> doc;
  mLoadInfo->GetLoadingDocument(getter_AddRefs(doc));

  nsAutoCString uri;
  nsresult rv = nsContentUtils::AnonymizeURI(mURI, uri);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return;
  }

  uint64_t contentWindowId;
  GetTopLevelContentWindowId(&contentWindowId);
  if (contentWindowId) {
    nsContentUtils::ReportToConsoleByWindowID(
        u"A resource is blocked by OpaqueResponseBlocking, please check browser console for details."_ns,
        nsIScriptError::warningFlag, "ORB"_ns, contentWindowId,
        SourceLocation(mURI.get()));
  }

  AutoTArray<nsString, 2> params;
  params.AppendElement(NS_ConvertUTF8toUTF16(uri));
  params.AppendElement(aReason);
  nsContentUtils::ReportToConsole(nsIScriptError::warningFlag, "ORB"_ns, doc,
                                  nsContentUtils::eNECKO_PROPERTIES,
                                  "ResourceBlockedORB", params);
}

NS_IMETHODIMP HttpBaseChannel::SetEarlyHintLinkType(
    uint32_t aEarlyHintLinkType) {
  mEarlyHintLinkType = aEarlyHintLinkType;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::GetEarlyHintLinkType(
    uint32_t* aEarlyHintLinkType) {
  *aEarlyHintLinkType = mEarlyHintLinkType;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetHasContentDecompressed(bool aValue) {
  LOG(("HttpBaseChannel::SetHasContentDecompressed [this=%p value=%d]\n", this,
       aValue));
  mHasContentDecompressed = aValue;
  return NS_OK;
}
NS_IMETHODIMP
HttpBaseChannel::GetHasContentDecompressed(bool* value) {
  *value = mHasContentDecompressed;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::SetRenderBlocking(bool aRenderBlocking) {
  mRenderBlocking = aRenderBlocking;
  return NS_OK;
}

NS_IMETHODIMP
HttpBaseChannel::GetRenderBlocking(bool* aRenderBlocking) {
  *aRenderBlocking = mRenderBlocking;
  return NS_OK;
}

NS_IMETHODIMP HttpBaseChannel::GetLastTransportStatus(
    nsresult* aLastTransportStatus) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

void HttpBaseChannel::SetFetchPriorityDOM(
    mozilla::dom::FetchPriority aPriority) {
  switch (aPriority) {
    case mozilla::dom::FetchPriority::Auto:
      SetFetchPriority(nsIClassOfService::FETCHPRIORITY_AUTO);
      return;
    case mozilla::dom::FetchPriority::High:
      SetFetchPriority(nsIClassOfService::FETCHPRIORITY_HIGH);
      return;
    case mozilla::dom::FetchPriority::Low:
      SetFetchPriority(nsIClassOfService::FETCHPRIORITY_LOW);
      return;
    default:
      MOZ_ASSERT_UNREACHABLE();
  }
}

}  // namespace net
}  // namespace mozilla
