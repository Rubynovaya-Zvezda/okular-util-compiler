'use strict'

const fs = require('fs')
const path = require('path')
const { LanguageClient, commands, workspace, window } = require('coc.nvim')

let client = null

function hasMarker(uri) {
  let dir = path.dirname(String(uri).replace(/^file:\/\//, ''))
  for (let i = 0; i < 32 && dir && dir !== path.dirname(dir); i++) {
    if (fs.existsSync(path.join(dir, '.okc')) || fs.existsSync(path.join(dir, 'okc.json'))) return true
    const mod = path.join(dir, 'MODULE.bazel')
    if (fs.existsSync(mod) && /okular-compiler/.test(fs.readFileSync(mod, 'utf8'))) return true
    dir = path.dirname(dir)
  }
  return false
}

function isOkcDocument(doc) {
  if (!doc) return false
  if (doc.languageId === 'okc' || /\.okc$/i.test(doc.uri)) return true
  if (hasMarker(doc.uri)) return true
  const text = typeof doc.getText === 'function' ? doc.getText() : ''
  return /\bspace\s+\w+\s*\{|\b\w+(?:\s*::\s*\w+)+/.test(text)
}

function settings() {
  const conf = workspace.getConfiguration('okc')
  return {
    enable: conf.get('enable', true),
    command: conf.get('command', 'okc'),
    args: conf.get('args', []) || [],
    clangd: conf.get('clangd', '') || '',
    rootPatterns: conf.get('rootPatterns', ['.git', 'compile_commands.json']) || []
  }
}

function makeClient(s = settings()) {
  const args = ['lsp'].concat(s.args).map(String)
  const env = Object.assign({}, process.env)
  if (s.clangd) env.SPACE_CLANGD = s.clangd
  // No explicit transport: coc uses stdio streams without appending --stdio,
  // which some clangd versions reject. okc lsp also strips it defensively.
  const serverOptions = {
    command: s.command,
    args,
    options: { env }
  }
  const clientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'c' },
      { scheme: 'file', language: 'okc' },
      { scheme: 'file', pattern: '**/*.okc' },
      { scheme: 'untitled', language: 'c' },
      { scheme: 'untitled', language: 'okc' }
    ],
    synchronize: { configurationSection: ['okc'] },
    initializationOptions: { okc: true },
    outputChannelName: 'okc',
    rootPatterns: s.rootPatterns,
    requireRootPattern: false
  }
  return new LanguageClient('okc', 'okc language server', serverOptions, clientOptions)
}

function start(context, s) {
  if (client) return
  client = makeClient(s)
  context.subscriptions.push(client.start())
  context.subscriptions.push(commands.registerCommand('okc.restart', async () => {
    if (client) await client.restart()
  }))
}

async function activate(context) {
  const s = settings()
  if (!s.enable) {
    window.showInformationMessage('okc: language server is disabled (okc.enable)')
    return
  }
  const docs = workspace.textDocuments || []
  const found = docs.find(isOkcDocument)
  if (found) {
    start(context, s)
    return
  }
  const listener = workspace.onDidOpenTextDocument(doc => {
    if (isOkcDocument(doc)) {
      listener.dispose()
      start(context, s)
    }
  })
  context.subscriptions.push(listener)
}

async function deactivate() {
  if (client) {
    const current = client
    client = null
    await current.stop()
  }
}

exports.activate = activate
exports.deactivate = deactivate
exports.settings = settings
exports.makeClient = makeClient
exports.isOkcDocument = isOkcDocument
