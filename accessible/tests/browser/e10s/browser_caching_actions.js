/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const gClickEvents = ["mousedown", "mouseup", "click"];

const gActionDescrMap = {
  jump: "Jump",
  press: "Press",
  check: "Check",
  uncheck: "Uncheck",
  select: "Select",
  open: "Open",
  close: "Close",
  switch: "Switch",
  click: "Click",
  collapse: "Collapse",
  expand: "Expand",
  activate: "Activate",
  cycle: "Cycle",
  clickAncestor: "Click ancestor",
};

async function testActions(browser, docAcc, id, expectedActions, domEvents) {
  info(`Testing element ${id}`);
  const acc = findAccessibleChildByID(docAcc, id);
  is(acc.actionCount, expectedActions.length, "Correct action count");

  let actionNames = [];
  let actionDescriptions = [];
  for (let i = 0; i < acc.actionCount; i++) {
    actionNames.push(acc.getActionName(i));
    actionDescriptions.push(acc.getActionDescription(i));
  }

  is(actionNames.join(","), expectedActions.join(","), "Correct action names");
  is(
    actionDescriptions.join(","),
    expectedActions.map(a => gActionDescrMap[a]).join(","),
    "Correct action descriptions"
  );

  if (!domEvents) {
    return;
  }

  // We need to set up the listener, and wait for the promise in two separate
  // content tasks.
  await invokeContentTask(browser, [id, domEvents], (_id, _domEvents) => {
    let promises = _domEvents.map(
      evtName =>
        new Promise(resolve => {
          const listener = e => {
            if (e.target.id == _id) {
              content.removeEventListener(evtName, listener);
              resolve(42);
            }
          };
          content.addEventListener(evtName, listener);
        })
    );
    content.evtPromise = Promise.all(promises);
  });

  acc.doAction(0);

  let eventFired = await invokeContentTask(browser, [], async () => {
    await content.evtPromise;
    content.evtPromise = null;
    return true;
  });

  ok(eventFired, `DOM events fired '${domEvents}'`);
}

