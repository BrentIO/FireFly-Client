#!/usr/bin/env node
// Reads devices.yaml from the repo root and updates the hardware variant options
// in .github/ISSUE_TEMPLATE/bug_report.yml in place.
// Includes ACTIVE and RETIRED devices; excludes INACTIVE.
// Triggered by .github/workflows/update-issue-form.yaml on changes to devices.yaml.

import { readFileSync, writeFileSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, resolve } from 'path'

const __dirname = dirname(fileURLToPath(import.meta.url))
const repoRoot = resolve(__dirname, '..')

const yaml = readFileSync(resolve(repoRoot, 'devices.yaml'), 'utf8')

const sections = yaml.split(/(?=\n\s{2}-\s+product_hex:)/).filter(s => s.includes('product_id:'))

const products = sections
  .map(section => {
    const pid = (section.match(/product_id:\s*(\S+)/) ?? [])[1]
    const status = (section.match(/status:\s*(\S+)/) ?? [])[1]
    return { pid, status }
  })
  .filter(d => d.pid && (d.status === 'ACTIVE' || d.status === 'RETIRED'))
  .sort((a, b) => a.pid.localeCompare(b.pid))

const optionLines = [...products.map(p => `        - ${p.pid}`), '        - Other / Unknown'].join('\n') + '\n'

const formPath = resolve(repoRoot, '.github/ISSUE_TEMPLATE/bug_report.yml')
const original = readFileSync(formPath, 'utf8')

const pattern = /([ \t]+-[ \t]type: dropdown\n[ \t]+id: product[\s\S]*?options:\n)((?:[ \t]+-[ \t].+\n)+)/

if (!pattern.test(original)) {
  console.error('ERROR: Could not find product dropdown options block in bug_report.yml')
  process.exit(1)
}

const updated = original.replace(pattern, (_, preamble) => preamble + optionLines)

writeFileSync(formPath, updated)
console.log(`Updated hardware variant options with ${products.length} products in bug_report.yml`)
products.forEach(p => console.log(`  ${p.pid} (${p.status})`))
