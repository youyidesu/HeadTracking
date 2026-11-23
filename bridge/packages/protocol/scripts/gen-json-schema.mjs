/**
 * 生成 json-schema/index.json —— 列出所有帧定义，供外部工具发现。
 * 用法：npm run gen:schema -w @ht/protocol
 */
import { readdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const schemaDir = join(here, '..', 'json-schema');

const files = readdirSync(schemaDir)
  .filter((f) => f.endsWith('.json') && f !== 'index.json')
  .sort();

const index = {
  $schema: 'https://json-schema.org/draft/2020-12/schema',
  title: 'headtracking protocol schemas',
  version: 1,
  files,
};

writeFileSync(join(schemaDir, 'index.json'), JSON.stringify(index, null, 2) + '\n');
console.log(`✓ json-schema/index.json（${files.length} 个 schema）`);
