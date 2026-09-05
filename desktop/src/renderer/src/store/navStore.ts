import { create } from 'zustand'

export type SectionId = 'nodes' | 'live' | 'experiments' | 'networks' | 'lab' | 'flash' | 'settings'

interface NavState {
  section: SectionId
  setSection: (section: SectionId) => void
}

export const useNavStore = create<NavState>((set) => ({
  section: 'nodes',
  setSection: (section) => set({ section })
}))
