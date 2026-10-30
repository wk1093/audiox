import { createReadStream, statSync } from 'node:fs'
import { createServer } from 'node:http'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '.vitepress/dist')
const base = '/audiox/docs/'
const contentTypes = {
  '.css': 'text/css; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.woff2': 'font/woff2',
}

createServer((request, response) => {
  let pathname
  try {
    pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname)
  } catch {
    response.writeHead(400).end('Bad request')
    return
  }

  if (!pathname.startsWith(base)) {
    response.writeHead(404).end('Not found')
    return
  }

  let file = path.resolve(root, `.${pathname.slice(base.length)}`)
  if (!file.startsWith(`${root}${path.sep}`) && file !== root) {
    response.writeHead(404).end('Not found')
    return
  }

  try {
    if (statSync(file).isDirectory()) file = path.join(file, 'index.html')
    if (!statSync(file).isFile()) throw new Error('not a file')
  } catch {
    if (!path.extname(file)) file += '.html'
    try {
      if (!statSync(file).isFile()) throw new Error('not a file')
    } catch {
      response.writeHead(404).end('Not found')
      return
    }
  }

  response.writeHead(200, {
    'Content-Type': contentTypes[path.extname(file)] ?? 'application/octet-stream',
    'X-Content-Type-Options': 'nosniff',
  })
  if (request.method === 'HEAD') response.end()
  else createReadStream(file).pipe(response)
}).listen(4173, '127.0.0.1', () => {
  console.log(`Built site served at http://127.0.0.1:4173${base}`)
})
