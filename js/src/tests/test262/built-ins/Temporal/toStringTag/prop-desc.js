// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2021 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal-@@tostringtag
description: The @@toStringTag property of Temporal
includes: [propertyHelper.js]
features: [Symbol.toStringTag, Temporal]
---*/

verifyProperty(Temporal, Symbol.toStringTag, {
  value: "Temporal",
  writable: false,
  enumerable: false,
  configurable: true,
});

reportCompare(0, 0);
