import { app } from 'electron'

/** Minimal stand-in for @electron-toolkit/utils' `is`, avoids an extra dependency. */
export const is = {
  get dev(): boolean {
    return !app.isPackaged
  }
}
