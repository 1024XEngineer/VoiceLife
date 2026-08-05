import assert from 'node:assert/strict';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

import { checkSourceText, checkSourceTree } from './check-public-api-docs.mjs';

test('rejects an undocumented exported interface', () => {
    const errors = checkSourceText('fixture.ts', 'export interface Example {}\n');

    assert.equal(errors.length, 1);
    assert.match(errors[0], /interface Example.*缺少/);
});

test('rejects an exported declaration documented only in English', () => {
    const errors = checkSourceText('fixture.ts', '/** Example contract. */\nexport interface Example {}\n');

    assert.equal(errors.length, 1);
    assert.match(errors[0], /必须包含中文职责说明/);
});

test('checks exported function parameter and return tags', () => {
    const errors = checkSourceText(
        'fixture.ts',
        '/** 解析输入。 */\nexport function parse(input: string): number { return input.length; }\n',
    );

    assert.equal(errors.length, 2);
    assert.match(errors[0], /缺少 @param input/);
    assert.match(errors[1], /缺少 @returns/);
});

test('does not let exported functions bypass documentation with inheritDoc', () => {
    const errors = checkSourceText(
        'fixture.ts',
        '/** {@inheritDoc ExampleContract.parse} */\nexport function parse(input: string): number { return input.length; }\n',
    );

    assert.equal(errors.length, 3);
    assert.match(errors[0], /导出函数 parse.*必须包含中文职责说明/);
    assert.match(errors[1], /导出函数 parse.*缺少 @param input/);
    assert.match(errors[2], /导出函数 parse.*缺少 @returns/);
});

test('rejects undocumented methods of exported interfaces', () => {
    const errors = checkSourceText(
        'fixture.ts',
        '/** 示例服务。 */\nexport interface Example {\n  find(id: string): Promise<string>;\n}\n',
    );

    assert.equal(errors.length, 1);
    assert.match(errors[0], /接口方法 Example\.find.*缺少/);
});

test('checks interface method parameter and return tags', () => {
    const errors = checkSourceText(
        'fixture.ts',
        '/** 示例服务。 */\nexport interface Example {\n  /** 查找值。 */\n  find(id: string): Promise<string>;\n}\n',
    );

    assert.equal(errors.length, 2);
    assert.match(errors[0], /接口方法 Example\.find.*缺少 @param id/);
    assert.match(errors[1], /接口方法 Example\.find.*缺少 @returns/);
});

test('does not let interface methods bypass documentation with inheritDoc', () => {
    const errors = checkSourceText(
        'fixture.ts',
        [
            '/** 示例服务。 */',
            'export interface Example {',
            '  /** {@inheritDoc BaseExample.find} */',
            '  find(id: string): Promise<string>;',
            '}',
        ].join('\n'),
    );

    assert.equal(errors.length, 3);
    assert.match(errors[0], /接口方法 Example\.find.*必须包含中文职责说明/);
    assert.match(errors[1], /接口方法 Example\.find.*缺少 @param id/);
    assert.match(errors[2], /接口方法 Example\.find.*缺少 @returns/);
});

test('checks public class members but ignores private methods', () => {
    const errors = checkSourceText(
        'fixture.ts',
        [
            '/** 示例实现。 */',
            'export class Example {',
            '  public run(value: string): number { return value.length; }',
            '  private reset(): void {}',
            '}',
        ].join('\n'),
    );

    assert.equal(errors.length, 1);
    assert.match(errors[0], /类公开成员 Example\.run.*缺少/);
});

test('accepts class members that inherit interface documentation', () => {
    const errors = checkSourceText(
        'fixture.ts',
        [
            '/** 示例实现。 */',
            'export class Example {',
            '  /** {@inheritDoc ExampleContract.run} */',
            '  public run(value: string): number { return value.length; }',
            '}',
        ].join('\n'),
    );

    assert.deepEqual(errors, []);
});

test('accepts fully documented exported declarations', () => {
    const errors = checkSourceText(
        'fixture.ts',
        [
            '/** 示例契约。 */',
            'export interface Example {}',
            '/**',
            ' * 解析输入。',
            ' * @param input 待解析文本。',
            ' * @returns 文本长度。',
            ' */',
            'export function parse(input: string): number { return input.length; }',
        ].join('\n'),
    );

    assert.deepEqual(errors, []);
});

test('scans every TypeScript file in nested source directories', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'im-gateway-tsdoc-'));
    try {
        await mkdir(join(directory, 'nested'));
        await writeFile(join(directory, 'documented.ts'), '/** 已记录。 */\nexport type Documented = string;\n');
        await writeFile(join(directory, 'nested', 'missing.ts'), 'export class Missing {}\n');

        const result = await checkSourceTree(directory);

        assert.equal(result.fileCount, 2);
        assert.equal(result.errors.length, 1);
        assert.match(result.errors[0], /class Missing.*缺少/);
    } finally {
        await rm(directory, { recursive: true });
    }
});
