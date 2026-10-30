import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'AudioX Documentation',
  description: 'Guides for AudioX named automation macros and the Raspberry Pi audio workstation.',
  base: '/audiox/docs/',
  cleanUrls: true,
  vite: {
    server: { host: '127.0.0.1' },
  },
  themeConfig: {
    siteTitle: 'AudioX Docs',
    nav: [
      { text: 'Lua scripts', link: '/lua/getting-started' },
      { text: 'AudioX on GitHub', link: 'https://github.com/wk1093/audiox' },
    ],
    sidebar: [
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
