// The uniapi name ("uniapi", lettered in Orbitron, as outlines: name.js, written by make.py) placed
// beside text on a line, the way it sits everywhere the logo and name appear:
//
//   - its short letters (u, n, a) are exactly as tall as the text's capitals (the A of API),
//     and its baseline is the text's;
//   - fitted to the screen's pixels, so u, n, a and the capitals share their top and bottom pixel
//     rows: the browser draws text on whole pixels (its capitals a whole number of pixels tall,
//     its baseline rounded to the nearest pixel edge) and an SVG where it is laid out, so the
//     name's x-height is the same whole number of pixels and its baseline is moved onto the edge
//     where the text's is drawn (measured at 125 %: the rows then match exactly).
//
// Its colours are those of a logo variant (colors: light, dark, gray): "uni" in the logo's left
// colour, "api" in its right, a crisp 1 px outline in the two mixed, half transparent.
//
//   <h1><span class="name" data-variant="dark"></span> <span>API</span></h1>
//   fitUniapiName(document.querySelectorAll('h1 .name'), document.querySelector('h1 span:last-child'));
//
// Holders are elements on the text's line; each gets the name as inline SVG. Holders may be hidden
// (one per theme); they are placed again when the window resizes and whenever place() is called.
// Needs name.js loaded first (UNIAPI_NAME).
(function () {
    const SVG = 'http://www.w3.org/2000/svg';

    function nameSvg(variant) {
        const n = window.UNIAPI_NAME;
        const c = n.colors[variant];
        const svg = document.createElementNS(SVG, 'svg');
        svg.setAttribute('role', 'img');
        svg.setAttribute('aria-label', 'uniapi');
        svg.setAttribute('preserveAspectRatio', 'none');
        svg.style.position = 'relative';
        svg.style.overflow = 'visible';
        const g = document.createElementNS(SVG, 'g');
        g.setAttribute('stroke', c.outline);
        g.setAttribute('stroke-opacity', c.outlineOpacity);
        g.setAttribute('stroke-linejoin', 'round');
        g.setAttribute('paint-order', 'stroke fill');
        for (const [part, fill] of [['uni', c.uni], ['api', c.api]]) {
            const path = document.createElementNS(SVG, 'path');
            path.setAttribute('d', n[part]);
            path.setAttribute('fill', fill);
            g.append(path);
        }
        svg.append(g);
        return svg;
    }

    // The text's capital height, measured at 200 px and scaled down (at the text's own size the
    // canvas rounds it).
    function capHeight(text) {
        const style = getComputedStyle(text);
        const ctx = document.createElement('canvas').getContext('2d');
        ctx.font = `${style.fontStyle} ${style.fontWeight} 200px ${style.fontFamily}`;
        return ctx.measureText('W').actualBoundingBoxAscent * parseFloat(style.fontSize) / 200;
    }

    window.fitUniapiName = function (holders, text) {
        const n = window.UNIAPI_NAME;
        holders = [...holders];
        const svgs = holders.map((holder) => {
            const svg = nameSvg(holder.dataset.variant);
            holder.replaceChildren(svg);
            return svg;
        });
        // A zero-size box on the text's line: its bottom is the line's baseline.
        const marker = document.createElement('span');
        marker.style.cssText = 'display: inline-block; width: 0; height: 0';
        text.before(marker);

        const place = () => {
            const dpr = devicePixelRatio;
            const capPixels = Math.round(capHeight(text) * dpr);
            const unit = capPixels / dpr / n.xheight;          // CSS px per outline unit
            const pad = 1 / unit;                               // a pixel round the letters (outline)
            // Above and below the baseline in whole pixels, so the baseline is a pixel edge.
            const above = Math.ceil((-n.top + pad) * unit * dpr), below = Math.ceil((n.bottom + pad) * unit * dpr);
            const top = above / dpr / unit, bottom = below / dpr / unit;
            for (const svg of svgs) {
                svg.setAttribute('viewBox', `${-pad} ${-top} ${n.width + 2 * pad} ${top + bottom}`);
                svg.firstChild.setAttribute('stroke-width', 2 * pad);   // 1 px of it shows round the letters
                Object.assign(svg.style, {
                    width: `${(n.width + 2 * pad) * unit}px`, height: `${(above + below) / dpr}px`,
                    margin: '0 -1px', verticalAlign: `${-below / dpr}px`, top: '0px',
                });
            }
            const visible = svgs.find((svg) => svg.getClientRects().length);
            if (!visible) return;
            const r = visible.getBoundingClientRect();
            const scale = r.height / parseFloat(visible.style.height);   // a zoom on the page, if any
            const drawnBaseline = r.top + (above / dpr) * scale;
            const textBaseline = Math.round(marker.getBoundingClientRect().bottom * dpr) / dpr;
            // Moved by `top`, which leaves the line (and the text on it) where it is.
            const shift = (textBaseline - drawnBaseline) / scale;
            for (const svg of svgs) svg.style.top = `${shift}px`;
        };
        place();
        document.fonts?.ready.then(place);
        addEventListener('resize', place);
        return place;
    };
})();
