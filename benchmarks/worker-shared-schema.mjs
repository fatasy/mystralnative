import { SharedTable } from 'mystral/shared';

export const CharacterRows = SharedTable.define('mystral.benchmark.characters/v1', {
    energy: 'u32',
    settlementId: 'u32',
    birthDay: 'i32',
});
