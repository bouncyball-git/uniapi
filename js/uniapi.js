// uniapi: the page side. Load it with <script src="/uniapi/uniapi.js"></script>;
// the app serves it itself.
//
//   const app = uniapi.connect({ page: 'demo' });
//   const sum = await app.native.add(1, 2);          // runs a C++ function (app.expose)
//   app.expose({ greet(name) { return 'hi ' + name; } });   // C++ can call these
//   app.unexpose('greet');                            // no longer callable
//   app.on('progress', (data) => ...);                // events from C++ (app.emit)
//   app.off('progress', handler);                     // stops a handler (without one: all of them)
//   app.emit('clicked', { id: 3 });                   // events to C++ (app.on)
//   app.stream('frames', (bytes, meta) => ...);       // large data from C++ (App::stream)
//   app.unstream('frames');                           // stops receiving it
//   await app.native.thumbnail(file, 200);            // binary data to C++ (uniapi::Bytes)
//
// Binary values (ArrayBuffer, typed arrays, DataView, Blob/File) can go anywhere in a call's
// arguments or an event's data. They are sent outside the JSON, without base64; C++ takes
// them as uniapi::Bytes. Messages still reach C++ in the order you send them.
//
// Register handlers right after connect(), in the same task: the app only starts sending
// once the page is ready, and that can't happen before your code yields.
//
// Stream bytes are a view of shared memory when the platform supports it (Windows, Linux), and a
// fetched copy otherwise. Either way they are valid only until the handler returns or the
// promise it returns settles; copy anything you keep (bytes.slice()).
//
// The protocol is described at the top of src/core/app.cpp.
(function () {
    'use strict';

    const DOM_STREAM = 'uniapi.dom';
    const UPLOAD_KEY = '$uniapi.upload';      // stands in for binary data; see src/core/app.cpp
    const DOWNLOAD_KEY = '$uniapi.download';  // binary data from C++, fetched before use

    // --- Binary data in calls and events ---------------------------------------------------

    function isBinary(value) {
        return value instanceof ArrayBuffer || ArrayBuffer.isView(value) ||
            (typeof Blob !== 'undefined' && value instanceof Blob);
    }

    function isPlainObject(value) {
        if (value === null || typeof value !== 'object') return false;
        const proto = Object.getPrototypeOf(value);
        return proto === Object.prototype || proto === null;
    }

    function containsBinary(value, depth = 0) {
        if (isBinary(value)) return true;
        if (depth > 64) return false;
        if (Array.isArray(value)) return value.some((item) => containsBinary(item, depth + 1));
        if (isPlainObject(value)) return Object.values(value).some((item) => containsBinary(item, depth + 1));
        return false;
    }

    // A copy of `value` with each binary value replaced by a placeholder, which `found`
    // collects as {placeholder, body}.
    function replaceBinary(value, found, depth = 0) {
        if (isBinary(value)) {
            const placeholder = {};
            found.push({ placeholder, body: value });
            return placeholder;
        }
        if (depth > 64) return value;
        if (Array.isArray(value)) return value.map((item) => replaceBinary(item, found, depth + 1));
        if (isPlainObject(value)) {
            const copy = {};
            for (const [key, item] of Object.entries(value)) copy[key] = replaceBinary(item, found, depth + 1);
            return copy;
        }
        return value;
    }

    // A JSON.stringify replacer. Half an emoji (a lone surrogate, e.g. after slice()) isn't
    // valid Unicode; C++'s JSON parser refuses it and would drop the whole message, leaving a
    // call unanswered. It is sent as U+FFFD instead, as TextEncoder does.
    const LONE_SURROGATE = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/g;
    function wellFormed(key, value) {
        if (typeof value !== 'string') return value;
        if (value.isWellFormed ? value.isWellFormed() : !/[\uD800-\uDFFF]/.test(value)) return value;
        return value.replace(LONE_SURROGATE, '�');
    }

    // --- Transport: the platform's message channel ------------------------------------

    function createTransport() {
        const webview = window.chrome && window.chrome.webview;
        if (webview) {  // Windows, WebView2
            return {
                post: (text) => webview.postMessage(text),
                onMessage: (listener) => webview.addEventListener('message', (e) => listener(e.data)),
                onRegion: (listener) => webview.addEventListener('sharedbufferreceived', (e) =>
                    listener(e.additionalData, e.getBuffer(), (buffer) => webview.releaseBuffer(buffer))),
            };
        }
        // WebKitGTK (Linux): the app gives the top frame a bridge to its message handler, which
        // is kept out of reach of iframes (see AddBridge in backend_linux.cpp).
        const post = window.__uniapiPost;
        if (typeof post === 'function') {
            const messageListeners = [];
            const regionListeners = [];
            // Native code calls these: messages through evaluate_javascript, shared regions from
            // the web process extension.
            window.__uniapi = {
                receive: (text) => messageListeners.forEach((l) => l(text)),
                region: (info, buffer) => regionListeners.forEach((l) => l(info, buffer, () => {})),
            };
            return {
                post: (text) => post(text),
                onMessage: (listener) => messageListeners.push(listener),
                onRegion: (listener) => regionListeners.push(listener),
            };
        }
        throw new Error('uniapi: this page is not running inside a uniapi app');
    }

    // --- DOM command lists (see include/uniapi/dom.hpp) -------------------------------

    function createDomApplier() {
        const nodes = new Map();  // ID 0 is document.body, looked up when used (connect() may run in <head>)
        const decoder = new TextDecoder();
        return (bytes) => {
            const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
            let offset = 0;
            const u8 = () => view.getUint8(offset++);
            const u32 = () => {
                const value = view.getUint32(offset, true);
                offset += 4;
                return value;
            };
            const string = () => {
                const length = u32();
                const text = decoder.decode(bytes.subarray(offset, offset + length));
                offset += length;
                return text;
            };
            const node = (id) => {
                const element = id === 0 ? document.body : nodes.get(id);
                if (!element) throw new Error(`uniapi: unknown element id ${id}`);
                return element;
            };
            while (offset < bytes.byteLength) {
                const op = u8();
                switch (op) {
                case 1: {  // Bind
                    const id = u32();
                    const selector = string();
                    const element = document.querySelector(selector);
                    if (!element) throw new Error(`uniapi: nothing matches "${selector}"`);
                    nodes.set(id, element);
                    break;
                }
                case 2: {  // Create
                    const id = u32();
                    nodes.set(id, document.createElement(string()));
                    break;
                }
                case 3: {  // Append
                    const parent = node(u32());
                    parent.append(node(u32()));
                    break;
                }
                case 4: {  // Remove
                    const id = u32();
                    node(id).remove();
                    nodes.delete(id);
                    break;
                }
                case 5:  // Clear
                    node(u32()).replaceChildren();
                    break;
                case 6: {  // SetText
                    const element = node(u32());
                    element.textContent = string();
                    break;
                }
                case 7: {  // SetAttribute
                    const element = node(u32());
                    const name = string();
                    element.setAttribute(name, string());
                    break;
                }
                case 8: {  // SetStyle
                    const element = node(u32());
                    const property = string();
                    element.style.setProperty(property, string());
                    break;
                }
                case 9: {  // ToggleClass
                    const element = node(u32());
                    const name = string();
                    element.classList.toggle(name, u8() !== 0);
                    break;
                }
                default:
                    // The rest of the list can't be decoded once an opcode is unknown.
                    throw new Error(`uniapi: unknown DOM opcode ${op} at byte ${offset - 1}`);
                }
            }
        };
    }

    // --- connect() ----------------------------------------------------------------------

    let connected = false;

    function connect(options = {}) {
        // One connection per page: a second one would restart the app's session under it,
        // failing the first one's calls. An iframe would take over the page's session the same way.
        if (connected) throw new Error('uniapi: connect() was already called on this page');
        if (window.top !== window) throw new Error('uniapi: connect() works in the top-level page, not in an iframe');
        const transport = createTransport();
        connected = true;
        const pageName = options.page || 'page';
        const exposed = new Map();        // functions C++ may call
        const eventHandlers = new Map();  // name -> [handler]
        const streamHandlers = new Map([[DOM_STREAM, createDomApplier()]]);
        const pendingCalls = new Map();   // id -> { resolve, reject }
        let nextCallId = 1;

        // Streams by name: { id, slots, shared }. A replaced stream gets a new id.
        const streams = new Map();
        // Shared regions by stream id. They may arrive before or after the messages that
        // mention them, so they are kept by id and waited for when missing.
        const regions = new Map();        // id -> [ArrayBuffer per slot]
        const regionWaiters = new Map();  // "id/slot" -> [resolve]
        const retired = new Set();        // ids of replaced streams
        let releaseRegion = () => {};
        // A region that never arrives must not hang the page: after this long the page fetches
        // instead (all streams when it happens while connecting, else just that stream).
        const regionTimeout = options.regionTimeout ?? 3000;
        // Testing only (tests/selftest): ignore shared regions, 'all' or those arriving 'later'
        // than the connection, to exercise that fallback.
        const dropRegions = options._dropRegions;
        let sharedOk = true;  // false once this page gave up on shared memory
        let welcomed = false;
        let isReady = false;
        let resolveReady;
        const ready = new Promise((resolve) => { resolveReady = resolve; });
        const capabilities = { sharedMemory: false };

        const send = (message) => transport.post(JSON.stringify(message, wellFormed));

        // Calls and events with binary data wait for their uploads; later messages queue behind
        // them so C++ still sees everything in order. Until the connection is ready they all
        // wait (the first entry in the queue is `ready` itself), so C++ gets them after onConnect,
        // when it can answer with calls and events of its own.
        let resolveSession;  // the session from the welcome message names this page's uploads
        const sessionKnown = new Promise((resolve) => { resolveSession = resolve; });
        let nextUpload = 1;
        let queued = 1;
        let queue = ready.then(() => { queued--; });

        async function upload(number, body) {
            // WebKitGTK crashes reading a Blob request body in the app (2.52), so send its bytes.
            if (typeof Blob !== 'undefined' && body instanceof Blob) body = await body.arrayBuffer();
            const id = await sessionKnown;
            const response = await fetch(`/uniapi/upload/${id}/${number}`, {
                method: 'POST',
                body,
                headers: { 'Content-Type': 'application/octet-stream' },
            });
            if (!response.ok) throw new Error(`sending binary data failed (${response.status})`);
        }

        // Sends `message` once `field` (its args or data) is uploaded. onError gets the error if
        // an upload fails or the message can't be sent (e.g. a value JSON can't hold, like a BigInt).
        function sendWithBinary(message, field, onError) {
            let uploads = null;
            if (containsBinary(message[field])) {
                const found = [];
                message[field] = replaceBinary(message[field], found);
                uploads = Promise.all(found.map(({ placeholder, body }) => {
                    const number = nextUpload++;
                    placeholder[UPLOAD_KEY] = number;
                    return upload(number, body);
                }));
                uploads.catch(() => {});  // handled in the queue below, not "unhandled"
                message.uploads = true;
            }
            if (!uploads && queued === 0) {
                try {
                    send(message);
                } catch (err) {
                    onError(err);
                }
                return;
            }
            queued++;
            const waitFor = uploads || Promise.resolve();
            // The catch keeps the queue itself from rejecting, so one failed message can't skip
            // or fail the messages after it.
            queue = queue.then(() => waitFor).then(() => send(message)).catch(onError).finally(() => queued--);
        }

        function addStream(info) {
            const previous = streams.get(info.name);
            if (previous && previous.id !== info.id) {
                retired.add(previous.id);
                for (const buffer of regions.get(previous.id) || []) if (buffer) releaseRegion(buffer);
                regions.delete(previous.id);
            }
            streams.set(info.name, {
                id: info.id, slots: info.slots, shared: info.shared && sharedOk,
            });
        }

        // The regions didn't all arrive while connecting: fetch every stream from now on.
        function giveUpSharedMemory() {
            console.warn(`uniapi: shared memory regions didn't arrive within ${regionTimeout} ms; ` +
                         'streams use the fetch fallback');
            sharedOk = false;
            capabilities.sharedMemory = false;
            for (const entry of streams.values()) entry.shared = false;
            for (const list of regions.values()) for (const buffer of list) if (buffer) releaseRegion(buffer);
            regions.clear();
            for (const waiters of regionWaiters.values()) waiters.forEach((resolve) => resolve());
            regionWaiters.clear();
        }

        function missingRegions() {
            let missing = 0;
            for (const entry of streams.values()) {
                if (!entry.shared) continue;
                const list = regions.get(entry.id) || [];
                for (let slot = 0; slot < entry.slots; slot++) if (!list[slot]) missing++;
            }
            return missing;
        }

        function checkReady() {
            if (isReady || !welcomed || missingRegions() > 0) return;
            isReady = true;
            send(sharedOk ? { t: 'ready' } : { t: 'ready', sharedMemory: false });
            resolveReady(app);
        }

        // The region, or null if it doesn't arrive within regionTimeout.
        function regionFor(id, slot) {
            const buffer = (regions.get(id) || [])[slot];
            if (buffer) return buffer;
            return new Promise((resolve) => {
                const key = `${id}/${slot}`;
                const done = () => {
                    clearTimeout(timer);
                    resolve((regions.get(id) || [])[slot] || null);
                };
                const timer = setTimeout(() => {
                    const waiters = regionWaiters.get(key) || [];
                    const index = waiters.indexOf(done);
                    if (index >= 0) waiters.splice(index, 1);
                    if (waiters.length === 0) regionWaiters.delete(key);
                    resolve(null);
                }, regionTimeout);
                if (!regionWaiters.has(key)) regionWaiters.set(key, []);
                regionWaiters.get(key).push(done);
            });
        }

        transport.onRegion((info, buffer, release) => {
            releaseRegion = release;
            const dropped = dropRegions === 'all' || (dropRegions === 'later' && isReady);
            // Not wanted: a replaced stream's, after giving up on shared memory, or for a stream
            // that already switched to fetching because its regions came too late.
            const fetching = (id) => [...streams.values()].some((entry) => entry.id === id && !entry.shared);
            if (!info || info.id === undefined || retired.has(info.id) || !sharedOk || dropped || fetching(info.id)) {
                release(buffer);
                return;
            }
            if (!regions.has(info.id)) regions.set(info.id, []);
            const list = regions.get(info.id);
            if (list[info.slot]) release(list[info.slot]);
            list[info.slot] = buffer;
            const key = `${info.id}/${info.slot}`;
            const waiters = regionWaiters.get(key);
            regionWaiters.delete(key);
            if (waiters) waiters.forEach((resolve) => resolve());
            checkReady();
        });

        // Binary data from C++ (uniapi::Bytes) arrives as placeholders, each fetched once and
        // replaced by a Uint8Array before the message is handled. Messages after one that is
        // still downloading wait for it, so they are handled in the order C++ sent them.
        let inbound = Promise.resolve();
        let inboundQueued = 0;

        async function download(token) {
            const response = await fetch(`/uniapi/download/${encodeURIComponent(token)}`);
            if (!response.ok) throw new Error(`receiving binary data failed (${response.status})`);
            return new Uint8Array(await response.arrayBuffer());
        }

        // `value` with every download placeholder replaced (in place) by its bytes.
        async function resolveDownloads(value, depth = 0) {
            if (value === null || typeof value !== 'object' || depth > 64) return value;
            if (!Array.isArray(value)) {
                const keys = Object.keys(value);
                if (keys.length === 1 && keys[0] === DOWNLOAD_KEY && typeof value[DOWNLOAD_KEY] === 'string') {
                    return download(value[DOWNLOAD_KEY]);
                }
            }
            await Promise.all(Object.keys(value).map(async (key) => {
                value[key] = await resolveDownloads(value[key], depth + 1);
            }));
            return value;
        }

        const payloadField = { result: 'value', call: 'args', event: 'data' };

        function receive(message) {
            if (!message.downloads && inboundQueued === 0) {
                handle(message);
                return;
            }
            inboundQueued++;
            let downloads = null;
            const field = payloadField[message.t];
            if (message.downloads && field) {
                // Start fetching now; handle it when the messages before it are done.
                downloads = resolveDownloads(message[field]).then((value) => { message[field] = value; });
                downloads.catch(() => {});  // handled in the queue below, not "unhandled"
            }
            inbound = inbound
                .then(() => downloads)
                .then(() => handle(message), (err) => {
                    console.error('uniapi: binary data from C++ could not be received', err);
                    const error = String(err && err.message || err);
                    if (message.t === 'call') {
                        send({ t: 'result', id: message.id, ok: false, error });
                    } else if (message.t === 'result') {
                        const pending = pendingCalls.get(message.id);
                        pendingCalls.delete(message.id);
                        if (pending) pending.reject(new Error(error));
                    }
                })
                // Keeps the queue from rejecting, so a failure can't fail the messages after it.
                .catch((err) => console.error('uniapi: handling a message failed', err))
                .finally(() => inboundQueued--);
        }

        transport.onMessage((text) => {
            let message;
            try {
                message = JSON.parse(text);
            } catch {
                return;
            }
            receive(message);
        });

        function handle(message) {
            switch (message.t) {
            case 'welcome':
                capabilities.sharedMemory = !!message.sharedMemory;
                resolveSession(message.session);
                message.streams.forEach(addStream);
                welcomed = true;
                checkReady();
                if (!isReady) {
                    setTimeout(() => {
                        if (isReady) return;
                        giveUpSharedMemory();
                        checkReady();
                    }, regionTimeout);
                }
                break;
            case 'stream':  // C++ created or replaced a stream; its regions come separately
                addStream(message.stream);
                break;
            case 'frame':
                deliver(message);
                break;
            case 'call':
                answer(message);
                break;
            case 'result': {
                const pending = pendingCalls.get(message.id);
                if (!pending) return;
                pendingCalls.delete(message.id);
                if (message.ok) pending.resolve(message.value);
                else pending.reject(new Error(message.error));
                break;
            }
            case 'event':
                for (const handler of eventHandlers.get(message.name) || []) {
                    try {
                        handler(message.data);
                    } catch (err) {
                        console.error(`uniapi: handler for event "${message.name}" failed`, err);
                    }
                }
                break;
            }
        }

        // Frames reach a stream's handler in the order C++ published them, even when their bytes
        // are ready out of order (fetches finishing in any order, a region arriving late). That
        // matters for DOM lists, where a later list can use an element an earlier one created.
        // A handler may still hold its slot (a pending promise) while the next frame arrives.
        const handlerTurns = new Map();  // stream name -> resolves once the previous frame's handler was called

        async function deliver({ stream, id, slot, size, meta }) {
            const previousTurn = handlerTurns.get(stream) || Promise.resolve();
            let turnDone;
            handlerTurns.set(stream, new Promise((resolve) => { turnDone = resolve; }));
            const entry = streams.get(stream);
            const handler = streamHandlers.get(stream);
            try {
                // A frame of a stream that has since been replaced is dropped unread.
                if (entry && entry.id === id && handler) {
                    let bytes = null;
                    if (entry.shared) {
                        const buffer = (regions.get(id) || [])[slot] || await regionFor(id, slot);
                        if (streams.get(stream) !== entry) return;  // replaced while waiting
                        if (buffer) {
                            bytes = new Uint8Array(buffer, 0, size);
                        } else if (entry.shared) {
                            // Its regions never came: fetch this stream from now on.
                            console.warn(`uniapi: shared memory for stream "${stream}" didn't arrive; ` +
                                         'it uses the fetch fallback');
                            entry.shared = false;
                            send({ t: 'unshared', stream, id });
                        }
                    }
                    if (!bytes) {
                        const response = await fetch(`/uniapi/stream/${encodeURIComponent(stream)}/${slot}`);
                        if (!response.ok) throw new Error(`fetching stream data failed (${response.status})`);
                        bytes = new Uint8Array(await response.arrayBuffer());
                    }
                    await previousTurn;
                    if (streams.get(stream) !== entry) return;  // replaced while waiting its turn
                    if (!streamHandlers.has(stream)) return;    // unstream() while waiting
                    const handled = handler(bytes, meta);
                    turnDone();
                    await handled;
                }
            } catch (err) {
                console.error(`uniapi: stream "${stream}" handler failed`, err);
            } finally {
                previousTurn.then(turnDone);  // a skipped frame still keeps its place in line
                send({ t: 'release', stream, id, slot });
            }
        }

        async function answer({ id, fn, args }) {
            const target = exposed.get(fn);
            try {
                if (!target) throw new Error(`there is no JavaScript function named "${fn}"`);
                const value = await target(...(args || []));
                // Binary data in the result goes to C++ like call arguments do.
                sendWithBinary({ t: 'result', id, ok: true, value: value === undefined ? null : value }, 'value',
                               (err) => send({ t: 'result', id, ok: false, error: String(err && err.message || err) }));
            } catch (err) {
                send({ t: 'result', id, ok: false, error: String(err && err.message || err) });
            }
        }

        function call(fn, ...args) {
            const id = nextCallId++;
            return new Promise((resolve, reject) => {
                pendingCalls.set(id, { resolve, reject });
                sendWithBinary({ t: 'call', id, fn, args }, 'args', (err) => {
                    pendingCalls.delete(id);
                    reject(err);
                });
            });
        }

        const app = {
            // app.native.someFunction(...args) calls the C++ function exposed under that name.
            native: new Proxy({}, {
                get: (_, name) => (name === 'then' || typeof name !== 'string')
                    ? undefined  // so `await app.native` doesn't try to call a "then" function
                    : (...args) => call(name, ...args),
            }),
            call,
            expose(functions) {
                for (const [name, fn] of Object.entries(functions)) exposed.set(name, fn);
                return app;
            },
            // Takes back functions registered with expose(): a call to one then fails as a call
            // to a name never registered does. One that is running finishes its call.
            unexpose(...names) {
                for (const name of names) exposed.delete(name);
                return app;
            },
            on(event, handler) {
                if (!eventHandlers.has(event)) eventHandlers.set(event, []);
                eventHandlers.get(event).push(handler);
                return app;
            },
            // Stops a handler added with on(); without a handler, every handler of the event.
            // The list is replaced, not changed, so a handler may call this while the event
            // is being delivered: the handlers of that delivery still all run.
            off(event, handler) {
                const handlers = eventHandlers.get(event);
                if (!handlers) return app;
                const kept = handler === undefined ? [] : handlers.filter((h) => h !== handler);
                if (kept.length) eventHandlers.set(event, kept);
                else eventHandlers.delete(event);
                return app;
            },
            emit(event, data) {
                sendWithBinary({ t: 'event', name: event, data: data === undefined ? null : data }, 'data',
                               (err) => console.error(`uniapi: event "${event}" not sent`, err));
            },
            stream(name, handler) {
                streamHandlers.set(name, handler);
                return app;
            },
            // Stops receiving a stream: its handler isn't called again (one that is running
            // finishes), and the frames that still arrive are handed back unread.
            unstream(name) {
                streamHandlers.delete(name);
                return app;
            },
            // Shows the window, kept hidden until the page is drawn (AppOptions::showWhenReady):
            // call it once the page has built itself.
            show() {
                send({ t: 'show' });
            },
            ready,
            capabilities,
        };

        send({ t: 'hello', page: pageName });
        return app;
    }

    window.uniapi = { connect };
})();
