// Flags: --experimental-modules --experimental-json-modules
/* eslint-disable node-core/required-modules */

import '../common/index.mjs';
import { strictEqual } from 'assert';

import secret from '../fixtures/experimental.json';
import * as namespace from '../fixtures/experimental.json';

strictEqual(secret.ofLife, 42);
strictEqual(namespace.ofLife, undefined);
strictEqual(namespace.default.ofLife, 42);
