/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import { createSelector } from "devtools/client/shared/vendor/reselect";
import { parse } from "../utils/url";

export const getThreads = createSelector(
  state => state.threads.threads,
  threads => threads.filter(thread => !isMainThread(thread))
);

export function getAllThreads(state) {
  return state.threads.threads;
}

function isMainThread(thread) {
  return thread.isTopLevel;
}

export function getMainThread(state) {
  return state.threads.threads.find(isMainThread);
}

/*
 * Gets domain from the main thread url (without www prefix)
 */
export function getMainThreadHost(state) {
  const url = getMainThread(state)?.url;
  if (!url) {
    return null;
  }
  const { host } = parse(url);
  if (!host) {
    return null;
  }
  return host.startsWith("www.") ? host.substring("www.".length) : host;
}

export function getThread(state, threadActor) {
  return getAllThreads(state).find(thread => thread.actor === threadActor);
}

export function getIsThreadCurrentlyTracing(state, thread) {
  return state.threads.mutableTracingThreads.has(thread);
}

export function getIsCurrentlyTracing(state) {
  return state.threads.mutableTracingThreads.size > 0;
}
