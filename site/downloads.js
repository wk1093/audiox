'use strict';

let downloadCatalog = null;
let downloadChannel = 'stable';

function renderDownloads() {
  const channel = downloadChannel;
  const release = downloadCatalog?.[channel];
  const error = document.getElementById('download-error');
  const cards = document.getElementById('download-cards');
  const releaseLink = document.getElementById('release-link');
  cards.replaceChildren();
  error.hidden = true;
  releaseLink.hidden = true;
  for (const name of ['stable', 'nightly']) {
    const active = name === channel;
    const button = document.getElementById(`channel-${name}`);
    button.classList.toggle('active', active);
    button.setAttribute('aria-pressed', String(active));
  }
  document.getElementById('release-heading').textContent = `Latest ${channel} release`;
  document.getElementById('channel-description').textContent = channel === 'stable'
    ? 'Manually published releases. Recommended for most users.'
    : 'Recent builds from main. These are experimental and may have bugs.';
  if (!release) {
    document.getElementById('release-meta').textContent = 'No downloads have been published for this channel yet.';
    error.textContent = 'This channel will appear when an AXU-enabled release is published. Older releases are listed on GitHub.';
    error.hidden = false;
    return;
  }
  document.getElementById('release-meta').textContent = [release.version, release.tag, release.date || 'date unavailable'].filter(Boolean).join(' / ');
  releaseLink.href = release.release_url;
  releaseLink.hidden = false;
  for (const board of ['pi4', 'pi5']) {
    const asset = release.assets[board];
    if (!asset) continue;
    const card = document.getElementById('download-card').content.cloneNode(true);
    const find = (selector) => card.querySelector(selector);
    find('.board-label').textContent = `${board.toUpperCase()} / 64-bit`;
    find('.board-name').textContent = board === 'pi4' ? 'Raspberry Pi 4' : 'Raspberry Pi 5';
    find('.board-description').textContent = `${asset.name || release.tag}${board === 'pi5' ? ' - Pi 5 support is untested on hardware.' : ''}`;
    const size = (bytes) => `${(bytes / 1048576).toFixed(1)} MiB`;
    find('.axu-link').href = asset.url;
    find('.axu-size').textContent = `${size(asset.size)} / update an existing device`;
    find('.axu-hash').textContent = asset.sha256;
    find('.axu-checksum-link').href = asset.checksum_url;
    if (asset.image) {
      find('.image-link').href = asset.image.url;
      find('.image-size').textContent = `${size(asset.image.size)} / compressed .img.xz`;
      find('.image-hash').textContent = asset.image.sha256 || 'See the checksum download.';
      find('.image-checksum-link').href = `${asset.image.url}.sha256`;
    } else {
      find('.image-link').hidden = true;
      find('.image-hash').textContent = 'No SD image published for this release.';
      find('.image-checksum-link').hidden = true;
    }
    cards.appendChild(card);
  }
}

for (const channel of ['stable', 'nightly']) {
  document.getElementById(`channel-${channel}`).addEventListener('click', () => {
    downloadChannel = channel;
    if (downloadCatalog) renderDownloads();
  });
}

async function loadDownloads() {
  try {
    const response = await fetch('./updates.json', { cache: 'no-store' });
    if (!response.ok) throw new Error(`Release catalog returned HTTP ${response.status}`);
    const data = await response.json();
    if (!data || typeof data !== 'object' || Array.isArray(data)) throw new Error('Invalid release catalog');
    downloadCatalog = data;
    renderDownloads();
  } catch (error) {
    document.getElementById('download-cards').replaceChildren();
    document.getElementById('release-meta').textContent = 'Downloads are temporarily unavailable.';
    const notice = document.getElementById('download-error');
    notice.textContent = `${error.message}. Try again later, or use the Release history link above.`;
    notice.hidden = false;
  }
}

function showSitePage() {
  const selected = window.location.hash === '#download' ? 'download' : 'home';
  for (const name of ['home', 'download']) {
    const active = name === selected;
    document.getElementById(`page-${name}`).hidden = !active;
    const link = document.getElementById(`nav-${name}`);
    link.classList.toggle('active', active);
    if (active) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  }
  document.title = selected === 'download' ? 'AudioX Downloads' : 'AudioX';
}

window.addEventListener('hashchange', showSitePage);
showSitePage();
loadDownloads();
