export const GARDEN_STAGES = [
  { name: '种子', from: 0 },
  { name: '发芽', from: 3 },
  { name: '长叶', from: 8 },
  { name: '开花', from: 14 },
] as const

export function gardenStageIndex(total: number) {
  return GARDEN_STAGES.reduce<number>((stage, candidate, index) => total >= candidate.from ? index : stage, 0)
}
