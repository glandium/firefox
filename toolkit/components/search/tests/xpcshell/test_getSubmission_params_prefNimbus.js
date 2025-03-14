/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/* Test that preference parameters used in search URLs can be set
   by Nimbus, and that their special characters are URL encoded. */

"use strict";

const { NimbusFeatures } = ChromeUtils.importESModule(
  "resource://nimbus/ExperimentAPI.sys.mjs"
);

const baseURL = "https://example.com/search?";

let getVariableStub;
let updateStub;

const CONFIG = [
  {
    identifier: "preferenceEngine",
    base: {
      urls: {
        search: {
          base: "https://example.com/search",
          params: [
            {
              name: "code",
              experimentConfig: "code",
            },
            {
              name: "test",
              experimentConfig: "test",
            },
          ],
          searchTermParamName: "q",
        },
      },
    },
  },
];

add_setup(async function () {
  updateStub = sinon.stub(NimbusFeatures.searchConfiguration, "onUpdate");
  getVariableStub = sinon.stub(
    NimbusFeatures.searchConfiguration,
    "getVariable"
  );
  sinon.stub(NimbusFeatures.searchConfiguration, "ready").resolves();

  SearchTestUtils.setRemoteSettingsConfig(CONFIG);

  registerCleanupFunction(async () => {
    sinon.restore();
  });
});

add_task(async function test_pref_initial_value() {
  // These values should match the nimbusParams below and the data/test/manifest.json
  // search engine configuration
  getVariableStub.withArgs("extraParams").returns([
    {
      key: "code",
      // The & and = in this parameter are to check that they are correctly
      // encoded, and not treated as a separate parameter.
      value: "good&id=unique",
    },
  ]);

  await Services.search.init();

  Assert.ok(
    updateStub.called,
    "Should have called onUpdate to listen for future updates"
  );

  const engine = Services.search.getEngineById("preferenceEngine");
  Assert.equal(
    engine.getSubmission("foo").uri.spec,
    baseURL + "code=good%26id%3Dunique&q=foo",
    "Should have got the submission URL with the correct code"
  );
});

add_task(async function test_pref_updated() {
  getVariableStub.withArgs("extraParams").returns([
    {
      key: "code",
      // The & and = in this parameter are to check that they are correctly
      // encoded, and not treated as a separate parameter.
      value: "supergood&id=unique123456",
    },
  ]);
  // Update the pref without re-init nor restart.
  updateStub.firstCall.args[0]();

  const engine = Services.search.getEngineById("preferenceEngine");
  Assert.equal(
    engine.getSubmission("foo").uri.spec,
    baseURL + "code=supergood%26id%3Dunique123456&q=foo",
    "Should have got the submission URL with the updated code"
  );
});

add_task(async function test_multiple_params() {
  getVariableStub.withArgs("extraParams").returns([
    {
      key: "code",
      value: "sng",
    },
    {
      key: "test",
      value: "sup",
    },
  ]);
  // Update the pref without re-init nor restart.
  updateStub.firstCall.args[0]();

  let engine = Services.search.getEngineById("preferenceEngine");
  Assert.equal(
    engine.getSubmission("foo").uri.spec,
    baseURL + "code=sng&test=sup&q=foo",
    "Should have got the submission URL with both parameters"
  );

  // Test removing just one of the parameters.
  getVariableStub.withArgs("extraParams").returns([
    {
      key: "code",
      value: "sng",
    },
  ]);
  // Update the pref without re-init nor restart.
  updateStub.firstCall.args[0]();

  engine = Services.search.getEngineById("preferenceEngine");
  Assert.equal(
    engine.getSubmission("foo").uri.spec,
    baseURL + "code=sng&q=foo",
    "Should have got the submission URL with one parameter"
  );
});

add_task(async function test_pref_cleared() {
  // Update the pref without re-init nor restart.
  // Note you can't delete a preference from the default branch.
  getVariableStub.withArgs("extraParams").returns([]);
  updateStub.firstCall.args[0]();

  let engine = Services.search.getEngineById("preferenceEngine");
  Assert.equal(
    engine.getSubmission("foo").uri.spec,
    baseURL + "q=foo",
    "Should have just the base URL after the pref was cleared"
  );
});
