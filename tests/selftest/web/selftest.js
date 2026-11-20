// The page half of the uniapi self-test. C++ (main.cpp) drives the test: it calls runTests()
// for the JavaScript-to-C++ checks and the other functions exposed here for the rest.
'use strict';

// ?regions=all|later: ignore shared regions (all of them, or those arriving after connecting)
// to test the fallback to fetching; a short timeout keeps the test quick.
const regions = new URLSearchParams(location.search).get('regions');
const app = uniapi.connect(regions ? { page: 'selftest', _dropRegions: regions, regionTimeout: 500 }
                                   : { page: 'selftest' });

// A call made at once, before the connection is complete: C++ must see it after onConnect,
// when it can answer with calls and events of its own.
const earlyCall = app.native.connectedWhenCalled();

const held = [];  // resolvers of stream handlers that keep their slot until releaseHeld()
const stats = { frames: 0, bad: 0, problems: [] };

const patternByte = (seed, i) => (seed + i * 31) & 0xff;  // main.cpp writes the same pattern
const pattern = (size, seed) => {
    const bytes = new Uint8Array(size);
    for (let i = 0; i < size; i++) bytes[i] = patternByte(seed, i);
    return bytes;
};
// Size and weighted sum of some bytes; Checksum() in main.cpp computes the same sum.
const info = (bytes) => {
    let sum = 0;
    for (let i = 0; i < bytes.length; i++) sum += bytes[i] * (i % 251 + 1);
    return { size: bytes.length, sum };
};
const binLog = [];  // the "binlog" events from C++, for the ordering check
app.on('binlog', (d) => binLog.push(d.data ? `${d.n}:${d.data.length}` : `${d.n}`));
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(condition, what, ms = 5000) {
    const end = performance.now() + ms;
    while (!condition()) {
        if (performance.now() > end) throw new Error(`timed out waiting for ${what}`);
        await sleep(2);
    }
}

function deepEqual(a, b) {
    if (a === b) return true;
    if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) return false;
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    const keys = Object.keys(a);
    return keys.length === Object.keys(b).length && keys.every((k) => deepEqual(a[k], b[k]));
}

app.stream('test', (bytes, meta) => {
    stats.frames++;
    let problem = '';
    if (!(bytes instanceof Uint8Array)) {
        problem = 'bytes is not a Uint8Array';
    } else if (bytes.length !== meta.size) {
        problem = `got ${bytes.length} bytes, expected ${meta.size}`;
    } else {
        for (let i = 0; i < bytes.length; i++) {
            if (bytes[i] !== patternByte(meta.seed, i)) {
                problem = `byte ${i} is ${bytes[i]}, expected ${patternByte(meta.seed, i)}`;
                break;
            }
        }
    }
    if (problem) {
        stats.bad++;
        if (stats.problems.length < 5) stats.problems.push(`seed ${meta.seed}: ${problem}`);
    }
    if (meta.hold) return new Promise((resolve) => held.push(resolve));
});

// A stream name with a space and non-ASCII: its fetch URL is percent-encoded.
const spaced = { frames: 0, ok: false };
app.stream('two words ✓', (bytes, meta) => {
    spaced.frames++;
    spaced.ok = bytes.length === meta.size && bytes.every((b, i) => b === patternByte(meta.seed, i));
});

app.on('ping', (data) => app.emit('echo-back', data));

// app.off: two handlers of one event. jsOff takes one of them away ('a') or all, and reports
// which handlers have run since it was last asked.
const offRan = [];
const offA = () => offRan.push('a');
const offB = () => offRan.push('b');
app.on('off-test', offA).on('off-test', offB);
// app.unexpose, C++'s unexpose and app.unstream. jsGone is taken back by jsUnexpose; jsTryNative calls
// a C++ function and says how that went; the 'gone' stream's handler counts its frames until
// jsUnstream stops it.
let goneFrames = 0;
app.stream('gone', () => { goneFrames++; });
app.expose({
    jsGone: () => 'here',
    jsUnexpose: () => { app.unexpose('jsGone', 'jsNeverExposed'); },
    jsTryNative: async (name) => {
        try {
            await app.native[name]();
            return 'ok';
        } catch (err) {
            return err.message;
        }
    },
    jsGoneFrames: () => goneFrames,
    jsUnstream: () => { app.unstream('gone'); },
});

