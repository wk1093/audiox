import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'AudioX Documentation',
  description: 'Setup, routing, effects, soundboard, MIDI, automation, and maintenance guides for the AudioX Raspberry Pi audio workstation.',
  base: '/audiox/docs/',
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/audiox/docs/logo_small.svg' }],
    ['link', { rel: 'icon', type: 'image/png', sizes: '512x512', href: '/audiox/docs/logo_small.png' }],
    ['link', { rel: 'apple-touch-icon', href: '/audiox/docs/logo_small.png' }],
  ],
  cleanUrls: true,
  vite: {
    server: { host: '127.0.0.1' },
  },
  themeConfig: {
    siteTitle: 'AudioX Docs',
    search: { provider: 'local' },
    nav: [
      { text: 'Getting started', link: '/guide/getting-started' },
      { text: 'Device guides', link: '/guide/web-interface' },
      { text: 'Lua scripts', link: '/lua/getting-started' },
      { text: 'AudioX on GitHub', link: 'https://github.com/wk1093/audiox' },
    ],
    sidebar: [
      {
        text: 'Start here',
        items: [
          { text: 'Documentation home', link: '/' },
          { text: 'Getting started', link: '/guide/getting-started' },
          { text: 'Hardware and compatibility', link: '/guide/hardware' },
          { text: 'Networking and USB connection', link: '/guide/networking' },
          { text: 'WebUI overview', link: '/guide/web-interface' },
        ],
      },
      {
        text: 'Audio and controls',
        items: [
          { text: 'Routing', link: '/guide/routing' },
          { text: 'Bluetooth', link: '/guide/bluetooth' },
          { text: 'Effects', link: '/guide/effects' },
          { text: 'Volume', link: '/guide/volume' },
          { text: 'Soundboard', link: '/guide/soundboard' },
          { text: 'MIDI', link: '/guide/midi' },
          { text: 'Macros and automation', link: '/guide/macros' },
          { text: 'Display and touch', link: '/guide/display' },
        ],
      },
      {
        text: 'Configuration and maintenance',
        items: [
          { text: 'Config and backups', link: '/guide/config' },
          { text: 'System and updates', link: '/guide/system' },
          { text: 'Troubleshooting', link: '/guide/troubleshooting' },
          { text: 'Files and HTTP reference', link: '/guide/reference' },
          { text: 'Building and development', link: '/guide/development' },
          { text: 'Releases and publishing', link: '/guide/publishing' },
        ],
      },
      {
        text: 'Lua scripts',
        items: [
          { text: 'Getting started', link: '/lua/getting-started' },
          { text: 'Events and state', link: '/lua/events-and-state' },
          { text: 'API reference', link: '/lua/api' },
          { text: 'Keyboard and lighting', link: '/lua/keyboard-and-lighting' },
          { text: 'MIDI and soundboard', link: '/lua/midi-and-soundboard' },
          { text: 'Cookbook', link: '/lua/cookbook' },
          { text: 'Limits and troubleshooting', link: '/lua/limits-and-troubleshooting' },
        ],
      },
    ],
    socialLinks: [
      { icon: 'github', link: 'https://github.com/wk1093/audiox' },
    ],
    footer: {
      message: 'AudioX is open source and under active development.',
      copyright: 'AudioX contributors',
    },
  },
})
