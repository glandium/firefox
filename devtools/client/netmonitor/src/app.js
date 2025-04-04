/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {
  createFactory,
  createElement,
} = require("resource://devtools/client/shared/vendor/react.mjs");
const {
  render,
  unmountComponentAtNode,
} = require("resource://devtools/client/shared/vendor/react-dom.mjs");
const Provider =
  require("resource://devtools/client/shared/vendor/react-redux.js").Provider;
const ToolboxProvider = require("resource://devtools/client/framework/store-provider.js");
const {
  visibilityHandlerStore,
} = require("resource://devtools/client/shared/redux/visibilityHandlerStore.js");

const FluentReact = require("resource://devtools/client/shared/vendor/fluent-react.js");
const App = require("resource://devtools/client/netmonitor/src/components/App.js");
const {
  FluentL10n,
} = require("resource://devtools/client/shared/fluent-l10n/fluent-l10n.js");
const LocalizationProvider = createFactory(FluentReact.LocalizationProvider);
const {
  EVENTS,
} = require("resource://devtools/client/netmonitor/src/constants.js");

const {
  getDisplayedRequestById,
} = require("resource://devtools/client/netmonitor/src/selectors/index.js");

const SearchDispatcher = require("resource://devtools/client/netmonitor/src/workers/search/index.js");

/**
 * Global App object for Network panel. This object depends
 * on the UI and can't be created independently.
 *
 * This object can be consumed by other panels (e.g. Console
 * is using inspectRequest), by the Launchpad (bootstrap), etc.
 *
 * @param {Object} api An existing API object to be reused.
 */
function NetMonitorApp(api) {
  this.api = api;
}

NetMonitorApp.prototype = {
  async bootstrap({ toolbox, document }) {
    // Get the root element for mounting.
    this.mount = document.querySelector("#mount");

    const openLink = link => {
      const parentDoc = toolbox.doc;
      const iframe = parentDoc.getElementById(
        "toolbox-panel-iframe-netmonitor"
      );
      const { top } = iframe.ownerDocument.defaultView;
      top.openWebLinkIn(link, "tab");
    };

    const openSplitConsole = err => {
      toolbox.openSplitConsole().then(() => {
        toolbox.target.logErrorInPage(err, "har");
      });
    };

    const { actions, connector, store } = this.api;

    const sourceMapURLService = toolbox.sourceMapURLService;

    const fluentL10n = new FluentL10n();
    await fluentL10n.init(["devtools/client/netmonitor.ftl"]);

    // Render the root Application component.
    render(
      createElement(
        Provider,
        // Also wrap the store in order to pause store update notifications while the panel is hidden.
        // (this can't be done from create-store as it is loaded from the toolbox, without the browser loader
        //  and isn't bound to the netmonitor document)
        { store: visibilityHandlerStore(store) },
        LocalizationProvider(
          { bundles: fluentL10n.getBundles() },
          createElement(
            ToolboxProvider,
            { store: toolbox.store },
            createElement(App, {
              actions,
              connector,
              openLink,
              openSplitConsole,
              sourceMapURLService,
              toolboxDoc: toolbox.doc,
            })
          )
        )
      ),
      this.mount
    );
  },

  /**
   * Clean up (unmount from DOM, remove listeners, disconnect).
   */
  destroy() {
    unmountComponentAtNode(this.mount);

    SearchDispatcher.stop();

    // Make sure to destroy the API object. It's usually destroyed
    // in the Toolbox destroy method, but we need it here for case
    // where the Network panel is initialized without the toolbox
    // and running in a tab (see initialize.js for details).
    this.api.destroy();
  },

  /**
   * Selects the specified request in the waterfall and opens the details view.
   * This is a firefox toolbox specific API, which providing an ability to inspect
   * a network request directly from other internal toolbox panel.
   *
   * @param {string} requestId The actor ID of the request to inspect.
   * @return {object} A promise resolved once the task finishes.
   */
  async inspectRequest(requestId) {
    const { actions, store } = this.api;

    // Look for the request in the existing ones or wait for it to appear,
    // if the network monitor is still loading.
    return new Promise(resolve => {
      let request = null;
      const inspector = () => {
        request = getDisplayedRequestById(store.getState(), requestId);
        if (!request) {
          // Reset filters so that the request is visible.
          actions.toggleRequestFilterType("all");
          request = getDisplayedRequestById(store.getState(), requestId);
        }

        // If the request was found, select it. Otherwise this function will be
        // called again once new requests arrive.
        if (request) {
          this.api.off(EVENTS.REQUEST_ADDED, inspector);
          actions.selectRequest(request.id);
          resolve();
        }
      };

      inspector();

      if (!request) {
        this.api.on(EVENTS.REQUEST_ADDED, inspector);
      }
    });
  },
};

exports.NetMonitorApp = NetMonitorApp;
