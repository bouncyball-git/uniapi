// Settings form for the demo. Calls the C++ functions main.cpp exposes:
//   getState() applySettings(options) run() clearHistory() saveResults()
// A C++ exception arrives here as a rejected promise.

const DEFAULTS = { width: 640, height: 360, mode: 'timed', slots: 3, draw: '2d', stats: false };
const MAX_SHARED_BYTES = 4 * 1024 ** 3;  // matches kMaxSharedBytes in main.cpp
const MODE_LABELS = { timed: 'Timed', benchmark: 'Benchmark', transport: 'Transfer only' };
const DRAW_LABELS = { '2d': '2D canvas', '2d-cpu': '2D canvas, CPU', webgl: 'WebGL' };

const form = document.getElementById('settings');
const preset = document.getElementById('preset');
const width = document.getElementById('width');
const height = document.getElementById('height');
const slots = document.getElementById('slots');
const applyButton = document.getElementById('apply');
const errorBanner = document.getElementById('error');
const memoryHint = document.getElementById('memory');
const historyTable = document.getElementById('history');
const historyEmpty = document.getElementById('history-empty');
const saveButton = document.getElementById('save');
const saveStatus = document.getElementById('save-status');

function fillForm(settings) {
    const size = `${settings.width}x${settings.height}`;
    const isPreset = [...preset.options].some((option) => option.value === size);
    preset.value = isPreset ? size : 'custom';
    width.value = settings.width;
    height.value = settings.height;
    form.elements.mode.value = settings.mode;
    form.elements.draw.value = settings.draw;
    slots.value = settings.slots;
    form.elements.stats.checked = Boolean(settings.stats);
    updateForm();
}

function readForm() {
    const [w, h] = preset.value === 'custom'
        ? [width.valueAsNumber, height.valueAsNumber]
        : preset.value.split('x').map(Number);
    return {
        width: w,
        height: h,
        mode: form.elements.mode.value,
        draw: form.elements.draw.value,
        slots: slots.valueAsNumber,
        stats: form.elements.stats.checked,
    };
}

// The same syntax as the command line; main.cpp parses both with one function.
function toOptions(settings) {
    const options = [
        `--size=${settings.width}x${settings.height}`,
        `--slots=${settings.slots}`,
        `--draw=${settings.draw}`,
        `--stats=${settings.stats ? 'on' : 'off'}`,
    ];
    if (settings.mode === 'benchmark') options.push('--benchmark');
    if (settings.mode === 'transport') options.push('--benchmark=transport');
    return options.join(' ');
}

