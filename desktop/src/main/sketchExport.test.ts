import { describe, expect, it } from 'vitest'
import { mkdtemp, readFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join, sep } from 'node:path'
import { sketchTarget, writeSketch } from './sketchExport'

describe('sketchTarget', () => {
  it('puts the file in a folder named after it, which is what the IDE requires', () => {
    const t = sketchTarget(join('C:', 'work'), 'transceptor.ino')
    expect(t?.folder).toBe(join('C:', 'work', 'transceptor'))
    expect(t?.filePath).toBe(join('C:', 'work', 'transceptor', 'transceptor.ino'))
  })

  it('refuses a name that could aim the write out of the chosen folder', () => {
    // The renderer supplies this name. It is ours today; the guard is here so
    // it stays harmless if it ever stops being.
    for (const bad of ['../evil.ino', '..\\evil.ino', 'a/b.ino', 'a\\b.ino', 'C:evil.ino', '..', '.ino']) {
      expect(sketchTarget('/tmp', bad)).toBeNull()
    }
  })

  it('refuses anything that is not a .ino', () => {
    for (const bad of ['sketch.txt', 'sketch', 'sketch.ino.exe', 'sketch.INO']) {
      expect(sketchTarget('/tmp', bad)).toBeNull()
    }
  })
})

describe('writeSketch', () => {
  it('creates the folder and writes the source verbatim', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'espstation-sketch-'))
    try {
      const src = '#include <Arduino.h>\nvoid setup() {}\n'
      const res = await writeSketch(dir, 'transceptor.ino', src)
      expect(res.saved).toBe(true)
      expect(res.path).toBe(join(dir, 'transceptor', 'transceptor.ino'))
      expect(await readFile(join(dir, 'transceptor', 'transceptor.ino'), 'utf8')).toBe(src)
    } finally {
      await rm(dir, { recursive: true, force: true })
    }
  })

  it('overwrites an earlier export instead of failing on it', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'espstation-sketch-'))
    try {
      await writeSketch(dir, 'receptor.ino', 'first')
      const res = await writeSketch(dir, 'receptor.ino', 'second')
      expect(res.saved).toBe(true)
      expect(await readFile(join(dir, 'receptor', 'receptor.ino'), 'utf8')).toBe('second')
    } finally {
      await rm(dir, { recursive: true, force: true })
    }
  })

  it('reports a bad name instead of writing anywhere', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'espstation-sketch-'))
    try {
      const res = await writeSketch(dir, `..${sep}escape.ino`, 'x')
      expect(res.saved).toBe(false)
      expect(res.reason).toMatch(/not a sketch file name/)
    } finally {
      await rm(dir, { recursive: true, force: true })
    }
  })
})
