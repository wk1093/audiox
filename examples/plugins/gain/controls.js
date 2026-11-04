'use strict';
const gain = document.getElementById('gain');
const value = document.getElementById('value');
gain.addEventListener('input', () => {
    value.textContent = Number(gain.value).toFixed(2);
    parent.postMessage({ type: 'audiox:set-param', param: 'gain', value: Number(gain.value) }, '*');
});
window.addEventListener('message', (event) => {
    if (event.source !== parent || event.data?.type !== 'audiox:parameters') return;
    const next = event.data.params?.gain;
    if (!Number.isFinite(next) || next < 0 || next > 2) return;
    gain.value = String(next);
    value.textContent = next.toFixed(2);
});