app.expose({
    jsOff: (which) => {
        if (which === 'a') app.off('off-test', offA);
        else app.off('off-test');
        return offRan.splice(0);
    },
});

app.expose({
    jsAdd: (a, b) => a + b,
    jsAsync: async (x) => {
        await sleep(5);
        return x * 2;
    },
    jsThrows: () => {
        throw new Error('js boom');
    },
    jsEcho: (value) => value,
    jsUndefined: () => undefined,
    never: () => new Promise(() => {}),

    async releaseHeld(count) {
        await waitFor(() => held.length >= count, `${count} held frames`);
        const n = held.length;
        held.splice(0).forEach((resolve) => resolve());
        return n;
    },

    // C++ checks frames and bad; problems and shared show up in a failed check's details.
    streamStats: () => ({
        frames: stats.frames,
        bad: stats.bad,
        problems: stats.problems,
        shared: app.capabilities.sharedMemory,
    }),

    async inspectDom() {
        const bound = document.getElementById('bind-target');
        await waitFor(() => bound.textContent === 'bound again', 'both DOM lists');
        const div = document.getElementById('dom-test');
        if (!div) return { missing: 'dom-test' };
        const list = div.querySelector('ul');
        return {
            text: div.textContent,
            from: div.dataset.from,
            color: div.style.color,
            className: div.className,
            children: div.children.length,
            listItems: list ? list.children.length : -1,
            inBody: div.parentElement === document.body,
            bound: bound.textContent,
        };
    },

    pageInfo: () => ({ search: location.search }),
    spacedStats: () => spaced,

    jsBytesInfo: (bytes) => ({ isBytes: bytes instanceof Uint8Array, ...info(bytes) }),
    jsMakeBytes: (size, seed) => pattern(size, seed),
    binLog: () => binLog,

    // Called at the end (on the reloaded page) with every check C++ recorded.
    showResults(summary, ok, checks) {
        const status = document.getElementById('status');
        status.textContent = summary;
        status.className = ok ? 'pass' : 'fail';
        const results = document.getElementById('results');
        results.replaceChildren();
        let step = null;
        for (const check of checks) {
            if (check.step !== step) {
                step = check.step;
                const heading = document.createElement('h2');
                heading.textContent = step;
                results.append(heading);
            }
            const row = document.createElement('div');
            row.className = check.note ? 'check note' : check.ok ? 'check ok' : 'check bad';
            const mark = document.createElement('span');
            mark.className = 'mark';
            mark.textContent = check.note ? 'i' : check.ok ? '✓' : '✗';
            const name = document.createElement('span');
            name.textContent = check.name;
            row.append(mark, name);
            if (check.detail) {
                const detail = document.createElement('span');
                detail.className = 'detail';
                detail.textContent = check.detail;
                row.append(detail);
            }
            results.append(row);
        }
    },

    runTests,
});

