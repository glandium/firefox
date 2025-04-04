// META: global=window,worker
// META: title=IndexedDB: IDBCursor method continuePrimaryKey()
// META: script=resources/support.js

// Spec: http://w3c.github.io/IndexedDB/#dom-idbcursor-continueprimarykey

'use strict';

indexeddb_test(
    (t, db, txn) => {
      const store = db.createObjectStore('store');
      const index = store.createIndex('index', 'indexKey', {multiEntry: true});

      store.put({indexKey: ['a', 'b']}, 1);
      store.put({indexKey: ['a', 'b']}, 2);
      store.put({indexKey: ['a', 'b']}, 3);
      store.put({indexKey: ['b']}, 4);

      const expectedIndexEntries = [
        {key: 'a', primaryKey: 1},
        {key: 'a', primaryKey: 2},
        {key: 'a', primaryKey: 3},
        {key: 'b', primaryKey: 1},
        {key: 'b', primaryKey: 2},
        {key: 'b', primaryKey: 3},
        {key: 'b', primaryKey: 4},
      ];

      const request = index.openCursor();
      request.onerror = t.unreached_func('IDBIndex.openCursor should not fail');
      request.onsuccess = t.step_func(() => {
        const cursor = request.result;
        const expectedEntry = expectedIndexEntries.shift();
        if (expectedEntry) {
          assert_equals(
              cursor.key, expectedEntry.key,
              'The index entry keys should reflect the object store contents');
          assert_equals(
              cursor.primaryKey, expectedEntry.primaryKey,
              'The index entry primary keys should reflect the object store ' +
                  'contents');
          cursor.continue();
        } else {
          assert_equals(
              cursor, null,
              'The index should not have entries that do not reflect the ' +
                  'object store contents');
        }
      });
    },
    (t, db) => {
      const testCases = [
        // Continuing index key.
        {
          call: cursor => {
            cursor.continue();
          },
          result: {key: 'a', primaryKey: 2}
        },
        {
          call: cursor => {
            cursor.continue('a');
          },
          exception: 'DataError'
        },
        {
          call: cursor => {
            cursor.continue('b');
          },
          result: {key: 'b', primaryKey: 1}
        },
        {
          call: cursor => {
            cursor.continue('c');
          },
          result: null
        },

        // Called w/ index key and primary key.
        {
          call: cursor => {
            cursor.continuePrimaryKey('a', 3);
          },
          result: {key: 'a', primaryKey: 3}
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey('a', 4);
          },
          result: {key: 'b', primaryKey: 1}
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey('b', 1);
          },
          result: {key: 'b', primaryKey: 1}
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey('b', 4);
          },
          result: {key: 'b', primaryKey: 4}
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey('b', 5);
          },
          result: null
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey('c', 1);
          },
          result: null
        },

        // Called w/ primary key but w/o index key.
        {
          call: cursor => {
            cursor.continuePrimaryKey(null, 1);
          },
          exception: 'DataError'
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey(null, 2);
          },
          exception: 'DataError'
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey(null, 3);
          },
          exception: 'DataError'
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey(null, 4);
          },
          exception: 'DataError'
        },
        {
          call: cursor => {
            cursor.continuePrimaryKey(null, 5);
          },
          exception: 'DataError'
        },

        // Called w/ index key but w/o primary key.
        {
          call: cursor => {
            cursor.continuePrimaryKey('a', null);
          },
          exception: 'DataError'
        },
      ];

      const verifyContinueCalls = () => {
        if (!testCases.length) {
          t.done();
          return;
        }

        const testCase = testCases.shift();

        const txn = db.transaction('store', 'readonly');
        txn.oncomplete = t.step_func(verifyContinueCalls);

        const request = txn.objectStore('store').index('index').openCursor();
        let calledContinue = false;
        request.onerror =
            t.unreached_func('IDBIndex.openCursor should not fail');
        request.onsuccess = t.step_func(() => {
          const cursor = request.result;
          if (calledContinue) {
            if (testCase.result) {
              assert_equals(
                  cursor.key, testCase.result.key,
                  `${testCase.call.toString()} - result key`);
              assert_equals(
                  cursor.primaryKey, testCase.result.primaryKey,
                  `${testCase.call.toString()} - result primary key`);
            } else {
              assert_equals(cursor, null);
            }
          } else {
            calledContinue = true;
            if ('exception' in testCase) {
              assert_throws_dom(testCase.exception, () => {
                testCase.call(cursor);
              }, testCase.call.toString());
            } else {
              testCase.call(cursor);
            }
          }
        });
      };
      verifyContinueCalls();
    });