addAccessibleTask(
  `<ul>
    <li id="li_clickable1" data-event="click">Clickable list item</li>
    <li id="li_clickable2" data-event="mousedown">Clickable list item</li>
    <li id="li_clickable3" data-event="mouseup">Clickable list item</li>
  </ul>

  <img id="onclick_img" data-event="click"
        src="http://example.com/a11y/accessible/tests/mochitest/moz.png">

  <a id="link1" href="#">linkable textleaf accessible</a>
  <div id="link2" data-event="click">linkable textleaf accessible</div>

  <a id="link3" href="#">
    <img id="link3img" alt="image in link"
          src="http://example.com/a11y/accessible/tests/mochitest/moz.png">
  </a>

  <a href="about:mozilla" id="link4" target="_blank" rel="opener">
    <img src="../moz.png" id="link4img">
  </a>
  <a id="link5" data-event="mousedown">
    <img src="../moz.png" id="link5img">
  </a>
  <a id="link6" data-event="click">
    <img src="../moz.png" id="link6img">
  </a>
  <a id="link7" data-event="mouseup">
    <img src="../moz.png" id="link7img">
  </a>

  <div>
    <label for="TextBox_t2" id="label1">
      <span>Explicit</span>
    </label>
    <input name="in2" id="TextBox_t2" type="text" maxlength="17">
  </div>

  <div data-event="click"><p id="p_in_clickable_div">p in clickable div</p></div>

  <img id="map_img" usemap="#map" src="http://example.com/a11y/accessible/tests/mochitest/moz.png" alt="map_img">
  <map name="map">
    <!-- These coords are deliberately small so that the area does not include
      the center of the image.
      -->
    <area id="area" href="#" shape="rect" coords="0,0,2,2" alt="area">
  </map>
  `,
  async function (browser, docAcc) {
    is(docAcc.actionCount, 0, "Doc should not have any actions");

    const _testActions = async (id, expectedActions, domEvents) => {
      await testActions(browser, docAcc, id, expectedActions, domEvents);
    };

    await _testActions("li_clickable1", ["click"], gClickEvents);
    await _testActions("li_clickable2", ["click"], gClickEvents);
    await _testActions("li_clickable3", ["click"], gClickEvents);

    await _testActions("onclick_img", ["click"], gClickEvents);
    await _testActions("link1", ["jump"], gClickEvents);
    await _testActions("link2", ["click"], gClickEvents);
    await _testActions("link3", ["jump"], gClickEvents);
    await _testActions("link3img", ["clickAncestor"], gClickEvents);
    await _testActions("link4", ["jump"], gClickEvents);
    await _testActions("link4img", ["clickAncestor"], gClickEvents);
    await _testActions("link5", ["click"], gClickEvents);
    await _testActions("link5img", ["clickAncestor"], gClickEvents);
    await _testActions("link6", ["click"], gClickEvents);
    await _testActions("link6img", ["clickAncestor"], gClickEvents);
    await _testActions("link7", ["click"], gClickEvents);
    await _testActions("link7img", ["clickAncestor"], gClickEvents);
    await _testActions("label1", ["click"], gClickEvents);
    await _testActions("p_in_clickable_div", ["clickAncestor"], gClickEvents);
    await _testActions("area", ["jump"], gClickEvents);

    await invokeContentTask(browser, [], () => {
      content.document.getElementById("li_clickable1").onclick = null;
    });

    let acc = findAccessibleChildByID(docAcc, "li_clickable1");
    await untilCacheIs(() => acc.actionCount, 0, "li has no actions");
    let thrown = false;
    try {
      acc.doAction(0);
    } catch (e) {
      thrown = true;
    }
    ok(thrown, "doAction should throw exception");

    // Remove 'for' from label
    await invokeContentTask(browser, [], () => {
      content.document.getElementById("label1").removeAttribute("for");
    });
    acc = findAccessibleChildByID(docAcc, "label1");
    await untilCacheIs(() => acc.actionCount, 0, "label has no actions");
    thrown = false;
    try {
      acc.doAction(0);
      ok(false, "doAction should throw exception");
    } catch (e) {
      thrown = true;
    }
    ok(thrown, "doAction should throw exception");

    // Add 'longdesc' to image
    await invokeContentTask(browser, [], () => {
      content.document
        .getElementById("onclick_img")
        // eslint-disable-next-line @microsoft/sdl/no-insecure-url
        .setAttribute("longdesc", "http://example.com");
    });
    acc = findAccessibleChildByID(docAcc, "onclick_img");
    await untilCacheIs(() => acc.actionCount, 2, "img has 2 actions");
    await _testActions("onclick_img", ["click", "showlongdesc"]);

    // Remove 'onclick' from image with 'longdesc'
    await invokeContentTask(browser, [], () => {
      content.document.getElementById("onclick_img").onclick = null;
    });
    acc = findAccessibleChildByID(docAcc, "onclick_img");
    await untilCacheIs(() => acc.actionCount, 1, "img has 1 actions");
    await _testActions("onclick_img", ["showlongdesc"]);

    // Remove 'href' from link and test linkable child
    let link1Acc = findAccessibleChildByID(docAcc, "link1");
    is(
      link1Acc.firstChild.getActionName(0),
      "clickAncestor",
      "linkable child has clickAncestor action"
    );
    let onRecreation = waitForEvents({
      expected: [
        [EVENT_HIDE, link1Acc],
        [EVENT_SHOW, "link1"],
      ],
    });
    await invokeContentTask(browser, [], () => {
      let link1 = content.document.getElementById("link1");
      link1.removeAttribute("href");
    });
    await onRecreation;
    link1Acc = findAccessibleChildByID(docAcc, "link1");
    await untilCacheIs(() => link1Acc.actionCount, 0, "link has no actions");
    is(link1Acc.firstChild.actionCount, 0, "linkable child's actions removed");

    // Add a click handler to the body. Ensure it propagates to descendants.
    await invokeContentTask(browser, [], () => {
      content.document.body.onclick = () => {};
    });
    await untilCacheIs(() => docAcc.actionCount, 1, "Doc has 1 action");
    await _testActions("link1", ["clickAncestor"]);

    await invokeContentTask(browser, [], () => {
      content.document.body.onclick = null;
    });
    await untilCacheIs(() => docAcc.actionCount, 0, "Doc has no actions");
    is(link1Acc.actionCount, 0, "link has no actions");

    // Add a click handler to the root element. Ensure it propagates to
    // descendants.
    await invokeContentTask(browser, [], () => {
      content.document.documentElement.onclick = () => {};
    });
    await untilCacheIs(() => docAcc.actionCount, 1, "Doc has 1 action");
    await _testActions("link1", ["clickAncestor"]);
  },
  {
    chrome: true,
    topLevel: true,
    iframe: true,
    remoteIframe: true,
    contentSetup: async function contentSetup() {
      // Attach dummy event handlers here, because inline event handler attributes
      // will be blocked in the chrome context.
      for (const el of content.document.querySelectorAll("[data-event]")) {
        el["on" + el.dataset.event] = () => {};
      }
    },
  }
);

/**
 * Test access key.
 */
addAccessibleTask(
  `
<button id="noKey">noKey</button>
<button id="key" accesskey="a">key</button>
  `,
  async function (browser, docAcc) {
    const noKey = findAccessibleChildByID(docAcc, "noKey");
    is(noKey.accessKey, "", "noKey has no accesskey");
    const key = findAccessibleChildByID(docAcc, "key");
    is(key.accessKey, MAC ? "⌃⌥a" : "Alt+Shift+a", "key has correct accesskey");

    info("Changing accesskey");
    await invokeContentTask(browser, [], () => {
      content.document.getElementById("key").accessKey = "b";
    });
    await untilCacheIs(
      () => key.accessKey,
      MAC ? "⌃⌥b" : "Alt+Shift+b",
      "Correct accesskey after change"
    );

    info("Removing accesskey");
    await invokeContentTask(browser, [], () => {
      content.document.getElementById("key").removeAttribute("accesskey");
    });
    await untilCacheIs(
      () => key.accessKey,
      "",
      "Empty accesskey after removal"
    );

    info("Adding accesskey");
    await invokeContentTask(browser, [], () => {
      content.document.getElementById("key").accessKey = "c";
    });
    await untilCacheIs(
      () => key.accessKey,
      MAC ? "⌃⌥c" : "Alt+Shift+c",
      "Correct accesskey after addition"
    );
  },
  {
    chrome: true,
    topLevel: true,
    iframe: false, // Bug 1796846
    remoteIframe: false, // Bug 1796846
  }
);