// The JavaScript-to-C++ checks. Returns [{name, ok, detail}] for C++ to print.
async function runTests() {
    const results = [];
    const check = (name, ok, detail) => results.push({ name, ok: !!ok, detail: ok ? '' : String(detail) });

    async function expectValue(name, promise, expected) {
        try {
            const value = await promise;
            check(name, deepEqual(value, expected), `got ${JSON.stringify(value)}`.slice(0, 200));
        } catch (err) {
            check(name, false, `rejected: ${err && err.message}`);
        }
    }

    async function expectError(name, promise, text) {
        try {
            const value = await promise;
            check(name, false, `resolved with ${JSON.stringify(value)}`);
        } catch (err) {
            check(name, err instanceof Error && err.message.includes(text), `message "${err && err.message}"`);
        }
    }

    const n = app.native;
    await expectValue('a typed C++ function returns a value', n.add(2, 3), 5);
    await expectValue('app.call() calls by name', app.call('add', 4, 5), 9);
    await expectValue('string arguments', n.concat('uni', 'api'), 'uniapi');
    const unicode = 'héllo ✓ 日本 \u{1F600}';
    await expectValue('UTF-8 text survives the round trip', n.echo(unicode), unicode);
    const nested = { b: [1, 2, { c: null }], a: true, n: -1.5, s: 'x', e: {}, l: [] };
    await expectValue('nested JSON survives the round trip', n.echo(nested), nested);
    const big = Array.from({ length: 20000 }, (_, i) => i);
    await expectValue('a 20,000-element array survives the round trip', n.echo(big), big);
    await expectValue('std::vector parameter', n.sum([1, 2, 3, 4.5]), 10.5);
    await expectValue('a void function resolves with null', n.nothing(), null);
    await expectError('a C++ exception rejects with its message', n.fail('boom'), 'boom');
    await expectError('a non-standard exception rejects', n.failOddly(), 'unknown C++ exception');
    await expectError('an unknown C++ function rejects', n.nope(), 'there is no C++ function named "nope"');
    await expectError('a wrong argument type rejects', n.add('x', 1), 'type must be number');
    await expectError('too few arguments reject', n.add(1), 'expected 2 argument(s)');
    await expectValue('extra arguments are ignored', n.add(1, 2, 3), 3);
    await expectValue('Reply resolves from a worker thread', n.doubleLater(21), 42);
    await expectError('Reply rejects from a worker thread', n.rejectLater(), 'later');
    await expectValue('the first answer wins', n.answerTwice(), 1);
    await expectValue('post() from a worker thread', n.postBack(), 'posted');
    try {
        const sums = await Promise.all(Array.from({ length: 200 }, (_, i) => n.add(i, i)));
        check('200 calls in flight at once', sums.every((v, i) => v === 2 * i), JSON.stringify(sums.slice(0, 5)));
    } catch (err) {
        check('200 calls in flight at once', false, `rejected: ${err.message}`);
    }
    check('app.native is not thenable', n.then === undefined, typeof n.then);
    check('capabilities.sharedMemory is a boolean', typeof app.capabilities.sharedMemory === 'boolean',
          typeof app.capabilities.sharedMemory);
    check('app.ready resolves to app', (await app.ready) === app, 'a different object');

    // Binary data to C++ (uniapi::Bytes).
    const small = pattern(1000, 7);
    await expectValue('a Uint8Array arrives as uniapi::Bytes', n.bytesInfo(small), info(small));
    const large = pattern(16 << 20, 3);
    await expectValue('16 MB arrive intact', n.bytesInfo(large), info(large));
    const part = large.subarray(100, 1100);
    await expectValue('a subarray sends only its own bytes', n.bytesInfo(part), info(part));
    await expectValue('an ArrayBuffer', n.bytesInfo(small.buffer.slice(0)), info(small));
    await expectValue('a DataView', n.bytesInfo(new DataView(small.buffer, 10, 20)), info(small.subarray(10, 30)));
    await expectValue('a Blob', n.bytesInfo(new Blob([small])), info(small));
    await expectValue('empty binary data', n.bytesInfo(new Uint8Array(0)), { size: 0, sum: 0 });
    await expectValue('two binary arguments and a string between them',
                      n.bytesPair(pattern(10, 1), 'x', pattern(20, 2)), 'x:10+20');
    await expectValue('binary data nested in objects and arrays',
                      n.bytesNested({ items: [1, { data: small }], name: 'n' }), 'n:1000');
    await expectValue('Bytes kept by a worker thread after the call', n.bytesLater(small), info(small).sum);
    await n.bytesAfterAnswer(small);
    await expectValue('the JSON placeholder stops working once the call is answered',
                      n.bytesGoneAfterAnswer(), true);
    await expectError('a made-up placeholder is refused', n.bytesInfo({ '$uniapi.bytes': 1 }), 'binary data');
    await expectError('a string where binary data is expected rejects', n.bytesInfo('text'),
                      'expected binary data');
    app.emit('log', { n: 1, data: pattern(1 << 20, 5) });
    app.emit('log', { n: 2 });
    app.emit('log', { n: 3, data: pattern(3, 1) });
    await expectValue('events with binary data stay in order with the rest', n.eventLog(),
                      ['1:1048576', '2', '3:3']);

    // Binary data from C++ (a uniapi::Bytes in what C++ returns).
    async function expectBytes(name, promise, problemWith) {
        try {
            const value = await promise;
            const problem = problemWith(value);  // '' when the value is right
            check(name, !problem, problem);
        } catch (err) {
            check(name, false, `rejected: ${err && err.message}`);
        }
    }
    const matches = (bytes, size, seed) => {
        if (!(bytes instanceof Uint8Array)) return `not a Uint8Array: ${Object.prototype.toString.call(bytes)}`;
        if (bytes.length !== size) return `length ${bytes.length}`;
        for (let i = 0; i < size; i++) if (bytes[i] !== patternByte(seed, i)) return `byte ${i} is ${bytes[i]}`;
        return '';
    };
    const text = (bytes) => (bytes instanceof Uint8Array ? new TextDecoder().decode(bytes) : String(bytes));
    await expectBytes('a returned uniapi::Bytes arrives as a Uint8Array', n.makeBytes(1000, 11),
                      (b) => matches(b, 1000, 11));
    await expectBytes('16 MB arrive intact from C++', n.makeBytes(16 << 20, 4), (b) => matches(b, 16 << 20, 4));
    await expectBytes('Bytes nested in objects and arrays', n.makeNested(), (v) =>
        v.name === 'n' && text(v.data) === 'abc' && v.list[0] === 1 && text(v.list[1]) === 'xy'
            ? '' : `got ${JSON.stringify(v)}`);
    await expectBytes('empty Bytes arrive as an empty Uint8Array', n.makeEmptyBytes(),
                      (b) => (b instanceof Uint8Array && b.length === 0 ? '' : `got ${b}`));
    await expectBytes('Bytes from Reply::resolve on a worker thread', n.makeBytesLater(10),
                      (b) => (b instanceof Uint8Array && b.length === 10 && b.every((x) => x === 7) ? '' : `got ${b}`));
    await expectBytes('bytes make the round trip page -> C++ -> page', n.echoBytes(small),
                      (b) => matches(b, 1000, 7));

    // C++ functions written as coroutines (uniapi::Task, C++20).
    await expectValue('an exposed coroutine answers when it finishes', n.coroAdd(2, 3), 5);
    await expectError('an exception after a co_await rejects the call', n.coroFail('cboom'), 'cboom');
    await expectError('an exception before the first co_await rejects too', n.coroThrowsAtOnce('early'), 'early');
    await expectValue('a Task<void> resolves with null', n.coroVoid(), null);
    await expectValue('a coroutine can do work on a background thread and come back',
                      n.coroBackground(1000), { sum: 499500, offUi: true, backOnUi: true });
    await expectValue('a coroutine can call JavaScript while the page awaits it', n.coroCallsJs(41), 42);
    await expectValue('a coroutine that finishes on a background thread answers', n.coroFinishOffUi(21), 42);

    // A page file whose name has spaces: the browser asks for it percent-encoded.
    try {
        const response = await fetch('file name with spaces.txt');
        const body = response.ok ? (await response.text()).trim() : `status ${response.status}`;
        check('a file whose name has spaces is served', body === 'spaces ok', body);
    } catch (err) {
        check('a file whose name has spaces is served', false, err.message);
    }

    // An embedded text file stored compressed (uniapi_embed) arrives whole.
    try {
        const response = await fetch('embed/pattern.txt');
        const body = response.ok ? await response.text() : `status ${response.status}`;
        const expected = 'uniapi embeds this line, compressed and back.\n'.repeat(300);
        check('an embedded file stored compressed is served whole', body === expected,
              `${body.length} characters, ${response.headers.get('content-type')}`);
    } catch (err) {
        check('an embedded file stored compressed is served whole', false, err.message);
    }

    // Minified JSON and SVG still say what they said.
    try {
        const config = await (await fetch('embed/config.json')).json();
        check('a minified embedded JSON file reads the same', JSON.stringify(config) === JSON.stringify({
            name: 'uniapi minify check', levels: [-60, -18, 0.5, 6], nested: { on: true, text: 'spaces  inside  strings  stay', nothing: null },
        }), JSON.stringify(config));
        const image = new Image();
        image.src = 'embed/icon.svg';
        await image.decode();
        check('a minified embedded SVG still loads as an image of its size', image.naturalWidth === 24 && image.naturalHeight === 24,
              `${image.naturalWidth} x ${image.naturalHeight}`);
    } catch (err) {
        check('minified embedded JSON and SVG files still work', false, err.message);
    }

    // The minified module still does what it did.
    try {
        const module = await import('./embed/minify.js');
        check('a minified embedded module still works', module.answer() === 42, String(module.answer()));
    } catch (err) {
        check('a minified embedded module still works', false, err.message);
    }

    await expectValue('a call made right after connect() reaches C++ after onConnect', earlyCall, true);

    // Half an emoji (a lone surrogate, e.g. from slice()) isn't valid Unicode: C++ refused the
    // whole message and the call never answered. Now it arrives as U+FFFD.
    const answerWithin = (promise, ms) => Promise.race([promise, sleep(ms).then(() => {
        throw new Error(`no answer within ${ms} ms`);
    })]);
    await expectValue('a string holding half an emoji arrives, with U+FFFD for the half',
                      answerWithin(n.echo('a\uD83Db'), 2000), 'a�b');
    let second = null;
    try {
        uniapi.connect({ page: 'second' });
    } catch (err) {
        second = err.message;
    }
    check('a second connect() on the same page is refused', second && second.includes('already'), String(second));

    // A cross-origin iframe (a data: URL) must not reach C++ by any route. It found the native
    // message handler on Linux before the handler moved into its own script world.
    const iframeSays = new Promise((resolve) => {
        const listener = (event) => {
            if (typeof event.data !== 'string' || !event.data.startsWith('probe:')) return;
            window.removeEventListener('message', listener);
            resolve(event.data.slice(6));
        };
        window.addEventListener('message', listener);
    });
    const frame = document.createElement('iframe');
    frame.hidden = true;
    frame.src = 'data:text/html,' + encodeURIComponent(`<script>
        const msg = JSON.stringify({ t: 'call', id: 99999, fn: 'calledFromIframe', args: [] });
        const tried = [];
        const attempt = (name, fn) => { try { fn(); tried.push(name + ' went through'); } catch (err) { tried.push(name + ' refused'); } };
        attempt('webkit handler', () => window.webkit.messageHandlers.uniapi.postMessage(msg));
        attempt('chrome.webview', () => window.chrome.webview.postMessage(msg));
        attempt('parent.__uniapiPost', () => parent.__uniapiPost(msg));
        attempt('parent document', () => parent.document.dispatchEvent(new CustomEvent('uniapi:post', { detail: msg })));
        parent.postMessage('probe:' + tried.join(', '), '*');
    <\/script>`);
    document.body.append(frame);
    const said = await answerWithin(iframeSays, 3000).catch((err) => err.message);
    await sleep(200);  // anything the iframe sent has arrived by now
    frame.remove();
    await expectValue(`a cross-origin iframe can't call C++ (it tried: ${said})`, n.wasCalledFromIframe(), false);

    // Robustness (found in the code audit).
    await expectValue('a function that exposes its own name again finishes its call', n.reexpose(), 'first version');
    await expectValue('...and the new version answers the next call', n.reexpose(), 'second version');
    app.emit('throwing', {});
    await expectValue('a throwing event handler leaves the app working', n.add(1, 1), 2);
    let badArgs;
    try {
        // Binary data sends it through the queue; the BigInt then fails JSON (the old bug: the
        // queue stayed rejected and failed the next queued message).
        badArgs = await n.bytesInfo(new Uint8Array([1]), 10n);
    } catch (err) {
        badArgs = `rejected: ${err.message}`;
    }
    check('a call with arguments JSON cannot hold rejects', String(badArgs).startsWith('rejected'), badArgs);
    let afterBad;
    try {
        afterBad = await n.bytesInfo(new Uint8Array([1, 2]));  // goes through the send queue
    } catch (err) {
        afterBad = `rejected: ${err.message}`;
    }
    check('...and the next message still goes out', afterBad && afterBad.size === 2, JSON.stringify(afterBad));
    return results;
}
