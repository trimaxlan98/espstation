/**
 * Handing a bench sketch to the Arduino IDE.
 *
 * WHY A DIRECTORY PICKER AND NOT A SAVE-FILE DIALOG. The Arduino IDE will not
 * open `foo.ino` unless it sits in a folder called `foo`. A plain save dialog
 * lets someone put `transceptor.ino` on the Desktop, and the IDE then either
 * refuses it or offers to move it — a papercut on the one step where the user
 * has left this app and cannot be helped any more. So the renderer asks for a
 * FOLDER and this module creates `<chosen>/transceptor/transceptor.ino`,
 * which is a sketch the IDE opens on a double click.
 *
 * The content comes from the renderer, which got it from the bundle. Nothing
 * here trusts the file NAME, though: it is pinned to a bare `*.ino` with no
 * separators, so no argument from the renderer can aim the write outside the
 * folder the user picked.
 */
import { mkdir, writeFile } from 'node:fs/promises'
import { join } from 'node:path'

export interface SketchSaveResult {
  saved: boolean
  /** Where the .ino landed, when it did. */
  path?: string
  /** Why it did not, when it did not — 'cancelled' is not an error. */
  reason?: string
}

/**
 * The folder and file a sketch must be written to, or null when the name is
 * not one this app produces.
 *
 * Rejects anything with a separator, a drive letter, a `..`, or an extension
 * other than `.ino`. It is a whitelist, not a blacklist: the name has to be
 * letters, digits, `_` or `-` followed by `.ino`.
 */
export function sketchTarget(dir: string, file: string): { folder: string; filePath: string } | null {
  if (!/^[A-Za-z0-9_-]+\.ino$/.test(file)) return null
  const stem = file.slice(0, -'.ino'.length)
  const folder = join(dir, stem)
  return { folder, filePath: join(folder, file) }
}

/**
 * Creates the sketch folder and writes the file.
 *
 * `recursive: true` so an existing folder is not an error: re-exporting the
 * same sketch over an earlier copy is the normal case, not a mistake. The
 * file itself IS overwritten — the caller has already shown the user where it
 * is going.
 */
export async function writeSketch(dir: string, file: string, content: string): Promise<SketchSaveResult> {
  const target = sketchTarget(dir, file)
  if (target === null) return { saved: false, reason: `not a sketch file name: ${file}` }
  await mkdir(target.folder, { recursive: true })
  await writeFile(target.filePath, content, 'utf8')
  return { saved: true, path: target.filePath }
}
