'use strict'

const assert = require('assert')
const fs = require('fs')
const os = require('os')
const path = require('path')
const Module = require('module')

let values = {}
let created = []
let registered = []

class LanguageClient {
  constructor(id, name, serverOptions, clientOptions) {
    this.id = id
    this.name = name
    this.serverOptions = serverOptions
    this.clientOptions = clientOptions
    this.restarts = 0
    created.push(this)
  }
  start() { return { dispose() {} } }
  async restart() { this.restarts++ }
  async stop() {}
}

const coc = {
  LanguageClient,
  TransportKind: { stdio: 0 },
  workspace: {
    textDocuments: [],
    getConfiguration: () => ({ get: (key, fallback) =>
      Object.prototype.hasOwnProperty.call(values, key) ? values[key] : fallback }),
    onDidOpenTextDocument: handler => { coc.openHandler = handler; return { dispose() {} } }
  },
  commands: {
    registerCommand: (name, handler) => { registered.push(name); return { dispose() {} } }
  },
  window: { showInformationMessage: () => Promise.resolve() }
}

const originalLoad = Module._load
Module._load = function (request, parent, isMain) {
  if (request === 'coc.nvim') return coc
  return originalLoad.call(this, request, parent, isMain)
}

const okc = require('../lib/index.js')

async function main() {
  const pkg = require('../package.json')
  assert(pkg.engines && pkg.engines.coc, 'engines.coc is required by coc.nvim')

  values = { command: '/opt/okc', args: ['--log=verbose'], clangd: 'clangd-19' }
  const s = okc.settings()
  assert.strictEqual(s.command, '/opt/okc')
  assert.deepStrictEqual(s.args, ['--log=verbose'])

  const client = okc.makeClient(s)
  assert.strictEqual(client.serverOptions.command, '/opt/okc')
  assert.deepStrictEqual(client.serverOptions.args, ['lsp', '--log=verbose'])
  assert.strictEqual(client.serverOptions.options.env.SPACE_CLANGD, 'clangd-19')
  assert.strictEqual(client.serverOptions.transport, undefined)
  assert.deepStrictEqual(client.clientOptions.documentSelector.map(d => d.language),
    ['c', 'okc', undefined, 'c', 'okc'])

  assert.strictEqual(okc.isOkcDocument({ languageId: 'c', uri: 'file:///tmp/plain.c', getText: () => 'int x;' }), false)
  assert.strictEqual(okc.isOkcDocument({ languageId: 'okc', uri: 'file:///tmp/a.okc', getText: () => '' }), true)
  assert.strictEqual(okc.isOkcDocument({ languageId: 'c', uri: 'file:///tmp/a.c', getText: () => 'space x { int v; }' }), true)
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'coc-okc-'))
  fs.writeFileSync(path.join(dir, '.okc'), '')
  assert.strictEqual(okc.isOkcDocument({ languageId: 'c', uri: 'file://' + path.join(dir, 'a.c'), getText: () => 'int x;' }), true)
  const bmod = fs.mkdtempSync(path.join(os.tmpdir(), 'coc-okc-bazel-'))
  fs.writeFileSync(path.join(bmod, 'MODULE.bazel'), 'bazel_dep(name = "okular-compiler", version = "0.1.0")\n')
  assert.strictEqual(okc.isOkcDocument({ languageId: 'c', uri: 'file://' + path.join(bmod, 'a.c'), getText: () => 'int x;' }), true)

  coc.workspace.textDocuments = [{ languageId: 'c', uri: 'file:///tmp/a.c', getText: () => 'x::y' }]
  created = []
  const context = { subscriptions: [] }
  await okc.activate(context)
  assert.strictEqual(created.length, 1)
  assert(registered.includes('okc.restart'))
  assert.strictEqual(context.subscriptions.length >= 2, true)
  await okc.deactivate()

  values = { enable: false }
  created = []
  registered = []
  await okc.activate({ subscriptions: [] })
  assert.strictEqual(created.length, 0)

  values = { enable: true }
  coc.workspace.textDocuments = []
  created = []
  await okc.activate({ subscriptions: [] })
  assert.strictEqual(created.length, 0)
  coc.openHandler({ languageId: 'c', uri: 'file:///tmp/later.c', getText: () => 'space x { int v; }' })
  assert.strictEqual(created.length, 1)
  await okc.deactivate()

  console.log('coc-okc extension tests passed')
  Module._load = originalLoad
}

main().catch(err => { console.error(err); process.exitCode = 1 })