function formatBytes(bytes) {
    return bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(2)} GB` : `${(bytes / 1024 ** 2).toFixed(1)} MB`;
}

// Keeps dependent fields, validity and the memory estimate in step with the inputs.
function updateForm() {
    const custom = preset.value === 'custom';
    for (const field of document.querySelectorAll('[data-custom]')) field.hidden = !custom;
    width.required = height.required = custom;

    const transport = form.elements.mode.value === 'transport';
    for (const radio of form.elements.draw) radio.disabled = transport;
    document.getElementById('draw-note').hidden = !transport;

    const settings = readForm();
    const frameBytes = settings.width * settings.height * 4;
    const totalBytes = frameBytes * settings.slots;
    const sizeValid = Number.isInteger(settings.width) && Number.isInteger(settings.height) &&
        settings.width >= 1 && settings.height >= 1 && settings.width <= 16384 && settings.height <= 16384;

    if (!sizeValid || !Number.isInteger(settings.slots) || settings.slots < 1 || settings.slots > 64) {
        slots.setCustomValidity('');
        memoryHint.textContent = '';
        memoryHint.classList.remove('error');
        return;
    }
    const tooLarge = totalBytes > MAX_SHARED_BYTES;
    slots.setCustomValidity(tooLarge ? 'These settings need more than 4 GB of memory for the stream.' : '');
    memoryHint.classList.toggle('error', tooLarge);
    memoryHint.textContent = `${formatBytes(frameBytes)} per frame × ${settings.slots} = ` +
        `${formatBytes(totalBytes)} of stream memory` +
        (tooLarge ? ', over the 4 GB limit. Use a smaller size or fewer buffers.' : '.');
}

function showError(message) {
    errorBanner.textContent = message;
    errorBanner.hidden = !message;
    if (message) errorBanner.scrollIntoView({ block: 'nearest' });
}

// A button holds each of its labels (controls.html: data-when="idle" / "busy"), one shown, so it
// keeps the longest one's size.
function showLabel(button, busy) {
    for (const label of button.querySelectorAll('[data-when]')) label.hidden = (label.dataset.when === 'busy') !== busy;
}

function setBusy(busy) {
    applyButton.disabled = busy;
    showLabel(applyButton, busy);
}

function setSaving(busy) {
    saveButton.disabled = busy;
    showLabel(saveButton, busy);
}

function showSaveStatus(message, isError) {
    saveStatus.textContent = message;
    saveStatus.classList.toggle('error', Boolean(isError));
    saveStatus.hidden = !message;
}

function formatNumber(value, digits) {
    return value === null || value === undefined ? '—' : value.toFixed(digits);
}

function renderHistory(history) {
    const body = historyTable.tBodies[0];
    body.replaceChildren();
    historyTable.hidden = history.length === 0;
    historyEmpty.hidden = history.length > 0;
    document.getElementById('clear').hidden = history.length === 0;
    saveButton.hidden = history.length === 0;
    if (history.length === 0) showSaveStatus('');

    const bestFps = Math.max(...history.map((run) => run.fps));
    for (const run of history) {
        const s = run.settings;
        const row = body.insertRow();
        if (run.fps === bestFps && history.length > 1) row.classList.add('best');
        const cells = [
            [`${s.width} × ${s.height}`],
            [MODE_LABELS[s.mode] ?? s.mode],
            [s.mode === 'transport' ? '—' : (DRAW_LABELS[s.draw] ?? s.draw)],
            [String(s.slots), 'num'],
            [formatNumber(run.fps, run.fps >= 100 ? 0 : 1), 'num fps'],
            [formatNumber(1000 / run.fps, 2), 'num'],
            [formatNumber(run.renderMs, 2), 'num'],
            [formatNumber(run.drawMs, 2), 'num'],
            [formatNumber(run.gbps, 2), 'num'],
        ];
        for (const [text, className] of cells) {
            const cell = row.insertCell();
            cell.textContent = text;
            if (className) cell.className = className;
        }
        const useButton = document.createElement('button');
        useButton.type = 'button';
        useButton.className = 'button ghost small';
        useButton.textContent = 'Use';
        useButton.title = 'Load these settings into the form';
        useButton.addEventListener('click', () => {
            fillForm(s);
            showError('');
            window.scrollTo({ top: 0, behavior: 'smooth' });
            preset.focus({ preventScroll: true });
        });
        row.insertCell().append(useButton);
    }
}

function showState(state) {
    renderHistory(state.history);
    document.getElementById('config-path').textContent = `Applied settings are saved to ${state.configPath}`;
}

let app = null;
try {
    app = uniapi.connect({ page: 'controls' });
} catch (err) {
    showError('This page only works inside the app.');
    fillForm(DEFAULTS);
}

if (app) {
    app.native.getState().then((state) => {
        fillForm(state.settings);
        showState(state);
    }, (err) => showError(err.message));

    saveButton.addEventListener('click', async () => {
        showSaveStatus('');
        setSaving(true);
        try {
            const result = await app.native.saveResults();
            if (result.saved) {
                const runs = result.runs === 1 ? '1 run' : `${result.runs} runs`;
                showSaveStatus(`Saved ${runs} to ${result.path}`);
            }
        } catch (err) {
            showSaveStatus(err.message, true);
        } finally {
            setSaving(false);
        }
    });

    form.addEventListener('submit', async (event) => {
        event.preventDefault();
        updateForm();
        if (!form.reportValidity()) return;
        showError('');
        setBusy(true);
        try {
            await app.native.applySettings(toOptions(readForm()));  // then C++ opens the demo page
        } catch (err) {
            setBusy(false);
            showError(err.message);
        }
    });

    document.getElementById('back').addEventListener('click', () => app.native.run());
    document.getElementById('clear').addEventListener('click', async () => showState(await app.native.clearHistory()));
}

form.addEventListener('input', updateForm);
document.getElementById('reset').addEventListener('click', () => {
    fillForm(DEFAULTS);
    showError('');
});
